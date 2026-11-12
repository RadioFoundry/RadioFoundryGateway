// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* rdofradio's SoapySDR device against a fake SoapySDR (shim/SoapySDR): --setting reaches the driver (an RTL-SDR's
 * bias tee), and a setting the driver refuses stops the open. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SoapySDR/Device.h>

#include "rdofradio_dev.h"

static int checks, failures;
#define CHECK(c, ...)                         \
    do {                                      \
        checks++;                             \
        if (!(c)) {                           \
            failures++;                       \
            printf("FAIL %d: ", __LINE__);    \
            printf(__VA_ARGS__);              \
            printf("\n");                     \
        }                                     \
    } while (0)

static SoapySDRDevice *const DEV = (SoapySDRDevice *)&checks;
static SoapySDRStream *const STREAM = (SoapySDRStream *)&failures;
static char written[256];
static const char *refuse;

const char *SoapySDR_errToStr(int e) { return "fake"; }
const char *SoapySDRDevice_lastError(void) { return "the fake's error"; }
SoapySDRKwargs SoapySDRKwargs_fromString(const char *s) { SoapySDRKwargs k = {0}; return k; }
void SoapySDRKwargs_clear(SoapySDRKwargs *k) {}
SoapySDRDevice *SoapySDRDevice_make(const SoapySDRKwargs *k) { return DEV; }
int SoapySDRDevice_unmake(SoapySDRDevice *d) { return 0; }
char *SoapySDRDevice_getDriverKey(const SoapySDRDevice *d) { return strdup("rtlsdr"); }
int SoapySDRDevice_writeSetting(SoapySDRDevice *d, const char *key, const char *value)
{
    if (refuse && !strcmp(key, refuse))
        return -2;
    size_t n = strlen(written);
    snprintf(written + n, sizeof written - n, "%s:%s;", key, value);
    return 0;
}
SoapySDRRange SoapySDRDevice_getGainRange(const SoapySDRDevice *d, int dir, size_t ch)
{
    SoapySDRRange r = {0, 50, 1};
    return r;
}
int SoapySDRDevice_setSampleRate(SoapySDRDevice *d, int dir, size_t ch, double rate) { return 0; }
double SoapySDRDevice_getSampleRate(const SoapySDRDevice *d, int dir, size_t ch) { return 2.4e6; }
int SoapySDRDevice_setFrequency(SoapySDRDevice *d, int dir, size_t ch, double hz, const SoapySDRKwargs *a) { return 0; }
int SoapySDRDevice_setGainMode(SoapySDRDevice *d, int dir, size_t ch, bool automatic) { return 0; }
int SoapySDRDevice_setGain(SoapySDRDevice *d, int dir, size_t ch, double db) { return 0; }
double SoapySDRDevice_getGain(const SoapySDRDevice *d, int dir, size_t ch) { return 0; }
int SoapySDRDevice_setBandwidth(SoapySDRDevice *d, int dir, size_t ch, double hz) { return 0; }
double SoapySDRDevice_getBandwidth(const SoapySDRDevice *d, int dir, size_t ch) { return 0; }
char *SoapySDRDevice_getNativeStreamFormat(const SoapySDRDevice *d, int dir, size_t ch, double *fs)
{
    *fs = 128;
    return strdup("CS8");
}
SoapySDRStream *SoapySDRDevice_setupStream(SoapySDRDevice *d, int dir, const char *f, const size_t *c, size_t n,
                                           const SoapySDRKwargs *a) { return STREAM; }
int SoapySDRDevice_closeStream(SoapySDRDevice *d, SoapySDRStream *s) { return 0; }
int SoapySDRDevice_activateStream(SoapySDRDevice *d, SoapySDRStream *s, int f, long long t, size_t n) { return 0; }
int SoapySDRDevice_deactivateStream(SoapySDRDevice *d, SoapySDRStream *s, int f, long long t) { return 0; }
int SoapySDRDevice_readStream(SoapySDRDevice *d, SoapySDRStream *s, void *const *b, size_t n, int *f, long long *t,
                              long us) { return SOAPY_SDR_TIMEOUT; }
int SoapySDRDevice_writeStream(SoapySDRDevice *d, SoapySDRStream *s, const void *const *b, size_t n, int *f,
                               long long t, long us) { return (int)n; }

int main(void)
{
    static const char *const settings[] = {"biastee=true", "digital_agc=a=b"};
    rdofradio_dev_setup_t su = {.rate = 2.4e6, .rx_hz = 438e6, .tx_hz = 438e6, .gain = 30, .stream = "", .rx_only = true,
                                .settings = settings, .n_settings = 2};
    char err[200] = "";
    rdofradio_dev_t *d = rdofradio_soapy_open("driver=rtlsdr", &su, err, sizeof err);
    CHECK(d != NULL, "it opens: %s", err);
    CHECK(!strcmp(written, "biastee:true;digital_agc:a=b;"), "what the driver was told: %s", written);
    if (d)
        d->close(d);

    written[0] = 0;
    su.n_settings = 0;
    d = rdofradio_soapy_open("driver=rtlsdr", &su, err, sizeof err);
    CHECK(d && !written[0], "no settings, nothing written: %s", written);
    if (d)
        d->close(d);

    su.n_settings = 2;
    refuse = "biastee";
    err[0] = 0;
    d = rdofradio_soapy_open("driver=rtlsdr", &su, err, sizeof err);
    CHECK(!d && strstr(err, "biastee"), "a setting the driver refuses stops the open: %s", err);

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
