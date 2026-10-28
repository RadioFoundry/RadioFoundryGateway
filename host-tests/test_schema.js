// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The schema (config/rdof-config.schema.json) and the C parsers agree (docs/spec/fw/rxtx-combinations.md 4.1,
 * docs/spec/fw/config-v3.md):
 *  - every version 2 and 3 fixture: good ones pass the schema, bad ones fail it, except "bad-semantic-*" (only the C
 *    sees those: names that don't exist, ownership, caps), which pass it; test_setup.c and test_site.c run the same
 *    files through the C;
 *  - every key list in common/src/setup.c (version 2) and common/src/site.c (version 3) is the schema's list for
 *    that place, so a key added to one and not the other fails here.
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

// JSONC: // and /* */ comments outside strings go.
function jsonc(text) {
    let out = "", str = false;
    for (let i = 0; i < text.length; i++) {
        const c = text[i];
        if (str) {
            out += c;
            if (c === "\\") out += text[++i];
            else if (c === '"') str = false;
        } else if (c === '"') {
            str = true;
            out += c;
        } else if (c === "/" && text[i + 1] === "/") {
            while (i < text.length && text[i] !== "\n") i++;
            out += "\n";
        } else if (c === "/" && text[i + 1] === "*") {
            i = text.indexOf("*/", i + 2) + 1;
            if (i === 0) break;
        } else out += c;
    }
    return JSON.parse(out);
}

// The fixtures.
for (const v of ["v2", "v3"]) {
    const dir = path.join(__dirname, "fixtures/config", v);
    for (const f of fs.readdirSync(dir).filter((f) => f.endsWith(".json")).sort()) {
        const errs = check(schema, jsonc(fs.readFileSync(path.join(dir, f), "utf8")));
        const msg = errs.map((e) => e.path + ": " + e.message).join("; ");
        if (f.startsWith("good-") || f.startsWith("bad-semantic-"))
            expect(!errs.length, v + "/" + f + " should pass the schema: " + msg);
        else expect(errs.length > 0, v + "/" + f + " should fail the schema");
    }
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
const D = schema.$defs, P = D.v2.properties;
const props = (s) => Object.keys(s.properties || {});
const roleProps = (s) => [...new Set(props(D.talker).concat(props(s)))];
const places = {
    TOP_KEYS: props(D.v2),
    SDR_KEYS: props(D.sdr_keys),
    SDR_RX_KEYS: props(D.sdr_keys.properties.rx),
    SDR_TX_KEYS: props(D.sdr_keys.properties.tx),
    MATCH_KEYS: props(D.sdr_keys.properties.match),
    MODEM_KEYS: props(D.modem_keys),
    RADIO_KEYS: props(D.radio_keys),
    PORT_KEYS: [...new Set(D.port.oneOf.flatMap(props))],
    NTP_KEYS: props(P.ntp.oneOf[1]),
    DEV_BENCH_KEYS: props(D.dev_bench),
    MMDVM_KEYS: props(D.modem_keys.properties.mmdvm),
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
    LOAD_KEYS: props(D.selftest_load),
    BENCH_KEYS: props(P.bench),
    BENCH_TX_KEYS: props(P.bench.properties.tx),
    ROAM_CH_KEYS: props(D.chan_ref.oneOf[1]),
    MON_CH_KEYS: props(D.mon_chan_ref.oneOf[1]),
    IDLE_KEYS: props(D.roamer.properties.idle.oneOf[1]),
    SCAN_KEYS: props(D.roamer.properties.scan),
    TUNER_KEYS: props(D.modem_keys.properties.mmdvm.properties.tuner),
    AGC_KEYS: props(D.sdr_keys.properties.rx.properties.agc),
    USB_KEYS: props(D.sdr_keys.properties.usb),
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

// Version 3: site.c's key lists against the schema's places.
const site = fs.readFileSync(path.join(top, "common/src/site.c"), "utf8");
function macro(name) {
    return site.match(new RegExp("#define " + name + "\\s*\\\\?\\s*([^]*?)\\n(?!\\s)"))[1].match(/"[^"]+"/g).map((s) => JSON.parse(s));
}
const sitePol = macro("POLICY_KEYS"), siteTalker = macro("TALKER_KEYS");
function siteList(name) {
    const m = site.match(new RegExp("static const char \\*const " + name + "\\[\\] = \\{([^}]*)\\}"));
    if (!m) throw new Error("no " + name + " in site.c");
    let keys = m[1].match(/"[^"]+"/g).map((s) => JSON.parse(s));
    if (/POLICY_KEYS/.test(m[1])) keys = keys.concat(sitePol);
    if (/TALKER_KEYS/.test(m[1])) keys = keys.concat(siteTalker);
    return keys;
}
const pk = props(D.policy_keys), fobj = D.freq3.anyOf[1];
const sitePlaces = {
    TOP_KEYS: props(D.v3),
    FREQ_KEYS: props(fobj).concat(pk),
    DEFAULT_KEYS: props(D.defaults3).concat(pk),
    POL_KEYS: props(D.policy3).concat(pk),
    SLOT_KEYS: props(D.slot_policy.anyOf[1]).concat(pk),
    DEV_OWN_KEYS: props(D.dev3_own),
    HEARS_KEYS: props(D.dev3_own.properties.hears.anyOf[1]),
    REPEATER_KEYS: props(D.repeater3).concat(pk),
    PARROT_KEYS: props(D.parrot3),
    IPGW_KEYS: props(D.ipgw3),
    HBLINK_KEYS: props(D.hblink3),
    MQTT_KEYS: props(D.mqtt3),
};
for (const [name, schemaKeys] of Object.entries(sitePlaces)) {
    const c = [...new Set(siteList(name))].sort(), s = [...new Set(schemaKeys)].sort();
    const onlyC = c.filter((k) => !s.includes(k)), onlyS = s.filter((k) => !c.includes(k));
    expect(!onlyC.length && !onlyS.length,
           "site.c " + name + ": only in site.c [" + onlyC.join(", ") + "], only in the schema [" + onlyS.join(", ") + "]");
}
// site.c's other lists are version 2's keys it passes on (checked there) or the board's sections.
const passOn = ["RADIO_FREQ_KEYS", "DEV_TALKER_KEYS", "REPEATER_V2_KEYS", "IPGW_V2_KEYS", "HBLINK_V2_KEYS", "MQTT_V2_KEYS",
                "BOARD_KEYS"];
for (const m of site.matchAll(/static const char \*const (\w+_KEYS)\[\]/g))
    expect(m[1] in sitePlaces || passOn.includes(m[1]), "site.c " + m[1] + ": not compared with the schema");
// The board sections pass on: each is version 2's.
for (const k of siteList("BOARD_KEYS"))
    expect(JSON.stringify(D.v3.properties[k]) === JSON.stringify({$ref: "#/$defs/v2/properties/" + k}), "v3." + k);

console.log(checks + " checks, " + failed + " failed");
process.exit(failed ? 1 : 0);
