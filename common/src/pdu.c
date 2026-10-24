// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* DMR layer 2 PDUs: LC, CSBK, data header and blocks; no talker alias text. */
#include "rdof/pdu.h"

#include <string.h>

#include "rdof/bits.h"
#include "rdof/features.h"

static int64_t signed_field(uint64_t v, int width)
{
    return (v & (1ULL << (width - 1))) ? (int64_t)v - (int64_t)(1ULL << width) : (int64_t)v;
}

void rdof_parse_full_lc(const uint8_t lc9[9], rdof_full_lc_t *o)
{
    memset(o, 0, sizeof *o);
    memcpy(o->raw, lc9, 9);
    o->pf = rdof_get_field(lc9, 0, 1);
    o->flco = (int)rdof_get_field(lc9, 2, 6);
    o->fid = (int)rdof_get_field(lc9, 8, 8);
    o->src_id = o->dst_id = RDOF_NONE;
    o->ta_format = o->ta_length = RDOF_NONE;
    o->position_error = RDOF_NONE;
    if (o->flco == RDOF_FLCO_GROUP || o->flco == RDOF_FLCO_PRIVATE) {
        o->service_options = lc9[2];
        o->dst_id = (int32_t)rdof_get_field(lc9, 24, 24);
        o->src_id = (int32_t)rdof_get_field(lc9, 48, 24);
        o->group = o->flco == RDOF_FLCO_GROUP;
    } else if (o->fid == 0 && o->flco == RDOF_FLCO_GPS_INFO) {
        o->has_gps = true;
        o->position_error = (int)rdof_get_field(lc9, 20, 3);
        o->lon = (double)signed_field(rdof_get_field(lc9, 23, 25), 25) * 360.0 / (double)(1 << 25);
        o->lat = (double)signed_field(rdof_get_field(lc9, 48, 24), 24) * 180.0 / (double)(1 << 24);
    } else if (o->fid == 0 && o->flco == RDOF_FLCO_TA_HEADER) {
        o->ta_format = (int)rdof_get_field(lc9, 16, 2);
        o->ta_length = (int)rdof_get_field(lc9, 18, 5);
        o->ta_nbits = 49;
        for (int i = 0; i < 49; i++)
            o->ta_bits[i] = (uint8_t)rdof_get_field(lc9, 23 + i, 1);
    } else if (o->fid == 0 && o->flco >= RDOF_FLCO_TA_BLOCK1 && o->flco <= RDOF_FLCO_TA_BLOCK3) {
        o->ta_nbits = 56;
        for (int i = 0; i < 56; i++)
            o->ta_bits[i] = (uint8_t)rdof_get_field(lc9, 16 + i, 1);
    }
}

bool rdof_decode_voice_lc(const uint8_t burst[RDOF_BURST_BYTES], int data_type, rdof_full_lc_t *out)
{
    uint8_t lc12[12];
    rdof_bptc196_decode(burst, lc12);
    uint32_t mask = data_type == RDOF_DT_VOICE_LC_HEADER ? RDOF_MASK_VOICE_LC_HEADER : RDOF_MASK_TERMINATOR_LC;
    if (!rdof_check_rs129(lc12, mask))
        return false;
    rdof_parse_full_lc(lc12, out);
    return true;
}

/* Opcodes with target in octets 4-6 and source in 7-9 (ETSI -2 7.1.2 / -4). */
static bool addressed_csbk(int op)
{
    return op == 0x04 || op == 0x05 || op == 0x1F || op == 0x20 || op == 0x21 || op == 0x24 || op == 0x38 ||
           op == RDOF_CSBK_PREAMBLE;
}

void rdof_decode_csbk(const uint8_t burst[RDOF_BURST_BYTES], rdof_csbk_t *c)
{
    uint8_t d[12];
    rdof_bptc196_decode(burst, d);
    memset(c, 0, sizeof *c);
    memcpy(c->raw, d, 12);
    c->crc_ok = rdof_check_crc16(d, RDOF_MASK_CSBK);
    c->opcode = d[0] & 0x3F;
    c->fid = d[1];
    c->last_block = d[0] & 0x80;
    c->pf = d[0] & 0x40;
    c->src_id = c->dst_id = RDOF_NONE;
    if (c->fid == 0 && addressed_csbk(c->opcode)) {
        c->dst_id = (int32_t)rdof_be24(d + 4);
        c->src_id = (int32_t)rdof_be24(d + 7);
    }
    if (c->fid == 0 && c->opcode == RDOF_CSBK_PREAMBLE) {
        c->data_follows = d[2] & 0x80;
        c->group = d[2] & 0x40;
        c->blocks_to_follow = d[3];
    }
}

void rdof_parse_data_header(const uint8_t d[12], rdof_data_header_t *h)
{
    memset(h, 0, sizeof *h);
    memcpy(h->raw, d, 12);
    h->dpf = (int)rdof_get_field(d, 4, 4);
    h->sap = (int)rdof_get_field(d, 8, 4);
    h->group = rdof_get_field(d, 0, 1);
    h->response_requested = rdof_get_field(d, 1, 1);
    h->dst_id = (uint32_t)rdof_get_field(d, 16, 24);
    h->src_id = (uint32_t)rdof_get_field(d, 40, 24);
    h->crc_ok = rdof_check_crc16(d, RDOF_MASK_DATA_HEADER);
    h->full_message = true;
    h->fsn = h->ns = h->udt_format = h->udt_opcode = h->dd_format = RDOF_NONE;
    h->response_class = h->response_type = h->response_status = h->mfid = RDOF_NONE;
    if (h->dpf == RDOF_DPF_UNCONFIRMED || h->dpf == RDOF_DPF_CONFIRMED) {
        h->pad_octets = (int)((rdof_get_field(d, 3, 1) << 4) | rdof_get_field(d, 12, 4));
        h->full_message = rdof_get_field(d, 64, 1);
        h->blocks = (int)rdof_get_field(d, 65, 7);
        h->fsn = (int)rdof_get_field(d, 76, 4);
        if (h->dpf == RDOF_DPF_CONFIRMED)
            h->ns = (int)rdof_get_field(d, 73, 3);
    } else if (h->dpf == RDOF_DPF_RESPONSE) {
        h->blocks = (int)rdof_get_field(d, 65, 7);
        h->response_class = (int)rdof_get_field(d, 72, 2);
        h->response_type = (int)rdof_get_field(d, 74, 3);
        h->response_status = (int)rdof_get_field(d, 77, 3);
    } else if (h->dpf == RDOF_DPF_SHORT_DATA_DEFINED || h->dpf == RDOF_DPF_SHORT_DATA_RAW) {
        h->blocks = (int)((rdof_get_field(d, 2, 2) << 4) | rdof_get_field(d, 12, 4));
        if (h->dpf == RDOF_DPF_SHORT_DATA_DEFINED) {
            h->dd_format = (int)rdof_get_field(d, 64, 6);
            h->full_message = rdof_get_field(d, 71, 1);
            h->bit_padding = (int)rdof_get_field(d, 72, 8);
        }
    } else if (h->dpf == RDOF_DPF_UDT) {
        h->udt_format = (int)rdof_get_field(d, 12, 4);
        h->pad_nibbles = (int)rdof_get_field(d, 64, 5);
        h->blocks = (int)rdof_get_field(d, 70, 2) + 1; /* "appended blocks" is stored minus one */
        h->udt_opcode = (int)rdof_get_field(d, 74, 6);
    } else if (h->dpf == RDOF_DPF_PROPRIETARY) {
        h->mfid = d[1];
        h->src_id = h->dst_id = 0;
    }
}

void rdof_decode_data_header(const uint8_t burst[RDOF_BURST_BYTES], rdof_data_header_t *out)
{
    uint8_t d[12];
    rdof_bptc196_decode(burst, d);
    rdof_parse_data_header(d, out);
}

#if RDOF_FEATURE_DATA_BLOCKS
bool rdof_decode_data_block(const uint8_t burst[RDOF_BURST_BYTES], int data_type, rdof_data_block_t *b)
{
    memset(b, 0, sizeof *b);
    b->data_type = data_type;
    if (data_type == RDOF_DT_RATE_12) {
        b->fec_ok = rdof_bptc196_decode(burst, b->payload);
        b->len = 12;
    } else if (data_type == RDOF_DT_RATE_34) {
        b->errors = rdof_trellis34_decode(burst, b->payload);
        b->fec_ok = b->errors == 0;
        b->len = 18;
    } else if (data_type == RDOF_DT_RATE_1) {
        rdof_rate1_payload(burst, b->payload);
        b->fec_ok = true;
        b->len = 24;
    } else {
        return false;
    }
    return true;
}

bool rdof_split_confirmed(const rdof_data_block_t *block, int *dbsn, uint8_t *body, int *body_len)
{
    int mask = block->data_type == RDOF_DT_RATE_12   ? RDOF_MASK_CRC9_RATE12
               : block->data_type == RDOF_DT_RATE_34 ? RDOF_MASK_CRC9_RATE34
                                                   : RDOF_MASK_CRC9_RATE1;
    int n = (int)rdof_get_field(block->payload, 0, 7);
    int crc9 = (int)rdof_get_field(block->payload, 7, 9);
    int len = block->len - 2;
    if (len < 0 || len > (int)sizeof block->payload - 2) {
        /* not a data block (rdof_decode_data_block's false leaves len 0) */
        *dbsn = *body_len = 0;
        return false;
    }
    memcpy(body, block->payload + 2, (size_t)len);
    *dbsn = n;
    *body_len = len;
    return rdof_check_crc9(body, (size_t)len, n, crc9, mask);
}
#endif /* RDOF_FEATURE_DATA_BLOCKS */
