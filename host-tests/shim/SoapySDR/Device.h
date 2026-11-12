// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The part of SoapySDR's C API rdofradio_soapy.c uses, for a test with no SoapySDR installed (test_soapy.c fakes it). */
#ifndef SHIM_SOAPYSDR_DEVICE_H
#define SHIM_SOAPYSDR_DEVICE_H

#include <stdbool.h>
#include <stddef.h>

#define SOAPY_SDR_TX 0
#define SOAPY_SDR_RX 1
#define SOAPY_SDR_TIMEOUT (-1)
#define SOAPY_SDR_OVERFLOW (-4)

typedef struct SoapySDRDevice SoapySDRDevice;
typedef struct SoapySDRStream SoapySDRStream;
typedef struct {
    size_t size;
    char **keys, **vals;
} SoapySDRKwargs;
typedef struct {
    double minimum, maximum, step;
} SoapySDRRange;

const char *SoapySDR_errToStr(int e);
const char *SoapySDRDevice_lastError(void);
SoapySDRKwargs SoapySDRKwargs_fromString(const char *s);
void SoapySDRKwargs_clear(SoapySDRKwargs *k);
SoapySDRDevice *SoapySDRDevice_make(const SoapySDRKwargs *k);
int SoapySDRDevice_unmake(SoapySDRDevice *d);
char *SoapySDRDevice_getDriverKey(const SoapySDRDevice *d);
int SoapySDRDevice_writeSetting(SoapySDRDevice *d, const char *key, const char *value);
SoapySDRRange SoapySDRDevice_getGainRange(const SoapySDRDevice *d, int dir, size_t ch);
int SoapySDRDevice_setSampleRate(SoapySDRDevice *d, int dir, size_t ch, double rate);
double SoapySDRDevice_getSampleRate(const SoapySDRDevice *d, int dir, size_t ch);
int SoapySDRDevice_setFrequency(SoapySDRDevice *d, int dir, size_t ch, double hz, const SoapySDRKwargs *args);
int SoapySDRDevice_setGainMode(SoapySDRDevice *d, int dir, size_t ch, bool automatic);
int SoapySDRDevice_setGain(SoapySDRDevice *d, int dir, size_t ch, double db);
double SoapySDRDevice_getGain(const SoapySDRDevice *d, int dir, size_t ch);
int SoapySDRDevice_setBandwidth(SoapySDRDevice *d, int dir, size_t ch, double hz);
double SoapySDRDevice_getBandwidth(const SoapySDRDevice *d, int dir, size_t ch);
char *SoapySDRDevice_getNativeStreamFormat(const SoapySDRDevice *d, int dir, size_t ch, double *full_scale);
SoapySDRStream *SoapySDRDevice_setupStream(SoapySDRDevice *d, int dir, const char *fmt, const size_t *chs, size_t n,
                                           const SoapySDRKwargs *args);
int SoapySDRDevice_closeStream(SoapySDRDevice *d, SoapySDRStream *s);
int SoapySDRDevice_activateStream(SoapySDRDevice *d, SoapySDRStream *s, int flags, long long t, size_t n);
int SoapySDRDevice_deactivateStream(SoapySDRDevice *d, SoapySDRStream *s, int flags, long long t);
int SoapySDRDevice_readStream(SoapySDRDevice *d, SoapySDRStream *s, void *const *b, size_t n, int *flags, long long *t,
                              long us);
int SoapySDRDevice_writeStream(SoapySDRDevice *d, SoapySDRStream *s, const void *const *b, size_t n, int *flags,
                               long long t, long us);

#endif
