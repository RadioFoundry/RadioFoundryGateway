// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* Shared bits for sdr sources. */
#ifndef RDOF_SDR_PRIVATE_H
#define RDOF_SDR_PRIVATE_H

#include <stddef.h>

#define RDOF_PI 3.14159265358979323846

/* Every allocation goes through these (rdof_sdr_set_allocator), so a firmware can put DSP state in fast RAM. */
void *rdof_sdr_alloc(size_t n);
void *rdof_sdr_alloc_hot(size_t n); /* rdof_sdr_set_hot_allocator */
void *rdof_sdr_zalloc(size_t n);
void rdof_sdr_free(void *p);

#endif
