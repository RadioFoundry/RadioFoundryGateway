// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// wifilist's list edits from JSON (cJSON), for its web API and an app's settings import.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "wifilist.h"

static bool get_str(const cJSON *o, const char *k, char *out, size_t cap, bool *present, char *err, size_t ecap)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (present) {
        *present = v != NULL;
    }
    if (!v) {
        return true;
    }
    if (!cJSON_IsString(v) || strlen(v->valuestring) >= cap) {
        snprintf(err, ecap, "%s: a string of at most %d bytes", k, (int)cap - 1);
        return false;
    }
    memcpy(out, v->valuestring, strlen(v->valuestring) + 1);
    return true;
}

esp_err_t wifilist_update_json(const char *json, size_t len, char *err, size_t ecap)
{
    struct {
        wifilist_edit_t in[WIFILIST_MAX];
        wifilist_net_t old[WIFILIST_MAX], out[WIFILIST_MAX];
    } *m = calloc(1, sizeof(*m));
    if (!m) {
        snprintf(err, ecap, "no memory");
        return ESP_ERR_NO_MEM;
    }
    wifilist_edit_t *in = m->in;
    wifilist_net_t *old = m->old, *out = m->out;
    cJSON *j = cJSON_ParseWithLength(json, len);
    esp_err_t e = ESP_ERR_INVALID_ARG;
    int n = 0;
    if (!cJSON_IsObject(j)) {
        snprintf(err, ecap, "wifi: an object {networks: [...]} or {ssid, password}");
        goto done;
    }
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(j, "networks");
    if (!list) {
        // {"ssid", "password"}: a list of one (an empty ssid: none).
        memset(&in[0], 0, sizeof(in[0]));
        if (!get_str(j, "ssid", in[0].net.ssid, sizeof(in[0].net.ssid), NULL, err, ecap) ||
            !get_str(j, "password", in[0].net.password, sizeof(in[0].net.password), &in[0].has_password, err, ecap)) {
            goto done;
        }
        n = in[0].net.ssid[0] ? 1 : 0;
    } else if (!cJSON_IsArray(list)) {
        snprintf(err, ecap, "wifi.networks: a list");
        goto done;
    } else {
        const cJSON *it;
        cJSON_ArrayForEach(it, list)
        {
            if (n == WIFILIST_MAX) {
                snprintf(err, ecap, "at most %d networks", WIFILIST_MAX);
                goto done;
            }
            wifilist_edit_t *w = &in[n];
            memset(w, 0, sizeof(*w));
            if (!cJSON_IsObject(it)) {
                snprintf(err, ecap, "wifi.networks: objects {ssid, priority, hidden, password}");
                goto done;
            }
            if (!get_str(it, "ssid", w->net.ssid, sizeof(w->net.ssid), NULL, err, ecap) ||
                !get_str(it, "password", w->net.password, sizeof(w->net.password), &w->has_password, err, ecap) ||
                !get_str(it, "was", w->was, sizeof(w->was), NULL, err, ecap)) {
                goto done;
            }
            const cJSON *p = cJSON_GetObjectItemCaseSensitive(it, "priority");
            if (p && (!cJSON_IsNumber(p) || p->valuedouble < WIFILIST_PRIORITY_MIN ||
                      p->valuedouble > WIFILIST_PRIORITY_MAX)) {
                snprintf(err, ecap, "%s: priority %d to %d", w->net.ssid, WIFILIST_PRIORITY_MIN, WIFILIST_PRIORITY_MAX);
                goto done;
            }
            w->net.priority = p ? (int8_t)p->valuedouble : 0;
            w->net.hidden = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "hidden"));
            n++;
        }
    }
    int n_old = wifilist_get(old, WIFILIST_MAX);
    n = wifilist_merge(old, n_old, in, n, out, err, ecap);
    if (n >= 0) {
        e = wifilist_set(out, n, err, ecap);
    }
done:
    cJSON_Delete(j);
    free(m);
    return e;
}
