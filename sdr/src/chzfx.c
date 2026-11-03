// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The FPGA channelizer's bit-exact model (chzfx.h): NCO, mixer and decimating FIR per channel, in integers.
 */
#include "rdof/sdr/chzfx.h"

#if RDOF_FEATURE_SDR

#include <math.h>
#include <string.h>

#include "rdof/sdr/channelizer.h"
#include "sdr_private.h"

const int16_t rdof_chzfx_qcos[1024] = {
    32767, 32767, 32767, 32767, 32766, 32766, 32765, 32765, 32764, 32764, 32763, 32762,
    32761, 32760, 32759, 32758, 32757, 32755, 32754, 32752, 32751, 32749, 32747, 32746,
    32744, 32742, 32740, 32738, 32736, 32733, 32731, 32729, 32726, 32724, 32721, 32718,
    32716, 32713, 32710, 32707, 32704, 32701, 32697, 32694, 32691, 32687, 32684, 32680,
    32676, 32673, 32669, 32665, 32661, 32657, 32653, 32648, 32644, 32640, 32635, 32631,
    32626, 32621, 32617, 32612, 32607, 32602, 32597, 32592, 32586, 32581, 32576, 32570,
    32565, 32559, 32553, 32547, 32542, 32536, 32530, 32524, 32517, 32511, 32505, 32499,
    32492, 32486, 32479, 32472, 32466, 32459, 32452, 32445, 32438, 32431, 32423, 32416,
    32409, 32401, 32394, 32386, 32378, 32371, 32363, 32355, 32347, 32339, 32331, 32322,
    32314, 32306, 32297, 32289, 32280, 32272, 32263, 32254, 32245, 32236, 32227, 32218,
    32209, 32200, 32190, 32181, 32171, 32162, 32152, 32142, 32132, 32123, 32113, 32103,
    32092, 32082, 32072, 32062, 32051, 32041, 32030, 32020, 32009, 31998, 31987, 31976,
    31965, 31954, 31943, 31932, 31921, 31909, 31898, 31886, 31875, 31863, 31851, 31839,
    31827, 31815, 31803, 31791, 31779, 31767, 31754, 31742, 31729, 31717, 31704, 31691,
    31679, 31666, 31653, 31640, 31627, 31613, 31600, 31587, 31573, 31560, 31546, 31533,
    31519, 31505, 31491, 31477, 31463, 31449, 31435, 31421, 31407, 31392, 31378, 31363,
    31349, 31334, 31319, 31305, 31290, 31275, 31260, 31245, 31229, 31214, 31199, 31183,
    31168, 31152, 31137, 31121, 31105, 31089, 31074, 31058, 31041, 31025, 31009, 30993,
    30977, 30960, 30944, 30927, 30910, 30894, 30877, 30860, 30843, 30826, 30809, 30792,
    30775, 30757, 30740, 30723, 30705, 30687, 30670, 30652, 30634, 30616, 30598, 30580,
    30562, 30544, 30526, 30508, 30489, 30471, 30452, 30434, 30415, 30396, 30377, 30359,
    30340, 30321, 30302, 30282, 30263, 30244, 30224, 30205, 30185, 30166, 30146, 30126,
    30107, 30087, 30067, 30047, 30027, 30007, 29986, 29966, 29946, 29925, 29905, 29884,
    29864, 29843, 29822, 29801, 29780, 29759, 29738, 29717, 29696, 29675, 29653, 29632,
    29610, 29589, 29567, 29545, 29524, 29502, 29480, 29458, 29436, 29414, 29392, 29369,
    29347, 29325, 29302, 29280, 29257, 29234, 29212, 29189, 29166, 29143, 29120, 29097,
    29074, 29050, 29027, 29004, 28980, 28957, 28933, 28910, 28886, 28862, 28838, 28815,
    28791, 28767, 28742, 28718, 28694, 28670, 28645, 28621, 28596, 28572, 28547, 28523,
    28498, 28473, 28448, 28423, 28398, 28373, 28348, 28322, 28297, 28272, 28246, 28221,
    28195, 28170, 28144, 28118, 28092, 28066, 28040, 28014, 27988, 27962, 27936, 27910,
    27883, 27857, 27830, 27804, 27777, 27750, 27724, 27697, 27670, 27643, 27616, 27589,
    27562, 27534, 27507, 27480, 27452, 27425, 27397, 27370, 27342, 27314, 27287, 27259,
    27231, 27203, 27175, 27147, 27118, 27090, 27062, 27034, 27005, 26977, 26948, 26919,
    26891, 26862, 26833, 26804, 26775, 26746, 26717, 26688, 26659, 26630, 26600, 26571,
    26542, 26512, 26482, 26453, 26423, 26393, 26364, 26334, 26304, 26274, 26244, 26214,
    26183, 26153, 26123, 26092, 26062, 26031, 26001, 25970, 25940, 25909, 25878, 25847,
    25816, 25785, 25754, 25723, 25692, 25661, 25629, 25598, 25567, 25535, 25504, 25472,
    25440, 25409, 25377, 25345, 25313, 25281, 25249, 25217, 25185, 25153, 25121, 25088,
    25056, 25024, 24991, 24959, 24926, 24893, 24861, 24828, 24795, 24762, 24729, 24696,
    24663, 24630, 24597, 24564, 24530, 24497, 24464, 24430, 24397, 24363, 24329, 24296,
    24262, 24228, 24194, 24160, 24126, 24092, 24058, 24024, 23990, 23956, 23921, 23887,
    23852, 23818, 23783, 23749, 23714, 23679, 23645, 23610, 23575, 23540, 23505, 23470,
    23435, 23400, 23364, 23329, 23294, 23258, 23223, 23188, 23152, 23116, 23081, 23045,
    23009, 22973, 22938, 22902, 22866, 22830, 22794, 22757, 22721, 22685, 22649, 22612,
    22576, 22540, 22503, 22466, 22430, 22393, 22356, 22320, 22283, 22246, 22209, 22172,
    22135, 22098, 22061, 22024, 21986, 21949, 21912, 21874, 21837, 21799, 21762, 21724,
    21687, 21649, 21611, 21573, 21535, 21498, 21460, 21422, 21383, 21345, 21307, 21269,
    21231, 21192, 21154, 21116, 21077, 21039, 21000, 20962, 20923, 20884, 20845, 20807,
    20768, 20729, 20690, 20651, 20612, 20573, 20534, 20494, 20455, 20416, 20377, 20337,
    20298, 20258, 20219, 20179, 20139, 20100, 20060, 20020, 19981, 19941, 19901, 19861,
    19821, 19781, 19741, 19700, 19660, 19620, 19580, 19539, 19499, 19459, 19418, 19378,
    19337, 19297, 19256, 19215, 19174, 19134, 19093, 19052, 19011, 18970, 18929, 18888,
    18847, 18806, 18765, 18723, 18682, 18641, 18599, 18558, 18517, 18475, 18434, 18392,
    18350, 18309, 18267, 18225, 18183, 18142, 18100, 18058, 18016, 17974, 17932, 17890,
    17848, 17805, 17763, 17721, 17679, 17636, 17594, 17551, 17509, 17467, 17424, 17381,
    17339, 17296, 17253, 17211, 17168, 17125, 17082, 17039, 16996, 16953, 16910, 16867,
    16824, 16781, 16738, 16694, 16651, 16608, 16565, 16521, 16478, 16434, 16391, 16347,
    16304, 16260, 16216, 16173, 16129, 16085, 16041, 15997, 15954, 15910, 15866, 15822,
    15778, 15734, 15690, 15645, 15601, 15557, 15513, 15468, 15424, 15380, 15335, 15291,
    15246, 15202, 15157, 15113, 15068, 15024, 14979, 14934, 14889, 14845, 14800, 14755,
    14710, 14665, 14620, 14575, 14530, 14485, 14440, 14395, 14350, 14304, 14259, 14214,
    14169, 14123, 14078, 14032, 13987, 13942, 13896, 13850, 13805, 13759, 13714, 13668,
    13622, 13577, 13531, 13485, 13439, 13393, 13347, 13302, 13256, 13210, 13164, 13118,
    13071, 13025, 12979, 12933, 12887, 12841, 12794, 12748, 12702, 12655, 12609, 12563,
    12516, 12470, 12423, 12377, 12330, 12284, 12237, 12190, 12144, 12097, 12050, 12003,
    11957, 11910, 11863, 11816, 11769, 11722, 11675, 11628, 11581, 11534, 11487, 11440,
    11393, 11346, 11299, 11252, 11204, 11157, 11110, 11063, 11015, 10968, 10920, 10873,
    10826, 10778, 10731, 10683, 10636, 10588, 10541, 10493, 10445, 10398, 10350, 10302,
    10255, 10207, 10159, 10111, 10063, 10016,  9968,  9920,  9872,  9824,  9776,  9728,
     9680,  9632,  9584,  9536,  9488,  9440,  9391,  9343,  9295,  9247,  9199,  9150,
     9102,  9054,  9006,  8957,  8909,  8860,  8812,  8764,  8715,  8667,  8618,  8570,
     8521,  8473,  8424,  8375,  8327,  8278,  8230,  8181,  8132,  8084,  8035,  7986,
     7937,  7889,  7840,  7791,  7742,  7693,  7644,  7596,  7547,  7498,  7449,  7400,
     7351,  7302,  7253,  7204,  7155,  7106,  7057,  7008,  6958,  6909,  6860,  6811,
     6762,  6713,  6663,  6614,  6565,  6516,  6466,  6417,  6368,  6319,  6269,  6220,
     6171,  6121,  6072,  6022,  5973,  5924,  5874,  5825,  5775,  5726,  5676,  5627,
     5577,  5528,  5478,  5428,  5379,  5329,  5280,  5230,  5180,  5131,  5081,  5032,
     4982,  4932,  4882,  4833,  4783,  4733,  4684,  4634,  4584,  4534,  4485,  4435,
     4385,  4335,  4285,  4235,  4186,  4136,  4086,  4036,  3986,  3936,  3886,  3836,
     3786,  3737,  3687,  3637,  3587,  3537,  3487,  3437,  3387,  3337,  3287,  3237,
     3187,  3137,  3087,  3037,  2987,  2936,  2886,  2836,  2786,  2736,  2686,  2636,
     2586,  2536,  2486,  2436,  2385,  2335,  2285,  2235,  2185,  2135,  2085,  2034,
     1984,  1934,  1884,  1834,  1783,  1733,  1683,  1633,  1583,  1532,  1482,  1432,
     1382,  1332,  1281,  1231,  1181,  1131,  1080,  1030,   980,   930,   880,   829,
      779,   729,   679,   628,   578,   528,   477,   427,   377,   327,   276,   226,
      176,   126,    75,    25,
};

/* floor(v / 2^s): an arithmetic shift, written so it doesn't depend on how C shifts a negative number */
static int64_t asr(int64_t v, int s)
{
    return v >= 0 ? v >> s : ~(~v >> s);
}

/* round(v / 2^s), half up */
static int64_t round_shift(int64_t v, int s)
{
    return s ? asr(v + ((int64_t)1 << (s - 1)), s) : v;
}

/* a two's complement word as signed, without C's implementation-defined conversion */
static int64_t as_signed(uint64_t v)
{
    return v >> 63 ? -(int64_t)(~v) - 1 : (int64_t)v;
}

static int64_t sat(int64_t v, int bits)
{
    const int64_t hi = ((int64_t)1 << (bits - 1)) - 1, lo = -hi - 1;
    return v > hi ? hi : v < lo ? lo : v;
}

int32_t rdof_chzfx_lo(uint32_t phase)
{
    const unsigned k = phase >> 20, quad = k >> 10, i = k & 1023u;
    switch (quad) {
    case 0:
        return rdof_chzfx_qcos[i];
    case 1:
        return -rdof_chzfx_qcos[1023u - i];
    case 2:
        return -rdof_chzfx_qcos[i];
    default:
        return rdof_chzfx_qcos[1023u - i];
    }
}

uint32_t rdof_chzfx_inc(double offset_hz, double fs)
{
    double t = offset_hz / fs;
    t -= floor(t); /* [0, 1) of a turn */
    return (uint32_t)((uint64_t)llround(t * 4294967296.0) & 0xFFFFFFFFu);
}

int rdof_chzfx_check(const rdof_chzfx_cfg_t *cfg)
{
    if (cfg->decim < 1 || cfg->decim > RDOF_CHZFX_MAX_DECIM || cfg->taps < 1 || cfg->taps > RDOF_CHZFX_MAX_TAPS ||
        cfg->taps + cfg->decim > RDOF_CHZFX_HIST || cfg->channels < 1 || cfg->channels > RDOF_CHZFX_MAX_CH ||
        cfg->out_shift < 1 || cfg->out_shift > 47 || cfg->cic_r < 0 || cfg->cic_r > RDOF_CHZFX_MAX_CIC ||
        cfg->cic_shift < 0 || cfg->cic_shift > RDOF_CHZFX_CIC_N * 6 ||
        (cfg->cic_r > 1 && cfg->cic_r * cfg->decim > RDOF_CHZFX_MAX_STEP))
        return -1;
    for (int m = 0; m < cfg->taps; m++)
        if (cfg->coef[m] != sat(cfg->coef[m], RDOF_CHZFX_COEF_BITS))
            return -1;
    return 0;
}

int rdof_chzfx_design(rdof_chzfx_cfg_t *cfg, double fs, int decim, const double *offsets_hz, int channels,
                      double cutoff_hz)
{
    return rdof_chzfx_design_cic(cfg, fs, 1, decim, offsets_hz, channels, cutoff_hz);
}

int rdof_chzfx_design_cic(rdof_chzfx_cfg_t *cfg, double fs, int cic_r, int decim, const double *offsets_hz,
                          int channels, double cutoff_hz)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->decim = decim;
    cfg->taps = RDOF_CHZ_OUT_FFT / 4 * decim + 1; /* rdof_chz_init's N / 4 + 1 */
    cfg->channels = channels;
    cfg->out_shift = 1;
    cfg->cic_r = cic_r;
    if (fs <= 0.0 || cic_r < 1 || rdof_chzfx_check(cfg) != 0)
        return -1;
    /* the CIC's gain R^N over 2^cic_shift, at most 1 (the mixer's headroom stays) */
    double gain = 1.0;
    if (cic_r > 1) {
        gain = pow(cic_r, RDOF_CHZFX_CIC_N);
        while (gain > 1.0) {
            gain /= 2.0;
            cfg->cic_shift++;
        }
    }
    double h[RDOF_CHZFX_MAX_TAPS], peak = 0.0;
    rdof_chz_lowpass(cfg->taps, cutoff_hz, fs / cic_r, h);
    for (int m = 0; m < cfg->taps; m++) {
        h[m] /= gain;
        peak = fmax(peak, fabs(h[m]));
    }
    /* the largest scale 2^sh that keeps every tap in the coefficient's bits */
    const double top = (double)((1 << (RDOF_CHZFX_COEF_BITS - 1)) - 1);
    int sh = 0;
    while (sh < 40 && peak * ldexp(1.0, sh + 1) <= top)
        sh++;
    for (int m = 0; m < cfg->taps; m++)
        cfg->coef[m] = (int32_t)lround(ldexp(h[m], sh));
    /* the mixer leaves the input times 2^(15 - MIX_SHIFT) (16), the FIR times 2^sh more: keep OUT_FRAC bits */
    cfg->out_shift = sh + (15 - RDOF_CHZFX_MIX_SHIFT) - RDOF_CHZFX_OUT_FRAC;
    for (int c = 0; c < channels; c++)
        cfg->inc[c] = rdof_chzfx_inc(offsets_hz[c], fs);
    return rdof_chzfx_check(cfg);
}

void rdof_chzfx_reset(rdof_chzfx_t *s)
{
    memset(s->hist_i, 0, sizeof s->hist_i);
    memset(s->hist_q, 0, sizeof s->hist_q);
    for (int c = 0; c < RDOF_CHZFX_MAX_CH; c++)
        s->phase[c] = s->cfg.phase0[c];
    s->pending_mask = 0;
    memset(s->integ, 0, sizeof s->integ);
    memset(s->comb, 0, sizeof s->comb);
    s->cic_count = 0;
    s->wr = 0;
    s->count = 0;
    s->seq = 0;
}

rdof_chzfx_t *rdof_chzfx_new(const rdof_chzfx_cfg_t *cfg)
{
    if (rdof_chzfx_check(cfg) != 0)
        return NULL;
    rdof_chzfx_t *s = rdof_sdr_alloc(sizeof *s);
    if (!s)
        return NULL;
    s->cfg = *cfg;
    rdof_chzfx_reset(s);
    return s;
}

void rdof_chzfx_free(rdof_chzfx_t *s)
{
    rdof_sdr_free(s);
}

void rdof_chzfx_set_inc(rdof_chzfx_t *s, int c, uint32_t inc)
{
    if (c < 0 || c >= s->cfg.channels)
        return;
    s->pending[c] = inc;
    s->pending_mask |= 1u << c;
}

int rdof_chzfx_out_len(const rdof_chzfx_t *s, int n_in)
{
    int w = n_in; /* history samples */
    if (s->cfg.cic_r > 1)
        w = n_in <= s->cic_count ? 0 : (n_in - s->cic_count - 1) / s->cfg.cic_r + 1;
    if (w <= s->count)
        return 0;
    return (w - s->count - 1) / s->cfg.decim + 1;
}

/* One channel's mixed sample through the CIC; at a CIC output (out), its sample to *yi, *yq. */
static void cic(rdof_chzfx_t *s, int c, int64_t mi, int64_t mq, int out, int32_t *yi, int32_t *yq)
{
    for (int p = 0; p < 2; p++) {
        uint64_t *in = s->integ[c][p], v = (uint64_t)(p ? mq : mi);
        for (int k = 0; k < RDOF_CHZFX_CIC_N; k++)
            v = in[k] += v;
        if (!out)
            continue;
        uint64_t *cb = s->comb[c][p];
        for (int k = 0; k < RDOF_CHZFX_CIC_N; k++) {
            const uint64_t d = v - cb[k];
            cb[k] = v;
            v = d;
        }
        const int32_t y = (int32_t)sat(round_shift(as_signed(v), s->cfg.cic_shift), RDOF_CHZFX_MIX_BITS);
        *(p ? yq : yi) = y;
    }
}

/* One channel's output: the FIR over its newest taps samples (the newest at wr). */
static rdof_iq16_t fir(const rdof_chzfx_t *s, int c, unsigned wr)
{
    const rdof_chzfx_cfg_t *g = &s->cfg;
    int64_t ai = 0, aq = 0;
    for (int m = 0; m < g->taps; m++) {
        const unsigned p = (wr - (unsigned)m) & (RDOF_CHZFX_HIST - 1);
        ai += (int64_t)g->coef[m] * s->hist_i[c][p];
        aq += (int64_t)g->coef[m] * s->hist_q[c][p];
    }
    rdof_iq16_t y = {(int16_t)sat(round_shift(ai, g->out_shift), 16), (int16_t)sat(round_shift(aq, g->out_shift), 16)};
    return y;
}

int rdof_chzfx_process(rdof_chzfx_t *s, const rdof_iq16_t *in, int n_in, rdof_iq16_t *out)
{
    const rdof_chzfx_cfg_t *g = &s->cfg;
    const int C = g->channels;
    int produced = 0;
    const int R = g->cic_r > 1 ? g->cic_r : 1;
    for (int n = 0; n < n_in; n++) {
        const int64_t xi = in[n].i, xq = in[n].q;
        const unsigned wr = s->wr;
        const int write = R == 1 || s->cic_count == 0; /* a history sample this input */
        for (int c = 0; c < C; c++) {
            const int64_t co = rdof_chzfx_lo(s->phase[c]), si = rdof_chzfx_lo(s->phase[c] - (1u << 30));
            /* x e^{-j theta} = (xi cos + xq sin) + j (xq cos - xi sin) */
            const int64_t mi = sat(round_shift(xi * co + xq * si, RDOF_CHZFX_MIX_SHIFT), RDOF_CHZFX_MIX_BITS);
            const int64_t mq = sat(round_shift(xq * co - xi * si, RDOF_CHZFX_MIX_SHIFT), RDOF_CHZFX_MIX_BITS);
            if (R == 1) {
                s->hist_i[c][wr] = (int32_t)mi;
                s->hist_q[c][wr] = (int32_t)mq;
            } else {
                cic(s, c, mi, mq, write, &s->hist_i[c][wr], &s->hist_q[c][wr]);
            }
            s->phase[c] += g->inc[c];
        }
        if (R > 1)
            s->cic_count = write ? R - 1 : s->cic_count - 1;
        if (!write)
            continue;
        if (s->count == 0) {
            for (int c = 0; c < C; c++)
                out[(size_t)produced * C + c] = fir(s, c, wr);
            produced++;
            s->seq++;
            /* the shadow steps latch here: the next input sample uses them */
            for (int c = 0; c < C; c++)
                if (s->pending_mask & (1u << c))
                    s->cfg.inc[c] = s->pending[c];
            s->pending_mask = 0;
            s->count = g->decim;
        }
        s->count--;
        s->wr = (wr + 1) & (RDOF_CHZFX_HIST - 1);
    }
    return produced;
}

size_t rdof_chzfx_frame(int type, uint32_t seq, const rdof_iq16_t *x, int n, uint64_t *out)
{
    out[0] = (uint64_t)RDOF_CHZFX_MAGIC << 48 | (uint64_t)(type & 0xF) << 44 | (uint64_t)(n & 0xFFF) << 32 | seq;
    const size_t words = rdof_chzfx_frame_words(n);
    for (size_t w = 1; w < words; w++)
        out[w] = 0;
    for (int k = 0; k < n; k++) {
        const uint64_t v = (uint64_t)(uint16_t)x[k].i | (uint64_t)(uint16_t)x[k].q << 16;
        out[1 + k / 2] |= v << (32 * (k % 2));
    }
    return words;
}

int rdof_chzfx_unframe(const uint64_t *w, size_t avail, rdof_chzfx_frame_t *f)
{
    if (avail < 1 || (w[0] >> 48) != RDOF_CHZFX_MAGIC)
        return -1;
    f->type = (int)(w[0] >> 44 & 0xF);
    f->n = (int)(w[0] >> 32 & 0xFFF);
    f->seq = (uint32_t)w[0];
    f->data = w + 1;
    if (f->type != RDOF_CHZFX_FRAME_CH && f->type != RDOF_CHZFX_FRAME_RAW)
        return -1;
    const size_t words = rdof_chzfx_frame_words(f->n);
    return words <= avail ? (int)words : 0;
}

rdof_iq16_t rdof_chzfx_frame_sample(const rdof_chzfx_frame_t *f, int k)
{
    const uint32_t v = (uint32_t)(f->data[k / 2] >> (32 * (k % 2)));
    rdof_iq16_t x = {(int16_t)(uint16_t)v, (int16_t)(uint16_t)(v >> 16)};
    return x;
}

#endif
