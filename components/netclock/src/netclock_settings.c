// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// netclock's settings as JSON (cJSON), for its web API and an app's settings import / export.
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "netclock.h"

size_t netclock_settings_json(char *buf, size_t cap)
{
    netclock_settings_t s;
    netclock_get_settings(&s);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "servers", s.servers);
    cJSON_AddNumberToObject(j, "poll_s", s.poll_s);
    bool ok = cJSON_PrintPreallocated(j, buf, (int)cap, false);
    cJSON_Delete(j);
    return ok ? strlen(buf) : 0;
}

esp_err_t netclock_settings_from_json(const char *json, size_t len, char *err, size_t cap)
{
    cJSON *j = cJSON_ParseWithLength(json, len);
    netclock_settings_t s;
    netclock_get_settings(&s);
    const cJSON *servers = cJSON_IsString(j) ? j : cJSON_GetObjectItemCaseSensitive(j, "servers");
    const cJSON *poll = cJSON_GetObjectItemCaseSensitive(j, "poll_s");
    esp_err_t e = ESP_ERR_INVALID_ARG;
    if (!cJSON_IsObject(j) && !cJSON_IsString(j)) {
        snprintf(err, cap, "clock settings: an object {servers, poll_s} or a string of servers");
    } else if (servers && !cJSON_IsString(servers)) {
        snprintf(err, cap, "servers: a string");
    } else if (poll && !cJSON_IsNumber(poll)) {
        snprintf(err, cap, "poll_s: a number");
    } else {
        if (servers) {
            if (strlen(servers->valuestring) >= sizeof(s.servers)) {
                snprintf(err, cap, "servers: at most %d characters", (int)sizeof(s.servers) - 1);
                cJSON_Delete(j);
                return ESP_ERR_INVALID_ARG;
            }
            snprintf(s.servers, sizeof(s.servers), "%s", servers->valuestring);
        }
        if (poll) {
            s.poll_s = poll->valuedouble < 0 ? 1 : (uint32_t)poll->valuedouble;
        }
        e = netclock_set_settings(&s, err, cap);
    }
    cJSON_Delete(j);
    return e;
}
