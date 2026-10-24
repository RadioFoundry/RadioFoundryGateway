// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A small JSON writer into a fixed buffer. Output is compact
 * (no spaces after , and :) for the values the gateway writes:
 * keys in the order written, floats rounded to 3 decimals, as the
 * shortest text that reads back the same.
 */
#ifndef RDOF_JSON_H
#define RDOF_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_JSON_DEPTH 8

typedef struct {
    char *buf;
    size_t cap, len;
    bool overflow;
    int depth;
    bool first[RDOF_JSON_DEPTH];
} rdof_json_t;

void rdof_json_init(rdof_json_t *j, char *buf, size_t cap);
/* Open an object: the top one (key NULL) or one under key. More than RDOF_JSON_DEPTH open overflows. */
void rdof_json_open(rdof_json_t *j, const char *key);
void rdof_json_close(rdof_json_t *j);
void rdof_json_str(rdof_json_t *j, const char *key, const char *value);
void rdof_json_int(rdof_json_t *j, const char *key, long long value);
void rdof_json_bool(rdof_json_t *j, const char *key, bool value);
/* A value written as given (a number already formatted). */
void rdof_json_raw(rdof_json_t *j, const char *key, const char *text);
/* value rounded to 3 decimals, as the shortest text that reads back the same */
void rdof_json_float3(rdof_json_t *j, const char *key, double value);
/* Finish: true if it all fit (the buffer is NUL terminated either way). */
bool rdof_json_done(rdof_json_t *j);

/* v rounded to 3 decimals, as the shortest text that reads back the same (".0" when whole), into out. */
void rdof_format_float3(double v, char *out, size_t cap);
/* Standard base64 with padding; out needs 4 * ((n + 2) / 3) + 1 bytes. */
void rdof_base64(const uint8_t *in, size_t n, char *out);

#ifdef __cplusplus
}
#endif

#endif
