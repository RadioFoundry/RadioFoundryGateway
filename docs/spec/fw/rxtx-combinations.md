# Devices, resources and roles: any receivers, any transmitters (design; not built)

What: how RadioFoundry Gateway runs any mix of receivers and transmitters: two SDRs, four split-duplex radios, an SDR
plus a hotspot plus a split radio, an SFR radio, several SDRs for a trunked system, and later one SDR sending two or
more carriers. It also covers how bench requests (transmit, receive, an I/Q capture) share the same hardware, and the
config format (version 2) with its JSON Schema. This is the plan for the refactor; none of it is built yet.
Sources today: `common/src/{config,split,benchtx,benchcfg}.c`, `common/include/rdof/{config,modem,split}.h`,
`sdr/src/{sdrmodem,mod}.c`, `sdr/include/rdof/sdr/{frontend,sdrmodem,mod}.h`,
`esp_idf/main/{roamer,split_port,modem_sdr,sdr_task,bench_api,bench_tx,bench_radio}.c`, `sdr/cli/rdofradio.c`.
Related: [config.md](config.md), [sdr-tx.md](sdr-tx.md), [sdr-device-api.md](sdr-device-api.md),
[../bench-api.md](../bench-api.md), [../repeater.md](../repeater.md), [../slot-timing.md](../slot-timing.md) ("Remote sites").

## 1. Where the limits are today

The core can mostly do this already. The limits are in the config, the wiring and a few single pointers.

| Limit | Where | Kind |
|---|---|---|
| 2 roamers or repeaters and 1 SDR on a board | `config.h:40` `RDOF_MAX_RADIOS 2`; `rdof_roles_t` has one `has_sdr` / `sdr[32]`; `config.c:681-684` | config |
| One SDR modem; one owner | `modem_sdr.c:30,36` (`static mdm`, `owner`); `config.c:1363` | wiring |
| One receiver and one front end | `sdr_task.c:45,49` (`g_rx`, `fe`) | wiring (board) |
| Split: 1 SDR half, 1 USB half | `config.c:562-567`; `split_port.c:275` (the shell half reuses its parent's index) | config, wiring |
| The SDR as a split half on the board | `split_port.c:238-245`: the sample clock isn't stamped against `esp_timer` | missing feature |
| One TX source per front end | `frontend.h:151` `tx_set(src)`; `fe_lr2021.c` `tx.src`, `fe_sim.c` `st.tx` | API |
| One carrier per modulator | `mod.h:68-70` one `offset_hz` / `phase`; `sdrmodem.h:135` one `rdof_mod_t` | by design (keep) |
| RX and TX from two devices only through a split | `split.h`: `halves[2]` behind one `rdof_modem_t` | by design (keep) |
| Ports described in three places | a radio's `modem`/`uart`/`usb`, a split half's port, `bench.radios[].port` | config |
| The bench: one SDR, one TX job, `via=sdr\|modem\|radio:NAME` | `bench_tx.c`, `benchtx.c` | API |
| Host tool: one SDR modem, at most one radio | `rdofradio.c:99`; `--split-tx` | wiring (host) |
| USB host: a radio on USB *or* an RTL-SDR without a hub | `main.c:131-147` | hardware (board) |

Already general, and kept:
- `rdof_sdr_rx_t` is multi-channel (up to `RDOF_SDR_MAX_CHANNELS` 16, channels plus taps). It has one `on_burst(ctx, channel, ...)`.
- `rdof_sdrmodem_t` keeps all its state in its struct. Several can share one receiver, each on its own channel.
- `rdof_split_t` composes any RX-capable MMDVM half with any TX-capable one. The SDR modem already works as either half.
- The repeater and the transceiver talk MMDVM to one duplex modem, whatever is behind it.
- The bench already has sessions, leases on frequencies, a queue with `busy` and positions, and TX gates.

## 2. Model

**A device** is one piece of hardware the gateway opens, named once in the config:

| `kind` | What | Examples |
|---|---|---|
| `sdr` | a sample stream in, and out if it can transmit | RTL-SDR, the W12's LR2021, PlutoSDR, any SoapySDR or libiio device |
| `modem` | anything that speaks MMDVM on a serial port | an MMDVM hat or hotspot, a RadioFoundry radio's hotspot mode |
| `radio` | a RadioFoundry radio on its debug link (tune, PTT, guarded link) | bench stations |

A RadioFoundry radio can be both a `modem` and a `radio` (two names, one port, one owner at a time). What a modem can do
(duplex, `SPLIT1`, `DCDM`) comes from its description at run time. The config may require it (`"needs": ["split"]`), so a
missing capability is refused at start-up, not after the first call.

**A resource** is one thing a device gives. Resources are the unit of ownership:

| Resource | From | How many | Shared? |
|---|---|---|---|
| RX window | `sdr` | 1: the tuned center and rate | yes, while no one holds it exclusive (a retune) |
| RX channel | `sdr` | up to `RDOF_SDR_MAX_CHANNELS` in the window (demod, taps) | yes: any number of listeners |
| TX carrier | `sdr` that transmits | `tx.carriers` (1 for now, §5) | no |
| whole modem | `modem` | 1: duplex or simplex; its RX and TX can't be used apart | no |
| split half | `modem` with `SPLIT1` | RX or TX, one at a time | no |
| link | `radio` | 1 | no |

**A role** is what runs: a repeater, a roamer, an SFR repeater, DCDM, a monitor (RX only), and later a trunked site,
simulcast and voting. A role takes resources:

| Role | Takes |
|---|---|
| repeater (duplex) | one RX and one TX, on any devices |
| roamer / transceiver | one simplex or duplex modem, or one RX plus one TX on its frequency |
| SFR repeater | one simplex modem (the radio times its own slots) |
| DCDM | one duplex modem with `DCDM` in its description, not a split (R13.1) |
| monitor | RX channels on one or more SDRs |
| trunked site (later) | a control channel's RX and TX plus a pool of RX/TX pairs, on any devices |
| simulcast, voting (later) | one RX and several TX, or several RX and one TX ([../slot-timing.md](../slot-timing.md)) |

**How RX and TX meet.** A role on one modem talks to it directly, as today. A role whose RX and TX are on two devices is a
split (`rdof_split_t`), built by the runtime, never written out in the config. The pairs:
- an SDR channel and a radio's TX half;
- a radio's RX half and an SDR carrier;
- two radios;
- a channel on one SDR and a carrier on another.

A role on one SDR's channel and one of that SDR's carriers runs one `rdof_sdrmodem_t` in duplex, as `sdr_transceiver`
does now. The SDR modem becomes **one a role**, not one a board. They share the device's receiver and front end.

## 3. The broker

**`rdof_broker_t`** (new, `common/`) is a table of resources with their owner and caps. There is no I/O, so it's host-tested.
- **Each resource** has: its device, kind, frequency range (window or TX range), caps (`dmr`, `iq`, `fm`, `timed`,
  `power` range), shareable or not, and owner (none, a role, or a bench session).
- **Roles** claim at start-up. A role's claim lasts while the role runs.
- **Bench sessions** claim through their lease and queue in line (bench-api.md §4). The broker is that queue, so the
  bench's `busy` positions come from it.
- **The config check is the broker run dry.** Validating a config means building the table and claiming every role's
  resources. The messages name what ran out ("pluto.tx: carrier 2 of 1", "hs1: both R1 and R2 take it").
- **Picking.** A claim names a device, or asks for `"auto"` with requirements: frequency, direction, caps, timed or not.
  The broker picks in a fixed order:
  1. a shared RX channel already on the frequency;
  2. a free channel inside a running window;
  3. an idle SDR it may retune;
  4. for TX: a free SDR carrier (timed) before a whole modem, and a whole modem before a bench radio.

  The order is a table, so it can change without touching the callers.

The runtime keeps the table live. A device that goes away (unplugged, USB error) marks its resources `unavailable`.
Their owners get the same `on_lost` they get today, and `auto` claims can move.

### 3.1 The bench on the broker

The bench API keeps its shape (sessions, leases, gates). What changes is how a request names the hardware:
- **`via=device:NAME`** names a device. The current forms are aliases: `via=radio:NAME` (a `radio` or `modem`
  device), `via=modem` (the SDR modem of the one SDR), and `via=sdr`.
- **`via=auto`** lets the broker pick the hardware for the job (TX bursts, a capture, a bursts stream, I/Q), by
  frequency and caps. The ack and `done` say which device ran it.
- **Resources owned by a role are off limits to the bench**, except shared RX channels, which anyone may listen to. Once
  an SDR transmits more than one carrier, a bench job can claim a free carrier on an SDR a repeater is using. Nothing in
  the API changes for that.
- **`GET /info`** gains `devices`: each device with its resources, caps and owners. The current `sdr`, `radios` and
  `channels` stay, built from that list.
- **What a device may transmit for the bench** (`freqs_hz`, `max_key_s`, power) moves into the device, as `bench.tx`.
  The top-level `bench.tx` stays as the default.
- **A queued request** is a lease plus a job. A job that can't start now waits in line (`queued`, with its position),
  as the lease does today. A job is one of `tx_bursts`, `tx_iq`, `tx_fm`, `rx_bursts`, `iq` and `capture`.

Nothing here is new protocol, only new values. A client that sends `via=auto` to an older board gets `bad_request` and
can fall back.

## 4. Config, version 2

`"version": 2` at the top. Version 1 (today's) still loads: `rdof_config_upgrade` turns it into version 2 on read, and the
board saves version 2 from then on. A host test runs every version 1 fixture through the upgrade and checks that the
roles come out the same.

```json
{
  "version": 2,
  "name": "site1",
  "devices": {
    "pluto": { "kind": "sdr", "driver": "iio", "uri": "local:",
               "rx": { "center_hz": "auto", "rate": 1000000, "gain_db": "auto", "ppm": 0 },
               "tx": { "carriers": 1, "power_dbm": -10, "center_hz": "auto",
                       "bench": { "freqs_hz": [438487500], "max_key_s": 10 } } },
    "rtl":   { "kind": "sdr", "driver": "rtlsdr", "match": { "serial": "00000001" },
               "rx": { "rate": 288000, "gain_db": 30 } },
    "hs1":   { "kind": "modem", "port": { "usb": "cdc-acm", "vid": "0483", "pid": "5740", "serial": "..." },
               "needs": ["split"] },
    "rf1":   { "kind": "radio", "port": { "uart": 1, "tx": 17, "rx": 18, "baud": 921600 } }
  },
  "channels": { "13": { "freq": 438062500, "cc": 2 }, "...": {} },
  "roles": {
    "R1":  { "type": "repeater", "rx_freq": 438250000, "tx_freq": 438012500,
             "rx": { "device": "pluto" }, "tx": { "device": "hs1" }, "peer_id": 9800001 },
    "R2":  { "type": "repeater", "rx_freq": 438375000, "tx_freq": 438350000,
             "rx": { "device": "pluto" }, "tx": { "device": "pluto" } },
    "MON": { "type": "monitor", "rx": { "device": "auto" }, "channels": [13, 4, 6] },
    "SFR": { "type": "sfr", "modem": { "device": "hs1" } }
  },
  "run": ["R1", "R2", "MON"],
  "bench": { "devices": ["rf1", "pluto"], "tx": { "max_key_s": 10 } },
  "mqtt": {}, "hblink": {}, "slot_timing": {}
}
```

- **`devices`** holds every port and SDR setting in one place. A role, a split half and the bench name a device; none
  carries its own port. Selectors (`match`: serial, USB path, URI) tell identical devices apart.
- **`roles`** replaces `radios`, and `run` lists roles. A role's `rx` and `tx` each name a device or `"auto"` (with
  requirements taken from the role's frequencies). A role on one modem has `modem` instead.
- **An SDR's TX** has `carriers` and `center_hz` (`"auto"`: the middle of its carriers). Carriers are never written per
  role or numbered: a role names the device, and the broker gives it a carrier at its frequency. So more carriers
  later is a bigger number, not a new format.
- **`center_hz: "auto"`** on RX puts the window over every channel the roles and the monitor want on that device, and
  refuses a plan that doesn't fit. A trunked site naming several SDRs with `"auto"` gets its channels spread over them.
- **Box keys** (`mqtt`, `hblink`, `slot_timing`, `location`, `oled`, `ip_gateway`, `wifi`, `ntp`) keep their place and
  meaning.
- **Caps come from the platform**, not the format: the board from its RAM (each extra role about 20 KB internal plus
  PSRAM), UARTs and USB ports; hosts from a compile-time cap (16 devices, 16 roles) for the static arrays. The message
  says what ran out.

### 4.1 The JSON Schema

`config/rdof-config.schema.json`, JSON Schema 2020-12, committed. It is the one description of the format:
- **Its scope:** keys, types, ranges, enums, required keys, `additionalProperties: false`, and the conditionals on
  `kind` and `type` (`if`/`then`). One file, with `$defs` for the device kinds, the role types, ports and the box
  sections, and a `description` on each key.
- **What it can't check** is left to the C parser, which is still the authority:
  - device names that exist;
  - ownership: two roles on one modem, too many carriers;
  - frequency plans that fit a window;
  - platform caps.
- **Where it's used:**
  - The web UI checks against it as you edit, with `config/schema-check.js` (our own checker of the keywords the
    schema uses; it runs in the browser and in node). Not wired into the page yet.
  - `tools/config_check.py FILE` checks the schema with the `jsonschema` package (MIT), outside the firmware, or
    with node and `schema-check.js` when the package isn't installed. Asking the board for the full check
    (`--board IP`) waits for `?check=1`.
  - The board serves the schema at `GET /api/config/schema`.
  - `POST /api/config?check=1` runs the full check (the schema's rules, then the broker run dry) and saves nothing. It
    answers `{"ok", "errors": [{"path", "message"}], "roles": [...]}`. A save runs the same check first.
- **Kept in step:** `host-tests/test_schema.js` (ctest `rdof_schema_tests`, when node is installed) runs every
  version 2 fixture in `host-tests/fixtures/config/v2/` through the schema, and `test_setup.c` runs them through the
  C parser. A fixture's name says good or bad; both must agree, except for `bad-semantic-*` (only the C catches it).
  The test also compares each key list in `common/src/setup.c` with the schema's keys for that place, so a key
  added to one and not the other fails it.
- **Version 1 configs** are checked after the upgrade. The schema describes version 2 only.

## 5. SDR transmit: carriers

`rdof_mod_t` stays one carrier. What changes is what sits between the modems and the front end.

**`rdof_sdr_tx_t`** (new, `sdr/`) is the front end's one TX source. It holds up to `RDOF_SDR_MAX_CARRIERS` carriers,
each with a pull from one role's SDR modem and an offset from the TX center:

```c
int  rdof_sdr_tx_claim(rdof_sdr_tx_t *t, double offset_hz, const rdof_fe_tx_source_t *src); /* carrier id, or < 0 */
int  rdof_sdr_tx_retune(rdof_sdr_tx_t *t, int id, double offset_hz);
void rdof_sdr_tx_release(rdof_sdr_tx_t *t, int id);
const rdof_fe_tx_source_t *rdof_sdr_tx_source(rdof_sdr_tx_t *t);  /* what tx_set gets */
```

- **One carrier** passes straight through, so output, timing and CPU stay the same as today. The golden tests stay byte
  for byte.
- **More than one carrier:** each carrier renders into its own buffer and the mixer adds them at 1/N. A carrier that
  isn't keyed adds nothing, and the sum is keyed when any carrier is. This is built and host-tested at 2 carriers
  (each demodulates clean in the receiver), so the API is proven.
- **What runs:** `RDOF_SDR_MAX_CARRIERS` is 4 in the code. Every front end reports `caps.tx_carriers` = 1 for now, and
  the config check refuses `tx.carriers` above a device's caps ("one carrier a transmitter for now").
- **The SDR modem** claims its carrier through the broker. Its `tx_tune` becomes a retune of that carrier. It no longer
  calls `tx_set` itself.

**Left for turning it on** (no format change):
- the power split: each carrier −10·log10(N) dB, plus PA back-off for the peak-to-average of N FSK carriers;
- the CPU budget for N modulators on the Pluto and the host;
- spacing and bandwidth checks;
- raising `tx_carriers` per front end once tested on air.

FREQ-mode front ends (the LR2021 tunes its own carrier) stay at 1.

## 6. Time

A role's RX and TX need one slot clock.

| Pair | Clock | State |
|---|---|---|
| RX channel and TX carrier on one SDR | the device's sample clock | works |
| RX and TX on two SDRs | each SDR's stamped sample clock, tied to UTC or the host clock | design |
| SDR RX, radio TX half (host) | the split's TIME clock (`split_now48`) | works (`rdofradio --split-tx`) |
| SDR as a split half on the board | needs the sample clock stamped against `esp_timer` | not done (`split_port.c`) |
| two radios as halves | the split's clock fit | works |
| several TX for one RX (simulcast) | UTC ([../slot-timing.md](../slot-timing.md)) | design |

The broker refuses a pairing that has no clock: an SDR half on the board, for now.

## 7. Steps

Each step builds, passes the host tests, and leaves every current config running as it does.

1. **The format:** the schema, the version 2 parser, `rdof_config_upgrade`, and the fixtures (each combination in
   the request: 2 SDRs; 4 split radios; SDR + hotspot + split radio; SFR; a monitor on 2 SDRs), checked by both the
   schema and the C parser. Also `tools/config_check.py`, `GET /api/config/schema` and `?check=1`.
2. **The broker:** the table and the dry-run check, replacing the counts in `config.c`. Host tests.
3. **`rdof_sdr_tx_t`:** one carrier live, two tested; front ends report `tx_carriers`; the SDR modem claims through the
   broker. The golden tests stay unchanged.
4. **The host tool** (`rdofradio`, Linux / macOS / the Pluto): runs a version 2 config with N devices and N roles in one
   process. The current flags build a config in memory. The Pluto and the Mac run the combinations first, since they
   have the RAM.
5. **The board:** an array of SDR modems and receivers; `split_port.c` drops "one USB half"; caps follow RAM; the web
   page's config editor moves to version 2.
6. **The bench:** `via=device:NAME` and `via=auto`, `devices` in `/info`, and jobs queued by the broker.
7. **Later:** the SDR split half on the board (the `esp_timer` stamp), multi-carrier on air (§5), trunking.

## 8. Licensing

All of this is the project's own MIT code, with no new libraries in the firmware or the host tool.
- **The schema** is our own file.
- **`tools/config_check.py`** uses `jsonschema` (MIT), a tool dependency only.
- **The device layers already in use** stay as they are: SoapySDR is Boost 1.0; libiio and libad9361 are LGPL, linked
  dynamically, only in the host tool, and listed in `PROVENANCE.md`.
- **The web UI's checker** must be a permissive validator (Ajv is MIT), or our own.

Nothing GPL comes in. Summing carriers is plain arithmetic in our own code.
