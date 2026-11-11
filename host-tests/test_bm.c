// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The BrandMeister login (rdof/hbpeer.h with rdof_hb_peer_set_login) against a fake master in process: SHA-256,
 * RPTC from a DMRC, the login, pings and pongs, a wrong password, MSTNAK at each step, lost answers, MSTCL, the
 * back-off, a config change, DMRD only while logged in. No network. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "rdof/bits.h"
#include "rdof/config.h"
#include "rdof/hblink.h"
#include "rdof/hbpeer.h"
#include "rdof/sha256.h"

static int checks, failures;

#define CHECK(c)                                                                                                     \
    do {                                                                                                             \
        checks++;                                                                                                    \
        if (!(c)) {                                                                                                  \
            failures++;                                                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                      \
        }                                                                                                            \
    } while (0)

static void hex(const char *h, uint8_t *out)
{
    for (size_t i = 0; h[2 * i]; i++) {
        unsigned v;
        sscanf(h + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static void test_sha256(void)
{
    uint8_t got[32], want[32];
    rdof_sha256("abc", 3, got);
    hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want);
    CHECK(!memcmp(got, want, 32));
    rdof_sha256("", 0, got);
    hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", want);
    CHECK(!memcmp(got, want, 32));
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"; /* two blocks of padding */
    rdof_sha256(m, strlen(m), got);
    hex("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", want);
    CHECK(!memcmp(got, want, 32));
    rdof_sha256_t s; /* fed a byte at a time */
    rdof_sha256_init(&s);
    for (size_t i = 0; m[i]; i++)
        rdof_sha256_update(&s, m + i, 1);
    rdof_sha256_final(&s, got);
    CHECK(!memcmp(got, want, 32));
}

/* ---- the fake master ---- */
#define PASSWORD "passw0rd"
static const uint8_t SALT[4] = {1, 2, 3, 4};
static struct {
    bool nak_at[4];  /* MSTNAK at RPTL, RPTK, RPTC, RPTPING */
    bool deaf;       /* answers nothing */
    bool no_pong;
    int logins, pings, configs;
    uint8_t rptc[RDOF_RPTC_LEN];
    uint32_t id;
} M;

/* The master's answer to what the peer sent (its length, 0: none). */
static size_t master(const uint8_t *d, size_t n, uint8_t *out)
{
    if (M.deaf)
        return 0;
    const uint32_t id = n >= 8 ? rdof_be32(d + (n >= 11 && !memcmp(d, "RPTPING", 7) ? 7 : 4)) : 0;
    int step = -1;
    if (n == 8 && !memcmp(d, "RPTL", 4))
        step = 0, M.logins++, M.id = id;
    else if (n == 40 && !memcmp(d, "RPTK", 4))
        step = 1;
    else if (n == RDOF_RPTC_LEN && !memcmp(d, "RPTC", 4))
        step = 2, M.configs++, memcpy(M.rptc, d, n);
    else if (n == 11 && !memcmp(d, "RPTPING", 7))
        step = 3, M.pings++;
    if (step < 0)
        return 0;
    if (M.nak_at[step]) {
        memcpy(out, "MSTNAK", 6);
        rdof_put_be32(out + 6, id);
        return 10;
    }
    if (step == 1) { /* check the hash */
        uint8_t want[32];
        rdof_sha256_t h;
        rdof_sha256_init(&h);
        rdof_sha256_update(&h, SALT, 4);
        rdof_sha256_update(&h, PASSWORD, strlen(PASSWORD));
        rdof_sha256_final(&h, want);
        if (memcmp(d + 8, want, 32)) {
            memcpy(out, "MSTNAK", 6);
            rdof_put_be32(out + 6, id);
            return 10;
        }
    }
    if (step == 3) {
        if (M.no_pong)
            return 0;
        memcpy(out, "MSTPONG", 7);
        rdof_put_be32(out + 7, id);
        return 11;
    }
    memcpy(out, "RPTACK", 6);
    if (step == 0)
        memcpy(out + 6, SALT, 4);
    else
        rdof_put_be32(out + 6, id);
    return 10;
}

static uint8_t DMRC[RDOF_DMRC_MAX];
static size_t DMRC_N;
static rdof_hbp_event_t last_ev[8];
static int nev;

/* Run the peer and the master from t0 to t1 in 50 ms steps, as a board's loop would. */
static void run(rdof_hb_peer_t *p, double t0, double t1)
{
    for (double t = t0; t < t1; t += 0.05) {
        rdof_hbp_event_t ev;
        if (rdof_hb_peer_tick(p, t, &ev)) {
            rdof_hb_peer_set_config(p, DMRC, DMRC_N, true, t);
            uint8_t pkt[RDOF_HB_OUT_MAX], ans[64];
            const size_t n = rdof_hb_peer_packet(p, pkt);
            const size_t k = master(pkt, n, ans);
            if (k) {
                rdof_hbp_event_t e = rdof_hb_peer_received(p, ans, k, t + 0.01);
                if (e != RDOF_HBP_NONE && nev < 8)
                    last_ev[nev++] = e;
            }
        }
        if (ev != RDOF_HBP_NONE && nev < 8)
            last_ev[nev++] = ev;
    }
}

static void reset(rdof_hb_peer_t *p, const char *password)
{
    memset(&M, 0, sizeof M);
    nev = 0;
    rdof_hb_peer_init(p, 310012399, 5.0);
    rdof_hb_peer_set_login(p, password, "Trussville, AL", "RadioFoundry test", "https://example.org");
    rdof_hb_peer_link_up(p);
}

static void test_rptc(void)
{
    const rdof_location_t loc = {33.6198, -86.6089, 12};
    const uint32_t idle = 438500000;
    const int idle_cc = 1;
    DMRC_N = rdof_dmrc_packet(310012399, "N0CALL", 438500000, 438500000, 1, '4', 5, "RadioFoundry Gateway roaming",
                              "radiofoundry_gateway simplex", &loc, &idle, &idle_cc, DMRC);
    uint8_t r[RDOF_RPTC_LEN];
    CHECK(rdof_rptc_from_dmrc(DMRC, DMRC_N, 37, "Trussville, AL", "RadioFoundry test", "https://example.org", r) ==
          RDOF_RPTC_LEN);
    CHECK(!memcmp(r, "RPTC", 4) && rdof_be32(r + 4) == 310012399);
    CHECK(!memcmp(r + 8, "N0CALL  ", 8));
    CHECK(!memcmp(r + 16, "438500000438500000", 18));
    CHECK(!memcmp(r + 34, "3701", 4)); /* dBm, not the DMRC's RF level (5) */
    CHECK(!memcmp(r + 38, "33.61980-86.60890012", 20));
    CHECK(!memcmp(r + 58, "Trussville, AL      ", 20));
    CHECK(!memcmp(r + 78, "RadioFoundry test  ", 19));
    CHECK(r[97] == '4');
    CHECK(!memcmp(r + 98, "https://example.org ", 20) && r[221] == ' ');
    CHECK(!memcmp(r + 222, "RadioFoundry Gateway roaming", 28) && r[261] == ' ');
    CHECK(!memcmp(r + 262, "MMDVM ", 6) && r[301] == ' ');
    /* no location in the DMRC: zeros */
    uint8_t d2[RDOF_DMRC_MAX];
    size_t n2 = rdof_dmrc_packet(310012399, "N0CALL", 438500000, 438500000, 1, '4', 5, "v", "s", NULL, NULL, NULL, d2);
    rdof_rptc_from_dmrc(d2, n2, 120, NULL, NULL, NULL, r);
    CHECK(!memcmp(r + 34, "99", 2) && !memcmp(r + 38, "0.00000000.000000000", 20) && r[58] == ' ' && r[97] == '4');
    rdof_rptc_from_dmrc(d2, n2, -3, NULL, NULL, NULL, r);
    CHECK(!memcmp(r + 34, "00", 2));
    /* the peer's power: rounded, held to 0..99, NAN 0 */
    rdof_hb_peer_t p;
    rdof_hb_peer_init(&p, 1, 5.0);
    rdof_hb_peer_set_power(&p, 36.6, 0.0);
    CHECK(p.power_dbm == 37);
    rdof_hb_peer_set_power(&p, NAN, 0.0);
    CHECK(p.power_dbm == 0);
    rdof_hb_peer_set_power(&p, 140.0, 0.0);
    CHECK(p.power_dbm == 99);
    /* a role's power on its map: a W12-like 0..255 over -9.5..22 dBm */
    rdof_config_t c;
    memset(&c, 0, sizeof c);
    c.mode = RDOF_CFG_REPEATER;
    c.power = 255;
    CHECK(isnan(rdof_hb_role_power_dbm(&c, 0)));
    c.tx_map = (rdof_txmap_t){-9.5, 22.0};
    CHECK(fabs(rdof_hb_role_power_dbm(&c, 0) - 22.0) < 1e-9);
    c.mode = RDOF_CFG_ROAMER;
    c.nchannels = 2;
    c.channels[0] = (rdof_cfg_channel_t){.freq = 446050000, .power = 0};
    c.channels[1] = (rdof_cfg_channel_t){.freq = 446100000, .power = 255};
    CHECK(fabs(rdof_hb_role_power_dbm(&c, 446100000) - 22.0) < 1e-9);
    CHECK(fabs(rdof_hb_role_power_dbm(&c, 1) + 9.5) < 1e-9);
}

static void test_login(void)
{
    rdof_hb_peer_t p;
    uint8_t pkt[RDOF_HB_OUT_MAX];

    /* the whole login, then pings */
    reset(&p, PASSWORD);
    CHECK(!rdof_hb_peer_may_send(&p) && !strcmp(rdof_hb_peer_state_name(&p), "login"));
    run(&p, 0, 0.2);
    CHECK(p.up && p.state == RDOF_HBL_UP && M.logins == 1 && M.configs == 1 && M.id == 310012399);
    CHECK(nev == 1 && last_ev[0] == RDOF_HBP_REGISTERED && rdof_hb_peer_may_send(&p));
    CHECK(!memcmp(M.rptc + 8, "N0CALL  ", 8) && rdof_be32(M.rptc + 4) == 310012399);
    run(&p, 0.2, 30);
    CHECK(p.up && M.pings == 5 && M.logins == 1); /* every 5 s from 0.06 */
    CHECK(!strcmp(rdof_hb_peer_state_name(&p), "up"));
    CHECK(rdof_hb_peer_close_packet(&p, pkt) == 9 && !memcmp(pkt, "RPTCL", 5) && rdof_be32(pkt + 5) == 310012399);

    /* the hash as BrandMeister checks it: SHA-256(salt || password) */
    reset(&p, PASSWORD);
    run(&p, 0, 0.01);          /* RPTL sent and answered */
    CHECK(p.state == RDOF_HBL_AUTH && p.pending);
    {
        rdof_hbp_event_t ev;
        CHECK(rdof_hb_peer_tick(&p, 0.02, &ev));
        CHECK(rdof_hb_peer_packet(&p, pkt) == 40 && !memcmp(pkt, "RPTK", 4));
        uint8_t want[32];
        hex("3bee14ffd8e5cfe58a54e3420b826556c4a585fb37474ed28db2f653f303213d", want);
        CHECK(!memcmp(pkt + 8, want, 32));
    }

    /* a wrong password: refused at RPTK, 60 s before the next try */
    reset(&p, "wrong");
    run(&p, 0, 1);
    CHECK(!p.up && p.state == RDOF_HBL_BACKOFF && p.bad_password && p.refused == 1 && p.refused_at == RDOF_HBL_AUTH);
    CHECK(nev == 1 && last_ev[0] == RDOF_HBP_REFUSED && !strcmp(rdof_hb_peer_state_name(&p), "bad_password"));
    run(&p, 1, 59);
    CHECK(M.logins == 1);
    run(&p, 59, 62);
    CHECK(M.logins == 2);

    /* MSTNAK at RPTL, then at RPTC: back off 1, 2, 4 s */
    reset(&p, PASSWORD);
    M.nak_at[0] = true;
    run(&p, 0, 0.5);
    CHECK(p.state == RDOF_HBL_BACKOFF && !p.bad_password && p.refused_at == RDOF_HBL_LOGIN);
    CHECK(!strcmp(rdof_hb_peer_state_name(&p), "refused"));
    run(&p, 0.5, 7.2); /* tries at 0, ~1, ~3, ~7 */
    CHECK(M.logins == 4);
    M.nak_at[0] = false;
    M.nak_at[2] = true;
    run(&p, 7.2, 30);
    CHECK(!p.up && p.refused_at == RDOF_HBL_CONFIG && M.configs >= 1);
    M.nak_at[2] = false;
    run(&p, 30, 100);
    CHECK(p.up && p.backoff_s == 1.0); /* back-off reset once up */

    /* no answer at all: a try every back-off, 5 s for each */
    reset(&p, PASSWORD);
    M.deaf = true;
    run(&p, 0, 5.2);
    CHECK(M.logins == 0 && p.state == RDOF_HBL_BACKOFF && p.logins == 1);
    run(&p, 5.2, 6.4);
    CHECK(p.logins == 2 && p.state == RDOF_HBL_LOGIN);
    M.deaf = false;

    /* pongs stop: down after three pings unanswered, then the login again */
    reset(&p, PASSWORD);
    run(&p, 0, 1);
    M.no_pong = true;
    nev = 0;
    run(&p, 1, 20.5);
    CHECK(!p.up && nev == 1 && last_ev[0] == RDOF_HBP_QUIET && p.pings_missed == 1);
    M.no_pong = false;
    run(&p, 20.5, 25);
    CHECK(p.up && M.logins == 2);

    /* status: a login's state as JSON; none without a login */
    char st[192];
    CHECK(rdof_hb_peer_login_status(&p, st, sizeof st) > 0 && strstr(st, "\"state\":\"up\"") &&
          strstr(st, "\"refused_at\":\"\"") && st[0] == '{' && st[strlen(st) - 1] == '}');
    rdof_hb_peer_t plain;
    rdof_hb_peer_init(&plain, 1, 5.0);
    CHECK(rdof_hb_peer_login_status(&plain, st, sizeof st) == 0 && !st[0]);

    /* MSTCL while up: refused, logs in again after 1 s */
    reset(&p, PASSWORD);
    run(&p, 0, 1);
    CHECK(rdof_hb_peer_received(&p, (const uint8_t *)"MSTCL\x12\x7b\x6f\xef", 9, 1.0) == RDOF_HBP_REFUSED);
    CHECK(!p.up && p.refused_at == RDOF_HBL_UP && !strcmp(rdof_hb_peer_state_name(&p), "backoff"));
    rdof_hb_peer_login_status(&p, st, sizeof st);
    CHECK(strstr(st, "\"state\":\"backoff\"") && strstr(st, "\"refused_at\":\"up\"") &&
          strstr(st, "\"refusal\":\"4d5354434c127b6fef\""));
    run(&p, 1.0, 3);
    CHECK(p.up && M.logins == 2);

    /* DMRD only while logged in; RPTSBKN ignored */
    uint8_t dmrd[RDOF_DMRD_LEN] = {'D', 'M', 'R', 'D'};
    CHECK(rdof_hb_peer_received(&p, dmrd, sizeof dmrd, 3) == RDOF_HBP_PACKET);
    CHECK(rdof_hb_peer_received(&p, (const uint8_t *)"RPTSBKN\x12\x7b\x6f\xef", 11, 3) == RDOF_HBP_NONE);
    rdof_hb_peer_link_down(&p);
    CHECK(rdof_hb_peer_received(&p, dmrd, sizeof dmrd, 3) == RDOF_HBP_NONE && !rdof_hb_peer_may_send(&p));
    rdof_hb_peer_link_up(&p);
    run(&p, 3, 4);
    CHECK(p.up && M.logins == 3);
    /* a simplex login ('4'): DMRD goes up on slot 2; anything else as it is */
    uint8_t tmp[RDOF_HB_OUT_MAX];
    dmrd[15] = 0x01; /* slot 1, a group call, voice B */
    const uint8_t *up = rdof_hb_peer_up(&p, dmrd, sizeof dmrd, tmp);
    CHECK(up == tmp && tmp[15] == 0x81 && !memcmp(tmp + 16, dmrd + 16, sizeof dmrd - 16) && dmrd[15] == 0x01);
    CHECK(rdof_hb_peer_up(&p, (const uint8_t *)"RPTPING\x12\x7b\x6f\xef", 11, tmp) != tmp);
    p.dmrc[38] = '3';
    CHECK(rdof_hb_peer_up(&p, dmrd, sizeof dmrd, tmp) == dmrd);
    p.dmrc[38] = '4';

    /* a new config while up: log in again with it */
    uint8_t old[RDOF_DMRC_MAX];
    size_t oldn = DMRC_N;
    memcpy(old, DMRC, DMRC_N);
    DMRC_N = rdof_dmrc_packet(310012399, "N0CALL", 438600000, 438600000, 2, '4', 5, "v", "s", NULL, NULL, NULL, DMRC);
    run(&p, 4, 9.5);
    CHECK(p.up && M.logins == 4 && !memcmp(M.rptc + 16, "438600000", 9) && !memcmp(M.rptc + 36, "02", 2));
    memcpy(DMRC, old, oldn);
    DMRC_N = oldn;

    /* without a login nothing changed: DMRC is the packet */
    rdof_hb_peer_init(&p, 9800001, 10);
    rdof_hb_peer_set_config(&p, DMRC, DMRC_N, true, 0);
    CHECK(rdof_hb_peer_packet(&p, pkt) == DMRC_N && !memcmp(pkt, "DMRC", 4) && rdof_hb_peer_may_send(&p));
    CHECK(rdof_hb_peer_close_packet(&p, pkt) == 0);
}

/* A simplex frequency's login: RPTC carries the owner's callsign (BrandMeister checks it against the ID), not the
   frequency's name as an HBlink4 per_path peer does, and says hotspot. */
static void test_callsign(void)
{
    static const char json[] =
        "{\"version\": 3, \"callsign\": \"N0CALL\", \"defaults\": {\"cc\": 1},"
        " \"devices\": [{\"name\": \"w12\", \"kind\": \"sdr\", \"driver\": \"lr2021\"}],"
        " \"frequencies\": [{\"name\": \"438.500\", \"freq\": 438.5}],"
        " \"networks\": [{\"type\": \"brandmeister\", \"host\": \"bm.example.net\", \"password\": \"example\","
        " \"peer_ids\": {\"438.500\": 310012301}}]}";
    static rdof_config_t cfg;
    char error[256] = "";
    bool ok = rdof_config_parse(json, strlen(json), &cfg, error, sizeof error);
    if (!ok)
        printf("config: %s\n", error);
    CHECK(ok);
    if (!ok)
        return;
    CHECK(cfg.hb_login && cfg.peer_id == 310012301 && !strcmp(cfg.callsign, "N0CALL"));
    uint8_t d[RDOF_DMRC_MAX], r[RDOF_RPTC_LEN];
    const size_t n = rdof_hb_role_dmrc(&cfg, 438500000, 1, d);
    CHECK(rdof_rptc_from_dmrc(d, n, 0, "", "", "", r) == RDOF_RPTC_LEN);
    CHECK(!memcmp(r + 8, "N0CALL  ", 8));
    CHECK(!memcmp(r + 222, "RadioFoundry Gateway hotspot", 28));
}

/* A repeater's login: RPTC with its in and out, slots '3', says repeater; what it hears goes up on its own slot. */
static void test_repeater(void)
{
    static const char json[] =
        "{\"version\": 3, \"callsign\": \"N0CALL\", \"defaults\": {\"cc\": 1},"
        " \"devices\": [{\"name\": \"rpt\", \"kind\": \"modem\", \"type\": \"mmdvm\", \"port\": {\"path\": \"/dev/null\"},"
        " \"hears\": {\"idle\": \"r in\"}, \"transmits\": [\"r out\"]}],"
        " \"frequencies\": [{\"name\": \"r in\", \"freq\": 438.75}, {\"name\": \"r out\", \"freq\": 438.25}],"
        " \"apps\": [{\"name\": \"R1\", \"type\": \"repeater\", \"in\": \"r in\", \"out\": \"r out\"}],"
        " \"networks\": [{\"type\": \"brandmeister\", \"host\": \"bm.example.net\", \"password\": \"example\","
        " \"peer_ids\": {\"R1\": 310012303}}]}";
    static rdof_config_t cfg;
    char error[256] = "";
    bool ok = rdof_config_parse(json, strlen(json), &cfg, error, sizeof error);
    if (!ok)
        printf("config: %s\n", error);
    CHECK(ok);
    if (!ok)
        return;
    CHECK(cfg.hb_login && cfg.mode == RDOF_CFG_REPEATER && cfg.peer_id == 310012303);
    uint8_t d[RDOF_DMRC_MAX], r[RDOF_RPTC_LEN];
    const size_t n = rdof_hb_role_dmrc(&cfg, 0, 1, d);
    CHECK(rdof_rptc_from_dmrc(d, n, 0, "", "", "", r) == RDOF_RPTC_LEN);
    CHECK(rdof_be32(r + 4) == 310012303 && !memcmp(r + 8, "N0CALL  ", 8));
    CHECK(!memcmp(r + 16, "438750000438250000", 18)); /* RX: what it hears; TX: what it sends */
    CHECK(!memcmp(r + 36, "01", 2) && r[97] == '3');
    CHECK(!memcmp(r + 222, "RadioFoundry Gateway repeater ", 30) && !memcmp(r + 262, "MMDVM ", 6));
    /* slot 1 goes up on slot 1, slot 2 on slot 2 */
    rdof_hb_peer_t p;
    rdof_hb_peer_init(&p, cfg.peer_id, 10);
    rdof_hb_peer_set_config(&p, d, n, true, 0);
    uint8_t dmrd[RDOF_DMRD_LEN] = {'D', 'M', 'R', 'D'}, tmp[RDOF_HB_OUT_MAX];
    dmrd[15] = 0x01;
    CHECK(rdof_hb_peer_up(&p, dmrd, sizeof dmrd, tmp) == dmrd);
    dmrd[15] = 0x81;
    CHECK(rdof_hb_peer_up(&p, dmrd, sizeof dmrd, tmp) == dmrd);
}

int main(void)
{
    test_sha256();
    test_callsign();
    test_repeater();
    test_rptc();
    test_login();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
