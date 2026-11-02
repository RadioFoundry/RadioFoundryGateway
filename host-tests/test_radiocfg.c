// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* rdofradio's settings (sdr/cli/rdofradio_cfg.h): the firmware's document read as rdofradio runs it, what it
 * refuses, and a round trip through what the command line's flags make. */
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
    const char *example = "{\n  // a repeater on the PlutoSDR\n  \"version\": 3,\n  \"name\": \"example\",\n"
                          "  \"id\": 9990601,\n  \"defaults\": {\"cc\": 1},\n"
                          "  \"devices\": [{\"name\": \"pluto\", \"kind\": \"sdr\", \"driver\": \"iio\"}],\n"
                          "  \"frequencies\": [{\"name\": \"in\", \"freq\": 438.25}, {\"name\": \"out\", \"freq\": 438.0125}],\n"
                          "  \"apps\": [{\"name\": \"example\", \"type\": \"repeater\", \"in\": \"in\", \"out\": \"out\"},\n"
                          "           {\"type\": \"parrot\", \"tg\": 9990}]\n}\n";
    CHECK(parse(example));
    if (err[0])
        printf("  %s\n", err);
    CHECK(C.full && C.rx_hz == 438250000 && C.tx_hz == 438012500 && C.cc == 1 && C.peer_id == 9990601);
    CHECK(C.has_parrot && C.parrot == 9990 && !strcmp(C.name, "example") && !strcmp(C.box, "example"));
    CHECK(C.cfg.slot1 && C.cfg.slot2);

    /* the repeater's own settings, applied as the firmware does */
    static rdof_repeater_t r;
    rdof_repeater_init(&r, NULL, C.peer_id, C.rx_hz, C.tx_hz, C.cc);
    const char *rep = "{\"version\": 3, \"name\": \"b\", \"id\": 3, \"devices\": [{\"kind\": \"sdr\", \"driver\": "
                      "\"soapy\"}], \"frequencies\": [{\"name\": \"in\", \"freq\": 438.25, \"cc\": 2}, {\"name\": "
                      "\"out\", \"freq\": 438.0125, \"cc\": 2}], \"apps\": [{\"name\": \"R\", \"type\": \"repeater\", "
                      "\"in\": \"in\", \"out\": \"out\", \"active_slots\": [2], \"hang_s\": 7, \"call_hang_s\": 2, "
                      "\"cwid\": \"N0CALL\"}]}";
    CHECK(parse(rep));
    if (err[0])
        printf("  %s\n", err);
    rdofradio_cfg_apply_repeater(&C, &r);
    CHECK(!r.enabled[1] && r.enabled[2] && r.inactivity_s == 7 && r.call_hang_s == 2 && !strcmp(r.cwid, "N0CALL"));
    CHECK(!C.has_parrot && C.cc == 2);

    /* what it can't run: SFR, a transceiver on a modem, nothing that transmits, not JSON, version 2 */
    CHECK(!parse("{\"version\": 3, \"name\": \"b\", \"id\": 3, \"devices\": [{\"kind\": \"sdr\", \"driver\": "
                 "\"soapy\"}], \"frequencies\": [{\"freq\": 438.25, \"cc\": 1, \"mode\": \"sfr\"}]}") &&
          strstr(err, "SFR"));
    if (!strstr(err, "SFR"))
        printf("  %s\n", err);
    CHECK(!parse("{\"version\": 3, \"name\": \"b\", \"id\": 1, \"defaults\": {\"cc\": 1}, \"devices\": [{\"name\": "
                 "\"R\", \"kind\": \"modem\", \"type\": \"radio_hotspot\", \"port\": {\"usb\": \"auto\"}}], "
                 "\"frequencies\": [438.0625]}") &&
          strstr(err, "a transceiver on a modem"));
    CHECK(!parse("{\"version\": 3, \"name\": \"b\", \"id\": 1, \"defaults\": {\"cc\": 1}, \"devices\": [{\"kind\": "
                 "\"sdr\"}], \"frequencies\": [438.0625]}") &&
          strstr(err, "nothing transmits"));
    /* one unit that transmits: a second isn't run, so it's refused */
    CHECK(!parse("{\"version\": 3, \"name\": \"b\", \"id\": 1, \"defaults\": {\"cc\": 1}, \"devices\": [{\"name\": "
                 "\"P\", \"kind\": \"sdr\", \"driver\": \"iio\"}, {\"name\": \"M\", \"kind\": \"modem\", \"type\": "
                 "\"mmdvm_hs\", \"port\": {\"path\": \"/dev/ttyACM0\"}}], \"frequencies\": [{\"name\": \"in\", "
                 "\"freq\": 438.25}, {\"name\": \"out\", \"freq\": 438.0125}, {\"name\": \"A\", \"freq\": 438.1, "
                 "\"devices\": [\"M\"]}], \"apps\": [{\"name\": \"R\", \"type\": \"repeater\", \"in\": \"in\", "
                 "\"out\": \"out\"}]}") &&
          !strcmp(err, "M: rdofradio runs one unit that transmits (R here)"));
    if (strcmp(err, "M: rdofradio runs one unit that transmits (R here)"))
        printf("  %s\n", err);
    /* a host: no UARTs of its own */
    CHECK(!parse("{\"version\": 3, \"name\": \"b\", \"id\": 1, \"defaults\": {\"cc\": 1}, \"devices\": [{\"name\": "
                 "\"R\", \"kind\": \"modem\", \"type\": \"mmdvm_hs\", \"port\": {\"uart\": 1, \"tx\": 17, \"rx\": 18}}], "
                 "\"frequencies\": [438.0625]}") &&
          !strcmp(err, "devices.R.port: this host has no UARTs of its own (give the serial device's path)"));
    CHECK(!parse("{not json") && err[0]);
    CHECK(!parse("{\"version\": 2, \"name\": \"b\", \"devices\": {}, \"roles\": {}}") && strstr(err, "version"));

    /* the mqtt network as the board publishes it; no hblink network: HBlink4 off */
    CHECK(parse("{\"version\": 3, \"name\": \"b\", \"id\": 3, \"wifi\": {}, \"defaults\": {\"cc\": 1, \"power\": 20}, "
                "\"devices\": [{\"kind\": \"sdr\", \"driver\": \"soapy\"}], \"frequencies\": [{\"name\": \"in\", "
                "\"freq\": 438.25}, {\"name\": \"out\", \"freq\": 438.0125}], \"apps\": [{\"name\": \"R\", \"type\": "
                "\"repeater\", \"in\": \"in\", \"out\": \"out\"}], \"networks\": [{\"type\": \"mqtt\", \"host\": "
                "\"m\"}]}"));
    if (err[0])
        printf("  %s\n", err);
    CHECK(!C.has_hblink && C.cfg.has_mqtt && !strcmp(C.cfg.mqtt_host, "m") && C.cfg.mqtt_port == 1883 &&
          !strcmp(C.cfg.mqtt_root, "DMR") && !strcmp(C.cfg.mqtt_app, "repeater"));
    char text[1024];
    rdofradio_cfg_describe(&C, "file x.json", "--soapy driver=plutosdr", text, sizeof text);
    CHECK(strstr(text, "b runs: R") && strstr(text, "RX 438.25 MHz, TX 438.0125 MHz") &&
          !strstr(text, "not used") && strstr(text, "HBlink4: off") && strstr(text, "MQTT: m:1883, root DMR\n") &&
          strstr(text, "driver=plutosdr"));
    if (!strstr(text, "HBlink4: off"))
        printf("%s\n", text);
    /* an hblink network: the master */
    const char *R1 = "\"devices\": [{\"kind\": \"sdr\", \"driver\": \"soapy\"}], \"frequencies\": [{\"name\": \"in\", "
                     "\"freq\": 438.25, \"cc\": 1}, {\"name\": \"out\", \"freq\": 438.0125, \"cc\": 1}], \"apps\": "
                     "[{\"name\": \"R\", \"type\": \"repeater\", \"in\": \"in\", \"out\": \"out\"}]";
    char doc[1024];
    snprintf(doc, sizeof doc, "{\"version\": 3, \"name\": \"b\", \"id\": 3, \"callsign\": \"N0CALL\", %s, "
             "\"networks\": [{\"type\": \"hblink\", \"host\": \"master.example\", \"port\": 62032}]}", R1);
    CHECK(parse(doc) && C.has_hblink && !strcmp(C.cfg.master_host, "master.example") && C.cfg.master_port == 62032);
    if (err[0])
        printf("  %s\n", err);
    rdofradio_cfg_describe(&C, "file x.json", "", text, sizeof text);
    CHECK(strstr(text, "HBlink4: master.example:62032, peer 3\n") != NULL && strstr(text, "clock: the OS's"));
    if (!strstr(text, "HBlink4: master.example"))
        printf("%s\n", text);
    /* the clock: netclock from servers, unless clock is os */
    snprintf(doc, sizeof doc, "{\"version\": 3, \"name\": \"b\", \"id\": 3, \"ntp\": \"pool.ntp.org\", %s}", R1);
    CHECK(parse(doc) && C.own_clock && !strcmp(C.ntp_servers, "pool.ntp.org"));
    snprintf(doc, sizeof doc, "{\"version\": 3, \"name\": \"b\", \"id\": 3, \"ntp\": {\"servers\": \"a b\", "
             "\"poll_s\": 64, \"clock\": \"os\"}, %s}", R1);
    CHECK(parse(doc) && !C.own_clock && C.ntp_poll_s == 64);
    snprintf(doc, sizeof doc, "{\"version\": 3, \"name\": \"b\", \"id\": 3, \"ntp\": {\"clock\": \"netclock\"}, %s}", R1);
    CHECK(parse(doc) && C.own_clock && !C.ntp_servers[0]);
    snprintf(doc, sizeof doc, "{\"version\": 3, \"name\": \"b\", \"id\": 3, \"ntp\": {\"clock\": \"chrony\"}, %s}", R1);
    CHECK(!parse(doc) && strstr(err, "ntp.clock"));
    char tiny[16];
    CHECK(rdofradio_cfg_describe(&C, "x", "", tiny, sizeof tiny) == 15);

    /* the command line's flags as a document, and back: the same radio */
    size_t n = rdofradio_cfg_from_flags(text, sizeof text, "soapy", true, 438250000, 438012500, 1, true, 9990, 9990601);
    CHECK(n > 0 && n == strlen(text));
    CHECK(parse(text) && C.full && C.rx_hz == 438250000 && C.tx_hz == 438012500 && C.cc == 1 && C.has_parrot &&
          C.parrot == 9990 && C.peer_id == 9990601);
    if (err[0])
        printf("  %s\n%s", err, text);
    n = rdofradio_cfg_from_flags(text, sizeof text, "soapy", false, 438000000, 438000000, 3, false, 0, 9990601);
    CHECK(parse(text) && !C.full && C.rx_hz == 438000000 && C.tx_hz == 438000000 && C.cc == 3 && !C.has_parrot);
    if (err[0])
        printf("  %s\n%s", err, text);
    n = rdofradio_cfg_from_flags(text, sizeof text, "soapy", false, 438000000, 438000000, 3, true, 0, 9990601);
    CHECK(parse(text) && !C.full && !C.has_parrot); /* a parrot app answers one ID */
    if (err[0])
        printf("  %s\n%s", err, text);
    /* --iio: the iio driver */
    n = rdofradio_cfg_from_flags(text, sizeof text, "iio", true, 438250000, 438012500, 1, false, 0, 9990601);
    CHECK(strstr(text, "\"driver\": \"iio\"") && !strstr(text, "soapy") && parse(text) && C.full);
    if (err[0])
        printf("  %s\n%s", err, text);

    /* the SDR's other frequencies: receive-only channels, each its own peer with identity per_path; the
       repeater's own isn't one */
    const char *mon = "{\"version\": 3, \"name\": \"pluto\", \"id\": 9800001, \"defaults\": {\"cc\": 1}, "
                      "\"devices\": [{\"name\": \"pluto\", \"kind\": \"sdr\", \"driver\": \"iio\"}], "
                      "\"frequencies\": [{\"name\": \"m1\", \"freq\": 438.075, \"tx\": false}, {\"name\": \"m2\", "
                      "\"freq\": 438.15, \"cc\": 3, \"tx\": false}, "
                      "{\"name\": \"in\", \"freq\": 438.25}, {\"name\": \"out\", \"freq\": 438.0125}], "
                      "\"apps\": [{\"name\": \"R1\", \"type\": \"repeater\", \"in\": \"in\", \"out\": \"out\"}], "
                      "\"networks\": [{\"type\": \"hblink\", \"host\": \"m\", \"identity\": \"per_path\", "
                      "\"peer_ids\": {\"m1\": 9800101, \"m2\": 9800102}}]}";
    CHECK(parse(mon) && C.full && C.rx_hz == 438250000);
    if (err[0])
        printf("  %s\n", err);
    CHECK(C.nmon == 2 && C.mon[0].freq == 438.075e6 && C.mon[0].cc == 1 && C.mon[1].freq == 438.15e6 &&
          C.mon[1].cc == 3);
    CHECK(C.mon[0].hblink && C.mon[1].hblink && C.mon[0].peer_id == 9800101 && C.mon[1].peer_id == 9800102);
    CHECK(!strcmp(C.mon_app, "sdr") && !strcmp(C.mon_name, "pluto-rx") && C.mon_status_s == 30);
    if (strcmp(C.mon_name, "pluto-rx"))
        printf("  SDR name %s app %s\n", C.mon_name, C.mon_app);
    rdofradio_cfg_describe(&C, "file x.json", "", text, sizeof text);
    CHECK(strstr(text, "monitor m1: 438.075 MHz, CC 1, peer 9800101\n") &&
          strstr(text, "monitor m2: 438.15 MHz, CC 3, peer 9800102\n"));
    if (!strstr(text, "monitor m2"))
        printf("%s\n", text);

    /* the commercial site (a transceiver on the SDR, every frequency monitored) and a repeater with forwarders */
    CHECK(parse("{\"version\": 3, \"name\": \"plant\", \"id\": 9800100, \"defaults\": {\"cc\": 2}, "
                "\"devices\": [{\"kind\": \"sdr\", \"driver\": \"iio\"}], "
                "\"frequencies\": [{\"freq\": 438.0625, \"tg\": 201}, {\"freq\": 438.0875, \"tg\": 202}], "
                "\"apps\": [{\"type\": \"parrot\", \"tg\": 9990}], "
                "\"networks\": [{\"type\": \"hblink\", \"host\": \"h\"}]}"));
    if (err[0])
        printf("  %s\n", err);
    CHECK(C.site.npath == 2 && !C.full && C.rx_hz == 438062500 && C.peer_id == 9800100);
    CHECK(C.has_hblink && C.has_parrot && C.parrot == 9990 && C.nmon == 1 && C.mon[0].freq == 438087500);
    CHECK(!C.mon[0].hblink); /* one identity: through the radio's peer */
    CHECK(parse("{\"version\": 3, \"name\": \"tower\", \"id\": 9800300, \"defaults\": {\"cc\": 1}, "
                "\"devices\": [{\"kind\": \"sdr\", \"driver\": \"iio\"}], "
                "\"frequencies\": [{\"name\": \"in\", \"freq\": 438.75}, {\"name\": \"out\", \"freq\": 438.5}, "
                "{\"freq\": 438.9375, \"tx\": false, \"tg\": \"*\"}], "
                "\"apps\": [{\"name\": \"R1\", \"type\": \"repeater\", \"in\": \"in\", \"out\": \"out\"}], "
                "\"networks\": [{\"type\": \"hblink\", \"host\": \"h\"}]}"));
    if (err[0])
        printf("  %s\n", err);
    CHECK(C.full && C.rx_hz == 438750000 && C.tx_hz == 438500000 && C.nmon == 1 && !C.has_parrot);

    printf("radiocfg: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
