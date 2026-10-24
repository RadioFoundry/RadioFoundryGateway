// SPDX-License-Identifier: MIT AND BSD-3-Clause-Clear
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * LR2021 system and common radio commands (not tied to one modem): chip modes, calibration, measurements, clocks,
 * IRQs, CAD / CCA, timestamps, RSSI and its calibration, register blocks. Opcodes, argument encodings, frame builders
 * and answer parsers as plain C with no I/O; the driver in lr2021.h sends them. Commands and values already in
 * lr2021_cmd.h are not repeated here.
 *
 * Command encodings ported from Semtech's USP LR20xx driver (github.com/Lora-net/usp, commit 540bba0), under the
 * Clear BSD License:
 *
 * The Clear BSD License
 * Copyright Semtech Corporation 2022-2026. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted (subject to the limitations in the disclaimer
 * below) provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Semtech corporation nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE GRANTED BY
 * THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT
 * NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SEMTECH CORPORATION BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Sections cited as "DS n.n" are the LR2021/LR2022/LR2012 datasheet, DS.LR20xx Rev. 2.2. Where the datasheet's byte
 * layout disagrees with the driver above, the chip's ROM 1.24 was checked and the note says which won.
 *
 * Builders write the whole frame (opcode first, fields big-endian) into buf and return its length. Parsers take the
 * answer bytes after the two status bytes, as lr2021_read returns them; LR2021_ANS_* is how many to read.
 */
#ifndef LR2021_RADIO_H
#define LR2021_RADIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lr2021_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opcodes not in lr2021_cmd.h (DS 5.6, tables 5-6 and 5-7) */
enum {
    LR2021_CONFIG_LF_CLOCK = 0x0118,
    LR2021_CONFIG_CLK_OUTPUTS = 0x0119,
    LR2021_GET_VBAT = 0x0124,
    LR2021_GET_TEMP = 0x0125,
    LR2021_GET_RANDOM_NUMBER = 0x0126,
    LR2021_SET_SLEEP = 0x0127,
    LR2021_SET_ADDITIONAL_REG_TO_RETAIN = 0x012A,
    LR2021_SET_LBD_CFG = 0x0130,
    LR2021_SET_XOSC_CP_TRIM = 0x0131,
    LR2021_SET_TEMP_COMP_CFG = 0x0132,
    LR2021_SET_NTC_PARAMS = 0x0133,
    LR2021_SET_RSSI_CALIBRATION = 0x0205,
    LR2021_GET_PACKET_TYPE = 0x0208,
    LR2021_STOP_TIMEOUT_ON_PREAMBLE = 0x0209,
    LR2021_RESET_RX_STATS = 0x020A, /* table 5-7 and the ROM; DS table 6-24 misprints 0x022A (GetLoraPacketStatus) */
    LR2021_SET_RX_DUTY_CYCLE = 0x0210,
    LR2021_SET_AUTO_RX_TX = 0x0211,
    LR2021_GET_RX_PKT_LENGTH = 0x0212,
    LR2021_SET_DEFAULT_RX_TX_TIMEOUT = 0x0215,
    LR2021_SET_TIMESTAMP_SOURCE = 0x0216,
    LR2021_GET_TIMESTAMP_VALUE = 0x0217,
    LR2021_SET_CCA = 0x0218,
    LR2021_GET_CCA_RESULT = 0x0219,
    LR2021_SET_CAD_PARAMS = 0x021B,
    LR2021_SET_CAD = 0x021C,
};

/* Answer lengths: bytes after the two status bytes. */
enum {
    LR2021_ANS_VERSION = 2,         /* GetVersion: major, minor (DS 6.7.2) */
    LR2021_ANS_ERRORS = 2,          /* GetErrors (DS 6.7.4) */
    LR2021_ANS_IRQ = 4,             /* GetAndClearIrqStatus (DS 6.9.2); also GetStatus's 4 bytes after Stat */
    LR2021_ANS_FIFO_FLAGS = 2,      /* GetFifoIrqFlags, GetAndClearFifoIrqFlags: RX, TX (DS 6.10.2, 6.10.4) */
    LR2021_ANS_FIFO_LEVEL = 2,      /* GetRxFifoLevel, GetTxFifoLevel (DS 6.10.5, 6.10.6) */
    LR2021_ANS_VBAT = 2,            /* DS 6.5.1 */
    LR2021_ANS_TEMP = 2,            /* DS 6.5.2 */
    LR2021_ANS_RANDOM = 4,          /* DS 6.6.2 */
    LR2021_ANS_PACKET_TYPE = 1,     /* DS 8.1.2 */
    LR2021_ANS_RSSI_INST = 2,       /* DS 7.3.3; 1 for whole dB */
    LR2021_ANS_RX_PKT_LENGTH = 2,   /* DS 7.3.10 */
    LR2021_ANS_TIMESTAMP = 4,       /* DS 7.3.7 */
    LR2021_ANS_CCA_RESULT = 4,      /* DS 7.3.8; 3 for whole dB */
};

/* The largest frame a builder here writes: SetRssiCalibration with both tables. */
#define LR2021_RADIO_FRAME_MAX 165

/* ── IRQ bits (DS 5.7, table 5-20). RX_FIFO, TX_DONE and TIMEOUT are in lr2021_cmd.h. ── */
#define LR2021_IRQ_TX_FIFO (1u << 1)            /* TX FIFO threshold reached */
#define LR2021_IRQ_RNG_REQ_VALID (1u << 2)      /* RTToF: valid ranging request received (subordinate) */
#define LR2021_IRQ_TX_TIMESTAMP (1u << 3)       /* end of TX, for timestamps only */
#define LR2021_IRQ_RX_TIMESTAMP (1u << 4)       /* end of RX, for timestamps only */
#define LR2021_IRQ_PREAMBLE_DETECTED (1u << 5)
#define LR2021_IRQ_HEADER_VALID (1u << 6)       /* LoRa header detected; a valid sync word for the others */
#define LR2021_IRQ_CAD_DETECTED (1u << 7)
#define LR2021_IRQ_LORA_HDR_TIMESTAMP (1u << 8) /* LoRa header's precise timestamp */
#define LR2021_IRQ_LORA_HEADER_ERR (1u << 9)    /* LoRa header CRC error */
#define LR2021_IRQ_LOW_BATTERY (1u << 10)
#define LR2021_IRQ_PA_OCP_OVP (1u << 11)        /* PA over-current / over-voltage protection */
#define LR2021_IRQ_LORA_TX_RX_HOP (1u << 12)    /* LoRa intra-packet hopping */
#define LR2021_IRQ_SYNC_FAIL (1u << 13)         /* FSK: sync word match failed after detection */
#define LR2021_IRQ_LORA_SYMBOL_END (1u << 14)
#define LR2021_IRQ_LORA_TIMESTAMP_STAT (1u << 15)
#define LR2021_IRQ_ERROR (1u << 16)             /* an error other than a command error (GetErrors) */
#define LR2021_IRQ_CMD_ERROR (1u << 17)         /* a host command failed */
#define LR2021_IRQ_RX_DONE (1u << 18)
#define LR2021_IRQ_CAD_DONE (1u << 20)
#define LR2021_IRQ_CRC_ERROR (1u << 22)
#define LR2021_IRQ_LEN_ERROR (1u << 23)
#define LR2021_IRQ_ADDR_ERROR (1u << 24)
#define LR2021_IRQ_FHSS (1u << 25)              /* LR-FHSS: ramp-up for intra-packet hopping */
#define LR2021_IRQ_INTER_PACKET1 (1u << 26)     /* the host can load a new frequency table */
#define LR2021_IRQ_INTER_PACKET2 (1u << 27)     /* the host can load a new payload */
#define LR2021_IRQ_RNG_RESP_DONE (1u << 28)     /* RTToF: subordinate sent its response */
#define LR2021_IRQ_RNG_REQ_DISCARDED (1u << 29) /* RTToF: request discarded, no address match */
#define LR2021_IRQ_RNG_EXCH_VALID (1u << 30)    /* RTToF: manager got a valid response */
#define LR2021_IRQ_RNG_TIMEOUT (1u << 31)       /* RTToF: manager got no response */
#define LR2021_IRQ_ALL 0xFFFFFFFFu

/* A name for IRQ bit 0 to 31 ("?" past that). */
const char *lr2021_radio_irq_name(int bit);

/* ── Values ─────────────────────────────────────────────────────────────── */

/* SetPacketType / GetPacketType (DS 8.1.1). LORA, FSK and OOK are in lr2021_cmd.h. Guarded: other headers here
 * define some of them too. */
#ifndef LR2021_PACKET_BLE
#define LR2021_PACKET_BLE 0x3
#endif
#ifndef LR2021_PACKET_RTTOF
#define LR2021_PACKET_RTTOF 0x4
#endif
#ifndef LR2021_PACKET_FLRC
#define LR2021_PACKET_FLRC 0x5
#endif
#ifndef LR2021_PACKET_BPSK
#define LR2021_PACKET_BPSK 0x6
#endif
#ifndef LR2021_PACKET_LR_FHSS
#define LR2021_PACKET_LR_FHSS 0x7
#endif
#ifndef LR2021_PACKET_WMBUS
#define LR2021_PACKET_WMBUS 0x8
#endif
#ifndef LR2021_PACKET_WISUN
#define LR2021_PACKET_WISUN 0x9
#endif
#ifndef LR2021_PACKET_ZWAVE
#define LR2021_PACKET_ZWAVE 0xC
#endif
#ifndef LR2021_PACKET_OQPSK
#define LR2021_PACKET_OQPSK 0xD /* O-QPSK 15.4 */
#endif

/* SetDioFunction's other functions (DS 6.8.1; the first ones are in lr2021_cmd.h) and its sleep pulls. */
enum {
    LR2021_DIO_HF_CLK_OUT = 0x7,
    LR2021_DIO_LF_CLK_OUT = 0x8, /* DIO7 to DIO11 only */
    LR2021_DIO_TX_TRIGGER = 0x9, /* input: high starts SetTx with the default timeout */
    LR2021_DIO_RX_TRIGGER = 0xA, /* input: high starts SetRx with the default timeout */
};
enum { LR2021_PULL_NONE = 0, LR2021_PULL_DOWN = 1, LR2021_PULL_UP = 2, LR2021_PULL_AUTO = 3 };

/* Every block Calibrate knows (DS table 6-28). */
#define LR2021_CAL_ALL 0x6F

/* SetRegMode (DS 6.3.18): LDO only (default), or the SIMO DC-DC for VDCC and the PA supply in TX. */
enum { LR2021_REG_LDO = 0x0, LR2021_REG_SIMO = 0x2 };
/* ConfigLfClock (DS 6.11.1): the internal 32 kHz RC (default), or 32.768 kHz on DIO11. */
enum { LR2021_LF_CLOCK_RC = 0x0, LR2021_LF_CLOCK_DIO11 = 0x2 };
/* GetTemp's sources (DS 6.5.2). */
enum { LR2021_TEMP_VBE = 0, LR2021_TEMP_XOSC = 1, LR2021_TEMP_NTC = 2 };
/* GetRandomNumber's entropy sources (DS 6.6.2); both is the default and the advice. */
enum { LR2021_RNG_PLL = 0x1, LR2021_RNG_ADC = 0x2 };
/* SetTempCompCfg (DS 6.12.1). */
enum { LR2021_TEMP_COMP_OFF = 0, LR2021_TEMP_COMP_RELATIVE = 1, LR2021_TEMP_COMP_ABSOLUTE = 2 };
/* SetRxDutyCycle's mode (DS 6.3.8): plain RX windows, or a LoRa CAD on each wake-up. */
enum { LR2021_DUTY_RX = 0, LR2021_DUTY_LORA_CAD = 1 };
/* SetAutoRxTx's mode (DS 6.3.9). */
enum { LR2021_AUTO_OFF = 0, LR2021_AUTO_ALWAYS = 1, LR2021_AUTO_RX_OK = 2 };
/* SetTimestampSource's events (DS 7.3.6); three slots, index 0 to 2. */
enum {
    LR2021_TS_NONE = 0,
    LR2021_TS_LAST_BIT_SENT = 1,
    LR2021_TS_LAST_BIT_RECEIVED = 2,
    LR2021_TS_FSK_SYNC = 3,   /* any FSK modem, W-MBUS and BLE included */
    LR2021_TS_LORA_HEADER = 4,
};
/* SetCadParams' exit mode (DS 6.3.14): back to the fallback mode, TX if the channel stayed quiet, RX on activity. */
enum { LR2021_CAD_EXIT_FALLBACK = 0, LR2021_CAD_EXIT_TX = 1, LR2021_CAD_EXIT_RX = 2 };
/* SetCca's gain step: 0 the AGC, 1 to 13 fixed, or this to leave it alone (DS 6.3.15). */
#define LR2021_CCA_GAIN_KEEP 0xFF
/* SetTxTestMode's 0: a normal TX, as SetTx(0) (DS 21.2). The others are in lr2021_cmd.h. */
#define LR2021_TEST_NORMAL 0

/* ── Time units ─────────────────────────────────────────────────────────── */

/* us in 32.768 kHz RTC steps (SetRx / SetTx timeouts, duty cycle, default timeouts), rounded, at most 0xFFFFFE
 * (512 s): 0xFFFFFF means continuous RX and is never made here. */
uint32_t lr2021_radio_rtc_steps(uint64_t us);
/* us in 32 MHz clock ticks (SetCca, SetCadParams' cad_timeout), at most 0xFFFFFF (524 ms). */
uint32_t lr2021_radio_hf_ticks(uint32_t us);

/* ── Chip modes (DS 6.3) ────────────────────────────────────────────────── */

/* SetSleep (DS 6.3.1): clk_32k keeps the 32 kHz clock running; retain keeps the context (else a cold start on
 * wake-up); sleep_time in LF clock periods wakes the chip after it (with clk_32k; 0: until NSS). 7 bytes. */
size_t lr2021_radio_set_sleep(uint8_t *buf, bool clk_32k, bool retain, uint32_t sleep_time);
/* SetStandby (DS 6.3.2): RC, or XOSC (the crystal running). 3 bytes. */
size_t lr2021_radio_set_standby(uint8_t *buf, bool xosc);
/* SetFs (DS 6.3.3). 2 bytes. */
size_t lr2021_radio_set_fs(uint8_t *buf);
/* SetAdditionalRegToRetain (DS 6.3.4): one more register kept through sleep, slot 0 to 31, a word address. 6 bytes. */
size_t lr2021_radio_add_retained_reg(uint8_t *buf, uint8_t slot, uint32_t addr);
/* SetRx / SetTx with no timeout: the one SetDefaultRxTxTimeout set (DS 6.3.5, 6.3.6). 2 bytes. */
size_t lr2021_radio_set_rx_default(uint8_t *buf);
size_t lr2021_radio_set_tx_default(uint8_t *buf);
/* SetRxTxFallbackMode (DS 6.3.7): a LR2021_FALLBACK_*. 3 bytes. */
size_t lr2021_radio_set_fallback(uint8_t *buf, uint8_t mode);
/* SetRxDutyCycle (DS 6.3.8): RX windows of rx_max_time every cycle_time (RTC steps, 24 bits), sleeping between;
 * mode a LR2021_DUTY_*. cycle_time under rx_max_time is refused by the chip. 9 bytes. */
size_t lr2021_radio_set_rx_duty_cycle(uint8_t *buf, uint32_t rx_max_time, uint32_t cycle_time, uint8_t mode);
/* SetAutoRxTx (DS 6.3.9): after RX done go to TX (or after TX done to RX) once, delay 32 MHz ticks later, with a
 * timeout in RTC steps; clear stops it on a timeout or a bad packet. mode a LR2021_AUTO_*. 10 bytes. */
size_t lr2021_radio_set_auto_rx_tx(uint8_t *buf, bool clear, uint8_t mode, uint32_t timeout, uint32_t delay);
/* StopTimeoutOnPreamble (DS 6.3.10): the RX timeout stops on a preamble (true) or on the sync word / header
 * (false, the default). 3 bytes. */
size_t lr2021_radio_stop_timeout_on_preamble(uint8_t *buf, bool on_preamble);
/* SetCadParams (DS 6.3.14): the RSSI CAD of the non-LoRa modems. cad_timeout in 32 MHz ticks, the threshold in dBm
 * (-80 is 0x50; clamped to -255..0), exit a LR2021_CAD_EXIT_*, trx_timeout the RX / TX timeout after it (RTC
 * steps). 10 bytes. */
size_t lr2021_radio_set_cad_params(uint8_t *buf, uint32_t cad_timeout, int threshold_dbm, uint8_t exit,
                                   uint32_t trx_timeout);
/* SetCad (DS 6.3.13). 2 bytes. */
size_t lr2021_radio_set_cad(uint8_t *buf);
/* SetCca (DS 6.3.15): RX for duration 32 MHz ticks to measure RSSI, then a Timeout IRQ; gain_step 0 (AGC), 1 to
 * 13, or LR2021_CCA_GAIN_KEEP. 6 bytes. */
size_t lr2021_radio_set_cca(uint8_t *buf, uint32_t duration, uint8_t gain_step);
/* SetDefaultRxTxTimeout (DS 6.3.17): the timeouts (RTC steps) for DIO triggers and SetRx / SetTx without one.
 * 8 bytes. */
size_t lr2021_radio_set_default_timeouts(uint8_t *buf, uint32_t rx, uint32_t tx);
/* SetRegMode (DS 6.3.18), in standby RC only: LR2021_REG_*. 3 bytes. */
size_t lr2021_radio_set_reg_mode(uint8_t *buf, uint8_t mode);
/* Calibrate (DS 6.4.1): LR2021_CAL_* blocks (others dropped). 3 bytes. */
size_t lr2021_radio_calibrate(uint8_t *buf, uint8_t blocks);
/* SetTxTestMode (DS 21.2): LR2021_TEST_NORMAL, LR2021_TEST_PREAMBLE, _CW or _PRBS9. 3 bytes. */
size_t lr2021_radio_set_tx_test_mode(uint8_t *buf, uint8_t mode);

/* ── Measurements (DS 6.5, 6.6) ─────────────────────────────────────────── */

/* GetVbat (DS 6.5.1) in mV (or raw), measured to bits of resolution (8 to 13, clamped). 3 bytes. */
size_t lr2021_radio_get_vbat(uint8_t *buf, bool mv, int bits);
/* The supply in volts: mV / 1000, or from raw as (5 x raw / 8192 - 1) x 1.35 V (Vana, typical). */
double lr2021_radio_vbat_v(const uint8_t *ans, bool mv);
/* GetTemp (DS 6.5.2) from a LR2021_TEMP_* source, in degrees C (or raw), bits of resolution (8 to 13). 3 bytes. */
size_t lr2021_radio_get_temp(uint8_t *buf, uint8_t source, bool celsius, int bits);
/* The temperature in degrees C. celsius: a signed 13-bit count of 1/32 degree in bits 15:3. Raw: a 13-bit count in
 * bits 12:0, (raw / 8192 x 1.35 - 0.7295) x 1000 / -1.7 + 25 with the typical Vana, Vbe25 and slope (Vbe sources). */
double lr2021_radio_temp_c(const uint8_t *ans, bool celsius);
/* The 13-bit raw count of a GetVbat or GetTemp answer in raw format. */
uint16_t lr2021_radio_meas_raw(const uint8_t *ans);
/* An NTC's temperature from a raw GetTemp count, the bias ratio and beta (K) given to SetNtcParams (DS 6.5.2). */
double lr2021_radio_ntc_temp_c(uint16_t raw, double r_bias_ratio, double beta_k);
/* SetLbdCfg (DS 6.6.1): the low battery detector on or off and its threshold trim (0 1.60 V, 1 1.67, 2 1.74,
 * 3 1.80, 4 1.88 (default), 5 1.95, 6 2.00, 7 2.10). Enable in bit 0, the trim in bits 3:1, as the ROM reads it
 * (the datasheet's drawing puts the trim at the top). 3 bytes. */
size_t lr2021_radio_set_lbd(uint8_t *buf, bool enable, uint8_t trim);
/* GetRandomNumber (DS 6.6.2) from LR2021_RNG_* sources. Not for cryptography. 3 bytes. */
size_t lr2021_radio_get_random(uint8_t *buf, uint8_t sources);
uint32_t lr2021_radio_random(const uint8_t *ans);

/* ── Status, IRQs, FIFOs (DS 6.7 to 6.10) ───────────────────────────────── */

/* GetVersion (DS 6.7.2): major << 8 | minor (0x0118 for an LR2021). */
uint16_t lr2021_radio_version(const uint8_t *ans);
/* GetErrors (DS 6.7.4): the LR2021_ERR_* word. */
uint16_t lr2021_radio_errors(const uint8_t *ans);
/* GetAndClearIrqStatus (DS 6.9.2), or the 4 bytes after Stat in GetStatus: the LR2021_IRQ_* word. */
uint32_t lr2021_radio_irq(const uint8_t *ans);
/* GetFifoIrqFlags / GetAndClearFifoIrqFlags (DS 6.10.2, 6.10.4): the LR2021_FIFO_* flags of each FIFO. */
typedef struct {
    uint8_t rx, tx;
} lr2021_fifo_flags_t;
lr2021_fifo_flags_t lr2021_radio_fifo_flags(const uint8_t *ans);
/* GetRxFifoLevel / GetTxFifoLevel (DS 6.10.5, 6.10.6): bytes in the FIFO. */
uint16_t lr2021_radio_fifo_level(const uint8_t *ans);

/* ── Clocks and temperature compensation (DS 6.11, 6.12) ────────────────── */

/* ConfigLfClock (DS 6.11.1): LR2021_LF_CLOCK_*. 3 bytes. */
size_t lr2021_radio_config_lf_clock(uint8_t *buf, uint8_t source);
/* ConfigClkOutputs (DS 6.11.2): the HF clock on a DIO set to LR2021_DIO_HF_CLK_OUT, 32 MHz >> scaling (0 to 15). */
size_t lr2021_radio_config_clk_outputs(uint8_t *buf, uint8_t scaling);
/* SetXoscCpTrim (DS 6.11.4): the crystal's foot capacitors (codes 0 to 47, clamped) and extra start time in us.
 * 5 bytes. */
size_t lr2021_radio_set_xosc_trim(uint8_t *buf, uint8_t xta, uint8_t xtb, uint8_t start_us);
/* The trim code nearest pf: XTA 11.3 pF + 0.47 pF a step, XTB 11.1 pF + 0.47 (tables 6-67, 6-68), clamped. */
uint8_t lr2021_radio_xosc_trim_code(double pf, bool xtb);
/* SetTempCompCfg (DS 6.12.1): LR2021_TEMP_COMP_*, and whether an NTC is fitted. Fails with a TCXO. 3 bytes. */
size_t lr2021_radio_set_temp_comp(uint8_t *buf, uint8_t mode, bool ntc);
/* SetNtcParams (DS 6.12.2): the bias-to-NTC(25 C) resistor ratio (0 to 1.998, 1/512 steps), beta in K (2 K steps,
 * to 8190), the first-order delay coefficient. Clamped. 7 bytes. */
size_t lr2021_radio_set_ntc_params(uint8_t *buf, double r_ratio, double beta_k, uint8_t delay);

/* ── Common radio (DS 7.3, 7.4, 8.1) ────────────────────────────────────── */

/* GetRssiInst (DS 7.3.3): 2 bytes. The answer in dBm: -(byte 0 << 1 | byte 1 bit 0) / 2. Bit 0, as the ROM writes
 * it and the driver reads it (the datasheet's drawing puts it at bit 7). */
size_t lr2021_radio_get_rssi_inst(uint8_t *buf);
double lr2021_radio_rssi_dbm(const uint8_t *ans);

/* SetRssiCalibration (DS 7.3.4): a path's 27 gain entries, G1 to G11, G12 boost 0 to 7, G13 boost 0 to 7. */
#define LR2021_RSSI_CAL_ENTRIES 27
typedef struct {
    int16_t gain[LR2021_RSSI_CAL_ENTRIES]; /* front-end gain, 0.25 dB steps, signed 10 bits */
    uint8_t nf[LR2021_RSSI_CAL_ENTRIES];   /* noise figure, 0.25 dB steps */
} lr2021_rssi_cal_t;
/* The LF table, the HF table or both (NULL: that path left alone; LF first). 3 + 81 bytes a table, at most
 * LR2021_RADIO_FRAME_MAX. */
size_t lr2021_radio_set_rssi_cal(uint8_t *buf, const lr2021_rssi_cal_t *lf, const lr2021_rssi_cal_t *hf);
/* The datasheet's tables for its EVK: LF 868-915 MHz and HF 2.4 GHz (loaded at boot), LF 490 MHz (which the
 * application must load; tables 7-6 to 7-8). */
extern const lr2021_rssi_cal_t lr2021_rssi_cal_lf_915;
extern const lr2021_rssi_cal_t lr2021_rssi_cal_hf_2g4;
extern const lr2021_rssi_cal_t lr2021_rssi_cal_lf_490;

/* ResetRxStats (DS 7.3.5). 2 bytes. */
size_t lr2021_radio_reset_rx_stats(uint8_t *buf);
/* SetTimestampSource (DS 7.3.6): slot 0 to 2 timestamps a LR2021_TS_* event. 3 bytes. */
size_t lr2021_radio_set_timestamp_source(uint8_t *buf, uint8_t index, uint8_t source);
/* GetTimestampValue (DS 7.3.7): 32 MHz ticks from slot index's event to this request's NSS falling edge. 3 bytes. */
size_t lr2021_radio_get_timestamp(uint8_t *buf, uint8_t index);
uint32_t lr2021_radio_timestamp_ticks(const uint8_t *ans);
double lr2021_radio_timestamp_us(const uint8_t *ans);
/* GetCcaResult (DS 7.3.8): the last CCA's RSSI, in dBm (half dB). 2 bytes. */
size_t lr2021_radio_get_cca_result(uint8_t *buf);
typedef struct {
    double min_dbm, max_dbm, avg_dbm;
} lr2021_cca_t;
/* n: the answer bytes read, 4 (half dB) or 3 (whole dB). */
lr2021_cca_t lr2021_radio_cca(const uint8_t *ans, size_t n);
/* GetRxPktLength (DS 7.3.10): the last payload's length in bytes. 2 bytes. */
size_t lr2021_radio_get_rx_pkt_length(uint8_t *buf);
uint16_t lr2021_radio_rx_pkt_length(const uint8_t *ans);
/* SelPa (DS 7.4.2): switch to the LF or the HF PA, both already set up by SetPaConfig. 3 bytes. */
size_t lr2021_radio_sel_pa(uint8_t *buf, bool hf);
/* SetPacketType (DS 8.1.1), in standby or FS only. 3 bytes. */
size_t lr2021_radio_set_packet_type(uint8_t *buf, uint8_t type);
/* GetPacketType (DS 8.1.2). 2 bytes. */
size_t lr2021_radio_get_packet_type(uint8_t *buf);
uint8_t lr2021_radio_packet_type(const uint8_t *ans);

/* ── Register blocks (DS 6.2) ───────────────────────────────────────────── */

/* WriteRegMem32 of n words (1 to 32; others give 0) from addr, auto-incrementing (DS 6.2.1). 5 + 4n bytes. */
size_t lr2021_radio_write_regs(uint8_t *buf, uint32_t addr, const uint32_t *words, int n);
/* WriteRegMemMask32 (DS 6.2.2): only the bits set in mask take value's. 13 bytes. */
size_t lr2021_radio_write_reg_mask(uint8_t *buf, uint32_t addr, uint32_t mask, uint32_t value);
/* ReadRegMem32's answer (DS 6.2.3, request: lr2021_cmd_read_reg32): n big-endian words into out. */
void lr2021_radio_words(const uint8_t *ans, int n, uint32_t *out);

#ifdef __cplusplus
}
#endif

#endif
