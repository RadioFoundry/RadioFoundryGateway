// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * DMR link control for voice calls (ETSI TS 102 361-1 annex B, TS 102 361-2 clause 7):
 *
 *  - the full LC (9 octets: PF + FLCO, FID, service options, destination, source)
 *  - the voice LC header / terminator form: LC + RS(12,9) parity masked 0x969696 / 0x999999 (B.3.6, B.3.12)
 *  - the embedded LC: LC + 5-bit checksum (B.3.11), in the 128-bit embedded signaling matrix (Hamming(16,11,4)
 *    rows, column parity, B.2.1), interleaved and spread over voice bursts B-E as 4 x 32 bits with LCSS
 *    first / continuation / continuation / last
 *  - CRC-CCITT (B.3.7) for the BS activation CSBK
 *  - talker alias (FLCO 4 header, FLCO 5-7 blocks) decoding (all four formats) and encoding (7-bit, ISO 8-bit)
 *  - GPS Info (FLCO 8): position error, longitude, latitude, encoding and decoding, and the range / bearing between
 *    two positions for the screens
 *
 * No board or modem knowledge: modems that build the embedded signaling themselves only need the 9 LC octets;
 * a software modem or a host test uses the fragment functions.
 */
#ifndef ODR_DMR_LC_H
#define ODR_DMR_LC_H

#include <stdint.h>
#include <stdbool.h>

#define DMR_LC_BYTES			9
#define DMR_LC_FULL_BYTES		12	// LC + RS(12,9) parity

// FLCO (TS 102 361-2 7.1.1)
#define DMR_FLCO_GROUP_VOICE		0x00	// Grp_V_Ch_Usr
#define DMR_FLCO_UNIT_VOICE		0x03	// UU_V_Ch_Usr (private call)
#define DMR_FLCO_TA_HEADER		0x04	// talker alias header
#define DMR_FLCO_TA_BLOCK1		0x05
#define DMR_FLCO_TA_BLOCK2		0x06
#define DMR_FLCO_TA_BLOCK3		0x07
#define DMR_FLCO_GPS			0x08	// GPS info

// Service options (TS 102 361-2 7.2.1)
#define DMR_SVC_EMERGENCY		0x80
#define DMR_SVC_PRIVACY			0x40
#define DMR_SVC_BROADCAST		0x08
#define DMR_SVC_OVCM			0x04
#define DMR_SVC_PRIORITY_MASK		0x03

#define DMR_ID_ALL_CALL			0xFFFFFFu
#define DMR_ID_MAX			0xFFFFFFu

// RS(12,9) parity masks (B.3.12)
#define DMR_RS_MASK_VOICE_HEADER	0x96
#define DMR_RS_MASK_TERMINATOR		0x99

// LCSS (link control start / stop) of the EMB
#define DMR_LCSS_SINGLE			0
#define DMR_LCSS_FIRST			1
#define DMR_LCSS_LAST			2
#define DMR_LCSS_CONTINUATION		3

typedef struct {
	bool pf;		// protect flag
	uint8_t flco;		// 6 bits
	uint8_t fid;
	uint8_t svc;		// service options
	uint32_t dst;		// 24 bits
	uint32_t src;		// 24 bits
} dmr_lc_t;

void dmr_lc_pack(const dmr_lc_t *lc, uint8_t out[DMR_LC_BYTES]);
void dmr_lc_unpack(const uint8_t in[DMR_LC_BYTES], dmr_lc_t *lc);
// FLCO 0 (group) or 3 (private): an LC that names a voice call's addresses
bool dmr_lc_is_voice(const uint8_t lc[DMR_LC_BYTES]);

// --- RS(12,9) over GF(256) (primitive polynomial 0x11D, generator roots alpha^1..alpha^3) ---

// Parity of 9 octets, highest power first, unmasked
void dmr_rs129_parity(const uint8_t data[DMR_LC_BYTES], uint8_t parity[3]);
// LC + masked parity: the 12 octets of a voice LC header (mask 0x96) or terminator (0x99)
void dmr_lc_encode_full(const uint8_t lc[DMR_LC_BYTES], uint8_t mask, uint8_t out[DMR_LC_FULL_BYTES]);
// Check (and correct up to one octet of) a received header / terminator. Returns 0 clean, 1 one octet corrected,
// -1 uncorrectable. lc gets the (corrected) LC either way
int dmr_lc_decode_full(const uint8_t in[DMR_LC_FULL_BYTES], uint8_t mask, uint8_t lc[DMR_LC_BYTES]);

// --- CRC-CCITT (B.3.7): polynomial 0x1021, initial 0, inverted, then XOR the data type mask ---

uint16_t dmr_crc_ccitt(const uint8_t *buf, uint8_t len);
// buf[0..9] -> buf[10..11] = CRC ^ mask, MSB first
void dmr_crc16_append(uint8_t buf[12], uint16_t mask);
#define DMR_CRC_MASK_CSBK		0xA5A5
#define DMR_CRC_MASK_PI			0x6969	// PI header (data type 0)

// BS_Dwn_Act CSBK (CSBKO 0x38, LB = 1, FID 0, any BS) from own_id, with its masked CRC: 12 octets
void dmr_csbk_bs_activation(uint32_t own_id, uint8_t out[12]);

// --- Embedded LC ---

// 5-bit checksum: the sum of the 9 LC octets modulo 31
uint8_t dmr_emb_lc_checksum(const uint8_t lc[DMR_LC_BYTES]);
// The 128 interleaved bits (16 octets, MSB first) of the embedded signaling for an LC
void dmr_emb_lc_encode_bits(const uint8_t lc[DMR_LC_BYTES], uint8_t bits[16]);
// The same as the four 32-bit fragments of bursts B, C, D, E
void dmr_emb_lc_encode(const uint8_t lc[DMR_LC_BYTES], uint8_t frag[4][4]);
// The 128 interleaved bits back to the LC: Hamming(16,11,4) single error per row corrected, column parity and checksum
// checked. Returns false if the LC can't be trusted
bool dmr_emb_lc_decode_bits(const uint8_t bits[16], uint8_t lc[DMR_LC_BYTES]);
// The LCSS a voice burst carries when its superframe holds an embedded LC: A 0 (no EMB), B 1, C 3, D 3, E 2, F 0
uint8_t dmr_emb_lcss(uint8_t voice_index);

// Reassembly of received fragments in LCSS order (first, continuation x 2, last)
typedef struct {
	uint8_t bits[16];
	uint8_t next;		// fragments held, 0 = waiting for a first
} dmr_emb_rx_t;

void dmr_emb_rx_reset(dmr_emb_rx_t *rx);
// One burst's 32 embedded bits with its EMB LCSS. True when an LC was completed and checked (written to lc)
bool dmr_emb_rx_push(dmr_emb_rx_t *rx, const uint8_t frag[4], uint8_t lcss, uint8_t lc[DMR_LC_BYTES]);

// --- Talker alias (TS 102 361-2 7.1.1.3, 7.1.1.4, 7.2.18) ---

#define DMR_TA_TEXT_MAX			31	// 7-bit format: 7 + 3 x 8 characters
#define DMR_TA_ISO8_MAX			27	// 8-bit format: 6 + 3 x 7 characters
#define DMR_TA_LC_MAX			4	// header and three blocks

typedef enum { DMR_TA_7BIT = 0, DMR_TA_ISO8 = 1, DMR_TA_UTF8 = 2, DMR_TA_UTF16 = 3 } dmr_ta_format_t;

typedef struct {
	uint8_t stream[28];	// 217 alias bits: header 49, blocks 56 each
	uint8_t have;		// bit 0 header, bits 1-3 blocks 1-3
	uint8_t format;		// dmr_ta_format_t
	uint8_t length;		// characters announced
	char text[DMR_TA_TEXT_MAX + 1];	// printable ASCII so far ('?' for others), NUL terminated
	bool complete;
} dmr_ta_t;

void dmr_ta_reset(dmr_ta_t *ta);
// One embedded LC with FLCO 4-7. True when text or completeness changed. Characters outside printable ASCII (ISO 8-bit
// above 0x7E, UTF-8 multi-byte sequences, UTF-16 above 0x7E and surrogate pairs) show as one '?' each
bool dmr_ta_push(dmr_ta_t *ta, const uint8_t lc[DMR_LC_BYTES]);
// The talker alias LCs for text: header (FLCO 4) and as many blocks (FLCO 5-7) as the text needs, FID 0.
// format DMR_TA_7BIT (up to 31 characters) or DMR_TA_ISO8 (anything else; up to 27). Returns the LC count, 0 for ""
int dmr_ta_encode(const char *text, uint8_t format, uint8_t lcs[DMR_TA_LC_MAX][DMR_LC_BYTES]);

// --- GPS Info LC (TS 102 361-2 7.1.1: FLCO 8; the Position Error, Longitude and Latitude elements of 7.2) ---
//
//   octet 0  PF, R, FLCO = 001000      octet 1  FID (0: the ETSI form)
//   octet 2  reserved (4 bits), position error (3 bits), longitude bit 24
//   octets 3-5  longitude bits 23-0: 25-bit two's complement, 360 / 2^25 degree per step (east positive)
//   octets 6-8  latitude: 24-bit two's complement, 180 / 2^24 degree per step (north positive)

// Position error (3 bits): the horizontal error is below 2 m, 20 m, 200 m, 2 km, 20 km, 200 km or more, 7 unknown
#define DMR_GPS_ERR_2M			0
#define DMR_GPS_ERR_MORE		6	// over 200 km
#define DMR_GPS_ERR_UNKNOWN		7

typedef struct {
	uint8_t err;		// position error code 0-7
	int32_t lat, lon;	// 1e-7 degree
} dmr_gps_t;

// The LC (FLCO 8, FID 0). Out of range latitude / longitude are clamped to the field's range
void dmr_gps_encode(const dmr_gps_t *g, uint8_t lc[DMR_LC_BYTES]);
// A GPS Info LC to a position: false when it isn't one (FLCO, FID) or carries 0, 0 (no fix)
bool dmr_gps_decode(const uint8_t lc[DMR_LC_BYTES], dmr_gps_t *g);
// The error code for an error in meters (0 = unknown)
uint8_t dmr_gps_err_code(uint32_t meters);
// The upper bound in meters of an error code (0: unknown or over 200 km)
uint32_t dmr_gps_err_m(uint8_t code);
// From position 1 to position 2: distance in meters and the initial bearing in degrees (0 north, 90 east). A flat
// earth approximation around the mid latitude: within 0.5 % up to a few hundred km (DMR ranges), no libm needed
void dmr_gps_range(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2, uint32_t *meters, uint16_t *bearing);

#endif
