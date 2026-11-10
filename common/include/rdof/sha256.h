// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* SHA-256 (FIPS 180-4), for the BrandMeister login's RPTK (rdof/hbpeer.h). Small and portable; not constant time. */
#ifndef RDOF_SHA256_H
#define RDOF_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t h[8];
    uint64_t len; /* bytes so far */
    uint8_t buf[64];
    size_t nbuf;
} rdof_sha256_t;

void rdof_sha256_init(rdof_sha256_t *s);
void rdof_sha256_update(rdof_sha256_t *s, const void *data, size_t n);
void rdof_sha256_final(rdof_sha256_t *s, uint8_t out[32]);
void rdof_sha256(const void *data, size_t n, uint8_t out[32]);

#ifdef __cplusplus
}
#endif

#endif
