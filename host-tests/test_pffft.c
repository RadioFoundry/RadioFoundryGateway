// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * pffft as the FFT backend (sdr/pffft/rdof_pffft.h) against the built-in FFT: the same output, forward and
 * inverse, at the channelizer's sizes, with aligned and unaligned buffers; the sizes it can't do fall back.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/sdr/fft.h"
#include "rdof_pffft.h"

static int failures;
#define CHECK(c)                                                                                                  \
    do {                                                                                                          \
        if (!(c)) {                                                                                               \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            failures++;                                                                                           \
        }                                                                                                         \
    } while (0)

/* the largest difference against the built-in FFT, relative to the output's peak */
static double compare(int n, bool inverse, int skew)
{
    rdof_cf_t *in = malloc((size_t)(n + 1) * sizeof *in), *ref = malloc((size_t)n * sizeof *ref);
    rdof_cf_t *buf = malloc((size_t)(n + 1) * sizeof *buf);
    rdof_cf_t *out = buf + skew; /* skew 1: 8 bytes off, so not 16-aligned */
    unsigned seed = 12345u + (unsigned)n;
    for (int i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        in[skew + i].re = (float)((seed >> 8) % 2001) / 1000.0f - 1.0f;
        seed = seed * 1103515245u + 12345u;
        in[skew + i].im = (float)((seed >> 8) % 2001) / 1000.0f - 1.0f;
    }
    rdof_fft_set_backend(NULL);
    rdof_fft_t *b = rdof_fft_new(n, inverse);
    rdof_fft_run(b, in + skew, ref);
    rdof_fft_free(b);
    rdof_fft_set_backend(rdof_pffft_backend());
    rdof_fft_t *p = rdof_fft_new(n, inverse);
    rdof_fft_run(p, in + skew, out);
    rdof_fft_free(p);
    rdof_fft_set_backend(NULL);
    double peak = 0, err = 0;
    for (int i = 0; i < n; i++) {
        peak = fmax(peak, hypot(ref[i].re, ref[i].im));
        err = fmax(err, hypot(ref[i].re - out[i].re, ref[i].im - out[i].im));
    }
    free(in);
    free(ref);
    free(buf);
    return err / peak;
}

int main(void)
{
    static const int sizes[] = {16, 64, 384, 512, 1024, 1536, 2048, 3200, 4096};
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++)
        for (int inv = 0; inv < 2; inv++)
            for (int skew = 0; skew < 2; skew++) {
                const double e = compare(sizes[k], inv, skew);
                if (!(e < 1e-5))
                    fprintf(stderr, "n %d %s skew %d: %.2g\n", sizes[k], inv ? "inverse" : "forward", skew, e);
                CHECK(e < 1e-5);
            }
    /* sizes pffft can't do: not a multiple of 16, a factor of 7; the built-in FFT takes them */
    const rdof_fft_backend_t *be = rdof_pffft_backend();
    CHECK(be->create(24, false) == NULL);
    CHECK(be->create(16 * 7, false) == NULL);
    CHECK(compare(24, false, 0) < 1e-5);
    printf("pffft (%s): %s\n", rdof_pffft_simd(), failures ? "FAILED" : "ok");
    return failures != 0;
}
