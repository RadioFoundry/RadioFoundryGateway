// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DMR voice link control: full LC, RS(12,9), embedded LC, CRC-CCITT, talker alias, GPS Info (see dmr_lc.h)
#include <string.h>
#include "dmr_lc.h"
#include "dmrFec.h"

// --- Full LC ---

void dmr_lc_pack(const dmr_lc_t *lc, uint8_t out[DMR_LC_BYTES])
{
	out[0] = (uint8_t)((lc->pf ? 0x80 : 0x00) | (lc->flco & 0x3F));
	out[1] = lc->fid;
	out[2] = lc->svc;
	out[3] = (uint8_t)(lc->dst >> 16);
	out[4] = (uint8_t)(lc->dst >> 8);
	out[5] = (uint8_t)lc->dst;
	out[6] = (uint8_t)(lc->src >> 16);
	out[7] = (uint8_t)(lc->src >> 8);
	out[8] = (uint8_t)lc->src;
}

void dmr_lc_unpack(const uint8_t in[DMR_LC_BYTES], dmr_lc_t *lc)
{
	lc->pf = (in[0] & 0x80) != 0;
	lc->flco = in[0] & 0x3F;
	lc->fid = in[1];
	lc->svc = in[2];
	lc->dst = ((uint32_t)in[3] << 16) | ((uint32_t)in[4] << 8) | in[5];
	lc->src = ((uint32_t)in[6] << 16) | ((uint32_t)in[7] << 8) | in[8];
}

bool dmr_lc_is_voice(const uint8_t lc[DMR_LC_BYTES])
{
	uint8_t flco = lc[0] & 0x3F;

	return flco == DMR_FLCO_GROUP_VOICE || flco == DMR_FLCO_UNIT_VOICE;
}

// --- GF(256), primitive polynomial x^8 + x^4 + x^3 + x^2 + 1 ---

static uint8_t gf_mul(uint8_t a, uint8_t b)
{
	uint8_t p = 0;

	while (b) {
		if (b & 1)
			p ^= a;
		a = (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1D : 0x00));
		b >>= 1;
	}
	return p;
}

static uint8_t gf_pow(uint8_t a, unsigned n)
{
	uint8_t r = 1;

	while (n--)
		r = gf_mul(r, a);
	return r;
}

static uint8_t gf_inv(uint8_t a)
{
	return gf_pow(a, 254);	// a^255 = 1
}

// --- RS(12,9): g(x) = (x + a)(x + a^2)(x + a^3) = x^3 + 0x0E x^2 + 0x38 x + 0x40 ---

void dmr_rs129_parity(const uint8_t data[DMR_LC_BYTES], uint8_t parity[3])
{
	uint8_t r2 = 0, r1 = 0, r0 = 0;	// remainder r2 x^2 + r1 x + r0

	for (int i = 0; i < DMR_LC_BYTES; i++) {
		uint8_t fb = data[i] ^ r2;

		r2 = r1 ^ gf_mul(fb, 0x0E);
		r1 = r0 ^ gf_mul(fb, 0x38);
		r0 = gf_mul(fb, 0x40);
	}
	parity[0] = r2;
	parity[1] = r1;
	parity[2] = r0;
}

void dmr_lc_encode_full(const uint8_t lc[DMR_LC_BYTES], uint8_t mask, uint8_t out[DMR_LC_FULL_BYTES])
{
	uint8_t p[3];

	dmr_rs129_parity(lc, p);
	memmove(out, lc, DMR_LC_BYTES);
	out[9] = p[0] ^ mask;
	out[10] = p[1] ^ mask;
	out[11] = p[2] ^ mask;
}

int dmr_lc_decode_full(const uint8_t in[DMR_LC_FULL_BYTES], uint8_t mask, uint8_t lc[DMR_LC_BYTES])
{
	uint8_t c[DMR_LC_FULL_BYTES];
	uint8_t s[4] = { 0, 0, 0, 0 };

	memcpy(c, in, DMR_LC_FULL_BYTES);
	c[9] ^= mask;
	c[10] ^= mask;
	c[11] ^= mask;

	// Syndromes S_j = c(a^j), octet k the coefficient of x^(11 - k)
	for (int j = 1; j <= 3; j++) {
		uint8_t aj = gf_pow(2, (unsigned)j), acc = 0;

		for (int k = 0; k < DMR_LC_FULL_BYTES; k++)
			acc = gf_mul(acc, aj) ^ c[k];	// Horner
		s[j] = acc;
	}

	int result = 0;

	if (s[1] | s[2] | s[3]) {
		result = -1;
		// One octet in error, value Y at degree d: S1 = Y X, S2 = Y X^2, S3 = Y X^3 with X = a^d
		if (s[1] && s[2]) {
			uint8_t x = gf_mul(s[2], gf_inv(s[1]));

			if (gf_mul(s[2], x) == s[3]) {
				uint8_t ad = 1;

				for (int d = 0; d < DMR_LC_FULL_BYTES; d++, ad = gf_mul(ad, 2)) {
					if (ad == x) {
						c[11 - d] ^= gf_mul(s[1], gf_inv(x));
						result = 1;
						break;
					}
				}
			}
		}
	}
	memcpy(lc, c, DMR_LC_BYTES);
	return result;
}

// --- CRC-CCITT ---

uint16_t dmr_crc_ccitt(const uint8_t *buf, uint8_t len)
{
	uint16_t crc = 0;

	for (uint8_t i = 0; i < len; i++) {
		crc ^= (uint16_t)(buf[i] << 8);
		for (int b = 0; b < 8; b++)
			crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
	}
	return (uint16_t)~crc;
}

void dmr_crc16_append(uint8_t buf[12], uint16_t mask)
{
	uint16_t crc = dmr_crc_ccitt(buf, 10) ^ mask;

	buf[10] = (uint8_t)(crc >> 8);
	buf[11] = (uint8_t)crc;
}

void dmr_csbk_bs_activation(uint32_t own_id, uint8_t out[12])
{
	memset(out, 0, 12);
	out[0] = 0x80 | 0x38;	// LB, PF 0, CSBKO BS_Dwn_Act
	out[1] = 0x00;		// FID standard
	// out[2..3] reserved, out[4..6] BS address 0 (any)
	out[7] = (uint8_t)(own_id >> 16);
	out[8] = (uint8_t)(own_id >> 8);
	out[9] = (uint8_t)own_id;
	dmr_crc16_append(out, DMR_CRC_MASK_CSBK);
}

// --- Embedded LC ---
//
// Matrix of 8 rows x 16 columns, bit (r, c) at index 16r + c. Rows 0-1 carry 11 LC bits, rows 2-6 carry 10 LC bits and
// one checksum bit in column 10 (checksum MSB in row 2); columns 11-15 of rows 0-6 are the Hamming(16,11,4) parity
// (the Hamming(15,11,3) parity, then the overall parity). Row 7 is the even parity of each column. Transmission is
// column by column: air bit k is matrix index (16k) mod 127, and air bit 127 is index 127.

static inline int bit_get(const uint8_t *b, int i)
{
	return (b[i >> 3] >> (7 - (i & 7))) & 1;
}

static inline void bit_put(uint8_t *b, int i, int v)
{
	if (v)
		b[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
	else
		b[i >> 3] &= (uint8_t)~(0x80 >> (i & 7));
}

static int matrix_to_air(int k)
{
	return k == 127 ? 127 : (k * 16) % 127;
}

uint8_t dmr_emb_lc_checksum(const uint8_t lc[DMR_LC_BYTES])
{
	unsigned sum = 0;

	for (int i = 0; i < DMR_LC_BYTES; i++)
		sum += lc[i];
	return (uint8_t)(sum % 31);
}

static int row_data_bits(int r)
{
	return r < 2 ? 11 : 10;
}

void dmr_emb_lc_encode_bits(const uint8_t lc[DMR_LC_BYTES], uint8_t bits[16])
{
	uint8_t m[128];
	uint8_t cs = dmr_emb_lc_checksum(lc);
	int n = 0;

	memset(m, 0, sizeof(m));
	for (int r = 0; r < 7; r++) {
		for (int c = 0; c < row_data_bits(r); c++, n++)
			m[16 * r + c] = (uint8_t)bit_get(lc, n);
		if (r >= 2)
			m[16 * r + 10] = (cs >> (6 - r)) & 1;

		uint16_t data = 0;
		for (int c = 0; c < 11; c++)
			data = (uint16_t)((data << 1) | m[16 * r + c]);
		uint16_t cw = dmrFecHamming15113Encode(data);
		uint8_t par = 0;
		for (int c = 0; c < 15; c++) {
			m[16 * r + c] = (cw >> (14 - c)) & 1;
			par ^= m[16 * r + c];
		}
		m[16 * r + 15] = par;
	}
	for (int c = 0; c < 16; c++) {
		uint8_t p = 0;

		for (int r = 0; r < 7; r++)
			p ^= m[16 * r + c];
		m[112 + c] = p;
	}
	memset(bits, 0, 16);
	for (int k = 0; k < 128; k++)
		bit_put(bits, k, m[matrix_to_air(k)]);
}

void dmr_emb_lc_encode(const uint8_t lc[DMR_LC_BYTES], uint8_t frag[4][4])
{
	uint8_t bits[16];

	dmr_emb_lc_encode_bits(lc, bits);
	for (int i = 0; i < 4; i++)
		memcpy(frag[i], &bits[4 * i], 4);
}

bool dmr_emb_lc_decode_bits(const uint8_t bits[16], uint8_t lc[DMR_LC_BYTES])
{
	uint8_t m[128];

	for (int k = 0; k < 128; k++)
		m[matrix_to_air(k)] = (uint8_t)bit_get(bits, k);

	for (int r = 0; r < 7; r++) {
		uint16_t cw = 0;
		uint8_t par = 0;

		for (int c = 0; c < 15; c++) {
			cw = (uint16_t)((cw << 1) | m[16 * r + c]);
			par ^= m[16 * r + c];
		}
		par ^= m[16 * r + 15];
		bool syndrome = dmrFecHamming15113Encode(cw >> 4) != cw;

		if (par) {
			// Odd weight: one error, in the first 15 bits (syndrome) or the overall parity bit (none)
			if (syndrome && !dmrFecHamming15113Correct(&cw))
				return false;
		} else if (syndrome) {
			return false;	// two errors
		}
		for (int c = 0; c < 15; c++)
			m[16 * r + c] = (cw >> (14 - c)) & 1;
	}
	// After the row corrections the column parities must hold; one failing column is a lone error in row 7
	int bad_columns = 0;

	for (int c = 0; c < 16; c++) {
		uint8_t p = m[112 + c];

		for (int r = 0; r < 7; r++)
			p ^= m[16 * r + c];
		bad_columns += p;
	}
	if (bad_columns > 1)
		return false;

	uint8_t cs = 0;
	int n = 0;

	memset(lc, 0, DMR_LC_BYTES);
	for (int r = 0; r < 7; r++) {
		for (int c = 0; c < row_data_bits(r); c++, n++)
			bit_put(lc, n, m[16 * r + c]);
		if (r >= 2)
			cs = (uint8_t)((cs << 1) | m[16 * r + 10]);
	}
	return cs == dmr_emb_lc_checksum(lc);
}

uint8_t dmr_emb_lcss(uint8_t voice_index)
{
	static const uint8_t LCSS[6] = { DMR_LCSS_SINGLE, DMR_LCSS_FIRST, DMR_LCSS_CONTINUATION, DMR_LCSS_CONTINUATION,
					 DMR_LCSS_LAST, DMR_LCSS_SINGLE };

	return voice_index < 6 ? LCSS[voice_index] : DMR_LCSS_SINGLE;
}

void dmr_emb_rx_reset(dmr_emb_rx_t *rx)
{
	memset(rx, 0, sizeof(*rx));
}

bool dmr_emb_rx_push(dmr_emb_rx_t *rx, const uint8_t frag[4], uint8_t lcss, uint8_t lc[DMR_LC_BYTES])
{
	switch (lcss) {
	case DMR_LCSS_FIRST:
		memcpy(rx->bits, frag, 4);
		rx->next = 1;
		return false;
	case DMR_LCSS_CONTINUATION:
		if (rx->next == 1 || rx->next == 2) {
			memcpy(&rx->bits[4 * rx->next], frag, 4);
			rx->next++;
		} else {
			rx->next = 0;
		}
		return false;
	case DMR_LCSS_LAST:
		if (rx->next != 3) {
			rx->next = 0;
			return false;
		}
		memcpy(&rx->bits[12], frag, 4);
		rx->next = 0;
		return dmr_emb_lc_decode_bits(rx->bits, lc);
	default:
		return false;	// single fragment (null embedded, reverse channel, privacy): not part of an LC
	}
}

// --- Talker alias ---

#define TA_HEADER_BITS	49
#define TA_BLOCK_BITS	56

void dmr_ta_reset(dmr_ta_t *ta)
{
	memset(ta, 0, sizeof(*ta));
}

static uint32_t stream_bits(const uint8_t *s, int at, int n)
{
	uint32_t v = 0;

	for (int i = 0; i < n; i++)
		v = (v << 1) | (uint32_t)bit_get(s, at + i);
	return v;
}

// One character for the screens: printable ASCII as it is, anything else '?'
static char ta_char(uint32_t cp)
{
	return (cp >= 0x20 && cp < 0x7F) ? (char)cp : '?';
}

// The alias text from the contiguous bits received so far. Formats (TS 102 361-2 7.2.18): 7-bit characters from the
// header's first data bit; ISO 8-bit, UTF-8 and UTF-16 (big endian code units) from its second (the first is unused).
// The length counts characters; some senders count bytes (UTF-8) or overstate it, so the alias also counts as
// complete at the first zero character (the padding) or once all four parts are in
static void ta_render(dmr_ta_t *ta)
{
	int avail = 0;	// contiguous alias bits from the start

	if (ta->have & 1) {
		avail = TA_HEADER_BITS;
		for (int b = 1; b <= 3 && (ta->have & (1 << b)); b++)
			avail += TA_BLOCK_BITS;
	}

	int at = ta->format == DMR_TA_7BIT ? 0 : 1;
	int width = ta->format == DMR_TA_7BIT ? 7 : (ta->format == DMR_TA_UTF16 ? 16 : 8);
	int n = 0, chars = 0;
	bool ended = false;

	while (chars < ta->length && n < DMR_TA_TEXT_MAX && at + width <= avail) {
		uint32_t cp = stream_bits(ta->stream, at, width);
		int units = 1;

		if (cp == 0) {
			ended = true;	// zero padding after the text: some senders count bytes, or overstate the length
			break;
		}
		if (ta->format == DMR_TA_UTF8 && cp >= 0x80) {
			// A multi-byte sequence is one character: the lead byte says how many continuation bytes follow
			units = cp >= 0xF0 ? 4 : cp >= 0xE0 ? 3 : cp >= 0xC0 ? 2 : 1;
			cp = '?';
		} else if (ta->format == DMR_TA_UTF16 && cp >= 0xD800 && cp < 0xDC00) {
			units = 2;	// surrogate pair
			cp = '?';
		}
		if (at + units * width > avail)
			break;	// the rest of the character is in a part not received yet
		at += units * width;
		ta->text[n++] = ta_char(cp);
		chars++;
	}
	ta->text[n] = 0;

	int need = ta->length < DMR_TA_TEXT_MAX ? ta->length : DMR_TA_TEXT_MAX;
	ta->complete = (ta->have & 1) && (n >= need || ended || ta->have == 0x0F);
}

bool dmr_ta_push(dmr_ta_t *ta, const uint8_t lc[DMR_LC_BYTES])
{
	uint8_t flco = lc[0] & 0x3F;
	char before[DMR_TA_TEXT_MAX + 1];
	bool was_complete = ta->complete;

	memcpy(before, ta->text, sizeof(before));
	if (flco == DMR_FLCO_TA_HEADER) {
		uint8_t format = lc[2] >> 6, length = (lc[2] >> 1) & 0x1F;

		if ((ta->have & 1) && (format != ta->format || length != ta->length))
			dmr_ta_reset(ta);	// a different alias: start again
		ta->format = format;
		ta->length = length;
		bit_put(ta->stream, 0, lc[2] & 1);
		for (int i = 0; i < 48; i++)
			bit_put(ta->stream, 1 + i, bit_get(&lc[3], i));
		ta->have |= 1;
	} else if (flco >= DMR_FLCO_TA_BLOCK1 && flco <= DMR_FLCO_TA_BLOCK3) {
		int b = flco - DMR_FLCO_TA_BLOCK1 + 1;
		int at = TA_HEADER_BITS + (b - 1) * TA_BLOCK_BITS;

		for (int i = 0; i < TA_BLOCK_BITS; i++)
			bit_put(ta->stream, at + i, bit_get(&lc[2], i));
		ta->have |= (uint8_t)(1 << b);
	} else {
		return false;
	}
	ta_render(ta);
	return was_complete != ta->complete || memcmp(before, ta->text, sizeof(before)) != 0;
}

int dmr_ta_encode(const char *text, uint8_t format, uint8_t lcs[DMR_TA_LC_MAX][DMR_LC_BYTES])
{
	uint8_t stream[28];
	int width = format == DMR_TA_7BIT ? 7 : 8;
	int at = format == DMR_TA_7BIT ? 0 : 1;
	int max = format == DMR_TA_7BIT ? DMR_TA_TEXT_MAX : DMR_TA_ISO8_MAX;
	int len = 0;

	if (format != DMR_TA_7BIT)
		format = DMR_TA_ISO8;
	memset(stream, 0, sizeof(stream));
	while (text[len] && len < max) {
		uint8_t c = (uint8_t)text[len];

		if (format == DMR_TA_7BIT && c >= 0x80)
			c = '?';
		for (int i = 0; i < width; i++)
			bit_put(stream, at++, (c >> (width - 1 - i)) & 1);
		len++;
	}
	if (len == 0)
		return 0;

	int blocks = at > TA_HEADER_BITS ? (at - TA_HEADER_BITS + TA_BLOCK_BITS - 1) / TA_BLOCK_BITS : 0;

	memset(lcs, 0, (size_t)(blocks + 1) * DMR_LC_BYTES);
	lcs[0][0] = DMR_FLCO_TA_HEADER;
	lcs[0][2] = (uint8_t)((format << 6) | ((len & 0x1F) << 1) | bit_get(stream, 0));
	for (int i = 0; i < 48; i++)
		bit_put(&lcs[0][3], i, bit_get(stream, 1 + i));
	for (int b = 1; b <= blocks; b++) {
		lcs[b][0] = (uint8_t)(DMR_FLCO_TA_BLOCK1 + b - 1);
		for (int i = 0; i < TA_BLOCK_BITS; i++)
			bit_put(&lcs[b][2], i, bit_get(stream, TA_HEADER_BITS + (b - 1) * TA_BLOCK_BITS + i));
	}
	return blocks + 1;
}

// --- GPS Info ---

#define GPS_LON_BITS	25
#define GPS_LAT_BITS	24

// e7 degrees to a field of bits bits covering span_e7 (two's complement, rounded half away from zero, clamped)
static int32_t gps_to_field(int32_t e7, int64_t span_e7, int bits)
{
	int64_t num = (int64_t)e7 * ((int64_t)1 << bits);
	int64_t v = num >= 0 ? (num + span_e7 / 2) / span_e7 : -((-num + span_e7 / 2) / span_e7);
	int64_t max = ((int64_t)1 << (bits - 1)) - 1;

	if (v > max)
		v = max;
	if (v < -max - 1)
		v = -max - 1;
	return (int32_t)v;
}

static int32_t gps_from_field(uint32_t raw, int64_t span_e7, int bits)
{
	int64_t v = (int64_t)(raw & ((1u << bits) - 1u));

	if (v & ((int64_t)1 << (bits - 1)))
		v -= (int64_t)1 << bits;
	int64_t num = v * span_e7;
	int64_t half = (int64_t)1 << (bits - 1);

	return (int32_t)(num >= 0 ? (num + half) >> bits : -((-num + half) >> bits));
}

void dmr_gps_encode(const dmr_gps_t *g, uint8_t lc[DMR_LC_BYTES])
{
	uint32_t lon = (uint32_t)gps_to_field(g->lon, 3600000000LL, GPS_LON_BITS) & 0x1FFFFFFu;
	uint32_t lat = (uint32_t)gps_to_field(g->lat, 1800000000LL, GPS_LAT_BITS) & 0xFFFFFFu;

	lc[0] = DMR_FLCO_GPS;
	lc[1] = 0;
	lc[2] = (uint8_t)(((g->err & 7u) << 1) | (lon >> 24));
	lc[3] = (uint8_t)(lon >> 16);
	lc[4] = (uint8_t)(lon >> 8);
	lc[5] = (uint8_t)lon;
	lc[6] = (uint8_t)(lat >> 16);
	lc[7] = (uint8_t)(lat >> 8);
	lc[8] = (uint8_t)lat;
}

bool dmr_gps_decode(const uint8_t lc[DMR_LC_BYTES], dmr_gps_t *g)
{
	if ((lc[0] & 0x3F) != DMR_FLCO_GPS || lc[1] != 0)
		return false;
	uint32_t lon = ((uint32_t)(lc[2] & 1u) << 24) | ((uint32_t)lc[3] << 16) | ((uint32_t)lc[4] << 8) | lc[5];
	uint32_t lat = ((uint32_t)lc[6] << 16) | ((uint32_t)lc[7] << 8) | lc[8];

	if (lon == 0 && lat == 0)
		return false;
	g->err = (lc[2] >> 1) & 7u;
	g->lon = gps_from_field(lon, 3600000000LL, GPS_LON_BITS);
	g->lat = gps_from_field(lat, 1800000000LL, GPS_LAT_BITS);
	return true;
}

uint8_t dmr_gps_err_code(uint32_t meters)
{
	uint32_t limit = 2;

	if (meters == 0)
		return DMR_GPS_ERR_UNKNOWN;
	for (uint8_t code = 0; code < DMR_GPS_ERR_MORE; code++, limit *= 10)
		if (meters < limit || (code == 5 && meters <= limit))
			return code;
	return DMR_GPS_ERR_MORE;
}

uint32_t dmr_gps_err_m(uint8_t code)
{
	static const uint32_t m[] = { 2, 20, 200, 2000, 20000, 200000 };

	return code < 6 ? m[code] : 0;
}

// cos(x) for |x| <= pi/2 (the latitude), a 10th order Taylor polynomial: error under 1e-6
static float gps_cos(float x)
{
	float x2 = x * x;

	return 1.0f - x2 / 2.0f * (1.0f - x2 / 12.0f * (1.0f - x2 / 30.0f * (1.0f - x2 / 56.0f * (1.0f - x2 / 90.0f))));
}

static float gps_sqrt(float v)
{
	if (v <= 0.0f)
		return 0.0f;
	float r = v > 1.0f ? v / 2.0f : 1.0f;

	for (int i = 0; i < 30; i++) {
		float n = 0.5f * (r + v / r);

		if (n == r)
			break;
		r = n;
	}
	return r;
}

// atan2 in degrees 0-359 for a compass bearing (x east, y north): an octant reduction and a 2nd order fit (error
// under 0.3 degree)
static uint16_t gps_bearing(float x, float y)
{
	float ax = x < 0.0f ? -x : x, ay = y < 0.0f ? -y : y;

	if (ax == 0.0f && ay == 0.0f)
		return 0;
	bool swap = ax > ay;
	float t = swap ? ay / ax : ax / ay;	// 0..1
	float a = t * (45.0f + 15.64f * (1.0f - t));	// atan(t) ~ t (pi / 4 + 0.273 (1 - t)) rad, in degrees
	if (swap)
		a = 90.0f - a;
	if (y < 0.0f)
		a = 180.0f - a;
	if (x < 0.0f)
		a = 360.0f - a;
	int d = (int)(a + 0.5f);

	return (uint16_t)(d >= 360 ? d - 360 : d);
}

void dmr_gps_range(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2, uint32_t *meters, uint16_t *bearing)
{
	const float rad = 3.14159265f / 180.0f / 1e7f;
	int64_t dlon = (int64_t)lon2 - lon1;

	if (dlon > 1800000000LL)
		dlon -= 3600000000LL;
	if (dlon < -1800000000LL)
		dlon += 3600000000LL;
	float mid = (float)(((int64_t)lat1 + lat2) / 2) * rad;
	float x = (float)dlon * rad * gps_cos(mid);	// radians of arc, east
	float y = (float)((int64_t)lat2 - lat1) * rad;	// north
	float d = gps_sqrt(x * x + y * y) * 6371008.8f;

	*meters = d > 4.0e9f ? 4000000000u : (uint32_t)(d + 0.5f);
	*bearing = gps_bearing(x, y);
}
