// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The schema (config/rdof-config.schema.json) and the C parsers agree (docs/spec/fw/config-v3.md):
 *  - every fixture in fixtures/config/v3: good ones pass the schema, bad ones fail it, except "bad-semantic-*" (only
 *    the C sees those: names that don't exist, ownership, caps), which pass it; test_site.c runs the same files
 *    through the C;
 *  - every key list in common/src/site.c and common/src/siteplan.c is the schema's list for that place, so a key
 *    added to one and not the other fails here.
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
const dir = path.join(__dirname, "fixtures/config/v3");
for (const f of fs.readdirSync(dir).filter((f) => f.endsWith(".json")).sort()) {
    const errs = check(schema, jsonc(fs.readFileSync(path.join(dir, f), "utf8")));
    const msg = errs.map((e) => e.path + ": " + e.message).join("; ");
    if (f.startsWith("good-") || f.startsWith("bad-semantic-"))
        expect(!errs.length, f + " should pass the schema: " + msg);
    else expect(errs.length > 0, f + " should fail the schema");
}

// The key lists: each C list against the schema's place for it.
function lists(file) {
    const src = fs.readFileSync(path.join(top, file), "utf8");
    const macros = {};
    for (const m of src.matchAll(/#define (\w+KEYS|DEV_COMMON)\s*\\?\s*([^]*?)\n(?!\s)/g))
        macros[m[1]] = m[2].match(/"[^"]+"/g).map((s) => JSON.parse(s));
    const out = {};
    for (const m of src.matchAll(/static const char \*const (\w+_KEYS)\[\] = \{([^}]*)\}/g)) {
        let keys = (m[2].match(/"[^"]+"/g) || []).map((s) => JSON.parse(s));
        for (const [name, mk] of Object.entries(macros))
            if (new RegExp("\\b" + name + "\\b").test(m[2])) keys = keys.concat(mk);
        out[m[1]] = keys;
    }
    return out;
}
const D = schema.$defs;
const props = (s) => Object.keys(s.properties || {});
const pk = props(D.policy_keys), own = props(D.dev3_own), sdr = D.sdr_keys.properties, mmdvm = D.modem_keys.properties.mmdvm;
const places = {
    "common/src/site.c": {
        TOP_KEYS: props(D.v3),
        FREQ_KEYS: props(D.freq3.anyOf[1]).concat(pk),
        DEFAULT_KEYS: props(D.defaults3).concat(pk),
        POL_KEYS: props(D.policy3).concat(pk),
        SLOT_KEYS: props(D.slot_policy.anyOf[1]).concat(pk),
        HEARS_KEYS: props(D.dev3_own.properties.hears.anyOf[1]),
        REPEATER_KEYS: props(D.repeater3).concat(pk),
        PARROT_KEYS: props(D.parrot3),
        IPGW_KEYS: props(D.ipgw3),
        HBLINK_KEYS: props(D.hblink3),
        BM_KEYS: props(D.bm3),
        MQTT_KEYS: props(D.mqtt3),
    },
    "common/src/siteplan.c": {
        SDR_KEYS: props(D.sdr_keys).concat(own),
        MODEM_KEYS: props(D.modem_keys).concat(own),
        MODEM_TX_KEYS: props(D.modem_tx),
        RADIO_KEYS: props(D.radio_keys).concat(own),
        SDR_RX_KEYS: props(sdr.rx),
        SDR_TX_KEYS: props(sdr.tx),
        MATCH_KEYS: props(sdr.match),
        PORT_KEYS: [...new Set(D.port.oneOf.flatMap(props))],
        DEV_BENCH_KEYS: props(D.dev_bench),
        MMDVM_KEYS: props(mmdvm),
        TUNER_KEYS: props(mmdvm.properties.tuner),
        AGC_KEYS: props(sdr.rx.properties.agc),
        USB_KEYS: props(sdr.usb),
        SPLIT_KEYS: props(D.split),
        LOCATION_KEYS: props(D.location),
        OLED_KEYS: props(D.oled),
        I2C_KEYS: props(D.oled.properties.i2c),
        TIMING_KEYS: props(D.slot_timing),
        WIFI_KEYS: props(D.wifi),
        NET_KEYS: props(D.wifi.properties.networks.items),
        NTP_KEYS: props(D.ntp.oneOf[1]),
        LOAD_KEYS: props(D.selftest_load),
        BENCH_KEYS: props(D.bench),
        BENCH_TX_KEYS: props(D.bench.properties.tx),
    },
};
// Lists that are part of another (checked there).
const partOf = ["RADIO_FREQ_KEYS", "DEV_TALKER_KEYS"];
for (const [file, want] of Object.entries(places)) {
    const have = lists(file);
    for (const [name, schemaKeys] of Object.entries(want)) {
        if (!(name in have)) {
            expect(false, file + " " + name + ": no such list");
            continue;
        }
        const c = [...new Set(have[name])].sort(), s = [...new Set(schemaKeys)].sort();
        const onlyC = c.filter((k) => !s.includes(k)), onlyS = s.filter((k) => !c.includes(k));
        expect(!onlyC.length && !onlyS.length,
               file + " " + name + ": only in the C [" + onlyC.join(", ") + "], only in the schema [" + onlyS.join(", ") + "]");
    }
    for (const name of Object.keys(have))
        expect(name in want || partOf.includes(name), file + " " + name + ": not compared with the schema");
}

console.log(checks + " checks, " + failed + " failed");
process.exit(failed ? 1 : 0);
