// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// The reader: a UART's events give each chunk's arrival time (a small RX threshold keeps it within a few ms),
// lines go to the parser, and each burst's second to the callback.
#include "gnss.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_GNSS_UART_RX_GPIO >= 0

static const char *TAG = "gnss";

#define UART CONFIG_GNSS_UART_NUM
#define RX_BUF 1024
#define RX_THRESH 8            // bytes: an event this soon after a burst's first byte
#define FIND_US 3000000        // no good sentence this long: try the next pins and baud
#define CHAR_US(baud) (10e6 / (baud))

static gnss_time_cb_t s_cb;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
EXT_RAM_BSS_ATTR static gnss_status_t s_st;     // the reader task's own (PSRAM: 9600 baud needs no speed)
EXT_RAM_BSS_ATTR static gnss_status_t s_view;   // its copy for the status, under s_mux
static int64_t s_last_good_us;

static void publish(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_view = s_st;
    s_view.last_s = s_last_good_us ? (int32_t)((esp_timer_get_time() - s_last_good_us) / 1000000) : -1;
    taskEXIT_CRITICAL(&s_mux);
}

// What autodetect tries in turn: the configured pins and baud first, then the pins swapped, then other bauds.
static const int BAUDS[] = {CONFIG_GNSS_BAUD, 9600, 115200, 38400, 57600, 4800};
#define N_BAUDS (int)(sizeof(BAUDS) / sizeof(BAUDS[0]))

static void setup(int rx, int tx, int baud)
{
    uart_set_baudrate(UART, (uint32_t)baud);
    uart_set_pin(UART, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_flush_input(UART);
    s_st.rx_gpio = rx;
    s_st.tx_gpio = tx;
    s_st.baud = baud;
    s_st.found = false;
}

static void reader(void *arg)
{
    QueueHandle_t q = arg;
    EXT_RAM_BSS_ATTR static char line[GNSS_NMEA_MAX_LINE + 2];
    EXT_RAM_BSS_ATTR static uint8_t buf[RX_BUF];
    size_t len = 0;
    bool skipping = false;  // a line too long: drop it to its end
    int attempt = 0;
    int64_t since = esp_timer_get_time();
    for (;;) {
        uart_event_t e;
        const bool got = xQueueReceive(q, &e, pdMS_TO_TICKS(500)) == pdTRUE;
        const int64_t now = esp_timer_get_time();
        if (!s_st.found && now - since > FIND_US) {
#if CONFIG_GNSS_AUTODETECT
            // Next: the pins as configured or swapped, at each baud.
            attempt = (attempt + 1) % (2 * N_BAUDS);
            const bool swap = attempt % 2;
            const int rx = swap ? CONFIG_GNSS_UART_TX_GPIO : CONFIG_GNSS_UART_RX_GPIO;
            const int tx = swap ? CONFIG_GNSS_UART_RX_GPIO : CONFIG_GNSS_UART_TX_GPIO;
            if (rx >= 0) {
                setup(rx, tx, BAUDS[attempt / 2]);
                s_st.tries++;
                publish();
            }
#endif
            since = now;
            len = 0;
            continue;
        }
        if (!got) {
            continue;
        }
        if (e.type == UART_FIFO_OVF || e.type == UART_BUFFER_FULL) {
            uart_flush_input(UART);
            xQueueReset(q);
            s_st.overflows++;
            len = 0;
            continue;
        }
        if (e.type != UART_DATA || !e.size) {
            continue;
        }
        size_t buffered = 0;
        uart_get_buffered_data_len(UART, &buffered);
        // The event's bytes ended at about now, less what came since it (still buffered beyond them).
        const int n = uart_read_bytes(UART, buf, e.size < sizeof(buf) ? e.size : sizeof(buf), 0);
        if (n <= 0) {
            continue;
        }
        const int64_t end = now - (int64_t)((buffered > (size_t)n ? buffered - (size_t)n : 0) * CHAR_US(s_st.baud));
        gnss_burst_bytes(&s_st.burst, end, (size_t)n, s_st.baud);
        s_st.bytes += (uint32_t)n;
        for (int i = 0; i < n; i++) {
            const char c = (char)buf[i];
            if (c == '$') {
                len = 0;
                skipping = false;
            }
            if (skipping) {
                continue;
            }
            if (len >= sizeof(line) - 1) {
                skipping = true;
                len = 0;
                continue;
            }
            line[len++] = c;
            if (c != '\n') {
                continue;
            }
            line[len] = 0;
            int64_t start = 0, utc = 0;
            bool tell = false;
            s_st.lines++;
            const gnss_nmea_kind_t k = gnss_nmea_line(&s_st.nmea, line, len);
            if (k > GNSS_NMEA_BAD) {
                s_st.found = true;
                s_last_good_us = now;
            }
            if (k == GNSS_NMEA_RMC || k == GNSS_NMEA_GGA || k == GNSS_NMEA_ZDA) {
                tell = gnss_burst_time(&s_st.burst, &s_st.nmea, &start, &utc);
            }
            len = 0;
            if (tell && s_cb) {
                s_cb(start, utc);
            }
        }
        if (s_st.found && attempt >= 0) {
            ESP_LOGI(TAG, "NMEA on RX GPIO%d (TX GPIO%d) at %d baud", s_st.rx_gpio, s_st.tx_gpio, s_st.baud);
            attempt = -1;  // found: autodetect stops (a receiver gone quiet later stays on these settings)
        }
        if (attempt < 0 && now - s_last_good_us > FIND_US) {
            ESP_LOGW(TAG, "no NMEA for %d s: looking again", FIND_US / 1000000);
            s_st.found = false;
            since = now;
            attempt = 0;
        }
        publish();
    }
}

esp_err_t gnss_start(gnss_time_cb_t on_time)
{
    s_cb = on_time;
    gnss_nmea_init(&s_st.nmea);
    gnss_burst_init(&s_st.burst);
    const uart_config_t cfg = {
        .baud_rate = CONFIG_GNSS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    QueueHandle_t q = NULL;
    esp_err_t err = uart_driver_install(UART, RX_BUF * 2, 0, 16, &q, 0);
    if (err == ESP_OK) {
        err = uart_param_config(UART, &cfg);
    }
    if (err == ESP_OK) {
        err = uart_set_rx_full_threshold(UART, RX_THRESH);
    }
    if (err == ESP_OK) {
        err = uart_set_rx_timeout(UART, 2);  // an event 2 characters after the line goes quiet
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART%d: %s", UART, esp_err_to_name(err));
        return err;
    }
    setup(CONFIG_GNSS_UART_RX_GPIO, CONFIG_GNSS_UART_TX_GPIO, CONFIG_GNSS_BAUD);
    s_st.running = true;
    publish();
#if CONFIG_GNSS_TASK_CORE < 0
    const BaseType_t core = tskNO_AFFINITY;
#else
    const BaseType_t core = CONFIG_GNSS_TASK_CORE;
#endif
    if (xTaskCreatePinnedToCore(reader, "gnss", 3584, q, CONFIG_GNSS_TASK_PRIORITY, NULL, core) != pdPASS) {
        s_st.running = false;
        publish();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "reading NMEA on UART%d, RX GPIO%d, TX GPIO%d, %d baud%s", UART, CONFIG_GNSS_UART_RX_GPIO,
             CONFIG_GNSS_UART_TX_GPIO, CONFIG_GNSS_BAUD, CONFIG_GNSS_AUTODETECT ? " (autodetect)" : "");
    return ESP_OK;
}

void gnss_status(gnss_status_t *out)
{
    const int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_mux);
    *out = s_view;
    const int64_t last = s_last_good_us;
    taskEXIT_CRITICAL(&s_mux);
    out->last_s = last ? (int32_t)((now - last) / 1000000) : -1;
}

#else

esp_err_t gnss_start(gnss_time_cb_t on_time)
{
    (void)on_time;
    return ESP_ERR_NOT_SUPPORTED;
}

void gnss_status(gnss_status_t *out)
{
    memset(out, 0, sizeof(*out));
    gnss_nmea_init(&out->nmea);
    out->rx_gpio = out->tx_gpio = -1;
    out->last_s = -1;
}

#endif

size_t gnss_status_json(char *buf, size_t cap)
{
    EXT_RAM_BSS_ATTR static gnss_status_t s;  // about 600 bytes: not on the caller's stack (one caller at a time: the web server)
    gnss_status(&s);
    return gnss_json(&s, buf, cap);
}

size_t gnss_status_append(char *json, size_t len, size_t cap)
{
    EXT_RAM_BSS_ATTR static gnss_status_t s;  // the status publishers' tasks, one at a time
    static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    static volatile bool busy;
    taskENTER_CRITICAL(&lock);
    const bool mine = !busy;
    busy = true;
    taskEXIT_CRITICAL(&lock);
    if (!mine) {
        return len;  // another task is using it: this status goes without
    }
    gnss_status(&s);
    len = gnss_json_append(&s, json, len, cap);
    busy = false;
    return len;
}
