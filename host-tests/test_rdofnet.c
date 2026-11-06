// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* rdofradio's HBlink4 peer (sdr/cli/rdofradio_net.h) against a fake master on 127.0.0.1: the DMRC at once and as
 * the keepalive, registered on DMRP, the master's other packets passed on, a refusal, and gone quiet. Then a
 * BrandMeister login over the socket: RPTL, RPTK with the salted hash, RPTC, RPTPING, no DMRD before it's in, RPTCL
 * on close. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "rdof/sha256.h"
#include "rdofradio_net.h"

static int fails, checks;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                        \
        }                                                                                                              \
    } while (0)

static char said[8][200];
static int nsaid;
static void say(const char *line)
{
    if (nsaid < 8)
        snprintf(said[nsaid++], sizeof said[0], "%s", line);
}
static uint8_t got[64];
static size_t ngot;
static void on_packet(void *ctx, const uint8_t *d, size_t n)
{
    ngot = n < sizeof got ? n : sizeof got;
    memcpy(got, d, ngot);
}

/* The master's next packet (up to 1 s), and where it came from. */
static ssize_t master_recv(int m, uint8_t *buf, size_t cap, struct sockaddr_in *from)
{
    struct pollfd pf = {m, POLLIN, 0};
    if (poll(&pf, 1, 1000) <= 0)
        return -1;
    socklen_t fl = sizeof *from;
    return recvfrom(m, buf, cap, 0, (struct sockaddr *)from, &fl);
}

/* Poll the peer until something arrives (the loopback is quick, but not instant). */
static void settle(rdofnet_peer_t *p, double now)
{
    for (int i = 0; i < 50; i++) {
        rdofnet_peer_poll(p, now);
        usleep(2000);
    }
}

int main(void)
{
    const int m = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    CHECK(m >= 0 && !bind(m, (struct sockaddr *)&a, sizeof a) && !getsockname(m, (struct sockaddr *)&a, &al));
    const int port = ntohs(a.sin_port);

    rdofnet_peer_t p;
    rdofnet_peer_init(&p, "127.0.0.1", port, 9800001, 10.0, "R1");
    p.say = say;
    p.on_packet = on_packet;
    const uint8_t dmrc[] = "DMRC\x00\x2f\x4d\x61 a config";
    rdofnet_peer_set_config(&p, dmrc, sizeof dmrc, 0); /* not open yet: kept for the poll */
    rdofnet_peer_poll(&p, 0);
    uint8_t buf[512];
    struct sockaddr_in from;
    ssize_t n = master_recv(m, buf, sizeof buf, &from);
    CHECK(n == (ssize_t)sizeof dmrc && !memcmp(buf, dmrc, sizeof dmrc)); /* the DMRC at once on open */

    sendto(m, "DMRP\x00\x2f\x4d\x61", 8, 0, (struct sockaddr *)&from, sizeof from);
    settle(&p, 1);
    CHECK(p.peer.up && nsaid == 1 && strstr(said[0], "registered"));

    const uint8_t dmrd[] = "DMRD a burst";
    sendto(m, dmrd, sizeof dmrd, 0, (struct sockaddr *)&from, sizeof from);
    settle(&p, 2);
    CHECK(ngot == sizeof dmrd && !memcmp(got, dmrd, ngot));

    /* the keepalive: the DMRC again at 10 s */
    rdofnet_peer_poll(&p, 10.5);
    n = master_recv(m, buf, sizeof buf, &from);
    CHECK(n == (ssize_t)sizeof dmrc);

    /* refused: said once, however many times it comes */
    sendto(m, "MSTNAK\x00\x2f\x4d\x61", 10, 0, (struct sockaddr *)&from, sizeof from);
    sendto(m, "MSTNAK\x00\x2f\x4d\x61", 10, 0, (struct sockaddr *)&from, sizeof from);
    settle(&p, 11);
    CHECK(!p.peer.up && p.peer.refused == 2 && nsaid == 2 && strstr(said[1], "refused"));

    /* registered again, then quiet: down after three keepalives without a DMRP */
    sendto(m, "DMRP\x00\x2f\x4d\x61", 8, 0, (struct sockaddr *)&from, sizeof from);
    settle(&p, 12);
    CHECK(p.peer.up);
    for (double t = 20.5; t < 60; t += 10)
        rdofnet_peer_poll(&p, t);
    CHECK(!p.peer.up && nsaid == 4 && strstr(said[3], "no DMRP"));
    CHECK(p.peer.tx >= 5 && p.errors == 0);

    rdofnet_peer_close(&p);

    /* a login (the first peer's last keepalives read and dropped) */
    {
        struct pollfd pf = {m, POLLIN, 0};
        while (poll(&pf, 1, 50) > 0)
            recv(m, buf, sizeof buf, 0);
    }
    rdofnet_peer_t b;
    rdofnet_peer_init(&b, "127.0.0.1", port, 310012399, 5.0, "bm");
    b.say = say;
    b.on_packet = on_packet;
    rdofnet_peer_set_login(&b, "passw0rd", "Anytown", "test", NULL);
    nsaid = 0;
    rdofnet_peer_set_config(&b, dmrc, sizeof dmrc, 0); /* no DMRC sent: the login's source */
    rdofnet_peer_poll(&b, 0);
    n = master_recv(m, buf, sizeof buf, &from);
    CHECK(n == 8 && !memcmp(buf, "RPTL", 4) && buf[4] == 0x12 && buf[7] == 0xEF); /* 310012399 = 0x127A6FEF */
    const uint8_t d0[] = "DMRD before the login";
    rdofnet_peer_send(&b, d0, sizeof d0);           /* held: not logged in */
    sendto(m, "RPTACK\x0a\x0b\x0c\x0d", 10, 0, (struct sockaddr *)&from, sizeof from);
    settle(&b, 0.1);
    n = master_recv(m, buf, sizeof buf, &from);
    {
        uint8_t want[32], sp[12] = {0x0a, 0x0b, 0x0c, 0x0d, 'p', 'a', 's', 's', 'w', '0', 'r', 'd'};
        rdof_sha256(sp, sizeof sp, want);
        CHECK(n == 40 && !memcmp(buf, "RPTK", 4) && !memcmp(buf + 8, want, 32));
    }
    sendto(m, "RPTACK\x12\x7a\x6f\xef", 10, 0, (struct sockaddr *)&from, sizeof from);
    settle(&b, 0.2);
    n = master_recv(m, buf, sizeof buf, &from);
    CHECK(n == 302 && !memcmp(buf, "RPTC", 4) && !memcmp(buf + 58, "Anytown ", 8) && !memcmp(buf + 78, "test ", 5));
    CHECK(!b.peer.up);
    sendto(m, "RPTACK\x12\x7a\x6f\xef", 10, 0, (struct sockaddr *)&from, sizeof from);
    settle(&b, 0.3);
    CHECK(b.peer.up && nsaid == 1 && strstr(said[0], "brandmeister") && strstr(said[0], "logged in"));
    rdofnet_peer_send(&b, d0, sizeof d0);
    n = master_recv(m, buf, sizeof buf, &from);
    CHECK(n == (ssize_t)sizeof d0 && !memcmp(buf, "DMRD", 4)); /* the first DMRD the master sees is this one */
    rdofnet_peer_poll(&b, 5.5);
    n = master_recv(m, buf, sizeof buf, &from);
    CHECK(n == 11 && !memcmp(buf, "RPTPING", 7));
    sendto(m, "MSTPONG\x12\x7a\x6f\xef", 11, 0, (struct sockaddr *)&from, sizeof from);
    settle(&b, 5.6);
    CHECK(b.peer.up && b.peer.last_pong >= 5.5);
    rdofnet_peer_close(&b);
    n = master_recv(m, buf, sizeof buf, &from);
    CHECK(n == 9 && !memcmp(buf, "RPTCL", 5));
    close(m);
    printf("rdofnet: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
