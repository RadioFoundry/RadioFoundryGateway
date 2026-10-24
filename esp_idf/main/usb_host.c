// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The S3's one USB port as a host, for whichever roles use it: the radios
 * (CDC-ACM, usb_modem.c) and the RTL-SDR (rtlsdr). One host stack,
 * installed once; each class driver is a client of its own.
 *
 * One radio on USB that says nothing of usb.serial or usb.port: its driver
 * opens the first device with the radio's VID / PID, as it always has.
 * Otherwise (two radios, or a hub) a watcher client assigns each device to a
 * radio (rdof/usbmatch.h: VID / PID, then usb.serial or usb.port), and each
 * radio opens only its own device's address, so a replug lands on the same
 * radio. Hubs need CONFIG_USB_HOST_HUBS_SUPPORTED (the w12 and 16mb hub
 * overlays turn it on).
 *
 * A radio and an RTL-SDR both on USB without "usb_hub" (usb_host_either): the
 * watcher starts the role of what's plugged in; the other kind plugged in
 * later restarts the board, which then runs that one.
 */
#include <string.h>

#include "app.h"
#include "mem.h"

#if CONFIG_RDOF_MODEM_USB || CONFIG_RDOF_SDR

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rdof/usbmatch.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"

static const char *TAG = "usb";
static bool installed;

static void lib_task(void *arg)
{
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
            usb_host_device_free_all();
    }
}

/* A device plugged in across a restart of the S3 (VBUS comes from outside, so it never loses power) may not show up:
 * an FTDI cable sometimes didn't, until replugged. Nothing on the bus a few seconds in: turn the host's root port off
 * and on once (the S3's port state only; VBUS stays on), which has the host look at the line again. */
static void port_kick(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(3000));
    uint8_t addrs[1];
    int n = 0;
    if (usb_host_device_addr_list_fill(sizeof addrs, addrs, &n) == ESP_OK && n == 0) {
        ESP_LOGI(TAG, "nothing on USB yet: turning the host port off and on to look again");
        usb_host_lib_set_root_port_power(false);
        vTaskDelay(pdMS_TO_TICKS(200));
        usb_host_lib_set_root_port_power(true);
    }
    vTaskDelete(NULL);
}

void usb_host_start(void)
{
    if (installed)
        return;
#if !CONFIG_RDOF_USB_HOST
    static bool said;
    if (!said)
        ESP_LOGE(TAG, "USB host is built out (CONFIG_RDOF_USB_HOST): nothing on USB will be found");
    said = true;
    return;
#endif
    usb_host_config_t host = {.skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1};
    ESP_ERROR_CHECK(usb_host_install(&host));
    installed = true;
    /* above the RTL-SDR's client (18) and the DSP; on core 0 with the rest of USB */
    mem_task("usb", "usb_lib", lib_task, 4096, NULL, 19, 0, NULL); /* planned stacks (mem.c's usb role) */
    mem_task("usb", "usb_kick", port_kick, 2560, NULL, 5, 0, NULL);
    ESP_LOGI(TAG, "USB host up; what's plugged in needs 5 V on VBUS from outside the S3");
}

/* The CDC-ACM host driver, once, for whoever needs it first (the radios on USB, the bench API's radios). */
void usb_host_cdc_install(void)
{
    static bool cdc;
    if (!installed)
        return; /* built out (CONFIG_RDOF_USB_HOST) */
    if (!cdc) {
        ESP_ERROR_CHECK(cdc_acm_host_install(NULL));
        cdc = true;
    }
}

bool usb_host_is_rtl(uint16_t vid, uint16_t pid)
{
    return vid == 0x0BDA && (pid == 0x2838 || pid == 0x2832);
}

/* ── the watcher: what's plugged in, and whose it is ────────────────────── */

#if CONFIG_RDOF_MODEM_USB

static usb_host_client_handle_t watcher;
static QueueHandle_t arrived;
static SemaphoreHandle_t lock;
static void (*start_radio)(void), (*start_rtl)(void);
static char running; /* usb_host_either: 'r' or 's' once a role runs */

static rdof_usb_want_t wants[RDOF_MAX_RADIOS];
static bool wanting[RDOF_MAX_RADIOS], taken[RDOF_MAX_RADIOS];
static uint8_t addr_of[RDOF_MAX_RADIOS];
static SemaphoreHandle_t got[RDOF_MAX_RADIOS];

static void watch_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV)
        xQueueSend(arrived, &msg->new_dev.address, 0);
}

static void watch_events(void *arg)
{
    for (;;)
        usb_host_client_handle_events(watcher, portMAX_DELAY);
}

/* A UTF-16 string descriptor as ASCII ('?' for the rest). */
static void ascii(const usb_str_desc_t *d, char *out, size_t cap)
{
    size_t n = 0;
    for (int i = 0; d && i < (d->bLength - 2) / 2 && n + 1 < cap; i++)
        out[n++] = d->wData[i] < 0x80 ? (char)d->wData[i] : '?';
    out[n] = 0;
}

/* The device at addr: VID / PID, serial number, and the hub ports from the board down to it. */
static bool describe(uint8_t addr, rdof_usb_dev_t *d)
{
    usb_device_handle_t h;
    const usb_device_desc_t *dd;
    usb_device_info_t info;
    memset(d, 0, sizeof *d);
    if (usb_host_device_open(watcher, addr, &h) != ESP_OK)
        return false;
    bool ok = usb_host_get_device_descriptor(h, &dd) == ESP_OK && usb_host_device_info(h, &info) == ESP_OK;
    if (ok) {
        d->vid = dd->idVendor;
        d->pid = dd->idProduct;
        ascii(info.str_desc_serial_num, d->serial, sizeof d->serial);
        /* up the hubs: each parent's port, nearest the device first */
        uint8_t up[7];
        int n = 0;
        usb_parent_dev_info_t p = info.parent;
        while (p.dev_hdl && n < (int)sizeof up) {
            usb_device_info_t pi;
            up[n++] = p.port_num;
            /* the hub driver holds its hubs open, so their handles are good while the device is */
            if (usb_host_device_info(p.dev_hdl, &pi) != ESP_OK)
                break;
            p = pi.parent;
        }
        uint8_t down[7];
        for (int i = 0; i < n; i++)
            down[i] = up[n - 1 - i];
        rdof_usb_port_path(down, n, d->port, sizeof d->port);
    }
    usb_host_device_close(watcher, h);
    return ok;
}

/* Every device that's plugged in, looked at again (a radio started asking, or let one go). */
static void rescan(void)
{
    uint8_t addrs[16];
    int n = 0;
    if (usb_host_device_addr_list_fill(sizeof addrs, addrs, &n) == ESP_OK)
        for (int i = 0; i < n; i++)
            xQueueSend(arrived, &addrs[i], 0);
}

static void assign(uint8_t addr, const rdof_usb_dev_t *d)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < RDOF_MAX_RADIOS; i++)
        if (taken[i] && addr_of[i] == addr) {
            xSemaphoreGive(lock);
            return; /* already someone's */
        }
    bool want_now[RDOF_MAX_RADIOS];
    for (int i = 0; i < RDOF_MAX_RADIOS; i++)
        want_now[i] = taken[i] || !wanting[i];
    int k = rdof_usb_match(d, wants, RDOF_MAX_RADIOS, want_now);
    if (k >= 0) {
        taken[k] = true;
        addr_of[k] = addr;
    }
    xSemaphoreGive(lock);
    if (k >= 0) {
        ESP_LOGI(TAG, "%04X:%04X serial %s%s%s: %s", d->vid, d->pid, d->serial[0] ? d->serial : "(none)",
                 d->port[0] ? " at port " : "", d->port, g_rcfg[k]->name);
        xSemaphoreGive(got[k]);
    }
}

static void watch_task(void *arg)
{
    for (;;) {
        uint8_t addr;
        rdof_usb_dev_t d;
        xQueueReceive(arrived, &addr, portMAX_DELAY);
        if (!describe(addr, &d))
            continue;
        char k = usb_host_is_rtl(d.vid, d.pid) ? 's' : d.vid == CONFIG_RDOF_USB_VID && d.pid == CONFIG_RDOF_USB_PID ? 'r'
                                                                                                                : 0;
        if (start_radio) { /* usb_host_either */
            if (!k) {
                ESP_LOGI(TAG, "%04X:%04X is neither the radio nor an RTL-SDR", d.vid, d.pid);
                continue;
            }
            if (!running) {
                running = k;
                ESP_LOGI(TAG, "%04X:%04X: running the %s", d.vid, d.pid, k == 's' ? "SDR" : "roamer / repeater");
                (k == 's' ? start_rtl : start_radio)();
            } else if (k != running) {
                ESP_LOGW(TAG, "%04X:%04X while the %s runs: restarting to run the %s", d.vid, d.pid,
                         running == 's' ? "SDR" : "roamer / repeater", k == 's' ? "SDR" : "roamer / repeater");
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        }
        if (k == 'r')
            assign(addr, &d);
    }
}

static void watch_start(void)
{
    static bool started;
    if (started)
        return;
    started = true;
    usb_host_start();
    if (!installed)
        return; /* built out: nothing to watch (it said so) */
    lock = xSemaphoreCreateMutex();
    arrived = xQueueCreate(16, sizeof(uint8_t));
    usb_host_client_config_t cc = {.is_synchronous = false,
                                   .max_num_event_msg = 5,
                                   .async = {.client_event_callback = watch_cb, .callback_arg = NULL}};
    ESP_ERROR_CHECK(usb_host_client_register(&cc, &watcher));
    xTaskCreatePinnedToCore(watch_events, "usb_watch_ev", 3072, NULL, 15, NULL, 0);
    xTaskCreatePinnedToCore(watch_task, "usb_watch", 8192, NULL, 9, NULL, 0); /* starts a role: as app_main would */
    rescan();
}

/* The radios on USB need telling apart: more than one, or one that names its device. */
static bool matching(void)
{
    int n = 0;
    for (int k = 0; k < g_roles.nradios; k++) {
        const rdof_config_t *c = g_rcfg[k];
        if (!c || !rdof_config_uses_usb(c))
            continue;
        n++;
        if (c->usb_serial[0] || c->usb_port[0])
            return true;
    }
    return n > 1 || g_roles.usb_hub;
}

uint8_t usb_host_wait_device(int radio)
{
    if (!matching())
        return 0; /* as before: any address (CDC_HOST_ANY_DEV_ADDR), the first one there */
    watch_start();
    if (!lock)
        return 0; /* the USB host is built out */
    if (!got[radio])
        got[radio] = xSemaphoreCreateBinary();
    const rdof_config_t *c = g_rcfg[radio];
    xSemaphoreTake(lock, portMAX_DELAY);
    wants[radio] = (rdof_usb_want_t){CONFIG_RDOF_USB_VID, CONFIG_RDOF_USB_PID, c->usb_serial, c->usb_port};
    wanting[radio] = true;
    xSemaphoreGive(lock);
    rescan();
    xSemaphoreTake(got[radio], portMAX_DELAY);
    return addr_of[radio];
}

void usb_host_release(int radio)
{
    if (!lock)
        return;
    xSemaphoreTake(lock, portMAX_DELAY);
    taken[radio] = false;
    wanting[radio] = false;
    xSemaphoreGive(lock);
}

#if CONFIG_RDOF_SDR
void usb_host_either(void (*radio)(void), void (*rtl)(void))
{
    start_radio = radio;
    start_rtl = rtl;
    g_board.usb_either = true;
    ESP_LOGI(TAG, "both roles are set to use USB: plug in the radio (%04X:%04X) or an RTL-SDR", CONFIG_RDOF_USB_VID,
             CONFIG_RDOF_USB_PID);
    watch_start();
}
#else
void usb_host_either(void (*radio)(void), void (*rtl)(void)) {}
#endif

#else

uint8_t usb_host_wait_device(int radio) { return 0; }
void usb_host_release(int radio) {}
void usb_host_either(void (*radio)(void), void (*rtl)(void)) {}

#endif

#else

void usb_host_start(void) {}
void usb_host_cdc_install(void) {}
void usb_host_either(void (*radio)(void), void (*rtl)(void)) {}
bool usb_host_is_rtl(uint16_t vid, uint16_t pid) { return false; }
uint8_t usb_host_wait_device(int radio) { return 0; }
void usb_host_release(int radio) {}

#endif
