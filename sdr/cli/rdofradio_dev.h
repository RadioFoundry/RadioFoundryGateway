// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * rdofradio's device: one SDR that receives and transmits at one rate, each direction on its own LO, behind a few
 * calls (with a PlutoSDR's FPGA TX interpolator, the transmit stream at a fraction of it). Two of them:
 *   --soapy ARGS  any SoapySDR device (rdofradio_soapy.c)
 *   --iio URI     an AD9361 (the PlutoSDR family) through libiio straight (rdofradio_iio.c): only the libraries a
 *                 stock PlutoSDR already has, and with libad9361 its own FIR decimation below 2.083 MS/s (the
 *                 FPGA's decimator, which SoapyPlutoSDR uses there, is broken on some boards)
 * Opened receive only (rx_only), any receiver does (an RTL-SDR through SoapySDR): write and stop_tx do nothing.
 * Samples are complex floats, full scale 1.0. TX gain is in dB above the device's least: the PlutoSDR's 0..89 is
 * its attenuation of 89..0 dB, on both. write is called from the transmit thread alone, the rest from the main
 * loop.
 */
#ifndef RDOFRADIO_DEV_H
#define RDOFRADIO_DEV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/sdr/fft.h" /* rdof_cf_t */
#include "rdof/sdr/txguard.h"

#define RDOF_DEV_TIMEOUT (-1)  /* read: nothing in the time given */
#define RDOF_DEV_OVERFLOW (-2) /* read: samples were dropped before these */
#define RDOF_DEV_ERROR (-3)

typedef struct {
    double rate;         /* S/s, both directions */
    double rx_hz, tx_hz; /* the LOs */
    double gain;         /* RX gain, dB; NAN: the device's AGC */
    double bw;           /* analog bandwidth, Hz, both directions; 0: the device's own (iio: the rate) */
    const char *stream;  /* --stream: the backend's stream arguments, "" for its defaults */
    bool rx_only;        /* receive only (a radio transmits: --split-tx): no TX stream, the TX LO off */
    /* > 0: the receive DMA's 64-bit words as they come, about this many a second (the FPGA channelizer's frames,
     * libiio only): read_words in place of read */
    double words_rate;
    /* > 1: the transmit stream at rate / tx_interp (the FPGA's TX interpolator makes up the rest, libiio only) */
    int tx_interp;
    /* --setting KEY=VALUE, the driver's own (an RTL-SDR's biastee=true; SoapySDR only) */
    const char *const *settings;
    int n_settings;
} rdofradio_dev_setup_t;

typedef struct rdofradio_dev rdofradio_dev_t;
struct rdofradio_dev {
    char name[48];  /* for the log and the page: "SoapySDR plutosdr", "libiio AD9361" */
    double rate;    /* what it runs at */
    double tx_rate; /* the transmit stream's: rate, or rate / tx_interp */
    double tx_gain_min, tx_gain_max;
    float iq_scale;  /* the bench history's cs8 gain (rdof_bl_ring_scale): 1 for an 8-bit ADC (an RTL-SDR), 16 for an
                        AD9361's 12 */
    char error[200]; /* the last failure */
    int (*set_rx_freq)(rdofradio_dev_t *d, double hz);
    int (*set_tx_freq)(rdofradio_dev_t *d, double hz); /* the TX LO (not opened rx_only) */
    int (*set_tx_gain)(rdofradio_dev_t *d, double db);
    double (*rx_gain)(rdofradio_dev_t *d);
    int (*set_rx_gain)(rdofradio_dev_t *d, double db); /* dB; NAN: the device's AGC. NULL: it keeps its gain */
    double (*bandwidth)(rdofradio_dev_t *d); /* RX analog, Hz */
    int (*start)(rdofradio_dev_t *d);        /* both streams */
    /* up to n samples: how many, or RDOF_DEV_* */
    int (*read)(rdofradio_dev_t *d, rdof_cf_t *x, int n, long timeout_us);
    /* words_rate: the next DMA buffer's words, *w valid until the next call: how many, or RDOF_DEV_* */
    int (*read_words)(rdofradio_dev_t *d, const uint64_t **w, long timeout_us);
    /* The ADC's own samples, once, in place of the receive stream (libiio with read_words; NULL elsewhere), from the
     * thread that has told the loop to stop reading: rx_pause ends the stream. raw_fill: n samples in a row, int16 I, Q,
     * into out, *after the samples that came after them before it returned; 0, or -1 with why in err. rx_resume starts
     * the stream again. */
    void (*rx_pause)(rdofradio_dev_t *d);
    int (*raw_fill)(rdofradio_dev_t *d, long n, double rate, int16_t *out, long *after, char *err, size_t cap);
    /* For a raw capture: the chip at rate (0: as it is) with the RX LO at lo_hz (0: as it is), what it took in *got;
     * raw_untune puts back what raw_tune found. The TX shares the rate. 0, or -1 with why in err. */
    int (*raw_tune)(rdofradio_dev_t *d, double rate, double lo_hz, double *got, char *err, size_t cap);
    int (*raw_untune)(rdofradio_dev_t *d);
    /* The transmit buffer anew, empty (after a rate change); 0 or -1 */
    int (*tx_restart)(rdofradio_dev_t *d);
    int (*rx_resume)(rdofradio_dev_t *d);
    /* up to n samples: how many were taken, or RDOF_DEV_* */
    int (*write)(rdofradio_dev_t *d, const rdof_cf_t *x, int n, long timeout_us);
    void (*stop_tx)(rdofradio_dev_t *d);     /* the transmit stream down; the writer has stopped */
    /* A device that takes its transmit samples in blocks (rdof/sdr/txguard.h): tx_block samples a push, tx_nbuf
     * blocks queued when a push returns; tx_room the samples the block being filled still takes. 0 and NULL: no guard. The caller sets
     * txguard before start, and write reports each push to it. */
    int tx_block, tx_nbuf;
    int (*tx_room)(rdofradio_dev_t *d);
    rdof_txguard_t *txguard;
    void (*close)(rdofradio_dev_t *d);       /* everything, and frees d */
};

/* A device opened, its TX gain at the least, set up as asked, its streams ready to start. NULL with the reason
 * in err. */
rdofradio_dev_t *rdofradio_soapy_open(const char *args, const rdofradio_dev_setup_t *s, char *err, size_t cap);
rdofradio_dev_t *rdofradio_iio_open(const char *uri, const rdofradio_dev_setup_t *s, char *err, size_t cap);
/* --sim-sdr: a receiver with nothing on the air (noise), for no-RF tests; receive only. */
rdofradio_dev_t *rdofradio_sim_open(const rdofradio_dev_setup_t *s, char *err, size_t cap);

#endif
