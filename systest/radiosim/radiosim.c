// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * One RadioFoundry radio as a process, for the system tests (docs/spec/system-tests.md).
 *
 * RadioFoundry's own MMDVM and hotspot core (core/hotspot) runs on its simulated
 * DMR modem (tests/host/sim/dmr_sim_modem.c), built from RADIOFOUNDRY_DIR. The
 * test drives it over stdin / stdout in virtual time: nothing happens between
 * requests. A process, because the core is a singleton and because the gateway
 * carries its own copies of some RadioFoundry files, which would collide.
 *
 * Requests, one per line; each is answered by its output lines, then a line ".":
 *   t <ms>                       advance the clock to ms (absolute, from 1000): a poll every 5 ms, a slot every 30 ms
 *   h <hex>                      bytes from the host on the serial port (any split; framed here)
 *   r <ts> <kind> <index> <sync> <cc> <payload hex> [<lc hex>]
 *                                a burst heard (dmr_burst_t: kind, voice_index, sync family; lc for headers,
 *                                terminators and voice F)
 *   k <knob> <0|1>               a modem knob: refuse_tx, repeater, stalled
 *   q                            quit
 * Output lines:
 *   o <ms> <hex>                 a frame the radio wrote to the host
 *   a <ms> <slot> <kind> <index> <sync> <lcss> <emb hex> <payload hex>
 *                                a burst the radio sent on air (slot: the modem's slot counter)
 *   e <ms> <what> ...            the radio core was asked: enter <rx_hz>, freq <rx_hz> <tx_hz>, power <level>, leave
 *
 * Arguments: [description], the GET_VERSION text (default "RadioFoundry RadioSim 00000000").
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hotspot/hotspot.h"
#include "hotspot/mmdvm.h"
#include "dmr_sim_modem.h"

static uint32_t now = 1000;	/* the virtual clock, ms */
static uint32_t next_slot;	/* when the modem's next 30 ms slot ends */
static mmdvm_framer_t framer;

static void put_hex(const uint8_t *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		printf("%02X", b[i]);
	}
	if (!n) {
		printf("-");
	}
}

static void io_write(const uint8_t *buf, size_t len)
{
	printf("o %u ", (unsigned)now);
	put_hex(buf, len);
	printf("\n");
}

static mmdvm_io_t io = { .write = io_write, .description = "RadioFoundry RadioSim 00000000" };

static bool r_enter(uint32_t rx)
{
	printf("e %u enter %u\n", (unsigned)now, (unsigned)rx);
	return true;
}

static bool r_set_freq(uint32_t rx, uint32_t tx)
{
	printf("e %u freq %u %u\n", (unsigned)now, (unsigned)rx, (unsigned)tx);
	return true;
}

static void r_set_power(uint8_t level)
{
	printf("e %u power %u\n", (unsigned)now, level);
}

static void r_leave(void)
{
	printf("e %u leave\n", (unsigned)now);
}

static const hotspot_radio_ops_t radio_ops = { .enter = r_enter, .set_freq = r_set_freq, .set_power = r_set_power,
	.leave = r_leave };

static void flush_air(void)
{
	for (unsigned i = 0; i < sim.air_count && i < SIM_AIR_MAX; i++) {
		const sim_air_t *a = &sim.air[i];

		printf("a %u %u %u %u %u %u ", (unsigned)now, (unsigned)a->slot, a->kind, a->voice_index, a->sync, a->lcss);
		put_hex(a->emb, sizeof(a->emb));
		printf(" ");
		put_hex(a->payload, a->len);
		printf("\n");
	}
	sim.air_count = 0;
}

static void advance(uint32_t to)
{
	while ((int32_t)(to - now) >= 5) {
		now += 5;
		if ((int32_t)(now - next_slot) >= 0) {
			next_slot += 30;
			sim_slot();
		}
		mmdvm_poll(now);
		flush_air();
	}
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

static size_t unhex(const char *s, uint8_t *out, size_t cap)
{
	size_t n = 0;

	while (n < cap && hexval(s[0]) >= 0 && hexval(s[1]) >= 0) {
		out[n++] = (uint8_t)((hexval(s[0]) << 4) | hexval(s[1]));
		s += 2;
	}
	return n;
}

static void host_bytes(const char *hex)
{
	uint8_t b[512];
	size_t n = unhex(hex, b, sizeof(b));

	for (size_t i = 0; i < n; i++) {
		uint16_t len = mmdvm_framer_byte(&framer, b[i]);

		if (len) {
			mmdvm_frame(framer.buf, len, now);
		}
	}
	flush_air();
}

static void heard(const char *args)
{
	int ts, cc;
	unsigned kind, index, sync;
	char ph[2 * DMR_PAYLOAD_MAX + 2] = { 0 }, lh[32] = { 0 };
	dmr_burst_t b;
	int got = sscanf(args, "%d %u %u %u %d %56s %31s", &ts, &kind, &index, &sync, &cc, ph, lh);

	if (got < 6) {
		return;
	}
	memset(&b, 0, sizeof(b));
	b.ts = (int8_t)ts;
	b.kind = (uint8_t)kind;
	b.voice_index = (uint8_t)index;
	b.sync = (uint8_t)sync;
	b.cc = (uint8_t)cc;
	b.crc_ok = true;
	b.in_step = true;
	b.rssi_dbm = -80;
	b.len = (uint8_t)unhex(ph, b.payload, DMR_PAYLOAD_MAX);
	if (got == 7) {
		b.lc_valid = unhex(lh, b.lc, 9) == 9;
	}
	sim_rx(&b);
	flush_air();
}

static void knob(const char *args)
{
	char name[32];
	int on;

	if (sscanf(args, "%31s %d", name, &on) != 2) {
		return;
	}
	if (!strcmp(name, "refuse_tx")) {
		sim.refuse_tx = on;
	} else if (!strcmp(name, "repeater")) {
		sim.repeater = on;
	} else if (!strcmp(name, "stalled")) {
		sim.stalled = on;
	}
}

int main(int argc, char **argv)
{
	char line[2048];

	if (argc > 1) {
		io.description = argv[1];
	}
	setvbuf(stdout, NULL, _IOFBF, 1 << 16);
	sim_reset();
	next_slot = now + 30;
	mmdvm_framer_reset(&framer);
	hotspot_init(&sim_modem, mmdvm_send);
	hotspot_set_radio(&radio_ops);
	mmdvm_init(&io);
	while (fgets(line, sizeof(line), stdin) != NULL) {
		if (line[0] == 'q') {
			break;
		}
		switch (line[0]) {
		case 't':
			advance((uint32_t)strtoul(line + 2, NULL, 10));
			break;
		case 'h':
			host_bytes(line + 2);
			break;
		case 'r':
			heard(line + 2);
			break;
		case 'k':
			knob(line + 2);
			break;
		default:
			break;
		}
		printf(".\n");
		fflush(stdout);
	}
	return 0;
}
