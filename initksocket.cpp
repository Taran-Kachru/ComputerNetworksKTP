/* initksocket.cpp  -  KTP init process.
 *
 * Creates and owns the shared memory and ALL UDP sockets, runs:
 *   thread C : control channel (k_socket / k_bind / k_close) + reaping
 *   thread R : receive path  (select + dropMessage + data/ACK handling)
 *   thread S : timeouts, retransmission, new sends, zero-window persist probe,
 *              and the receiver window-reopen re-advertisement (nospace fix)
 *   GC child : detects dead owners (kill(pid,0)) and marks entries TO_REAP
 * Handles SIGINT/SIGTERM: stops threads, kills GC, unlinks shared memory.
 */
#include "ktp_internal.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <arpa/inet.h>
#include <thread>
#include <atomic>

static SharedMem *g_sm = nullptr;
static std::atomic<int> g_stop{0};
static std::atomic<int> g_dump{0};
static int g_debug = 0;
#define DBG(...) do { if (g_debug) { fprintf(stderr, __VA_ARGS__); fflush(stderr); } } while (0)
static pid_t g_gc_pid = -1;
static float g_drop_p = P;

/* Retransmission/persist timeout in ns.  Defaults to the header's T (5 s).  May
 * be overridden at run time with env KTP_T (seconds, may be fractional) purely
 * to speed up development/testing; the submitted header keeps #define T 5 and
 * the final experiment table is produced with the default T=5.  This does not
 * change any default behaviour. */
static uint64_t T_NS = (uint64_t)T * 1000000000ULL;

/* Thread S wakes on this fine-grained nanosleep tick so that new sends and
 * window-opening are prompt.  The retransmission timeout and the zero-window
 * persist interval remain exactly T seconds (measured against T_NS); only the
 * polling granularity is small.  A coarse T/2 sleep would throttle throughput
 * to (window / (T/2)) messages per second, which is why a fine tick is used. */
static const long S_TICK_NS = 2 * 1000 * 1000;   /* 2 ms */

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */
static void clear_sock_dynamic(SockInfo *s) {
    /* reset everything except the mutex (which stays initialised) */
    s->owner_pid = 0; s->udp_fd = -1;
    s->src_ip = s->dst_ip = 0; s->src_port = s->dst_port = 0; s->bound = 0;
    memset(s->sbuf, 0, sizeof(s->sbuf));
    memset(s->rbuf, 0, sizeof(s->rbuf));
    s->send_base = s->next_seq = 1;
    s->swnd = SWND_MAX; s->adv_rwnd = RECV_BUF_SIZE;
    s->last_ack = 0; s->have_last_ack = 0;
    s->timer_ns = 0; s->probe_ns = 0; s->send_closed = 0;
    s->rcv_base = s->expected = 1;
    s->started = 0; s->nospace = 0; s->nospace_expected = 0; s->reopen_dropped = 0;
    s->tx_total = 0; s->msgs_unique = 0;
}

static int recv_free(SockInfo *s) {
    int used = 0;
    for (int i = 0; i < RECV_RING; i++) if (s->rbuf[i].in_use) used++;
    return RECV_BUF_SIZE - used;
}

static void send_ack(int fd, uint32_t ip, uint16_t port,
                     uint8_t seq, uint8_t rwnd, uint8_t flags) {
    KTPHeader h; memset(&h, 0, sizeof(h));
    h.type = KTP_ACK; h.seq = seq; h.rwnd = rwnd; h.flags = flags; h.len = 0;
    struct sockaddr_in d; memset(&d, 0, sizeof(d));
    d.sin_family = AF_INET; d.sin_addr.s_addr = ip; d.sin_port = port;
    sendto(fd, &h, sizeof(h), 0, (struct sockaddr *)&d, sizeof(d));
}

static void send_data(int fd, uint32_t ip, uint16_t port, const KTPPacket *p) {
    struct sockaddr_in d; memset(&d, 0, sizeof(d));
    d.sin_family = AF_INET; d.sin_addr.s_addr = ip; d.sin_port = port;
    size_t n = sizeof(KTPHeader) + p->hdr.len;
    sendto(fd, p, n, 0, (struct sockaddr *)&d, sizeof(d));
}

/* ------------------------------------------------------------------ */
/* Thread C : control channel + reaping of dead entries                */
/* ------------------------------------------------------------------ */
static void reap_dead(void) {
    for (int i = 0; i < N; i++) {
        SockInfo *s = &g_sm->sock[i];
        ktp_lock(&s->lock);
        if (s->state == ST_TO_REAP) {
            if (s->udp_fd >= 0) close(s->udp_fd);
            clear_sock_dynamic(s);
            s->state = ST_FREE;
        }
        ktp_unlock(&s->lock);
    }
}

static void do_op_socket(ControlReq *r) {
    int id = -1;
    for (int i = 0; i < N; i++) {
        if (g_sm->sock[i].state == ST_FREE) { id = i; break; }
    }
    if (id < 0) { r->ret = -1; r->err = ENOSPACE; return; }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { r->ret = -1; r->err = ENOSPACE; return; }
    SockInfo *s = &g_sm->sock[id];
    ktp_lock(&s->lock);
    clear_sock_dynamic(s);
    s->owner_pid = r->pid;
    s->udp_fd = fd;
    const char *dr = getenv("KTP_DROP_REOPEN");
    s->reopen_dropped = dr ? atoi(dr) : 0;
    s->state = ST_ALLOTTED;
    ktp_unlock(&s->lock);
    r->ret = id; r->err = 0;
}

static void do_op_bind(ControlReq *r) {
    int id = r->sockid;
    if (id < 0 || id >= N) { r->ret = -1; r->err = ENOTBOUND; return; }
    SockInfo *s = &g_sm->sock[id];
    ktp_lock(&s->lock);
    if (s->state != ST_ALLOTTED || s->owner_pid != r->pid) {
        ktp_unlock(&s->lock); r->ret = -1; r->err = ENOTBOUND; return;
    }
    int fd = s->udp_fd;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = r->src_ip; a.sin_port = r->src_port;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        ktp_unlock(&s->lock); r->ret = -1; r->err = ENOTBOUND; return;
    }
    s->src_ip = r->src_ip; s->src_port = r->src_port;
    s->dst_ip = r->dst_ip; s->dst_port = r->dst_port;
    s->bound = 1;
    ktp_unlock(&s->lock);
    r->ret = 0; r->err = 0;
}

static void do_op_close(ControlReq *r) {
    int id = r->sockid;
    if (id < 0 || id >= N) { r->ret = -1; r->err = ENOTBOUND; return; }
    SockInfo *s = &g_sm->sock[id];
    ktp_lock(&s->lock);
    if (s->state != ST_FREE) {
        if (s->udp_fd >= 0) close(s->udp_fd);
        clear_sock_dynamic(s);
        s->state = ST_FREE;
    }
    ktp_unlock(&s->lock);
    r->ret = 0; r->err = 0;
}

static void thread_C(void) {
    while (!g_stop.load()) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200000000L;             /* 0.2 s */
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        if (sem_timedwait(&g_sm->sem_submit, &ts) == 0) {
            ControlReq *r = &g_sm->req;
            switch (r->op) {
                case OP_SOCKET: do_op_socket(r); break;
                case OP_BIND:   do_op_bind(r);   break;
                case OP_CLOSE:  do_op_close(r);  break;
                default: r->ret = -1; r->err = EINVAL; break;
            }
            sem_post(&g_sm->sem_done);
        }
        reap_dead();
        if (g_dump.exchange(0)) {
            int allotted = 0;
            for (int i = 0; i < N; i++) {
                SockInfo *s = &g_sm->sock[i];
                if (s->state != ST_ALLOTTED) continue;
                allotted++;
                ktp_lock(&s->lock);
                fprintf(stderr,
                  "[dump] id=%d bound=%d sbase=%u nseq=%u swnd=%d adv=%d lastack=%u/%d "
                  "exp=%u rbase=%u nospace=%d free=%d txt=%llu uniq=%llu sclosed=%d\n",
                  i, s->bound, s->send_base, s->next_seq, s->swnd, s->adv_rwnd,
                  s->last_ack, s->have_last_ack, s->expected, s->rcv_base, s->nospace,
                  recv_free(s), (unsigned long long)s->tx_total,
                  (unsigned long long)s->msgs_unique, s->send_closed);
                ktp_unlock(&s->lock);
            }
            fprintf(stderr, "[dumpsummary] allotted=%d\n", allotted);
            fflush(stderr);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Thread R : receive path                                             */
/* ------------------------------------------------------------------ */
static void handle_data(SockInfo *s, const KTPPacket *p,
                        int *do_ack, uint8_t *ack_seq, uint8_t *ack_rwnd) {
    uint8_t seq = p->hdr.seq;
    *do_ack = 0;

    int off = ktp_sdiff(seq, s->rcv_base);          /* position from read base */
    if (off < 0) {                                   /* already delivered -> dup */
        *do_ack = 1; *ack_seq = (uint8_t)(s->expected - 1); *ack_rwnd = recv_free(s);
        return;
    }
    if (off >= RECV_BUF_SIZE) {                       /* beyond buffer window */
        *do_ack = 1; *ack_seq = (uint8_t)(s->expected - 1); *ack_rwnd = recv_free(s);
        if (recv_free(s) == 0) { s->nospace = 1; s->nospace_expected = s->expected; }
        return;
    }
    RecvSlot *sl = &s->rbuf[seq % RECV_RING];
    if (sl->in_use && sl->seq == seq) {               /* duplicate in buffer */
        if (ktp_sdiff(seq, s->expected) < 0) {        /* in-order dup -> re-ack */
            *do_ack = 1; *ack_seq = (uint8_t)(s->expected - 1); *ack_rwnd = recv_free(s);
        }
        return;                                       /* OOO dup: silent */
    }
    if (recv_free(s) == 0) {                           /* no room to buffer */
        DBG("[R] NOSPACE DROP seq=%u exp=%u rbase=%u free=0\n", seq, s->expected, s->rcv_base);
        *do_ack = 1; *ack_seq = (uint8_t)(s->expected - 1); *ack_rwnd = 0;
        s->nospace = 1; s->nospace_expected = s->expected;
        return;
    }
    /* store the message */
    sl->in_use = 1; sl->seq = seq; sl->len = p->hdr.len;
    sl->eof = (p->hdr.flags & KTP_FLAG_EOF) ? 1 : 0;
    if (p->hdr.len > 0) memcpy(sl->data, p->data, p->hdr.len);
    s->started = 1;

    if (seq == s->expected) {                          /* in-order: advance+cascade */
        s->expected = (uint8_t)(s->expected + 1);
        while (true) {
            RecvSlot *nx = &s->rbuf[s->expected % RECV_RING];
            if (nx->in_use && nx->seq == s->expected)
                s->expected = (uint8_t)(s->expected + 1);
            else break;
        }
        int fr = recv_free(s);
        *do_ack = 1; *ack_seq = (uint8_t)(s->expected - 1); *ack_rwnd = (uint8_t)fr;
        DBG("[R] inorder seq=%u -> exp=%u ACK(%u,rwnd=%d)\n", seq, s->expected, *ack_seq, fr);
        if (fr == 0) { s->nospace = 1; s->nospace_expected = s->expected; }
        else         { s->nospace = 0; }
    }
    /* else out-of-order within window: buffered, NO ack */
}

static void handle_ack(SockInfo *s, const KTPPacket *p) {
    uint8_t ackseq = p->hdr.seq;
    int adv = p->hdr.rwnd;
    int old_eff = s->swnd < s->adv_rwnd ? s->swnd : s->adv_rwnd;

    if (!s->have_last_ack || ktp_sdiff(ackseq, s->last_ack) > 0) {
        /* new cumulative ack: slide window up to and including ackseq */
        uint8_t b = s->send_base;
        while (ktp_sdiff(ackseq, b) >= 0 && ktp_sdiff(s->next_seq, b) > 0) {
            SendSlot *sl = &s->sbuf[b % SEND_RING];
            sl->in_use = 0; sl->sent = 0;
            b = (uint8_t)(b + 1);
        }
        s->send_base = b;
        s->last_ack = ackseq; s->have_last_ack = 1;
        s->adv_rwnd = adv;
        s->swnd = adv < SWND_MAX ? adv : SWND_MAX;
        s->timer_ns = ktp_now_ns();               /* restart timer for remainder */
        DBG("[S<-ACK] new ack=%u adv=%d -> sbase=%u swnd=%d nseq=%u\n",
            ackseq, adv, s->send_base, s->swnd, s->next_seq);
    } else if (ackseq == s->last_ack) {
        /* duplicate ack = window update only, do NOT slide */
        s->adv_rwnd = adv;
        s->swnd = adv < SWND_MAX ? adv : SWND_MAX;
        int new_eff = s->swnd < s->adv_rwnd ? s->swnd : s->adv_rwnd;
        if (old_eff == 0 && new_eff > 0) s->timer_ns = 0;  /* force fast retransmit */
    }
    /* older ack: ignore */
}

static void thread_R(void) {
    while (!g_stop.load()) {
        fd_set rfds; FD_ZERO(&rfds);
        int maxfd = -1;
        /* Build the fd set.  state/udp_fd are read under the entry lock so there
         * is no race with thread C creating/closing sockets. */
        for (int i = 0; i < N; i++) {
            SockInfo *s = &g_sm->sock[i];
            ktp_lock(&s->lock);
            if (s->state == ST_ALLOTTED && s->udp_fd >= 0) {
                FD_SET(s->udp_fd, &rfds);
                if (s->udp_fd > maxfd) maxfd = s->udp_fd;
            }
            ktp_unlock(&s->lock);
        }
        struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 500000;   /* 0.5 s */
        int rv = (maxfd < 0) ? 0 : select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
        if (rv <= 0) continue;                      /* timeout / signal: reloop */

        for (int i = 0; i < N; i++) {
            SockInfo *s = &g_sm->sock[i];
            ktp_lock(&s->lock);
            int fd = (s->state == ST_ALLOTTED) ? s->udp_fd : -1;
            ktp_unlock(&s->lock);
            if (fd < 0 || !FD_ISSET(fd, &rfds)) continue;

            KTPPacket pkt;
            struct sockaddr_in from; socklen_t fl = sizeof(from);
            ssize_t n = recvfrom(fd, &pkt, sizeof(pkt), 0,
                                 (struct sockaddr *)&from, &fl);
            if (n < (ssize_t)sizeof(KTPHeader)) continue;

            /* REQUIRED: dropMessage right after recvfrom; if 1, the packet is lost */
            if (dropMessage(g_drop_p) == 1) continue;

            if (pkt.hdr.type == KTP_DATA) {
                int do_ack = 0; uint8_t aseq = 0, arwnd = 0;
                uint32_t dip; uint16_t dport;
                ktp_lock(&s->lock);
                handle_data(s, &pkt, &do_ack, &aseq, &arwnd);
                dip = s->dst_ip; dport = s->dst_port;
                ktp_unlock(&s->lock);
                if (do_ack) send_ack(fd, dip, dport, aseq, arwnd, 0);
            } else if (pkt.hdr.type == KTP_ACK) {
                ktp_lock(&s->lock);
                handle_ack(s, &pkt);
                ktp_unlock(&s->lock);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Thread S : timeout / retransmit / new send / persist probe / reopen */
/* ------------------------------------------------------------------ */
struct OutPkt { int is_ack; int len; KTPPacket pkt; uint8_t aseq, arwnd, aflags; };

static void thread_S(void) {
    int disable_persist = getenv("KTP_DISABLE_PERSIST") ? 1 : 0;
    while (!g_stop.load()) {
        struct timespec req, rem;
        req.tv_sec = 0; req.tv_nsec = S_TICK_NS;
        while (nanosleep(&req, &rem) == -1 && errno == EINTR) req = rem;
        if (g_stop.load()) break;

        for (int i = 0; i < N; i++) {
            SockInfo *s = &g_sm->sock[i];

            OutPkt out[2 * SEND_BUF_SIZE + 2];
            int nout = 0;
            int fd; uint32_t dip; uint16_t dport;
            uint64_t now = ktp_now_ns();

            ktp_lock(&s->lock);
            if (s->state != ST_ALLOTTED || !s->bound) { ktp_unlock(&s->lock); continue; }
            fd = s->udp_fd; dip = s->dst_ip; dport = s->dst_port;

            /* who is in flight (sent, unacked) ? */
            int had_unacked = 0;
            for (uint8_t b = s->send_base; ktp_sdiff(s->next_seq, b) > 0; b = (uint8_t)(b + 1)) {
                SendSlot *sl = &s->sbuf[b % SEND_RING];
                if (sl->in_use && sl->sent) { had_unacked = 1; break; }
            }

            /* (1) retransmit all in-flight on timeout (ignores window) */
            if (had_unacked && now - s->timer_ns > T_NS) {
                for (uint8_t b = s->send_base; ktp_sdiff(s->next_seq, b) > 0; b = (uint8_t)(b + 1)) {
                    SendSlot *sl = &s->sbuf[b % SEND_RING];
                    if (sl->in_use && sl->sent) {
                        OutPkt *o = &out[nout++];
                        o->is_ack = 0; o->len = (int)(sizeof(KTPHeader) + sl->len);
                        o->pkt.hdr.type = KTP_DATA; o->pkt.hdr.seq = sl->seq;
                        o->pkt.hdr.rwnd = 0; o->pkt.hdr.flags = sl->eof ? KTP_FLAG_EOF : 0;
                        o->pkt.hdr.len = sl->len;
                        if (sl->len > 0) memcpy(o->pkt.data, sl->data, sl->len);
                        s->tx_total++;
                    }
                }
                s->timer_ns = now;
                had_unacked = 1;
            }

            /* (2) send new messages that fit in the effective window */
            int eff = s->swnd < s->adv_rwnd ? s->swnd : s->adv_rwnd;
            if (eff < 0) eff = 0;
            for (int k = 0; k < eff; k++) {
                uint8_t seq = (uint8_t)(s->send_base + k);
                if (ktp_sdiff(s->next_seq, seq) <= 0) break;   /* no such buffered msg */
                SendSlot *sl = &s->sbuf[seq % SEND_RING];
                if (sl->in_use && !sl->sent) {
                    OutPkt *o = &out[nout++];
                    o->is_ack = 0; o->len = (int)(sizeof(KTPHeader) + sl->len);
                    o->pkt.hdr.type = KTP_DATA; o->pkt.hdr.seq = sl->seq;
                    o->pkt.hdr.rwnd = 0; o->pkt.hdr.flags = sl->eof ? KTP_FLAG_EOF : 0;
                    o->pkt.hdr.len = sl->len;
                    if (sl->len > 0) memcpy(o->pkt.data, sl->data, sl->len);
                    sl->sent = 1; s->tx_total++;
                    DBG("[S] send new seq=%u eff=%d (sbase=%u nseq=%u adv=%d)\n",
                        sl->seq, eff, s->send_base, s->next_seq, s->adv_rwnd);
                    if (!had_unacked) { s->timer_ns = now; had_unacked = 1; }
                }
            }

            /* (3) zero-window persist probe: window 0 but data pending */
            if (!disable_persist && eff == 0) {
                SendSlot *sl = &s->sbuf[s->send_base % SEND_RING];
                int have_pending = (ktp_sdiff(s->next_seq, s->send_base) > 0) && sl->in_use;
                if (have_pending && now - s->probe_ns > T_NS) {
                    OutPkt *o = &out[nout++];
                    o->is_ack = 0; o->len = (int)(sizeof(KTPHeader) + sl->len);
                    o->pkt.hdr.type = KTP_DATA; o->pkt.hdr.seq = sl->seq;
                    o->pkt.hdr.rwnd = 0;
                    o->pkt.hdr.flags = (sl->eof ? KTP_FLAG_EOF : 0) | KTP_FLAG_PROBE;
                    o->pkt.hdr.len = sl->len;
                    if (sl->len > 0) memcpy(o->pkt.data, sl->data, sl->len);
                    sl->sent = 1; s->tx_total++;
                    s->probe_ns = now;
                    if (!had_unacked) { s->timer_ns = now; had_unacked = 1; }
                }
            }

            /* (4) receiver window-reopen re-advertisement (nospace fix) */
            if (s->nospace) {
                int fr = recv_free(s);
                if (fr > 0) {
                    if (ktp_sdiff(s->expected, s->nospace_expected) > 0) {
                        s->nospace = 0;                 /* sender already resumed */
                    } else if (s->reopen_dropped != 0) {
                        /* TEST: simulate lost reopen ACK(s).  >0 drops that many;
                         * -1 drops them all (forces reliance on the persist probe). */
                        if (s->reopen_dropped > 0) s->reopen_dropped--;
                    } else {
                        OutPkt *o = &out[nout++];
                        o->is_ack = 1; o->aseq = (uint8_t)(s->expected - 1);
                        o->arwnd = (uint8_t)fr; o->aflags = 0;
                        if (fr == RECV_BUF_SIZE) s->nospace = 0;  /* drained; probe backstops */
                    }
                }
            }
            ktp_unlock(&s->lock);

            for (int j = 0; j < nout; j++) {
                if (out[j].is_ack) send_ack(fd, dip, dport, out[j].aseq, out[j].arwnd, out[j].aflags);
                else               send_data(fd, dip, dport, &out[j].pkt);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Garbage collector (separate process)                                */
/* ------------------------------------------------------------------ */
static void gc_loop(void) {
    while (true) {
        for (int i = 0; i < N; i++) {
            SockInfo *s = &g_sm->sock[i];
            ktp_lock(&s->lock);
            if (s->state == ST_ALLOTTED && s->owner_pid > 0) {
                if (kill(s->owner_pid, 0) < 0 && errno == ESRCH) {
                    s->state = ST_TO_REAP;         /* init's thread C will free it */
                }
            }
            ktp_unlock(&s->lock);
        }
        struct timespec ts; ts.tv_sec = 2; ts.tv_nsec = 0;
        nanosleep(&ts, nullptr);
    }
}

/* ------------------------------------------------------------------ */
/* SM lifecycle + signals                                              */
/* ------------------------------------------------------------------ */
static void cleanup(void) {
    if (g_gc_pid > 0) { kill(g_gc_pid, SIGKILL); waitpid(g_gc_pid, nullptr, 0); }
    if (g_sm) {
        for (int i = 0; i < N; i++) pthread_mutex_destroy(&g_sm->sock[i].lock);
        pthread_mutex_destroy(&g_sm->ctrl_mutex);
        sem_destroy(&g_sm->sem_submit);
        sem_destroy(&g_sm->sem_done);
        munmap(g_sm, sizeof(SharedMem));
    }
    shm_unlink(KTP_SHM_NAME);
}

static void on_signal(int) { g_stop.store(1); }
static void on_usr1(int) { g_dump.store(1); }

static void init_shared_mem(void) {
    shm_unlink(KTP_SHM_NAME);                     /* clear any stale region */
    int fd = shm_open(KTP_SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd < 0) { perror("shm_open"); exit(1); }
    if (ftruncate(fd, sizeof(SharedMem)) < 0) { perror("ftruncate"); exit(1); }
    void *p = mmap(nullptr, sizeof(SharedMem), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    g_sm = reinterpret_cast<SharedMem *>(p);
    memset(g_sm, 0, sizeof(SharedMem));

    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&ma, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(&g_sm->ctrl_mutex, &ma);
    for (int i = 0; i < N; i++) {
        pthread_mutex_init(&g_sm->sock[i].lock, &ma);
        g_sm->sock[i].state = ST_FREE;
        g_sm->sock[i].udp_fd = -1;
    }
    pthread_mutexattr_destroy(&ma);

    sem_init(&g_sm->sem_submit, 1, 0);
    sem_init(&g_sm->sem_done, 1, 0);
}

int main(void) {
    const char *pe = getenv("KTP_P");
    if (pe) g_drop_p = (float)atof(pe);
    g_debug = getenv("KTP_DEBUG") ? 1 : 0;
    const char *te = getenv("KTP_T");
    if (te) { double t = atof(te); if (t > 0) T_NS = (uint64_t)(t * 1e9); }

    init_shared_mem();

    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    struct sigaction su; memset(&su, 0, sizeof(su));
    su.sa_handler = on_usr1;
    sigaction(SIGUSR1, &su, nullptr);

    g_gc_pid = fork();
    if (g_gc_pid == 0) { gc_loop(); _exit(0); }    /* child: GC */

    printf("[init] KTP init running: N=%d T=%d P=%.3f (drop p=%.3f). Ctrl-C to stop.\n",
           N, T, (double)P, (double)g_drop_p);
    fflush(stdout);

    std::thread tc(thread_C), tr(thread_R), ts(thread_S);
    tc.join(); tr.join(); ts.join();

    cleanup();
    printf("[init] stopped, shared memory removed.\n");
    return 0;
}
