// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * SoapySDR loaded at run time (RDOF_SOAPY_DLOPEN), for the packages: one rdofradio for every release, whichever
 * SoapySDR it has (libSoapySDR.so.0.8, 0.7 or 0.6, tried in that order) or none (an MMDVM modem needs none).
 *
 * The functions rdofradio_soapy.c and rdofsdr.c call, with 0.8's C API (built against 0.8's headers), each passed on
 * to the library's own. Of these only SoapySDRDevice_setupStream changed: before 0.8 it returns an error code and
 * puts the stream in a pointer it's given. With no library, SoapySDRDevice_make returns NULL and lastError says
 * what to install.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stddef.h>

#include <SoapySDR/Device.h>
#include <SoapySDR/Types.h>
#include <SoapySDR/Version.h>

#if !defined(SOAPY_SDR_API_VERSION) || SOAPY_SDR_API_VERSION < 0x00080000
#error "rdofradio_soapydl.c: build it with SoapySDR 0.8's headers"
#endif

static void *lib;
static int abi8; /* 0.8: setupStream returns the stream */
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void load(void)
{
    static const char *const names[] = {"libSoapySDR.so.0.8", "libSoapySDR.so.0.7", "libSoapySDR.so.0.6"};
    for (size_t i = 0; i < sizeof names / sizeof *names && !lib; i++)
        if ((lib = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL)))
            abi8 = i == 0;
}

static void *fn(const char *name)
{
    pthread_once(&once, load);
    return lib ? dlsym(lib, name) : NULL;
}

/* each call: look the function up once, then call it (or return what failure returns there) */
#define CALL(ret, fail, name, params, args)                                                                           \
    ret name params                                                                                                   \
    {                                                                                                                 \
        static ret(*f) params;                                                                                        \
        if (!f)                                                                                                       \
            f = (ret(*) params)fn(#name);                                                                             \
        if (!f)                                                                                                       \
            return fail;                                                                                              \
        return f args;                                                                                                \
    }
#define CALLV(name, params, args)                                                                                     \
    void name params                                                                                                  \
    {                                                                                                                 \
        static void(*f) params;                                                                                       \
        if (!f)                                                                                                       \
            f = (void(*) params)fn(#name);                                                                            \
        if (f)                                                                                                        \
            f args;                                                                                                   \
    }

static const char missing[] = "SoapySDR isn't installed: apt install libsoapysdr0.8 (or 0.7, 0.6) and the device's "
                              "module (soapysdr-module-rtlsdr for an RTL-SDR)";

const char *SoapySDRDevice_lastError(void)
{
    static const char *(*f)(void);
    if (!f)
        f = (const char *(*)(void))fn("SoapySDRDevice_lastError");
    return f ? f() : missing;
}

CALL(const char *, "SoapySDR isn't installed", SoapySDR_errToStr, (const int code), (code))
CALL(SoapySDRKwargs, (SoapySDRKwargs){0}, SoapySDRKwargs_fromString, (const char *s), (s))
CALLV(SoapySDRKwargs_clear, (SoapySDRKwargs * a), (a))
CALL(SoapySDRDevice *, NULL, SoapySDRDevice_make, (const SoapySDRKwargs *a), (a))
CALL(int, -1, SoapySDRDevice_unmake, (SoapySDRDevice * d), (d))
CALL(char *, NULL, SoapySDRDevice_getDriverKey, (const SoapySDRDevice *d), (d))
CALL(int, -1, SoapySDRDevice_activateStream,
     (SoapySDRDevice * d, SoapySDRStream *s, const int flags, const long long timeNs, const size_t numElems),
     (d, s, flags, timeNs, numElems))
CALL(int, -1, SoapySDRDevice_deactivateStream,
     (SoapySDRDevice * d, SoapySDRStream *s, const int flags, const long long timeNs), (d, s, flags, timeNs))
CALL(int, -1, SoapySDRDevice_closeStream, (SoapySDRDevice * d, SoapySDRStream *s), (d, s))
CALL(int, -1, SoapySDRDevice_readStream,
     (SoapySDRDevice * d, SoapySDRStream *s, void *const *buffs, const size_t numElems, int *flags, long long *timeNs,
      const long timeoutUs),
     (d, s, buffs, numElems, flags, timeNs, timeoutUs))
CALL(int, -1, SoapySDRDevice_writeStream,
     (SoapySDRDevice * d, SoapySDRStream *s, const void *const *buffs, const size_t numElems, int *flags,
      const long long timeNs, const long timeoutUs),
     (d, s, buffs, numElems, flags, timeNs, timeoutUs))
CALL(int, -1, SoapySDRDevice_setFrequency,
     (SoapySDRDevice * d, const int dir, const size_t ch, const double hz, const SoapySDRKwargs *a), (d, dir, ch, hz, a))
CALL(int, -1, SoapySDRDevice_setSampleRate, (SoapySDRDevice * d, const int dir, const size_t ch, const double r),
     (d, dir, ch, r))
CALL(double, 0, SoapySDRDevice_getSampleRate, (const SoapySDRDevice *d, const int dir, const size_t ch), (d, dir, ch))
CALL(int, -1, SoapySDRDevice_setBandwidth, (SoapySDRDevice * d, const int dir, const size_t ch, const double bw),
     (d, dir, ch, bw))
CALL(double, 0, SoapySDRDevice_getBandwidth, (const SoapySDRDevice *d, const int dir, const size_t ch), (d, dir, ch))
CALL(int, -1, SoapySDRDevice_setGain, (SoapySDRDevice * d, const int dir, const size_t ch, const double v),
     (d, dir, ch, v))
CALL(double, 0, SoapySDRDevice_getGain, (const SoapySDRDevice *d, const int dir, const size_t ch), (d, dir, ch))
CALL(SoapySDRRange, (SoapySDRRange){0}, SoapySDRDevice_getGainRange,
     (const SoapySDRDevice *d, const int dir, const size_t ch), (d, dir, ch))
CALL(int, -1, SoapySDRDevice_setGainMode, (SoapySDRDevice * d, const int dir, const size_t ch, const bool automatic),
     (d, dir, ch, automatic))
CALL(char *, NULL, SoapySDRDevice_getNativeStreamFormat,
     (const SoapySDRDevice *d, const int dir, const size_t ch, double *fullScale), (d, dir, ch, fullScale))
CALL(int, -1, SoapySDRDevice_writeSetting, (SoapySDRDevice * d, const char *key, const char *value), (d, key, value))

SoapySDRStream *SoapySDRDevice_setupStream(SoapySDRDevice *d, const int dir, const char *format,
                                           const size_t *channels, const size_t numChans, const SoapySDRKwargs *a)
{
    static void *f;
    if (!f)
        f = fn("SoapySDRDevice_setupStream");
    if (!f)
        return NULL;
    if (abi8)
        return ((SoapySDRStream * (*)(SoapySDRDevice *, int, const char *, const size_t *, size_t,
                                      const SoapySDRKwargs *))f)(d, dir, format, channels, numChans, a);
    SoapySDRStream *s = NULL;
    const int r = ((int (*)(SoapySDRDevice *, SoapySDRStream **, int, const char *, const size_t *, size_t,
                            const SoapySDRKwargs *))f)(d, &s, dir, format, channels, numChans, a);
    return r == 0 ? s : NULL;
}
