# KTP — Reliable Transport over UDP

An implementation of **KTP (K Transport Protocol)**: a reliable, in-order,
message-oriented transport layer built on top of unreliable UDP sockets, with
window-based flow control. Written in **C++17** for Linux.

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
| `ksocket.cpp` | Library implementation of the `k_*` calls and `dropMessage` |
| `ktp_internal.h` | Shared-memory layout and helpers (internal) |
| `initksocket.cpp` | Init process: threads R and S, garbage collector, shared memory |
| `user1.cpp` / `user2.cpp` | Test sender / receiver (transfer a file end-to-end) |
| `Makefile*` | Build the static library, the init process, and the user programs |
| `documentation.txt` | Full design writeup, data structures, protocol, and results |
| `tests/` | Test and experiment scripts |

## Build

Requires a Linux toolchain (`g++` with C++17, `make`, `ar`). From the project root:

```sh
make            # builds libksocket.a, initksocket, user1, user2
```

## Run

```sh
# 1. start the init process (creates the shared memory; Ctrl-C to stop)
./initksocket &

# 2. start the receiver, then the sender (local ip/port, remote ip/port, file)
./user2 127.0.0.1 5002 127.0.0.1 5001 received.dat &
./user1 127.0.0.1 5001 127.0.0.1 5002 input.dat

# 3. verify
cmp input.dat received.dat
```

Multiple sender/receiver pairs can run concurrently on different ports.

## Testing

The `tests/` directory contains scripts for the single-pair transfer, the loss
sweep (`p = 0.05 … 0.50`), the wraparound case, the zero-window deadlock
demonstration, multiple concurrent pairs, the error paths, and the garbage
collector. The average-transmissions-per-message results for varying `p` are
tabulated at the top of `documentation.txt`.

## Notes

- `T` (retransmission timeout) and `P` (default drop probability) are defined in
  `ksocket.h`. Runtime environment overrides exist purely to speed up testing and
  do not change the defaults.
- Everything placed in shared memory is plain-old-data; process-shared robust
  mutexes guard all concurrent access.
