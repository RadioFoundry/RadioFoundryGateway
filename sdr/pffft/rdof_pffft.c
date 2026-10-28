// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * pffft as an rdof_fft backend (rdof_pffft.h). pffft.c is compiled in here, unchanged.
 *
 * On 32-bit ARM it's built scalar: its NEON code ran slower than its scalar code on the PlutoSDR's Cortex-A9
 * (3200 points: 617 against 554 us). Elsewhere it uses the CPU's SIMD (SSE, NEON on 64-bit ARM), which wants its
 * buffers 16-byte aligned: a plan keeps aligned copies for callers whose buffers aren't.
 */
#if defined(__arm__) && !defined(__aarch64__) && !defined(PFFFT_SIMD_DISABLE)
#define PFFFT_SIMD_DISABLE
#endif
#include "pffft.c"

#include <stdint.h>
#include <string.h>

#include "rdof_pffft.h"

typedef struct {
    PFFFT_Setup *setup;
    pffft_direction_t dir;
    int n;
    float *in, *out, *work; /* aligned, n complex each */
} plan_t;

static void destroy(void *p_)
{
    plan_t *p = p_;
    if (!p)
        return;
    if (p->setup)
        pffft_destroy_setup(p->setup);
    pffft_aligned_free(p->in);
    pffft_aligned_free(p->out);
    pffft_aligned_free(p->work);
    free(p);
}

static void *create(int n, bool inverse)
{
    if (n < 16 || n % 16) /* pffft asserts on a complex size that isn't a multiple of 16 */
        return NULL;
    plan_t *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->n = n;
    p->dir = inverse ? PFFFT_BACKWARD : PFFFT_FORWARD;
    p->setup = pffft_new_setup(n, PFFFT_COMPLEX); /* NULL for a prime factor other than 2, 3, 5 */
    const size_t bytes = 2 * (size_t)n * sizeof(float);
    p->in = pffft_aligned_malloc(bytes);
    p->out = pffft_aligned_malloc(bytes);
    p->work = pffft_aligned_malloc(bytes);
    if (!p->setup || !p->in || !p->out || !p->work) {
        destroy(p);
        return NULL;
    }
    return p;
}

static void run(void *p_, const rdof_cf_t *in, rdof_cf_t *out)
{
    plan_t *p = p_;
    const size_t bytes = 2 * (size_t)p->n * sizeof(float);
    const float *src = (const float *)in;
    float *dst = (float *)out;
    if ((uintptr_t)in % 16) {
        memcpy(p->in, in, bytes);
        src = p->in;
    }
    if ((uintptr_t)out % 16)
        dst = p->out;
    pffft_transform_ordered(p->setup, src, dst, p->work, p->dir); /* unnormalized both ways, as rdof_fft */
    if (dst != (float *)out)
        memcpy(out, dst, bytes);
}

static const rdof_fft_backend_t backend = {create, run, destroy};

const rdof_fft_backend_t *rdof_pffft_backend(void) { return &backend; }

const char *rdof_pffft_simd(void)
{
#if defined(PFFFT_SIMD_SSE)
    return "SSE";
#elif defined(PFFFT_SIMD_NEON)
    return "NEON";
#elif defined(PFFFT_SIMD_ALTIVEC)
    return "Altivec";
#else
    return "scalar";
#endif
}
