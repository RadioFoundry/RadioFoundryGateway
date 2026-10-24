// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The MMDVM modem serial protocol: framing, config bytes, status and version
 * replies, the transmit-side burst regeneration, and the radio's detection
 * and RSSI. No I/O: the firmware owns the port and the waiting.
 */
#ifndef RDOF_MMDVM_H
#define RDOF_MMDVM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/fec.h"
#include "rdof/features.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_MMDVM_FRAME_START 0xE0
#define RDOF_MMDVM_GET_VERSION 0x00
#define RDOF_MMDVM_GET_STATUS 0x01
#define RDOF_MMDVM_SET_CONFIG 0x02
#define RDOF_MMDVM_SET_MODE 0x03
#define RDOF_MMDVM_SET_FREQ 0x04
#define RDOF_MMDVM_SEND_CWID 0x0A
#define RDOF_MMDVM_DMR_DATA1 0x18
#define RDOF_MMDVM_DMR_LOST1 0x19
#define RDOF_MMDVM_DMR_DATA2 0x1A
#define RDOF_MMDVM_DMR_LOST2 0x1B
#define RDOF_MMDVM_DMR_SHORTLC 0x1C
#define RDOF_MMDVM_DMR_START 0x1D
#define RDOF_MMDVM_DMR_ABORT 0x1E
#define RDOF_MMDVM_ACK 0x70
#define RDOF_MMDVM_NAK 0x7F

#define RDOF_MODE_IDLE 0
#define RDOF_MODE_DMR 2
#define RDOF_DMR_ENABLED 0x02
#define RDOF_MMDVM_SIMPLEX 0x80
/* SET_CONFIG flags, with RDOF_MMDVM_SIMPLEX: dual capacity direct mode (both slots on one frequency, ETSI TDMA
 * direct mode syncs). The gateway's extension, sent only to a modem with DCDM in its description (rdof_dcdm_capable). */
#define RDOF_MMDVM_DCDM 0x40

/* DMR_DATA control byte */
#define RDOF_SYNC_AUDIO 0x20
#define RDOF_SYNC_DATA 0x40
#define RDOF_CONTROL_IDLE 0x80

#define RDOF_NAK_RF_BUSY 6
#define RDOF_NAK_SFR_OFF 2
#define RDOF_STATUS_INBOUND_BUSY 0x80

/* Longest frame: protocol 2's long form, 255 + 255 bytes. */
#define RDOF_MMDVM_MAX_FRAME 512

/* Commands sent without waiting for a reply: the firmware only NAKs a bad one. */
bool rdof_mmdvm_unanswered(uint8_t command);

/* E0 <len> <command> <payload>. Returns the frame length, or 0 if it doesn't fit (or payload > 252). */
size_t rdof_mmdvm_frame(uint8_t command, const uint8_t *payload, size_t len, uint8_t *out, size_t cap);

typedef struct {
    bool rx_invert, tx_invert, ptt_invert;
    int tx_delay_ms;
    double rx_level, tx_level, cwid_level;
    int rx_dc_offset, tx_dc_offset, dmr_delay;
} rdof_modem_config_t;

void rdof_modem_config_default(rdof_modem_config_t *c);
int rdof_mmdvm_pct(double level);
int rdof_modem_config_flags(const rdof_modem_config_t *c, bool duplex);
/* SET_CONFIG payloads: 23 bytes (protocol 1), 37 bytes (protocol 2). Return the length. */
size_t rdof_mmdvm_config_v1(const rdof_modem_config_t *c, int cc, int state, bool duplex, uint8_t out[23]);
size_t rdof_mmdvm_config_v2(const rdof_modem_config_t *c, int cc, int state, bool duplex, uint8_t out[37]);
/* SET_FREQ payload: flags, RX, TX (little endian), RF level, POCSAG = TX. 14 bytes. */
size_t rdof_mmdvm_set_freq_payload(uint32_t rx_hz, uint32_t tx_hz, int rf_level, uint8_t out[14]);

/* Bytes off the port -> frames; skips junk. */
typedef void (*rdof_mmdvm_frame_cb)(void *ctx, uint8_t command, const uint8_t *payload, size_t len);
typedef struct {
    uint8_t buf[2 * RDOF_MMDVM_MAX_FRAME];
    size_t len;
} rdof_mmdvm_reader_t;

void rdof_mmdvm_reader_init(rdof_mmdvm_reader_t *r);
void rdof_mmdvm_reader_feed(rdof_mmdvm_reader_t *r, const uint8_t *data, size_t n, rdof_mmdvm_frame_cb cb, void *ctx);

typedef struct {
    int transmitting, carrier, space, space1, inbound_busy;
} rdof_mmdvm_status_t;

/* GET_STATUS reply payload. false if short. */
bool rdof_mmdvm_parse_status(int protocol, const uint8_t *p, size_t len, rdof_mmdvm_status_t *out);
/* GET_VERSION reply payload: returns the protocol, writes the description (trimmed, NUL terminated). */
int rdof_mmdvm_parse_version(const uint8_t *p, size_t len, char *desc, size_t cap);

/* the radio */
/* RDOF_RADIO_SPLIT: two split duplex halves as one duplex modem (rdof/split.h; its description is RDOF_SPLIT_DESCRIPTION). */
typedef enum { RDOF_RADIO_UNKNOWN = 0, RDOF_RADIO_HOTSPOT, RDOF_RADIO_MMDVM_HS, RDOF_RADIO_MMDVM, RDOF_RADIO_SPLIT } rdof_radio_kind_t;
rdof_radio_kind_t rdof_radio_detect(const char *description);
/* "OpenDigitalRadio" or "OpenGD77" in the description (any case): a radio_hotspot. */
bool rdof_is_radio_hotspot(const char *description);
const char *rdof_radio_kind_name(rdof_radio_kind_t k);
/* A radio's settings type as its kind (the inverse of rdof_radio_kind_name; "opengd77" is a radio_hotspot): unknown for
   "auto" or anything else. */
rdof_radio_kind_t rdof_radio_kind_of(const char *type);
/* A radio in hotspot mode (RadioFoundry or OpenGD77) whose firmware can be a single frequency repeater ("SFR" in its description): its (inbound,
 * outbound) slots, from "TS1>TS2" when it says, else (1, 2). false if it can't. */
bool rdof_sfr_slots(const char *description, int *inbound, int *outbound);
/* Its outbound framing after the slots ("SFR TS1>TS2 TS"): TS, BS, MS..., or "" when it doesn't say. */
void rdof_sfr_framing(const char *description, char *out, size_t cap);
/* The word DCDM in the description: the modem takes RDOF_MMDVM_DCDM (dual capacity direct mode). */
bool rdof_dcdm_capable(const char *description);
/* AT1846S RSSI register -> dBm, as the OpenGD77 firmware's own meter converts it. */
int rdof_radio_hotspot_dbm(int raw, uint32_t freq_hz);

#if RDOF_FEATURE_TX
/* What a simplex (or, bs = true, duplex) modem must be handed. */
void rdof_regenerate(uint8_t control, const uint8_t burst[RDOF_BURST_BYTES], int cc, bool bs, uint8_t out[RDOF_BURST_BYTES]);
/* n preamble CSBKs ahead of a CSBK sent on its own; out holds n bursts. */
/* A silence burst for voice position n (0 = A, 1-5 = B-F), R5.10: BS or MS voice sync on A, else an EMB for cc and
 * the position and the 32-bit embedded field (NULL: zeros). Returns its control byte. */
uint8_t rdof_silence_burst(int n, int cc, bool bs, const uint8_t *embedded32, uint8_t out[RDOF_BURST_BYTES]);
void rdof_csbk_preambles(uint32_t src, uint32_t dst, bool group, int n, int cc, uint8_t (*out)[RDOF_BURST_BYTES]);
#endif /* RDOF_FEATURE_TX */


/* A DMR_DATA control byte -> (data type, voice burst index). */
void rdof_control_to_dt(uint8_t control, int *dt, int *n);

#ifdef __cplusplus
}
#endif

#endif
