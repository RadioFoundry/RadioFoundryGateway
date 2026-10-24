// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A radio on USB as a CDC-ACM device (an OpenGD77 or other radio_hotspot
 * radio: VID 0x1FC9, PID 0x0094 by default), one per roamer that says modem:
 * usb. usb_host.c says which device is whose (by usb.serial or usb.port when
 * two of them are on a hub). The radio ignores line coding and DTR / RTS and
 * doesn't reset on open; both are set anyway (as pyserial does) and errors
 * ignored. The port is reopened whenever the radio comes back.
 *
 * The S3 doesn't power the bus: VBUS (5 V) for the radio has to come from
 * outside (the board's 5V pin, the adapter, or a powered hub).
 */
#include "app.h"

#if CONFIG_RDOF_MODEM_USB

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/cdc_acm_host.h"

static const char *TAG = "usb";

typedef struct {
    cdc_acm_dev_hdl_t dev;
    SemaphoreHandle_t gone;
} usb_radio_t;

static usb_radio_t radios[RDOF_MAX_RADIOS];

static bool usb_write(void *ctx, const uint8_t *data, size_t len)
{
    roam_t *r = ctx;
    cdc_acm_dev_hdl_t d = radios[r->i].dev;
    if (!d)
        return false;
    esp_err_t e = cdc_acm_host_data_tx_blocking(d, data, len, 200);
    if (e != ESP_OK)
        ESP_LOGW(TAG, "write: %s", esp_err_to_name(e));
    return e == ESP_OK;
}

static bool on_data(const uint8_t *data, size_t len, void *arg)
{
    modem_on_bytes(arg, data, len);
    return true;
}

static void on_event(const cdc_acm_host_dev_event_data_t *event, void *ctx)
{
    roam_t *r = ctx;
    switch (event->type) {
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "%s%sradio unplugged", r->i ? r->cfg->name : "", r->i ? ": " : "");
        modem_transport_down(r);
        xSemaphoreGive(radios[r->i].gone);
        break;
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "CDC-ACM error %d", event->data.error);
        break;
    default:
        break;
    }
}

static void open_task(void *arg)
{
    roam_t *r = arg;
    usb_radio_t *u = &radios[r->i];
    bool said = false;
    for (;;) {
        uint8_t addr = usb_host_wait_device(r->i);
        cdc_acm_host_open_config_t cfg = {
            .vid = CONFIG_RDOF_USB_VID,
            .pid = CONFIG_RDOF_USB_PID,
            .interface_idx = CONFIG_RDOF_USB_INTERFACE,
            .dev_addr = addr,
            .connection_timeout_ms = 1000,
            .out_buffer_size = 512,
            .in_buffer_size = 512,
            .event_cb = on_event,
            .data_cb = on_data,
            .user_arg = r,
        };
        cdc_acm_dev_hdl_t d;
        esp_err_t e = cdc_acm_host_open(&cfg, &d);
        if (e != ESP_OK && !addr) {
            if (!said)
                ESP_LOGI(TAG, "waiting for the radio (%04X:%04X): %s", CONFIG_RDOF_USB_VID, CONFIG_RDOF_USB_PID,
                         esp_err_to_name(e));
            said = true;
            continue; /* open waits connection_timeout_ms itself */
        }
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "radio at address %u won't open: %s", addr, esp_err_to_name(e));
            usb_host_release(r->i);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        said = false;
        ESP_LOGI(TAG, "%s%sradio connected", r->i ? r->cfg->name : "", r->i ? ": " : "");
        cdc_acm_host_desc_print(d);
        cdc_acm_line_coding_t lc = {.dwDTERate = 115200, .bCharFormat = 0, .bParityType = 0, .bDataBits = 8};
        cdc_acm_host_line_coding_set(d, &lc);
        cdc_acm_host_set_control_line_state(d, true, false);
        u->dev = d;
        xSemaphoreTake(u->gone, 0);
        modem_transport_up(r, &r->usb);
        xSemaphoreTake(u->gone, portMAX_DELAY);
        u->dev = NULL;
        cdc_acm_host_close(d);
        if (addr)
            usb_host_release(r->i);
    }
}

void usb_modem_start(roam_t *r)
{
    radios[r->i].gone = xSemaphoreCreateBinary();
    r->usb = (modem_transport_t){"usb", usb_write, r};
    usb_host_start();
    usb_host_cdc_install();
    roam_task(r, open_task, "usb_open", 4096, 9);
    ESP_LOGI(TAG, "USB host up; the radio needs 5 V on VBUS from outside the S3");
}

#else

void usb_modem_start(roam_t *r) {}

#endif
