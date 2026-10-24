// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// Host tests for the LR2021 transmit paths (lr2021_tx.h). The frame bytes are the ones that went on air on the W12
// (docs/spec/lr2021-iq.md section 3); the sample helpers against the spec's scales.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lr2021_tx.h"

static int checks, fails;

#define CHECK(cond, ...)                                \
    do {                                                \
        checks++;                                       \
        if (!(cond)) {                                  \
            fails++;                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

static void frame(const uint8_t *got, size_t n, const uint8_t *want, size_t wn, const char *what)
{
    bool same = n == wn && !memcmp(got, want, n);
    char g[64] = "";
    for (size_t i = 0, o = 0; i < n && o + 4 < sizeof g; i++)
        o += (size_t)snprintf(g + o, sizeof g - o, "%02X ", got[i]);
    CHECK(same, "%s: got %s", what, g);
}
#define WANT(i, ...)                                         \
    do {                                                     \
        static const uint8_t w_[] = {__VA_ARGS__};           \
        frame(f[i], len[i], w_, sizeof w_, "frame " #i);     \
    } while (0)

static lr2021_tx_setup_t base(lr2021_tx_mode_t mode)
{
    lr2021_tx_setup_t s = {0};
    s.mode = mode;
    s.freq_hz = 461350418; /* 461.3375 MHz with the W12's -28 ppm */
    s.standby_first = true;
    s.power_code = lr2021_tx_power_code(16);
    s.ramp_code = 4;
    s.pa_lf_duty = 4;
    s.pa_lf_slices = 2;
    s.fallback = LR2021_FALLBACK_STBY_RC;
    s.rate = 48000;
    s.fdev_hz = 2048;
    s.rx_bw = 0x1C;
    s.n = 4800;
    return s;
}

static void setups(void)
{
    uint8_t f[LR2021_TX_FRAMES_MAX][LR2021_TX_FRAME_MAX];
    size_t len[LR2021_TX_FRAMES_MAX];

    // frequency samples: the common head, then FSK at the sample rate, the 1 KB FIFO and 0x0293 selector 1
    lr2021_tx_setup_t s = base(LR2021_TX_FREQ);
    int n = lr2021_tx_frames(&s, f, len);
    CHECK(n == 13, "FREQ frames %d", n);
    WANT(0, 0x01, 0x28, 0x00);
    WANT(1, 0x02, 0x07, 0x02);
    WANT(2, 0x02, 0x00, 0x1B, 0x7F, 0xA6, 0x12);
    WANT(3, 0x02, 0x02, 0x00, 0x42, 0x10);
    WANT(4, 0x02, 0x03, 0x20, 0x04);
    WANT(5, 0x02, 0x06, 0x01);
    WANT(6, 0x02, 0x40, 0x00, 0x00, 0xBB, 0x80, 0x00, 0x1C, 0x00, 0x08, 0x00);
    WANT(7, 0x02, 0x41, 0x00, 0x00, 0x00, 0x00, 0x12, 0xC0, 0x00);
    WANT(8, 0x02, 0x44, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    WANT(9, 0x01, 0x04, 0xF3, 0x00, 0x2C, 0x00, 0x80, 0x40, 0x00);
    WANT(10, 0x01, 0x04, 0xF3, 0x00, 0x34, 0x00, 0x00, 0x03, 0xFC);
    WANT(11, 0x01, 0x1F);
    WANT(12, 0x02, 0x93, 0x12, 0xC0, 0x00, 0x00, 0xBB, 0x80, 0x01);

    // CalibFE goes after the packet type, before the frequency (and before the FIFO mapping it would overwrite)
    s.calib_fe = true;
    n = lr2021_tx_frames(&s, f, len);
    CHECK(n == 14, "with CalibFE %d", n);
    WANT(2, 0x01, 0x23, 0x00, 0x73, 0x00, 0x00, 0x00, 0x00);

    // polar I/Q: OOK, SetOokModulationParams, no FSK packet or sync word, selector 0
    s = base(LR2021_TX_IQ_POLAR);
    n = lr2021_tx_frames(&s, f, len);
    CHECK(n == 11, "IQ_POLAR frames %d", n);
    WANT(1, 0x02, 0x07, 0x0A);
    WANT(6, 0x02, 0x81, 0x00, 0x00, 0xBB, 0x80, 0x00, 0x1C, 0x00);
    WANT(10, 0x02, 0x93, 0x12, 0xC0, 0x00, 0x00, 0xBB, 0x80, 0x00);

    // phase-only I/Q and phase steps: FSK, selectors 0 and 2
    s = base(LR2021_TX_IQ_PHASE);
    n = lr2021_tx_frames(&s, f, len);
    WANT(n - 1, 0x02, 0x93, 0x12, 0xC0, 0x00, 0x00, 0xBB, 0x80, 0x00);
    s = base(LR2021_TX_PHASE_STEP);
    n = lr2021_tx_frames(&s, f, len);
    WANT(n - 1, 0x02, 0x93, 0x12, 0xC0, 0x00, 0x00, 0xBB, 0x80, 0x02);

    // the chip's 4-FSK: variant 1 before the modulation params, the shape kept, no 0x0293
    s = base(LR2021_TX_FSK4);
    s.rate = 9600;
    s.fdev_hz = lr2021_fsk4_fdev(1944);
    s.shape = 0x05;
    s.n = 33;
    n = lr2021_tx_frames(&s, f, len);
    CHECK(n == 13, "FSK4 frames %d", n);
    WANT(6, 0x02, 0x3B, 0x01);
    WANT(7, 0x02, 0x40, 0x00, 0x00, 0x25, 0x80, 0x05, 0x1C, 0x00, 0x08, 0x1A);
    WANT(8, 0x02, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x00);
    WANT(12, 0x01, 0x1F);

    // a carrier: the common head only, and SetTxTestMode 2 to key up
    s = base(LR2021_TX_CW);
    s.n = 0;
    n = lr2021_tx_frames(&s, f, len);
    CHECK(n == 6, "CW frames %d", n);
    uint8_t b[16];
    size_t bn = lr2021_tx_start_frame(&s, b, 0);
    frame(b, bn, (const uint8_t[]){0x02, 0x0E, 0x02}, 3, "CW start");
    s = base(LR2021_TX_FREQ);
    bn = lr2021_tx_start_frame(&s, b, 64000);
    frame(b, bn, (const uint8_t[]){0x02, 0x0D, 0x00, 0xFA, 0x00}, 5, "SetTx start");

    // refused: no rate, no samples, FREQ with no fdev, an unknown mode
    s = base(LR2021_TX_FREQ);
    s.rate = 0;
    CHECK(lr2021_tx_frames(&s, f, len) == -1, "rate 0 accepted");
    s = base(LR2021_TX_IQ_POLAR);
    s.n = 0;
    CHECK(lr2021_tx_frames(&s, f, len) == -1, "n 0 accepted");
    s = base(LR2021_TX_FREQ);
    s.fdev_hz = 0;
    CHECK(lr2021_tx_frames(&s, f, len) == -1, "fdev 0 accepted");
    s.mode = (lr2021_tx_mode_t)9;
    CHECK(lr2021_tx_frames(&s, f, len) == -1, "mode 9 accepted");

    CHECK(lr2021_tx_sample_bytes(LR2021_TX_IQ_POLAR) == 2 && lr2021_tx_sample_bytes(LR2021_TX_FREQ) == 1 &&
              lr2021_tx_sample_bytes(LR2021_TX_CW) == 0,
          "sample bytes");
}

static void live(void)
{
    // the PLL word: 920 MHz with -28 ppm read back from the chip as 0x39806984 (R3.30)
    CHECK(lr2021_pll_word(lr2021_freq_corrected(920e6, -28)) == 0x39806984, "pll word %08X",
          (unsigned)lr2021_pll_word(lr2021_freq_corrected(920e6, -28)));
    CHECK(lr2021_pll_word(1000000) == 1048576, "1 MHz is 2^20");
    uint8_t b[16];
    size_t n = lr2021_cmd_retune(b, lr2021_freq_corrected(920e6, -28));
    frame(b, n, (const uint8_t[]){0x01, 0x04, 0xF4, 0x01, 0x44, 0x39, 0x80, 0x69, 0x84}, 9, "retune");
    n = lr2021_cmd_set_pa_config(b, true, 7, 7, 31);
    frame(b, n, (const uint8_t[]){0x02, 0x02, 0x80, 0x77, 0x1F}, 5, "PA config HF");
}

static void samples(void)
{
    // phase steps: rate / 128 Hz a count (R3.18), rounded, clamped
    CHECK(lr2021_phase_step_sample(375, 48000) == 1, "375 Hz at 48k");
    CHECK(lr2021_phase_step_sample(-7500, 48000) == -20, "-7.5 kHz at 48k");
    CHECK(lr2021_phase_step_sample(1e6f, 48000) == 127 && lr2021_phase_step_sample(-1e6f, 48000) == -128, "clamp");

    // 4-FSK: the chip's dibits MSB first (00 -1, 01 -3, 10 +1, 11 +3), padding with -1; DMR's are XOR 0xAA
    const int8_t sym[] = {-1, -3, 1, 3, 3, 3};
    uint8_t out[4];
    CHECK(lr2021_fsk4_pack(sym, 6, out) == 2 && out[0] == 0x1B && out[1] == 0xF0, "pack %02X %02X", out[0], out[1]);
    uint8_t dmr[] = {0x00, 0x55, 0xAA, 0xFF}; // DMR +1, +3, -1, -3 runs
    lr2021_fsk4_from_dmr(dmr, 4);
    CHECK(dmr[0] == 0xAA && dmr[1] == 0xFF && dmr[2] == 0x00 && dmr[3] == 0x55, "from dmr");
    CHECK(lr2021_fsk4_fdev(1944) == 2074, "fdev for 1944: %u", (unsigned)lr2021_fsk4_fdev(1944));

    CHECK(fabsf(lr2021_iq_freq_limit(48000) - 14880) < 1 && lr2021_iq_freq_limit(125000) == 20000, "freq limit");

    // phase-only I/Q: magnitude 100, a zero keeps the last phase
    const float pi[] = {0, 5, 0, 0, -3, 0};
    int8_t p[6];
    lr2021_phase_iq8(pi, 3, p);
    CHECK(p[0] == 0 && p[1] == 100 && p[2] == 0 && p[3] == 100 && p[4] == -100 && p[5] == 0, "phase iq");
}

static void polar(void)
{
    enum { N = 400 };
    static float iq[2 * N];
    static int8_t out[2 * N];
    // a two-tone: the envelope goes through zero and the phase jumps 180 degrees there
    for (int i = 0; i < N; i++) {
        iq[2 * i] = cosf(2 * 3.14159265f * i / 100.0f);
        iq[2 * i + 1] = 0;
    }
    // no table: magnitudes as they are (full scale 127), and none under r_min
    lr2021_polar_iq8(iq, N, 0, 0.2f, NULL, out);
    float lo = 1e9f, hi = 0, maxstep = 0;
    for (int i = 0; i < N; i++) {
        float m = hypotf(out[2 * i], out[2 * i + 1]);
        lo = m < lo ? m : lo;
        hi = m > hi ? m : hi;
        if (i) {
            float d = atan2f(out[2 * i + 1], out[2 * i]) - atan2f(out[2 * i - 1], out[2 * i - 2]);
            while (d > 3.14159265f) d -= 2 * 3.14159265f;
            while (d < -3.14159265f) d += 2 * 3.14159265f;
            maxstep = fabsf(d) > maxstep ? fabsf(d) : maxstep;
        }
    }
    CHECK(fabsf(hi - 127) < 1, "peak %.1f", hi);
    CHECK(lo > 0.2f * 127 - 1.5f, "floor %.1f", lo);
    // the 180 degree jump is spread over the dip (about 13 samples under 0.2): well under 90 degrees a step
    CHECK(maxstep < 0.5f, "largest phase step %.2f rad", maxstep);
    // with no r_min the jump stays in one step
    lr2021_polar_iq8(iq, N, 0, 0, NULL, out);
    CHECK(out[2 * 25] == 0 || abs(out[2 * 25]) < 2, "zero crossing kept");

    // the table: full scale maps to the first input at full output (62 on the W12's), less its phase shift (1 deg)
    const float one[] = {1, 0};
    int8_t o[2];
    lr2021_polar_iq8(one, 1, 1, 0, &lr2021_am_w12_920, o);
    CHECK(o[0] == 62 && o[1] == -1, "full scale %d %d", o[0], o[1]);
    // -12.9 dB asks for input 14, its phase shift -36 deg taken off: 14 at +36 deg
    const float q[] = {powf(10, -12.9f / 20), 0};
    lr2021_polar_iq8(q, 1, 1, 0, &lr2021_am_w12_920, o);
    CHECK(o[0] == 11 && o[1] == 8, "-12.9 dB %d %d", o[0], o[1]);
    // under the table's floor: its first input
    const float tiny[] = {1e-3f, 0};
    lr2021_polar_iq8(tiny, 1, 1, 0, &lr2021_am_w12_920, o);
    CHECK(o[0] == 2 && o[1] == 1, "floor %d %d", o[0], o[1]);
}

int main(void)
{
    setups();
    live();
    samples();
    polar();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
