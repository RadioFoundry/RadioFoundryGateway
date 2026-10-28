// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* rdofradio's settings (sdr/cli/rdofradio_cfg.h): the firmware's document read as rdofradio runs it, what it
 * refuses, the keys it lists as not used, and a round trip through what the command line's flags make. */
#include <stdio.h>
#include <string.h>

#include "rdofradio_cfg.h"

static int fails, checks;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                        \
        }                                                                                                              \
    } while (0)

static rdofradio_cfg_t C;
static char err[200];

static bool parse(const char *t)
{
    err[0] = 0;
    return rdofradio_cfg_parse(t, strlen(t), &C, err, sizeof err);
}

int main(void)
{
    /* the example repeater (docs/reference.md, rdofradio --config) */
    const char *example = "{\n  // a repeater on the PlutoSDR\n  \"name\": \"example\",\n  \"radios\": [\n"
                         "    {\"type\": \"repeater\", \"name\": \"example\", \"peer_id\": 9990601,\n"
                         "     \"rx_freq\": 438.25, \"tx_freq\": 438.0125, \"cc\": 1, \"parrot\": 9990}\n  ]\n}\n";
    CHECK(parse(example));
    if (err[0])
        printf("  %s\n", err);
    CHECK(C.full && C.rx_hz == 438250000 && C.tx_hz == 438012500 && C.cc == 1 && C.peer_id == 9990601);
    CHECK(C.has_parrot && C.parrot == 9990 && !strcmp(C.name, "example") && !strcmp(C.box, "example"));
    CHECK(C.unused[0] == 0);
    CHECK(C.cfg.slot1 && C.cfg.slot2);

    /* the repeater's own settings, applied as the firmware does */
    static rdof_repeater_t r;
    rdof_repeater_init(&r, NULL, C.peer_id, C.rx_hz, C.tx_hz, C.cc);
    CHECK(parse("{\"name\": \"b\", \"radios\": [{\"type\": \"repeater\", \"name\": \"R\", \"peer_id\": 3, "
                "\"rx_freq\": 438.25, \"tx_freq\": 438.0125, \"cc\": 2, \"slots\": [2], \"hang_s\": 7, "
                "\"call_hang_s\": 2, \"cwid\": \"N0CALL\"}]}"));
    rdofradio_cfg_apply_repeater(&C, &r);
    CHECK(!r.enabled[1] && r.enabled[2] && r.inactivity_s == 7 && r.call_hang_s == 2 && !strcmp(r.cwid, "N0CALL"));
    CHECK(!C.has_parrot && C.cc == 2);

    /* what it can't run */
    CHECK(!parse("{\"name\": \"b\", \"radios\": [{\"type\": \"repeater\", \"name\": \"R\", \"peer_id\": 3, "
                 "\"cc\": 1, \"mode\": \"sfr\", \"freq\": 438.25, \"radio\": {\"type\": \"radio_hotspot\"}}]}") &&
          strstr(err, "sfr"));
    if (!strstr(err, "sfr"))
        printf("  %s\n", err);
    CHECK(!parse("{\"name\": \"b\", \"cc\": 1, \"channels\": {\"1\": 438.0625}, \"radios\": [{\"type\": \"opengd77\", "
                 "\"name\": \"R\", \"peer_id\": 1, \"idle\": 1}]}") &&
          strstr(err, "roamer"));
    CHECK(!parse("{\"name\": \"b\", \"radios\": [{\"type\": \"sdr\", \"name\": \"S\"}]}") && err[0]);
    CHECK(!parse("{not json") && err[0]);
    CHECK(!parse("{\"name\": \"b\", \"radios\": [{\"type\": \"repeater\", \"name\": \"R\", \"rx_freq\": 438.25}]}") &&
          err[0]);

    /* keys the firmware takes but rdofradio doesn't use: listed, not refused */
    CHECK(parse("{\"name\": \"b\", \"mqtt\": {\"host\": \"m\"}, \"wifi\": {}, \"radios\": [{\"type\": \"repeater\", "
                "\"name\": \"R\", \"peer_id\": 3, \"rx_freq\": 438.25, \"tx_freq\": 438.0125, \"cc\": 1, \"power\": 20, "
                "\"hblink\": {\"master\": \"h\", \"port\": 62031, \"passphrase\": \"p\"}}]}"));
    if (err[0])
        printf("  %s\n", err);
    CHECK(strstr(C.unused, "mqtt") && strstr(C.unused, "wifi") && strstr(C.unused, "R.power") &&
          strstr(C.unused, "R.hblink") && !strstr(C.unused, "rx_freq"));
    char text[1024];
    rdofradio_cfg_describe(&C, "file x.json", "--soapy driver=plutosdr", text, sizeof text);
    CHECK(strstr(text, "b runs: R") && strstr(text, "RX 438.25 MHz, TX 438.0125 MHz") &&
          strstr(text, "not used on this host: mqtt, wifi, R.power, R.hblink") && strstr(text, "driver=plutosdr"));
    char tiny[16];
    CHECK(rdofradio_cfg_describe(&C, "x", "", tiny, sizeof tiny) == 15);

    /* the command line's flags as a document, and back: the same radio */
    size_t n = rdofradio_cfg_from_flags(text, sizeof text, true, 438250000, 438012500, 1, true, 9990, 9990601);
    CHECK(n > 0 && n == strlen(text));
    CHECK(parse(text) && C.full && C.rx_hz == 438250000 && C.tx_hz == 438012500 && C.cc == 1 && C.has_parrot &&
          C.parrot == 9990 && C.peer_id == 9990601 && !C.unused[0]);
    if (err[0])
        printf("  %s\n%s", err, text);
    n = rdofradio_cfg_from_flags(text, sizeof text, false, 438000000, 438000000, 3, false, 0, 9990601);
    CHECK(parse(text) && !C.full && C.rx_hz == 438000000 && C.tx_hz == 438000000 && C.cc == 3 && !C.has_parrot);
    if (err[0])
        printf("  %s\n%s", err, text);
    CHECK(strstr(C.unused, "radios sdr"));

    /* version 2: a repeater on one SDR; version, devices and roles are its own keys, not unused ones */
    const char *v2 = "{\"version\": 2, \"name\": \"pluto\", \"devices\": {\"pluto\": {\"kind\": \"sdr\", "
                     "\"driver\": \"iio\", \"tx\": {}}}, \"roles\": {\"R1\": {\"type\": \"repeater\", "
                     "\"peer_id\": 9800001, \"rx_freq\": 438.25, \"tx_freq\": 438.0125, \"cc\": 1, \"parrot\": 9990, "
                     "\"modem\": {\"device\": \"pluto\"}, \"cwid\": \"N0CALL\"}}, \"mqtt\": {\"host\": \"x\"}}";
    CHECK(parse(v2) && C.full && C.rx_hz == 438250000 && C.tx_hz == 438012500 && C.cc == 1 && C.parrot == 9990);
    CHECK(!strstr(C.unused, "version") && !strstr(C.unused, "devices") && !strstr(C.unused, "roles") &&
          strstr(C.unused, "mqtt"));
    if (err[0])
        printf("  %s / unused: %s\n", err, C.unused);

    printf("radiocfg: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
