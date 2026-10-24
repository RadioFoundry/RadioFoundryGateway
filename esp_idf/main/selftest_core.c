// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The self-test's output check (see selftest_core.h). */
#include "selftest_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/sdr/iq.h"
#include "rdof/sdr/receiver.h"
#include "selftest/clip.h"

uint32_t sdr_crc32(uint32_t crc, const void *data, size_t n)
{
    const uint8_t *p = data;
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* A CRC-32 and a count per channel over its messages in order: two cores interleave channels, never one
 * channel's own, and each channel's are only touched from the core demodulating it. */
static uint32_t chan_crc[CLIP_CHANNELS];
static int chan_messages[CLIP_CHANNELS];

static void add(sdr_check_t *c, const char *ch, const char *kind, const char *text)
{
    int k = 0;
    while (k < CLIP_CHANNELS - 1 && strcmp(clip_channels[k].id, ch))
        k++;
    uint32_t crc = chan_crc[k];
    crc = sdr_crc32(crc, ch, strlen(ch));
    crc = sdr_crc32(crc, " ", 1);
    crc = sdr_crc32(crc, kind, strlen(kind));
    crc = sdr_crc32(crc, " ", 1);
    crc = sdr_crc32(crc, text, strlen(text));
    chan_crc[k] = sdr_crc32(crc, "\n", 1);
    chan_messages[k]++;
}

static void on_json(void *ctx, const char *ch, const char *json) { add(ctx, ch, "json", json); }
static void on_raw(void *ctx, const char *ch, const char *json) { add(ctx, ch, "raw", json); }

void (*sdr_selftest_step)(rdof_sdr_rx_t *rx, const rdof_cf_t *iq, int n, int split);

static inline uint8_t clamp_byte(int32_t v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* Fault v put into the clip (docs/spec/fw/device.md), in integer steps, bit-exact with the generator. */
void sdr_clip_impair(int v, const uint8_t *in, size_t n, size_t at, uint32_t *noise, uint8_t *out)
{
    const char *kind = clip_variants[v].name;
    if (!strcmp(kind, "noise")) {
        uint32_t x = *noise;
        for (size_t j = 0; j < n; j++) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            int32_t r = (int32_t)(x & 0xFF) + (int32_t)((x >> 8) & 0xFF) + (int32_t)((x >> 16) & 0xFF) +
                        (int32_t)(x >> 24) - 510;
            out[j] = clamp_byte((int32_t)in[j] + ((r * CLIP_NOISE_GAIN) >> 8));
        }
        *noise = x;
        return;
    }
    for (size_t j = 0; j + 1 < n; j += 2) {
        int32_t i = 2 * (int32_t)in[j] - 255, q = 2 * (int32_t)in[j + 1] - 255;
        if (!strcmp(kind, "offset")) {
            const short *r = clip_rot[((at + j) / 2) % CLIP_ROT_PERIOD];
            int32_t ri = i * r[0] - q * r[1] + 255 * 16384 + 16384, rq = i * r[1] + q * r[0] + 255 * 16384 + 16384;
            out[j] = clamp_byte(ri < 0 ? 0 : ri >> 15);
            out[j + 1] = clamp_byte(rq < 0 ? 0 : rq >> 15);
        } else { /* hot */
            out[j] = clamp_byte((4 * i + 256) >> 1);
            out[j + 1] = clamp_byte((4 * q + 256) >> 1);
        }
    }
}

static bool check(const uint8_t *clip, size_t len, int split, int v, sdr_check_t *out)
{
    rdof_sdr_rx_t *rxp = calloc(1, sizeof *rxp);
    rdof_cf_t *iq = malloc(sizeof(rdof_cf_t) * CLIP_CHUNK);
    uint8_t *bad = v >= 0 ? malloc(2 * CLIP_CHUNK) : NULL;
    rdof_sdr_channel_t ch[CLIP_CHANNELS];
    if (!rxp || !iq || (v >= 0 && !bad)) {
        free(rxp);
        free(iq);
        free(bad);
        return false;
    }
#define rx (*rxp)
    memset(out, 0, sizeof *out);
    memset(chan_crc, 0, sizeof chan_crc);
    memset(chan_messages, 0, sizeof chan_messages);
    out->name = v >= 0 ? clip_variants[v].name : "clean";
    out->expect_messages = v >= 0 ? clip_variants[v].messages : CLIP_EXPECT_MESSAGES;
    out->expect_crc = v >= 0 ? clip_variants[v].crc : CLIP_EXPECT_CRC;
    out->expect_bursts = v >= 0 ? clip_variants[v].bursts : CLIP_EXPECT_BURSTS;
    uint32_t noise = CLIP_NOISE_SEED;
    for (int c = 0; c < CLIP_CHANNELS; c++) {
        ch[c].id = clip_channels[c].id;
        ch[c].freq_hz = clip_channels[c].freq;
        ch[c].cc = clip_channels[c].cc;
        ch[c].slot = clip_channels[c].slot;
    }
    rdof_sdr_rx_config_t cfg = {CLIP_RATE, CLIP_CENTER, CLIP_RSSI_OFFSET, 0.0, true, CLIP_CHUNK};
    rdof_sink_t sink = {.on_json = on_json, .on_raw = on_raw, .ctx = out};
    if (rdof_sdr_rx_init(&rx, &cfg, ch, CLIP_CHANNELS, &sink, CLIP_T0)) {
        rdof_sdr_rx_free(&rx);
        free(rxp);
        free(iq);
        free(bad);
        return false;
    }
    const int n = (int)(len / 2);
    for (int i = 0; i < n; i += CLIP_CHUNK) {
        int m = n - i < CLIP_CHUNK ? n - i : CLIP_CHUNK;
        const uint8_t *src = clip + 2 * (size_t)i;
        if (v >= 0) {
            sdr_clip_impair(v, src, 2 * (size_t)m, 2 * (size_t)i, &noise, bad);
            src = bad;
        }
        rdof_iq_from_cu8(src, (size_t)m, iq);
        if (sdr_selftest_step) { /* the firmware's own step, one core or two (it profiles them) */
            sdr_selftest_step(&rx, iq, m, split);
        } else if (split > 0) {
            rdof_sdr_rx_forward(&rx, iq, m); /* split by channel end to end, as two cores run it */
            rdof_sdr_rx_channels(&rx, split, CLIP_CHANNELS, 1); /* the second core's share first: order mustn't matter */
            rdof_sdr_rx_channels(&rx, 0, split, 0);
            rdof_sdr_rx_finish(&rx);
        } else {
            rdof_sdr_rx_process(&rx, iq, m);
        }
    }
    for (int c = 0; c < CLIP_CHANNELS; c++) {
        out->bursts += rx.bursts[c];
        out->messages += chan_messages[c];
        uint8_t be[4] = {(uint8_t)(chan_crc[c] >> 24), (uint8_t)(chan_crc[c] >> 16), (uint8_t)(chan_crc[c] >> 8),
                         (uint8_t)chan_crc[c]};
        out->crc = sdr_crc32(out->crc, be, 4);
    }
    rdof_sdr_rx_free(&rx);
#undef rx
    free(rxp);
    free(iq);
    free(bad);
    out->pass = out->messages == out->expect_messages && out->bursts == out->expect_bursts &&
                (v >= 0 || out->crc == out->expect_crc);
    return out->pass;
}

bool sdr_selftest_check(const uint8_t *clip, size_t len, int split, sdr_check_t *out)
{
    return check(clip, len, split, -1, out);
}

bool sdr_selftest_check_variant(const uint8_t *clip, size_t len, int v, sdr_check_t *out)
{
    return check(clip, len, 0, v, out);
}
