// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Host tests for common: every function checked against golden vectors
 * from the unpublished reference (vectors.h; PROVENANCE.md, "Origin").
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/ambe.h"
#include "rdof/bits.h"
#include "rdof/config.h"
#include "rdof/status.h"
#include "rdof/oled.h"
#include "rdof/fec.h"
#include "rdof/hblink.h"
#include "rdof/json.h"
#include "rdof/layer2.h"
#include "rdof/mmdvm.h"
#include "rdof/jsonp.h"
#include "rdof/pdu.h"
#include "rdof/rawburst.h"
#include "rdof/slotgrid.h"
#include "rdof/split.h"
#include "rdof/tracker.h"
#include "rdof/rust.h"
#include "rdof/usbmatch.h"
#include "../common/src/radiofoundry/dmrFec.h"
#include "vectors.h"

int checks, failures;

#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                     \
        checks++;                                                                                            \
        if (!(cond)) {                                                                                       \
            failures++;                                                                                      \
            if (failures <= 40) {                                                                            \
                printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                      \
                printf(__VA_ARGS__);                                                                         \
                printf("\n");                                                                                \
            }                                                                                                \
        }                                                                                                    \
    } while (0)

static size_t unhex(const char *s, uint8_t *out)
{
    size_t n = strlen(s) / 2;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
    return n;
}

static void tohex(const uint8_t *b, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++)
        snprintf(out + 2 * i, 3, "%02x", b[i]);
    out[2 * n] = 0;
}

static size_t unbits(const char *s, uint8_t *out)
{
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++)
        out[i] = s[i] == '1';
    return n;
}

static bool hex_eq(const uint8_t *b, size_t n, const char *want)
{
    char got[4096];
    tohex(b, n, got);
    return strcmp(got, want) == 0;
}

static bool bits_eq(const uint8_t *b, size_t n, const char *want)
{
    if (strlen(want) != n)
        return false;
    for (size_t i = 0; i < n; i++)
        if (b[i] != (want[i] == '1'))
            return false;
    return true;
}

/* ── FEC ───────────────────────────────────────────────────────────────────── */

static void test_fec(void)
{
    uint8_t in[64], out[64], bits[256];
#if RDOF_FEATURE_DATA_BLOCKS
    uint8_t info[196];
#endif
    for (size_t i = 0; i < V_BPTC_N; i++) {
        unhex(v_bptc[i].payload, in);
        rdof_data_burst(in, v_bptc[i].dt, v_bptc[i].cc, v_bptc[i].sync, out);
        CHECK(hex_eq(out, 33, v_bptc[i].burst), "bptc encode %zu", i);
    }
    for (size_t i = 0; i < V_BPTC_DEC_N; i++) {
        unhex(v_bptc_dec[i].burst, in);
        bool ok = rdof_bptc196_decode(in, out);
        CHECK(hex_eq(out, 12, v_bptc_dec[i].out) && ok == v_bptc_dec[i].ok, "bptc decode %zu", i);
    }
    for (size_t i = 0; i < V_EMBLC_N; i++) {
        unhex(v_emblc[i].lc, in);
        rdof_embedded_lc_encode(in, bits);
        CHECK(bits_eq(bits, 128, v_emblc[i].raw), "emb lc encode %zu", i);
    }
    for (size_t i = 0; i < V_EMBLC_DEC_N; i++) {
        unbits(v_emblc_dec[i].raw, bits);
        bool ok = rdof_embedded_lc_decode(bits, out);
        CHECK(ok == v_emblc_dec[i].ok && (!ok || hex_eq(out, 9, v_emblc_dec[i].lc)), "emb lc decode %zu", i);
    }
#if RDOF_FEATURE_DATA_BLOCKS
    for (size_t i = 0; i < V_TRELLIS_N; i++) {
        unhex(v_trellis[i].payload, in);
        rdof_trellis34_encode(in, info);
        CHECK(bits_eq(info, 196, v_trellis[i].info), "trellis encode %zu", i);
    }
    for (size_t i = 0; i < V_TRELLIS_DEC_N; i++) {
        unhex(v_trellis_dec[i].burst, in);
        int metric = rdof_trellis34_decode(in, out);
        CHECK(hex_eq(out, 18, v_trellis_dec[i].out) && metric == v_trellis_dec[i].metric, "trellis decode %zu (%d)",
              i, metric);
    }
    for (size_t i = 0; i < V_RATE1_N; i++) {
        unhex(v_rate1[i].burst, in);
        rdof_rate1_payload(in, out);
        CHECK(hex_eq(out, 24, v_rate1[i].out), "rate 1 %zu", i);
    }
#endif
    for (size_t i = 0; i < V_CRC_N; i++) {
        size_t n = unhex(v_crc[i].data, in);
        CHECK(rdof_crc16_ccitt(in, n) == v_crc[i].crc16, "crc16 %zu", i);
#if RDOF_FEATURE_DATA_BLOCKS
        CHECK(rdof_crc32_dmr(in, n) == v_crc[i].crc32, "crc32 %zu", i);
#endif
    }
#if RDOF_FEATURE_DATA_BLOCKS
    for (size_t i = 0; i < V_CRC9_N; i++) {
        size_t n = unhex(v_crc9[i].block, in);
        rdof_bytes_to_bits(in, n, bits);
        rdof_u64_to_bits((uint64_t)v_crc9[i].dbsn, 7, bits + 8 * n);
        CHECK(rdof_crc9_dmr(bits, 8 * n + 7) == v_crc9[i].crc9, "crc9 %zu", i);
        CHECK(rdof_check_crc9(in, n, v_crc9[i].dbsn, v_crc9[i].stored, v_crc9[i].mask) == v_crc9[i].ok, "crc9 ok %zu", i);
    }
#endif
    for (size_t i = 0; i < V_RS_N; i++) {
        unhex(v_rs[i].msg, in);
        rdof_rs129_parity(in, out);
        CHECK(hex_eq(out, 3, v_rs[i].parity), "rs %zu", i);
        uint8_t lc12[12];
        rdof_voice_lc12(in, RDOF_DT_TERMINATOR_WITH_LC, lc12);
        CHECK(rdof_check_rs129(lc12, RDOF_MASK_TERMINATOR_LC) && !rdof_check_rs129(lc12, RDOF_MASK_VOICE_LC_HEADER), "rs check %zu", i);
    }
    /* Hamming: every single-bit error corrected */
    const rdof_hamming_t *codes[3] = {RDOF_H_15_11, RDOF_H_13_9, RDOF_H_16_11};
    const int ns[3] = {15, 13, 16}, ks[3] = {11, 9, 11};
    for (int c = 0; c < 3; c++)
        for (int pos = 0; pos < ns[c]; pos++) {
            uint8_t w[16] = {0}, x[16];
            for (int k = 0; k < ks[c]; k++)
                w[k] = (uint8_t)((pos * 7 + k * 3) % 2);
            rdof_hamming_encode(codes[c], w);
            memcpy(x, w, sizeof x);
            x[pos] ^= 1;
            bool fixed;
            CHECK(rdof_hamming_decode(codes[c], x, &fixed) && fixed && !memcmp(x, w, (size_t)ns[c]), "hamming %d %d", c, pos);
        }
}

/* ── layer 2 ───────────────────────────────────────────────────────────────── */

static void test_layer2(void)
{
    uint8_t in[64], bits[64];
    for (int v = 0; v < 256; v++)
        CHECK(rdof_golay2087_encode(v) == v_golay2087_enc[v], "golay2087 enc %d", v);
    for (int v = 0; v < 128; v++)
        CHECK(rdof_qr1676_encode(v) == v_qr1676_enc[v], "qr1676 enc %d", v);
    for (int v = 0; v < 16; v++)
        CHECK(rdof_hamming74_encode(v) == v_h74_enc[v], "h74 enc %d", v);
    int e, val;
    for (size_t i = 0; i < V_GOLAY2087_DEC_N; i++) {
        val = rdof_golay2087_decode((uint32_t)v_golay2087_dec[i].word, &e);
        CHECK(val == v_golay2087_dec[i].value && e == v_golay2087_dec[i].errors, "golay2087 dec %zu", i);
    }
    for (size_t i = 0; i < V_QR1676_DEC_N; i++) {
        val = rdof_qr1676_decode((uint32_t)v_qr1676_dec[i].word, &e);
        CHECK(val == v_qr1676_dec[i].value && e == v_qr1676_dec[i].errors, "qr1676 dec %zu", i);
    }
    /* the table decoders (layer2.c) against RadioFoundry's, every word: the same value wherever it decodes */
    int golay_bad = 0, qr_bad = 0;
    for (uint32_t w = 0; w < (1u << 20); w++) {
        uint8_t d;
        const int r = dmrFecGolay2087Decode(w, &d);
        val = rdof_golay2087_decode(w, &e);
        golay_bad += r < 0 ? (val != -1 || e != 4) : (val != d || e != r);
    }
    for (uint32_t w = 0; w < (1u << 16); w++) {
        uint8_t d;
        const int r = dmrFecQr1676Decode((uint16_t)w, &d);
        val = rdof_qr1676_decode(w, &e);
        qr_bad += r < 0 ? (val != -1 || e != 3) : (val != d || e != r);
    }
    CHECK(golay_bad == 0 && qr_bad == 0, "table decoders differ from RadioFoundry's: golay %d, qr %d", golay_bad, qr_bad);
    for (size_t i = 0; i < V_H74_DEC_N; i++) {
        val = rdof_hamming74_decode((uint32_t)v_h74_dec[i].word, &e);
        CHECK(val == v_h74_dec[i].value && e == v_h74_dec[i].errors, "h74 dec %zu", i);
    }
    for (size_t i = 0; i < V_SLOTTYPE_N; i++) {
        int cc = -1, dt = -1;
        unhex(v_slottype[i].burst, in);
        bool ok = rdof_decode_slot_type(in, &cc, &dt, &e);
        CHECK(ok == v_slottype[i].ok && e == v_slottype[i].errors && (!ok || (cc == v_slottype[i].cc && dt == v_slottype[i].dt)),
              "slot type %zu", i);
    }
    for (size_t i = 0; i < V_EMB_N; i++) {
        int cc = -1, pi, lcss;
        unhex(v_emb[i].burst, in);
        bool ok = rdof_decode_emb(in, &cc, &pi, &lcss, &e);
        CHECK(ok == v_emb[i].ok && e == v_emb[i].errors && pi == v_emb[i].pi && lcss == v_emb[i].lcss &&
                  (!ok || cc == v_emb[i].cc),
              "emb %zu", i);
    }
    for (size_t i = 0; i < V_CACH_N; i++) {
        unbits(v_cach[i].bits, bits);
        CHECK(rdof_decode_cach_slot(bits) == v_cach[i].slot, "cach %zu", i);
    }
    for (size_t i = 0; i < V_CACH_ENC_N; i++) {
        rdof_encode_cach(v_cach_enc[i].slot, v_cach_enc[i].at, v_cach_enc[i].lcss, bits);
        CHECK(bits_eq(bits, 24, v_cach_enc[i].bits), "cach enc %zu", i);
    }
    for (size_t i = 0; i < V_SYNC_N; i++) {
        unhex(v_sync[i].burst, in);
        for (int k = 0; k < 8; k++) {
            uint64_t p = rdof_family_voice_sync((rdof_family_t)(k / 2));
            CHECK(rdof_sync_errors(in, k % 2 ? rdof_data_sync(p) : p) == v_sync[i].errors[k], "sync errors %zu %d", i, k);
        }
        rdof_family_t fam = RDOF_FAMILY_COUNT;
        bool data = false;
        bool found = rdof_detect_sync(in, 5, &fam, &data, &e);
        CHECK(found == v_sync[i].found && e == v_sync[i].best &&
                  (!found || ((int)fam == v_sync[i].family && data == v_sync[i].data)),
              "detect sync %zu", i);
    }
    for (size_t i = 0; i < V_AMBE_N; i++) {
        uint8_t params[49];
        unhex(v_ambe[i].frame, in);
        int errs = rdof_ambe_decode_frame(in, params);
        CHECK(errs == v_ambe[i].errors && bits_eq(params, 49, v_ambe[i].params), "ambe %zu", i);
    }
}

/* ── PDUs ──────────────────────────────────────────────────────────────────── */

static void test_pdu(void)
{
    uint8_t in[64];
    for (size_t i = 0; i < V_LC_N; i++) {
        const struct v_lc *v = &v_lc[i];
        rdof_full_lc_t lc;
        unhex(v->lc, in);
        rdof_parse_full_lc(in, &lc);
        CHECK(lc.pf == v->pf && lc.flco == v->flco && lc.fid == v->fid && lc.src_id == v->src && lc.dst_id == v->dst &&
                  lc.group == v->group && lc.service_options == v->so && lc.has_gps == v->gps &&
                  lc.position_error == v->pos_err && lc.ta_format == v->ta_format && lc.ta_length == v->ta_length &&
                  bits_eq(lc.ta_bits, (size_t)lc.ta_nbits, v->ta_bits),
              "full lc %zu", i);
        if (v->gps)
            CHECK(lc.lat == v->lat && lc.lon == v->lon, "gps %zu %.9f %.9f", i, lc.lat, lc.lon);
    }
    for (size_t i = 0; i < V_VOICE_LC_N; i++) {
        rdof_full_lc_t lc;
        unhex(v_voice_lc[i].burst, in);
        bool ok = rdof_decode_voice_lc(in, v_voice_lc[i].dt, &lc);
        CHECK(ok == v_voice_lc[i].ok && (!ok || (lc.src_id == v_voice_lc[i].src && lc.dst_id == v_voice_lc[i].dst)),
              "voice lc %zu", i);
    }
    for (size_t i = 0; i < V_CSBK_N; i++) {
        const struct v_csbk *v = &v_csbk[i];
        rdof_csbk_t c;
        unhex(v->burst, in);
        rdof_decode_csbk(in, &c);
        CHECK(c.opcode == v->opcode && c.fid == v->fid && c.last_block == v->last && c.pf == v->pf &&
                  c.crc_ok == v->crc_ok && c.src_id == v->src && c.dst_id == v->dst && c.group == v->group &&
                  c.data_follows == v->data_follows && c.blocks_to_follow == v->btf,
              "csbk %zu", i);
    }
    for (size_t i = 0; i < V_DHDR_N; i++) {
        const struct v_dhdr *v = &v_dhdr[i];
        rdof_data_header_t x;
        unhex(v->data, in);
        rdof_parse_data_header(in, &x);
        CHECK(x.dpf == v->dpf && x.sap == v->sap && x.group == v->group && x.response_requested == v->rr &&
                  x.dst_id == (uint32_t)v->dst && x.src_id == (uint32_t)v->src && x.crc_ok == v->crc_ok &&
                  x.blocks == v->blocks && x.pad_octets == v->pad_octets && x.full_message == v->full &&
                  x.fsn == v->fsn && x.ns == v->ns && x.udt_format == v->udt_format &&
                  x.pad_nibbles == v->pad_nibbles && x.udt_opcode == v->udt_opcode && x.dd_format == v->dd_format &&
                  x.bit_padding == v->bit_padding && x.response_class == v->rclass &&
                  x.response_type == v->rtype && x.response_status == v->rstatus && x.mfid == v->mfid,
              "data header %zu (dpf %d)", i, v->dpf);
    }
#if RDOF_FEATURE_DATA_BLOCKS
    for (size_t i = 0; i < V_DBLOCK_N; i++) {
        const struct v_dblock *v = &v_dblock[i];
        rdof_data_block_t b;
        uint8_t body[24];
        int dbsn, len;
        unhex(v->burst, in);
        CHECK(rdof_decode_data_block(in, v->dt, &b), "data block %zu", i);
        bool ok = rdof_split_confirmed(&b, &dbsn, body, &len);
        CHECK(b.fec_ok == v->fec_ok && b.errors == v->errors && hex_eq(b.payload, (size_t)b.len, v->payload) &&
                  dbsn == v->dbsn && ok == v->crc9_ok,
              "data block %zu", i);
    }
#endif

}

/* ── MMDVM ─────────────────────────────────────────────────────────────────── */

struct reader_out {
    char text[8192];
    size_t len;
};

static void collect_frame(void *ctx, uint8_t cmd, const uint8_t *p, size_t n)
{
    struct reader_out *o = ctx;
    if (o->len)
        o->text[o->len++] = ';';
    o->len += (size_t)snprintf(o->text + o->len, 4, "%02x:", cmd);
    tohex(p, n, o->text + o->len);
    o->len += 2 * n;
}

static void test_mmdvm(void)
{
    static uint8_t in[8192], out[600];
    for (size_t i = 0; i < V_CONFIG_N; i++) {
        const struct v_config *v = &v_config[i];
        rdof_modem_config_t c = {v->rx_inv, v->tx_inv, v->ptt_inv, v->tx_delay, v->rx_level, v->tx_level,
                               v->cwid_level, v->rx_dc, v->tx_dc, v->dmr_delay};
        rdof_mmdvm_config_v1(&c, v->cc, v->state, v->duplex, out);
        CHECK(hex_eq(out, 23, v->v1), "config v1 %zu", i);
        rdof_mmdvm_config_v2(&c, v->cc, v->state, v->duplex, out);
        CHECK(hex_eq(out, 37, v->v2), "config v2 %zu", i);
    }
    rdof_modem_config_t d;
    rdof_modem_config_default(&d);
    CHECK(d.tx_delay_ms == 100 && d.rx_level == 50.0 && !d.rx_invert, "config default");
    for (size_t i = 0; i < V_FRAME_N; i++) {
        size_t n = unhex(v_frame[i].payload, in);
        size_t len = rdof_mmdvm_frame((uint8_t)v_frame[i].cmd, in, n, out, sizeof out);
        CHECK(hex_eq(out, len, v_frame[i].frame), "frame %zu", i);
    }
    for (size_t i = 0; i < V_SETFREQ_N; i++) {
        size_t n = rdof_mmdvm_set_freq_payload((uint32_t)v_setfreq[i].rx, (uint32_t)v_setfreq[i].tx, v_setfreq[i].level, out);
        CHECK(hex_eq(out, n, v_setfreq[i].payload), "set freq %zu", i);
    }
    for (size_t i = 0; i < V_READER_N; i++) {
        static rdof_mmdvm_reader_t r;
        static struct reader_out o;
        size_t n = unhex(v_reader[i].stream, in), chunk = (size_t)v_reader[i].chunk;
        o.len = 0;
        o.text[0] = 0;
        rdof_mmdvm_reader_init(&r);
        for (size_t k = 0; k < n; k += chunk)
            rdof_mmdvm_reader_feed(&r, in + k, n - k < chunk ? n - k : chunk, collect_frame, &o);
        o.text[o.len] = 0;
        CHECK(strcmp(o.text, v_reader[i].frames) == 0, "reader %zu", i);
    }
    for (size_t i = 0; i < V_STATUS_N; i++) {
        const struct v_status *v = &v_status[i];
        rdof_mmdvm_status_t s = {0};
        size_t n = unhex(v->payload, in);
        bool ok = rdof_mmdvm_parse_status(v->protocol, in, n, &s);
        CHECK(ok == v->ok && (!ok || (s.transmitting == v->transmitting && s.carrier == v->carrier &&
                                      s.space == v->space && s.space1 == v->space1 && s.inbound_busy == v->inbound)),
              "status %zu", i);
    }
    for (size_t i = 0; i < V_VERSION_N; i++) {
        char desc[128];
        size_t n = unhex(v_version[i].payload, in);
        int proto = rdof_mmdvm_parse_version(in, n, desc, sizeof desc);
        CHECK(proto == v_version[i].protocol && strcmp(desc, v_version[i].desc) == 0, "version %zu: '%s'", i, desc);
    }
    for (size_t i = 0; i < V_DETECT_N; i++)
        CHECK(strcmp(rdof_radio_kind_name(rdof_radio_detect(v_detect[i].desc)), v_detect[i].kind) == 0, "detect %zu", i);
    for (int k = RDOF_RADIO_HOTSPOT; k <= RDOF_RADIO_SPLIT; k++)
        CHECK(rdof_radio_kind_of(rdof_radio_kind_name((rdof_radio_kind_t)k)) == (rdof_radio_kind_t)k, "kind of %s",
              rdof_radio_kind_name((rdof_radio_kind_t)k));
    CHECK(rdof_radio_kind_of("opengd77") == RDOF_RADIO_HOTSPOT && rdof_radio_kind_of("auto") == RDOF_RADIO_UNKNOWN &&
              rdof_radio_kind_of("") == RDOF_RADIO_UNKNOWN,
          "kind of opengd77, auto, nothing");
#if RDOF_FEATURE_REPEATER
    for (size_t i = 0; i < sizeof v_has_split / sizeof v_has_split[0]; i++)
        CHECK(rdof_has_split(v_has_split[i].desc) == v_has_split[i].yes, "has split %zu", i);
#endif
    for (size_t i = 0; i < V_GD77DBM_N; i++)
        CHECK(rdof_radio_hotspot_dbm(v_gd77dbm[i].raw, (uint32_t)v_gd77dbm[i].freq) == v_gd77dbm[i].dbm, "gd77 dbm %zu", i);
#if RDOF_FEATURE_TX
    for (size_t i = 0; i < V_REGEN_N; i++) {
        unhex(v_regen[i].burst, in);
        rdof_regenerate((uint8_t)v_regen[i].control, in, v_regen[i].cc, v_regen[i].bs, out);
        CHECK(hex_eq(out, 33, v_regen[i].out), "regenerate %zu", i);
    }
    for (size_t i = 0; i < V_PREAMBLES_N; i++) {
        uint8_t b[4][33];
        rdof_csbk_preambles((uint32_t)v_preambles[i].src, (uint32_t)v_preambles[i].dst, v_preambles[i].group,
                          v_preambles[i].n, v_preambles[i].cc, b);
        CHECK(hex_eq(&b[0][0], 33 * (size_t)v_preambles[i].n, v_preambles[i].bursts), "preambles %zu", i);
    }
#endif

}

/* ── HBlink4 ───────────────────────────────────────────────────────────────── */

static void test_hblink(void)
{
    uint8_t in[256], out[256];
    for (size_t i = 0; i < V_DMRC_N; i++) {
        const struct v_dmrc *v = &v_dmrc[i];
        rdof_location_t loc = {v->lat, v->lon, v->height};
        uint32_t idle_freq = (uint32_t)v->idle_freq;
        int idle_cc = v->idle_cc;
        size_t n = rdof_dmrc_packet((uint32_t)v->peer, v->callsign, (uint32_t)v->rx, (uint32_t)v->tx, v->cc, v->slots,
                                  v->power, v->version, v->software, v->has_loc ? &loc : NULL,
                                  v->has_idle ? &idle_freq : NULL, v->has_idle ? &idle_cc : NULL, out);
        CHECK(hex_eq(out, n, v->packet), "dmrc %zu", i);
    }
    for (size_t i = 0; i < V_DMRD_N; i++) {
        const struct v_dmrd *v = &v_dmrd[i];
        unhex(v->burst, in);
        rdof_dmrd_packet(v->seq, (uint32_t)v->src, (uint32_t)v->dst, (uint32_t)v->peer, v->slot, v->group, v->dt, v->n,
                       (uint32_t)v->stream, in, v->ber, v->rssi, out);
        CHECK(hex_eq(out, RDOF_DMRD_LEN, v->packet), "dmrd %zu", i);
    }
    for (size_t i = 0; i < V_PARSE_DMRD_N; i++) {
        const struct v_parse_dmrd *v = &v_parse_dmrd[i];
        rdof_net_burst_t nb;
        size_t n = unhex(v->packet, in);
        bool ok = rdof_parse_dmrd(in, n, &nb);
        CHECK(ok == v->ok && (!ok || (nb.control == v->control && nb.terminator == v->terminator &&
                                      nb.src == v->src && nb.dst == v->dst && nb.group == v->group &&
                                      nb.slot == v->slot && hex_eq(nb.stream_id, 4, v->stream) &&
                                      !memcmp(nb.burst, in + 20, 33))),
              "parse dmrd %zu", i);
    }
    for (size_t i = 0; i < V_DMRT_N; i++) {
        const struct v_dmrt *v = &v_dmrt[i];
        rdof_tune_t t;
        size_t n = unhex(v->packet, in);
        bool ok = rdof_parse_dmrt(in, n, &t);
        CHECK(ok == v->ok && (!ok || (hex_eq(t.stream_id, 4, v->stream) && t.freq == v->freq && t.cc == v->cc &&
                                      t.power == v->power && t.preempt == v->preempt)),
              "dmrt %zu", i);
    }
    for (size_t i = 0; i < V_DMRK_N; i++) {
        unhex(v_dmrk[i].stream, in);
        rdof_dmrk_packet((uint32_t)v_dmrk[i].peer, in, v_dmrk[i].status, (uint32_t)v_dmrk[i].freq, v_dmrk[i].cc, out);
        CHECK(hex_eq(out, RDOF_DMRK_LEN, v_dmrk[i].packet), "dmrk %zu", i);
    }
}

/* ── tracker + HBlink sink ─────────────────────────────────────────────────── */

#if RDOF_FEATURE_TRACKER
struct capture {
    const struct v_scenario *sc;
    int next;
    int bad;
    int rand_k;
};

static void expect(struct capture *c, const char *kind, const char *text)
{
    const struct v_scenario *sc = c->sc;
    checks++;
    if (c->next >= sc->nouts) {
        failures++;
        if (c->bad++ < 3)
            printf("FAIL %s: extra output %s %s\n", sc->name, kind, text);
        c->next++;
        return;
    }
    const struct v_out *o = &sc->outs[c->next++];
    if (strcmp(o->kind, kind) || strcmp(o->text, text)) {
        failures++;
        if (c->bad++ < 3)
            printf("FAIL %s output %d:\n  want %s %s\n  got  %s %s\n", sc->name, c->next - 1, o->kind, o->text, kind, text);
    }
}

static void cap_json(void *ctx, const char *ch, const char *json) { expect(ctx, "json", json); }
static void cap_raw(void *ctx, const char *ch, const char *json) { expect(ctx, "raw", json); }

static void cap_raw_burst(void *ctx, const char *ch, const rdof_rawburst_t *r)
{
    uint8_t b[RDOF_RAWBURST_MAX];
    char hex[2 * RDOF_RAWBURST_MAX + 1];
    tohex(b, rdof_rawburst_encode(r, b), hex);
    expect(ctx, "rawb", hex);
}

static void cap_frame(void *ctx, const char *ch, const rdof_frame_t *f)
{
    char text[200], hex[67], src[16] = "-", dst[16] = "-";
    if (f->has_addr) {
        snprintf(src, sizeof src, "%lu", (unsigned long)f->src);
        snprintf(dst, sizeof dst, "%lu", (unsigned long)f->dst);
    }
    tohex(f->burst, 33, hex);
    snprintf(text, sizeof text, "%d,%d,%d,%s,%s,%d,%d,%d,%d,%d,%s", f->slot, f->dt, f->n, src, dst, f->group, f->new_stream,
            f->synthetic, f->errors, f->rssi, hex);
    expect(ctx, "frame", text);
}

static void hb_send(void *ctx, const uint8_t *data, size_t len)
{
    char hex[2 * RDOF_DMRD_LEN + 1];
    tohex(data, len, hex);
    expect(ctx, "dmrd", hex);
}

static void hb_before(void *ctx) { expect(ctx, "before", ""); }

static uint32_t hb_rand(void *ctx)
{
    struct capture *c = ctx;
    return (uint32_t)v_stream_ids[c->rand_k++] - 1; /* the sink maps r -> r % 0xFFFFFFFE + 1 */
}

static void test_tracker(void)
{
    for (size_t s = 0; s < V_SCENARIOS_N; s++) {
        const struct v_scenario *sc = &v_scenarios[s];
        struct capture cap = {sc, 0, 0, 0};
        static rdof_tracker_t tr;
        static rdof_hb_sink_t hb;
        rdof_tracker_init(&tr, sc->channel, sc->rssi_offset, sc->freq_hz);
        rdof_sink_t sink = {cap_json, cap_raw, cap_frame, &cap, .on_raw_burst = cap_raw_burst};
        rdof_tracker_add_sink(&tr, sink);
        rdof_hb_sink_init(&hb, 9990003);
        hb.send = hb_send;
        hb.before_stream = hb_before;
        hb.rand32 = hb_rand;
        hb.ctx = &cap;
        rdof_tracker_add_sink(&tr, rdof_hb_sink_as_sink(&hb));
        for (int i = 0; i < sc->nsteps; i++) {
            const struct v_step *st = &sc->steps[i];
            if (st->op == 't') {
                rdof_tracker_tick(&tr, st->t);
                continue;
            }
            rdof_burst_t b;
            memset(&b, 0, sizeof b);
            unhex(st->burst, b.burst);
            b.dt = st->dt;
            b.n = st->n;
            b.slot = st->slot;
            b.family = (rdof_family_t)st->family;
            b.cc = st->cc;
            b.sync_errors = st->sync_errors;
            b.power_dbfs = st->power;
            rdof_tracker_feed(&tr, &b, st->t);
        }
        checks++;
        if (cap.next != sc->nouts) {
            failures++;
            printf("FAIL %s: %d outputs, want %d\n", sc->name, cap.next, sc->nouts);
        }
    }
}

#endif
/* hblink transport sctp and its sctp_* keys (firmware only). */
static void test_config_sctp(void)
{
    static const struct {
        const char *extra, *want; /* want: the sctp= part of the description, or "error" */
    } cases[] = {
        {"", ""},
        {", \"transport\": \"sctp\"", "sctp=encap:9899,heartbeat:10000,retransmit:3,ttl:0;"},
        {", \"transport\": \"sctp\", \"sctp_encap_port\": 9900, \"sctp_heartbeat_ms\": 5000, "
         "\"sctp_max_retransmit\": 5, \"sctp_ttl_ms\": 400",
         "sctp=encap:9900,heartbeat:5000,retransmit:5,ttl:400;"},
        {", \"transport\": \"sctp\", \"sctp_ttl_ms\": \"300\"", "sctp=encap:9899,heartbeat:10000,retransmit:3,ttl:300;"},
        {", \"transport\": \"sctp\", \"sctp_ttl_ms\": -1", "error"},
        {", \"sctp_encap_port\": 70000", "error"},
        {", \"sctp_max_retransmit\": 0", "error"},
        {", \"sctp_heartbeat_ms\": \"often\"", "error"},
    };
    const char *base = v_box[0].json, *at = strstr(base, "\"port\": 62031");
    CHECK(at != NULL, "a box vector with hblink.port");
    if (!at)
        return;
    at += strlen("\"port\": 62031");
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        static char json[16384], got[2048];
        static rdof_config_t cfg;
        char error[128];
        snprintf(json, sizeof json, "%.*s%s%s", (int)(at - base), base, cases[i].extra, at);
        bool ok = rdof_config_parse(json, strlen(json), &cfg, error, sizeof error);
        if (!strcmp(cases[i].want, "error")) {
            CHECK(!ok && strstr(error, "hblink"), "sctp %zu refused (%s)", i, ok ? "taken" : error);
            continue;
        }
        CHECK(ok, "sctp %zu: %s", i, error);
        rdof_config_describe(&cfg, got, sizeof got);
        const char *s = strstr(got, "sctp=");
        CHECK(cases[i].want[0] ? s && !strncmp(s, cases[i].want, strlen(cases[i].want)) : !s, "sctp %zu: %s", i, got);
    }
}

/* Which radios a board runs ("run", else the first roamer or repeater, else the first SDR), and the roamer that
   rdof_config_parse picks with "run". */
static void test_roles(void)
{
    static const struct {
        const char *json;
        int ok, radio, sdr, plain;
        const char *radio_name, *sdr_name;
    } cases[] = {
        {"{\"name\":\"b\",\"radios\":[{\"type\":\"sdr\",\"name\":\"S\"},{\"type\":\"opengd77\",\"name\":\"R\"}]}", 1, 1, 0,
         0, "R", ""},
        {"{\"name\":\"b\",\"radios\":[{\"type\":\"sdr\",\"name\":\"S\"},{\"type\":\"sdr\",\"name\":\"T\"}]}", 1, 0, 1, 0,
         "", "S"},
        {"{\"name\":\"b\",\"radios\":[{\"type\":\"sdr\"},{\"type\":\"repeater\"}]}", 1, 1, 0, 0, "b-repeater2", ""},
        {"{\"name\":\"b\",\"run\":[\"S\",\"R\"],\"radios\":[{\"type\":\"sdr\",\"name\":\"S\"},{\"type\":\"mmdvm_hs\","
         "\"name\":\"R\"}]}",
         1, 1, 1, 0, "R", "S"},
        {"{\"name\":\"b\",\"run\":\"b-sdr2\",\"radios\":[{\"type\":\"mmdvm_hs\"},{\"type\":\"sdr\"}]}", 1, 0, 1, 0, "",
         "b-sdr2"},
        {"{\"run\":\"R2\",\"radios\":[{\"type\":\"mmdvm_hs\",\"name\":\"R1\"},{\"type\":\"opengd77\",\"name\":\"R2\"}]}",
         1, 1, 0, 0, "R2", ""},
        {"{\"name\":\"x\",\"device\":{\"sample_rate\":288000},\"channels\":[]}", 1, 0, 1, 1, "", "x"},
        {"{\"run\":[\"R1\",\"R2\"],\"radios\":[{\"type\":\"mmdvm_hs\",\"name\":\"R1\"},{\"type\":\"opengd77\","
         "\"name\":\"R2\"}]}",
         1, 1, 0, 0, "R1", ""},
        {"{\"run\":[\"R1\",\"R2\",\"R3\"],\"radios\":[{\"type\":\"mmdvm_hs\",\"name\":\"R1\"},{\"type\":"
         "\"opengd77\",\"name\":\"R2\"},{\"type\":\"opengd77\",\"name\":\"R3\"}]}",
         0, 0, 0, 0, "", ""},
        {"{\"run\":\"nope\",\"radios\":[{\"type\":\"mmdvm_hs\",\"name\":\"R1\"}]}", 0, 0, 0, 0, "", ""},
        {"{\"run\":5,\"radios\":[{\"type\":\"mmdvm_hs\",\"name\":\"R1\"}]}", 0, 0, 0, 0, "", ""},
        {"{\"radios\":[]}", 0, 0, 0, 0, "", ""},
        {"{\"radios\":[{\"type\":\"yaesu\"}]}", 0, 0, 0, 0, "", ""},
        {"[1]", 0, 0, 0, 0, "", ""},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        rdof_roles_t r;
        char error[128];
        bool ok = rdof_box_roles(cases[i].json, strlen(cases[i].json), &r, error, sizeof error);
        CHECK(ok == cases[i].ok, "roles %zu: %d (%s)", i, ok, error);
        if (ok && cases[i].ok)
            CHECK(!strcmp(r.box, i == 5 || i == 7 ? "radiofoundry" : i == 6 ? "x" : "b") && r.has_radio == cases[i].radio && r.has_sdr == cases[i].sdr && r.sdr_plain == cases[i].plain &&
                      !strcmp(r.has_radio ? r.radio : "", cases[i].radio_name) &&
                      !strcmp(r.has_sdr ? r.sdr : "", cases[i].sdr_name),
                  "roles %zu: radio %d %s, sdr %d %s, plain %d", i, r.has_radio, r.radio, r.has_sdr, r.sdr,
                  r.sdr_plain);
    }
    /* "run" picks the roamer: the second of two in a box vector, and refuses a box whose run names only the SDR */
    static char json[16384], got[2048];
    static rdof_config_t cfg;
    char error[128];
    const char *base = v_box[0].json, *at = strstr(base, "\"radios\": [");
    CHECK(at != NULL, "a box vector with radios");
    if (!at)
        return;
    snprintf(json, sizeof json, "%.*s\"run\": \"R2\", \"radios\": [{\"type\": \"opengd77\", \"name\": \"R2\", "
             "\"peer_id\": 9990302, \"idle\": 1}, %s", (int)(at - base), base, at + strlen("\"radios\": ["));
    bool ok = rdof_config_parse(json, strlen(json), &cfg, error, sizeof error);
    CHECK(ok && !strcmp(cfg.name, "R2") && cfg.peer_id == 9990302, "run R2: %s %s", ok ? cfg.name : "", error);
    snprintf(json, sizeof json, "%.*s\"run\": \"ROAM1\", %s", (int)(at - base), base, at);
    ok = rdof_config_parse(json, strlen(json), &cfg, error, sizeof error);
    if (ok)
        rdof_config_describe(&cfg, got, sizeof got);
    CHECK(ok && !strcmp(got, v_box[0].want), "run ROAM1: the same as without run (%s)", error);
    snprintf(json, sizeof json, "%.*s\"run\": \"sdr-461\", %s", (int)(at - base), base, at);
    ok = rdof_config_parse(json, strlen(json), &cfg, error, sizeof error);
    CHECK(!ok &&
              !strcmp(error, "no roamer or repeater in radios"), "run naming only the SDR: %s", ok ? "taken" : error);
}

/* "enabled": false leaves a radio out: unchecked, "run" may name it, its index still counts in the
   next one's default name; anything but true or false is refused; a disabled SDR covers no channels and is no
   sdr_transceiver's front end. */
static void test_enabled(void)
{
    static const struct {
        const char *json, *sdr, *radio, *error;
    } cases[] = {
        {"{\"name\":\"b\",\"run\":[\"sim\",\"rtl\"],\"radios\":[{\"type\":\"sdr\",\"name\":\"sim\",\"enabled\":false},"
         "{\"type\":\"sdr\",\"name\":\"rtl\",\"enabled\":true}]}", "rtl", "", NULL},
        {"{\"name\":\"b\",\"radios\":[{\"type\":\"mmdvm_hs\",\"name\":\"R\",\"enabled\":false},{\"type\":\"sdr\","
         "\"name\":\"S\"}]}", "S", "", NULL},
        {"{\"name\":\"b\",\"radios\":[{\"type\":\"yaesu\",\"enabled\":false},{\"type\":\"sdr\"}]}", "b-sdr2", "", NULL},
        {"{\"name\":\"b\",\"run\":[\"S\",\"T\",\"R\"],\"radios\":[{\"type\":\"sdr\",\"name\":\"S\"},{\"type\":\"sdr\","
         "\"name\":\"T\",\"enabled\":false},{\"type\":\"opengd77\",\"name\":\"R\"}]}", "S", "R", NULL},
        {"{\"radios\":[{\"type\":\"sdr\",\"enabled\":\"no\"}]}", NULL, NULL, "radios[0]: enabled: true or false"},
        {"{\"radios\":[{\"type\":\"sdr\"},{\"type\":\"sdr\",\"enabled\":0}]}", NULL, NULL,
         "radios[1]: enabled: true or false"},
        {"{\"radios\":[{\"type\":\"sdr\",\"enabled\":false}]}", NULL, NULL, "no radios"},
        {"{\"run\":[\"S\",\"X\"],\"radios\":[{\"type\":\"sdr\",\"name\":\"S\"}]}", NULL, NULL,
         "run: a name that isn't in radios"},
    };
    char error[128];
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        rdof_roles_t r;
        error[0] = 0;
        bool ok = rdof_box_roles(cases[i].json, strlen(cases[i].json), &r, error, sizeof error);
        if (cases[i].error)
            CHECK(!ok && !strcmp(error, cases[i].error), "enabled %zu: want \"%s\", got %s", i, cases[i].error,
                  ok ? "ok" : error);
        else
            CHECK(ok && r.has_sdr && !strcmp(r.sdr, cases[i].sdr) && r.has_radio == !!cases[i].radio[0] &&
                      !strcmp(r.has_radio ? r.radio : "", cases[i].radio),
                  "enabled %zu: sdr %s, radio %s (%s)", i, r.has_sdr ? r.sdr : "-", r.has_radio ? r.radio : "-", error);
    }
    static rdof_config_t cfg;
    static const char covered[] =
        "{\"cc\": 1, \"channels\": {\"1\": 461.0375, \"2\": 461.0875}, \"radios\": [{\"type\": \"mmdvm_hs\","
        " \"peer_id\": 1, \"idle\": 1}, {\"type\": \"sdr\", \"channels\": [1]}, {\"type\": \"sdr\", \"enabled\": false,"
        " \"channels\": [2, 9]}]}";
    bool ok = rdof_config_parse(covered, strlen(covered), &cfg, error, sizeof error);
    CHECK(ok && cfg.ncovered == 1 && cfg.covered[0] == 461037500, "a disabled SDR covers nothing: %d (%s)",
          ok ? cfg.ncovered : -1, error);
    static const char sdrtx[] =
        "{\"cc\": 1, \"channels\": {\"1\": 461.0375}, \"radios\": [{\"type\": \"sdr_transceiver\", \"peer_id\": 1,"
        " \"idle\": 1}, {\"type\": \"sdr\", \"enabled\": false, \"channels\": [1]}]}";
    ok = rdof_config_parse(sdrtx, strlen(sdrtx), &cfg, error, sizeof error);
    CHECK(!ok && !strcmp(error, "an sdr_transceiver needs an SDR in radios: its front end is the radio"),
          "a disabled SDR is no front end: %s", ok ? "taken" : error);
}

/* Two radios a board: "run" names both, each parsed by name, each with its own port; ports that clash refused; and
   which radio a USB device goes to. */
static void test_radios(void)
{
    static const char box[] =
        "{\"name\": \"b\", \"cc\": 1, \"modem\": \"uart\", \"uart\": {\"tx\": 5, \"rx\": 6},"
        " \"channels\": {\"1\": 461.0375, \"2\": 461.0875},"
        " \"run\": [\"A\", \"B\", \"S\"], \"radios\": ["
        "{\"type\": \"opengd77\", \"name\": \"A\", \"peer_id\": 1, \"idle\": 1, \"modem\": \"usb\","
        " \"usb\": {\"serial\": \"ODR-17\"}},"
        "{\"type\": \"radio_hotspot\", \"name\": \"B\", \"peer_id\": 2, \"idle\": 2, \"speed\": 57600,"
        " \"uart\": {\"num\": 2, \"tx\": 17, \"rx\": 18}},"
        "{\"type\": \"sdr\", \"name\": \"S\", \"device\": {\"sample_rate\": 288000}, \"channels\": [1]}]}";
    rdof_roles_t r;
    char error[160];
    bool ok = rdof_box_roles(box, strlen(box), &r, error, sizeof error);
    CHECK(ok && r.nradios == 2 && !strcmp(r.radios[0], "A") && !strcmp(r.radios[1], "B") && r.has_sdr &&
              !strcmp(r.radio, "A"),
          "two radios and an SDR: %s", error);
    static rdof_config_t a, b, c;
    ok = rdof_config_parse_radio(box, strlen(box), "B", &b, error, sizeof error) &&
         rdof_config_parse_radio(box, strlen(box), "A", &a, error, sizeof error) &&
         rdof_config_parse(box, strlen(box), &c, error, sizeof error);
    CHECK(ok && !strcmp(a.name, "A") && !strcmp(b.name, "B") && !strcmp(c.name, "A"), "parsed by name: %s", error);
    CHECK(!strcmp(a.modem_port, "usb") && !strcmp(a.usb_serial, "ODR-17") && !a.usb_port[0] && a.uart_num == 1 &&
              a.uart_speed == 115200,
          "A's port: %s %s", a.modem_port, a.usb_serial);
    CHECK(!strcmp(b.modem_port, "uart") && b.uart_num == 2 && b.uart_tx == 17 && b.uart_rx == 18 &&
              b.uart_speed == 57600 && !strcmp(b.radio_type, "radio_hotspot"),
          "B's port: %s UART%d %d/%d at %ld", b.modem_port, b.uart_num, b.uart_tx, b.uart_rx, b.uart_speed);
    CHECK(!rdof_config_parse_radio(box, strlen(box), "S", &c, error, sizeof error) &&
              !strcmp(error, "S isn't a roamer or repeater this board runs"),
          "the SDR by name: %s", error);
    const rdof_config_t *two[2] = {&a, &b};
    CHECK(rdof_config_check_ports(two, 2, error, sizeof error), "USB and UART2: %s", error);
    /* the same UART, then shared pins, then two USB radios with nothing to tell them apart */
    c = b;
    snprintf(c.name, sizeof c.name, "C");
    const rdof_config_t *same[2] = {&b, &c};
    CHECK(!rdof_config_check_ports(same, 2, error, sizeof error) &&
              !strcmp(error, "B and C are both on UART2: give one \"uart\": {\"num\": 2, ...}"),
          "same UART: %s", error);
    c.uart_num = 1;
    CHECK(!rdof_config_check_ports(same, 2, error, sizeof error) &&
              !strcmp(error, "B and C share a UART pin"), "pins: %s", error);
    c.uart_tx = 5;
    c.uart_rx = 6;
    CHECK(rdof_config_check_ports(same, 2, error, sizeof error), "UART1 and UART2: %s", error);
    c = a;
    c.usb_serial[0] = 0;
    const rdof_config_t *usb2[2] = {&a, &c};
    CHECK(!rdof_config_check_ports(usb2, 2, error, sizeof error) &&
              !strcmp(error, "A and A are both radios on USB: tell them apart with \"usb\": {\"serial\": ...} or "
                             "{\"port\": ...} on each"), "usb: %s", error);
    snprintf(c.usb_port, sizeof c.usb_port, "2");
    CHECK(rdof_config_check_ports(usb2, 2, error, sizeof error), "serial and port: %s", error);
    snprintf(c.usb_serial, sizeof c.usb_serial, "ODR-17");
    c.usb_port[0] = 0;
    CHECK(!rdof_config_check_ports(usb2, 2, error, sizeof error) &&
              !strcmp(error, "A and A would match the same USB device"), "same serial: %s",
          error);
    /* modem must be usb or uart */
    static char bad[sizeof box + 16];
    snprintf(bad, sizeof bad, "%s", box);
    char *m = strstr(bad, "\"modem\": \"usb\"");
    memcpy(m + 10, "udp", 3);
    CHECK(!rdof_config_parse_radio(bad, strlen(bad), "A", &c, error, sizeof error) &&
              !strcmp(error, "modem udp: usb or uart"),
          "modem udp: %s", error);

    /* sdr_transceiver: the SDR's front end as a roamer (half) or a repeater (full); its port is the SDR */
    static const char tx[] =
        "{\"name\": \"b\", \"cc\": 1, \"channels\": {\"1\": 461.0375, \"2\": 461.0875}, \"run\": [\"T\", \"S\"],"
        " \"radios\": [{\"type\": \"sdr_transceiver\", \"name\": \"T\", \"peer_id\": 3, \"idle\": 1,"
        " \"tx_power_dbm\": 14, \"modem\": \"uart\"},"
        "{\"type\": \"sdr\", \"name\": \"S\", \"device\": {\"sample_rate\": 288000}, \"channels\": [1, 2]}]}";
    ok = rdof_config_parse_radio(tx, strlen(tx), "T", &c, error, sizeof error);
    CHECK(ok && c.mode == RDOF_CFG_ROAMER && c.sdr_duplex == RDOF_SDR_DUPLEX_HALF && c.tx_power_dbm == 14 &&
              !strcmp(c.modem_port, "sdr") && !strcmp(c.radio_type, "auto") && c.idle.freq == 461037500,
          "sdr_transceiver, half: %s", error);
    static char txf[sizeof tx + 64];
    snprintf(txf, sizeof txf, "%s", tx);
    char *at = strstr(txf, "\"idle\": 1,");
    const char full[] = "\"duplex\": \"full\", \"rx_freq\": 461.0875, \"tx_freq\": 466.0875,";
    memmove(at + strlen(full), at + 10, strlen(at + 10) + 1);
    memcpy(at, full, strlen(full));
    ok = rdof_config_parse_radio(txf, strlen(txf), "T", &c, error, sizeof error);
    CHECK(ok && c.mode == RDOF_CFG_REPEATER && c.sdr_duplex == RDOF_SDR_DUPLEX_FULL && c.rx_freq == 461087500 &&
              c.tx_freq == 466087500 && !strcmp(c.modem_port, "sdr"),
          "sdr_transceiver, full: %s", error);
    memcpy(strstr(txf, "full"), "fool", 4);
    CHECK(!rdof_config_parse_radio(txf, strlen(txf), "T", &c, error, sizeof error) &&
              !strcmp(error, "duplex fool: half (a roamer) or full (a duplex repeater)"),
          "duplex fool: %s", error);
    snprintf(txf, sizeof txf, "%s", tx);
    memcpy(strstr(txf, "\"type\": \"sdr\","), "\"type\":\"auto\",", 14);
    CHECK(!rdof_config_parse_radio(txf, strlen(txf), "T", &c, error, sizeof error) &&
              !strcmp(error, "an sdr_transceiver needs an SDR in radios: its front end is the radio"),
          "no SDR: %s", error);
    rdof_config_t t2;
    ok = rdof_config_parse_radio(tx, strlen(tx), "T", &t2, error, sizeof error);
    snprintf(t2.name, sizeof t2.name, "T2");
    const rdof_config_t *twotx[2] = {&c, &t2};
    rdof_config_parse_radio(tx, strlen(tx), "T", &c, error, sizeof error);
    CHECK(!rdof_config_check_ports(twotx, 2, error, sizeof error) &&
              !strcmp(error, "T and T2 both use the SDR's modem: the board has one SDR"), "two on the SDR: %s",
          error);

#if RDOF_FEATURE_REPEATER
    /* a split repeater: each half's port on the board */
    static const char sp[] =
        "{\"name\": \"b\", \"cc\": 1, \"channels\": {\"1\": 461.0375},"
        " \"radios\": [{\"type\": \"repeater\", \"name\": \"R\", \"peer_id\": 3, \"rx_freq\": 461.0375,"
        " \"tx_freq\": 466.0375, \"radio\": {\"type\": \"split\", \"power_cap\": 32, \"max_key_s\": 120,"
        " \"tx\": {\"usb\": {\"serial\": \"ODR-9\"}},"
        " \"rx\": {\"modem\": \"uart\", \"uart\": {\"num\": 2, \"tx\": 17, \"rx\": 18}, \"speed\": 57600}}}]}";
    ok = rdof_config_parse_radio(sp, strlen(sp), "R", &c, error, sizeof error);
    CHECK(ok && !strcmp(c.radio_type, "split") && !strcmp(c.modem_port, "split") &&
              !strcmp(c.split[0].modem_port, "usb") && !strcmp(c.split[0].usb_serial, "ODR-9") &&
              !strcmp(c.split[1].modem_port, "uart") && c.split[1].uart_num == 2 && c.split[1].uart_speed == 57600 &&
              !strcmp(c.usb_serial, "ODR-9") && c.uart_num == 2 && c.split_power_cap == 32 &&
              c.split_max_key_s == 120 && rdof_config_uses_usb(&c),
          "split: %s", error);
    /* its UART half against another radio on UART2 */
    const rdof_config_t *sb[2] = {&c, &b};
    CHECK(!rdof_config_check_ports(sb, 2, error, sizeof error) &&
              !strcmp(error, "R and B are both on UART2: give one \"uart\": {\"num\": 2, ...}"),
          "split vs B: %s", error);
    /* both halves on USB; the SDR as the TX half; the SDR as the RX half */
    static char sp2[sizeof sp + 32];
    snprintf(sp2, sizeof sp2, "%s", sp);
    memcpy(strstr(sp2, "\"modem\": \"uart\""), "\"modem\": \"usb\" ", 15);
    CHECK(!rdof_config_parse_radio(sp2, strlen(sp2), "R", &a, error, sizeof error) &&
              !strcmp(error, "radio.tx and radio.rx are both on USB: one half on USB at most (the other on a UART, or "
                             "the SDR as the RX half)"),
          "two USB halves: %s", error);
    snprintf(sp2, sizeof sp2, "%s", sp);
    {
        char *t = strstr(sp2, "\"tx\": {\"usb\"") + 7;
        const char ins[] = "\"modem\": \"sdr\", ";
        memmove(t + strlen(ins), t, strlen(t) + 1);
        memcpy(t, ins, strlen(ins));
    }
    ok = rdof_config_parse_radio(sp2, strlen(sp2), "R", &a, error, sizeof error);
    CHECK(ok && !strcmp(a.split[0].modem_port, "sdr"), "SDR TX half: %s", error);
    memcpy(strstr(sp2, "\"modem\": \"uart\""), "\"modem\": \"sdr\" ", 15);
    CHECK(!rdof_config_parse_radio(sp2, strlen(sp2), "R", &a, error, sizeof error) &&
              !strcmp(error, "radio.tx and radio.rx are both the SDR: the board runs one SDR modem (two SDRs as a "
                             "split need two boards, or a radio for one half)"),
          "two SDR halves: %s", error);
    snprintf(sp2, sizeof sp2, "%s", sp);
    memcpy(strstr(sp2, "\"modem\": \"uart\""), "\"modem\": \"sdr\" ", 15);
    ok = rdof_config_parse_radio(sp2, strlen(sp2), "R", &a, error, sizeof error);
    CHECK(ok && !strcmp(a.split[1].modem_port, "sdr"), "SDR RX half: %s", error);
    const rdof_config_t *st[2] = {&a, &t2};
    CHECK(!rdof_config_check_ports(st, 2, error, sizeof error) &&
              !strcmp(error, "R and T2 both use the SDR's modem: the board has one SDR"), "SDR half and T: %s", error);
#endif

    /* devices to radios: by serial, by port, else the first free one of the kind */
    const rdof_usb_want_t w[4] = {{0x1FC9, 0x0094, "ODR-17", ""}, {0x1FC9, 0x0094, "", "1.2"}, {0x1FC9, 0x0094, "", ""},
                                {0x0BDA, 0x2838, "", ""}};
    bool taken[4] = {false};
    rdof_usb_dev_t d = {0x1FC9, 0x0094, "ODR-17", "1.2"};
    CHECK(rdof_usb_match(&d, w, 4, taken) == 0, "serial first");
    taken[0] = true;
    CHECK(rdof_usb_match(&d, w, 4, taken) == 1, "then the port");
    snprintf(d.serial, sizeof d.serial, "X");
    taken[0] = false;
    CHECK(rdof_usb_match(&d, w, 4, taken) == 1, "another serial on 1.2: the port's");
    snprintf(d.port, sizeof d.port, "1.3");
    CHECK(rdof_usb_match(&d, w, 4, taken) == 2, "elsewhere: the one that named nothing");
    taken[2] = true;
    CHECK(rdof_usb_match(&d, w, 4, taken) == -1, "nothing free for it");
    rdof_usb_dev_t rtl = {0x0BDA, 0x2838, "00000001", ""};
    CHECK(rdof_usb_match(&rtl, w, 4, NULL) == 3 && rdof_usb_match(&rtl, w, 3, NULL) == -1, "the RTL-SDR");
    char path[16];
    const uint8_t ports[3] = {1, 3, 12};
    rdof_usb_port_path(ports, 3, path, sizeof path);
    CHECK(!strcmp(path, "1.3.12"), "port path %s", path);
    rdof_usb_port_path(ports, 0, path, sizeof path);
    CHECK(!strcmp(path, ""), "on the board's port");
    rdof_usb_port_path(ports, 3, path, 4);
    CHECK(!strcmp(path, "1.3"), "port path cut: %s", path);
    const uint8_t hundred[1] = {100};
    rdof_usb_port_path(hundred, 1, path, sizeof path);
    CHECK(!strcmp(path, "100"), "port 100: %s", path);
    const uint8_t far[2] = {255, 7};
    rdof_usb_port_path(far, 2, path, sizeof path);
    CHECK(!strcmp(path, "255.7"), "three digits: %s", path);
    rdof_usb_port_path(far, 2, path, 3);
    CHECK(!strcmp(path, "25"), "cut inside a number: %s", path);
    rdof_usb_port_path(far, 2, path, 1);
    CHECK(!strcmp(path, ""), "room for the NUL only");
    path[0] = 'x';
    rdof_usb_port_path(far, 2, path, 0);
    CHECK(path[0] == 'x', "no room: untouched");
    /* NULL serial and port are "any", as "" is; the best named wins over an earlier one */
    const rdof_usb_want_t nw[3] = {{0x1FC9, 0x0094, NULL, NULL}, {0x1FC9, 0x0094, NULL, "1.3"},
                                 {0x1FC9, 0x0094, "X", NULL}};
    CHECK(rdof_usb_match(&d, nw, 3, NULL) == 2 && rdof_usb_match(&d, nw, 2, NULL) == 1 &&
              rdof_usb_match(&d, nw, 1, NULL) == 0 && rdof_usb_match(&d, nw, 0, NULL) == -1,
          "NULL wants");
    const rdof_usb_want_t tie[2] = {{0x1FC9, 0x0094, "", ""}, {0x1FC9, 0x0094, "", ""}};
    CHECK(rdof_usb_match(&d, tie, 2, NULL) == 0, "the first among equals");
}

static void test_config(void)
{
    for (size_t i = 0; i < V_SFR_N; i++) {
        int a = 0, b = 0;
        char fr[16];
        bool ok = rdof_sfr_slots(v_sfr[i].desc, &a, &b);
        rdof_sfr_framing(v_sfr[i].desc, fr, sizeof fr);
        CHECK(ok == v_sfr[i].ok && (!ok || (a == v_sfr[i].inbound && b == v_sfr[i].outbound)) &&
                  !strcmp(fr, v_sfr[i].framing),
              "sfr %zu: %d %d %d '%s'", i, ok, a, b, fr);
    }
    for (size_t i = 0; i < V_BOX_N; i++) {
        static rdof_config_t cfg;
        static char got[2048];
        char error[128];
        bool ok = rdof_config_parse(v_box[i].json, strlen(v_box[i].json), &cfg, error, sizeof error);
        if (ok)
            rdof_config_describe(&cfg, got, sizeof got);
        else
            strcpy(got, "error");
        CHECK(!strcmp(got, v_box[i].want), "box %zu (%s):\n  want %s\n  got  %s", i, ok ? "ok" : error, v_box[i].want,
              got);
    }
    test_config_sctp();
    test_roles();
    test_enabled();
    test_radios();
}

static void test_status(void)
{
    char out[1024];
    for (size_t i = 0; i < V_REPR_N; i++) {
        rdof_format_repr(v_repr[i].v, out, sizeof out);
        CHECK(!strcmp(out, v_repr[i].text), "repr %zu: %s vs %s", i, out, v_repr[i].text);
    }
    for (size_t i = 0; i < V_STATUS_JSON_N; i++) {
        const struct v_status_json *v = &v_status_json[i];
        rdof_location_t loc = {v->lat, v->lon, v->height};
        if (v->kind == 0) {
            rdof_roamer_status_t s = {v->at, (uint32_t)v->freq, v->cc, v->a, v->b, v->c, v->d, v->e};
            rdof_roamer_status_json(&s, out, sizeof out);
        } else if (v->kind == 1) {
            rdof_roamer_receiver_json(v->name, (uint32_t)v->freq, v->cc, v->device, v->has_loc ? &loc : NULL, out, sizeof out);
        } else if (v->kind == 2) {
            rdof_repeater_status_t s = {v->at, (uint32_t)v->freq, (uint32_t)v->tx, v->cc, v->a, v->b, v->c,
                                      {false, v->s1c, v->s2c}, {false, v->s1b, v->s2b}};
            rdof_repeater_status_json(&s, out, sizeof out);
        } else {
            rdof_repeater_receiver_json(v->name, (uint32_t)v->freq, (uint32_t)v->tx, v->cc, v->device,
                                      v->has_loc ? &loc : NULL, v->inbound, out, sizeof out);
        }
        CHECK(!strcmp(out, v->json), "status json %zu:\n  want %s\n  got  %s", i, v->json, out);
    }
}

static void test_oled(void)
{
    for (size_t i = 0; i < V_OLED_N; i++) {
        static rdof_frame_buf_t f;
        rdof_oled_clear(&f);
        int end = rdof_oled_text(&f, v_oled[i].x, v_oled[i].y, v_oled[i].text, v_oled[i].invert, v_oled[i].tight);
        CHECK(end == v_oled[i].end && rdof_oled_text_width(v_oled[i].text, v_oled[i].tight) == v_oled[i].width &&
                  hex_eq(f.buf, sizeof f.buf, v_oled[i].buf),
              "oled text %zu", i);
    }
    static rdof_frame_buf_t f;
    rdof_oled_status_t st = {"ROAM1", "10.0.0.50", true, true, false, true, false, 467375000, 467375000, 2, "listening",
                           "T201<9990123", true, "", false, "12:00:00"};
    rdof_oled_render(&f, &st);
    int lit = 0;
    for (int y = 0; y < RDOF_OLED_H; y++)
        for (int x = 0; x < RDOF_OLED_W; x++)
            lit += rdof_oled_get(&f, x, y);
    CHECK(lit > 300, "status screen draws something (%d pixels)", lit);
    uint32_t h = 2166136261u; /* FNV-1a of the frame: the C's screen, pinned */
    for (size_t i = 0; i < sizeof f.buf; i++)
        h = (h ^ f.buf[i]) * 16777619u;
    CHECK(h == 0x72e0d013u, "status screen %08x", (unsigned)h);
}

static void test_misc(void)
{
    char s[64];
    rdof_iso_time(1760000000.06, s);
    CHECK(strcmp(s, "2025-10-09T08:53:20.060Z") == 0, "iso %s", s);
    rdof_iso_time(0.0, s);
    CHECK(strcmp(s, "1970-01-01T00:00:00.000Z") == 0, "iso %s", s);
    rdof_format_float3(0.0, s, sizeof s);
    CHECK(strcmp(s, "0.0") == 0, "%s", s);
    rdof_format_float3(1.1400000001, s, sizeof s);
    CHECK(strcmp(s, "1.14") == 0, "%s", s);
    rdof_format_float3(12.0, s, sizeof s);
    CHECK(strcmp(s, "12.0") == 0, "%s", s);
    rdof_format_float3(0.0625, s, sizeof s);
    CHECK(strcmp(s, "0.062") == 0, "%s", s);
    rdof_base64((const uint8_t *)"ab", 2, s);
    CHECK(strcmp(s, "YWI=") == 0, "%s", s);
}

/* The stored settings keep their comments: wifi and ntp are moved out and back in as text. */
static void test_jsonc(void)
{
    static const char text[] =
        "{\n"
        "  // a comment, \"wifi\": not this one\n"
        "  \"name\": \"x\", /* \"ntp\": nor this */\n"
        "  \"wifi\": {\"ssid\": \"a,}\\\"b\", \"password\": \"p\"},\n"
        "  \"cc\": [1, {\"wifi\": 2}],\n"
        "  \"ntp\": \"pool.ntp.org\"\n"
        "}\n";
    char t[512], out[512];
    strcpy(t, text);
    size_t n = rdof_jsonc_drop(t, strlen(t), "wifi");
    CHECK(n == strlen(t) && !strcmp(t, "{\n  // a comment, \"wifi\": not this one\n  \"name\": \"x\", /* \"ntp\": nor this */\n"
                                       "  \"cc\": [1, {\"wifi\": 2}],\n  \"ntp\": \"pool.ntp.org\"\n}\n"),
          "drop wifi:\n%s", t);
    n = rdof_jsonc_drop(t, n, "ntp");
    CHECK(!strcmp(t, "{\n  // a comment, \"wifi\": not this one\n  \"name\": \"x\", /* \"ntp\": nor this */\n"
                     "  \"cc\": [1, {\"wifi\": 2}],\n}\n"),
          "drop ntp (a trailing comma is left):\n%s", t);
    CHECK(rdof_jsonc_drop(t, n, "absent") == n, "nothing to drop");
    size_t m = rdof_jsonc_add(t, n, "ntp", "{\"servers\":\"a b\"}", out, sizeof out);
    CHECK(m == strlen(out) && !strcmp(out, "{\n  // a comment, \"wifi\": not this one\n  \"name\": \"x\", /* \"ntp\": nor this */\n"
                                         "  \"cc\": [1, {\"wifi\": 2}],\n  \"ntp\": {\"servers\":\"a b\"}\n}\n"),
          "add after a trailing comma:\n%s", out);
    m = rdof_jsonc_add("{\"a\": 1 // c\n}", 14, "b", "2", out, sizeof out);
    CHECK(!strcmp(out, "{\"a\": 1,\n  \"b\": 2 // c\n}"), "add after a member: %s", out);
    m = rdof_jsonc_add(" { } ", 5, "b", "[]", out, sizeof out);
    CHECK(!strcmp(out, " {\n  \"b\": [] } "), "add to an empty object: %s", out);
    CHECK(rdof_jsonc_add("[1]", 3, "b", "2", out, sizeof out) == 0, "not an object");
    CHECK(rdof_jsonc_add("{\"a\": 1}", 8, "b", "2", out, 10) == 0, "doesn't fit");
    /* The result still parses as the same settings. */
    static rdof_jnode_t nodes[32];
    static char strs[256];
    rdof_jdoc_t doc = {nodes, 32, 0, strs, sizeof strs, 0, ""};
    char clean[512];
    size_t cl = rdof_jsonc_strip(out, strlen(out), clean);
    (void)cl;
    m = rdof_jsonc_add(t, n, "wifi", "{\"networks\":[]}", out, sizeof out);
    cl = rdof_jsonc_strip(out, m, clean);
    int root = rdof_jparse(&doc, clean, cl);
    CHECK(root >= 0 && rdof_jget(&doc, root, "wifi") >= 0 && rdof_jget(&doc, root, "cc") >= 0, "parses: %s", doc.error);
}

void test_txvr(void);
void test_repeater(void);
void test_guard(void);
void test_benchtx(void);
void test_split(void);
void test_jitter(void);
void test_core(void);
void test_ipgw(void);
void test_ipgw_modes(void);

/* ── the binary burst on .../raw ───────────────────────────────────────────── */

static void test_rawburst(void)
{
    for (size_t i = 0; i < sizeof v_rawburst / sizeof v_rawburst[0]; i++) {
        const struct v_rawburst *v = &v_rawburst[i];
        rdof_rawburst_t r = {.ms = v->ms, .slot = v->slot, .network = v->network, .dt = v->dt, .n = v->n,
                           .addressed = v->addressed, .src = (uint32_t)v->src, .dst = (uint32_t)v->dst,
                           .group = v->group, .utc = v->utc, .utc_us = v->utc_us, .utc_slot = v->utc_slot};
        unhex(v->burst, r.burst);
        uint8_t b[RDOF_RAWBURST_MAX];
        char hex[2 * RDOF_RAWBURST_MAX + 1], json[400];
        const size_t size = rdof_rawburst_encode(&r, b);
        CHECK(size == (v->utc ? 62u : 49u), "rawburst %zu size", i);
        tohex(b, size, hex);
        CHECK(!strcmp(hex, v->bin), "rawburst %zu binary\n  want %s\n  got  %s", i, v->bin, hex);
        CHECK(rdof_rawburst_json(&r, json, sizeof json) && !strcmp(json, v->json), "rawburst %zu json %s", i, json);
    }
    for (size_t i = 0; i < sizeof v_time_ms / sizeof v_time_ms[0]; i++) {
        char iso[32];
        rdof_iso_time(v_time_ms[i].t, iso);
        CHECK(rdof_time_ms(v_time_ms[i].t) == v_time_ms[i].ms, "time ms %zu: %lld", i, (long long)rdof_time_ms(v_time_ms[i].t));
        CHECK(!strcmp(iso, v_time_ms[i].iso), "iso time %zu: %s", i, iso);
    }
}

static void test_slotgrid(void)
{
    for (size_t i = 0; i < V_GRID_N; i++) {
        const struct v_grid *v = &v_grid[i];
        int ts, its, ots;
        int64_t ph, b, ie, oe;
        int64_t n = rdof_grid_slot_at(v->t, v->offset, &ts, &ph);
        int64_t nn = rdof_grid_next_boundary(v->t, v->offset, &b);
        int64_t in = rdof_grid_inbound(v->t, v->offset, &its, &ie);
        int64_t on = rdof_grid_outbound(v->t, v->offset, &ots, &oe);
        CHECK(n == v->at_n && ts == v->at_ts && ph == v->at_phase && nn == v->next_n && b == v->next_b &&
                  in == v->in_n && its == v->in_ts && ie == v->in_err && on == v->out_n && ots == v->out_ts &&
                  oe == v->out_err && rdof_grid_boundary(n, v->offset) + ph == v->t,
              "grid %zu: t %lld off %d", i, v->t, v->offset);
    }
    static rdof_jnode_t nodes[64];
    static char strs[512];
    for (size_t i = 0; i < V_GATES_N; i++) {
        rdof_jdoc_t doc = {nodes, 64, 0, strs, sizeof strs, 0, {0}};
        int root = rdof_jparse(&doc, v_gates[i].timing, strlen(v_gates[i].timing));
        rdof_slot_timing_t t;
        char why[96];
        bool ok = rdof_slot_timing_parse(&doc, -1, root, &t, why, sizeof why);
        CHECK(ok, "gate %zu: timing %s (%s)", i, v_gates[i].timing, why);
        rdof_slot_gate_t g;
        rdof_slot_gate_init(&g, &t);
        for (int k = 0; k < v_gates[i].nsteps; k++) {
            const struct v_gate_step *st = &v_gates[i].steps[k];
            rdof_slot_gate_update(&g, st->now, st->synced, st->source, st->err);
            CHECK(!strcmp(rdof_gate_state_name(g.state), st->state) &&
                      fabs(rdof_slot_gate_holdover_age(&g, st->now) - st->age) < 1e-9,
                  "gate %zu step %d: %s age %g, want %s %g", i, k, rdof_gate_state_name(g.state),
                  rdof_slot_gate_holdover_age(&g, st->now), st->state, st->age);
        }
    }
    for (size_t i = 0; i < V_SCLOCKS_N; i++) {
        const struct v_sclock *v = &v_sclocks[i];
        rdof_sample_clock_t c;
        rdof_sample_clock_init(&c, v->rate, v->window, v->stamped);
        c.ppm = v->ppm;
        int bad = 0;
        for (int k = 0; k < v->nsteps; k++) {
            const struct v_sclock_step *st = &v->steps[k];
            rdof_sample_clock_add(&c, st->end, st->arrival);
            int64_t at = 0;
            double back = 0;
            int chunk = (int)(k == 0 ? st->end : st->end - v->steps[k - 1].end);
            rdof_sample_clock_utc(&c, (double)(st->end - chunk / 3), &at);
            rdof_sample_clock_index(&c, at, &back);
            if (at != st->utc_at || c.has_err != (bool)st->has_err || (c.has_err && c.err_ns != st->err) ||
                fabs(back - st->index_back) > 1e-6 * fabs(st->index_back) + 1e-6)
                bad++;
        }
        CHECK(bad == 0, "sample clock %zu: %d steps differ", i, bad);
    }
    /* the setting's describe */
    rdof_slot_timing_t t;
    rdof_slot_timing_default(&t);
    char d[64];
    rdof_slot_timing_describe(&t, d, sizeof d);
    CHECK(!strcmp(d, "free,0,20,2000,holdover,60.0"), "describe %s", d);
}

int main(void)
{
#ifdef RDOF_RUST
    const char *mismatch = rdof_rust_layout_mismatch();
    if (mismatch) { /* the tests would only corrupt memory */
        printf("FAIL rust's mirror of %s differs from the C header\n", mismatch);
        return 1;
    }
#endif
    test_fec();
    test_layer2();
    test_pdu();
    test_mmdvm();
    test_hblink();
#if RDOF_FEATURE_TRACKER
    test_tracker();
#endif
    test_rawburst();
    test_misc();
    test_jsonc();
    test_config();
    test_status();
    test_oled();
    test_txvr();
    test_repeater();
    test_split();
    test_jitter();
    test_slotgrid();
    test_guard();
    test_benchtx();
    test_core();
    test_ipgw();
    test_ipgw_modes();
    printf("%d checks, %d failures (features: tx %d, data blocks %d, tracker %d)\n", checks, failures,
           RDOF_FEATURE_TX, RDOF_FEATURE_DATA_BLOCKS, RDOF_FEATURE_TRACKER);
    return failures ? 1 : 0;
}
