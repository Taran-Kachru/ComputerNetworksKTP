# KTP — Reliable Transport over UDP

An implementation of **KTP (K Transport Protocol)**: a reliable, in-order,
message-oriented transport layer built on top of unreliable UDP sockets, with
window-based flow control. Written in **C (C11)** for Linux.

KTP mirrors the familiar UDP socket API (`socket`, `bind`, `sendto`, `recvfrom`,
`close`) with `k_`-prefixed calls, but guarantees that every 512-byte message is
delivered to the receiver exactly once and in order, even over a link that drops
packets.

## Features

- Sliding-window flow control (sender window `swnd`, receiver window `rwnd`),
  cumulative acknowledgements, and per-window timeout/retransmission.
- 8-bit sequence numbers with correct modular wraparound.
- Out-of-order buffering and duplicate detection at the receiver.
- A shared-memory design with an `init` process that runs the receiver thread
  (R), the sender/timeout thread (S), and a garbage-collector process that
  reclaims sockets whose owning process died without closing them.
- A simulated lossy link via `dropMessage(p)` so the protocol can be exercised
  under controlled drop probabilities.
- A solution to the zero-window ("nospace") deadlock via a sender-side persist
  probe (see `documentation.txt`).

## Layout

| File | Description |
|------|-------------|
| `ksocket.h` | Public API, constants, error codes, and packet format |
| `ksocket.c` | Library implementation of the `k_*` calls and `dropMessage` |
| `ktp_internal.h` | Shared-memory layout and helpers (internal) |
| `initksocket.c` | Init process: threads R and S, garbage collector, shared memory |
| `user1.c` / `user2.c` | Test sender / receiver (transfer a file end-to-end) |
| `Makefile*` | Build the static library, the init process, and the user programs |
| `documentation.txt` | Full design writeup, data structures, protocol, and results |
| `tests/` | Test and experiment scripts |

## Build

Requires a Linux toolchain (`gcc` with C11, `ar`; `make` optional). From the
project root:

```sh
make              # builds libksocket.a, initksocket, user1, user2
```

**Without make** — a plain `gcc`/`ar` build script is provided:

```sh
bash build.sh     # same outputs, no make needed
```

or run the four commands by hand:

```sh
gcc -std=c11 -Wall -Wextra -O2 -D_GNU_SOURCE -c ksocket.c -o ksocket.o
ar rcs libksocket.a ksocket.o
gcc -std=c11 -Wall -Wextra -O2 -D_GNU_SOURCE initksocket.c -o initksocket -L. -lksocket -pthread -lrt
gcc -std=c11 -Wall -Wextra -O2 -D_GNU_SOURCE user1.c -o user1 -L. -lksocket -pthread -lrt
gcc -std=c11 -Wall -Wextra -O2 -D_GNU_SOURCE user2.c -o user2 -L. -lksocket -pthread -lrt
```

(If linking reports `cannot find -lrt`, drop `-lrt` — modern glibc has those
symbols in libc.)

## Run

```sh
# 1. generate test data (creates data/input_bin.dat ~200 KB)
bash tests/gen_files.sh

# 2. start the init process (creates the shared memory; Ctrl-C to stop)
./initksocket &

# 3. start the receiver, then the sender (local ip/port, remote ip/port, file)
./user2 127.0.0.1 5002 127.0.0.1 5001 received.dat &
./user1 127.0.0.1 5001 127.0.0.1 5002 data/input_bin.dat

# 4. verify
cmp data/input_bin.dat received.dat && echo "identical"
kill %1   # stop initksocket
```

Multiple sender/receiver pairs can run concurrently on different ports.

## Testing

Run the whole test suite (it builds via `build.sh` first, then runs every case)
from the project root:

```sh
bash tests/run_all.sh
```

Expected tail: `SUITE RESULT: 7 passed, 0 failed`. Individual cases can be run
directly, e.g.:

```sh
bash tests/run_pair.sh 0   data/input_bin.dat data/out.dat   # single pair, no loss
bash tests/run_pair.sh 0.3 data/input_bin.dat data/out.dat   # with 30% drops
bash tests/dbg_med.sh 300        # sequence-number wraparound (>256 messages)
bash tests/nospace_test.sh       # zero-window deadlock: hangs without fix, works with
bash tests/concurrent.sh 0.1     # 3 concurrent pairs
bash tests/run_errtest.sh        # ENOSPACE / ENOTBOUND / ENOMESSAGE
bash tests/gc_test.sh            # garbage collector reclaims a killed process
bash tests/experiment.sh         # p = 0.05..0.50 sweep -> tests/results.csv (T=5, slow)
```

The scripts cd to the project root automatically, so they work wherever the repo
is unzipped. The average-transmissions-per-message results for varying `p` are
tabulated at the top of `documentation.txt`.

## Visualization (for a demo / presentation)

A single script runs every test case live and renders the results as ASCII
tables and bar charts directly in the terminal — no GUI or plotting tools
needed:

```sh
bash tests/present.sh
```

It shows five views: (1) a correctness matrix (byte-identical under loss),
(2) retransmission overhead vs drop probability against the selective-repeat
lower bound `1/(1-p)`, (3) goodput vs loss, (4) the zero-window deadlock before
and after the persist-probe fix, and (5) the garbage collector reclaiming a
crashed process's socket. It pauses between sections when run interactively.

```sh
PRES_NOPAUSE=1 bash tests/present.sh   # run straight through (no Enter prompts)
PRES_MSGS=120 PRES_REPEATS=5 bash tests/present.sh   # larger/smoother sample
NO_COLOR=1 bash tests/present.sh       # plain output (e.g. for logging)
```

## Notes

- `T` (retransmission timeout) and `P` (default drop probability) are defined in
  `ksocket.h`. Runtime environment overrides exist purely to speed up testing and
  do not change the defaults.
- Everything placed in shared memory is plain-old-data; process-shared robust
  mutexes guard all concurrent access.
