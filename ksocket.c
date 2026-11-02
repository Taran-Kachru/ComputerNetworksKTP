/* ksocket.c  -  KTP library (user-process side of the API).
 *
 * Implements k_socket, k_bind, k_sendto, k_recvfrom, k_close, dropMessage and
 * the k_stats helper.  socket/bind/close are delegated to the init process over
 * the shared-memory control channel (init owns all UDP fds); sendto/recvfrom
 * operate directly on the shared-memory buffers.  No main() here (library).
 */
#include "ktp_internal.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <errno.h>

/* ------------------------------------------------------------------ */
/* Per-thread error variable (the spec's "global error variable").     */
/* ------------------------------------------------------------------ */
int *k_errno_location(void) {
    static __thread int e = 0;
    return &e;
}

/* ------------------------------------------------------------------ */
/* Shared-memory attachment (lazy, process-lifetime).                  */
/* ------------------------------------------------------------------ */
static SharedMem *g_sm = NULL;

static SharedMem *sm_attach(void) {
    if (g_sm) return g_sm;
    int fd = shm_open(KTP_SHM_NAME, O_RDWR, 0666);
    if (fd < 0) return NULL;                 /* init process not running */
    void *p = mmap(NULL, sizeof(SharedMem), PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return NULL;
    g_sm = (SharedMem *)p;
    return g_sm;
}

/* ------------------------------------------------------------------ */
/* dropMessage: 1 with probability p, else 0.                          */
/* ------------------------------------------------------------------ */
int dropMessage(float p) {
    static __thread int seeded = 0;
    static __thread unsigned int seed = 0;
    if (!seeded) {
        seed = (unsigned int)(ktp_now_ns() ^ (uint64_t)getpid());
        seeded = 1;
    }
    float r = (float)rand_r(&seed) / ((float)RAND_MAX + 1.0f);
    return (r < p) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Control-channel round trip (socket/bind/close).                     */
/* ------------------------------------------------------------------ */
static int control_call(SharedMem *sm, ControlReq *out) {
    if (ktp_lock(&sm->ctrl_mutex) != 0) return -1;
    sm->req = *out;
    sem_post(&sm->sem_submit);
    /* init is alive and will post sem_done; wait, retrying across EINTR. */
    while (sem_wait(&sm->sem_done) != 0 && errno == EINTR) { }
    *out = sm->req;
    ktp_unlock(&sm->ctrl_mutex);
    return 0;
}

/* ------------------------------------------------------------------ */
/* k_socket                                                            */
/* ------------------------------------------------------------------ */
int k_socket(int domain, int type, int protocol) {
    (void)domain; (void)protocol;
    if (type != SOCK_KTP) { k_errno = EINVAL; return -1; }
    SharedMem *sm = sm_attach();
    if (!sm) { k_errno = ENOSPACE; return -1; }

    ControlReq r;
    memset(&r, 0, sizeof(r));
    r.op  = OP_SOCKET;
    r.pid = getpid();
    if (control_call(sm, &r) != 0) { k_errno = ENOSPACE; return -1; }
    if (r.ret < 0) { k_errno = r.err; return -1; }
    return r.ret;                            /* KTP socket id */
}

/* ------------------------------------------------------------------ */
/* k_bind                                                              */
/* ------------------------------------------------------------------ */
int k_bind(int sockfd,
           const char *src_ip, uint16_t src_port,
           const char *dst_ip, uint16_t dst_port) {
    SharedMem *sm = sm_attach();
    if (!sm) { k_errno = ENOTBOUND; return -1; }
    if (sockfd < 0 || sockfd >= N) { k_errno = ENOTBOUND; return -1; }

    struct in_addr a;
    ControlReq r;
    memset(&r, 0, sizeof(r));
    r.op     = OP_BIND;
    r.pid    = getpid();
    r.sockid = sockfd;
    if (inet_aton(src_ip, &a) == 0) { k_errno = ENOTBOUND; return -1; }
    r.src_ip = a.s_addr;
    r.src_port = src_port;
    if (inet_aton(dst_ip, &a) == 0) { k_errno = ENOTBOUND; return -1; }
    r.dst_ip = a.s_addr;
    r.dst_port = dst_port;

    if (control_call(sm, &r) != 0) { k_errno = ENOTBOUND; return -1; }
    if (r.ret < 0) { k_errno = r.err; return -1; }
    return 0;
}

/* ------------------------------------------------------------------ */
/* k_sendto - enqueue into sender buffer (thread S does the sending).  */
/* A len==0 call enqueues the in-band end-of-stream (EOF) marker.      */
/* ------------------------------------------------------------------ */
ssize_t k_sendto(int sockfd, const void *buf, size_t len, int flags,
                 const struct sockaddr *dest_addr, socklen_t addrlen) {
    (void)flags; (void)addrlen;
    SharedMem *sm = sm_attach();
    if (!sm) { k_errno = ENOTBOUND; return -1; }
    if (sockfd < 0 || sockfd >= N) { k_errno = ENOTBOUND; return -1; }
    if (len > MSG_SIZE) len = MSG_SIZE;

    SockInfo *s = &sm->sock[sockfd];
    ktp_lock(&s->lock);
    if (s->state != ST_ALLOTTED || !s->bound) {
        ktp_unlock(&s->lock); k_errno = ENOTBOUND; return -1;
    }
    /* destination must match the bound peer (if supplied) */
    if (dest_addr) {
        const struct sockaddr_in *d = (const struct sockaddr_in *)dest_addr;
        if (d->sin_addr.s_addr != s->dst_ip || d->sin_port != s->dst_port) {
            ktp_unlock(&s->lock); k_errno = ENOTBOUND; return -1;
        }
    }
    /* send buffer full? */
    int inflight = ktp_sdiff(s->next_seq, s->send_base);   /* 0..SEND_BUF_SIZE */
    if (inflight >= SEND_BUF_SIZE) {
        ktp_unlock(&s->lock); k_errno = ENOSPACE; return -1;
    }
    uint8_t seq = s->next_seq;
    SendSlot *slot = &s->sbuf[seq % SEND_RING];
    slot->in_use = 1;
    slot->seq    = seq;
    slot->len    = (uint16_t)len;
    slot->eof    = (len == 0) ? 1 : 0;
    slot->sent   = 0;
    if (len > 0 && buf) memcpy(slot->data, buf, len);
    s->next_seq  = (uint8_t)(seq + 1);
    s->msgs_unique++;
    if (slot->eof) s->send_closed = 1;
    ktp_unlock(&s->lock);
    return (ssize_t)len;
}

/* ------------------------------------------------------------------ */
/* k_recvfrom - return the next in-order message, free its slot.       */
/* Returns payload bytes (>0), 0 for the EOF marker, -1/ENOMESSAGE.    */
/* ------------------------------------------------------------------ */
ssize_t k_recvfrom(int sockfd, void *buf, size_t len, int flags,
                   struct sockaddr *src_addr, socklen_t *addrlen) {
    (void)flags;
    SharedMem *sm = sm_attach();
    if (!sm) { k_errno = ENOMESSAGE; return -1; }
    if (sockfd < 0 || sockfd >= N) { k_errno = ENOMESSAGE; return -1; }

    SockInfo *s = &sm->sock[sockfd];
    ktp_lock(&s->lock);
    if (s->state != ST_ALLOTTED) {
        ktp_unlock(&s->lock); k_errno = ENOMESSAGE; return -1;
    }
    RecvSlot *slot = &s->rbuf[s->rcv_base % RECV_RING];
    if (!slot->in_use || slot->seq != s->rcv_base) {
        ktp_unlock(&s->lock); k_errno = ENOMESSAGE; return -1;
    }
    ssize_t n = slot->len;
    if (n > 0) {
        size_t cp = ((size_t)n < len) ? (size_t)n : len;
        if (buf) memcpy(buf, slot->data, cp);
    }
    if (src_addr && addrlen && *addrlen >= (socklen_t)sizeof(struct sockaddr_in)) {
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_addr.s_addr = s->dst_ip;
        sin.sin_port = s->dst_port;
        memcpy(src_addr, &sin, sizeof(sin));
        *addrlen = sizeof(sin);
    }
    slot->in_use = 0;
    s->rcv_base = (uint8_t)(s->rcv_base + 1);   /* frees buffer space -> rwnd grows */
    ktp_unlock(&s->lock);
    return n;                                   /* 0 => EOF marker delivered */
}

/* ------------------------------------------------------------------ */
/* k_close                                                             */
/* ------------------------------------------------------------------ */
int k_close(int sockfd) {
    SharedMem *sm = sm_attach();
    if (!sm) { k_errno = ENOTBOUND; return -1; }
    if (sockfd < 0 || sockfd >= N) { k_errno = ENOTBOUND; return -1; }
    ControlReq r;
    memset(&r, 0, sizeof(r));
    r.op     = OP_CLOSE;
    r.pid    = getpid();
    r.sockid = sockfd;
    if (control_call(sm, &r) != 0) { k_errno = ENOTBOUND; return -1; }
    return r.ret;
}

/* ------------------------------------------------------------------ */
/* k_stats - transmission counters for the experiment.                 */
/* ------------------------------------------------------------------ */
int k_stats(int sockfd, unsigned long long *tx_total,
            unsigned long long *msgs_unique) {
    SharedMem *sm = sm_attach();
    if (!sm || sockfd < 0 || sockfd >= N) return -1;
    SockInfo *s = &sm->sock[sockfd];
    ktp_lock(&s->lock);
    if (tx_total)    *tx_total    = s->tx_total;
    if (msgs_unique) *msgs_unique = s->msgs_unique;
    ktp_unlock(&s->lock);
    return 0;
}

int k_outstanding(int sockfd) {
    SharedMem *sm = sm_attach();
    if (!sm || sockfd < 0 || sockfd >= N) return -1;
    SockInfo *s = &sm->sock[sockfd];
    ktp_lock(&s->lock);
    int pending = ktp_sdiff(s->next_seq, s->send_base);
    ktp_unlock(&s->lock);
    return pending;
}
