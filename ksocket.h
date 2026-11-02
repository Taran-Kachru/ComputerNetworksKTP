/* ksocket.h  -  KTP (reliable, window-based, message-oriented transport over UDP)
 *
 * Public library header.  All declarations are given C linkage via extern "C"
 * so that the API is callable exactly like the UDP calls it mirrors, even though
 * the library is implemented in C++17.  See documentation.txt for the C->C++
 * rename rationale.
 */
#ifndef KSOCKET_H
#define KSOCKET_H

#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>

/* ------------------------------------------------------------------ */
/* Tunable parameters (spec: must live in this header, not hardcoded). */
/* ------------------------------------------------------------------ */
#define SOCK_KTP      79          /* the only socket "type" k_socket accepts   */
#define T             5           /* retransmission timeout, seconds           */
#define P             0.05f       /* default message-drop probability for R    */
#define N             20          /* max number of simultaneous KTP sockets    */

#define MSG_SIZE      512         /* fixed payload size (bytes)                */
#define RECV_BUF_SIZE 10          /* receiver message buffer capacity (msgs)   */
#define SEND_BUF_SIZE 10          /* sender message buffer capacity (msgs)     */
#define SWND_MAX      10          /* initial / maximum sender window size      */

/* Physical ring-array sizes.  The logical capacities above are 10 (as the spec
 * requires), but the arrays are sized 16 = next power of two >= 10 that divides
 * 256.  Because 256 % 16 == 0, an 8-bit sequence number indexes the ring with
 * seq & 15 and never collides across the 255->0 wrap (whereas seq % 10 would:
 * 255 and 5 both map to 5).  At most 10 slots are ever in use. */
#define SEND_RING     16
#define RECV_RING     16

/* ------------------------------------------------------------------ */
/* Error codes.  The spec asks for a global error variable; we expose  */
/* k_errno (thread-aware) plus these codes.  Values are kept clear of  */
/* common <errno.h> ranges.                                            */
/* ------------------------------------------------------------------ */
#define ENOSPACE     1001         /* no free SM slot / send buffer full        */
#define ENOTBOUND    1002         /* k_sendto dest != bound dest               */
#define ENOMESSAGE   1003         /* k_recvfrom: no message available          */

/* ------------------------------------------------------------------ */
/* Packet / header format (see DESIGN_NOTES.md section 3).             */
/* ------------------------------------------------------------------ */
#define KTP_DATA      1
#define KTP_ACK       2

#define KTP_FLAG_EOF   0x01       /* end-of-stream marker                      */
#define KTP_FLAG_PROBE 0x02       /* zero-window persist probe                 */

#ifdef __cplusplus
#include <type_traits>
extern "C" {
#endif

typedef struct KTPHeader {
    uint8_t  type;                /* KTP_DATA or KTP_ACK                        */
    uint8_t  seq;                 /* DATA: this seq; ACK: last in-order seq     */
    uint8_t  rwnd;                /* ACK: advertised receiver free space        */
    uint8_t  flags;               /* KTP_FLAG_*                                 */
    uint16_t len;                 /* DATA: valid payload bytes (0..512)         */
} KTPHeader;

typedef struct KTPPacket {
    KTPHeader hdr;
    char      data[MSG_SIZE];
} KTPPacket;

/* ------------------------------------------------------------------ */
/* Public API.  Signatures mirror the UDP calls (except k_bind).      */
/* k_socket returns a KTP socket id (0..N-1), used by the other calls. */
/* ------------------------------------------------------------------ */

/* The current thread's KTP error code (set by the calls on failure). */
int *k_errno_location(void);
#define k_errno (*k_errno_location())

int  k_socket(int domain, int type, int protocol);

int  k_bind(int sockfd,
            const char *src_ip, uint16_t src_port,
            const char *dst_ip, uint16_t dst_port);

ssize_t k_sendto(int sockfd, const void *buf, size_t len, int flags,
                 const struct sockaddr *dest_addr, socklen_t addrlen);

ssize_t k_recvfrom(int sockfd, void *buf, size_t len, int flags,
                   struct sockaddr *src_addr, socklen_t *addrlen);

int  k_close(int sockfd);

/* Simulated unreliable link: returns 1 (drop) with probability p, else 0. */
int  dropMessage(float p);

/* Non-spec helper for the experiments: total DATA transmissions and the
 * number of unique messages generated for this socket. Returns 0 on success. */
int  k_stats(int sockfd, unsigned long long *tx_total,
             unsigned long long *msgs_unique);

/* Non-spec helper: number of messages written but not yet acknowledged.
 * Lets a sender drain (wait for 0) before k_close so the file tail is not lost. */
int  k_outstanding(int sockfd);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KSOCKET_H */
