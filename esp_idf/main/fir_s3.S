/*
 * The matched filter's inner loop on the S3's SIMD unit (PIE): 8 outputs of a
 * 96-tap int16 FIR, 8 multiply-adds an instruction into the 40-bit ACCX.
 *
 * void rdof_s3_fir8(const int16_t *x, const int16_t *coefs, int32_t *out, const int32_t *round, int shift)
 *   x      a2: 16-byte aligned; output r reads x[0 .. 103] (the last 8 are a look-ahead, any value)
 *   coefs  a3: 16-byte aligned; 8 sets of 96 taps (set r for output r, already shifted by r), then 8 more
 *              int16 the look-ahead reads
 *   out    a4: 8 int32, out[r] = (round + sum(coefs[r][k] * x[k])) >> shift
 *   round  a5: 8-byte aligned {low 32 bits, high 8 bits} that ACCX starts from
 *   shift  a6
 *
 * The inner loop (the four load / multiply-add instructions, the q0 / q1 pre-loads, and the ee.ld.accx.ip /
 * ee.srs.accx framing) is adapted from esp-dsp v1.8.2 modules/fir/fixed/dsps_fird_s16_aes3.S:
 *   SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *   SPDX-License-Identifier: Apache-2.0 (LICENSES/Apache-2.0.txt)
 * Changed here: 8 outputs a call, a coefficient set per output, no delay line or decimation, the calling
 * convention and the pointer fix-ups. The rest of the file is this project's (MIT).
 */
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3

    .text
    .align  4
    .global rdof_s3_fir8
    .type   rdof_s3_fir8, @function
rdof_s3_fir8:
    entry   a1, 32
    movi.n  a7, 8                           // outputs left
.Lfir8_out:
    mov.n   a9, a2                          // data pointer, back to x for each output
    ee.ld.accx.ip   a5, 0                   // ACCX = rounding
    ee.vld.128.ip   q0, a3, 16              // coefficients 0..7
    ee.vld.128.ip   q1, a9, 16              // data 0..7
    movi.n  a8, 6
    loopnez a8, .Lfir8_mac_end              // 6 x 16 taps
        ee.vld.128.ip               q2, a3, 16
        ee.vmulas.s16.accx.ld.ip    q3, a9, 16, q0, q1
        ee.vld.128.ip               q0, a3, 16
        ee.vmulas.s16.accx.ld.ip    q1, a9, 16, q2, q3
.Lfir8_mac_end:
    ee.srs.accx     a10, a6, 0
    s32i.n  a10, a4, 0
    addi.n  a4, a4, 4
    addi    a3, a3, -16                     // the loop loaded one coefficient vector ahead: the next set starts there
    addi.n  a7, a7, -1
    bnez.n  a7, .Lfir8_out
    retw.n
    .size   rdof_s3_fir8, . - rdof_s3_fir8

#endif
