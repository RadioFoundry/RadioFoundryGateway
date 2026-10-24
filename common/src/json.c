// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#include "rdof/json.h"

#include <stdio.h>
#include <string.h>

void rdof_json_init(rdof_json_t *j, char *buf, size_t cap)
{
    j->buf = buf;
    j->cap = cap;
    j->len = 0;
    j->overflow = cap == 0;
    j->depth = 0;
    if (cap)
        buf[0] = 0;
}

static void put(rdof_json_t *j, const char *s, size_t n)
{
    if (j->overflow)
        return;
    if (j->len + n + 1 > j->cap) {
        j->overflow = true;
        return;
    }
    memcpy(j->buf + j->len, s, n);
    j->len += n;
    j->buf[j->len] = 0;
}

static void puts_(rdof_json_t *j, const char *s)
{
    put(j, s, strlen(s));
}

/* One UTF-8 character at s: its code point and length, or U+FFFD and 1 for a byte that doesn't start a valid one. */
static unsigned utf8(const unsigned char *s, int *len)
{
    static const unsigned min[4] = {0, 0x80, 0x800, 0x10000};
    int n = s[0] >= 0xF0 ? (s[0] < 0xF5 ? 3 : -1) : s[0] >= 0xE0 ? 2 : s[0] >= 0xC2 ? 1 : -1;
    unsigned cp = n == 3 ? s[0] & 0x07u : n == 2 ? s[0] & 0x0Fu : s[0] & 0x1Fu;
    for (int k = 1; k <= n; k++) {
        if ((s[k] & 0xC0) != 0x80) {
            n = -1;
            break;
        }
        cp = cp << 6 | (s[k] & 0x3Fu);
    }
    if (n < 0 || cp < min[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) {
        *len = 1;
        return 0xFFFD;
    }
    *len = n + 1;
    return cp;
}

/* ASCII only: control characters and everything past ASCII as \uXXXX, surrogate pairs
   past U+FFFF. */
static void quoted(rdof_json_t *j, const char *s)
{
    put(j, "\"", 1);
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            char e[2] = {'\\', (char)c};
            put(j, e, 2);
            s++;
        } else if (c < 0x20 || c >= 0x80) {
            int n = 1;
            unsigned cp = c < 0x80 ? c : utf8((const unsigned char *)s, &n);
            char e[16];
            if (cp >= 0x10000)
                snprintf(e, sizeof e, "\\u%04x\\u%04x", 0xD800 + ((cp - 0x10000) >> 10), 0xDC00 + ((cp - 0x10000) & 0x3FF));
            else
                snprintf(e, sizeof e, "\\u%04x", cp);
            put(j, e, strlen(e));
            s += n;
        } else {
            put(j, (const char *)&c, 1);
            s++;
        }
    }
    put(j, "\"", 1);
}

static void key(rdof_json_t *j, const char *k)
{
    /* past RDOF_JSON_DEPTH the writer has overflowed (rdof_json_open): nothing to track */
    if (j->depth > 0 && j->depth <= RDOF_JSON_DEPTH) {
        if (!j->first[j->depth - 1])
            put(j, ",", 1);
        j->first[j->depth - 1] = false;
    }
    if (k) {
        quoted(j, k);
        put(j, ":", 1);
    }
}

void rdof_json_open(rdof_json_t *j, const char *k)
{
    key(j, k);
    put(j, "{", 1);
    if (j->depth < RDOF_JSON_DEPTH)
        j->first[j->depth] = true;
    else
        j->overflow = true; /* deeper than it can track */
    j->depth++;
}

void rdof_json_close(rdof_json_t *j)
{
    put(j, "}", 1);
    if (j->depth > 0)
        j->depth--;
}

void rdof_json_str(rdof_json_t *j, const char *k, const char *v)
{
    key(j, k);
    quoted(j, v);
}

void rdof_json_int(rdof_json_t *j, const char *k, long long v)
{
    char s[24];
    snprintf(s, sizeof s, "%lld", v);
    key(j, k);
    puts_(j, s);
}

void rdof_json_raw(rdof_json_t *j, const char *k, const char *text)
{
    key(j, k);
    puts_(j, text);
}

void rdof_json_bool(rdof_json_t *j, const char *k, bool v)
{
    key(j, k);
    puts_(j, v ? "true" : "false");
}

void rdof_format_float3(double v, char *out, size_t cap)
{
    if (!cap)
        return;
    snprintf(out, cap, "%.3f", v);
    size_t n = strlen(out);
    while (n > 2 && out[n - 1] == '0' && out[n - 2] != '.')
        out[--n] = 0;
    if (strcmp(out, "-0.0") == 0)
        snprintf(out, cap, "-0.0");
}

void rdof_json_float3(rdof_json_t *j, const char *k, double v)
{
    char s[40];
    rdof_format_float3(v, s, sizeof s);
    key(j, k);
    puts_(j, s);
}

bool rdof_json_done(rdof_json_t *j)
{
    return !j->overflow;
}

void rdof_base64(const uint8_t *in, size_t n, char *out)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n)
            v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n)
            v |= in[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = i + 1 < n ? tbl[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? tbl[v & 63] : '=';
    }
    out[o] = 0;
}
