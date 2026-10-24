// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * tinysctp_peer: an association over a real UDP socket, for interop tests.
 *
 *   tinysctp_peer --host H [--encap-port 9899] [--port 62031] [--count N] [--interval-ms 20] [--size 55]
 *                 [--lifetime-ms 0] [--unordered] [--send-hex HEX --expect-hex PREFIX] [--timeout-ms 30000]
 *   tinysctp_peer --listen --port P --local-udp-port U        (echoes every message back)
 *
 * Echo mode (default): sends `count` numbered messages and expects each one back (the far end echoes).
 * Hex mode: sends HEX every interval until a message starting with PREFIX arrives.
 * Exit 0 on success; the association's status JSON is printed at the end.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinysctp.h"
#include "tinysctp_udp.h"

static tsctp_t *S;
static tsctp_udp_t U;
static bool up, down, listen_mode, matched;
static int count = 200, size = 55, interval = 20, timeout_ms = 30000, lifetime;
static bool unordered;
static uint8_t hex[512], expect[64];
static size_t hex_len, expect_len;
static uint32_t got, out_of_order, next_seq;
static uint64_t lat_sum, lat_max;

static size_t parse_hex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (s[0] && s[1] && n < cap) {
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1)
            break;
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

static void on_message(void *user, const tsctp_msg_t *m)
{
    (void)user;
    if (listen_mode) {
        tsctp_sendinfo_t si = {m->stream, m->ppid, m->unordered, 0};
        tsctp_send(S, m->data, m->len, &si, tsctp_udp_now_ms());
        got++;
        return;
    }
    if (hex_len) {
        printf("got %zu bytes, ppid %u, stream %u: ", m->len, (unsigned)m->ppid, m->stream);
        for (size_t i = 0; i < m->len && i < 64; i++)
            printf("%02x", m->data[i]);
        printf("\n");
        if (m->len >= expect_len && memcmp(m->data, expect, expect_len) == 0)
            matched = true;
        return;
    }
    if (m->len < 12)
        return;
    uint32_t seq = (uint32_t)m->data[0] << 24 | m->data[1] << 16 | m->data[2] << 8 | m->data[3];
    uint64_t sent = 0;
    for (int i = 4; i < 12; i++)
        sent = sent << 8 | m->data[i];
    uint64_t lat = tsctp_udp_now_ms() - sent;
    lat_sum += lat;
    if (lat > lat_max)
        lat_max = lat;
    if (seq < next_seq)
        out_of_order++;
    next_seq = seq + 1;
    got++;
}

static void on_event(void *user, tsctp_event_t ev, tsctp_reason_t why)
{
    (void)user;
    printf("event: %s%s%s\n", ev == TSCTP_EV_UP ? "up" : ev == TSCTP_EV_RESTART ? "restart" : "down",
           ev == TSCTP_EV_DOWN ? ", " : "", ev == TSCTP_EV_DOWN ? tsctp_reason_name(why) : "");
    if (ev == TSCTP_EV_UP || ev == TSCTP_EV_RESTART)
        up = true;
    else
        down = true;
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    int encap = 9899, port = 62031, local_udp = 0, local_port = 0;
    tsctp_config_t cfg;
    tsctp_config_defaults(&cfg);
    cfg.rto_initial_ms = 1000;
    cfg.rto_min_ms = 200;
    cfg.rto_max_ms = 3000;
    cfg.hb_interval_ms = 5000;
    cfg.max_retrans = 8;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "--host")) host = v, i++;
        else if (!strcmp(a, "--encap-port")) encap = atoi(v), i++;
        else if (!strcmp(a, "--port")) port = atoi(v), i++;
        else if (!strcmp(a, "--local-udp-port")) local_udp = atoi(v), i++;
        else if (!strcmp(a, "--local-port")) local_port = atoi(v), i++;
        else if (!strcmp(a, "--count")) count = atoi(v), i++;
        else if (!strcmp(a, "--size")) size = atoi(v), i++;
        else if (!strcmp(a, "--interval-ms")) interval = atoi(v), i++;
        else if (!strcmp(a, "--timeout-ms")) timeout_ms = atoi(v), i++;
        else if (!strcmp(a, "--lifetime-ms")) lifetime = atoi(v), i++;
        else if (!strcmp(a, "--unordered")) unordered = true;
        else if (!strcmp(a, "--listen")) listen_mode = true;
        else if (!strcmp(a, "--send-hex")) hex_len = parse_hex(v, hex, sizeof hex), i++;
        else if (!strcmp(a, "--expect-hex")) expect_len = parse_hex(v, expect, sizeof expect), i++;
        else {
            fprintf(stderr, "unknown option %s\n", a);
            return 2;
        }
    }
    if (size < 12)
        size = 12;
    if (tsctp_udp_open(&U, listen_mode ? NULL : host, (uint16_t)encap, (uint16_t)local_udp) != 0) {
        perror("udp");
        return 2;
    }
    cfg.local_port = (uint16_t)(listen_mode ? port : local_port);
    cfg.peer_port = (uint16_t)(listen_mode ? 0 : port);
    cfg.output = tsctp_udp_output;
    cfg.output_ctx = &U;
    cfg.random = tsctp_udp_random;
    cfg.on_message = on_message;
    cfg.on_event = on_event;
    S = tsctp_new(&cfg, tsctp_udp_now_ms());
    uint64_t start = tsctp_udp_now_ms(), next = start;
    if (listen_mode) {
        tsctp_listen(S);
        printf("listening: SCTP port %d on UDP %d\n", port, local_udp);
        fflush(stdout);
        for (;;)
            tsctp_udp_step(&U, S, 50);
    }
    tsctp_connect(S, start);
    int sent = 0;
    bool ok = false;
    while (tsctp_udp_now_ms() - start < (uint64_t)timeout_ms) {
        tsctp_udp_step(&U, S, 5);
        uint64_t now = tsctp_udp_now_ms();
        if (!up || tsctp_state(S) != TSCTP_ESTABLISHED || now < next)
            continue;
        next = now + (uint64_t)interval;
        tsctp_sendinfo_t si = {0, 0, unordered, (uint32_t)lifetime};
        if (hex_len) {
            tsctp_send(S, hex, hex_len, &si, now);
            if (matched) {
                ok = true;
                break;
            }
            continue;
        }
        if (sent < count) {
            uint8_t m[2048];
            memset(m, 0x5A, sizeof m);
            m[0] = (uint8_t)(sent >> 24), m[1] = (uint8_t)(sent >> 16), m[2] = (uint8_t)(sent >> 8), m[3] = (uint8_t)sent;
            for (int i = 0; i < 8; i++)
                m[4 + i] = (uint8_t)(now >> (56 - 8 * i));
            if (tsctp_send(S, m, (size_t)size, &si, now) == TSCTP_OK)
                sent++;
        } else if (got >= (uint32_t)count || (lifetime && now - start > (uint64_t)timeout_ms / 2)) {
            break;
        }
    }
    if (!hex_len) {
        ok = up && got == (uint32_t)count && out_of_order == 0;
        if (lifetime) /* given up messages are allowed; order and liveness aren't */
            ok = up && got > 0 && out_of_order == 0 && tsctp_state(S) == TSCTP_ESTABLISHED;
        printf("sent %d, echoed %u, out of order %u, latency avg %llu ms, max %llu ms\n", sent, got, out_of_order,
               got ? (unsigned long long)(lat_sum / got) : 0ULL, (unsigned long long)lat_max);
    }
    tsctp_shutdown(S, tsctp_udp_now_ms());
    for (int i = 0; i < 200 && tsctp_state(S) != TSCTP_CLOSED; i++)
        tsctp_udp_step(&U, S, 10);
    char json[1024];
    tsctp_status_json(S, json, sizeof json);
    printf("status: %s\n%s\n", json, ok ? "OK" : "FAILED");
    tsctp_free(S);
    tsctp_udp_close(&U);
    return ok ? 0 : 1;
}
