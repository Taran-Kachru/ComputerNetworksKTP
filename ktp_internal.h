/* ktp_internal.h  -  Shared-memory layout and helpers shared between the
 * library (ksocket.cpp) and the init process (initksocket.cpp).  Not part of
 * the public API; kept out of ksocket.h so user programs see only the API.
 *
 * Everything that lives in shared memory is plain-old-data: fixed arrays,
 * integers, pthread_mutex_t, sem_t.  No std::string/vector/map, no virtuals,
 * no pointers.  Enforced with static_assert at the bottom.
 */
#ifndef KTP_INTERNAL_H
#define KTP_INTERNAL_H

#include "ksocket.h"
#include <time.h>
#include <stdint.h>

#define KTP_SHM_NAME "/ktp_sm"

/* SockInfo.state */
#define ST_FREE     0
#define ST_ALLOTTED 1
#define ST_TO_REAP  2   /* owner died; init must close fd and free */

/* Control-channel ops */
#define OP_SOCKET 1
#define OP_BIND   2
#define OP_CLOSE  3

#ifdef __cplusplus
extern "C" {
#endif

/* -------- sender / receiver slots -------- */
typedef struct SendSlot {
    int      in_use;
    uint8_t  seq;
    uint16_t len;        /* payload bytes (0 for EOF marker) */
    int      eof;
    int      sent;       /* has been transmitted at least once */
    char     data[MSG_SIZE];
} SendSlot;

typedef struct RecvSlot {
    int      in_use;
    uint8_t  seq;
    uint16_t len;
    int      eof;
    char     data[MSG_SIZE];
} RecvSlot;

/* -------- one KTP socket entry -------- */
typedef struct SockInfo {
    int      state;              /* ST_FREE / ST_ALLOTTED / ST_TO_REAP */
    pid_t    owner_pid;
    int      udp_fd;             /* valid in init process only; -1 elsewhere */

    uint32_t src_ip; uint16_t src_port;
    uint32_t dst_ip; uint16_t dst_port;
    int      bound;

    pthread_mutex_t lock;        /* protects all fields below */

    /* sender */
    SendSlot sbuf[SEND_RING];
    uint8_t  send_base;
    uint8_t  next_seq;
    int      swnd;
    int      adv_rwnd;
    uint8_t  last_ack;
    int      have_last_ack;
    uint64_t timer_ns;           /* last (re)transmit time of the window */
    uint64_t probe_ns;           /* last zero-window probe time */
    int      send_closed;        /* EOF marker has been queued by app */

    /* receiver */
    RecvSlot rbuf[RECV_RING];
    uint8_t  rcv_base;           /* next seq app will read */
    uint8_t  expected;           /* next in-order seq expected from network */
    int      started;
    int      nospace;            /* buffer was full; window-reopen pending */
    uint8_t  nospace_expected;   /* 'expected' when nospace was set */
    int      reopen_dropped;     /* test: how many reopen ACKs still to drop */

    /* instrumentation */
    uint64_t tx_total;
    uint64_t msgs_unique;
} SockInfo;

/* -------- control request slot -------- */
typedef struct ControlReq {
    int      op;
    pid_t    pid;
    int      sockid;             /* bind/close: target entry */
    uint32_t src_ip; uint16_t src_port;
    uint32_t dst_ip; uint16_t dst_port;
    int      ret;                /* response: sockid or 0/-1 */
    int      err;                /* response: error code (0 if none) */
} ControlReq;

/* -------- the whole shared region -------- */
typedef struct SharedMem {
    pthread_mutex_t ctrl_mutex;  /* serialise control-channel users */
    sem_t    sem_submit;         /* client posts a request */
    sem_t    sem_done;           /* init signals completion */
    ControlReq req;
    SockInfo sock[N];
} SharedMem;

#ifdef __cplusplus
} /* extern "C" */
#endif

/* -------- small inline helpers (no shared state) -------- */

/* serial-number arithmetic over 8 bits: how far a is ahead of b (-128..127) */
static inline int ktp_sdiff(uint8_t a, uint8_t b) {
    return (int)(int8_t)(uint8_t)(a - b);
}

static inline uint64_t ktp_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Lock a process-shared robust mutex, recovering if the previous owner died
 * while holding it (EOWNERDEAD).  Returns 0 on success. */
static inline int ktp_lock(pthread_mutex_t *m) {
    int r = pthread_mutex_lock(m);
    if (r == 130 /* EOWNERDEAD */) {
        pthread_mutex_consistent(m);
        return 0;
    }
    return r;
}
static inline void ktp_unlock(pthread_mutex_t *m) {
    pthread_mutex_unlock(m);
}

#ifdef __cplusplus
/* Enforce that everything placed in shared memory is trivially copyable POD. */
static_assert(std::is_trivially_copyable<SendSlot>::value,  "SendSlot must be POD");
static_assert(std::is_trivially_copyable<RecvSlot>::value,  "RecvSlot must be POD");
static_assert(std::is_trivially_copyable<SockInfo>::value,  "SockInfo must be POD");
static_assert(std::is_trivially_copyable<ControlReq>::value,"ControlReq must be POD");
static_assert(std::is_standard_layout<SharedMem>::value,    "SharedMem must be standard-layout");
#endif

#endif /* KTP_INTERNAL_H */
