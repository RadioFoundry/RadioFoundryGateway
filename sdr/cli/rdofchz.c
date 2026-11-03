// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * rdofchz: bring-up of the FPGA channelizer on a PlutoSDR (chzhw.h): probe the block, start it, read its frames
 * from the RX DMA and report them. Not firmware: Linux, /dev/mem and libiio. The AD9361 (rate, LO, gain) is left
 * as it is: set it first, with iio_attr.
 *
 *   rdofchz [--addr 0x7C480000] --probe
 *   rdofchz [--addr A] [--uri URI] --fs HZ --center HZ [--cic R] [--decim D] --ch MHZ ... [--seconds S]
 *           [--out FILE] [--keep]
 *
 * --probe prints ID and CAPS and stops. Otherwise it designs the block's configuration (rdof_chzfx_design, or
 * rdof_chzfx_design_cic with --cic), starts it, reads S seconds of frames (default 5) and prints a line a second:
 * frames, lost output instants, junk words, STATUS, and each channel's level in dBFS. --out writes the channels'
 * samples, cs16 with the channels interleaved. --keep leaves the block on at exit (the default stops it: the
 * stock path to the DMA again).
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "rdof/sdr/channelizer.h"
#include "rdof/sdr/chzfx.h"
#include "rdof/sdr/chzhw.h"

#ifdef RDOF_IIO
#include <iio.h>
#endif

static volatile sig_atomic_t g_stop;

static void on_signal(int s)
{
    (void)s;
    g_stop = 1;
}

static uint32_t mem_rd(void *ctx, uint32_t off)
{
    return *(volatile uint32_t *)((uint8_t *)ctx + off);
}

static void mem_wr(void *ctx, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)((uint8_t *)ctx + off) = v;
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

#ifdef RDOF_IIO
typedef struct {
    int nch;
    double pow[RDOF_CHZFX_MAX_CH];
    long n;
    FILE *out;
} stats_t;

static void on_ch(void *ctx, uint32_t seq, const rdof_iq16_t *x, int n)
{
    stats_t *s = ctx;
    (void)seq;
    for (int c = 0; c < n && c < s->nch; c++)
        s->pow[c] += (double)x[c].i * x[c].i + (double)x[c].q * x[c].q;
    s->n++;
    if (s->out)
        fwrite(x, sizeof *x, (size_t)n, s->out);
}
#endif

static void usage(void)
{
    fprintf(stderr, "usage: rdofchz [--addr A] --probe\n"
                    "       rdofchz [--addr A] [--uri URI] --fs HZ --center HZ [--cic R] [--decim D] --ch MHZ ...\n"
                    "               [--seconds S] [--out FILE] [--keep]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    unsigned long addr = 0x7C480000ul;
    const char *uri = NULL, *out_path = NULL;
    double fs = 0, center = 0, seconds = 5;
    double mhz[RDOF_CHZFX_MAX_CH];
    int nch = 0, cic = 1, decim = 0, probe_only = 0, keep = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--probe")) {
            probe_only = 1;
        } else if (!strcmp(a, "--keep")) {
            keep = 1;
        } else if (!v) {
            usage();
        } else if (!strcmp(a, "--addr")) {
            addr = strtoul(v, NULL, 0), i++;
        } else if (!strcmp(a, "--uri")) {
            uri = v, i++;
        } else if (!strcmp(a, "--fs")) {
            fs = atof(v), i++;
        } else if (!strcmp(a, "--center")) {
            center = atof(v), i++;
        } else if (!strcmp(a, "--cic")) {
            cic = atoi(v), i++;
        } else if (!strcmp(a, "--decim")) {
            decim = atoi(v), i++;
        } else if (!strcmp(a, "--seconds")) {
            seconds = atof(v), i++;
        } else if (!strcmp(a, "--out")) {
            out_path = v, i++;
        } else if (!strcmp(a, "--ch") && nch < RDOF_CHZFX_MAX_CH) {
            mhz[nch++] = atof(v), i++;
        } else {
            usage();
        }
    }
    if (!probe_only && (fs <= 0 || center <= 0 || !nch))
        usage();

    const int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("/dev/mem");
        return 1;
    }
    void *regs = mmap(NULL, RDOF_CHZHW_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)addr);
    if (regs == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    const rdof_chzhw_io_t io = {mem_rd, mem_wr, regs};
    rdof_chzhw_t h;
    char err[160];
    if (rdof_chzhw_probe(&h, &io, err, sizeof err)) {
        fprintf(stderr, "probe: %s\n", err);
        return 1;
    }
    printf("channelizer at 0x%08lx: %d channels, %d lanes, D up to %d, %d taps, history %d, CIC R up to %d\n", addr,
           h.channels, h.lanes, h.max_decim, h.max_taps, h.hist, h.max_cic);
    if (probe_only)
        return 0;

    /* the output rate is 48 kS/s: D from the rate when not given */
    const int r = cic > 1 ? cic : 1;
    if (!decim)
        decim = (int)lround(fs / (48000.0 * r));
    double off[RDOF_CHZFX_MAX_CH];
    for (int c = 0; c < nch; c++)
        off[c] = mhz[c] * 1e6 - center;
    static rdof_chzfx_cfg_t cfg;
    if (rdof_chzfx_design_cic(&cfg, fs, r, decim, off, nch, RDOF_CHZ_CUTOFF_HZ)) {
        fprintf(stderr, "no configuration for %d channels, R %d, D %d at %.0f S/s\n", nch, r, decim, fs);
        return 1;
    }
    if (rdof_chzhw_start(&h, &cfg, 0, 0, err, sizeof err)) {
        fprintf(stderr, "start: %s\n", err);
        return 1;
    }
    printf("started: %d channels, R %d, D %d, %d taps, out at %.1f S/s\n", nch, r, decim, cfg.taps,
           fs / (r * decim));
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    int rc = 0;
#ifdef RDOF_IIO
    stats_t st = {.nch = nch};
    if (out_path && !(st.out = fopen(out_path, "wb"))) {
        perror(out_path);
        rc = 1;
        goto stop;
    }
    static rdof_chzhw_rx_t rx;
    rdof_chzhw_rx_init(&rx, nch, on_ch, NULL, &st);
    struct iio_context *ctx = uri ? iio_create_context_from_uri(uri) : iio_create_default_context();
    struct iio_device *dev = ctx ? iio_context_find_device(ctx, "cf-ad9361-lpc") : NULL;
    if (!dev) {
        fprintf(stderr, "no cf-ad9361-lpc\n");
        rc = 1;
        goto stop;
    }
    /* every scan element on: one DMA sample is then a whole 64-bit frame word on a 2r2t design */
    for (unsigned i = 0; i < iio_device_get_channels_count(dev); i++) {
        struct iio_channel *ch = iio_device_get_channel(dev, i);
        if (iio_channel_is_scan_element(ch))
            iio_channel_enable(ch);
    }
    iio_device_set_kernel_buffers_count(dev, 8);
    struct iio_buffer *buf = iio_device_create_buffer(dev, 8192, false);
    if (!buf) {
        fprintf(stderr, "buffer: %s\n", strerror(errno));
        rc = 1;
        goto stop;
    }
    const double t0 = now_s();
    double next = t0 + 1;
    uint64_t last_frames = 0;
    while (!g_stop && now_s() - t0 < seconds) {
        const ssize_t n = iio_buffer_refill(buf);
        if (n < 0) {
            fprintf(stderr, "refill: %s\n", strerror((int)-n));
            rc = 1;
            break;
        }
        const uint8_t *p = iio_buffer_start(buf);
        rdof_chzhw_rx_feed(&rx, (const uint64_t *)p, (size_t)((const uint8_t *)iio_buffer_end(buf) - p) / 8);
        if (now_s() >= next) {
            next += 1;
            printf("%6.1f s: %llu frames (%llu new), %llu lost, %llu junk, STATUS 0x%x, FRAMES %u\n", now_s() - t0,
                   (unsigned long long)rx.ch_frames, (unsigned long long)(rx.ch_frames - last_frames),
                   (unsigned long long)rx.lost, (unsigned long long)rx.junk, (unsigned)rdof_chzhw_status(&h),
                   (unsigned)rdof_chzhw_frames(&h));
            last_frames = rx.ch_frames;
            printf("  dBFS:");
            for (int c = 0; c < nch; c++) {
                printf(" %.1f", st.n ? 10 * log10(st.pow[c] / st.n / (32768.0 * 32768.0) + 1e-12) : -120.0);
                st.pow[c] = 0;
            }
            printf("\n");
            st.n = 0;
            fflush(stdout);
        }
    }
    iio_buffer_destroy(buf);
    iio_context_destroy(ctx);
    if (st.out)
        fclose(st.out);
stop:
#else
    (void)uri, (void)out_path, (void)seconds;
    printf("built without libiio: the block runs for %.0f s, the frames aren't read\n", seconds);
    for (double t0 = now_s(); !g_stop && now_s() - t0 < seconds;)
        usleep(100000);
    printf("STATUS 0x%x, FRAMES %u\n", (unsigned)rdof_chzhw_status(&h), (unsigned)rdof_chzhw_frames(&h));
#endif
    if (!keep)
        rdof_chzhw_stop(&h);
    return rc;
}
