// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Semtech LR2021 host commands: opcodes, argument encodings and the status
 * word, as plain C with no I/O (the driver in lr2021.h sends them). Written
 * from the LR2021/LR2022/LR2012 datasheet (DS.LR20xx Rev. 2.2), chapters 5 to
 * 8; the section or table each comes from is noted.
 *
 * A command is a 16-bit opcode, big-endian, then its arguments, all
 * multi-byte fields big-endian (5.4.1). Builders write the whole frame into
 * buf and return its length.
 */
#ifndef LR2021_CMD_H
#define LR2021_CMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opcodes (5.6, tables 5-4 to 5-8) */
enum {
    LR2021_READ_RX_FIFO = 0x0001,   /* direct read: data follows the two status bytes in the same frame */
    LR2021_WRITE_TX_FIFO = 0x0002,  /* direct write */
    LR2021_WRITE_REG_MEM32 = 0x0104,
    LR2021_WRITE_REG_MEM_MASK32 = 0x0105,
    LR2021_READ_REG_MEM32 = 0x0106,
    LR2021_GET_STATUS = 0x0100,
    LR2021_GET_VERSION = 0x0101,
    LR2021_GET_ERRORS = 0x0110,
    LR2021_CLEAR_ERRORS = 0x0111,
    LR2021_SET_DIO_FUNCTION = 0x0112,
    LR2021_SET_DIO_RF_SWITCH = 0x0113,
    LR2021_CLEAR_FIFO_IRQ_FLAGS = 0x0114,
    LR2021_SET_DIO_IRQ_CONFIG = 0x0115,
    LR2021_CLEAR_IRQ = 0x0116,
    LR2021_GET_AND_CLEAR_IRQ = 0x0117,
    LR2021_CONFIG_FIFO_IRQ = 0x011A,
    LR2021_GET_FIFO_IRQ_FLAGS = 0x011B,
    LR2021_GET_RX_FIFO_LEVEL = 0x011C,
    LR2021_GET_TX_FIFO_LEVEL = 0x011D,
    LR2021_CLEAR_RX_FIFO = 0x011E,
    LR2021_CLEAR_TX_FIFO = 0x011F,
    LR2021_SET_TCXO_MODE = 0x0120,
    LR2021_SET_REG_MODE = 0x0121,
    LR2021_CALIBRATE = 0x0122,
    LR2021_CALIB_FE = 0x0123,
    LR2021_SET_STANDBY = 0x0128,
    LR2021_SET_FS = 0x0129,
    LR2021_GET_AND_CLEAR_FIFO_IRQ_FLAGS = 0x012E,
    LR2021_SET_RF_FREQUENCY = 0x0200,
    LR2021_SET_RX_PATH = 0x0201,
    LR2021_SET_PA_CONFIG = 0x0202,
    LR2021_SET_TX_PARAMS = 0x0203,
    LR2021_SET_FALLBACK_MODE = 0x0206,
    LR2021_SET_PACKET_TYPE = 0x0207,
    LR2021_GET_RSSI_INST = 0x020B,
    LR2021_SET_RX = 0x020C,
    LR2021_SET_TX = 0x020D,
    LR2021_SEL_PA = 0x020F,
    LR2021_SET_AGC_GAIN_MANUAL = 0x021A,
    LR2021_SET_LORA_MODULATION = 0x0220,
    LR2021_SET_LORA_PACKET = 0x0221,
    LR2021_SET_LORA_SYNCWORD = 0x0223,
    LR2021_SET_FSK_MODULATION = 0x0240,
    LR2021_SET_FSK_PACKET = 0x0241,
    LR2021_SET_TX_TEST_MODE = 0x020E,
    LR2021_SET_OOK_MODULATION = 0x0281,
    /* Not in the datasheet: ROM 1.24's raw sample paths and FSK variant (docs/spec/lr2021-iq.md) */
    LR2021_SET_FSK_VARIANT = 0x023B,
    LR2021_SET_FSK_DEMOD_OPT = 0x022F, /* one byte into 0xF30C28 bits 28:24, 22:21 and 20 (spec R3.42) */
    LR2021_RAW_ROUTE = 0x0290,
    LR2021_RAW_TX = 0x0293,
};

/* SetPacketType (table 8-1) */
enum { LR2021_PACKET_LORA = 0x0, LR2021_PACKET_FSK = 0x2, LR2021_PACKET_OOK = 0xA };
/* RFU in the datasheet; ROM 1.24 treats it as raw I/Q (spec R3.22) */
#define LR2021_PACKET_RAW_IQ 0xB
/* LoRa bandwidth codes (table 9-3) */
enum { LR2021_LORA_BW_31 = 0x2, LR2021_LORA_BW_62 = 0x3, LR2021_LORA_BW_125 = 0x4 };
/* IRQ bits (table 5-20) */
#define LR2021_IRQ_RX_FIFO (1u << 0) /* the RX FIFO flags ConfigFifoIrq enables */
#define LR2021_IRQ_TX_DONE (1u << 19)
#define LR2021_IRQ_TIMEOUT (1u << 21)

/* SetRxTxFallbackMode (6.3.7) */
enum { LR2021_FALLBACK_STBY_RC = 1, LR2021_FALLBACK_STBY_XOSC = 2, LR2021_FALLBACK_FS = 3 };
/* SetTxTestMode (21.2): 1 infinite preamble, 2 continuous wave, 3 PRBS9 */
enum { LR2021_TEST_PREAMBLE = 1, LR2021_TEST_CW = 2, LR2021_TEST_PRBS9 = 3 };

/* GetVersion's answer for an LR2021 (table 6-40): firmware 1.24. */
#define LR2021_VERSION_MAJOR 0x01
#define LR2021_VERSION_MINOR 0x18

/* Calibrate's blocks (table 6-28) */
enum {
    LR2021_CAL_LF_RC = 0x01,
    LR2021_CAL_HF_RC = 0x02,
    LR2021_CAL_PLL = 0x04,
    LR2021_CAL_AAF = 0x08,
    LR2021_CAL_MU = 0x20,
    LR2021_CAL_PA_OFF = 0x40,
};

/* GetErrors bits (table 6-43) */
enum {
    LR2021_ERR_HF_XOSC_START = 1 << 0,
    LR2021_ERR_LF_XOSC_START = 1 << 1,
    LR2021_ERR_PLL_LOCK = 1 << 2,
    LR2021_ERR_LF_RC_CALIB = 1 << 3,
    LR2021_ERR_HF_RC_CALIB = 1 << 4,
    LR2021_ERR_PLL_CALIB = 1 << 5,
    LR2021_ERR_AAF_CALIB = 1 << 6,
    LR2021_ERR_IMG_CALIB = 1 << 7,
    LR2021_ERR_CHIP_BUSY = 1 << 8,
    LR2021_ERR_RXFREQ_NO_FE_CAL = 1 << 9,
    LR2021_ERR_MEAS_UNIT_ADC_CALIB = 1 << 10,
    LR2021_ERR_PA_OFFSET_CALIB = 1 << 11,
};

/* FIFO flags (6.10.1) */
enum {
    LR2021_FIFO_EMPTY = 0x01,
    LR2021_FIFO_LOW = 0x02,
    LR2021_FIFO_HIGH = 0x04,
    LR2021_FIFO_FULL = 0x08,
    LR2021_FIFO_OVERFLOW = 0x10,
    LR2021_FIFO_UNDERFLOW = 0x20,
};

/* SetDioFunction's functions (6.8.1) */
enum {
    LR2021_DIO_NONE = 0x0,
    LR2021_DIO_IRQ = 0x1,
    LR2021_DIO_RF_SWITCH = 0x2,
    LR2021_DIO_OUT_LOW = 0x5,
    LR2021_DIO_OUT_HIGH = 0x6,
};

/* SetDioRfSwitchConfig's modes (table 6-45): the DIO is high in each mode whose bit is set. */
enum {
    LR2021_SW_STANDBY = 0x01,
    LR2021_SW_RX_LF = 0x02,
    LR2021_SW_TX_LF = 0x04,
    LR2021_SW_RX_HF = 0x08,
    LR2021_SW_TX_HF = 0x10,
};

/* The status word sent at the start of every frame (table 6-38). */
typedef struct {
    uint8_t cmd;        /* 0 FAIL, 1 PERR (bad opcode or arguments), 2 OK, 3 DATA */
    bool irq;           /* an interrupt is pending */
    uint8_t reset;      /* the first reset source: 0 cleared, 1 POR / brown-out, 2 NRESET pin */
    uint8_t mode;       /* 0 sleep, 1 standby RC, 2 standby XOSC, 3 FS, 4 RX, 5 TX */
} lr2021_status_t;

enum { LR2021_CMD_FAIL = 0, LR2021_CMD_PERR = 1, LR2021_CMD_OK = 2, LR2021_CMD_DATA = 3 };
enum { LR2021_MODE_SLEEP, LR2021_MODE_STBY_RC, LR2021_MODE_STBY_XOSC, LR2021_MODE_FS, LR2021_MODE_RX, LR2021_MODE_TX };

lr2021_status_t lr2021_status(uint8_t hi, uint8_t lo);
const char *lr2021_mode_name(uint8_t mode);
/* The first set error bit's name ("" for none). */
const char *lr2021_error_name(uint16_t errors);

/* Frame builders: the whole frame (opcode first) into buf, which must hold 12 bytes; returns its length. */
size_t lr2021_cmd(uint8_t *buf, uint16_t op);
size_t lr2021_cmd_u8(uint8_t *buf, uint16_t op, uint8_t a);
size_t lr2021_cmd_set_rf_frequency(uint8_t *buf, uint32_t hz);
size_t lr2021_cmd_set_rx_path(uint8_t *buf, bool hf, uint8_t boost);
/* Up to three frequencies (0 for none) on a path: 4 MHz steps, rounded down as the chip does for its own; bit 15 HF. */
size_t lr2021_cmd_calib_fe(uint8_t *buf, const uint32_t *hz, int n, bool hf);
uint16_t lr2021_calib_fe_word(uint32_t hz, bool hf);
size_t lr2021_cmd_set_tcxo(uint8_t *buf, uint8_t tune, uint32_t start_32mhz_periods);
size_t lr2021_cmd_set_dio_function(uint8_t *buf, uint8_t dio, uint8_t func, uint8_t sleep_pull);
size_t lr2021_cmd_set_dio_rf_switch(uint8_t *buf, uint8_t dio, uint8_t modes);
/* SetDioRfSwitchConfig's mode bits: the states a DIO is high in */
enum { LR2021_RFSW_STANDBY = 0x01, LR2021_RFSW_RX_LF = 0x02, LR2021_RFSW_TX_LF = 0x04, LR2021_RFSW_RX_HF = 0x08,
       LR2021_RFSW_TX_HF = 0x10 };
size_t lr2021_cmd_set_dio_irq(uint8_t *buf, uint8_t dio, uint32_t irqs);
size_t lr2021_cmd_clear_irq(uint8_t *buf, uint32_t irqs);
size_t lr2021_cmd_clear_fifo_flags(uint8_t *buf, uint8_t rx, uint8_t tx);
size_t lr2021_cmd_config_fifo_irq(uint8_t *buf, uint8_t rx_en, uint8_t tx_en, uint16_t rx_high, uint16_t tx_low,
                                  uint16_t rx_low, uint16_t tx_high);
/* rx / tx timeout in 32 kHz RTC periods (24 bits); 0xFFFFFF: continuous until told otherwise (6.3.5). */
size_t lr2021_cmd_set_rx(uint8_t *buf, uint32_t timeout);
size_t lr2021_cmd_set_tx(uint8_t *buf, uint32_t timeout);
/* 0: the chip's AGC; 1 to 13: a fixed gain step (7.3.9). */
size_t lr2021_cmd_set_gain(uint8_t *buf, uint8_t step);
size_t lr2021_cmd_set_tx_params(uint8_t *buf, int8_t power_code, uint8_t ramp_code);
/* SetPaConfig (7.4.2): hf selects the HF PA; the LF PA's duty cycle and slices (DS table 7-19: 7 and 7 for +22
 * dBm at 915 MHz), the HF PA's duty cycle (16 to 31). */
size_t lr2021_cmd_set_pa_config(uint8_t *buf, bool hf, uint8_t lf_duty, uint8_t lf_slices, uint8_t hf_duty);
size_t lr2021_cmd_write_reg32(uint8_t *buf, uint32_t addr, uint32_t value);
size_t lr2021_cmd_read_reg32(uint8_t *buf, uint32_t addr, uint8_t words);

/* SetTxParams' power on the LF PA: half dB from -9.5 to +22 dBm (code -19 to 44), rounded down, clamped. */
int8_t lr2021_tx_power_code(double dbm);
double lr2021_tx_power_dbm(int8_t code);
/* SetTxParams' ramp: the shortest ramp of table 7-24 at least us long (the longest, 304 us, past that). */
uint8_t lr2021_ramp_code(int us);
int lr2021_ramp_us(uint8_t code);

/* A frequency in Hz with a crystal correction: positive ppm for a fast crystal, as the RTL's (the chip then gets
 * a lower number, so the signal lands where it should). */
uint32_t lr2021_freq_corrected(double hz, double ppm);

/* SetLoraModulationParams (9.9.1): sf 5..12, bw a LR2021_LORA_BW_* code, cr, ldro. */
size_t lr2021_cmd_lora_modulation(uint8_t *buf, uint8_t sf, uint8_t bw, uint8_t cr, uint8_t ldro);
/* SetLoraPacketParams (9.9.3): preamble symbols, payload length, then the header / CRC / IQ byte as given. */
size_t lr2021_cmd_lora_packet(uint8_t *buf, uint16_t preamble, uint8_t payload_len, uint8_t flags);
/* SetFskModulationParams (11.3.1): bit rate (bits a second), pulse shape, rx_bw (table 11-4's code), fdev Hz. */
size_t lr2021_cmd_fsk_modulation(uint8_t *buf, uint32_t bitrate, uint8_t shape, uint8_t rx_bw, uint32_t fdev_hz);
/* SetFskPacketParams (11.3.2), fixed length, no address, no CRC, no whitening: preamble bits, payload bytes. */
size_t lr2021_cmd_fsk_packet(uint8_t *buf, uint16_t preamble_bits, uint16_t payload_len);
/* SetOokModulationParams (16.3.1): bit rate (bits a second), pulse shape, rx_bw, depth (0 full, 1 up to 20 dB). */
size_t lr2021_cmd_ook_modulation(uint8_t *buf, uint32_t bitrate, uint8_t shape, uint8_t rx_bw, uint8_t depth);
/* The FSK modem's variant (0x023B, not in the datasheet): 0 plain 2-FSK, 1 4-FSK (spec R3.20). */
size_t lr2021_cmd_fsk_variant(uint8_t *buf, uint8_t variant);

/* ── ROM 1.24's raw sample paths: facts from docs/spec/lr2021-iq.md, not the datasheet ── */

/* The memory word that holds 0x600DB002 while a Semtech patch runs (spec R1.2). */
#define LR2021_PATCH_MARKER_ADDR 0x00800FF8u
#define LR2021_PATCH_ACTIVE 0x600DB002u
/* The raw route to the RX FIFO (spec 2.2): 13 bytes; buf must hold them. */
size_t lr2021_cmd_raw_route_fifo(uint8_t *buf);
/* The route's sources measured on the FIFO (spec R2.20): 1, int8 I then Q after the channel filter; 2, int16 I
 * then Q after it (the one lr2021_cmd_raw_route_fifo picks). */
enum { LR2021_RAW_SRC_CS8 = 1, LR2021_RAW_SRC_CS16 = 2 };
/* The route to the RX FIFO from a source at a rate code (0xFF: the modem's own, as set by its modulation
 * params); 13 bytes. */
size_t lr2021_cmd_raw_route(uint8_t *buf, uint8_t rate, uint8_t source);
/* The route through the rate converter (sources 0 and 6; spec R2.21): the chip's rate from the rate code, converted
 * to exactly out_hz S/s (bytes 1-4; the converter's ratio, rate / out_hz, must stay under 4). 13 bytes. */
enum { LR2021_RAW_SRC_CONV = 0, LR2021_RAW_SRC_CONV_FREQ = 6 };
size_t lr2021_cmd_raw_route_rate(uint8_t *buf, uint8_t rate, uint32_t out_hz, uint8_t source);
/* The rate code for the converter to make out_hz from: the fastest whose channel filter (lr2021_fsk_filter_share of
 * its rate) stays within share of out_hz, at least out_hz and under 4 x out_hz; -1 if none. */
int lr2021_conv_rx_bw_for(double out_hz, double share);
/* Bytes a sample on the FIFO for a route source (the ROM's table; 0 past it). */
int lr2021_raw_sample_bytes(uint8_t source);
/* An FSK rx_bw code read as the route's rate (spec R2.19): bits 2:0 log2(decimation / 4), bits 5:3 mode - 1,
 * bits 7:6 the channel filter. The sample rate, 32 MHz / (mode x decimation), as a fraction: num / den S/s. */
void lr2021_fsk_rx_rate(uint8_t rx_bw, uint32_t *num, uint32_t *den);
/* The share of the rate the channel filter of an rx_bw code passes within 3 dB (measured: 0.37 to 0.38 for
 * bits 7:6 = 0, then 0.34, 0.30, 0.27). */
double lr2021_fsk_filter_share(uint8_t rx_bw);
/* The code with the widest filter and the lowest rate at least rate S/s, no faster than max_rate (of equal ones,
 * the larger decimation); -1 if none. */
int lr2021_fsk_rx_bw_for(double rate, double max_rate);
/* The receive rate for a LoRa bandwidth code: 4 x the bandwidth (spec R2.8); 0 for one not measured. */
uint32_t lr2021_raw_rx_rate(uint8_t bw);
/* Raw transmit of n samples at rate S/s (spec R3.1); selector LR2021_RAW_TX_FREQ for frequency samples. */
enum { LR2021_RAW_TX_IQ = 0, LR2021_RAW_TX_FREQ = 1, LR2021_RAW_TX_PHASE = 2 };
size_t lr2021_cmd_raw_tx(uint8_t *buf, uint16_t n, uint32_t rate, uint8_t selector);
/* A frequency sample for LR2021_RAW_TX_FREQ: hz x 64 / fdev, rounded and clamped to -128..127 (spec R3.2). */
int8_t lr2021_raw_freq_sample(float hz, float fdev_hz);

/* The PLL's frequency word (spec R3.30): written during TX it retunes the carrier live, with no dip. */
#define LR2021_REG_PLL_FREQ 0x00F40144u
/* hz (already corrected for the crystal) as the PLL word: round(hz x 2^20 / 10^6), 0.954 Hz steps. */
uint32_t lr2021_pll_word(uint32_t hz);
/* WriteRegMem32 of the PLL word for hz: 9 bytes. */
size_t lr2021_cmd_retune(uint8_t *buf, uint32_t hz);

/* The extended (1024-byte) RX FIFO's registers and values (5.3.3.1.1). */
#define LR2021_REG_RX_FIFO_BASE 0x00F30028u
#define LR2021_REG_RX_FIFO_SIZE 0x00F30030u
#define LR2021_REG_TX_FIFO_BASE 0x00F3002Cu
#define LR2021_REG_TX_FIFO_SIZE 0x00F30034u
#define LR2021_RX_FIFO_BASE_1K 0x00804400u
#define LR2021_RX_FIFO_SIZE_1K 0x000003E8u
#define LR2021_TX_FIFO_BASE_1K 0x00804000u
#define LR2021_TX_FIFO_SIZE_1K 0x000003FCu

#ifdef __cplusplus
}
#endif

#endif
