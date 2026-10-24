// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The LR2021 front end (device.type lr2021, CONFIG_RDOF_SDR_LR2021): a Semtech
 * LR2021 on SPI (components/lr2021). Its receiver's raw I/Q comes through
 * ROM 1.24's undocumented route into the 1 KB RX FIFO (docs/spec/lr2021-iq.md),
 * read by a task on core 0, above Wi-Fi, into a PSRAM ring, and resampled to the settings'
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
#include "lr2021_tx.h"
#include "mem.h"
#include "rdof/sdr/resample.h"
#include "sdr_fe.h"

#if CONFIG_RDOF_SDR_LR2021

static const char *TAG = "lr2021";

#define IRQ_LEVEL_WIDE16 256
/* iq_bits 16: 4 bytes a sample. 240 kS/s from the chip's 250 k is 1 MB/s. 288 k (333.3 k, 1.33 MB/s) lost samples
 * about 70 times a second at 16 MHz SPI (reads 76% of the time, control 17%) and 0.4 at 20 MHz (out of spec) */
#define WIDE16_MAX 240000
#define WIDE16_CHIP_MAX 260000
#define READ_MAX 1024 /* bytes a FIFO read: the whole FIFO in one frame, by DMA */
#define FIFO_BYTES 1024
/* The FIFO level that raises the RX FIFO IRQ on the DIO (1.5 ms at 125 kS/s, the rest fills in 0.5): the reader
 * sleeps until it's there, then takes it in one transfer. In the wide mode the FIFO fills in 0.77 ms: half of it,
 * so the reader has 0.38 ms to wake. Without an IRQ pin it waits a tick at a time below a quarter of the FIFO. */
#define IRQ_LEVEL 768
#define IRQ_LEVEL_WIDE 512

#define POLL_LEVEL 256
#define IRQ_DIO 7 /* the W12's DIO7, wired to GPIO7 (CONFIG_RDOF_LR2021_IRQ) */
#define LOSS (LR2021_FIFO_OVERFLOW | LR2021_FIFO_UNDERFLOW) /* spec R2.14 */
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
    .duplex = RDOF_FE_HALF_DUPLEX, /* one synthesizer: RX or TX (TX not wired up yet) */
    .tx_mode = RDOF_FE_TX_NONE,
    .tx_freq_min = 150e6,
    .tx_freq_max = 960e6,
    .tx_power_min = -9.5, /* the LF PA, 0.5 dB steps (7.4.3) */
    .tx_power_max = 22,
    .tx_power_step = 0.5,
    .turnaround_us = 107, /* RX to TX, typical (table 3-23); TX to RX 91 */
};

static struct {
    lr2021_t *dev;
    char error[128];
    char id[32];
    double rate, center, native;
    int ppm;
    double gain;          /* a step, or NAN for the AGC */
    /* The mode (pick): narrow, the LoRa modem at bandwidth bw and int16 I/Q (the first one proven, spec R2.8);
     * wide, the FSK modem at rx_bw code, its rate converted by the chip to the settings' rate, int8 I/Q (spec R2.19
     * to R2.21). What comes out of the FIFO is at native_num / native_den S/s. */
    bool wide;
    uint8_t bw, code;
    int sbytes;           /* bytes a sample on the FIFO and in the ring */
    uint32_t native_num, native_den;
    uint32_t chip_num, chip_den; /* the wide mode: the rate before the converter, as a fraction */

    SemaphoreHandle_t spi; /* one user of the chip at a time: the reader, or a control call */
    bool in_session;
    volatile bool streaming, gone;
    bool iq16;            /* the wide mode in int16 (iq_bits 16) */
    uint8_t carry_n;      /* drain: bytes of a sample read and not yet in the ring (0 or 2) */
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
 * lr2021.h, lr2021_bounces), the reader's stack, the ring and read()'s blocks (PSRAM), and what the SPI bus takes
 * of DMA memory for itself (its state and descriptors). */
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

/* ── setup ──────────────────────────────────────────────────────────────── */

/* The front end's transmit path for a frequency: its PA inside its band, else its transmit bypass. It receives
 * through its LNA either way (the GC1109 has no receive bypass). */
static bool pa_band(double hz)
{
    return hz >= CONFIG_RDOF_LR2021_PA_LO_MHZ * 1e6 && hz <= CONFIG_RDOF_LR2021_PA_HI_MHZ * 1e6;
}

static void rf_switch(double hz)
{
    /* DIO9 to DIO11 drive the W12's GC1109 (CTX, CPS, CSD): high in the modes each mask names. Receive is CSD
     * high, CTX low; transmit CTX high, CPS high for the PA (datasheet table 4; MeshCore drives the W12 the same:
     * standby 0/0/0, receive 0/0/1, transmit 1/1/1). */
    static const uint8_t dio[3] = {9, 10, 11};
    static const uint8_t pa[3] = {CONFIG_RDOF_LR2021_DIO9_PA, CONFIG_RDOF_LR2021_DIO10_PA, CONFIG_RDOF_LR2021_DIO11_PA};
    static const uint8_t bypass[3] = {CONFIG_RDOF_LR2021_DIO9_BYPASS, CONFIG_RDOF_LR2021_DIO10_BYPASS,
                                      CONFIG_RDOF_LR2021_DIO11_BYPASS};
    const uint8_t *modes = pa_band(hz) ? pa : bypass;
    uint8_t f[12];
    for (int i = 0; i < 3; i++) {
        if (modes[i] == 0xFF)
            continue; /* not used */
        cmd(f, lr2021_cmd_set_dio_function(f, dio[i], LR2021_DIO_RF_SWITCH, 0));
        cmd(f, lr2021_cmd_set_dio_rf_switch(f, dio[i], modes[i]));
    }
}

/* After power-up or a reset: standby, the clock and regulator, calibration (DS 6.4). */
static int chip_setup(void)
{
    uint8_t f[12];
    cmd(f, lr2021_cmd_u8(f, LR2021_SET_STANDBY, 0));
    cmd(f, lr2021_cmd(f, LR2021_CLEAR_ERRORS));
    if (CONFIG_RDOF_LR2021_TCXO_TUNE >= 0)
        cmd(f, lr2021_cmd_set_tcxo(f, CONFIG_RDOF_LR2021_TCXO_TUNE, 32000 * 5)); /* 5 ms to start */
    cmd(f, lr2021_cmd_u8(f, LR2021_SET_REG_MODE, 0)); /* LDO */
    cmd(f, lr2021_cmd_u8(f, LR2021_CALIBRATE,
                         LR2021_CAL_LF_RC | LR2021_CAL_HF_RC | LR2021_CAL_PLL | LR2021_CAL_AAF | LR2021_CAL_MU |
                             LR2021_CAL_PA_OFF));
    uint16_t errs = 0;
    lr2021_errors(st.dev, &errs);
    if (errs)
        ESP_LOGW(TAG, "after calibration: errors 0x%04x (%s)", errs, lr2021_error_name(errs));
    return 0;
}

/* The mode for a rate: up to 120 kS/s the narrow one (LoRa, 125 kS/s, int16, resampled here); above, the wide one
 * (FSK, the route's rate converter making exactly the settings' rate from the fastest rate code whose channel filter
 * stays within 0.8 of it, int8; spec R2.21). With iq_bits 16, the wide one is int16 at the FSK code's own rate
 * (source 2), resampled here: about 50 dB more headroom than int8's 6 bits (R2.26), twice the SPI bytes, so up to
 * WIDE16_MAX. */
static void pick(double rate)
{
    st.iq16 = g_scfg.iq_bits == 16 && rate > 120000;
    int code = !(rate > 120000 && rate <= (st.iq16 ? WIDE16_MAX : WIDE_MAX)) ? -1
               : st.iq16 ? lr2021_fsk_rx_bw_for(rate, WIDE16_CHIP_MAX)
                         : lr2021_conv_rx_bw_for(rate, 0.8);
    st.wide = code >= 0;
    if (st.wide && st.iq16) {
        st.code = (uint8_t)code;
        st.sbytes = lr2021_raw_sample_bytes(LR2021_RAW_SRC_CS16);
        lr2021_fsk_rx_rate(st.code, &st.chip_num, &st.chip_den);
        st.native_num = st.chip_num;
        st.native_den = st.chip_den;
    } else if (st.wide) {
        st.code = (uint8_t)code;
        st.sbytes = lr2021_raw_sample_bytes(LR2021_RAW_SRC_CONV);
        lr2021_fsk_rx_rate(st.code, &st.chip_num, &st.chip_den);
        st.native_num = (uint32_t)lrint(rate); /* the converter's output */
        st.native_den = 1;
    } else {
        st.bw = LR2021_LORA_BW_31;
        st.sbytes = lr2021_raw_sample_bytes(LR2021_RAW_SRC_CS16);
        st.native_num = lr2021_raw_rx_rate(st.bw);
        st.native_den = 1;
    }
    st.native = (double)st.native_num / st.native_den;
}

/* The FIFO level that raises the IRQ in the mode picked. */
/* int16 in the wide mode fills the FIFO in 0.77 ms: at 512 bytes the reader had 0.38 ms to start, and lost
 * samples about 25 times a second */
static size_t irq_level(void) { return st.wide ? (st.iq16 ? IRQ_LEVEL_WIDE16 : IRQ_LEVEL_WIDE) : IRQ_LEVEL; }

/* The chip's own filter passes far less than the rate's 0.8 in the narrow mode: the LoRa bandwidth, a quarter of the
 * rate (+-15.6 kHz at 125 kS/s; it showed on the W12's spectrum). In the wide mode, the FSK channel filter: 0.37 of
 * the rate before the converter (+-185 kHz at 1 MS/s into 576 kS/s). A DMR channel (+-4.8 kHz, 7.6 with margin)
 * goes within that. As the share of the settings' rate, which
 * the planner uses. */
static double band_usable(double rate)
{
    if (rate <= 0)
        return 0.8;
    pick(rate);
    double half = (st.wide ? (double)st.chip_num / st.chip_den * lr2021_fsk_filter_share(st.code) : st.native / 4) / 2;
    double u = 2 * (half - 7.6e3) / rate;
    return u > 0.8 ? 0.8 : u;
}

static int init(void *ctx)
{
    caps.usable = band_usable(g_scfg.sample_rate);
    caps.link_bytes_per_s = st.sbytes * (double)st.native_num / st.native_den; /* pick ran in band_usable */
    if (CONFIG_RDOF_LR2021_PA_EN_GPIO >= 0) {
        /* the board's front-end supply (the W12's PA_EN_M, GPIO4) */
        gpio_config_t o = {.pin_bit_mask = 1ULL << CONFIG_RDOF_LR2021_PA_EN_GPIO, .mode = GPIO_MODE_OUTPUT};
        gpio_config(&o);
        gpio_set_level(CONFIG_RDOF_LR2021_PA_EN_GPIO, 1);
    }
    lr2021_config_t c = {
        .spi_host = CONFIG_RDOF_LR2021_SPI_HOST,
        .sck = CONFIG_RDOF_LR2021_SCK,
        .mosi = CONFIG_RDOF_LR2021_MOSI,
        .miso = CONFIG_RDOF_LR2021_MISO,
        .nss = CONFIG_RDOF_LR2021_NSS,
        .busy = CONFIG_RDOF_LR2021_BUSY,
        .nreset = CONFIG_RDOF_LR2021_NRESET,
        .irq = CONFIG_RDOF_LR2021_IRQ,
        .clock_hz = CONFIG_RDOF_LR2021_SPI_HZ,
    };
    c.mem = mem_take("sdr_fe", "lr2021_dev", NULL); /* NULL, not planned: the driver allocates it */
    esp_err_t e = lr2021_open(&c, &st.dev);
    if (e != ESP_OK)
        return failed("didn't open", e);
    uint8_t maj = 0, min = 0;
    if ((e = lr2021_version(st.dev, &maj, &min)) != ESP_OK)
        return failed("GetVersion", e);
    snprintf(st.id, sizeof st.id, "LR2021 fw %u.%u", maj, min);
    ESP_LOGI(TAG, "%s (SPI %d MHz)", st.id, CONFIG_RDOF_LR2021_SPI_HZ / 1000000);
    if (maj != LR2021_VERSION_MAJOR || min != LR2021_VERSION_MINOR)
        ESP_LOGW(TAG, "firmware %u.%u: the datasheet's LR2021 is %u.%u", maj, min, LR2021_VERSION_MAJOR,
                 LR2021_VERSION_MINOR);
    if (chip_setup() != 0)
        return -1;
    st.spi = xSemaphoreCreateMutex();
    st.gain = NAN;
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

/* The LoRa receiver the raw route sits in (spec 2.1), at the center: the bandwidth sets the rate. */
static int lora_setup(void)
{
    uint8_t f[16];
    esp_err_t e;
    const double hz = st.center;
    const uint32_t chip_hz = lr2021_freq_corrected(hz, st.ppm), cal_hz = (uint32_t)lrint(hz);
    rf_switch(hz);
    if ((e = cmd(f, lr2021_cmd_u8(f, LR2021_SET_STANDBY, 0))) != ESP_OK ||
        (e = cmd(f, lr2021_cmd_u8(f, LR2021_SET_PACKET_TYPE, LR2021_PACKET_LORA))) != ESP_OK)
        return failed("SetPacketType", e);
    if ((e = cmd(f, lr2021_cmd_calib_fe(f, &cal_hz, 1, false))) != ESP_OK)
        return failed("CalibFE", e);
    if ((e = cmd(f, lr2021_cmd_set_rf_frequency(f, chip_hz))) != ESP_OK)
        return failed("SetRfFrequency", e);
    if ((e = cmd(f, lr2021_cmd_set_rx_path(f, false, CONFIG_RDOF_LR2021_RX_BOOST))) != ESP_OK)
        return failed("SetRxPath", e);
    /* SF7, CR 4/5, a preamble of 8, 255 bytes, sync 0x12: they don't matter to the raw samples (spec R2.2) */
    if ((e = cmd(f, lr2021_cmd_lora_modulation(f, 7, st.bw, 1, 0))) != ESP_OK ||
        (e = cmd(f, lr2021_cmd_lora_packet(f, 8, 255, 0x02))) != ESP_OK ||
        (e = cmd(f, lr2021_cmd_u8(f, LR2021_SET_LORA_SYNCWORD, 0x12))) != ESP_OK)
        return failed("LoRa setup", e);
    uint16_t errs = 0;
    if ((e = lr2021_errors(st.dev, &errs)) != ESP_OK || errs) {
        snprintf(st.error, sizeof st.error, "LR2021 setup at %.5f MHz: %s", hz / 1e6,
                 errs ? lr2021_error_name(errs) : esp_err_to_name(e));
        return -1;
    }
    return 0;
}

/* The FSK receiver the wide route sits in, at the center: rx_bw sets the rate and the channel filter (spec
 * R2.19). The bit rate and deviation don't matter to the samples; they're set well inside the filter. */
static int fsk_setup(void)
{
    uint8_t f[16];
    esp_err_t e;
    const double hz = st.center;
    const uint32_t chip_hz = lr2021_freq_corrected(hz, st.ppm), cal_hz = (uint32_t)lrint(hz);
    const uint32_t bitrate = (uint32_t)((double)st.chip_num / st.chip_den * lr2021_fsk_filter_share(st.code) / 4);
    rf_switch(hz);
    if ((e = cmd(f, lr2021_cmd_u8(f, LR2021_SET_STANDBY, 0))) != ESP_OK ||
        (e = cmd(f, lr2021_cmd_u8(f, LR2021_SET_PACKET_TYPE, LR2021_PACKET_FSK))) != ESP_OK)
        return failed("SetPacketType", e);
    if ((e = cmd(f, lr2021_cmd_calib_fe(f, &cal_hz, 1, false))) != ESP_OK)
        return failed("CalibFE", e);
    if ((e = cmd(f, lr2021_cmd_set_rf_frequency(f, chip_hz))) != ESP_OK)
        return failed("SetRfFrequency", e);
    if ((e = cmd(f, lr2021_cmd_set_rx_path(f, false, CONFIG_RDOF_LR2021_RX_BOOST))) != ESP_OK)
        return failed("SetRxPath", e);
    if ((e = cmd(f, lr2021_cmd_fsk_modulation(f, bitrate, 0, st.code, bitrate / 2))) != ESP_OK)
        return failed("SetFskModulationParams", e);
    uint16_t errs = 0;
    if ((e = lr2021_errors(st.dev, &errs)) != ESP_OK || errs) {
        snprintf(st.error, sizeof st.error, "LR2021 FSK setup at %.5f MHz: %s", hz / 1e6,
                 errs ? lr2021_error_name(errs) : esp_err_to_name(e));
        return -1;
    }
    return 0;
}

static bool fifo_mapped(void)
{
    uint32_t base = 0, size = 0;
    if (lr2021_read32(st.dev, LR2021_REG_RX_FIFO_BASE, &base) != ESP_OK ||
        lr2021_read32(st.dev, LR2021_REG_RX_FIFO_SIZE, &size) != ESP_OK)
        return false;
    /* ROM 1.24 reads the base back without its alias bit (spec 2.2 step 4) */
    return (base == LR2021_RX_FIFO_BASE_1K || base == (LR2021_RX_FIFO_BASE_1K & 0xFFFF)) &&
           size == LR2021_RX_FIFO_SIZE_1K;
}

/* A raw receive session (spec 2.2). The caller holds the lock. */
static int session_start(void)
{
    uint8_t f[16];
    esp_err_t e;
    uint8_t maj = 0, min = 0;
    uint32_t marker = 0;
    if (lr2021_version(st.dev, &maj, &min) != ESP_OK || maj != LR2021_VERSION_MAJOR || min != LR2021_VERSION_MINOR) {
        snprintf(st.error, sizeof st.error, "LR2021 firmware %u.%u: raw I/Q needs 1.24", maj, min);
        return -1;
    }
    if (lr2021_read32(st.dev, LR2021_PATCH_MARKER_ADDR, &marker) != ESP_OK || marker == LR2021_PATCH_ACTIVE) {
        snprintf(st.error, sizeof st.error, "LR2021: a Semtech patch is running; raw I/Q needs the plain ROM");
        return -1;
    }
    if (st.wide ? fsk_setup() : lora_setup())
        return -1;
    cmd(f, lr2021_cmd_u8(f, LR2021_SET_STANDBY, 0));
    if ((e = cmd(f, st.wide && !st.iq16 ? lr2021_cmd_raw_route_rate(f, st.code, st.native_num, LR2021_RAW_SRC_CONV)
                                        : lr2021_cmd_raw_route_fifo(f))) != ESP_OK) /* int16: at the modem's rate */
        return failed("raw route", e);
    cmd(f, lr2021_cmd_write_reg32(f, LR2021_REG_RX_FIFO_BASE, LR2021_RX_FIFO_BASE_1K));
    cmd(f, lr2021_cmd_write_reg32(f, LR2021_REG_RX_FIFO_SIZE, LR2021_RX_FIFO_SIZE_1K));
    if (!fifo_mapped()) {
        snprintf(st.error, sizeof st.error, "LR2021: the 1 KB RX FIFO didn't map");
        return -1;
    }
    cmd(f, lr2021_cmd_clear_fifo_flags(f, 0x3F, 0x3F));
    if (CONFIG_RDOF_LR2021_IRQ >= 0) {
        /* the RX FIFO IRQ on the DIO at IRQ_LEVEL bytes (ConfigFifoIrq, 6.10.1; a high threshold over 256 is fine) */
        cmd(f, lr2021_cmd_set_dio_function(f, IRQ_DIO, LR2021_DIO_IRQ, 0));
        cmd(f, lr2021_cmd_set_dio_irq(f, IRQ_DIO, LR2021_IRQ_RX_FIFO));
        cmd(f, lr2021_cmd_config_fifo_irq(f, LR2021_FIFO_HIGH, 0, irq_level(), 0, 0, 0));
        cmd(f, lr2021_cmd_clear_irq(f, 0xFFFFFFFF));
    }
    if ((e = cmd(f, lr2021_cmd_set_rx(f, 0xFFFFFF))) != ESP_OK) /* continuous (6.3.5) */
        return failed("SetRx", e);
    /* The gain after SetRx: SetRx sets step 13 (0xF30154[23:16] = 0xDD), whatever came before, and the AGC
     * runs only once 0 is sent in RX (measured; spec R2.25) */
    if ((e = cmd(f, lr2021_cmd_set_gain(f, isnan(st.gain) ? 0 : (uint8_t)st.gain))) != ESP_OK)
        return failed("SetAgcGainManual", e);
    if (!fifo_mapped()) {
        snprintf(st.error, sizeof st.error, "LR2021: the RX FIFO moved when RX started");
        return -1;
    }
    cmd(f, lr2021_cmd(f, LR2021_CLEAR_RX_FIFO));
    cmd(f, lr2021_cmd_clear_fifo_flags(f, 0x3F, 0x3F));
    st.carry_n = 0;
    st.in_session = true;
    return 0;
}

/* End a session: standby, reset, set the chip up again (spec 2.5). The caller holds the lock. */
static void session_end(void)
{
    uint8_t f[4];
    if (!st.in_session)
        return;
    st.in_session = false;
    cmd(f, lr2021_cmd_u8(f, LR2021_SET_STANDBY, 0));
    lr2021_reset(st.dev);
    chip_setup();
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
    *got_rate = rate;
    if (!st.streaming || st.held)
        return 0; /* held: the session starts with these when the hold ends */
    lock();
    int r = 0;
    if (!st.held) { /* checked again under the lock: a hold may have come in between */
        session_end();
        r = session_start();
    }
    unlock();
    return r;
}

static int set_ppm(void *ctx, int ppm)
{
    st.ppm = ppm;
    if (!st.streaming || st.held)
        return 0;
    /* a new frequency needs CalibFE, which overwrites the FIFO: a new session (spec R2.5) */
    lock();
    int r = 0;
    if (!st.held) { /* checked again under the lock: a hold may have come in between */
        session_end();
        r = session_start();
    }
    unlock();
    return r;
}

/* The gain as SetAgcGainManual's step, 1 to 13 (the datasheet gives no dB for them); NAN: the chip's AGC. */
static int set_gain(void *ctx, double db, double *got)
{
    int step = isnan(db) ? 0 : (int)lrint(db);
    step = step < 1 ? 1 : step > 13 ? 13 : step;
    st.gain = isnan(db) ? NAN : step;
    *got = st.gain;
    if (!st.in_session || st.held)
        return 0; /* set when the session starts */
    uint8_t f[4];
    lock();
    esp_err_t e = st.held ? ESP_OK : cmd(f, lr2021_cmd_set_gain(f, isnan(db) ? 0 : (uint8_t)step));
    unlock();
    return e == ESP_OK ? 0 : failed("SetAgcGainManual", e);
}

static int gain_steps(void *ctx, const double **db)
{
    for (int i = 0; i < 13; i++)
        gains[i] = i + 1;
    *db = gains;
    return 13;
}

/* ── streaming ──────────────────────────────────────────────────────────── */

static void put(const uint8_t *b, size_t n)
{
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

static void count_error(void)
{
    portENTER_CRITICAL(&st.lock);
    st.stats.errors++;
    portEXIT_CRITICAL(&st.lock);
}

/* Lost samples: count it and, in the narrow mode, start a new session (spec R2.15); in the wide mode, empty the
 * FIFO (R2.28). The caller holds the lock. why: what showed it,
 * logged at most every 10 s with how many since. */
static void lost(const char *why)
{
    static int64_t said_us;
    static unsigned since;
    portENTER_CRITICAL(&st.lock);
    st.stats.overflows++;
    portEXIT_CRITICAL(&st.lock);
    since++;
    int64_t now = esp_timer_get_time();
    if (!said_us || now - said_us >= 10000000) {
        ESP_LOGW(TAG, "samples lost (%s); %u since the last said", why, since);
        said_us = now;
        since = 0;
    }
    if (st.wide) {
        /* empty the FIFO and go on: a new session takes about 30 ms, and at a wide rate the stream overflowed again
         * right after it (int16 at 288 k: 25 losses a second, the stream down most of the time) */
        uint8_t f[6];
        cmd(f, lr2021_cmd(f, LR2021_CLEAR_RX_FIFO));
        cmd(f, lr2021_cmd_clear_fifo_flags(f, 0x3F, 0x3F));
        st.carry_n = 0;
        return;
    }
    session_end();
    if (session_start())
        ESP_LOGW(TAG, "%s", st.error);
}

/* Every 20 s while streaming, from esp_timer's task (a log line in the reader stalled it past the FIFO): the
 * reader's share of its core (FreeRTOS run-time stats) and what it did. */
static volatile unsigned waits, late;
static volatile struct {
    unsigned high_now, reads, yields;
    uint64_t level_sum, read_us, dsp_us, ctl_us;
    uint64_t conv_us; /* read()'s convert and resample, on the DSP's core */
} dbg;

static void report(void *arg)
{
    static int64_t t0;
    static uint32_t run0;
    int64_t now = esp_timer_get_time();
    uint32_t run = 0;
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    if (st.reader) {
        TaskStatus_t ts;
        vTaskGetInfo(st.reader, &ts, pdFALSE, eRunning);
        run = (uint32_t)ts.ulRunTimeCounter; /* esp_timer us */
    }
#endif
    if (t0 && st.streaming) {
        double w = (now - t0) / 100.0;
        ESP_LOGI(TAG, "reader: %.1f%% of core 0 (read %.1f%%, control %.1f%%, convert %.1f%% of the time), %u reads of "
                      "%u bytes, %u waits, %u IRQs late, pin high before %u, %u yields, %u SPI bounces in all; DSP core: "
                      "%.1f%% converting and resampling",
                 (uint32_t)(run - run0) / w, dbg.read_us / w, dbg.ctl_us / w, dbg.dsp_us / w, dbg.reads,
                 dbg.reads ? (unsigned)(dbg.level_sum / dbg.reads) : 0, waits, late, dbg.high_now, dbg.yields,
                 st.dev ? (unsigned)lr2021_bounces(st.dev) : 0, dbg.conv_us / w);
    }
    memset((void *)&dbg, 0, sizeof dbg);
    waits = late = 0;
    t0 = now;
    run0 = run;
}

/* n bytes of the FIFO (whole samples, at most READ_MAX) in one read, by DMA (the task sleeps while it moves),
 * into the ring as they came (read() converts them, on the DSP's core). Every 8 KB (1 without the IRQ) the FIFO
 * flags are read (a loss shows there, spec R2.14); then_rearm: the IRQ re-armed after. False if it failed or
 * samples were lost. The caller holds the lock. */
/* Re-arm the IRQ: clear FIFO-high (the loss flags stay for the peek), then the IRQ it raised (the flag alone leaves
 * it, 6.10.3). Two frames, no status read: they can't fail. */
static void rearm(void)
{
    uint8_t f[6];
    lr2021_send(st.dev, f, lr2021_cmd_clear_fifo_flags(f, LR2021_FIFO_HIGH, 0));
    lr2021_send(st.dev, f, lr2021_cmd_clear_irq(f, LR2021_IRQ_RX_FIFO));
}

static bool drain(size_t n, bool then_rearm)
{
    /* in PSRAM: internal RAM is the DSP's (its buffers didn't fit with this there) */
    EXT_RAM_BSS_ATTR static uint8_t buf[READ_MAX + 2];
    static uint32_t since_flags;
    /* Each read is n - 2 bytes (n a multiple of 4), so its frame, 2 status bytes and the samples, is a multiple of
     * 4: the SPI driver then needs no internal buffer of its own for it (lr2021.c, xfer), of which there is little.
     * With 2-byte samples (int8) that's whole samples. With 4-byte ones (int16, and the narrow mode) every other read ends halfway
     * through one: its 2 bytes wait in buf[0..1] for the next read (carry_n), so the ring gets whole samples only. */
    if (n >= 4 && !(n & 3))
        n -= 2;
    const size_t c = st.carry_n;
    dbg.reads++;
    dbg.level_sum += n;
    int64_t ta = esp_timer_get_time();
    esp_err_t e = lr2021_read_fifo(st.dev, buf + c, n);
    dbg.read_us += esp_timer_get_time() - ta;
    if (e != ESP_OK) {
        count_error();
        return false;
    }
    int64_t tc = esp_timer_get_time();
    uint8_t rx = 0, tx = 0;
    bool checked = false;
    if ((since_flags += n) >= (CONFIG_RDOF_LR2021_IRQ >= 0 ? 8192 : 1024)) {
        since_flags = 0;
        checked = lr2021_fifo_flags_peek(st.dev, &rx, &tx) == ESP_OK;
    }
    if (CONFIG_RDOF_LR2021_IRQ >= 0 && then_rearm)
        rearm();
    dbg.ctl_us += esp_timer_get_time() - tc;
    if (checked && (rx & LOSS)) {
        lost(rx & LR2021_FIFO_OVERFLOW ? "FIFO overflow flag" : "FIFO underflow flag");
        return false;
    }
    int64_t tb = esp_timer_get_time();
    const size_t have = c + n, sample = (size_t)st.sbytes, whole = have - have % sample;
    put(buf, whole);
    st.carry_n = (uint8_t)(have - whole);
    if (st.carry_n)
        memcpy(buf, buf + whole, st.carry_n);
    dbg.dsp_us += esp_timer_get_time() - tb;
    return true;
}

/* The reader: the FIFO's samples, little-endian I then Q, 16 bits each (spec R2.7) or 8 in the wide mode, into
 * the ring. With the IRQ
 * pin it sleeps until the chip's RX FIFO IRQ says IRQ_LEVEL bytes are in, then reads them with no level query:
 * a read and two commands a millisecond. Without it, or after an IRQ that didn't come, it reads by the level. */
static void reader_task(void *arg)
{
    bool by_level = CONFIG_RDOF_LR2021_IRQ < 0;
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
            by_level = true;
            continue;
        }
        lock();
        if (st.held) {
            unlock();
            continue;
        }
        if (!st.in_session) {
            unlock();
            vTaskDelay(pdMS_TO_TICKS(100));
            lock();
            /* held again during the wait: the experiment has the chip */
            if (st.streaming && !st.held && !st.in_session && session_start())
                count_error();
            unlock();
            by_level = true; /* whatever is in the new session's FIFO, then the IRQ */
            continue;
        }
        if (!by_level) {
            unlock();
            /* The pin is checked first, so an IRQ raised since the last re-arm isn't missed. 2 ticks (1 to 2 ms;
             * the rest of the FIFO fills in 1): past that, read by the level. */
            if (gpio_get_level(CONFIG_RDOF_LR2021_IRQ))
                dbg.high_now++;
            waits++;
            if (!lr2021_wait_irq(st.dev, 2)) {
                late++;
                by_level = true;
                continue;
            }
            lock();
            if (st.in_session && !drain(irq_level(), !st.wide))
                by_level = true;
            /* In the wide mode, what came in during the read can pass the level before the re-arm, and the FIFO-high
             * flag then never rises again (no new edge): read by the level until it's nearly empty. */
            if (st.wide)
                by_level = true;
            unlock();
            continue;
        }
        uint16_t level = 0;
        int64_t tc = esp_timer_get_time();
        esp_err_t e = lr2021_rx_fifo_level(st.dev, &level);
        dbg.ctl_us += esp_timer_get_time() - tc;
        if (e != ESP_OK) {
            unlock();
            count_error();
            vTaskDelay(1);
            continue;
        }
        if (level >= FIFO_BYTES) { /* it filled: samples are gone (spec R2.13) */
            lost("the FIFO filled");
            unlock();
            continue;
        }
        size_t backlog = level & ~3u;
        if (CONFIG_RDOF_LR2021_IRQ < 0 && backlog < POLL_LEVEL) {
            /* Waiting only for an empty FIFO never waited (samples come in while it's read), and the task
             * starved core 0. */
            unlock();
            waits++;
            vTaskDelay(1);
            continue;
        }
        /* With the IRQ: what's there (the drain re-arms it), then back to waiting for it once the FIFO is well under
         * the IRQ's level, so the level rising past it raises the IRQ (the SPI outruns the samples: each read
         * leaves less behind). */
        if (backlog > READ_MAX)
            backlog = READ_MAX;
        bool ok = !backlog || drain(backlog, false);
        if (CONFIG_RDOF_LR2021_IRQ >= 0 && ok && backlog < irq_level() / 2) {
            int64_t tc = esp_timer_get_time();
            rearm();
            dbg.ctl_us += esp_timer_get_time() - tc;
            by_level = false;
        }
        unlock();
    }
}

static int start(void *ctx)
{
    st.resample = fabs(st.native - st.rate) > 0.5;
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
                 (double)st.chip_num / st.chip_den, caps.usable * st.rate / 1e3);
    }
    if (!st.blk.raw) {
        const size_t need = BLOCK * 4 + sizeof(rdof_cf_t) * (2 * BLOCK + 4);
        /* in PSRAM: in internal RAM they were no faster (the resampler's taps and history are what it reads most) */
        uint8_t *p = mem_take("sdr_fe", "blocks", NULL);
        if (!p)
            p = heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        bool internal = false;
        if (!p)
            return failed("read buffers", ESP_ERR_NO_MEM);
        st.blk.x = (rdof_cf_t *)p;
        st.blk.y = st.blk.x + BLOCK;
        st.blk.raw = (uint8_t *)(st.blk.y + BLOCK + 4);
        ESP_LOGI(TAG, "read buffers: %u bytes %s", (unsigned)need, internal ? "internal" : "in PSRAM");
    }
    xStreamBufferReset(st.ring);
    memset(&st.stats, 0, sizeof st.stats);
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
         * loss every 5 s idle and several a second on a call. It works in bursts with a tick's wait between, so
         * Wi-Fi still gets core 0 most of the time. On core 1 (above the DSP) it lost far more. */
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
    session_end();
    unlock();
}

/* The ring's samples as they came from the chip, BLOCK at a time, converted to float, resampled to the settings'
 * rate and written as int16 I/Q: up to len bytes. On the DSP's core, off the reader's. The first block waits up
 * to timeout_ms; the rest are what's there. */
static size_t read(void *ctx, uint8_t *out, size_t len, int timeout_ms, size_t *filled)
{
    *filled = 0;
    uint8_t *raw = st.blk.raw;
    rdof_cf_t *x = st.blk.x, *y = st.blk.y;
    const int sb = st.sbytes;
    const rdof_fe_format_t fmt = sb == 2 ? RDOF_FE_CS8 : RDOF_FE_CS16;
    int16_t *z = (int16_t *)out;
    size_t room = len / 4, done = 0;
    TickType_t wait = pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        /* inputs whose outputs fit in what's left */
        size_t n = st.resample ? (room > 2 ? (room - 2) * (size_t)st.rs.M / (size_t)st.rs.L : 0) : room;
        if (n > BLOCK)
            n = BLOCK;
        if (!n)
            break;
        /* whole samples: the reader puts whole ones, and this asks for whole ones */
        size_t got = xStreamBufferReceive(st.ring, raw, n * (size_t)sb, wait) / (size_t)sb;
        wait = 0;
        if (!got)
            break;
        int64_t t0 = esp_timer_get_time();
        if (!st.resample && fmt == RDOF_FE_CS8) {
            /* the converter's rate as is: int8 to int16 by a shift, no floats */
            const int8_t *b = (const int8_t *)raw;
            for (size_t i = 0; i < 2 * got; i++)
                z[i] = (int16_t)(b[i] * 256);
            dbg.conv_us += esp_timer_get_time() - t0;
            z += 2 * got;
            done += got;
            room -= got;
            continue;
        }
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
            st.in_session = true; /* reset even if no session ran: a clean chip under the patch */
            session_end();        /* reset and set up: standby, the chip's own settings */
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
        /* back to streaming: the chip reset and set up (an experiment's settings and errors gone), then the
         * reader starts a new session */
        st.in_session = true;
        session_end();
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
        return lr2021_clock(st.dev);
    lock();
    esp_err_t e = lr2021_set_clock(st.dev, hz, delay_ns);
    unlock();
    return e == ESP_OK ? hz : -1;
}

void fe_lr2021(rdof_fe_t *fe) { *fe = (rdof_fe_t){&ops, &caps, NULL}; }

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
