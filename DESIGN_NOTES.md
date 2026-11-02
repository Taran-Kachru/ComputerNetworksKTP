# KTP — Design Notes

Scratch design document written **before** the implementation. Records the layout,
algorithms, and every non-obvious decision with its rationale. The final prose for
the grader lives in `documentation.txt`; this file is the engineering log.

## 0. Environment / language decisions

* The spec asks for C (`.c`). The task requires **C++17 compiled with g++**. All sources
  are therefore `.cpp` and the library is built with `g++ -std=c++17 -Wall -Wextra`.
  The public API in `ksocket.h` is wrapped in `extern "C"` (guarded by `__cplusplus`) so
  the five calls + `dropMessage` keep C linkage and C-compatible signatures. Rename and
  rationale documented in `documentation.txt`.
* The assignment is Linux-only (System V/POSIX shm, `fork`, process-shared
  `pthread_mutex_t`, `nanosleep`, `kill(pid,0)`, `select`). The development machine is
  Windows + MinGW (no `fork`, no `<sys/shm.h>`). **All building/running/testing is done
  inside a Linux `gcc:13` Docker container** with the project bind-mounted. This is only
  a build host choice; the submitted code is portable Linux C++17.
* Shared memory uses **POSIX shm** (`shm_open`+`mmap`, link with `-lrt -pthread`),
  name `"/ktp_sm"`. Process-shared `pthread_mutex_t` and `sem_t` live inside the region.

## 1. The hard problem: who owns the UDP socket?

File descriptors are per-process. If `k_socket` created the UDP fd in the *user* process,
thread R/S in the *init* process could not use it. **Decision: the init process owns all
UDP sockets.** `k_socket`, `k_bind`, `k_close` do not touch fds directly — they submit a
request to init over a shared-memory control channel and block on a semaphore until init
has performed the `socket()/bind()/close()` on their behalf. `k_sendto`/`k_recvfrom` touch
only SM buffers, so they run entirely in the user process.

Control channel (global, in SM): one `ControlReq` slot + `ctrl_mutex` (serialises
requesting processes) + `sem_submit` (client posts, init's thread C waits) + `sem_done`
(init posts, client waits). Ops: `OP_SOCKET`, `OP_BIND`, `OP_CLOSE`.

## 2. Shared memory layout (`SharedMem`, all POD, `static_assert`-checked)

```
SharedMem {
  pthread_mutex_t ctrl_mutex;   // serialise control-channel users
  sem_t sem_submit, sem_done;   // control handshake
  ControlReq req;               // single in-flight control request/response
  SockInfo   sock[N];           // the N KTP socket entries
}
SockInfo {
  int   state;                  // FREE / ALLOTTED / TO_REAP
  pid_t owner_pid;              // creating process (for GC via kill(pid,0))
  int   udp_fd;                 // UDP fd (valid in init only); -1 otherwise
  uint32_t src_ip; uint16_t src_port;   // bound local addr
  uint32_t dst_ip; uint16_t dst_port;   // peer addr (from k_bind)
  int   bound;                  // 1 after k_bind

  pthread_mutex_t lock;         // protects everything below

  // ---- sender ----
  SendSlot sbuf[SEND_BUF_SIZE]; // ring, index = seq % SEND_BUF_SIZE
  uint8_t  send_base;           // oldest unacked seq
  uint8_t  next_seq;            // next seq to assign to app data
  int      swnd;                // sender window size (<=10), from adv rwnd
  int      adv_rwnd;            // last advertised receiver free space
  uint8_t  last_ack;            // last cumulative ack seq seen (dup-ack detect)
  int      have_last_ack;       // 0 until first ack
  uint64_t timer_ns;            // last (re)transmit time of in-flight window
  int      send_closed;         // app finished writing (EOF marker queued)

  // ---- receiver ----
  RecvSlot rbuf[RECV_BUF_SIZE]; // ring, index = seq % RECV_BUF_SIZE
  uint8_t  rcv_base;            // next seq app will read (k_recvfrom)
  uint8_t  expected;            // next in-order seq expected from network
  int      started;             // got first data yet
  int      nospace;             // receiver buffer was full (rwnd hit 0)
  uint8_t  nospace_expected;    // expected at the time nospace was set (resume detect)

  // ---- instrumentation ----
  uint64_t tx_total;            // total DATA transmissions incl. retransmit+probe
  uint64_t msgs_unique;         // distinct data messages generated (incl EOF marker)
}
SendSlot { int in_use; uint8_t seq; uint16_t len; int eof; int sent; char data[512]; }
RecvSlot { int in_use; uint8_t seq; uint16_t len; int eof; char data[512]; }
```

Ring indexing works because at most 10 live seqs are in flight at once and they form a
contiguous modular range, so `seq % 10` is collision-free within the window.

## 3. Packet / header format

```
KTPHeader { uint8_t type; uint8_t seq; uint8_t rwnd; uint8_t flags; uint16_t len; }  // 6 bytes
KTPPacket { KTPHeader hdr; char data[512]; }
type  = KTP_DATA | KTP_ACK
seq   = DATA: this message's seq;  ACK: last in-order seq received (cumulative)
rwnd  = ACK: receiver free space (messages)
flags = bit0 EOF (end-of-stream marker), bit1 PROBE (zero-window probe)
len   = DATA: valid payload bytes 0..512 (0 = EOF marker)
```
DATA sent as `sizeof(hdr)+len` bytes; ACK as `sizeof(hdr)`.

**EOF / partial-chunk handling.** Each file chunk is a DATA message with `len` = real
bytes (the final data chunk may be <512). End of file is a dedicated in-band message with
`len==0` and `flags=EOF`, queued by a zero-length `k_sendto`. It flows through the normal
reliable/in-order pipeline. `k_recvfrom` returns `>0` for data, **`0` when it delivers the
EOF marker** (UNIX end-of-stream semantics), `-1/ENOMESSAGE` when nothing is ready. This
avoids any out-of-band file-size exchange and cannot corrupt the file.

## 4. Sequence numbers & windows

8-bit seq, starts at 1, wraps mod 256 (0 is a legal value after wrap). All comparisons use
serial arithmetic: `sdiff(a,b) = (int8_t)(a-b)`; `a` is at/after `b` iff `sdiff>=0`.
Window ≤10 ≪ 128 so comparisons are unambiguous across wrap. A >100 KB file is 200+ chunks
so wraparound past 255 is exercised in normal runs.

* Receiver free space `rwnd = RECV_BUF_SIZE - (#in_use rbuf slots)`.
* Sender effective window `eff = min(swnd, adv_rwnd)`; may send seqs in
  `[send_base, send_base+eff)` that are buffered and unsent.

## 5. Thread R (init) — receive path

```
select(all allotted udp_fds, timeout 0.5s)
 for each ready fd:
   recvfrom() one packet; identify socket by fd
   if dropMessage(P) == 1: discard (lost), continue        // REQUIRED, right after recvfrom
   lock entry
   if DATA:
     if seq already delivered (sdiff(seq,expected)<0) -> duplicate: send ACK(last_inorder,rwnd); drop
     else if seq==expected: store, advance expected, cascade contiguous buffered, ACK
     else if within [expected, rcv_base+RECV_BUF_SIZE) and space: buffer OOO, NO ack
     else: no space/out of window: set nospace if full; send ACK(last_inorder,rwnd) so a probe is answered
     if free==0: set nospace, record nospace_expected
   if ACK:
     if sdiff(ackseq,send_base)>=0 and ackseq is new: slide window, free sbuf, swnd=min(10,rwnd), reset timer
     else (duplicate ack): only update swnd/adv_rwnd (window update) — DO NOT slide
   unlock
 on select timeout: refresh fd set (new sockets), reap TO_REAP entries
```

## 6. Thread S (init) — timeout / send / persist

`nanosleep(T/2)` loop. For each allotted socket (under its lock, copying out what's needed
before any `sendto`):
1. **Retransmit:** if there are unacked in-flight msgs and `now-timer_ns > T` → resend all
   `[send_base,next_seq)` buffered+sent msgs; `timer_ns=now`; `tx_total+=count`.
2. **Send new:** while there is an unsent buffered msg with seq in the effective window →
   `sendto`, mark sent, `tx_total++`, set `timer_ns` if it was idle.
3. **Zero-window persist probe (nospace fix):** if `eff==0` AND there is at least one unsent
   buffered msg → send exactly one probe (the msg at `send_base`) once per persist interval
   (`T`), flagged PROBE; `tx_total++`. Receiver always answers a probe/dup with current rwnd.
4. **Receiver re-advertise (nospace fix):** if this socket's `nospace` is set and free>0 →
   send a window-update ACK (last in-order seq + current rwnd). Do **not** clear `nospace`
   after a single send; clear it only when the buffer fills again or when `expected` has
   advanced past `nospace_expected` (evidence the sender resumed). Done in S (nanosleep
   cadence) so it is independent of `select()` starvation.

## 7. The `nospace` problem — failure modes and fix

**Scenario.** Receiver buffer fills → advertises `rwnd=0` → sets `nospace`. Sender's
effective window → 0 and, because every message it sent was already cumulatively ACKed,
it has **nothing in flight** and goes completely silent (its retransmit timer never fires —
there is nothing to retransmit). Later the app reads messages (`k_recvfrom`), freeing space.
The textbook receiver sends **one** duplicate ACK with the new rwnd and clears `nospace`.

**Failure modes identified:**
* **(a) Lost reopen ACK → permanent deadlock.** That single dup-ACK can be dropped by
  `dropMessage`/lost. Sender is silent, so no new data → receiver never generates another
  ACK → sender stuck at window 0 forever.
* **(b) Clearing `nospace` after one send** means the receiver never retries even if (a).
* **(c) select-timeout starvation.** The reopen check only runs on a `select()` timeout;
  continuous traffic on *other* sockets keeps `select` returning readable, so the check is
  delayed/starved.
* **(d) Race** between the app freeing space in `k_recvfrom` and R/S reading `nospace`/free
  space → stale rwnd or a missed update unless both are under the entry lock.
* **(e)** The sender must treat a window-update dup-ACK as *window-size only*, never as a
  cumulative ACK that slides the window.
* **(f)** The original `rwnd=0` ACK can also be lost; then the sender keeps sending into a
  full buffer, the receiver drops for no-space and (fixed here) still ACKs rwnd=0, and the
  sender's timeout retransmits — self-healing, but only because we always ACK such packets.

**Fix (guarantees no permanent deadlock for any p<1):**
1. **Sender zero-window persist probe** (primary, §6.3): while `eff==0` and data is pending,
   S emits one probe per persist interval. The receiver **always** answers any DATA
   (probe/duplicate/no-space) with an ACK carrying current rwnd (§5). Each probe+ACK
   survives with prob ≥ (1-p)², so over repeated independent probes the chance of永久
   deadlock → 0 for every p<1. This alone breaks the deadlock.
2. **Receiver re-advertises** the window update every S tick until the sender demonstrably
   resumed (§6.4), instead of a single clear-and-forget — fixes (b).
3. Reopen handling lives in **thread S** (nanosleep cadence), not R's select timeout —
   fixes (c).
4. All `nospace`/`rwnd`/free-space reads and writes are under the per-entry lock — fixes (d).
5. Duplicate-ACK path updates window size only — fixes (e).
6. No-space/duplicate DATA is always ACKed with current rwnd — fixes (f).

**Deterministic test** (env-gated, default off → submitted behaviour unchanged):
* `KTP_DROP_REOPEN=k` — receiver deterministically drops the first *k* window-update
  reopen ACKs (simulates losing the reopen notification).
* `KTP_SLOW_READ_MS=ms` — user2 sleeps between reads, forcing the buffer to fill (rwnd=0).
* `KTP_DISABLE_PERSIST=1` — turns the persist-probe fix off to demonstrate the hang.
Without the fix (`KTP_DISABLE_PERSIST=1`) + dropped reopen the transfer **hangs**; with the
fix (default) it **completes** with a byte-identical file. The normal `dropMessage(P)` calls
in R are untouched.

## 8. Garbage collector

Init forks a GC child (before any sockets exist). It shares the SM mapping. Every ~2 s it
`kill(owner_pid,0)` on each ALLOTTED entry; `ESRCH` → owner died without `k_close` → set
`state=TO_REAP`. GC cannot close init's fds, so init's thread C reaps `TO_REAP` entries
(close `udp_fd`, mark FREE). Normal `k_close` reaps via the `OP_CLOSE` control op.

## 9. Locking rules

* One `lock` per `SockInfo` protects that entry's buffers/windows/flags/counters.
* `ctrl_mutex`+sems protect the control channel; entry allocation happens only in init's
  single thread C (no alloc race).
* Never hold an entry lock across `sendto`/`sem_wait`: copy out the bytes to send (or the
  decision) under the lock, release, then `sendto`.
* R rebuilds the `select` fd set from ALLOTTED entries each iteration, so freed/created
  sockets are picked up without extra signalling.

## 10. Instrumentation

Per socket: `tx_total` (every DATA `sendto`, incl. retransmissions and probes) and
`msgs_unique` (distinct data messages generated = file chunks + EOF marker). Helper
`k_stats(fd,&tx,&msgs)` lets user1/the experiment script report
`avg transmissions/message = tx_total / msgs_unique`.

## 11. Constants (ksocket.h)

`T 5`, `P 0.05`, `N 20`, `MSG_SIZE 512`, `RECV_BUF_SIZE 10`, `SEND_BUF_SIZE 10`,
`SWND_MAX 10`, `SOCK_KTP`, errors `ENOSPACE/ENOTBOUND/ENOMESSAGE`. For the experiment the
drop probability R uses can be overridden at init start by env `KTP_P` (defaults to `P`);
`#define P` stays in the header as required and the final table is generated with `T=5`.

## 12. Test plan

p=0 identity; p=0.3 with retransmits; wraparound (>256 msgs); nospace hang/works test;
≥3 concurrent pairs; error paths (ENOSPACE/ENOTBOUND/ENOMESSAGE); GC on killed process;
ASan/UBSan/TSan + valgrind; repeated runs; p=0.05..0.50 table.
