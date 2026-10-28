// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The schema (config/rdof-config.schema.json) and the C parser agree (docs/spec/fw/rxtx-combinations.md 4.1):
 *  - every version 2 fixture: good ones pass the schema, bad ones fail it, except "bad-semantic-*" (only the C sees
 *    those: names that don't exist, ownership, caps), which pass it; test_setup.c runs the same files through the C;
 *  - every key list in common/src/setup.c is the schema's list for that place, so a key added to one and not the
 *    other fails here.
 * node host-tests/test_schema.js; exit 1 on a failure.
 */
"use strict";
const fs = require("fs");
const path = require("path");
const {check} = require("../config/schema-check.js");

const top = path.join(__dirname, "..");
const schema = JSON.parse(fs.readFileSync(path.join(top, "config/rdof-config.schema.json"), "utf8"));
let failed = 0, checks = 0;

function expect(ok, what) {
    checks++;
    if (!ok) {
        failed++;
        console.log("FAIL " + what);
    }
}

// The fixtures.
const dir = path.join(__dirname, "fixtures/config/v2");
for (const f of fs.readdirSync(dir).filter((f) => f.endsWith(".json")).sort()) {
    const errs = check(schema, JSON.parse(fs.readFileSync(path.join(dir, f), "utf8")));
    const msg = errs.map((e) => e.path + ": " + e.message).join("; ");
    if (f.startsWith("good-") || f.startsWith("bad-semantic-"))
        expect(!errs.length, f + " should pass the schema: " + msg);
    else expect(errs.length > 0, f + " should fail the schema");
}

// The key lists: setup.c's NAME_KEYS against the schema's place for it.
const src = fs.readFileSync(path.join(top, "common/src/setup.c"), "utf8");
const talker = src.match(/#define TALKER_KEYS\s*\\?\s*([^]*?)\n\S/)[1].match(/"[^"]+"/g).map((s) => JSON.parse(s));
function cList(name) {
    const m = src.match(new RegExp("static const char \\*const " + name + "\\[\\] = \\{([^}]*)\\}"));
    if (!m) throw new Error("no " + name + " in setup.c");
    const keys = m[1].match(/"[^"]+"/g).map((s) => JSON.parse(s));
    return /TALKER_KEYS/.test(m[1]) ? talker.concat(keys) : keys;
}
const D = schema.$defs, P = schema.properties;
const props = (s) => Object.keys(s.properties || {});
const roleProps = (s) => [...new Set(props(D.talker).concat(props(s)))];
const places = {
    TOP_KEYS: props(schema),
    SDR_KEYS: props(D.sdr_device),
    SDR_RX_KEYS: props(D.sdr_device.properties.rx),
    SDR_TX_KEYS: props(D.sdr_device.properties.tx),
    MATCH_KEYS: props(D.sdr_device.properties.match),
    MODEM_KEYS: props(D.modem_device),
    RADIO_KEYS: props(D.radio_device),
    PORT_KEYS: [...new Set(D.port.oneOf.flatMap(props))],
    DEV_BENCH_KEYS: props(D.dev_bench),
    MMDVM_KEYS: props(D.modem_device.properties.mmdvm),
    SPLIT_KEYS: props(D.split),
    ROAMER_KEYS: roleProps(D.roamer),
    REPEATER_KEYS: roleProps(D.repeater),
    SFR_KEYS: roleProps(D.sfr),
    DCDM_KEYS: roleProps(D.dcdm),
    MONITOR_KEYS: props(D.monitor),
    DEVREF_KEYS: props(D.devref),
    MQTT_KEYS: props(D.mqtt),
    HBLINK_KEYS: props(D.hblink),
    LOCATION_KEYS: props(D.location),
    OLED_KEYS: props(D.oled),
    I2C_KEYS: props(D.oled.properties.i2c),
    TIMING_KEYS: props(D.slot_timing),
    PLAN_KEYS: props(P.channels.additionalProperties.oneOf[1]),
    WIFI_KEYS: props(P.wifi),
    NET_KEYS: props(P.wifi.properties.networks.items),
    NTP_KEYS: props(P.ntp),
    LOAD_KEYS: props(D.selftest_load),
    BENCH_KEYS: props(P.bench),
    BENCH_TX_KEYS: props(P.bench.properties.tx),
    ROAM_CH_KEYS: props(D.chan_ref.oneOf[1]),
    MON_CH_KEYS: props(D.mon_chan_ref.oneOf[1]),
    IDLE_KEYS: props(D.roamer.properties.idle.oneOf[1]),
    SCAN_KEYS: props(D.roamer.properties.scan),
    TUNER_KEYS: props(D.modem_device.properties.mmdvm.properties.tuner),
    AGC_KEYS: props(D.sdr_device.properties.rx.properties.agc),
    USB_KEYS: props(D.sdr_device.properties.usb),
};
// The talker keys a monitor-less role takes come from the talker; "type" is each role's own.
for (const [name, schemaKeys] of Object.entries(places)) {
    const c = cList(name).sort(), s = schemaKeys.slice().sort();
    const onlyC = c.filter((k) => !s.includes(k)), onlyS = s.filter((k) => !c.includes(k));
    expect(!onlyC.length && !onlyS.length,
           name + ": only in setup.c [" + onlyC.join(", ") + "], only in the schema [" + onlyS.join(", ") + "]");
}
// Every key list in setup.c is covered above (SDR_BOARD_KEYS is version 1's, not a place in version 2).
for (const m of src.matchAll(/static const char \*const (\w+_KEYS)\[\]/g))
    expect(m[1] in places || m[1] === "SDR_BOARD_KEYS", m[1] + ": not compared with the schema");

console.log(checks + " checks, " + failed + " failed");
process.exit(failed ? 1 : 0);
