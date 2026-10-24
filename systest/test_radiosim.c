// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The gateway's modem client (rdof_modem, rdof_radio) against a RadioFoundry
 * radio: RadioFoundry's own hotspot core in radiosim, over the MMDVM serial
 * protocol, in virtual time.
 *
 *   test_radiosim <path to radiosim>
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "radiolink.h"
#include "rdof/layer2.h"
#include "rdof/modem.h"
#include "rdof/mmdvm.h"
#include "rdof/pdu.h"

static int failures;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c);                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

/* sim_air_t / dmr_burst_t kinds (RadioFoundry hal_dmr_modem.h) */
enum { KIND_VOICE_LC_HEADER = 0x1, KIND_TERMINATOR_LC = 0x2, KIND_VOICE = 0x10 };
enum { SYNC_MS = 1 };

#define MAX_AIR 64
#define MAX_RX 64

typedef struct {
    radiolink_t link;
    rdof_modem_t modem;
    radiolink_air_t air[MAX_AIR];
    int air_n;
    struct {
        int slot;
        uint8_t control;
        uint8_t burst[RDOF_BURST_BYTES];
    } rx[MAX_RX];
    int rx_n, lost_n;
    char events[16][48];
    int events_n;
} world_t;

static world_t w;

/* ── the port the modem sees: radiosim's serial port, the clock radiosim's ── */

static void on_frame(void *ctx, uint32_t ms, const uint8_t *f, size_t n)
{
    (void)ms;
    rdof_modem_feed(&((world_t *)ctx)->modem, f, n);
}

static void on_air(void *ctx, const radiolink_air_t *a)
{
    world_t *x = ctx;
    if (x->air_n < MAX_AIR)
        x->air[x->air_n++] = *a;
}

static void on_event(void *ctx, uint32_t ms, const char *what)
{
    world_t *x = ctx;
    (void)ms;
    if (x->events_n < 16)
        snprintf(x->events[x->events_n++], sizeof x->events[0], "%s", what);
}

static bool port_write(void *ctx, const uint8_t *data, size_t len)
{
    return radiolink_host(&((world_t *)ctx)->link, data, len);
}

static bool port_wait(void *ctx, int timeout_ms)
{
    world_t *x = ctx;
    for (int t = 0; t < timeout_ms && !x->modem.reply_ready; t += 5)
        if (!radiolink_advance(&x->link, 5))
            return false;
    return x->modem.reply_ready;
}

static double port_clock(void *ctx) { return ((world_t *)ctx)->link.now / 1000.0; }

static void port_sleep(void *ctx, int ms) { radiolink_advance(&((world_t *)ctx)->link, (uint32_t)ms); }

static void on_rx(void *ctx, int slot, uint8_t control, const uint8_t *burst, int rssi)
{
    world_t *x = ctx;
    (void)rssi;
    if (x->rx_n < MAX_RX) {
        x->rx[x->rx_n].slot = slot;
        x->rx[x->rx_n].control = control;
        memcpy(x->rx[x->rx_n].burst, burst, RDOF_BURST_BYTES);
        x->rx_n++;
    }
}

static void on_lost(void *ctx, int slot)
{
    (void)slot;
    ((world_t *)ctx)->lost_n++;
}

static bool start(const char *radiosim, const char *description)
{
    memset(&w, 0, sizeof w);
    w.link.on_frame = on_frame;
    w.link.on_air = on_air;
    w.link.on_event = on_event;
    w.link.ctx = &w;
    if (!radiolink_start(&w.link, radiosim, description))
        return false;
    rdof_modem_port_t port = {.write = port_write, .wait_reply = port_wait, .clock = port_clock,
                              .sleep_ms = port_sleep, .ctx = &w};
    rdof_modem_init(&w.modem, &port);
    w.modem.on_rx = on_rx;
    w.modem.on_lost = on_lost;
    w.modem.cb_ctx = &w;
    return true;
}

static bool saw_event(const char *prefix)
{
    for (int i = 0; i < w.events_n; i++)
        if (!strncmp(w.events[i], prefix, strlen(prefix)))
            return true;
    return false;
}

static void put_hex(char *out, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        snprintf(out + 2 * i, 3, "%02X", b[i]);
    if (!n)
        strcpy(out, "-");
}

/* A burst heard on the air: radiosim's "r" request. */
static void hear(int kind, int index, int sync, const uint8_t *payload, size_t len, const uint8_t *lc9)
{
    char line[256], ph[64], lh[32];
    put_hex(ph, payload, len);
    if (lc9) {
        put_hex(lh, lc9, 9);
        snprintf(line, sizeof line, "r 1 %d %d %d 1 %s %s", kind, index, sync, ph, lh);
    } else {
        snprintf(line, sizeof line, "r 1 %d %d %d 1 %s", kind, index, sync, ph);
    }
    CHECK(radiolink_request(&w.link, line));
}

/* ── the tests ─────────────────────────────────────────────────────────────── */

/* GET_VERSION: the gateway knows a RadioFoundry radio as a radio hotspot. */
static void test_detect(const char *radiosim)
{
    CHECK(start(radiosim, NULL));
    CHECK(rdof_modem_start(&w.modem, 3));
    CHECK(strstr(w.modem.version, "RadioFoundry") != NULL);
    CHECK(rdof_radio_detect(w.modem.version) == RDOF_RADIO_HOTSPOT);
    CHECK(rdof_is_radio_hotspot(w.modem.version));
    printf("  detect: \"%s\", protocol %d, %s\n", w.modem.version, w.modem.protocol,
           rdof_radio_kind_name(rdof_radio_detect(w.modem.version)));
    CHECK(w.modem.protocol == 1 || w.modem.protocol == 2);
    radiolink_stop(&w.link);
}

/* Tuned as the roamer tunes it: SET_FREQ, SET_CONFIG, SET_MODE, then a status. The radio core is asked in. */
static void tune(const char *radiosim, uint32_t freq)
{
    rdof_radio_t radio;
    rdof_mmdvm_status_t st;
    CHECK(start(radiosim, NULL));
    CHECK(rdof_modem_start(&w.modem, 3));
    rdof_radio_init(&radio, &w.modem, rdof_radio_detect(w.modem.version));
    CHECK(rdof_radio_tune(&radio, freq, 1, 100));
    if (w.modem.error[0])
        fprintf(stderr, "  modem: %s\n", w.modem.error);
    CHECK(rdof_modem_status(&w.modem, &st));
    CHECK(!st.transmitting);
    radiolink_advance(&w.link, 100);
    CHECK(saw_event("enter"));
    char want[48];
    snprintf(want, sizeof want, "freq %u %u", (unsigned)freq, (unsigned)freq);
    CHECK(saw_event(want));
}

static void test_tune(const char *radiosim)
{
    tune(radiosim, 461022500);
    radiolink_stop(&w.link);
}

/* A call down: header, a superframe of voice, terminator, as DMR_DATA2. It goes out on air whole, in order. */
static void test_call_down(const char *radiosim)
{
    uint8_t lc[9], b[RDOF_BURST_BYTES];
    tune(radiosim, 461022500);
    rdof_group_lc(9990003, 9, true, 0, lc);
    int air0 = w.air_n;

    rdof_lc_burst(lc, RDOF_DT_VOICE_LC_HEADER, 1, RDOF_BS_VOICE ^ RDOF_DATA_XOR, b);
    CHECK(rdof_modem_write_dmr(&w.modem, RDOF_SYNC_DATA | RDOF_DT_VOICE_LC_HEADER, b, 2));
    radiolink_advance(&w.link, 60);
    for (int sf = 0; sf < 2; sf++)
        for (int n = 0; n < 6; n++) {
            uint8_t control = rdof_silence_burst(n, 1, true, NULL, b);
            CHECK(rdof_modem_write_dmr(&w.modem, control, b, 2));
            radiolink_advance(&w.link, 60);
        }
    rdof_lc_burst(lc, RDOF_DT_TERMINATOR_WITH_LC, 1, RDOF_BS_VOICE ^ RDOF_DATA_XOR, b);
    CHECK(rdof_modem_write_dmr(&w.modem, RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC, b, 2));
    radiolink_advance(&w.link, 1500);

    int headers = 0, voice = 0, terms = 0, order_ok = 1, phase = 0, next_index = 0;
    for (int i = air0; i < w.air_n; i++) {
        const radiolink_air_t *a = &w.air[i];
        if (a->kind == KIND_VOICE_LC_HEADER) {
            headers++;
            order_ok &= phase == 0;
            CHECK(a->len >= 9 && !memcmp(a->payload, lc, 9));
        } else if (a->kind == KIND_VOICE) {
            voice++;
            order_ok &= phase <= 1 && a->index == next_index;
            next_index = (next_index + 1) % 6;
            phase = 1;
        } else if (a->kind == KIND_TERMINATOR_LC) {
            terms++;
            order_ok &= phase == 1;
            phase = 2;
            CHECK(a->len >= 9 && !memcmp(a->payload, lc, 9));
        }
    }
    printf("  call down: %d headers, %d voice, %d terminators, order %s (%d on air)\n", headers, voice,
                terms, order_ok ? "ok" : "wrong", w.air_n - air0);
    CHECK(headers >= 1);
    CHECK(voice == 12);
    CHECK(terms >= 1);
    CHECK(order_ok);
    radiolink_stop(&w.link);
}

/* A call up: a subscriber keys up; the gateway gets the header, the voice and the terminator as DMR_DATA2. */
static void test_call_up(const char *radiosim)
{
    uint8_t lc[9], lc12[12], ambe[27];
    tune(radiosim, 461022500);
    rdof_group_lc(9990004, 91, true, 0, lc);
    rdof_voice_lc12(lc, RDOF_DT_VOICE_LC_HEADER, lc12);

    hear(KIND_VOICE_LC_HEADER, 0, SYNC_MS, lc12, sizeof lc12, lc);
    radiolink_advance(&w.link, 60);
    for (int n = 0; n < 6; n++) {
        for (int i = 0; i < 27; i++)
            ambe[i] = (uint8_t)(0x40 + n * 27 + i);
        hear(KIND_VOICE, n, n == 0 ? SYNC_MS : 0, ambe, sizeof ambe, n == 5 ? lc : NULL);
        radiolink_advance(&w.link, 60);
    }
    rdof_voice_lc12(lc, RDOF_DT_TERMINATOR_WITH_LC, lc12);
    hear(KIND_TERMINATOR_LC, 0, SYNC_MS, lc12, sizeof lc12, lc);
    radiolink_advance(&w.link, 1000);

    int headers = 0, voice = 0, terms = 0;
    for (int i = 0; i < w.rx_n; i++) {
        int dt, n;
        uint8_t got[12];
        rdof_control_to_dt(w.rx[i].control, &dt, &n);
        CHECK(w.rx[i].slot == 2);
        if ((w.rx[i].control & RDOF_SYNC_DATA) && dt == RDOF_DT_VOICE_LC_HEADER) {
            headers++;
            CHECK(rdof_bptc196_decode(w.rx[i].burst, got) && !memcmp(got, lc, 9));
        } else if ((w.rx[i].control & RDOF_SYNC_DATA) && dt == RDOF_DT_TERMINATOR_WITH_LC) {
            terms++;
            CHECK(rdof_bptc196_decode(w.rx[i].burst, got) && !memcmp(got, lc, 9));
        } else if (!(w.rx[i].control & RDOF_SYNC_DATA)) {
            voice++;
        }
    }
    printf("  call up: %d headers, %d voice, %d terminators (%d frames)\n", headers, voice, terms,
                w.rx_n);
    CHECK(headers >= 1);
    CHECK(voice == 6);
    CHECK(terms == 1);
    radiolink_stop(&w.link);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <radiosim>\n", argv[0]);
        return 2;
    }
    test_detect(argv[1]);
    test_tune(argv[1]);
    test_call_down(argv[1]);
    test_call_up(argv[1]);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("radiosim: all passed\n");
    return 0;
}
