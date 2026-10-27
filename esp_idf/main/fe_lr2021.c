// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The LR2021 front end (device.type lr2021, CONFIG_RDOF_SDR_LR2021): a Semtech
 * LR2021 on SPI (components/lr2021). Its receiver's raw I/Q comes through
 * ROM 1.24's undocumented route into the 1 KB RX FIFO (docs/spec/lr2021-iq.md):
 * the component's session (lr2021_iq.h) sets it up and reads it, from a task on
 * core 0 above Wi-Fi, into a PSRAM ring; read() resamples it to the settings'
 * rate (the chip's is 125 or 250 kS/s, not a multiple of 48 kS/s).
 *
 * The W12's sub-GHz front end (a GC1109) has an LNA, a PA for about 860 to 930
 * MHz and a transmit bypass; the DIOs pick the PA inside that band, the bypass
 * outside it. A 902-928 MHz SAW filter follows its LNA (schematic W12-MB-V0.2),
 * so 461 MHz reaches the LR2021 about 46 dB down: strong signals only.
 *
 * First run on the W12 (2026-10-23): a bench MMDVM's DMR call on 461.0875 MHz decoded 131 of 131 bursts, BER
 * 0.5%. That board's crystal ran 28 ppm slow (device.ppm -28). The chip's filter passes the LoRa bandwidth only:
 * one DMR channel a tune at 125 kS/s (band_usable). Samples come out a few counts of 16 bits (lr2021-iq.md R2.17).
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lr2021.h"
#include "lr2021_fsk.h"
#include "lr2021_hal_esp_idf.h"
#include "lr2021_iq.h"
#include "lr2021_lora.h"
#include "lr2021_patch.h"
#include "lr2021_radio.h"
#include "lr2021_tx.h"
#include "mem.h"
#include "rdof/sdr/resample.h"
#include "sdr_fe.h"

#if CONFIG_RDOF_SDR_LR2021

static const char *TAG = "lr2021";

/* iq_bits 16: 4 bytes a sample. 240 kS/s from the chip's 250 k is 1 MB/s. 288 k (333.3 k, 1.33 MB/s) lost samples
 * about 70 times a second at 16 MHz SPI (reads 76% of the time, control 17%) and 0.4 at 20 MHz (out of spec) */
#define WIDE16_MAX 240000
#define READ_MAX 1024 /* bytes a FIFO read: the whole FIFO in one frame, by DMA */
#define FIFO_BYTES 1024
#define IRQ_DIO 7 /* the W12's DIO7, wired to GPIO7 (CONFIG_RDOF_LR2021_IRQ) */
#define RING_KB CONFIG_RDOF_SDR_RING_KB
/* The wide mode's fastest rate: 2 bytes a sample at 576 kS/s is 9.2 Mbit/s of the SPI's 16. At 768 kS/s (12.3
 * Mbit/s) the FIFO overflowed about 30 times a second at 16 MHz; at 26.67 MHz the reader polled without a pause and
 * starved core 0 (the web server stopped answering). */
#define WIDE_MAX 576000
#define BLOCK 512 /* samples read() converts and resamples at a time */

static double gains[13];

static rdof_fe_caps_t caps = {
    .kind = "LR2021",
    .format = RDOF_FE_CS16,
    .rate_min = 48000,
    .rate_max = WIDE_MAX, /* the narrow mode resampled to it, the wide mode made by the chip (pick) */
    .freq_min = 150e6, /* the LF path, RFI_LF (datasheet 1.1) */
    .freq_max = 960e6,
    .usable = 0.8, /* set from the rate in init (band_usable) */
    .dc_guard_hz = 5000, /* the raw samples' DC hasn't been measured: keep channels off the center */
    .gain = true,
    .duplex = RDOF_FE_HALF_DUPLEX, /* one synthesizer: the receiver stops for each run (tx_service) */
    .tx_mode = RDOF_FE_TX_FREQ,
    .tx_rate = 48000,
    .tx_freq_min = 150e6,
    .tx_freq_max = 960e6,
    .tx_power_min = -9.5, /* the LF PA, 0.5 dB steps (7.4.3) */
    .tx_power_max = 22,
    .tx_power_step = 0.5,
    .turnaround_us = 107, /* RX to TX, typical (table 3-23); TX to RX 91 */
};

/* The session (the component's): in PSRAM, as the reader's buffer was (internal RAM is the DSP's) */
EXT_RAM_BSS_ATTR static lr2021_iq_t iq;

static struct {
    lr2021_t *dev;
    lr2021_hal_t hal;     /* the HAL lr2021_open took (for its bounce count) */
    lr2021_board_t board; /* the pins and front end, from the Kconfig */
    bool iq_ok;           /* lr2021_iq_init took the chip (its ROM known, no patch running) */
    int clock_hz;
    char error[128];
    char id[32];
    double rate, center, native;
    int ppm;
    double gain;          /* a step, or NAN for the AGC */
    /* The mode (pick): narrow, the LoRa modem at 125 kS/s and int16 I/Q (the first one proven, spec R2.8);
     * wide, the FSK modem, its rate converted by the chip to the settings' rate, int8 I/Q (spec R2.19 to R2.21), or
     * with iq_bits 16 at the FSK code's own rate, int16. What comes out of the FIFO is at native_num / native_den S/s. */
    bool wide, iq16;
    lr2021_iq_plan_t plan;
    lr2021_iq_cfg_t cfg;  /* what the next session starts with */
    int sbytes;           /* bytes a sample on the FIFO and in the ring */
    uint32_t native_num, native_den;

    SemaphoreHandle_t spi; /* one user of the chip at a time: the reader, or a control call */
    volatile bool streaming, gone;
    volatile bool held; /* the chip lent out for experiments (fe_lr2021_hold): the reader leaves it alone */
    TaskHandle_t reader;
    StreamBufferHandle_t ring;
    StaticStreamBuffer_t ring_struct;
    uint8_t *ring_storage;
    rdof_resamp_t rs;
    bool resample;
    struct {
        uint8_t *raw;     /* BLOCK samples as they came */
        rdof_cf_t *x, *y; /* converted, resampled */
    } blk;                /* read()'s, from start(): internal RAM once the DSP has its own, else PSRAM */
    portMUX_TYPE lock;
    rdof_fe_stats_t stats;
} st = {.lock = portMUX_INITIALIZER_UNLOCKED};

/* The memory plan's "sdr_fe" (mem.c): the driver's handle with its 1 KB SPI frames (DMA-capable, aligned to 4: see
 * lr2021.h), the reader's stack, the ring and read()'s blocks (PSRAM), and what the SPI bus takes of DMA memory for
 * itself (its state and descriptors). */
void fe_lr2021_mem_claims(void)
{
    mem_add("sdr_fe", "lr2021_dev", lr2021_handle_size(), 4, RDOF_MEM_DMA, RDOF_MEM_ALLOC);
    mem_add_task("sdr_fe", "lr2021", 4096, RDOF_MEM_INTERNAL);
    mem_add("sdr_fe", "spi_bus", 1536, 4, RDOF_MEM_DMA, RDOF_MEM_BUDGET);
    mem_add("sdr_fe", "ring", (size_t)RING_KB * 1024 + 1, 4, RDOF_MEM_PSRAM, RDOF_MEM_ALLOC);
    mem_add("sdr_fe", "blocks", BLOCK * 4 + sizeof(rdof_cf_t) * (2 * BLOCK + 4), 16, RDOF_MEM_PSRAM, RDOF_MEM_ALLOC);
}

static int failed(const char *what, esp_err_t e)
{
    snprintf(st.error, sizeof st.error, "LR2021 %s: %s", what,
             st.dev && *lr2021_last_error(st.dev) ? lr2021_last_error(st.dev) : esp_err_to_name(e));
    return -1;
}

/* The session's own error (iq.error), as st.error. */
static int iq_failed(void)
{
    snprintf(st.error, sizeof st.error, "LR2021 %.120s", iq.error);
    return -1;
}

static esp_err_t cmd(const uint8_t *f, size_t n) { return lr2021_write(st.dev, f, n); }

/* wanting counts the tasks waiting for the lock: the reader, at a wide rate always busy and above them, gives
 * them a tick (the mutex alone would go straight back to it, and the web server stalled behind a hold) */
static volatile int wanting;
static void lock(void)
{
    __atomic_add_fetch(&wanting, 1, __ATOMIC_RELAXED);
    xSemaphoreTake(st.spi, portMAX_DELAY);
    __atomic_sub_fetch(&wanting, 1, __ATOMIC_RELAXED);
}
static void unlock(void) { xSemaphoreGive(st.spi); }

/* ── log lines, from esp_timer's task ───────────────────────────────────── */

/* A log line in the reader held it past the FIFO (2 FIFO losses in 3 runs after one ESP_LOGI): the reader leaves
 * its lines here, and a one-shot timer prints them from esp_timer's task. */
static char said[3][200];
static volatile unsigned said_put, said_got;
static esp_timer_handle_t said_timer;

static void said_print(void *arg)
{
    while (said_got != said_put) {
        ESP_LOGI(TAG, "%s", said[said_got % 3]);
        said_got++;
    }
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    if (said_put - said_got >= 3)
        return; /* the timer hasn't run yet: drop it */
    va_list a;
    va_start(a, fmt);
    vsnprintf(said[said_put % 3], sizeof said[0], fmt, a);
    va_end(a);
    said_put++;
    if (said_timer && !esp_timer_is_active(said_timer))
        esp_timer_start_once(said_timer, 1000);
}

/* ── setup ──────────────────────────────────────────────────────────────── */

/* The front end's transmit path for a frequency: its PA inside its band, else its transmit bypass. It receives
 * through its LNA either way (the GC1109 has no receive bypass). */
static bool pa_band(double hz)
{
    return hz >= CONFIG_RDOF_LR2021_PA_LO_MHZ * 1e6 && hz <= CONFIG_RDOF_LR2021_PA_HI_MHZ * 1e6;
}

/* The board as the component takes it (lr2021_board.h), from the Kconfig: DIO9 to DIO11 drive the W12's GC1109
 * (CTX, CPS, CSD), high in the modes each mask names. Receive is CSD high, CTX low; transmit CTX high, CPS high for
 * the PA (its datasheet's table 4; MeshCore drives the W12 the same: standby 0/0/0, receive 0/0/1, transmit 1/1/1).
 * The defaults are lr2021_board_w12. */
static void board_setup(void)
{
    lr2021_board_t *b = &st.board;
    *b = lr2021_board_generic;
    b->name = "kconfig";
    b->tcxo_tune = CONFIG_RDOF_LR2021_TCXO_TUNE;
    b->tcxo_start_us = 5000;
    static const uint8_t pa[3] = {CONFIG_RDOF_LR2021_DIO9_PA, CONFIG_RDOF_LR2021_DIO10_PA, CONFIG_RDOF_LR2021_DIO11_PA};
    static const uint8_t bypass[3] = {CONFIG_RDOF_LR2021_DIO9_BYPASS, CONFIG_RDOF_LR2021_DIO10_BYPASS,
                                      CONFIG_RDOF_LR2021_DIO11_BYPASS};
    int n = 0;
    for (int i = 0; i < 3; i++) {
        if (pa[i] == 0xFF)
            continue; /* not used */
        b->rfsw_dio[n] = (uint8_t)(9 + i);
        b->rfsw_pa[n] = pa[i];
        b->rfsw_bypass[n] = bypass[i] == 0xFF ? 0 : bypass[i];
        n++;
    }
    b->rfsw_n = n;
    b->pa_lo_hz = CONFIG_RDOF_LR2021_PA_LO_MHZ * 1e6;
    b->pa_hi_hz = CONFIG_RDOF_LR2021_PA_HI_MHZ * 1e6;
    b->irq_dio = CONFIG_RDOF_LR2021_IRQ >= 0 ? IRQ_DIO : -1;
    b->rx_boost = CONFIG_RDOF_LR2021_RX_BOOST;
}

/* The RF switch for a frequency, for the experiments (the session sets its own). */
static void rf_switch(double hz)
{
    uint8_t dio[LR2021_BOARD_RFSW_MAX], modes[LR2021_BOARD_RFSW_MAX], f[12];
    const int n = lr2021_board_rfsw(&st.board, hz, dio, modes);
    for (int i = 0; i < n; i++) {
        cmd(f, lr2021_cmd_set_dio_function(f, dio[i], LR2021_DIO_RF_SWITCH, 0));
        cmd(f, lr2021_cmd_set_dio_rf_switch(f, dio[i], modes[i]));
    }
}

static lr2021_iq_stats_t seen; /* the session's counts already added to st.stats */

/* After a reset: the chip set up again for the session (standby, the clock and regulator, calibration), its state
 * and counts cleared. The caller holds the lock. */
static void iq_reinit(void)
{
    const esp_err_t e = lr2021_iq_init(&iq, st.dev, &st.board, false);
    st.iq_ok = e == ESP_OK;
    memset(&seen, 0, sizeof seen);
    if (!st.iq_ok)
        iq_failed();
}

/* The mode for a rate: up to 120 kS/s the narrow one (LORA, 125 kS/s, int16, resampled here); above, the wide one
 * (CONV8: the route's rate converter making exactly the settings' rate from the fastest rate code whose channel
 * filter stays within 0.8 of it, int8; spec R2.21). With iq_bits 16, the wide one is FSK16, int16 at the FSK code's
 * own rate, resampled here: about 50 dB more headroom than int8's 6 bits (R2.26), twice the SPI bytes, so up to
 * WIDE16_MAX. */
static void pick(double rate)
{
    st.iq16 = g_scfg.iq_bits == 16 && rate > 120000;
    lr2021_iq_mode_t m = LR2021_IQ_LORA;
    if (rate > 120000 && rate <= (st.iq16 ? WIDE16_MAX : WIDE_MAX))
        m = st.iq16 ? LR2021_IQ_FSK16 : LR2021_IQ_CONV8;
    if (m == LR2021_IQ_LORA || lr2021_iq_plan(rate, m, &st.plan) != ESP_OK)
        lr2021_iq_plan(125000, m = LR2021_IQ_LORA, &st.plan);
    st.wide = m != LR2021_IQ_LORA;
    st.sbytes = st.plan.sample_bytes;
    st.native_num = st.plan.rate_num;
    st.native_den = st.plan.rate_den;
    st.native = st.plan.rate;
}

/* The chip's own filter passes far less than the rate's 0.8 in the narrow mode: the LoRa bandwidth, a quarter of the
 * rate (+-15.6 kHz at 125 kS/s; it showed on the W12's spectrum). In the wide mode, the FSK channel filter: 0.37 of
 * the rate before the converter (+-185 kHz at 1 MS/s into 576 kS/s). A DMR channel (+-4.8 kHz, 7.6 with margin)
 * goes within that. As the share of the settings' rate, which the planner uses. */
static double band_usable(double rate)
{
    if (rate <= 0)
        return 0.8;
    pick(rate);
    double u = 2 * (st.plan.passband_hz / 2 - 7.6e3) / rate;
    return u > 0.8 ? 0.8 : u;
}

#define READ_SLACK_US 600

/* The session's settings from the front end's. */
static void cfg_update(void)
{
    st.cfg = (lr2021_iq_cfg_t){
        .rate = st.wide ? st.rate : st.plan.rate,
        .mode = st.plan.mode,
        .center_hz = st.center,
        .ppm = st.ppm,
        .gain = isnan(st.gain) ? 0 : (int)st.gain,
        /* read with this long left before the FIFO fills: Wi-Fi and the link hold core 0 that long at times (at
         * 640 bytes in, the library's default, about 4 losses in 3 minutes at 384 kS/s, IQ R2.34) */
        .slack_us = READ_SLACK_US,
    };
}

static int init(void *ctx)
{
    caps.usable = band_usable(g_scfg.sample_rate);
    caps.link_bytes_per_s = st.sbytes * (double)st.native_num / st.native_den; /* pick ran in band_usable */
    lr2021_esp_cfg_t ec = {
        .spi_host = CONFIG_RDOF_LR2021_SPI_HOST,
        .sck = CONFIG_RDOF_LR2021_SCK,
        .mosi = CONFIG_RDOF_LR2021_MOSI,
        .miso = CONFIG_RDOF_LR2021_MISO,
        .nss = CONFIG_RDOF_LR2021_NSS,
        .busy = CONFIG_RDOF_LR2021_BUSY,
        .nreset = CONFIG_RDOF_LR2021_NRESET,
        .irq = CONFIG_RDOF_LR2021_IRQ,
        .pa_en = CONFIG_RDOF_LR2021_PA_EN_GPIO, /* the board's front-end supply (the W12's PA_EN_M, GPIO4) */
        .clock_hz = CONFIG_RDOF_LR2021_SPI_HZ,
    };
    lr2021_config_t c = {0};
    esp_err_t e = lr2021_hal_esp_open(&ec, &c.hal);
    if (e != ESP_OK)
        return failed("SPI and pins", e);
    st.hal = c.hal;
    st.clock_hz = CONFIG_RDOF_LR2021_SPI_HZ;
    c.mem = mem_take("sdr_fe", "lr2021_dev", NULL); /* NULL, not planned: the driver allocates it */
    if ((e = lr2021_open(&c, &st.dev)) != ESP_OK) {
        st.dev = NULL;
        return failed("didn't open", e);
    }
    uint8_t maj = 0, min = 0;
    if ((e = lr2021_version(st.dev, &maj, &min)) != ESP_OK)
        return failed("GetVersion", e);
    snprintf(st.id, sizeof st.id, "LR2021 fw %u.%u", maj, min);
    ESP_LOGI(TAG, "%s (SPI %d MHz)", st.id, CONFIG_RDOF_LR2021_SPI_HZ / 1000000);
    board_setup();
    /* A ROM the raw paths aren't known on (lr2021_chips.h) leaves the chip to the experiments: start() says why */
    iq_reinit();
    if (!st.iq_ok)
        ESP_LOGW(TAG, "%s", st.error);
    st.spi = xSemaphoreCreateMutex();
    st.gain = NAN;
    const esp_timer_create_args_t sa = {.callback = said_print, .name = "lr2021 log"};
    esp_timer_create(&sa, &said_timer);
    st.ring_storage = mem_take("sdr_fe", "ring", NULL);
    if (!st.ring_storage)
        st.ring_storage = heap_caps_malloc((size_t)RING_KB * 1024 + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!st.ring_storage)
        return failed("ring", ESP_ERR_NO_MEM);
    st.ring = xStreamBufferCreateStatic((size_t)RING_KB * 1024, 4, st.ring_storage, &st.ring_struct);
    snprintf(g_sstat.rtl_desc, sizeof g_sstat.rtl_desc, "%s on SPI", st.id);
    g_sstat.rtl_open = true;
    return 0;
}

static bool wait(void *ctx, int timeout_ms) { return st.dev != NULL; }

/* A new session with st.cfg (any running one ends first: standby, reset, setup, spec 2.5). The caller holds the
 * lock. */
static int session_start(void)
{
    if (!st.iq_ok)
        return -1; /* st.error has why */
    return lr2021_iq_start(&iq, &st.cfg) == ESP_OK ? 0 : iq_failed();
}

static int tune(void *ctx, double rate, double center_hz, int ppm, double *got_rate)
{
    st.rate = rate;
    st.center = center_hz;
    st.ppm = ppm;
    pick(rate);
    if (st.iq16 && !st.wide) {
        snprintf(st.error, sizeof st.error, "LR2021: iq_bits 16 takes sample_rate up to %d (got %.0f)", WIDE16_MAX,
                 rate);
        return -1;
    }
    cfg_update();
    *got_rate = rate;
    if (!st.streaming || st.held)
        return 0; /* held: the session starts with these when the hold ends */
    lock();
    int r = st.held ? 0 : session_start(); /* checked again under the lock: a hold may have come in between */
    unlock();
    return r;
}

static int set_ppm(void *ctx, int ppm)
{
    st.ppm = ppm;
    cfg_update();
    if (!st.streaming || st.held)
        return 0;
    /* a new frequency needs CalibFE, which overwrites the FIFO: a new session (spec R2.5) */
    lock();
    int r = st.held ? 0 : session_start();
    unlock();
    return r;
}

/* The gain as SetAgcGainManual's step, 1 to 13 (spec R2.25 has their dB); NAN: the chip's AGC. */
static int set_gain(void *ctx, double db, double *got)
{
    int step = isnan(db) ? 0 : (int)lrint(db);
    step = step < 1 ? 1 : step > 13 ? 13 : step;
    st.gain = isnan(db) ? NAN : step;
    *got = st.gain;
    cfg_update();
    if (!st.dev || st.held)
        return 0; /* set when the session starts */
    lock();
    esp_err_t e = st.held || !st.iq_ok ? ESP_OK : lr2021_iq_set_gain(&iq, st.cfg.gain);
    unlock();
    return e == ESP_OK ? 0 : iq_failed();
}

static int gain_steps(void *ctx, const double **db)
{
    for (int i = 0; i < 13; i++)
        gains[i] = i + 1;
    *db = gains;
    return 13;
}

/* ── streaming ──────────────────────────────────────────────────────────── */

/* The stream's samples since start() at the chip's rate: read from it, or filled in while it transmitted. The
 * reader's alone. */
static int64_t rx_count;

static void put(const uint8_t *b, size_t n)
{
    rx_count += (int64_t)(n / (size_t)st.sbytes);
    size_t sent = xStreamBufferSpacesAvailable(st.ring) >= n ? xStreamBufferSend(st.ring, b, n, 0) : 0;
    size_t used = xStreamBufferBytesAvailable(st.ring);
    portENTER_CRITICAL(&st.lock);
    st.stats.bytes += sent;
    if (sent < n) {
        st.stats.overflows++;
        st.stats.bytes_lost += n - sent;
    }
    if (used > st.stats.ring_peak)
        st.stats.ring_peak = used;
    portEXIT_CRITICAL(&st.lock);
}

/* The session's samples, as the FIFO had them, into the ring (read() converts them, on the DSP's core). */
static void sink(void *ctx, const uint8_t *data, size_t n, unsigned flags, int64_t t_us)
{
    put(data, n * (size_t)st.sbytes);
}

static void count_error(void)
{
    portENTER_CRITICAL(&st.lock);
    st.stats.errors++;
    portEXIT_CRITICAL(&st.lock);
}

/* The session's losses and errors since the last look into st.stats. The caller holds the lock. */
static volatile unsigned losses; /* for report's line */
static void count_session(void)
{
    const unsigned dl = iq.stats.losses - seen.losses, de = iq.stats.errors - seen.errors;
    if (dl || de) {
        portENTER_CRITICAL(&st.lock);
        st.stats.overflows += dl;
        st.stats.errors += de;
        portEXIT_CRITICAL(&st.lock);
        losses += dl;
    }
    seen = iq.stats;
}

/* Every 20 s while streaming, from esp_timer's task: the reader's share of its core (FreeRTOS run-time stats) and
 * what the session did. */
static volatile struct {
    unsigned yields;
    uint64_t conv_us; /* read()'s convert and resample, on the DSP's core */
} dbg;

static void report(void *arg)
{
    static int64_t t0;
    static uint32_t run0;
    static lr2021_iq_stats_t s0;
    int64_t now = esp_timer_get_time();
    uint32_t run = 0;
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    if (st.reader) {
        TaskStatus_t ts;
        vTaskGetInfo(st.reader, &ts, pdFALSE, eRunning);
        run = (uint32_t)ts.ulRunTimeCounter; /* esp_timer us */
    }
#endif
    const lr2021_iq_stats_t s = iq.stats;
    if (s.reads < s0.reads)
        s0 = (lr2021_iq_stats_t){0}; /* a new session state (iq_reinit) */
    if (t0 && st.streaming) {
        double w = (now - t0) / 100.0;
        ESP_LOGI(TAG, "reader: %.1f%% of core 0, %u reads, %u IRQs late, %u lost, %u yields, %u SPI bounces in all; "
                      "DSP core: %.1f%% converting and resampling",
                 (uint32_t)(run - run0) / w, (unsigned)(s.reads - s0.reads), (unsigned)(s.irq_late - s0.irq_late),
                 losses, dbg.yields, st.dev ? (unsigned)lr2021_hal_esp_bounces(&st.hal) : 0, dbg.conv_us / w);
    }
    memset((void *)&dbg, 0, sizeof dbg);
    losses = 0;
    s0 = s;
    t0 = now;
    run0 = run;
}

/* ── transmit ───────────────────────────────────────────────────────────── */

/* Half duplex: the receiver stops for each key-up (the user's choice: one chip roams every channel). The modem's
 * frequency samples (pull_freq, 48 kS/s) are pulled TX_LOOK ahead of the stream into a staging ring; each keyed span
 * (a DMR burst with its ramps) goes out as one run of frequency samples (spec R3.9-R3.19) keyed up at its time
 * (lr2021_iq_transmit's at_us: within a few us, R3.48), the receiver off around it (R3.16: no reset, and no CalibFE
 * in the cell the session calibrated). The stream's gap is filled with zeros so its sample count keeps real time.
 * Stream positions map to esp_timer through an anchor taken from the sample count while nothing is sent and held
 * through a transmission, so a call's runs keep their 60 ms spacing (the gaps' fill follows the same map). All of it
 * runs in the reader: it alone touches the chip while streaming. */
#define TX_RATE 48000
#define TX_FDEV 2048     /* 32 Hz a count (R3.10) */
#define TX_STAGE 8192    /* staged samples (170 ms), a power of 2 */
#define TX_LOOK 1920     /* pulled this far ahead of now (40 ms) */
#define TX_BLOCK 96      /* samples a pull (2 ms, about 150 us of the reader) */
#define TX_MAX_RUN 4096  /* the longest span sent as one run (85 ms): a longer one is cut there */
#define TX_MERGE 192     /* keyed spans this close (4 ms) go out as one run */
/* The switch to TX starts this long before a run's time: from a running session it takes about 2.8 ms to a filled
 * FIFO (R3.48); the reader's turn comes every 1-2 ms. Less than TX_LATE_US left: the run is dropped (late), as
 * lr2021_iq_transmit would drop it once ready past its time. */
#define TX_PREP_US 5500
#define TX_LATE_US 3200
#define TX_HOLD_US 300000 /* the anchor held this long after a run */

EXT_RAM_BSS_ATTR static struct {
    int8_t s[TX_STAGE];  /* frequency samples (lr2021_raw_freq_sample) */
    uint8_t k[TX_STAGE]; /* keyed */
} stage;

static struct {
    const rdof_fe_tx_source_t *volatile src;
    volatile double power, hz; /* dBm (tx_set), the carrier (tx_tune, 0 until then) */
    int64_t pos;               /* the next sample to pull (0: none pulled yet) */
    int64_t key;               /* the first keyed sample staged and not sent yet, or -1 */
    bool hold;                 /* the anchor held: a transmission on */
    int64_t a_pos, a_us;       /* the anchor: stream sample a_pos (48 kS/s) at esp_timer a_us */
    int64_t last_us;           /* the last run's end */
    /* this transmission's, for its log line */
    unsigned runs, late, early, failed, cut, underflows, calibrated;
    int err_min, err_max;      /* key-up's time against the run's, us */
    int64_t zeros;             /* samples filled in */
    lr2021_err_t last_err;
} tx = {.key = -1};

static int64_t tx_now48(int64_t us) { return tx.a_pos + (us - tx.a_us) * TX_RATE / 1000000; }
static int64_t tx_at_us(int64_t pos) { return tx.a_us + (pos - tx.a_pos) * 1000000 / TX_RATE; }
/* The stream's count at 48 kS/s, and a 48 kS/s position as the chip's count. */
static int64_t rx48(void) { return rx_count * TX_RATE * (int64_t)st.native_den / (int64_t)st.native_num; }
static int64_t native_at(int64_t pos48) { return pos48 * (int64_t)st.native_num / ((int64_t)TX_RATE * st.native_den); }

/* Pull the source up to sample upto, a block at a time, at most blocks of them, into the stage;
 * note the first keyed sample. */
static void tx_fill(int64_t upto, int blocks)
{
    EXT_RAM_BSS_ATTR static float hz[TX_BLOCK], env[TX_BLOCK];
    const rdof_fe_tx_source_t *src = tx.src;
    while (src && tx.pos < upto && blocks-- > 0) {
        const bool keyed = src->pull_freq(src->ctx, tx.pos, hz, env, TX_BLOCK);
        for (int i = 0; i < TX_BLOCK; i++) {
            const int64_t p = tx.pos + i;
            const bool k = keyed && env[i] > 0;
            stage.s[p & (TX_STAGE - 1)] = lr2021_raw_freq_sample(hz[i], TX_FDEV);
            stage.k[p & (TX_STAGE - 1)] = k;
            if (k && tx.key < 0)
                tx.key = p;
        }
        tx.pos += TX_BLOCK;
    }
}

/* Zeros into the stream for n samples (the receiver off): its count keeps real time. */
static void tx_zero_fill(int64_t n)
{
    EXT_RAM_BSS_ATTR static uint8_t z[1024];
    const size_t sb = (size_t)st.sbytes, max = sizeof z / sb;
    tx.zeros += n;
    while (n > 0) {
        const size_t k = n > (int64_t)max ? max : (size_t)n;
        size_t sent = xStreamBufferSpacesAvailable(st.ring) >= k * sb ? xStreamBufferSend(st.ring, z, k * sb, 0) : 0;
        rx_count += (int64_t)k;
        portENTER_CRITICAL(&st.lock);
        st.stats.bytes_filled += sent;
        if (sent < k * sb) {
            st.stats.overflows++;
            st.stats.bytes_lost += k * sb - sent;
        }
        portEXIT_CRITICAL(&st.lock);
        n -= (int64_t)k;
    }
}

/* The staged samples from at to end, as FIFO bytes (lr2021_iq_transmit's pull). */
struct stage_span {
    int64_t at, end;
};

static size_t stage_pull(void *ctx, uint8_t *buf, size_t max)
{
    struct stage_span *p = ctx;
    const size_t k = p->end - p->at < (int64_t)max ? (size_t)(p->end - p->at) : max;
    for (size_t j = 0; j < k; j++)
        buf[j] = (uint8_t)stage.s[(p->at + (int64_t)j) & (TX_STAGE - 1)];
    p->at += (int64_t)k;
    return k;
}

/* One run: n staged samples from k0 on, keyed up at esp_timer t_key (lr2021_iq_transmit: the receiver paused and
 * back after), then the gap filled. The caller holds the lock, in a session. */
static void tx_run(int64_t k0, int n, int64_t t_key)
{
    const lr2021_iq_tx_t t = {
        .mode = LR2021_TX_FREQ,
        .center_hz = tx.hz,
        .ppm = st.ppm,
        .rate = TX_RATE,
        .fdev_hz = TX_FDEV,
        .power_dbm = tx.power,
        .samples = (uint64_t)n,
        .at_us = t_key,
    };
    struct stage_span span = {k0, k0 + n};
    lr2021_tx_stream_stats_t s;
    if (lr2021_iq_tx_calibrates(&iq, &t))
        tx.calibrated++;
    const lr2021_err_t e = lr2021_iq_transmit(&iq, &t, stage_pull, &span, &s);
    if (s.late) {
        tx.late++;
    } else if (s.early) {
        tx.early++;
    } else if (e == ESP_OK) {
        const int err = (int)(s.key_us - t_key);
        if (!tx.runs || err < tx.err_min)
            tx.err_min = err;
        if (!tx.runs || err > tx.err_max)
            tx.err_max = err;
        tx.runs++;
    }
    if (s.tx_flags & LR2021_FIFO_UNDERFLOW)
        tx.underflows++;
    if (e != ESP_OK && !s.late && !s.early) {
        tx.failed++;
        tx.last_err = e;
    }
    tx.last_us = esp_timer_get_time();
    /* the session didn't come back (lr2021_iq_transmit's restart failed): the reader starts a new one */
    const int64_t gap = native_at(tx_now48(esp_timer_get_time())) - rx_count;
    if (gap > 0 && gap < (int64_t)(st.native * 0.5))
        tx_zero_fill(gap);
}

/* The end of a transmission: its line in the log, the counts cleared. */
static void tx_said(void)
{
    if (tx.runs || tx.late || tx.early || tx.failed)
        say("TX at %.5f MHz, %.1f dBm: %u runs, %u late, %u early (R3.17), %u failed%s%s, %u cut, %u underflows, "
            "%u calibrated; key-up %+d to %+d us of its time; %lld samples filled",
            tx.hz / 1e6, tx.power, tx.runs, tx.late, tx.early, tx.failed, tx.failed ? ": " : "",
            tx.failed ? esp_err_to_name(tx.last_err) : "", tx.cut, tx.underflows, tx.calibrated, tx.err_min,
            tx.err_max, (long long)tx.zeros);
    tx.runs = tx.late = tx.early = tx.failed = tx.cut = tx.underflows = tx.calibrated = 0;
    tx.err_min = tx.err_max = 0;
    tx.zeros = 0;
}

/* The reader's transmit turn, between reads: stage what's due and send a run whose time has come. True if it sent
 * one (the receiver restarted). */
static bool tx_service(void)
{
    const int64_t now = esp_timer_get_time();
    if (!tx.src || tx.hz <= 0) {
        if (tx.hold)
            tx_said();
        tx.hold = false;
        tx.pos = 0;
        tx.key = -1;
        return false;
    }
    if (!tx.hold) {
        tx.a_pos = rx48();
        tx.a_us = now;
    }
    const int64_t n48 = tx_now48(now);
    if (!tx.pos || tx.pos < n48 - TX_STAGE / 2) { /* the first pull, or far behind: from now */
        tx.pos = n48;
        tx.key = -1;
    }
    /* a block a turn: after a run the source is a run's length behind, and pulling all of it at once held the reader
     * 2.8 ms, past the RX FIFO (1.3 ms at 384 kS/s): about 2.5 FIFO losses a burst. 5 ms blocks still lost about
     * one a burst; 2 ms blocks none */
    tx_fill(n48 + TX_LOOK, 1);
    if (tx.key < 0) {
        if (tx.hold && now - tx.last_us > TX_HOLD_US) {
            tx.hold = false;
            tx_said();
        }
        return false;
    }
    if (!tx.hold) {
        tx.hold = true;
        tx.last_us = now;
    }
    const int64_t t_key = tx_at_us(tx.key);
    if (t_key - now > TX_PREP_US)
        return false;
    /* the span: keyed samples, through gaps under TX_MERGE, pulling on where it reaches the stage's end */
    const int64_t k0 = tx.key;
    int64_t k1 = k0;
    while (tx.src && k1 - k0 < TX_MAX_RUN) {
        while (k1 < tx.pos && stage.k[k1 & (TX_STAGE - 1)])
            k1++;
        if (k1 >= tx.pos) {
            tx_fill(tx.pos + TX_BLOCK, 1);
            continue;
        }
        int64_t g = k1;
        while (g < tx.pos && g < k1 + TX_MERGE && !stage.k[g & (TX_STAGE - 1)])
            g++;
        if (g >= k1 + TX_MERGE)
            break; /* a real gap: the span ends at k1 */
        if (g >= tx.pos) {
            tx_fill(tx.pos + TX_BLOCK, 1);
            continue;
        }
        k1 = g; /* keyed again soon: one run */
    }
    if (k1 - k0 > TX_MAX_RUN) {
        k1 = k0 + TX_MAX_RUN;
        tx.cut++;
    }
    bool ran = false;
    if (t_key - esp_timer_get_time() < TX_LATE_US) {
        tx.late++;
    } else {
        lock();
        if (iq.in_session && !st.held) {
            tx_run(k0, (int)(k1 - k0), t_key);
            ran = true;
        }
        unlock();
    }
    tx.key = -1;
    for (int64_t p = k1; p < tx.pos; p++)
        if (stage.k[p & (TX_STAGE - 1)]) {
            tx.key = p;
            break;
        }
    return ran;
}

static int tx_set(void *ctx, const rdof_fe_tx_source_t *src, double power_dbm)
{
    tx.power = power_dbm;
    tx.src = src; /* the reader picks it up */
    return 0;
}

/* The carrier for the next runs. Within 1 MHz of the receiver's center: the front end is calibrated there (R3.16;
 * CalibFE elsewhere would cost the stream 11 ms and the RX FIFO, R2.5). */
static int tx_tune(void *ctx, double hz)
{
    if (hz < caps.tx_freq_min || hz > caps.tx_freq_max || fabs(hz - st.center) > 1e6) {
        snprintf(st.error, sizeof st.error, "LR2021: can't transmit on %.5f MHz (receiving at %.5f)", hz / 1e6,
                 st.center / 1e6);
        return -1;
    }
    tx.hz = hz;
    return 0;
}

/* The reader: lr2021_iq_poll in a loop (the FIFO's samples, little-endian I then Q, 16 bits each, spec R2.7, or 8
 * in the wide mode, into the ring), the transmit turn between. The session waits for the RX FIFO IRQ or reads by
 * the level it reckons from the rate (spec R2.34), sleeping between reads. */
static void reader_task(void *arg)
{
    for (;;) {
        if (wanting) {
            dbg.yields++;
            vTaskDelay(1);
        }
        if (!st.streaming) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (st.held) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (tx.src || tx.hold)
            tx_service();
        lock();
        if (st.held || !st.streaming) {
            unlock();
            continue;
        }
        if (!iq.in_session) {
            /* lost (a failed restart), or back from a hold: a new session, 100 ms apart while they fail */
            const int r = session_start();
            unlock();
            if (r) {
                count_error();
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue;
        }
        const int got = lr2021_iq_poll(&iq, sink, NULL, 2);
        count_session();
        unlock();
        if (got < 0)
            vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static int start(void *ctx)
{
    st.resample = fabs(st.native - st.rate) > 0.5;
    if ((caps.format == RDOF_FE_CS8) != (st.wide && !st.iq16 && !st.resample)) {
        snprintf(st.error, sizeof st.error, "LR2021: the I/Q format changed with the rate: restart the board");
        return -1;
    }
    if (st.resample) {
        /* the chip's rate is native_num / native_den (666 666.7 S/s is 2 MHz / 3): both rates times den */
        rdof_resamp_free(&st.rs);
        if (rdof_resamp_init(&st.rs, (long)st.native_num, lrint(st.rate) * (long)st.native_den, 32, caps.usable,
                             BLOCK)) {
            snprintf(st.error, sizeof st.error, "LR2021: can't resample %.0f to %.0f S/s", st.native, st.rate);
            return -1;
        }
        ESP_LOGI(TAG, "I/Q at %.0f S/s (%s, int16), resampled to %.0f (%d / %d), channels within %.0f kHz",
                 st.native, st.wide ? "FSK" : "LoRa", st.rate, st.rs.L, st.rs.M, caps.usable * st.rate / 1e3);
    } else if (st.wide) {
        ESP_LOGI(TAG, "I/Q at %.0f S/s (FSK, int8, the chip's converter from %.0f), channels within %.0f kHz", st.native,
                 st.plan.chip_rate, caps.usable * st.rate / 1e3);
    }
    if (!st.blk.raw) {
        const size_t need = BLOCK * 4 + sizeof(rdof_cf_t) * (2 * BLOCK + 4);
        /* in PSRAM: in internal RAM they were no faster (the resampler's taps and history are what it reads most) */
        uint8_t *p = mem_take("sdr_fe", "blocks", NULL);
        if (!p)
            p = heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p)
            return failed("read buffers", ESP_ERR_NO_MEM);
        st.blk.x = (rdof_cf_t *)p;
        st.blk.y = st.blk.x + BLOCK;
        st.blk.raw = (uint8_t *)(st.blk.y + BLOCK + 4);
        ESP_LOGI(TAG, "read buffers: %u bytes in PSRAM", (unsigned)need);
    }
    xStreamBufferReset(st.ring);
    memset(&st.stats, 0, sizeof st.stats);
    rx_count = 0;
    cfg_update();
    lock();
    int r = session_start();
    unlock();
    if (r)
        return -1;
    st.streaming = true;
    if (!st.reader) {
        esp_timer_handle_t t;
        esp_timer_create_args_t a = {.callback = report, .name = "lr2021 report"};
        if (esp_timer_create(&a, &t) == ESP_OK)
            esp_timer_start_periodic(t, 20000000);
        /* Above Wi-Fi (23): under it, Wi-Fi and MQTT traffic kept it off the CPU past the FIFO's 2 ms, about one
         * loss every 5 s idle and several a second on a call. It sleeps between reads (lr2021_sleep_us), so Wi-Fi
         * still gets core 0 most of the time. On core 1 (above the DSP) it lost far more. */
        mem_task("sdr_fe", "lr2021", reader_task, 4096, NULL, 24, 0, &st.reader);
    }
    xTaskNotifyGive(st.reader);
    ESP_LOGI(TAG, "streaming at %.5f MHz (%s)", st.center / 1e6,
             pa_band(st.center) ? "in the front end's band" : "outside the front end's band: about 46 dB lost in its SAW");
    return 0;
}

static void stop(void *ctx)
{
    st.streaming = false;
    lock();
    lr2021_iq_stop(&iq);
    unlock();
}

/* The ring's samples: int8 as they came (caps.format CS8), or BLOCK at a time converted to float, resampled to the
 * settings' rate and written as int16 I/Q: len bytes, or what came in timeout_ms. On the DSP's core, off the reader's. It
 * waits for the whole len: taking what's there gave a DSP with time to spare chunks of a millisecond or two, whose
 * per-chunk costs then used the time up (the load read 0.94 with 5 channels or 6, 2026-10-27). */
static size_t read(void *ctx, uint8_t *out, size_t len, int timeout_ms, size_t *filled)
{
    *filled = 0;
    uint8_t *raw = st.blk.raw;
    rdof_cf_t *x = st.blk.x, *y = st.blk.y;
    const int sb = st.sbytes;
    const rdof_fe_format_t fmt = sb == 2 ? RDOF_FE_CS8 : RDOF_FE_CS16;
    const TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    if (caps.format == RDOF_FE_CS8) {
        /* as the FIFO gave it, straight into the chunk */
        size_t done = 0;
        len -= len % 2;
        while (done < len) {
            const TickType_t left = until - xTaskGetTickCount();
            const TickType_t wait = left <= pdMS_TO_TICKS(timeout_ms) ? left : 0;
            size_t got = xStreamBufferReceive(st.ring, out + done, len - done, wait);
            if (!got)
                break;
            done += got;
        }
        return done;
    }
    int16_t *z = (int16_t *)out;
    size_t room = len / 4, done = 0;
    for (;;) {
        /* inputs whose outputs fit in what's left */
        size_t n = st.resample ? (room > 2 ? (room - 2) * (size_t)st.rs.M / (size_t)st.rs.L : 0) : room;
        if (n > BLOCK)
            n = BLOCK;
        if (!n)
            break;
        /* whole samples: the reader puts whole ones, and this asks for whole ones */
        const TickType_t left = until - xTaskGetTickCount(); /* past the deadline it wraps: then don't wait */
        const TickType_t wait = left <= pdMS_TO_TICKS(timeout_ms) ? left : 0;
        size_t got = xStreamBufferReceive(st.ring, raw, n * (size_t)sb, wait) / (size_t)sb;
        if (!got)
            break;
        int64_t t0 = esp_timer_get_time();
        rdof_fe_convert(fmt, raw, got, x);
        int m = st.resample ? rdof_resamp_run(&st.rs, x, (int)got, y) : (int)got;
        const rdof_cf_t *o = st.resample ? y : x;
        for (int i = 0; i < m; i++) {
            float re = o[i].re * 32768.0f, im = o[i].im * 32768.0f;
            z[2 * i] = (int16_t)(re > 32767 ? 32767 : re < -32768 ? -32768 : re);
            z[2 * i + 1] = (int16_t)(im > 32767 ? 32767 : im < -32768 ? -32768 : im);
        }
        dbg.conv_us += esp_timer_get_time() - t0;
        z += 2 * m;
        done += (size_t)m;
        room -= (size_t)m;
    }
    return done * 4;
}

static bool gone(void *ctx) { return st.gone; }

static void stats(void *ctx, rdof_fe_stats_t *out)
{
    portENTER_CRITICAL(&st.lock);
    *out = st.stats;
    portEXIT_CRITICAL(&st.lock);
    out->errors += st.dev ? lr2021_failures(st.dev) : 0;
}

static const char *identity(void *ctx) { return st.id; }
static const char *error(void *ctx) { return st.error; }

static const rdof_fe_ops_t ops = {
    .init = init,
    .wait = wait,
    .tune = tune,
    .set_ppm = set_ppm,
    .set_gain = set_gain,
    .gain_steps = gain_steps,
    .start = start,
    .stop = stop,
    .read = read,
    .gone = gone,
    .stats = stats,
    .identity = identity,
    .error = error,
    .tx_set = tx_set,
    .tx_tune = tx_tune,
};

int fe_lr2021_read_mem(uint32_t addr, uint32_t *w, int n)
{
    if (!st.dev || !st.spi)
        return -1;
    lock();
    esp_err_t e = ESP_OK;
    for (int i = 0; i < n && e == ESP_OK; i += 64)
        e = lr2021_read_mem(st.dev, addr + 4 * (uint32_t)i, w + i, n - i > 64 ? 64 : n - i);
    unlock();
    return e == ESP_OK ? 0 : -1;
}

/* ── experiments (web.c's /api/lr2021/...) ─────────────────────────────── */

int fe_lr2021_hold(bool on, bool keep, int patch)
{
    if (!st.dev || !st.spi)
        return -1;
    lock();
    if (on && !st.held) {
        st.held = true;
        if (!keep || patch) {
            /* reset even if no session ran: a clean chip under the patch, in standby with the chip's own settings */
            lr2021_reset(st.dev);
            iq_reinit();
        }
    }
    if (on && patch) { /* 1: Semtech's patch RAM (the documented modems' fixes); 2: and ours in bank 2 */
        esp_err_t e = patch == 2 ? lr2021_patch_load_rdof(st.dev, NULL) : lr2021_patch_load(st.dev, NULL, 0);
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
            failed("patch load", e);
            unlock();
            return -1;
        }
    } else if (!on && st.held) {
        /* back to streaming: the chip reset and set up (an experiment's settings, errors and calibration gone),
         * then the reader starts a new session */
        lr2021_reset(st.dev);
        iq_reinit();
        st.held = false;
    }
    unlock();
    return 0;
}

bool fe_lr2021_held(void) { return st.held; }

int fe_lr2021_cmd(const uint8_t *frame, size_t len, uint8_t *out, size_t n, uint8_t status[2])
{
    if (!st.dev || !st.held)
        return -1;
    lock();
    const bool fifo = len == 2 && frame[0] == 0x00 && frame[1] == 0x01; /* ReadRxFifo: the data in the same frame */
    esp_err_t e = fifo ? lr2021_read_fifo(st.dev, out, n)
                  : n  ? lr2021_read(st.dev, frame, len, out, n)
                       : lr2021_write(st.dev, frame, len);
    lr2021_status_t s;
    if (lr2021_get_status(st.dev, &s) == ESP_OK && status) {
        status[0] = (uint8_t)(s.cmd);
        status[1] = (uint8_t)(s.mode);
    }
    unlock();
    return e == ESP_OK ? 0 : -1;
}

const char *fe_lr2021_error(void) { return st.dev ? lr2021_last_error(st.dev) : "no chip"; }

int fe_lr2021_capture(uint8_t *buf, size_t n, size_t *got, unsigned *overflows, int64_t *us)
{
    /* Read the FIFO as fast as the SPI goes until n bytes (whole samples) or 3 s, at the reader's priority so
     * Wi-Fi doesn't stall it (a tick's wait after 8 empty polls). An overflow (level at 1 KB, or the flag) is
     * counted and reading goes on. */
    if (!st.dev || !st.held)
        return -1;
    *got = 0;
    *overflows = 0;
    UBaseType_t prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, 24);
    lock();
    int64_t t0 = esp_timer_get_time();
    esp_err_t e = ESP_OK;
    uint8_t f[4];
    cmd(f, lr2021_cmd(f, LR2021_CLEAR_RX_FIFO)); /* what came in before the capture isn't counted */
    cmd(f, lr2021_cmd_clear_fifo_flags(f, 0x3F, 0x3F));
    int empty = 0;
    while (*got < n && e == ESP_OK && esp_timer_get_time() - t0 < 3000000) {
        uint16_t level = 0;
        if ((e = lr2021_rx_fifo_level(st.dev, &level)) != ESP_OK)
            break;
        if (level >= FIFO_BYTES)
            ++*overflows;
        size_t k = level & ~3u;
        if (k > READ_MAX)
            k = READ_MAX;
        if (k >= 4)
            k -= 2; /* a frame a multiple of 4, as drain's (the samples are bytes here: no carry) */
        if (k > n - *got)
            k = n - *got;
        if (!k) {
            if (++empty >= 8) /* a slow or stopped stream: let the rest of the core run */
                vTaskDelay(1);
            continue;
        }
        empty = 0;
        e = lr2021_read_fifo(st.dev, buf + *got, k);
        *got += k;
    }
    *us = esp_timer_get_time() - t0;
    uint8_t rx = 0, tx = 0;
    if (lr2021_fifo_flags_peek(st.dev, &rx, &tx) == ESP_OK && (rx & LR2021_FIFO_OVERFLOW))
        ++*overflows;
    unlock();
    vTaskPrioritySet(NULL, prio);
    return e == ESP_OK ? 0 : -1;
}

int fe_lr2021_seq(const uint8_t *const *frames, const size_t *lens, int n, int64_t *us, uint8_t (*status)[2])
{
    /* Each frame sent and BUSY waited for, its time (send to BUSY low) in us[i]; a frame of length 0 is a reset
     * (NRESET, then BUSY: the chip's boot). The status after each in status[i] (cmd code, mode). Stops at the
     * first that fails: returns how many ran. */
    if (!st.dev || !st.held)
        return -1;
    lock();
    int i = 0;
    for (; i < n; i++) {
        int64_t t0 = esp_timer_get_time();
        esp_err_t e = lens[i] ? lr2021_write(st.dev, frames[i], lens[i]) : lr2021_reset(st.dev);
        us[i] = esp_timer_get_time() - t0;
        lr2021_status_t s = {0};
        lr2021_get_status(st.dev, &s);
        status[i][0] = (uint8_t)s.cmd;
        status[i][1] = (uint8_t)s.mode;
        if (e != ESP_OK)
            break;
    }
    unlock();
    return i;
}

int fe_lr2021_tx(const uint8_t *data, size_t n, const fe_lr2021_tx_opts_t *o, fe_lr2021_tx_result_t *r)
{
    /* A raw transmit run for experiments: data into the TX FIFO (prefill bytes before the start, the rest as it
     * drains, data read round as a ring until o->total bytes), then the start (SetTx, or o->start), then the FIFO
     * kept topped up until TX_DONE, the chip leaving TX, or o->max_ms. With o->runs > 1, each TX_DONE starts the
     * next run at once (what's left in the FIFO goes on). o->at frames go out at their times from the first start,
     * their status read after each (a refusal doesn't stop the run). Every poll of the level is traced (time,
     * level, IRQs, mode) while there's room. At the reader's priority, as the capture. The setup (packet type,
     * modulation, 0x0293, PA) is the caller's, by fe_lr2021_cmd or _seq. Left in TX at the end: SetStandby. */
    if (!st.dev || !st.held || !n)
        return -1;
    memset(r, 0, sizeof *r);
    const size_t total = o->total ? o->total : n;
    const size_t chunk = o->chunk ? o->chunk : 256;
    const int runs = o->runs > 0 ? (o->runs > FE_LR2021_TX_RUNS ? FE_LR2021_TX_RUNS : o->runs) : 1;
    UBaseType_t prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, 24);
    lock();
    uint8_t f[12];
    size_t sent = 0;
    esp_err_t e = ESP_OK;
    /* the next k bytes of the ring into the FIFO */
    EXT_RAM_BSS_ATTR static uint8_t tmp[1024];
#define PUT(k_)                                                                                                     \
    do {                                                                                                            \
        size_t put_n = (k_);                                                                                        \
        for (size_t j = 0; j < put_n; j++)                                                                          \
            tmp[j] = data[(sent + j) % n];                                                                          \
        e = lr2021_write_tx_fifo(st.dev, tmp, put_n);                                                               \
        sent += put_n;                                                                                              \
    } while (0)
    if (!o->keep_fifo) {
        cmd(f, lr2021_cmd(f, LR2021_CLEAR_TX_FIFO));
        cmd(f, lr2021_cmd_clear_fifo_flags(f, 0x3F, 0x3F));
        cmd(f, lr2021_cmd_clear_irq(f, 0xFFFFFFFF));
    }
    size_t pre = o->prefill < total ? o->prefill : total;
    while (e == ESP_OK && sent < pre)
        PUT(pre - sent > 1020 ? 1020 : pre - sent);
    /* the start: SetTx, or the caller's frame (sent without a status check: its status is read after) */
    const int64_t t0 = esp_timer_get_time();
#define START()                                                                                                     \
    do {                                                                                                            \
        if (o->start_len)                                                                                           \
            e = lr2021_send(st.dev, o->start, o->start_len);                                                        \
        else if (!o->no_settx)                                                                                      \
            e = lr2021_send(st.dev, f, lr2021_cmd_set_tx(f, o->timeout));                                           \
    } while (0)
    START();
    {
        lr2021_status_t s = {0};
        lr2021_get_status(st.dev, &s);
        r->start_cmd = (uint8_t)s.cmd;
    }
    r->settx_us = esp_timer_get_time() - t0;
    int run = 0, next_at = 0;
    int64_t run_t0 = 0;
    r->run_start_us[0] = 0;
    bool done = false;
    while (e == ESP_OK && !done) {
        int64_t now = esp_timer_get_time() - t0;
        if (now > (int64_t)o->max_ms * 1000) {
            r->timed_out = true;
            break;
        }
        if (next_at < o->n_at && now >= o->at[next_at].us) {
            lr2021_send(st.dev, o->at[next_at].frame, o->at[next_at].len);
            lr2021_status_t s = {0};
            lr2021_get_status(st.dev, &s);
            r->at_us[next_at] = esp_timer_get_time() - t0;
            r->at_status[next_at][0] = (uint8_t)s.cmd;
            r->at_status[next_at][1] = (uint8_t)s.mode;
            next_at++;
            continue;
        }
        uint16_t level = 0;
        uint32_t irq = 0;
        lr2021_status_t s = {0};
        if ((e = lr2021_tx_fifo_level(st.dev, &level)) != ESP_OK)
            break;
        lr2021_get_status(st.dev, &s);
        lr2021_irq(st.dev, &irq);
        if (r->n_trace < FE_LR2021_TX_TRACE &&
            (r->n_trace < 8 || o->trace_all || level != r->trace[r->n_trace - 1].level ||
             irq != r->trace[r->n_trace - 1].irq || s.mode != r->trace[r->n_trace - 1].mode))
            r->trace[r->n_trace++] = (fe_lr2021_tx_trace_t){(uint32_t)now, level, (uint8_t)s.mode, irq};
        if (irq & LR2021_IRQ_TX_DONE) {
            r->run_done_us[run] = now;
            r->done_us = now;
            if (++run >= runs) {
                done = true;
                break;
            }
            /* the next run at once: clear TX_DONE, start again */
            lr2021_send(st.dev, f, lr2021_cmd_clear_irq(f, LR2021_IRQ_TX_DONE));
            START();
            run_t0 = esp_timer_get_time() - t0;
            r->run_start_us[run] = run_t0;
            continue;
        }
        if (s.mode != LR2021_MODE_TX && now - run_t0 > 2000 && (o->start_len || !o->no_settx)) {
            r->left_tx_us = now; /* out of TX with no TX_DONE */
            done = true;
            break;
        }
        if (sent < total && level + chunk <= 1020) {
            size_t k = 1020 - level;
            if (k > total - sent)
                k = total - sent;
            if (k > 1020)
                k = 1020;
            PUT(k);
            r->refills++;
        } else if (sent >= total && !r->all_in_us) {
            r->all_in_us = now;
        } else {
            esp_rom_delay_us(o->poll_us ? o->poll_us : 100);
        }
    }
#undef PUT
#undef START
    r->runs = run;
    r->sent = sent;
    r->end_us = esp_timer_get_time() - t0;
    lr2021_fifo_flags_peek(st.dev, &r->rx_flags, &r->tx_flags);
    lr2021_irq(st.dev, &r->irq);
    lr2021_status_t s = {0};
    lr2021_get_status(st.dev, &s);
    r->mode = s.mode;
    if (s.mode == LR2021_MODE_TX || r->timed_out)
        cmd(f, lr2021_cmd_u8(f, LR2021_SET_STANDBY, 0));
    unlock();
    vTaskPrioritySet(NULL, prio);
    return e == ESP_OK ? 0 : -1;
}

int fe_lr2021_spi(int hz, int delay_ns)
{
    if (!st.dev)
        return -1;
    if (hz <= 0)
        return st.clock_hz;
    lock();
    esp_err_t e = lr2021_set_clock(st.dev, hz, delay_ns);
    if (e == ESP_OK)
        st.clock_hz = hz;
    unlock();
    return e == ESP_OK ? hz : -1;
}

/* The format, set at the first pick (the rate changes only with a restart; the settings check picks too, while it
 * streams, so nothing of the session's is touched): int8 as the FIFO gives it when the chip's converter makes the
 * rate (pick's CONV8), else int16 from read()'s float path. int8 halves the DSP's chunk buffer in internal RAM and
 * spares read() a pass. */
void fe_lr2021(rdof_fe_t *fe)
{
    static bool set;
    if (!set) {
        const double rate = g_scfg.sample_rate;
        lr2021_iq_plan_t p;
        caps.format = rate > 120000 && rate <= WIDE_MAX && g_scfg.iq_bits != 16 &&
                              lr2021_iq_plan(rate, LR2021_IQ_CONV8, &p) == ESP_OK && fabs(p.rate - rate) <= 0.5
                          ? RDOF_FE_CS8
                          : RDOF_FE_CS16;
        set = true;
    }
    *fe = (rdof_fe_t){&ops, &caps, NULL};
}

#endif

/* The driver's TX modes (lr2021_tx.h) on the held chip: the setup frames, then the data through lr2021_tx_stream,
 * at the reader's priority. For bench checks of the driver (web.c's /api/lr2021/txmode). */
struct tx_src {
    const uint8_t *data;
    size_t n, at;
};

static size_t tx_pull(void *ctx, uint8_t *buf, size_t max)
{
    struct tx_src *s = ctx;
    size_t k = s->n - s->at < max ? s->n - s->at : max;
    memcpy(buf, s->data + s->at, k);
    s->at += k;
    return k;
}

int fe_lr2021_tx_mode(const lr2021_tx_setup_t *s, const uint8_t *data, size_t n, uint32_t timeout, int max_ms,
                      lr2021_tx_stream_stats_t *stats, int *setup_ok)
{
    if (!st.dev || !st.held)
        return -1;
    EXT_RAM_BSS_ATTR static uint8_t frames[LR2021_TX_FRAMES_MAX][LR2021_TX_FRAME_MAX], buf[1024];
    size_t lens[LR2021_TX_FRAMES_MAX];
    int nf = lr2021_tx_frames(s, frames, lens);
    *setup_ok = 0;
    if (nf < 0) {
        snprintf(st.error, sizeof st.error, "bad TX setup");
        return -1;
    }
    UBaseType_t prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, 24);
    lock();
    esp_err_t e = ESP_OK;
    for (int i = 0; i < nf && e == ESP_OK; i++)
        if ((e = cmd(frames[i], lens[i])) == ESP_OK)
            (*setup_ok)++;
    if (e == ESP_OK) {
        uint8_t start[8];
        struct tx_src src = {data, n, 0};
        const int bps = lr2021_tx_sample_bytes(s->mode);
        lr2021_tx_stream_cfg_t c = {
            .start = start,
            .start_len = lr2021_tx_start_frame(s, start, timeout),
            .run_bytes = s->mode == LR2021_TX_CW ? 0 : (uint32_t)s->n * (uint32_t)(bps ? bps : 1),
            .bytes_per_s = s->mode == LR2021_TX_FSK4 ? s->rate / 8 : s->rate * (uint32_t)bps,
            .max_ms = max_ms,
            .buf = buf,
            .buf_len = sizeof buf,
        };
        e = lr2021_tx_stream(st.dev, &c, tx_pull, &src, stats);
    }
    unlock();
    vTaskPrioritySet(NULL, prio);
    return e == ESP_OK ? 0 : failed("TX", e);
}

/* One packet with the chip's modem (lr2021_packet_send / _receive) on the held chip, set up by fe_lr2021_seq first. */
int fe_lr2021_packet(bool tx, uint8_t *data, size_t n, size_t *got, const lr2021_packet_cfg_t *c,
                     lr2021_packet_result_t *r, esp_err_t *err)
{
    if (!st.dev || !st.held)
        return -1;
    UBaseType_t prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, 24);
    lock();
    esp_err_t e = tx ? lr2021_packet_send(st.dev, data, n, c, r) : lr2021_packet_receive(st.dev, data, n, c, r);
    unlock();
    vTaskPrioritySet(NULL, prio);
    *err = e;
    if (got)
        *got = tx ? r->len : (r->len < n ? r->len : n);
    if (e != ESP_OK)
        failed(tx ? "packet TX" : "packet RX", e); /* the text for fe_lr2021_error, with the driver's own */
    return 0;
}

int fe_lr2021_dmr_rx(double hz, int family)
{
    if (!st.dev || !st.held)
        return -1;
    uint8_t f[16];
    esp_err_t e;
    const uint32_t chip_hz = lr2021_freq_corrected(hz, st.ppm), cal_hz = (uint32_t)lrint(hz);
    int r = -1;
    lock();
    rf_switch(hz);
    if ((e = cmd(f, lr2021_cmd_u8(f, LR2021_SET_STANDBY, 0))) != ESP_OK ||
        (e = cmd(f, lr2021_cmd_u8(f, LR2021_SET_PACKET_TYPE, LR2021_PACKET_FSK))) != ESP_OK)
        failed("SetPacketType", e);
    else if ((e = cmd(f, lr2021_cmd_calib_fe(f, &cal_hz, 1, false))) != ESP_OK)
        failed("CalibFE", e);
    else if ((e = cmd(f, lr2021_cmd_set_rf_frequency(f, chip_hz))) != ESP_OK)
        failed("SetRfFrequency", e);
    else if ((e = cmd(f, lr2021_cmd_set_rx_path(f, false, CONFIG_RDOF_LR2021_RX_BOOST))) != ESP_OK)
        failed("SetRxPath", e);
    else if ((e = lr2021_fsk4_dmr_rx(st.dev, family)) != ESP_OK)
        failed("DMR 4-FSK setup", e);
    else
        r = 0;
    unlock();
    return r;
}

int fe_lr2021_wake(void)
{
    if (!st.dev || !st.held)
        return -1;
    lock();
    esp_err_t e = lr2021_wake(st.dev);
    unlock();
    return e == ESP_OK ? 0 : failed("wake", e);
}
