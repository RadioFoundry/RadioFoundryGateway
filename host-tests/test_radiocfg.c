// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* rdofradio's settings (sdr/cli/rdofradio_cfg.h): the firmware's document read as rdofradio runs it, what it
 * refuses, and a round trip through what the command line's flags make. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "rdof/lastheard.h"
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

    /* what it can't run: SFR, nothing that transmits, not JSON, version 2 */
    CHECK(!parse("{\"version\": 3, \"name\": \"b\", \"id\": 3, \"devices\": [{\"kind\": \"sdr\", \"driver\": "
                 "\"soapy\"}], \"frequencies\": [{\"freq\": 438.25, \"cc\": 1, \"mode\": \"sfr\"}]}") &&
          strstr(err, "SFR"));
    if (!strstr(err, "SFR"))
        printf("  %s\n", err);
    /* a transceiver on a modem: rdofradio --mmdvm runs it, with the modem's settings */
    CHECK(parse("{\"version\": 3, \"name\": \"b\", \"id\": 1, \"defaults\": {\"cc\": 1}, \"devices\": [{\"name\": "
                "\"R\", \"kind\": \"modem\", \"type\": \"mmdvm_hs\", \"port\": {\"path\": \"/dev/ttyS0\"}, "
                "\"mmdvm\": {\"rx_offset_hz\": -150, \"tx_offset_hz\": -175, \"tx_level\": 50, \"rx_level\": 100}, "
                "\"tx\": {\"min_dbm\": 0, \"max_dbm\": 30}}], \"frequencies\": [{\"freq\": 438.0625, \"power_dbm\": -20}]}") &&
          C.on_modem && !C.full && C.rx_hz == 438062500 && C.cfg.nchannels == 1 && C.cfg.channels[0].power == 0 &&
          isnan(C.tx_power_dbm));
    {
        rdof_modem_t m;
        rdof_radio_t r;
        char why[120];
        CHECK(rdofradio_cfg_radio(&C, &r, &m, "MMDVM_HS_Dual_Hat-v1.6.1", why, sizeof why) &&
              r.kind == RDOF_RADIO_MMDVM_HS && r.rx_offset_hz == -150 && r.tx_offset_hz == -175 &&
              r.config.tx_level == 50 && r.config.rx_level == 100 && r.tuner == RDOF_TUNER_MODEM);
    }
    /* a repeater on a modem: on the modem too */
    CHECK(parse("{\"version\": 3, \"name\": \"b\", \"id\": 1, \"defaults\": {\"cc\": 1}, \"devices\": [{\"name\": "
                "\"R\", \"kind\": \"modem\", \"type\": \"mmdvm_hs\", \"port\": {\"path\": \"/dev/ttyS0\"}, "
                "\"hears\": {\"idle\": \"in\"}}], "
                "\"frequencies\": [{\"name\": \"in\", \"freq\": 438.0375, \"devices\": [\"R\"]}, "
                "{\"name\": \"out\", \"freq\": 438.0625, \"devices\": [\"R\"]}], "
                "\"apps\": [{\"name\": \"A\", \"type\": \"repeater\", \"in\": \"in\", \"out\": \"out\"}]}") &&
          C.on_modem && C.full && C.rx_hz == 438037500 && C.tx_hz == 438062500);
    if (err[0])
        printf("  %s\n", err);
    /* an SDR alone: nothing transmits */
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
          !strcmp(err, "M: rdofradio runs one unit that transmits, or split repeaters on the SDR (R here)"));
    if (strcmp(err, "M: rdofradio runs one unit that transmits, or split repeaters on the SDR (R here)"))
        printf("  %s\n", err);
    /* three split repeaters on the SDR, each with a radio at its port path: all three; the radio is the first */
    {
        char f[2048];
        snprintf(f, sizeof f,
                 "{\"version\": 3, \"name\": \"site\", \"id\": 3100000, \"defaults\": {\"cc\": 1}, \"devices\": ["
                 "{\"name\": \"P\", \"kind\": \"sdr\", \"driver\": \"iio\", \"transmits\": false},"
                 "{\"name\": \"T1\", \"kind\": \"modem\", \"port\": {\"path\": \"/dev/ttyACM0\"}, \"hears\": false, \"transmits\": [\"o1\"]},"
                 "{\"name\": \"T2\", \"kind\": \"modem\", \"port\": {\"path\": \"/dev/ttyACM1\", \"baud\": 230400}, \"hears\": false, \"transmits\": [\"o2\"]},"
                 "{\"name\": \"T3\", \"kind\": \"modem\", \"port\": {\"path\": \"/dev/ttyACM2\"}, \"hears\": false, \"transmits\": [\"o3\"]}],"
                 "\"frequencies\": [{\"name\": \"i1\", \"freq\": 438.75}, {\"name\": \"o1\", \"freq\": 438.5},"
                 "{\"name\": \"i2\", \"freq\": 438.175}, {\"name\": \"o2\", \"freq\": 438.55},"
                 "{\"name\": \"i3\", \"freq\": 438.325, \"cc\": 3}, {\"name\": \"o3\", \"freq\": 438.6, \"cc\": 3},"
                 "{\"name\": \"mon\", \"freq\": 438.625}],"
                 "\"apps\": [{\"name\": \"R1\", \"type\": \"repeater\", \"in\": \"i1\", \"out\": \"o1\"},"
                 "{\"name\": \"R2\", \"type\": \"repeater\", \"in\": \"i2\", \"out\": \"o2\", \"hang_s\": 7},"
                 "{\"name\": \"R3\", \"type\": \"repeater\", \"in\": \"i3\", \"out\": \"o3\"}]}");
        const bool ok = parse(f);
        CHECK(ok && C.nsplit == 3 && C.full && !C.on_modem && C.rx_hz == 438750000 && C.tx_hz == 438500000 &&
              !strcmp(C.split[0].port, "/dev/ttyACM0") && !strcmp(C.split[1].port, "/dev/ttyACM1") &&
              C.split[1].baud == 230400 && C.split[1].cfg.hang_s == 7 && C.split[2].rx_hz == 438325000 &&
              C.split[2].tx_hz == 438600000 && C.split[2].cc == 3 && !strcmp(C.split[2].name, "R3"));
        if (!ok)
            printf("  %s\n", err);
        /* the channels: every split's input (the first's too) and 438.625; no split's output */
        bool i1 = false, i2 = false, i3 = false, outs = false, mon = false;
        for (int i = 0; ok && i < C.nchan; i++) {
            const uint32_t hz = (uint32_t)llround(C.chan[i].freq);
            i1 |= hz == 438750000, i2 |= hz == 438175000, i3 |= hz == 438325000, mon |= hz == 438625000;
            outs |= hz == 438550000 || hz == 438600000 || hz == 438500000;
        }
        CHECK(i1 && i2 && i3 && mon && !outs && rdofradio_cfg_channel_at(&C, 438750000) >= 0);
        /* one without a port path: refused, naming it */
        char g[2048];
        snprintf(g, sizeof g, "%s", f);
        char *q = strstr(g, "{\"path\": \"/dev/ttyACM2\"}");
        if (q)
            memcpy(q, "{\"usb\": \"auto\"}         ", 24);
        CHECK(!parse(g) && strstr(err, "R3: its radio needs a port path"));
        if (!strstr(err, "R3: its radio needs a port path"))
            printf("  %s\n", err);
    }
    /* a host: no UARTs of its own */
    CHECK(!parse("{\"version\": 3, \"name\": \"b\", \"id\": 1, \"defaults\": {\"cc\": 1}, \"devices\": [{\"name\": "
                 "\"R\", \"kind\": \"modem\", \"type\": \"mmdvm_hs\", \"port\": {\"uart\": 1, \"tx\": 17, \"rx\": 18}}], "
                 "\"frequencies\": [438.0625]}") &&
          !strcmp(err, "devices.R.port: this host has no UARTs of its own (give the serial device's path)"));
    CHECK(!parse("{not json") && err[0]);
    CHECK(!parse("{\"version\": 2, \"name\": \"b\", \"devices\": {}, \"roles\": {}}") && strstr(err, "version"));

    /* the mqtt network as the board publishes it; no hblink network: HBlink4 off */
    CHECK(parse("{\"version\": 3, \"name\": \"b\", \"id\": 3, \"wifi\": {}, \"defaults\": {\"cc\": 1, \"power_dbm\": -2}, "
                "\"devices\": [{\"kind\": \"sdr\", \"driver\": \"soapy\", \"tx\": {\"power_dbm\": 3}}], "
                "\"frequencies\": [{\"name\": \"in\", "
                "\"freq\": 438.25}, {\"name\": \"out\", \"freq\": 438.0125}], \"apps\": [{\"name\": \"R\", \"type\": "
                "\"repeater\", \"in\": \"in\", \"out\": \"out\"}], \"networks\": [{\"type\": \"mqtt\", \"host\": "
                "\"m\"}]}"));
    if (err[0])
        printf("  %s\n", err);
    CHECK(!C.has_hblink && C.cfg.has_mqtt && !strcmp(C.cfg.mqtt_host, "m") && C.cfg.mqtt_port == 1883 &&
          !strcmp(C.cfg.mqtt_root, "DMR") && !strcmp(C.cfg.mqtt_app, "repeater"));
    /* the SDR's tx.power_dbm for --tx-power; -2 dBm on its map (3 dBm at 255, 0.2 dB a level): 230 */
    CHECK(C.tx_power_dbm == 3 && C.cfg.power == 230);
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

    /* the SDR's channels: the receive-only frequencies, each its own peer with identity per_path, then the
       repeater's input (its unit's: no peer of its own, through the repeater's); its output isn't one */
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
    CHECK(C.nchan == 3 && C.chan[0].freq == 438.075e6 && C.chan[0].cc == 1 && C.chan[1].freq == 438.15e6 &&
          C.chan[1].cc == 3 && C.chan[2].freq == 438.25e6 && !C.chan[2].hblink && C.chan[2].peer_id < 0);
    CHECK(rdofradio_cfg_channel_at(&C, 438250000) == 2 && rdofradio_cfg_channel_at(&C, 438012500) < 0);
    CHECK(C.chan[0].hblink && C.chan[1].hblink && C.chan[0].peer_id == 9800101 && C.chan[1].peer_id == 9800102);
    CHECK(!strcmp(C.sdr_app, "sdr") && !strcmp(C.sdr_name, "pluto-rx") && C.sdr_status_s == 30);
    if (strcmp(C.sdr_name, "pluto-rx"))
        printf("  SDR name %s app %s\n", C.sdr_name, C.sdr_app);
    rdofradio_cfg_describe(&C, "file x.json", "", text, sizeof text);
    CHECK(strstr(text, "channel m1: 438.075 MHz, CC 1, peer 9800101\n") &&
          strstr(text, "channel m2: 438.15 MHz, CC 3, peer 9800102\n") &&
          strstr(text, "channel R1-1: 438.25 MHz, CC 1\n"));
    if (!strstr(text, "channel m2"))
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
    /* every frequency a channel, the transceiver's idle one among them */
    CHECK(C.has_hblink && C.has_parrot && C.parrot == 9990 && C.nchan == 2 && C.chan[0].freq == 438062500 &&
          C.chan[1].freq == 438087500);
    CHECK(!C.chan[0].hblink && !C.chan[1].hblink); /* one identity: through the radio's peer */
    /* last heard: the transceiver's channel is an SDR channel too, so a call it hears is the SDR's entry only (the
       Pluto showed one simplex call as two, sdr/SDR01 and roamer); --mmdvm's modem (no channel) is the unit's */
    {
        const int uc = rdofradio_cfg_channel_at(&C, C.rx_hz);
        CHECK(uc >= 0 && !rdofradio_cfg_heard_unit(true, true) && rdofradio_cfg_heard_unit(false, true) &&
              !rdofradio_cfg_heard_unit(true, false));
        static rdof_lh_entry_t ring[8];
        static rdof_lh_t lh;
        rdof_lh_init(&lh, ring, 8);
        const char *js[] = {"{\"DMR\":{\"action\":\"start\",\"slot\":1,\"src_id\":9800001,\"dst_id\":9990,"
                            "\"group\":\"yes\",\"frequency\":\"438.0625\"}}",
                            "{\"DMR\":{\"action\":\"end\",\"slot\":1,\"src_id\":9800001,\"dst_id\":9990,"
                            "\"group\":\"yes\",\"duration\":7.38}}"};
        for (int k = 0; k < 2; k++) { /* as rdofradio's on_json: is_sdr and is_unit both hold for the unit's channel */
            rdof_lh_tracker_json(&lh, "sdr", "plant", "SDR01", js[k], k, 0);
            if (rdofradio_cfg_heard_unit(true, uc >= 0))
                rdof_lh_tracker_json(&lh, "roamer", "plant", NULL, js[k], k, 0);
        }
        CHECK(lh.n == 1 && !strcmp(ring[0].role, "sdr") && !ring[0].active && ring[0].src == 9800001);
    }
    CHECK(parse("{\"version\": 3, \"name\": \"tower\", \"id\": 9800300, \"defaults\": {\"cc\": 1}, "
                "\"devices\": [{\"kind\": \"sdr\", \"driver\": \"iio\"}], "
                "\"frequencies\": [{\"name\": \"in\", \"freq\": 438.75}, {\"name\": \"out\", \"freq\": 438.5}, "
                "{\"freq\": 438.9375, \"tx\": false, \"tg\": \"*\"}], "
                "\"apps\": [{\"name\": \"R1\", \"type\": \"repeater\", \"in\": \"in\", \"out\": \"out\"}], "
                "\"networks\": [{\"type\": \"hblink\", \"host\": \"h\"}]}"));
    if (err[0])
        printf("  %s\n", err);
    CHECK(C.full && C.rx_hz == 438750000 && C.tx_hz == 438500000 && C.nchan == 2 && C.chan[1].freq == 438750000 &&
          !C.has_parrot);

    printf("radiocfg: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
