// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * DMR layer 2 PDUs: full LC, CSBK, data headers and data blocks. The parts
 * the gateway uses.
 */
#ifndef RDOF_PDU_H
#define RDOF_PDU_H

#include <stdbool.h>
#include <stdint.h>

#include "rdof/fec.h"
#include "rdof/features.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RDOF_DT_PI_HEADER = 0,
    RDOF_DT_VOICE_LC_HEADER = 1,
    RDOF_DT_TERMINATOR_WITH_LC = 2,
    RDOF_DT_CSBK = 3,
    RDOF_DT_MBC_HEADER = 4,
    RDOF_DT_MBC_CONTINUATION = 5,
    RDOF_DT_DATA_HEADER = 6,
    RDOF_DT_RATE_12 = 7,
    RDOF_DT_RATE_34 = 8,
    RDOF_DT_IDLE = 9,
    RDOF_DT_RATE_1 = 10,
    RDOF_DT_USBD = 11,
    RDOF_DT_VOICE_SYNC = 0xF0,
    RDOF_DT_VOICE = 0xF1,
};

#define RDOF_FLCO_GROUP 0x00
#define RDOF_FLCO_PRIVATE 0x03
#define RDOF_FLCO_TA_HEADER 0x04
#define RDOF_FLCO_TA_BLOCK1 0x05
#define RDOF_FLCO_TA_BLOCK2 0x06
#define RDOF_FLCO_TA_BLOCK3 0x07
#define RDOF_FLCO_GPS_INFO 0x08
#define RDOF_SO_EMERGENCY 0x80

#define RDOF_DPF_UDT 0
#define RDOF_DPF_RESPONSE 1
#define RDOF_DPF_UNCONFIRMED 2
#define RDOF_DPF_CONFIRMED 3
#define RDOF_DPF_SHORT_DATA_DEFINED 13
#define RDOF_DPF_SHORT_DATA_RAW 14
#define RDOF_DPF_PROPRIETARY 15

#define RDOF_CSBK_PREAMBLE 0x3D

/* "Not present" for the optional ids and fields. */
#define RDOF_NONE (-1)

typedef struct {
    bool pf;
    int flco, fid;
    uint8_t raw[9];
    int32_t src_id, dst_id; /* RDOF_NONE unless group / private */
    bool group;
    int service_options;
    bool has_gps;
    double lat, lon;
    int position_error;
    int ta_format, ta_length; /* RDOF_NONE unless a talker alias header */
    uint8_t ta_bits[56];
    int ta_nbits;
} rdof_full_lc_t;

void rdof_parse_full_lc(const uint8_t lc9[9], rdof_full_lc_t *out);
/* Voice LC header / terminator: BPTC then RS(12,9). Returns false if the RS check fails. */
bool rdof_decode_voice_lc(const uint8_t burst[RDOF_BURST_BYTES], int data_type, rdof_full_lc_t *out);

typedef struct {
    int opcode, fid;
    bool last_block, pf;
    uint8_t raw[12];
    bool crc_ok;
    int32_t src_id, dst_id; /* RDOF_NONE unless addressed */
    bool group, data_follows;
    int blocks_to_follow;
} rdof_csbk_t;

void rdof_decode_csbk(const uint8_t burst[RDOF_BURST_BYTES], rdof_csbk_t *out);

typedef struct {
    int dpf, sap;
    bool group, response_requested;
    uint32_t dst_id, src_id;
    uint8_t raw[12];
    bool crc_ok;
    int blocks, pad_octets;
    bool full_message;
    int fsn, ns; /* RDOF_NONE when absent */
    int udt_format, pad_nibbles, udt_opcode;
    int dd_format, bit_padding;
    int response_class, response_type, response_status;
    int mfid;
} rdof_data_header_t;

void rdof_parse_data_header(const uint8_t data12[12], rdof_data_header_t *out);
void rdof_decode_data_header(const uint8_t burst[RDOF_BURST_BYTES], rdof_data_header_t *out);

typedef struct {
    int data_type;
    uint8_t payload[24];
    int len; /* 12 (rate 1/2), 18 (rate 3/4) or 24 (rate 1) */
    bool fec_ok;
    int errors;
} rdof_data_block_t;

#if RDOF_FEATURE_DATA_BLOCKS
/* false for a data type that isn't a data block. */
bool rdof_decode_data_block(const uint8_t burst[RDOF_BURST_BYTES], int data_type, rdof_data_block_t *out);
/* A confirmed block: DBSN, the user octets (len - 2, written to body) and whether the CRC-9 checks. */
bool rdof_split_confirmed(const rdof_data_block_t *block, int *dbsn, uint8_t *body, int *body_len);
#endif /* RDOF_FEATURE_DATA_BLOCKS */


#ifdef __cplusplus
}
#endif

#endif
