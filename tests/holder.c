/* holder.c - creates+binds a KTP socket and then blocks forever (never
 * calls k_close). Used to test the garbage collector: kill -9 this process
 * and the GC must free its SM entry. Prints the socket id it got. */
#include "../ksocket.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    uint16_t lp = (argc > 1) ? (uint16_t)atoi(argv[1]) : 7777;
    int s = k_socket(AF_INET, SOCK_KTP, 0);
    if (s < 0) { printf("holder: k_socket failed\n"); return 1; }
    k_bind(s, "127.0.0.1", htons(lp), "127.0.0.1", htons(lp + 1));
    printf("holder: sockid=%d pid=%d\n", s, getpid());
    fflush(stdout);
    for (;;) pause();
    return 0;
}
