// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * rtl_probe: the RTL-SDR driver (rtlsdr/) on a Linux host, over usbfs, for bench checks and the measurements in
 * docs/spec/rtlsdr.md section 3. No libusb: control and bulk transfers go through /dev/bus/usb ioctls.
 *
 *   rtl_probe info
 *   rtl_probe lock MHZ_FROM MHZ_TO MHZ_STEP                 tune each frequency; print those that fail
 *   rtl_probe cap MHZ RATE GAIN PPM SECONDS OUT.cu8         GAIN in dB, or "agc"
 *   rtl_probe tunes MHZ_CARRIER RATE GAIN PPM N             N centers near a carrier: its measured frequency each time
 *   rtl_probe gains MHZ_CARRIER RATE VGA SECONDS            a CW carrier's level at each LNA code (mixer 0), then
 *                                                           each mixer code (LNA 0)
 *   rtl_probe steps MHZ_CARRIER RATE SECONDS                a CW carrier's level at each step of the gain table
 *   rtl_probe filters MHZ RATE GAIN SECONDS OUT.csv         averaged spectra for each IF filter setting
 *   rtl_probe ifscan MHZ GAIN SECONDS OUT.csv               the same over the tuner's whole output: for each setting
 *                                                           the DDC's IF moved 1..7 MHz in 1 MHz steps (2.4 MS/s)
 *
 * RTL_BIAS=1 in the environment turns the bias tee on first.
 *
 * Build: cc -O2 -std=c11 -D_GNU_SOURCE -Irtlsdr/include rtlsdr/src/rtl2832u.c rtlsdr/src/r820.c
 *        tools/rtl_probe/rtl_probe.c -lm -o rtl_probe
 */
#include <complex.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "rtlsdr/rtl.h"

#define NFFT 2048
#define URBS 8
#define URB_LEN (64 * 1024)

static int fd = -1;

static int usb_control(void *ctx, bool in, uint16_t value, uint16_t index, uint8_t *data, uint16_t len)
{
    struct usbdevfs_ctrltransfer c = {
        .bRequestType = in ? 0xC0 : 0x40, .bRequest = 0, .wValue = value, .wIndex = index, .wLength = len,
        .timeout = 300, .data = data};
    return ioctl(fd, USBDEVFS_CONTROL, &c);
}

static void usb_sleep(void *ctx, int ms) { usleep((useconds_t)ms * 1000); }

static int read_sysfs(const char *dir, const char *name, char *out, size_t cap)
{
    char p[512];
    snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/%s", dir, name);
    FILE *f = fopen(p, "r");
    if (!f)
        return -1;
    if (!fgets(out, (int)cap, f))
        out[0] = 0;
    fclose(f);
    out[strcspn(out, "\n")] = 0;
    return 0;
}

static bool blog_v4;

static int usb_open(void)
{
    DIR *d = opendir("/sys/bus/usb/devices");
    struct dirent *e;
    char vid[16], pid[16], bus[16], dev[16], prod[128];
    while (d && (e = readdir(d))) {
        if (read_sysfs(e->d_name, "idVendor", vid, sizeof vid) || strcmp(vid, "0bda"))
            continue;
        read_sysfs(e->d_name, "idProduct", pid, sizeof pid);
        if (strcmp(pid, "2832") && strcmp(pid, "2838"))
            continue;
        read_sysfs(e->d_name, "busnum", bus, sizeof bus);
        read_sysfs(e->d_name, "devnum", dev, sizeof dev);
        prod[0] = 0;
        read_sysfs(e->d_name, "product", prod, sizeof prod);
        blog_v4 = strstr(prod, "Blog V4") != NULL;
        char path[64];
        snprintf(path, sizeof path, "/dev/bus/usb/%03d/%03d", atoi(bus), atoi(dev));
        fd = open(path, O_RDWR);
        if (fd < 0) {
            fprintf(stderr, "%s: %s\n", path, strerror(errno));
            closedir(d);
            return -1;
        }
        unsigned iface = 0;
        if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface) < 0) {
            fprintf(stderr, "claim interface 0: %s\n", strerror(errno));
            closedir(d);
            return -1;
        }
        fprintf(stderr, "device %s %s:%s \"%s\"%s\n", path, vid, pid, prod, blog_v4 ? " (Blog V4)" : "");
        closedir(d);
        return 0;
    }
    if (d)
        closedir(d);
    fprintf(stderr, "no RTL2832U found\n");
    return -1;
}

/* ── streaming: URBS bulk reads in flight ────────────────────────────────── */

static struct usbdevfs_urb urbs[URBS];
static uint8_t *bufs[URBS];

static int submit(int i)
{
    memset(&urbs[i], 0, sizeof urbs[i]);
    urbs[i].type = USBDEVFS_URB_TYPE_BULK;
    urbs[i].endpoint = RTL_BULK_EP;
    urbs[i].buffer = bufs[i];
    urbs[i].buffer_length = URB_LEN;
    urbs[i].usercontext = (void *)(intptr_t)i;
    return ioctl(fd, USBDEVFS_SUBMITURB, &urbs[i]);
}

/* Calls take(buf, n, ctx) for each transfer until it returns false or `bytes` arrived. Returns the bytes. */
static size_t stream(size_t bytes, bool (*take)(const uint8_t *, size_t, void *), void *ctx)
{
    for (int i = 0; i < URBS; i++) {
        if (!bufs[i])
            bufs[i] = malloc(URB_LEN);
        if (submit(i) < 0) {
            fprintf(stderr, "submit: %s\n", strerror(errno));
            return 0;
        }
    }
    size_t got = 0;
    bool more = true;
    int pending = URBS;
    while (pending) {
        struct usbdevfs_urb *u = NULL;
        if (ioctl(fd, USBDEVFS_REAPURB, &u) < 0) {
            fprintf(stderr, "reap: %s\n", strerror(errno));
            break;
        }
        pending--;
        int i = (int)(intptr_t)u->usercontext;
        if (more && u->status == 0 && u->actual_length > 0) {
            size_t n = (size_t)u->actual_length;
            if (got + n > bytes)
                n = bytes - got;
            got += n;
            more = take(bufs[i], n, ctx) && got < bytes;
        }
        if (more) {
            if (submit(i) == 0)
                pending++;
        }
    }
    return got;
}

static bool to_file(const uint8_t *b, size_t n, void *ctx) { return fwrite(b, 1, n, ctx) == n; }

/* ── spectra ─────────────────────────────────────────────────────────────── */

static void fft(double complex *x, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            double complex t = x[i];
            x[i] = x[j];
            x[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double complex w = cexp(-2.0 * M_PI * I / len);
        for (int i = 0; i < n; i += len) {
            double complex wk = 1;
            for (int k = 0; k < len / 2; k++) {
                double complex a = x[i + k], b = x[i + k + len / 2] * wk;
                x[i + k] = a + b;
                x[i + k + len / 2] = a - b;
                wk *= w;
            }
        }
    }
}

typedef struct {
    double pow[NFFT];
    int frames;
    uint8_t part[2 * NFFT];
    size_t have;
    size_t clipped, samples;
} spec_t;

static bool to_spec(const uint8_t *b, size_t n, void *ctx)
{
    spec_t *s = ctx;
    static double win[NFFT];
    if (win[NFFT / 2] == 0)
        for (int i = 0; i < NFFT; i++)
            win[i] = 0.5 - 0.5 * cos(2 * M_PI * i / NFFT);
    while (n) {
        size_t k = 2 * NFFT - s->have < n ? 2 * NFFT - s->have : n;
        memcpy(s->part + s->have, b, k);
        s->have += k, b += k, n -= k;
        if (s->have == 2 * NFFT) {
            double complex x[NFFT];
            for (int i = 0; i < NFFT; i++) {
                uint8_t re = s->part[2 * i], im = s->part[2 * i + 1];
                s->clipped += (re == 0 || re == 255 || im == 0 || im == 255);
                x[i] = ((re - 127.5) + I * (im - 127.5)) / 127.5 * win[i];
            }
            s->samples += NFFT;
            fft(x, NFFT);
            for (int i = 0; i < NFFT; i++)
                s->pow[(i + NFFT / 2) % NFFT] += creal(x[i] * conj(x[i]));
            s->frames++;
            s->have = 0;
        }
    }
    return true;
}

/* Averaged spectrum (dB, DC at NFFT/2) of `seconds` of samples, after the first 50 ms are dropped. */
static void measure(rtl_dev_t *dev, double seconds, spec_t *s)
{
    memset(s, 0, sizeof *s);
    rtl_reset_buffer(dev);
    spec_t skip;
    memset(&skip, 0, sizeof skip);
    stream((size_t)(dev->rate * 0.05) * 2, to_spec, &skip);
    stream((size_t)(dev->rate * seconds) * 2, to_spec, s);
    for (int i = 0; i < NFFT; i++)
        s->pow[i] = 10 * log10(s->pow[i] / (s->frames ? s->frames : 1) / NFFT + 1e-20);
}

static double median(const double *v, int n)
{
    double t[NFFT];
    memcpy(t, v, sizeof(double) * (size_t)n);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && t[j - 1] > t[j]; j--) {
            double x = t[j];
            t[j] = t[j - 1];
            t[j - 1] = x;
        }
    return t[n / 2];
}

/* The strongest bin within ±span_hz of off_hz: its offset (parabolic interpolation) and power. */
static double peak(const spec_t *s, double rate, double off_hz, double span_hz, double *db)
{
    double bin = rate / NFFT;
    int c = NFFT / 2 + (int)lround(off_hz / bin), w = (int)ceil(span_hz / bin);
    if (c < 1 || c > NFFT - 2) {
        fprintf(stderr, "peak: %.0f Hz off at %.0f S/s is outside the spectrum\n", off_hz, rate);
        *db = NAN;
        return NAN;
    }
    int best = c;
    for (int i = c - w; i <= c + w; i++)
        if (i > 0 && i < NFFT - 1 && s->pow[i] > s->pow[best])
            best = i;
    double a = s->pow[best - 1], b = s->pow[best], g = s->pow[best + 1];
    double p = (a - g) / (2 * (a - 2 * b + g));
    if (!isfinite(p))
        p = 0;
    *db = b;
    return (best - NFFT / 2 + p) * bin;
}

/* ── raw tuner writes, for the sweeps ────────────────────────────────────── */

static int demod_put(int page, uint8_t addr, uint8_t v)
{
    if (usb_control(NULL, false, (uint16_t)(addr << 8 | 0x20), (uint16_t)(0x10 | page), &v, 1) != 1)
        return -1;
    uint8_t sink;
    return usb_control(NULL, true, 0x0120, 0x000A, &sink, 1) == 1 ? 0 : -1;
}

/* reg = (reg & ~mask) | (val & mask), through the repeater, with the driver's shadow kept. */
static int tuner_put(rtl_dev_t *dev, uint8_t reg, uint8_t val, uint8_t mask)
{
    uint8_t v = (uint8_t)((dev->tun.regs[reg] & ~mask) | (val & mask));
    if (demod_put(1, 0x01, 0x18))
        return -1;
    int r = dev->tun.bus.write(dev->tun.bus.ctx, dev->tun.addr, reg, &v, 1);
    demod_put(1, 0x01, 0x10);
    if (r)
        return r;
    dev->tun.regs[reg] = v;
    return 0;
}

/* The DDC's IF (D1.6), set directly. */
static int set_ddc_if(double hz)
{
    uint32_t v = (uint32_t)(-(int32_t)lround(hz / RTL_XTAL_HZ * 4194304.0)) & 0x3FFFFF;
    return demod_put(1, 0x19, (uint8_t)(v >> 16 & 0x3F)) || demod_put(1, 0x1A, (uint8_t)(v >> 8)) ||
           demod_put(1, 0x1B, (uint8_t)v);
}

/* ── commands ────────────────────────────────────────────────────────────── */

static rtl_dev_t dev;

static int fail(const char *what, int r)
{
    fprintf(stderr, "%s: %d %s\n", what, r, dev.error);
    return 1;
}

static int set_gain(const char *g)
{
    if (!strcmp(g, "agc"))
        return rtl_set_tuner_gain_mode(&dev, false);
    int r = rtl_set_tuner_gain_mode(&dev, true);
    return r ? r : rtl_set_tuner_gain(&dev, (int)lround(atof(g) * 10));
}

static int setup(double mhz, uint32_t rate, const char *gain, int ppm)
{
    int r;
    if ((r = rtl_set_sample_rate(&dev, rate)))
        return fail("rate", r);
    if (ppm && (r = rtl_set_freq_correction(&dev, ppm)))
        return fail("ppm", r);
    if ((r = rtl_set_center_freq(&dev, (uint32_t)llround(mhz * 1e6))))
        return fail("freq", r);
    if (gain && (r = set_gain(gain)))
        return fail("gain", r);
    return 0;
}

static void print_tune(void)
{
    printf("  lo %.3f Hz, rf %.3f Hz, if %.3f Hz, sel_div %d nint %d sdm %u\n", dev.tun.pll.lo_hz, dev.tun.rf_hz,
           r820_if_hz(&dev.tun), dev.tun.pll.sel_div, dev.tun.pll.nint, dev.tun.pll.sdm);
}

int main(int argc, char **argv)
{
    if (argc < 2 || usb_open())
        return 2;
    rtl_io_t io = {usb_control, usb_sleep, NULL};
    int r = rtl_open(&dev, &io, false, blog_v4);
    if (r)
        return fail("open", r);
    /* RTL_BIAS=1: the bias tee on (a powered antenna or LNA, as on the bench) */
    const char *bias = getenv("RTL_BIAS");
    if (bias && atoi(bias) && (r = rtl_set_bias_tee(&dev, true)))
        return fail("bias tee", r);
    const char *cmd = argv[1];
    int rc = 0;

    if (!strcmp(cmd, "info")) {
        const int *g;
        int n = rtl_tuner_gains(&dev, &g);
        printf("tuner %s, %d gain steps:", rtl_tuner_name(dev.tuner), n);
        for (int i = 0; i < n; i++)
            printf(" %.1f", g[i] / 10.0);
        printf("\nfilter cal code %d\n", dev.tun.cal);
        rc = setup(461.1, 288000, "20", 0);
        uint8_t regs[32];
        int nr = -1;
        static const int tries[] = {32, 16, 8, 5}; /* as many as the RTL's I2C bridge reads at once */
        for (int k = 0; !rc && k < 4 && nr < 0; k++)
            nr = rtl_read_tuner_regs(&dev, regs, tries[k]);
        if (!rc && nr < 0)
            fprintf(stderr, "tuner registers: %d %s\n", nr, dev.error);
        if (nr > 0) {
            printf("461.1 MHz at 288000, tuner regs:");
            for (int i = 0; i < nr; i++)
                printf(" %02x", regs[i]);
            printf("\n");
            print_tune();
        }
    } else if (!strcmp(cmd, "lock") && argc == 5) {
        double a = atof(argv[2]), b = atof(argv[3]), step = atof(argv[4]);
        rc = setup(a, 2400000, NULL, 0);
        int n = 0, bad = 0;
        uint8_t regs[5];
        for (double f = a; f <= b + 1e-9; f += step, n++) {
            r = rtl_set_center_freq(&dev, (uint32_t)llround(f * 1e6));
            rtl_read_tuner_regs(&dev, regs, 5);
            if (r) {
                bad++;
                printf("%.3f MHz: %d %s\n", f, r, dev.error);
            } else if (((regs[4] >> 4) & 3) != (dev.tuner == RTL_TUNER_R828D ? 1 : 2)) {
                printf("%.3f MHz: locked, VCO fine tune %d (G.5)\n", f, (regs[4] >> 4) & 3);
            }
        }
        printf("%d frequencies, %d failed\n", n, bad);
    } else if (!strcmp(cmd, "cap") && argc == 8) {
        rc = setup(atof(argv[2]), (uint32_t)atoi(argv[3]), argv[4], atoi(argv[5]));
        if (!rc) {
            FILE *f = fopen(argv[7], "wb");
            rtl_reset_buffer(&dev);
            size_t want = (size_t)(dev.rate * atof(argv[6])) * 2;
            size_t got = stream(want, to_file, f);
            fclose(f);
            printf("rate %u, %zu of %zu bytes\n", dev.rate, got, want);
            print_tune();
        }
    } else if (!strcmp(cmd, "tunes") && argc == 7) {
        /* The carrier's frequency, measured from N centers spread over 100 kHz around it: right IF correction, the
         * same each time (to the FFT's precision); a backward one, scattered by twice each tune's PLL error. */
        double carrier = atof(argv[2]) * 1e6;
        uint32_t rate = (uint32_t)atoi(argv[3]);
        int n = atoi(argv[6]);
        rc = setup(carrier / 1e6 - 0.05, rate, argv[4], atoi(argv[5]));
        for (int k = 0; !rc && k < n; k++) {
            double center = carrier - 50000 - 2000.0 * k - 37.0 * k;
            if ((r = rtl_set_center_freq(&dev, (uint32_t)llround(center))))
                return fail("freq", r);
            spec_t s;
            measure(&dev, 1.0, &s);
            double db, off = peak(&s, dev.rate, carrier - dev.freq, 3000, &db);
            double pll_err = dev.tun.pll.lo_hz - (dev.tun.rf_hz + R820_IF_HZ);
            printf("center %.0f: carrier at %+.1f Hz from the asked (%.1f dB), pll error %+.1f Hz\n", (double)dev.freq,
                   dev.freq + off - carrier, db, pll_err);
        }
    } else if (!strcmp(cmd, "gains") && argc == 6) {
        /* LNA codes with the mixer at 0, then mixer codes with the LNA at 0; the VGA fixed. Tuned 50 kHz below
         * the carrier. */
        rc = setup(atof(argv[2]) - 0.05, (uint32_t)atoi(argv[3]), "0", 0);
        int vga = atoi(argv[4]);
        double secs = atof(argv[5]);
        tuner_put(&dev, 0x0C, (uint8_t)vga, 0x1F);
        for (int pass = 0; !rc && pass < 2; pass++)
            for (int code = 0; code < 16; code++) {
                tuner_put(&dev, 0x05, (uint8_t)(0x10 | (pass ? 0 : code)), 0x1F);
                tuner_put(&dev, 0x07, (uint8_t)(pass ? code : 0), 0x1F);
                usleep(50000);
                spec_t s;
                measure(&dev, secs, &s);
                double db, off = peak(&s, dev.rate, 50000, 5000, &db);
                printf("%s %2d: carrier %+.0f Hz %.2f dB, floor %.2f dB, clipped %.3f%%\n", pass ? "mixer" : "lna",
                       code, off, db, median(s.pow, NFFT), 100.0 * s.clipped / (s.samples ? s.samples : 1));
                fflush(stdout);
            }
    } else if (!strcmp(cmd, "steps") && argc == 5) {
        rc = setup(atof(argv[2]) - 0.05, (uint32_t)atoi(argv[3]), "0", 0);
        const int *g;
        int n = rtl_tuner_gains(&dev, &g);
        double first = NAN;
        for (int i = 0; !rc && i < n; i++) {
            if ((r = rtl_set_tuner_gain(&dev, g[i])))
                return fail("gain", r);
            usleep(50000);
            spec_t s;
            measure(&dev, atof(argv[4]), &s);
            double db;
            peak(&s, dev.rate, 50000, 5000, &db);
            if (i == 0)
                first = db;
            printf("step %2d: table %5.1f dB, carrier %+5.1f dB (%+.1f), clipped %.3f%%\n", i, g[i] / 10.0,
                   db - first, db - first - g[i] / 10.0, 100.0 * s.clipped / (s.samples ? s.samples : 1));
            fflush(stdout);
        }
    } else if (!strcmp(cmd, "ifscan") && argc == 6) {
        rc = setup(atof(argv[2]), 2400000, argv[3], 0);
        double secs = atof(argv[4]);
        FILE *f = fopen(argv[5], "w");
        fprintf(f, "bw,code,hpf,view_hz,rate");
        for (int i = 0; i < 256; i++)
            fprintf(f, ",b%d", i);
        fprintf(f, "\n");
        static const int codes[] = {0, 4, 8, 12, 15};
        for (int k = 0; !rc && k < 4 * 5 + 16; k++) {
            int bw = k < 20 ? k / 5 : 0, code = k < 20 ? codes[k % 5] : 0, hpf = k < 20 ? 11 : k - 20;
            tuner_put(&dev, 0x0A, (uint8_t)(0x10 | code), 0x1F);
            tuner_put(&dev, 0x0B, (uint8_t)(bw << 5 | hpf), 0x6F);
            for (int v = 1; v <= 7; v++) {
                set_ddc_if(v * 1e6);
                usleep(20000);
                spec_t s;
                measure(&dev, secs, &s);
                fprintf(f, "%d,%d,%d,%d,%u", bw, code, hpf, v * 1000000, dev.rate);
                for (int i = 0; i < 256; i++) {
                    double p = 0;
                    for (int j = 0; j < NFFT / 256; j++)
                        p += pow(10, s.pow[i * (NFFT / 256) + j] / 10);
                    fprintf(f, ",%.2f", 10 * log10(p / (NFFT / 256)));
                }
                fprintf(f, "\n");
            }
            fflush(f);
            fprintf(stderr, "bw %d code %2d hpf %2d\n", bw, code, hpf);
        }
        fclose(f);
    } else if (!strcmp(cmd, "filters") && argc == 7) {
        /* Each FILT_BW x FILT_CODE at HPF 11, then each HPF at the widest setting: the spectrum, 256 bins. */
        rc = setup(atof(argv[2]), (uint32_t)atoi(argv[3]), argv[4], 0);
        double secs = atof(argv[5]);
        FILE *f = fopen(argv[6], "w");
        fprintf(f, "bw,code,hpf,rate");
        for (int i = 0; i < 256; i++)
            fprintf(f, ",b%d", i);
        fprintf(f, "\n");
        for (int k = 0; !rc && k < 4 * 16 + 16; k++) {
            int bw = k < 64 ? k / 16 : 0, code = k < 64 ? k % 16 : 0, hpf = k < 64 ? 11 : k - 64;
            tuner_put(&dev, 0x0A, (uint8_t)(0x10 | code), 0x1F);
            tuner_put(&dev, 0x0B, (uint8_t)(bw << 5 | hpf), 0x6F);
            usleep(50000);
            spec_t s;
            measure(&dev, secs, &s);
            fprintf(f, "%d,%d,%d,%u", bw, code, hpf, dev.rate);
            for (int i = 0; i < 256; i++) {
                double p = 0;
                for (int j = 0; j < NFFT / 256; j++)
                    p += pow(10, s.pow[i * (NFFT / 256) + j] / 10);
                fprintf(f, ",%.2f", 10 * log10(p / (NFFT / 256)));
            }
            fprintf(f, "\n");
            fflush(f);
            fprintf(stderr, "bw %d code %2d hpf %2d: floor %.1f dB\n", bw, code, hpf, median(s.pow, NFFT));
        }
        fclose(f);
    } else {
        fprintf(stderr, "unknown command or arguments\n");
        rc = 2;
    }
    rtl_close(&dev);
    return rc;
}
