// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The settings: one box config (JSON; rdof/config.h, docs/spec/fw/config.md)
 * in NVS ("rd" / "config"), plus the board's Wi-Fi networks and NTP servers,
 * which the wifilist and netclock components keep in their own NVS
 * namespaces. In the JSON the page edits, exports and imports, those are the
 * "wifi" and "ntp" keys: added on the way out (without passwords), taken out
 * and handed to the components on the way in. Settings from before the
 * components had "wifi": {ssid, password} and "ntp": "server" in the stored
 * text; they move over once. Settings from the SDR-only firmware (a plain SDR
 * config, in "rd" / "sdr") are read as they are and move to "config" on the
 * next save.
 *
 * Which radios run comes from "run", else the first roamer or repeater, else
 * the first SDR (rdof_box_roles); each is resolved as rdof/config.h says.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "netclock.h"
#include "nvs.h"
#include "rdof/ipgw.h"
#include "rdof/oled_sdr.h"
#include "rdof/jsonp.h"
#if CONFIG_RDOF_SDR
#include "sdr_fe.h"
#endif
#include "wifilist.h"

static const char *TAG = "settings";

rdof_roles_t g_roles;
bool g_cfg_ok;
char g_cfg_error[160];
#if CONFIG_RDOF_ROAMER
rdof_config_t *g_rcfg[RDOF_MAX_RADIOS];
#endif
#if CONFIG_RDOF_SDR
rdof_sdr_config_t g_scfg;
#endif

/* What runs here by default, by what the build has. */
#if CONFIG_RDOF_ROAMER && CONFIG_RDOF_SDR
#define DEFAULT_RUN "[\"ROAM1\", \"s3-sdr\"]"
#elif CONFIG_RDOF_ROAMER
#define DEFAULT_RUN "\"ROAM1\""
#else
#define DEFAULT_RUN "\"s3-sdr\""
#endif

/* What a new board runs until it's set up (the softAP comes up: no Wi-Fi): a placeholder roamer (an OpenGD77 on
   USB) and an SDR on five 461 MHz channels at 288 kS/s; with both on USB, whichever is plugged in runs. */
static const char DEFAULT_JSON[] =
    "{\n"
    "  // RadioFoundry Gateway settings: a box config in JSON (docs/spec/fw/config.md). run: the radios this board runs\n"
    "  // (one roamer or repeater and one SDR at most; both on USB: whichever is plugged in). Plus modem (usb /\n"
    "  // uart), uart {tx, rx} for the roamer, oled {sda, scl} for the screen; usb {transfers, transfer_kb}, dsp_cores, idle_gate\n"
    "  // and learn_ppm in the SDR's section. The page adds wifi {networks} and ntp {servers, poll_s}, which the\n"
    "  // board keeps apart.\n"
    "  \"name\": \"esp32\",\n"
    "  \"mqtt\": {\"host\": \"192.0.2.115\", \"port\": 1883, \"username\": \"radios\", \"password\": \"\"},\n"
    "  \"hblink\": {\"host\": \"192.0.2.115\", \"port\": 62031},\n"
    "  \"cc\": 2,\n"
    "  \"channels\": {\"1\": {\"freq\": 461.6875, \"name\": \"Ch 1\"},\n"
    "               \"13\": {\"freq\": 461.0375, \"name\": \"Area 4\", \"tg\": 213},\n"
    "               \"4\": {\"freq\": 461.0875, \"name\": \"Service Bravo\", \"tg\": 204},\n"
    "               \"6\": {\"freq\": 461.1375, \"name\": \"Operations 3\", \"tg\": 206},\n"
    "               \"8\": {\"freq\": 461.1875, \"name\": \"Service Desk\", \"tg\": 208},\n"
    "               \"10\": {\"freq\": 461.2375, \"name\": \"Area 1\", \"tg\": 210}},\n"
    "  \"run\": " DEFAULT_RUN ",\n"
    "  \"radios\": [\n"
    "    {\"type\": \"radio_hotspot\", \"name\": \"ROAM1\", \"peer_id\": 9990301, \"callsign\": \"RT01\",\n"
    "     \"idle\": 1, \"channels\": \"all\"},\n"
    "    {\"type\": \"sdr\", \"name\": \"s3-sdr\", \"device\": {\"sample_rate\": 288000, \"gain\": 30},\n"
    "     \"hblink\": false, \"peer_ids\": 9990100, \"channels\": [13, 4, 6, 8, 10]}\n"
    "  ]\n"
    "}\n";

static void *big_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

char *settings_text(void)
{
    nvs_handle_t h;
    size_t len = 0;
    char *text = NULL;
    if (nvs_open("rd", NVS_READONLY, &h) == ESP_OK) {
        static const char *const keys[] = {"config", "sdr"}; /* "sdr": the SDR-only firmware's */
        for (int k = 0; k < 2 && !text; k++) {
            len = 0;
            if (nvs_get_blob(h, keys[k], NULL, &len) == ESP_OK && len > 0 && (text = big_alloc(len + 1))) {
                if (nvs_get_blob(h, keys[k], text, &len) != ESP_OK) {
                    free(text);
                    text = NULL;
                } else {
                    text[len] = 0;
                }
            }
        }
        nvs_close(h);
    }
    if (!text)
        text = strdup(DEFAULT_JSON);
    return text;
}

static bool store(const char *json, size_t len, char *error, size_t cap)
{
    nvs_handle_t h;
    if (nvs_open("rd", NVS_READWRITE, &h) != ESP_OK) {
        snprintf(error, cap, "can't open NVS");
        return false;
    }
    esp_err_t e = nvs_set_blob(h, "config", json, len);
    if (e == ESP_OK)
        nvs_erase_key(h, "sdr"); /* the SDR-only firmware's settings, now in "config" */
    if (e == ESP_OK)
        e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) {
        snprintf(error, cap, "NVS: %s", esp_err_to_name(e));
        return false;
    }
    return true;
}

/*
 * The "wifi" and "ntp" keys of text, to the components. import: a settings
 * import or save (the networks given replace the list; a network without a
 * password keeps the one stored for its name). Otherwise the stored text from
 * before the components: moved only if the component has nothing stored yet.
 * Sets *had when either key was there.
 */
static bool take_board_keys(const char *text, size_t len, bool import, bool *had, char *error, size_t cap)
{
    *had = false;
    char *clean = big_alloc(len + 1);
    if (!clean) {
        snprintf(error, cap, "no memory");
        return false;
    }
    size_t n = rdof_jsonc_strip(text, len, clean);
    cJSON *j = cJSON_ParseWithLength(clean, n);
    free(clean);
    if (!j)
        return true; /* resolve() says what's wrong with it */
    bool ok = true;
    const cJSON *ntp = cJSON_GetObjectItemCaseSensitive(j, "ntp");
    const cJSON *wifi = cJSON_GetObjectItemCaseSensitive(j, "wifi");
    *had = ntp || wifi;
    if (ntp && (import || !netclock_settings_stored())) {
        char *s = cJSON_PrintUnformatted(ntp);
        ok = s && netclock_settings_from_json(s, strlen(s), error, cap) == ESP_OK;
        cJSON_free(s);
    }
    if (ok && wifi && (import || !wifilist_stored())) {
        char *s = cJSON_PrintUnformatted(wifi);
        ok = s && wifilist_update_json(s, strlen(s), error, cap) == ESP_OK;
        cJSON_free(s);
        if (ok && !import)
            ESP_LOGI(TAG, "moved the Wi-Fi network from the settings to the network list");
    }
    cJSON_Delete(j);
    return ok;
}

static size_t drop_board_keys(char *text, size_t len)
{
    len = rdof_jsonc_drop(text, len, "wifi");
    return rdof_jsonc_drop(text, len, "ntp");
}

char *settings_export(void)
{
    char *text = settings_text();
    if (!text)
        return NULL;
    char wifi[1536], ntp[NETCLOCK_SERVERS_LEN + 64];
    size_t wl = wifilist_export_json(wifi, sizeof wifi), nl = netclock_settings_json(ntp, sizeof ntp);
    size_t len = strlen(text), cap = len + wl + nl + 64;
    char *a = big_alloc(cap), *b = big_alloc(cap);
    if (!a || !b || !wl || !nl || !rdof_jsonc_add(text, len, "wifi", wifi, a, cap) ||
        !rdof_jsonc_add(a, strlen(a), "ntp", ntp, b, cap)) {
        free(a);
        free(b);
        return text; /* as stored */
    }
    free(text);
    free(a);
    return b;
}

/*
 * Which radios run and each one's settings, from text: into the globals, or (check) into scratch copies, so what
 * runs keeps its settings until the restart. false with error set when it doesn't parse, or asks for a role this
 * build doesn't have.
 */
static bool resolve(const char *text, size_t len, bool check, rdof_roles_t *roles, char *error, size_t cap)
{
    if (!rdof_box_roles(text, len, roles, error, cap))
        return false;
    rdof_oled_settings_t oled; /* the screen's, whatever radios run (oled_task.c) */
    if (!rdof_oled_settings_parse(text, len, &oled, error, cap))
        return false;
    /* the IP gateway's (ipgw_task.c): whatever radios run; its tx must be one that does */
    rdof_ipgw_config_t *gw = malloc(sizeof *gw);
    if (!gw) {
        snprintf(error, cap, "no memory");
        return false;
    }
    bool gw_ok = rdof_ipgw_settings_parse(text, len, gw, error, cap);
    if (gw_ok && gw->on) {
#if CONFIG_RDOF_IPGW
        bool found = !gw->tx[0];
        for (int k = 0; k < roles->nradios && !found; k++)
            found = !strcmp(roles->radios[k], gw->tx);
        if (!found) {
            snprintf(error, cap, "ip_gateway: tx %s isn't a roamer or repeater in run", gw->tx);
            gw_ok = false;
        }
#else
        snprintf(error, cap, "ip_gateway: this build has no IP gateway (CONFIG_RDOF_IPGW)");
        gw_ok = false;
#endif
    }
    free(gw);
    if (!gw_ok)
        return false;
    /* an sdr_transceiver's needs, checked against the SDR's front end below */
    struct {
        rdof_sdr_duplex_t duplex;
        char name[24];
        uint32_t f[RDOF_CFG_CHANNELS + 1], tx;
        int nf;
    } tx = {0};
    if (roles->has_radio) {
#if CONFIG_RDOF_ROAMER
        /* each radio "run" names, parsed by name; then their ports, which they can't share */
        rdof_config_t *cfgs[RDOF_MAX_RADIOS] = {0};
        bool ok = true;
        for (int k = 0; ok && k < roles->nradios; k++) {
            cfgs[k] = check || !g_rcfg[k] ? big_alloc(sizeof *cfgs[k]) : g_rcfg[k];
            if (!cfgs[k]) {
                snprintf(error, cap, "no memory");
                ok = false;
            } else {
                ok = rdof_config_parse_radio(text, len, roles->radios[k], cfgs[k], error, cap);
            }
        }
        if (ok)
            ok = rdof_config_check_ports((const rdof_config_t *const *)cfgs, roles->nradios, error, cap);
        for (int k = 0; ok && k < roles->nradios; k++) {
            const rdof_config_t *c = cfgs[k];
            if (c->sdr_duplex == RDOF_SDR_DUPLEX_NONE)
                continue;
            tx.duplex = c->sdr_duplex;
            snprintf(tx.name, sizeof tx.name, "%s", c->name);
            if (c->mode == RDOF_CFG_REPEATER) {
                tx.f[tx.nf++] = c->rx_freq;
                tx.tx = c->tx_freq;
            } else {
                for (int i = 0; i < c->nchannels; i++)
                    tx.f[tx.nf++] = c->channels[i].freq;
                tx.f[tx.nf++] = c->idle.freq;
            }
        }
        /* a second radio's tasks and buffers: PSRAM it must have, and some internal RAM (docs/reference.md, "More
           than one radio") */
        if (ok && roles->nradios > 1 && !heap_caps_get_total_size(MALLOC_CAP_SPIRAM)) {
            snprintf(error, cap, "run names %d roamers or repeaters: a second needs PSRAM, and this board has none",
                     roles->nradios);
            ok = false;
        }
        for (int k = 0; k < roles->nradios; k++) {
            if (!check && ok)
                g_rcfg[k] = cfgs[k];
            else if (check || cfgs[k] != g_rcfg[k])
                free(cfgs[k]); /* a scratch copy, or a new one that didn't parse */
        }
        if (!ok)
            return false;
#else
        snprintf(error, cap, "the settings run %s, a roamer or repeater: this build has none (CONFIG_RDOF_ROAMER); "
                 "say what runs with \"run\"", roles->radio);
        return false;
#endif
    }
    if (roles->has_sdr) {
#if CONFIG_RDOF_SDR
        rdof_sdr_config_t *scratch = check ? big_alloc(sizeof *scratch) : NULL;
        rdof_sdr_config_t *sc = check ? scratch : &g_scfg;
        bool ok = (!check || scratch) && rdof_sdr_config_parse(text, len, sc, error, cap);
        if (check && !scratch)
            snprintf(error, cap, "no memory");
        /* the front end: in this build, and able to take the rate and the channels (the RTL-SDR's are checked
           when it tunes, as before) */
        rdof_fe_t fe;
        const char *why;
        if (ok && !sdr_fe_pick(sc->frontend, &fe, &why)) {
            snprintf(error, cap, "device.type %s: %s", rdof_fe_type_name(sc->frontend), why);
            ok = false;
        }
        if (ok && sc->frontend != RDOF_FE_RTLSDR) {
            double f[RDOF_SDR_MAX_CHANNELS];
            for (int i = 0; i < sc->nch; i++)
                f[i] = sc->ch[i].freq;
            ok = rdof_fe_check(fe.caps, sc->sample_rate, f, sc->nch, error, cap) == 0;
        }
        /* an sdr_transceiver: a front end that can transmit (full duplex for a repeater), on the SDR's channels */
        if (ok && tx.duplex) {
            if (fe.caps->duplex == RDOF_FE_RX_ONLY || fe.caps->tx_mode == RDOF_FE_TX_NONE || !fe.ops->tx_set) {
                snprintf(error, cap, "%s is an sdr_transceiver, but the SDR's front end (%s) can't transmit (yet)",
                         tx.name, fe.caps->kind);
                ok = false;
            } else if (tx.duplex == RDOF_SDR_DUPLEX_FULL && fe.caps->duplex != RDOF_FE_FULL_DUPLEX) {
                snprintf(error, cap, "%s has duplex: full, but the SDR's front end (%s) is half duplex: one chain, "
                         "switched", tx.name, fe.caps->kind);
                ok = false;
            }
            for (int i = 0; ok && i < tx.nf; i++) {
                bool heard = false;
                for (int c = 0; c < sc->nch; c++)
                    heard |= fabs(sc->ch[c].freq - tx.f[i]) <= 100;
                if (!heard) {
                    snprintf(error, cap, "%s: %.5f MHz isn't one of the SDR's channels: the SDR hears for it", tx.name,
                             tx.f[i] / 1e6);
                    ok = false;
                }
                const double t = tx.tx ? tx.tx : tx.f[i];
                if (ok && (t < fe.caps->tx_freq_min || t > fe.caps->tx_freq_max)) {
                    snprintf(error, cap, "%s: the %s can't transmit on %.5f MHz", tx.name, fe.caps->kind, t / 1e6);
                    ok = false;
                }
            }
        }
        free(scratch);
        if (!ok)
            return false;
#else
        snprintf(error, cap, "the settings run %s, an SDR: this build has none (CONFIG_RDOF_SDR)", roles->sdr);
        return false;
#endif
    }
    if (tx.duplex && !roles->has_sdr) {
        snprintf(error, cap, "%s is an sdr_transceiver: \"run\" must name the SDR too", tx.name);
        return false;
    }
    return true;
}

void settings_load(void)
{
    char *text = settings_text();
    char error[128];
    bool had = false;
    if (!take_board_keys(text, strlen(text), false, &had, error, sizeof error))
        ESP_LOGW(TAG, "the old wifi / ntp settings didn't move over: %s", error);
    if (had) {
        size_t len = drop_board_keys(text, strlen(text));
        if (store(text, len, error, sizeof error))
            ESP_LOGI(TAG, "wifi and ntp are kept apart from the settings now");
    }
    g_cfg_ok = resolve(text, strlen(text), false, &g_roles, g_cfg_error, sizeof g_cfg_error);
    if (!g_cfg_ok) {
        ESP_LOGE(TAG, "the stored settings don't parse (%s): running the defaults", g_cfg_error);
        resolve(DEFAULT_JSON, strlen(DEFAULT_JSON), false, &g_roles, error, sizeof error);
    }
    free(text);
    ESP_LOGI(TAG, "%s: %s%s%s", g_roles.box, g_roles.has_radio ? g_roles.radio : "",
             g_roles.has_radio && g_roles.has_sdr ? " and " : "", g_roles.has_sdr ? g_roles.sdr : "");
}

/* A one-shot request kept across a restart (the self-test and spike buttons). */
void settings_set_flag(const char *key)
{
    nvs_handle_t h;
    if (nvs_open("rd", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, 1);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool settings_take_flag(const char *key)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("rd", NVS_READWRITE, &h) != ESP_OK)
        return false;
    if (nvs_get_u8(h, key, &v) == ESP_OK && v) {
        nvs_erase_key(h, key);
        nvs_commit(h);
    }
    nvs_close(h);
    return v != 0;
}

bool settings_save(char *json, size_t len, char *error, size_t cap)
{
    static rdof_roles_t roles;
    if (!resolve(json, len, true, &roles, error, cap))
        return false;
    bool had;
    if (!take_board_keys(json, len, true, &had, error, cap))
        return false;
    len = drop_board_keys(json, len);
    if (!store(json, len, error, cap))
        return false;
    ESP_LOGI(TAG, "saved (%u bytes)", (unsigned)len);
    return true;
}
