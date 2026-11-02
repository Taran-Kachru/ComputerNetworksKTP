/* user1.c  -  KTP sender.  Reads a file and transfers it over a KTP socket.
 *
 * usage: user1 <src_ip> <src_port> <dst_ip> <dst_port> <infile> [stats_out]
 *   binds local (src_ip:src_port), peer (dst_ip:dst_port), sends <infile>.
 *   If stats_out is given, writes "tx_total msgs_unique avg" to it.
 */
#include "ksocket.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

static void nap(long ms) {
    struct timespec ts; ts.tv_sec = ms / 1000; ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: %s src_ip src_port dst_ip dst_port infile [stats_out]\n", argv[0]);
        return 2;
    }
    const char *src_ip = argv[1];
    uint16_t src_port = htons((uint16_t)atoi(argv[2]));
    const char *dst_ip = argv[3];
    uint16_t dst_port = htons((uint16_t)atoi(argv[4]));
    const char *infile = argv[5];
    const char *stats_out = (argc >= 7) ? argv[6] : NULL;

    FILE *f = fopen(infile, "rb");
    if (!f) { perror("fopen infile"); return 1; }

    int sock = k_socket(AF_INET, SOCK_KTP, 0);
    if (sock < 0) { fprintf(stderr, "k_socket failed (err %d)\n", k_errno); return 1; }
    if (k_bind(sock, src_ip, src_port, dst_ip, dst_port) < 0) {
        fprintf(stderr, "k_bind failed (err %d)\n", k_errno); return 1;
    }

    struct sockaddr_in dst; memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET; dst.sin_port = dst_port;
    inet_aton(dst_ip, &dst.sin_addr);

    char buf[MSG_SIZE];
    size_t r;
    unsigned long long chunks = 0;
    while ((r = fread(buf, 1, MSG_SIZE, f)) > 0) {
        while (k_sendto(sock, buf, r, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
            if (k_errno == ENOSPACE) { nap(5); continue; }   /* buffer full: retry */
            fprintf(stderr, "k_sendto failed (err %d)\n", k_errno); fclose(f); return 1;
        }
        chunks++;
    }
    fclose(f);

    /* in-band end-of-stream marker (zero-length message) */
    while (k_sendto(sock, NULL, 0, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        if (k_errno == ENOSPACE) { nap(5); continue; }
        fprintf(stderr, "k_sendto(EOF) failed (err %d)\n", k_errno); return 1;
    }

    /* drain: wait until every message (incl. EOF marker) is acknowledged */
    while (k_outstanding(sock) > 0) nap(20);

    unsigned long long tx = 0, uniq = 0;
    k_stats(sock, &tx, &uniq);
    double avg = uniq ? (double)tx / (double)uniq : 0.0;
    printf("[user1] sent %llu data chunks + EOF; unique=%llu tx_total=%llu avg=%.4f\n",
           chunks, uniq, tx, avg);
    if (stats_out) {
        FILE *so = fopen(stats_out, "w");
        if (so) { fprintf(so, "%llu %llu %.6f\n", tx, uniq, avg); fclose(so); }
    }

    k_close(sock);
    return 0;
}
