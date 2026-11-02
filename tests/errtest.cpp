/* errtest.cpp - exercise the KTP error paths. Requires init to be running. */
#include "../ksocket.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
static void check(const char *name, int cond) {
    printf("  %-45s : %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond) fails++;
}

int main() {
    /* ---- ENOMESSAGE: recv on empty buffer ---- */
    int s = k_socket(AF_INET, SOCK_KTP, 0);
    check("k_socket ok", s >= 0);
    k_bind(s, "127.0.0.1", htons(7001), "127.0.0.1", htons(7002));
    char buf[512];
    ssize_t r = k_recvfrom(s, buf, sizeof(buf), 0, nullptr, nullptr);
    check("k_recvfrom empty -> -1/ENOMESSAGE", r < 0 && k_errno == ENOMESSAGE);

    /* ---- ENOTBOUND: sendto to a destination != bound peer ---- */
    struct sockaddr_in bad; memset(&bad, 0, sizeof(bad));
    bad.sin_family = AF_INET; bad.sin_port = htons(9999);
    inet_aton("127.0.0.1", &bad.sin_addr);
    r = k_sendto(s, "hello", 5, 0, (struct sockaddr *)&bad, sizeof(bad));
    check("k_sendto wrong dest -> -1/ENOTBOUND", r < 0 && k_errno == ENOTBOUND);

    /* ---- ENOSPACE: fill the send buffer (no receiver -> nothing acked) ---- */
    struct sockaddr_in good; memset(&good, 0, sizeof(good));
    good.sin_family = AF_INET; good.sin_port = htons(7002);
    inet_aton("127.0.0.1", &good.sin_addr);
    int sent = 0, gotNoSpace = 0;
    for (int i = 0; i < 100; i++) {
        r = k_sendto(s, buf, 512, 0, (struct sockaddr *)&good, sizeof(good));
        if (r < 0 && k_errno == ENOSPACE) { gotNoSpace = 1; break; }
        if (r >= 0) sent++;
    }
    check("k_sendto full send buffer -> ENOSPACE", gotNoSpace && sent == SEND_BUF_SIZE);
    k_close(s);

    /* ---- ENOSPACE: exhaust the SM socket table (N entries) ---- */
    int ids[N + 4]; int n = 0, enospace = 0;
    for (int i = 0; i < N + 4; i++) {
        int x = k_socket(AF_INET, SOCK_KTP, 0);
        if (x < 0) { enospace = (k_errno == ENOSPACE); break; }
        ids[n++] = x;
    }
    check("k_socket exhausts SM -> ENOSPACE", enospace && n == N);
    for (int i = 0; i < n; i++) k_close(ids[i]);

    printf("ERRTEST: %s\n", fails == 0 ? "ALL PASS" : "SOME FAILED");
    return fails ? 1 : 0;
}
