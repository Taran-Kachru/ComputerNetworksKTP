/* user2.c  -  KTP receiver.  Receives a file over a KTP socket and writes it.
 *
 * usage: user2 <src_ip> <src_port> <dst_ip> <dst_port> <outfile>
 *   binds local (src_ip:src_port), peer (dst_ip:dst_port), writes to <outfile>.
 * env KTP_SLOW_READ_MS: sleep this many ms between reads (forces rwnd=0 for the
 *   nospace test); default 0.
 */
#include "ksocket.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void nap(long ms) {
    struct timespec ts; ts.tv_sec = ms / 1000; ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: %s src_ip src_port dst_ip dst_port outfile\n", argv[0]);
        return 2;
    }
    const char *src_ip = argv[1];
    uint16_t src_port = htons((uint16_t)atoi(argv[2]));
    const char *dst_ip = argv[3];
    uint16_t dst_port = htons((uint16_t)atoi(argv[4]));
    const char *outfile = argv[5];

    long slow_ms = 0;
    const char *sl = getenv("KTP_SLOW_READ_MS");
    if (sl) slow_ms = atol(sl);

    FILE *f = fopen(outfile, "wb");
    if (!f) { perror("fopen outfile"); return 1; }

    int sock = k_socket(AF_INET, SOCK_KTP, 0);
    if (sock < 0) { fprintf(stderr, "k_socket failed (err %d)\n", k_errno); return 1; }
    if (k_bind(sock, src_ip, src_port, dst_ip, dst_port) < 0) {
        fprintf(stderr, "k_bind failed (err %d)\n", k_errno); return 1;
    }

    char buf[MSG_SIZE];
    unsigned long long bytes = 0, msgs = 0;
    while (1) {
        ssize_t n = k_recvfrom(sock, buf, MSG_SIZE, 0, NULL, NULL);
        if (n < 0) { nap(5); continue; }        /* ENOMESSAGE: retry */
        if (n == 0) break;                       /* EOF marker */
        fwrite(buf, 1, (size_t)n, f);
        bytes += (unsigned long long)n; msgs++;
        if (slow_ms > 0) nap(slow_ms);
    }
    fflush(f); fclose(f);

    printf("[user2] received %llu messages, %llu bytes -> %s\n", msgs, bytes, outfile);
    k_close(sock);
    return 0;
}
