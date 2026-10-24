// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The OLED (SSD1306 or SH1106, 128x64, I2C): the SDR's channels and calls
 * when the board runs an SDR (rdof/oled_sdr.h), else the first roamer's or
 * repeater's state (rdof/oled.h). Redrawn every refresh_s (0.5 s at least),
 * only the pages that changed sent, in one I2C write. The task and the I2C
 * driver's interrupt are on core 0: core 1 is the SDR's DSP, with no time to
 * spare, and the screen takes no lock the DSP holds.
 * Settings: the box config's oled {address, sh1106, rotate180, refresh_s, sda, scl}.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "app.h"

#if CONFIG_RDOF_OLED

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "netclock.h"
#include "rdof/oled.h"
#include "rdof/oled_sdr.h"

#if CONFIG_RDOF_OLED_BUILTIN
#define BUILTIN true /* on unless the settings say oled: false */
#else
#define BUILTIN false
#endif

static const char *TAG = "oled";
static i2c_master_dev_handle_t dev;
static rdof_oled_settings_t S;

/* Command bytes in one write: 00, then each. */
static bool cmds(const uint8_t *c, size_t n)
{
    uint8_t b[33];
    if (n > sizeof b - 1)
        return false;
    b[0] = 0x00;
    memcpy(b + 1, c, n);
    return i2c_master_transmit(dev, b, n + 1, 50) == ESP_OK;
}

/* Data bytes in one write: 40, then the pages (1025 bytes at most: 23 ms at 400 kHz). */
static bool data(const uint8_t *d, size_t n)
{
    static uint8_t b[1 + RDOF_OLED_W * RDOF_OLED_PAGES];
    if (n > sizeof b - 1)
        return false;
    b[0] = 0x40;
    memcpy(b + 1, d, n);
    return i2c_master_transmit(dev, b, n + 1, 100) == ESP_OK;
}

/* The pages that differ from what's shown: an SSD1306 takes them as one run (its horizontal addressing wraps from
   page to page), an SH1106 a page at a time. */
static bool send(const rdof_frame_buf_t *frame, const rdof_frame_buf_t *shown, bool have_shown)
{
    int first = -1, last = -1;
    for (int page = 0; page < RDOF_OLED_PAGES; page++)
        if (!have_shown || memcmp(frame->buf + page * RDOF_OLED_W, shown->buf + page * RDOF_OLED_W, RDOF_OLED_W)) {
            if (first < 0)
                first = page;
            last = page;
        }
    if (first < 0)
        return true;
    if (!S.sh1106) {
        const uint8_t c[] = {0x21, 0, RDOF_OLED_W - 1, 0x22, (uint8_t)first, (uint8_t)last};
        return cmds(c, sizeof c) && data(frame->buf + first * RDOF_OLED_W, (size_t)(last - first + 1) * RDOF_OLED_W);
    }
    for (int page = first; page <= last; page++) {
        if (have_shown && !memcmp(frame->buf + page * RDOF_OLED_W, shown->buf + page * RDOF_OLED_W, RDOF_OLED_W))
            continue;
        uint8_t c[8];
        size_t n = rdof_oled_page_cmds(true, page, c, sizeof c);
        if (!cmds(c, n) || !data(frame->buf + page * RDOF_OLED_W, RDOF_OLED_W))
            return false;
    }
    return true;
}

static void clock_text(char *out, size_t cap)
{
    bool synced = false;
    time_t now = (time_t)(netclock_utc_ns(esp_timer_get_time(), &synced) / 1000000000);
    out[0] = 0;
    if (synced) {
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(out, cap, "%H:%M:%S", &tm);
    }
}

#if CONFIG_RDOF_ROAMER
/* The first roamer's or repeater's screen. */
static void render_roamer(rdof_frame_buf_t *frame, roam_t *R, const char *clock)
{
    char rx[48] = "", tx[48] = "", state[40] = "";
    uint32_t freq = 0;
    int cc = 0;
    radio_channel_now(R, &freq, &cc);
    snprintf(state, sizeof state, "%s", R->st.radio_ready ? "ready" : (R->st.radio_error[0] ? "radio error" : "no radio"));
#if RDOF_FEATURE_TX
    if (R->cfg->mode == RDOF_CFG_ROAMER && R->txvr && R->st.radio_ready)
        snprintf(state, sizeof state, "%s%s", rdof_txvr_state_name(R->txvr),
                 rdof_txvr_scanning(R->txvr) ? " scanning" : rdof_txvr_on_idle_channel(R->txvr) ? " on idle" : "");
#endif
    if (R->cfg->mode == RDOF_CFG_REPEATER) {
        freq = R->cfg->rx_freq;
        cc = R->cfg->cc;
    }
    snprintf(rx, sizeof rx, "%s", R->st.hearing ? R->st.heard : "");
    snprintf(tx, sizeof tx, "%s", R->st.on_air ? R->st.sending : "");
    rdof_oled_status_t s = {R->cfg->name, g_board.ip, g_board.wifi_up, R->st.mqtt_up, R->st.hblink_up,
                          R->st.radio_ready, R->cfg->mode == RDOF_CFG_REPEATER, freq, R->cfg->tx_freq, cc, state, rx,
                          R->st.hearing, tx, R->st.on_air, clock};
    rdof_oled_render(frame, &s);
}
#endif

#if CONFIG_RDOF_SDR
/* The SDR's screen, with the first roamer's TX row when there is one. */
static void render_sdr(rdof_frame_buf_t *frame, const char *clock)
{
    static rdof_oled_channel_t ch[RDOF_SDR_MAX_CHANNELS];
    const sdr_status_t *st = &g_sstat;
    int n = g_scfg.nch;
    for (int c = 0; c < n; c++) {
        const rdof_sdr_cfg_channel_t *cc = &g_scfg.ch[c];
        ch[c] = (rdof_oled_channel_t){.id = cc->id, .name = cc->name, .freq = cc->freq,
                                      .in = g_rx_ready && st->in_window[c]};
    }
    /* who each channel's tracker is hearing: the receiver's channels are the ones in the window, then any taps.
       Read without rx_lock, which the DSP takes for every block: a call that starts or ends meanwhile shows on the
       next frame, and a retune (the bench API's) frees the trackers only after g_rx_ready goes false, into memory
       that stays readable, so at worst one frame is wrong. */
    const rdof_tracker_t *trackers = g_rx_ready ? g_rx.trackers : NULL;
    int nrx = trackers ? g_rx.C : 0;
    for (int k = 0; k < nrx; k++) {
        const rdof_sdr_cfg_channel_t *rc = sdr_rx_cfg_channel(k);
        if (!rc || sdr_is_tap(rc->id))
            continue;
        int c = (int)(rc - g_scfg.ch);
        for (int slot = 0; slot < 2 && c >= 0 && c < n && !ch[c].active; slot++) {
            const rdof_call_t *call = &trackers[k].slots[slot].call;
            if (call->active) {
                ch[c].active = true;
                ch[c].src = call->src;
                ch[c].dst = call->dst;
                ch[c].group = call->group;
            }
        }
    }
    rdof_oled_sdr_t s = {
        .wifi = g_board.wifi_up,
        .ap = g_board.ap_mode,
        .ip = g_board.ip,
        .mqtt = g_scfg.has_mqtt && g_scfg.mqtt_host[0] ? (st->mqtt_up ? RDOF_OLED_UP : RDOF_OLED_DOWN) : RDOF_OLED_NONE,
        .hblink = st->hb_peers > 0 ? (st->hb_up == st->hb_peers ? RDOF_OLED_UP : RDOF_OLED_DOWN) : RDOF_OLED_NONE,
        .sdr = st->rtl_open && st->receiving ? RDOF_OLED_UP : RDOF_OLED_DOWN,
        .radio = RDOF_OLED_NONE,
        .clock = clock,
        .ch = ch,
        .nch = n,
        .error = st->rtl_error,
        .receiving = st->receiving,
        .center_hz = st->center_hz,
        .sample_rate = g_scfg.sample_rate,
        .cpu_load = st->cpu_load,
    };
    wifi_ap_record_t ap;
    if (s.wifi && !s.ap && esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        s.wifi_dbm = ap.rssi;
#if CONFIG_RDOF_ROAMER
    char tx[48] = "";
    roam_t *R = g_nroam > 0 ? g_roam[0] : NULL;
    if (R) { /* both roles: a flag is up when it's up for both */
        s.radio = R->st.radio_ready ? RDOF_OLED_UP : RDOF_OLED_DOWN;
        s.mqtt = s.mqtt != RDOF_OLED_DOWN && R->st.mqtt_up ? RDOF_OLED_UP : RDOF_OLED_DOWN;
        s.hblink = s.hblink != RDOF_OLED_DOWN && R->st.hblink_up ? RDOF_OLED_UP : RDOF_OLED_DOWN;
        s.has_tx = true;
        s.tx_active = R->st.on_air;
        if (R->st.on_air) {
            snprintf(tx, sizeof tx, "%s", R->st.sending);
        } else {
            uint32_t freq = R->cfg->mode == RDOF_CFG_REPEATER ? R->cfg->tx_freq : 0;
            int cc = 0;
            if (!freq)
                radio_channel_now(R, &freq, &cc);
            if (freq)
                snprintf(tx, sizeof tx, "idle %.4f", freq / 1e6);
        }
        s.tx_line = tx;
    }
#endif
    rdof_oled_render_sdr(frame, &s);
}
#endif

/* The bus and the display, from the task on core 0: the I2C driver's interrupt goes to the core that installs it. */
static bool bus_start(int sda, int scl)
{
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "can't start I2C on SDA GPIO%d / SCL GPIO%d", sda, scl);
        return false;
    }
    i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = (uint16_t)S.address,
                              .scl_speed_hz = 400000};
    if (i2c_master_bus_add_device(bus, &dc, &dev) != ESP_OK) {
        ESP_LOGE(TAG, "can't add the display");
        return false;
    }
    ESP_LOGI(TAG, "%s at 0x%02x on SDA GPIO%d / SCL GPIO%d", S.sh1106 ? "SH1106" : "SSD1306", S.address, sda, scl);
    return true;
}

static void oled_task(void *arg)
{
    static rdof_frame_buf_t frame, shown;
    bool open = false, have_shown = false;
    int sda = S.sda >= 0 ? S.sda : CONFIG_RDOF_OLED_SDA, scl = S.scl >= 0 ? S.scl : CONFIG_RDOF_OLED_SCL;
    if (!bus_start(sda, scl))
        vTaskDelete(NULL);
    vTaskDelay(pdMS_TO_TICKS(100)); /* the panel's power settles */
    for (;;) {
        if (!open) {
            uint8_t c[32];
            size_t n = rdof_oled_init_cmds(S.rotate180, 0x7F, c, sizeof c);
            open = cmds(c, n);
            have_shown = false;
            if (!open) {
                ESP_LOGW(TAG, "no display at 0x%02x; retrying", S.address);
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
        }
        char clock[16];
        clock_text(clock, sizeof clock);
        bool drawn = false;
#if CONFIG_RDOF_SDR
        if (g_roles.has_sdr && g_sstat.started) {
            render_sdr(&frame, clock);
            drawn = true;
        }
#endif
#if CONFIG_RDOF_ROAMER
        if (!drawn && g_nroam > 0) {
            render_roamer(&frame, g_roam[0], clock);
            drawn = true;
        }
#endif
        if (!drawn) { /* nothing started yet (one USB port: whichever is plugged in decides) */
            rdof_oled_clear(&frame);
            rdof_oled_text(&frame, 0, 0, g_board.ip[0] ? g_board.ip : "no address", false, false);
            rdof_oled_text(&frame, 0, 20, "waiting for a device", false, false);
        }
        open = send(&frame, &shown, have_shown);
        if (open) {
            shown = frame;
            have_shown = true;
        } else {
            ESP_LOGW(TAG, "display write failed; reopening");
        }
        int ms = (int)(S.refresh_s * 1000);
        vTaskDelay(pdMS_TO_TICKS(ms > 500 ? ms : 500));
    }
}

void oled_start(void)
{
    char *text = settings_text();
    if (!text || !rdof_oled_settings_parse(text, strlen(text), &S, NULL, 0))
        rdof_oled_settings_parse("{}", 2, &S, NULL, 0); /* settings that don't parse: the board's defaults */
    free(text);
    if (!(S.set ? S.on : BUILTIN) || (!g_roles.has_sdr && !g_roles.has_radio))
        return;
#if CONFIG_RDOF_OLED_POWER_GPIO >= 0
    gpio_set_level(CONFIG_RDOF_OLED_POWER_GPIO, CONFIG_RDOF_OLED_POWER_LEVEL);
    gpio_set_direction(CONFIG_RDOF_OLED_POWER_GPIO, GPIO_MODE_OUTPUT);
#endif
    /* core 0, low priority: below the LR2021's reader (24), the DSP helper (10) and the network */
    xTaskCreatePinnedToCore(oled_task, "oled", 4096, NULL, 3, NULL, 0);
}

#else

void oled_start(void) {}

#endif
