// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * fakebm: a simulated BrandMeister master for tests (docs/spec/fw/brandmeister.md, "Tests"). Never the real
 * network: the gateway's HIL runs point a board or rdofradio here on the LAN.
 *
 *   fakebm [--port 62031] [--password P] [--ids ID,ID...] [--nak-step login|auth|config] [--no-pong]
 *          [--reply-s 3] [--reply TEXT] [--no-tms-ack] [--min-preambles 12] [--log FILE]
 *
 * It speaks the Homebrew login (RPTL, RPTACK + salt, RPTK checked against SHA-256(salt || password), RPTC,
 * RPTPING / MSTPONG, MSTNAK, RPTCL), any number of logins at once, each by its ID and address. What a logged-in peer
 * sends:
 *   - a private call to 9990 (voice or data) is recorded and played back to that peer (the gateway's own parrot);
 *   - packet data to 262993 (the SMS service) is taken as the IP gateway takes it (rate 1/2, 3/4 or 1, confirmed or
 *     not, CRC checked): a confirmed packet gets the link-level ACK; a TMS text (UDP 4007) gets the TMS ACK
 *     (`BF 00 <seq>`), then after --reply-s an answer text, as BrandMeister's service does (§7). Unlike
 *     BrandMeister, its own packets go down unconfirmed at rate 1/2 (the IP gateway's). As BrandMeister, it ignores
 *     a packet whose data header came after fewer than --min-preambles preamble CSBKs (12; 0: any), a radio's
 *     response header (its ACK) excepted (brandmeister.md §7);
 *   - a group call goes to every other login that keyed the same TG in the last 15 minutes (BrandMeister's dynamic
 *     talkgroup and its default timer): a repeater's on the slot it keyed it on, a simplex hotspot's on slot 2;
 *   - every packet is logged one line each (stdout, and --log).
 * Each peer's DMRD down carry its own peer ID. Single threaded; POSIX sockets.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "rdof/bits.h"
#include "rdof/fec.h"
#include "rdof/hblink.h"
#include "rdof/hbpeer.h"
#include "rdof/ipgw.h"
#include "rdof/ipgwtx.h"
#include "rdof/layer2.h"
#include "rdof/parrot.h"
#include "rdof/pdu.h"
#include "rdof/sha256.h"

#define MAX_PEERS 16
#define SMS_ID 262993u
#define PARROT_ID 9990u
#define TMS_PORT 4007
#define DYN_TGS 4
#define DYN_S 900.0 /* BrandMeister's default dynamic talkgroup timer: 15 minutes */

enum { ST_FREE = 0, ST_SALTED, ST_AUTHED, ST_UP };

typedef struct {
    int state;
    uint32_t id;
    struct sockaddr_in addr;
    uint8_t salt[4];
    double last;
    uint8_t rptc[RDOF_RPTC_LEN];
    unsigned dmrd_in, dmrd_out;
    uint32_t last_src; /* who was heard here last: the SMS answers go to the peer that heard the radio */
    struct {
        uint32_t tg;
        int slot;
        double at;
    } dyn[DYN_TGS]; /* the TGs keyed here: dynamic subscriptions */
    uint32_t relay_sid; /* the last stream relayed here, for its log line */
    int preambles[3];   /* per slot: preamble CSBKs to the SMS service since its last data header */
    bool ignoring[3];   /* per slot: the packet under way came after too few: its blocks are dropped */
} peer_t;

static struct {
    int sock;
    const char *password;
    uint32_t ids[MAX_PEERS];
    int nids;
    int nak_step; /* -1 none, else ST_FREE (RPTL), ST_SALTED (RPTK), ST_AUTHED (RPTC) */
    int no_pong, no_tms_ack;
    int min_preambles;
    double reply_s;
    const char *reply;
    FILE *log;
    peer_t peers[MAX_PEERS];
    rdof_parrot_t parrot;
    peer_t *parrot_peer;
    rdof_ipgw_t gw;
    uint32_t sid;
    /* the answer due: to the flow, at a time */
    int ans_flow;
    double ans_at;
    uint32_t ans_radio;
    uint8_t ans_seq;
} F;

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void say(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    char ts[32];
    snprintf(ts, sizeof ts, "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000));
    printf("%s %s\n", ts, line);
    fflush(stdout);
    if (F.log) {
        fprintf(F.log, "%s %s\n", ts, line);
        fflush(F.log);
    }
}

static void send_to(const peer_t *p, const uint8_t *d, size_t n)
{
    sendto(F.sock, d, n, 0, (const struct sockaddr *)&p->addr, sizeof p->addr);
}

static void answer(const struct sockaddr_in *a, const char *tag, uint32_t id, const uint8_t *extra, size_t ne)
{
    uint8_t out[32];
    size_t n = strlen(tag);
    memcpy(out, tag, n);
    if (extra) {
        memcpy(out + n, extra, ne);
        n += ne;
    } else {
        rdof_put_be32(out + n, id);
        n += 4;
    }
    sendto(F.sock, out, n, 0, (const struct sockaddr *)a, sizeof *a);
}

static peer_t *by_id(uint32_t id)
{
    for (int i = 0; i < MAX_PEERS; i++)
        if (F.peers[i].state != ST_FREE && F.peers[i].id == id)
            return &F.peers[i];
    return NULL;
}

static bool id_allowed(uint32_t id)
{
    if (!F.nids)
        return true;
    for (int i = 0; i < F.nids; i++)
        if (F.ids[i] == id)
            return true;
    return false;
}

static const char *rptc_text(const uint8_t *r, int off, int len, char *buf)
{
    memcpy(buf, r + off, (size_t)len);
    buf[len] = 0;
    for (int i = len - 1; i >= 0 && buf[i] == ' '; i--)
        buf[i] = 0;
    return buf;
}

/* ---- DMRD down ---- */

static void dmrd_down(peer_t *p, const rdof_net_burst_t *b, uint32_t src, uint32_t dst, bool group, int slot)
{
    uint8_t out[RDOF_DMRD_LEN];
    const int ft = (b->control >> 4) & 0x0F; /* the MMDVM control byte: 0x20 voice sync, 0x40|dt data sync */
    int dt, n = 0;
    if (b->control & 0x40)
        dt = b->control & 0x0F;
    else if (b->control & 0x20)
        dt = RDOF_DT_VOICE_SYNC;
    else
        dt = RDOF_DT_VOICE, n = b->control & 0x0F;
    (void)ft;
    uint32_t sid = rdof_be32(b->stream_id);
    rdof_dmrd_packet(b->seq, src, dst, p->id, slot, group, dt, n, sid, b->burst, 0, 0, out);
    send_to(p, out, sizeof out);
    p->dmrd_out++;
}

/* The replay's own addresses: a private call or data comes back from 9990 to the caller (the parrot swapped them). */
static void parrot_send(void *ctx, const rdof_net_burst_t *b)
{
    if (F.parrot_peer)
        dmrd_down(F.parrot_peer, b, b->src, b->dst, b->group, b->slot);
}

static void parrot_start(void *ctx, const uint8_t sid[4], int slot, uint32_t src, uint32_t dst, bool group)
{
    if (F.parrot_peer)
        say("parrot: back to peer %u: %u -> %u %s s%d, %d bursts", (unsigned)F.parrot_peer->id, (unsigned)src,
            (unsigned)dst, group ? "TG" : "PC", slot, F.parrot.n);
}

static bool gw_send_dmr(void *ctx, const rdof_ipgw_dmr_t *pk)
{
    peer_t *p = NULL;
    for (int i = 0; i < MAX_PEERS && !p; i++)
        if (F.peers[i].state == ST_UP && F.peers[i].last_src == pk->dst)
            p = &F.peers[i];
    for (int i = 0; i < MAX_PEERS && !p; i++)
        if (F.peers[i].state == ST_UP)
            p = &F.peers[i];
    if (!p)
        return false;
    uint8_t sid[4];
    rdof_put_be32(sid, ++F.sid | 0x5A000000u);
    const int slot = pk->slot ? pk->slot : 2;
    for (int i = 0; i < pk->nbursts; i++) {
        rdof_net_burst_t b;
        rdof_ipgw_net_burst(pk, i, sid, slot, (uint8_t)i, &b);
        dmrd_down(p, &b, pk->src, pk->dst, false, slot);
    }
    say("down %u -> %u to peer %u: %s, %d bursts, slot %d", (unsigned)pk->src, (unsigned)pk->dst, (unsigned)p->id,
        pk->response ? "response (link ACK)" : "data", pk->nbursts, slot);
    return true;
}

static bool gw_open(void *ctx, int flow) { return true; }
static void gw_close(void *ctx, int flow) {}

/* A TMS packet's text (UTF-16LE to ASCII, '?' for the rest). */
static void tms_text(const uint8_t *t, size_t n, char *out, size_t cap)
{
    size_t k = 0;
    for (size_t i = 0; i + 1 < n && k + 1 < cap; i += 2) {
        const unsigned c = t[i] | t[i + 1] << 8;
        if (c == '\r' || c == '\n')
            continue;
        out[k++] = c >= 32 && c < 127 ? (char)c : '?';
    }
    out[k] = 0;
}

/* What the route to 262993 sends: the UDP payload of the radio's datagram, on flow. */
static bool gw_send_udp(void *ctx, int flow, uint32_t ip, uint16_t port, const uint8_t *d, size_t n, int ttl)
{
    const uint32_t radio = F.gw.flows[flow].radio;
    if (n < 5) {
        say("sms from %u: %zu bytes, too short for TMS", (unsigned)radio, n);
        return true;
    }
    const unsigned len = (unsigned)d[0] << 8 | d[1], h = d[2];
    size_t i = 4 + d[3];
    int seq = -1;
    if (h & 0x80 && i < n) {
        seq = d[i] & 0x1F;
        const bool ext = d[i] & 0x80;
        i++;
        if (ext && i < n)
            seq |= d[i] & 0x60, i++;
    }
    if ((h & 0x1F) == 0x1F) {
        say("sms from %u: TMS ACK, sequence %d", (unsigned)radio, seq);
        return true;
    }
    char text[200];
    tms_text(d + i, n > i ? (len + 2 < n ? len + 2 : n) - i : 0, text, sizeof text);
    say("sms from %u: TMS text (header %02X%s, sequence %d) \"%s\"", (unsigned)radio, h,
        h & 0x40 ? ", ACK asked" : "", seq, text);
    if (h & 0x40 && seq >= 0 && !F.no_tms_ack) {
        uint8_t ack[6];
        size_t an = 0;
        ack[an++] = 0;
        ack[an++] = 0;
        ack[an++] = 0xBF;
        ack[an++] = 0x00;
        if (seq >= 32) {
            ack[an++] = (uint8_t)(0x80 | (seq & 0x1F));
            ack[an++] = (uint8_t)(seq & 0x60);
        } else {
            ack[an++] = (uint8_t)seq;
        }
        ack[1] = (uint8_t)(an - 2);
        rdof_ipgw_from_net(&F.gw, flow, ip, port, ack, an, now_s());
        say("sms to %u: TMS ACK, sequence %d", (unsigned)radio, seq);
    }
    if (F.reply && F.reply[0]) {
        F.ans_flow = flow;
        F.ans_at = now_s() + F.reply_s;
        F.ans_radio = radio;
    }
    return true;
}

static void sms_answer_due(double now)
{
    if (F.ans_flow < 0 || now < F.ans_at)
        return;
    const int flow = F.ans_flow;
    F.ans_flow = -1;
    if (!F.gw.flows[flow].used)
        return;
    uint8_t m[256];
    size_t n = 2;
    const uint8_t seq = (uint8_t)(F.ans_seq++ & 0x1F);
    m[n++] = 0xE0; /* text, ACK asked */
    m[n++] = 0x00;
    m[n++] = (uint8_t)(0x80 | seq);
    m[n++] = 0x04; /* UTF-16LE */
    const char *t = "\r\n";
    for (int pass = 0; pass < 2; pass++, t = F.reply)
        for (; *t && n + 2 < sizeof m; t++) {
            m[n++] = (uint8_t)*t;
            m[n++] = 0;
        }
    m[0] = (uint8_t)((n - 2) >> 8);
    m[1] = (uint8_t)(n - 2);
    rdof_ipgw_from_net(&F.gw, flow, F.gw.flows[flow].peer_ip, F.gw.flows[flow].peer_port, m, n, now);
    say("sms to %u: TMS text, sequence %u \"%s\"", (unsigned)F.ans_radio, seq, F.reply);
}

/* ---- packets from the peers ---- */

static const char *dt_name(int dt)
{
    static const char *const N[] = {"PI",   "VLC",  "TLC",  "CSBK", "MBCH", "MBCC", "DATA", "R12",
                                    "R34",  "IDLE", "R1",   "dt11", "dt12", "dt13", "dt14", "dt15"};
    return N[dt & 15];
}

static void relay_group(peer_t *from, const uint8_t *d, size_t n, const rdof_net_burst_t *b, double now);

static void dmrd_up(peer_t *p, const uint8_t *d, size_t n, double now)
{
    rdof_net_burst_t b;
    if (!rdof_parse_dmrd(d, n, &b))
        return;
    p->dmrd_in++;
    p->last_src = b.src;
    const int ft = (d[15] >> 4) & 3, dt = d[15] & 0x0F;
    const uint32_t sid = rdof_be32(d + 16);
    if (ft == 2) /* data sync: a line each; voice: only the first */
        say("DMRD peer %u %u -> %u %s s%d %s sid=%08x", (unsigned)p->id, (unsigned)b.src, (unsigned)b.dst,
            b.group ? "TG" : "PC", b.slot, dt_name(dt), (unsigned)sid);
    else if (ft == 1 && d[4] == 0)
        say("DMRD peer %u %u -> %u %s s%d voice sid=%08x", (unsigned)p->id, (unsigned)b.src, (unsigned)b.dst,
            b.group ? "TG" : "PC", b.slot, (unsigned)sid);
    if (b.dst == PARROT_ID && !b.group) {
        F.parrot_peer = p;
        rdof_parrot_heard(&F.parrot, b.slot, b.control, b.burst, now);
    }
    if (b.group)
        relay_group(p, d, n, &b, now);
    if (b.dst == SMS_ID && !b.group && ft == 2) {
        const int s = b.slot == 1 ? 1 : 2;
        if (dt == RDOF_DT_CSBK) {
            p->preambles[s]++;
        } else if (dt == RDOF_DT_DATA_HEADER) {
            uint8_t h[12];
            const bool response = rdof_bptc196_decode(b.burst, h) && (h[0] & 0x0F) == 1;
            p->ignoring[s] = !response && p->preambles[s] < F.min_preambles;
            if (p->ignoring[s])
                say("sms from %u: ignored, %d preamble CSBKs before its header (BrandMeister answers after %d)",
                    (unsigned)b.src, p->preambles[s], F.min_preambles);
            p->preambles[s] = 0;
        }
        if (p->ignoring[s])
            return;
        const rdof_ipgw_where_t w = {.source = (int)(p - F.peers), .slot = b.slot, .freq = 0, .cc = -1};
        rdof_ipgw_burst(&F.gw, &w, dt, b.burst, now);
    }
}

/* A group call: this login subscribes to its TG on its slot, and it goes to every other login subscribed to it. */
static void relay_group(peer_t *from, const uint8_t *d, size_t n, const rdof_net_burst_t *b, double now)
{
    int k = 0;
    for (int i = 0; i < DYN_TGS; i++) {
        if (from->dyn[i].tg == b->dst) {
            k = i;
            break;
        }
        if (from->dyn[i].at < from->dyn[k].at)
            k = i;
    }
    from->dyn[k].tg = b->dst;
    from->dyn[k].slot = b->slot;
    from->dyn[k].at = now;
    for (int i = 0; i < MAX_PEERS; i++) {
        peer_t *p = &F.peers[i];
        if (p == from || p->state != ST_UP)
            continue;
        int slot = 0;
        for (int j = 0; j < DYN_TGS; j++)
            if (p->dyn[j].tg == b->dst && now - p->dyn[j].at < DYN_S)
                slot = p->dyn[j].slot;
        if (!slot)
            continue;
        if (p->rptc[97] == '4')
            slot = 2; /* a simplex hotspot: slot 2 */
        uint8_t out[RDOF_DMRD_LEN + 2];
        const size_t m = n < sizeof out ? n : sizeof out;
        memcpy(out, d, m);
        rdof_put_be32(out + 11, p->id);
        out[15] = (uint8_t)((out[15] & 0x7F) | (slot == 2 ? 0x80 : 0));
        send_to(p, out, m);
        p->dmrd_out++;
        const uint32_t sid = rdof_be32(d + 16);
        if (sid != p->relay_sid) {
            p->relay_sid = sid;
            say("relay TG %u from peer %u s%d to peer %u s%d", (unsigned)b->dst, (unsigned)from->id, b->slot,
                (unsigned)p->id, slot);
        }
    }
}

static void received(const struct sockaddr_in *a, const uint8_t *d, size_t n, double now)
{
    if (n >= 8 && !memcmp(d, "RPTL", 4)) {
        const uint32_t id = rdof_be32(d + 4);
        if (F.nak_step == ST_FREE || !id_allowed(id)) {
            say("RPTL %u from %s: MSTNAK", (unsigned)id, inet_ntoa(a->sin_addr));
            answer(a, "MSTNAK", id, NULL, 0);
            return;
        }
        peer_t *p = by_id(id);
        for (int i = 0; i < MAX_PEERS && !p; i++)
            if (F.peers[i].state == ST_FREE)
                p = &F.peers[i];
        if (!p) {
            answer(a, "MSTNAK", id, NULL, 0);
            return;
        }
        memset(p, 0, sizeof *p);
        p->id = id;
        p->addr = *a;
        p->state = ST_SALTED;
        p->last = now;
        for (int i = 0; i < 4; i++)
            p->salt[i] = (uint8_t)rand();
        answer(a, "RPTACK", 0, p->salt, 4);
        say("RPTL %u from %s:%d: salt %02x%02x%02x%02x", (unsigned)id, inet_ntoa(a->sin_addr), ntohs(a->sin_port),
            p->salt[0], p->salt[1], p->salt[2], p->salt[3]);
        return;
    }
    const uint32_t id = n >= 11 && !memcmp(d, "RPTPING", 7) ? rdof_be32(d + 7)
                        : n >= 9 && !memcmp(d, "RPTCL", 5)  ? rdof_be32(d + 5)
                        : n >= 8                            ? rdof_be32(d + (memcmp(d, "DMRD", 4) ? 4 : 11))
                                                            : 0;
    peer_t *p = by_id(id);
    if (!p || p->addr.sin_addr.s_addr != a->sin_addr.s_addr || p->addr.sin_port != a->sin_port) {
        if (!memcmp(d, "DMRD", 4))
            return; /* not logged in: ignored, as BrandMeister does */
        say("%.7s from %s for %u: not logged in: MSTNAK", (const char *)d, inet_ntoa(a->sin_addr), (unsigned)id);
        answer(a, "MSTNAK", id, NULL, 0);
        return;
    }
    p->last = now;
    if (n == 40 && !memcmp(d, "RPTK", 4)) {
        uint8_t want[32];
        rdof_sha256_t h;
        rdof_sha256_init(&h);
        rdof_sha256_update(&h, p->salt, 4);
        rdof_sha256_update(&h, F.password, strlen(F.password));
        rdof_sha256_final(&h, want);
        if (p->state != ST_SALTED || memcmp(want, d + 8, 32) || F.nak_step == ST_SALTED) {
            say("RPTK %u: %s: MSTNAK", (unsigned)id, p->state != ST_SALTED ? "out of order" : "wrong password");
            answer(a, "MSTNAK", id, NULL, 0);
            p->state = ST_FREE;
            return;
        }
        p->state = ST_AUTHED;
        answer(a, "RPTACK", id, NULL, 0);
        say("RPTK %u: password good", (unsigned)id);
    } else if (n >= RDOF_RPTC_LEN && !memcmp(d, "RPTC", 4)) {
        /* BrandMeister refuses an RPTC whose package ID isn't MMDVM (docs/spec/fw/brandmeister.md §3, RPTC) */
        static const char mmdvm[] = "MMDVM                                   ";
        const bool pkg_ok = !memcmp(d + 262, mmdvm, 40);
        if (p->state < ST_AUTHED || F.nak_step == ST_AUTHED || !pkg_ok) {
            say("RPTC %u: MSTNAK%s", (unsigned)id, pkg_ok ? "" : " (package ID not MMDVM)");
            answer(a, "MSTNAK", id, NULL, 0);
            p->state = ST_FREE;
            return;
        }
        memcpy(p->rptc, d, RDOF_RPTC_LEN);
        p->state = ST_UP;
        answer(a, "RPTACK", id, NULL, 0);
        char c[9], rx[10], tx[10], cc[3], loc[21], desc[20], sw[41], pkg[41];
        say("RPTC %u: up: %s rx %s tx %s cc %s slots %c \"%s\" \"%s\" sw \"%s\" pkg \"%s\"", (unsigned)id,
            rptc_text(d, 8, 8, c), rptc_text(d, 16, 9, rx), rptc_text(d, 25, 9, tx), rptc_text(d, 36, 2, cc), d[97],
            rptc_text(d, 58, 20, loc), rptc_text(d, 78, 19, desc), rptc_text(d, 222, 40, sw),
            rptc_text(d, 262, 40, pkg));
    } else if (n >= 11 && !memcmp(d, "RPTPING", 7)) {
        if (p->state == ST_UP && !F.no_pong)
            answer(a, "MSTPONG", id, NULL, 0);
    } else if (n >= 9 && !memcmp(d, "RPTCL", 5)) {
        say("RPTCL %u: closed", (unsigned)id);
        p->state = ST_FREE;
    } else if (n >= 53 && !memcmp(d, "DMRD", 4)) {
        if (p->state == ST_UP)
            dmrd_up(p, d, n, now);
    } else {
        say("%.4s from %u: ignored (%zu bytes)", (const char *)d, (unsigned)id, n);
    }
}

static void usage(void)
{
    fprintf(stderr, "usage: fakebm [--port 62031] [--password P] [--ids ID,...] [--nak-step login|auth|config]\n"
                    "              [--no-pong] [--reply-s S] [--reply TEXT] [--no-tms-ack] [--min-preambles 12]\n"
                    "              [--log FILE]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    int port = 62031;
    F.password = "passw0rd";
    F.nak_step = -1;
    F.min_preambles = 12;
    F.reply_s = 3.0;
    F.reply = "fakebm got it";
    F.ans_flow = -1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--port") && v)
            port = atoi(v), i++;
        else if (!strcmp(a, "--password") && v)
            F.password = v, i++;
        else if (!strcmp(a, "--ids") && v) {
            for (char *s = argv[++i]; *s && F.nids < MAX_PEERS;) {
                F.ids[F.nids++] = (uint32_t)strtoul(s, &s, 10);
                if (*s == ',')
                    s++;
            }
        } else if (!strcmp(a, "--nak-step") && v) {
            F.nak_step = !strcmp(v, "login") ? ST_FREE : !strcmp(v, "auth") ? ST_SALTED : ST_AUTHED;
            i++;
        } else if (!strcmp(a, "--no-pong"))
            F.no_pong = 1;
        else if (!strcmp(a, "--min-preambles") && v)
            F.min_preambles = atoi(v), i++;
        else if (!strcmp(a, "--no-tms-ack"))
            F.no_tms_ack = 1;
        else if (!strcmp(a, "--reply-s") && v)
            F.reply_s = atof(v), i++;
        else if (!strcmp(a, "--reply") && v)
            F.reply = v, i++;
        else if (!strcmp(a, "--log") && v)
            F.log = fopen(v, "a"), i++;
        else
            usage();
    }
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    F.sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in me = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = INADDR_ANY};
    if (F.sock < 0 || bind(F.sock, (struct sockaddr *)&me, sizeof me) < 0) {
        perror("fakebm: bind");
        return 1;
    }
    rdof_parrot_init(&F.parrot, PARROT_ID);
    F.parrot.send = parrot_send;
    F.parrot.start = parrot_start;
    rdof_ipgw_config_t gc;
    rdof_ipgw_config_default(&gc);
    gc.on = true;
    gc.ack = true;
    gc.id = SMS_ID;
    gc.up_pps = gc.down_pps = 0;
    rdof_ipgw_route_t *r = &gc.routes[gc.nroutes++];
    memset(r, 0, sizeof *r);
    snprintf(r->name, sizeof r->name, "sms");
    r->dst_kind = RDOF_IPGW_DST_PRIVATE;
    r->dst_lo = r->dst_hi = SMS_ID;
    snprintf(r->host, sizeof r->host, "127.0.0.1");
    r->ip = 0x7F000001u;
    r->port = TMS_PORT;
    r->payload = RDOF_IPGW_PAYLOAD_UDP;
    rdof_ipgw_init(&F.gw, &gc);
    F.gw.open = gw_open;
    F.gw.close = gw_close;
    F.gw.send_udp = gw_send_udp;
    F.gw.send_dmr = gw_send_dmr;
    say("fakebm on UDP %d%s%s", port, F.nids ? ", IDs limited" : "", F.nak_step >= 0 ? ", refusing a step" : "");
    double last_status = now_s();
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(F.sock, &fds);
        struct timeval tv = {0, 20000};
        if (select(F.sock + 1, &fds, NULL, NULL, &tv) > 0) {
            uint8_t buf[1500];
            struct sockaddr_in a;
            socklen_t al = sizeof a;
            const ssize_t n = recvfrom(F.sock, buf, sizeof buf, 0, (struct sockaddr *)&a, &al);
            if (n > 0)
                received(&a, buf, (size_t)n, now_s());
        }
        const double now = now_s();
        rdof_parrot_poll(&F.parrot, now);
        rdof_ipgw_tick(&F.gw, now);
        sms_answer_due(now);
        for (int i = 0; i < MAX_PEERS; i++)
            if (F.peers[i].state != ST_FREE && now - F.peers[i].last > 60) {
                say("peer %u: nothing for 60 s: dropped", (unsigned)F.peers[i].id);
                F.peers[i].state = ST_FREE;
            }
        if (now - last_status > 60) {
            last_status = now;
            for (int i = 0; i < MAX_PEERS; i++)
                if (F.peers[i].state == ST_UP)
                    say("peer %u up: %u DMRD in, %u out", (unsigned)F.peers[i].id, F.peers[i].dmrd_in,
                        F.peers[i].dmrd_out);
        }
    }
}
