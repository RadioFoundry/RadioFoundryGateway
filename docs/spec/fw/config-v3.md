# Config, version 3

Covers: the version 3 box config: devices, frequencies, apps, networks and the policies that say which calls move
between a talk path and a network; how a box runs it (§12); its limits (§13).
Code: the parser and the resolved view `common/src/site.c` ([site.h](../../../common/include/rdof/site.h)), ID sets
`idset.c`, the router `router.c` and, on a running box, `siteroute.c`; rdofradio (`sdr/cli/rdofradio.c`) and the
firmware (`esp_idf/main/site_run.c`) run it. The schema is `config/rdof-config.schema.json` (`tools/config_check.py`).
Related: [config.md](config.md) (version 1 keys), [rxtx-combinations.md](rxtx-combinations.md) (version 2, the broker,
the channel usages), [hblink.md](hblink.md), [mqtt.md](mqtt.md), [ipgw.md](ipgw.md).

## 1. Why a version 3

Version 2 names roles: a roamer, a repeater, a monitor, an SFR, a DCDM. Each role mixes three things: the hardware it
uses, the frequencies it works on, and where its calls go. That makes simple sites wordy (a monitor role to receive,
a roamer to transmit simplex on the same frequencies) and leaves no place for per-path rules (which talkgroups, which
private calls, which apps).

Version 3 splits them:

| Section | What it is | Says nothing about |
|---|---|---|
| `devices` | hardware and what it can do | frequencies' use, calls |
| `frequencies` | RF: frequency, color code, mode, whether anything may transmit there, and its call policy | hardware |
| `apps` | things that take frequencies (a repeater) or answer calls (parrot, IP gateway) | hardware |
| `networks` | connections that carry calls: HBlink, MQTT, later others | RF |
| `policies` | named call policies, shared by several paths | anything else |

Nothing names a role and nothing says what runs: what runs follows from the sections. Which device serves which
frequency is the broker's job ([rxtx-combinations.md](rxtx-combinations.md) §3); a config only pins a device when it
must.

## 2. Document

```jsonc
{
  "version": 3,
  "name": "site1",
  "id": 9800100,
  "defaults": {"cc": 2, "ts": 1},
  "devices": [ ... ],
  "frequencies": [ ... ],
  "apps": [ ... ],
  "networks": [ ... ],
  "policies": [ ... ]
}
```

| Key | Type | Default | Meaning |
|---|---|---|---|
| `version` | 3 | | required |
| `name` | str | `"radiofoundry"` | box name, as now |
| `id` | int | | the box's DMR ID: its HBlink peer ID, and the base for per-path peer IDs (§7.1); required with an `hblink` network |
| `callsign` | str | | the box's callsign, on every HBlink peer it runs |
| `pc_hold_s` | int | 900 | how long a radio heard on a path takes the private calls to it (§9.3) |
| `defaults` | object | `{}` | any frequency key, policy keys included, for every frequency that doesn't set it |
| `devices`, `frequencies`, `apps`, `networks`, `policies` | list | `[]` | §4 to §8 |

The board keys keep their place and meaning: `location`, `wifi`, `ntp`, `oled`, `slot_timing`, `bench`. `mqtt` and
`hblink` move into `networks`. JSONC, as now. Frequencies: a number below 1e5 is MHz, else Hz, as now.

### 2.1 Lists and names

Every section is a list. An entry's `name` is optional and defaults as below. Names are unique within a section.

| Section | Default name | A name is required when |
|---|---|---|
| `frequencies` | the frequency in MHz, as written (`"438.4875"`) | the same frequency appears twice |
| `devices` | the kind and its index in the list (`sdr0`, `modem1`) | never |
| `networks` | the type (`hblink`, `mqtt`) | two networks of one type |
| `apps` | the type (`parrot`, `repeater`) | two apps of one type |
| `policies` | | always |

A reference to a frequency matches its name first, then its frequency as a number: `439`, `"439"` and `"439.0"` all
find the entry at 439 MHz. A name that is another entry's frequency is refused.

A bare number in `frequencies` is an entry with only `freq`: `438.1125` is `{"freq": 438.1125}`.

## 3. ID sets

Talkgroups and private-call IDs use one syntax everywhere:

| Form | Meaning |
|---|---|
| `202` | one ID |
| `"3100-3199"` | a range, both ends included |
| `"*"` | any ID |
| `"!350"`, `"!4000-4099"` | not these (in a list) |
| `[202, "300-399", "!350"]` | a list: the union of the plain items, less the `!` items |
| `"202,300-399,!350"` | the same list as one string |
| `false` | none |

A list of only `!` items starts from `*`: `"!9"` is every ID but 9. Order doesn't matter. The resolved view (§11) shows
each set expanded to ranges.

## 4. Devices

The version 2 device keys stay as they are ([rxtx-combinations.md](rxtx-combinations.md) §4): `kind` (`sdr`, `modem`,
`radio`), `driver`, `match`, `rx`, `tx`, `port`, `type`, `needs`, `bench`. Two keys are new:

| Key | Type | Default | Meaning |
|---|---|---|---|
| `hears` | object or `false` | an SDR: every frequency in its window. A modem or radio: its idle frequency | where a one-frequency device listens: `{"idle": "3"}` or `{"scan": ["3", "4", "6"]}`; `false`: it never receives |
| `transmits` | list or `false` | every frequency it can reach that allows transmit | the frequencies it may transmit on; `false`: it never transmits |

An SDR has no `hears`: it hears every frequency inside its receive window, and the rate is chosen to cover them
(as rdofradio's automatic rate does now). `hears.scan` on an SDR is reserved for later (frequencies outside its
window).

A modem's `type` (its firmware, `mmdvm` and the others) and its other keys are version 2's. A device that transmits
on several frequencies also takes the version 2 roamer's wait-and-answer keys (`listen_s`, `clear_s`, `idle_after_s`,
`csbk_preambles`, `answer_s`, `answer_max_s`, the jitter buffer, `cwid`).

What version 2 called a roamer is a modem with `hears` and `transmits`. HBlink4 roaming (DMRT, the master moving the
device to a frequency) is a network option (§7.1), not a role.

## 5. Frequencies

| Key | Type | Default | Meaning |
|---|---|---|---|
| `freq` | number | | required |
| `name` | str | the frequency | §2.1 |
| `cc` | 0..15 | `defaults.cc` | color code. A frequency with `tx: false` may leave it out: any color code is taken |
| `mode` | `simplex` \| `sfr` \| `dcdm` | `simplex` | what the frequency carries when no app takes it (§6.1) |
| `ts` | 1 \| 2 | 1 | a simplex call's slot: plain direct mode carries none on the air, so a call heard here goes to the master, last heard and MQTT as `ts`, and the answer comes back on it |
| `tx` | bool | true | false: nothing ever transmits here; its paths are receive only |
| `power` | int | | transmit power, as now |
| `devices` | list | any | pin the devices that may serve it |
| `slots` | object | | per-slot policy, for a frequency heard as two slots (DCDM, a monitored repeater output) |
| policy keys | | | §8, for the frequency's path |

A frequency no app takes is a talk path by itself (§6.1). A frequency an app takes is no longer a path; the app
offers its own paths. Any frequency inside an SDR's window is heard either way, so a repeater's input can also be
reported.

## 6. Apps

| Type | Takes | Offers | Keys |
|---|---|---|---|
| `repeater` | `in` and `out` frequencies | paths `<name>/1`, `<name>/2` | `in`, `out`, `slots` (policy per slot), `hang_s`, `call_hang_s`, `cwid`, and the version 2 repeater keys |
| `parrot` | | answers the IDs in its `tg` or `pc` | `tg` or `pc` |
| `ipgw` | | answers its ID | `tg` or `pc`, and the version 2 `ip_gateway` keys |

Later, each as an app type, with no new section: `voting` (the best copy of each burst from several receivers),
`simulcast` (one path, several transmitters), `trunk` (a control frequency and a pool; it assigns its own paths),
`recorder`.

An app that answers IDs (parrot, IP gateway) claims them: a call to a claimed ID on a path whose `apps` allows the app
goes to the app and not to the network. Two apps may not claim the same ID.

### 6.1 Talk paths

A talk path is a place a call comes in or goes out. Policies (§8) are set on paths.

| Path | Name | Slots |
|---|---|---|
| a simplex frequency | the frequency's name | one path; transmits on `ts`, hears both |
| an SFR frequency | the frequency's name | one path (the radio times the slots) |
| a DCDM frequency | `<name>/1`, `<name>/2` | a path per slot; policy in `slots` |
| a frequency with `tx: false` | the frequency's name | one path, both slots heard; `slots` narrows it per slot |
| a repeater | `<app>/1`, `<app>/2` | a path per slot; policy in `slots` |

## 7. Networks

| Key | Type | Default | Meaning |
|---|---|---|---|
| `type` | `hblink` \| `mqtt` | | required. Later: `openbridge`, others |
| `name` | str | the type | §2.1 |

The first network in the list is the default: a path with no `net` uses it.

### 7.1 `hblink`

The version 2 `hblink` keys (`host`, `port`, `transport`, the SCTP keys), plus:

| Key | Default | Meaning |
|---|---|---|
| `identity` | `single` | `single`: the box is one peer (`id`), and its policies decide what goes where. `per_path`: each frequency or repeater is its own peer (`id` + 1, + 2, ... in list order, or `peer_ids`), and the master decides where a call goes: a stream sent to a peer goes only to that peer's paths |
| `peer_ids` | | `{"<frequency or repeater>": id}`, for `per_path`; a repeater's two slots share its peer. Each such peer gives the master its frequency's or repeater's name (the first 8 characters) as its callsign |
| `roaming` | false | let the master move a device that `transmits` on several frequencies (DMRT / DMRK) |

### 7.2 `mqtt`

The version 2 `mqtt` keys (`host`, `port`, `username`, `password`, `root`, `tls`), plus:

| Key | Default | Meaning |
|---|---|---|
| `report` | true | status, the call JSON and the raw bursts, on today's topics ([mqtt.md](mqtt.md)) |
| `calls` | false | carry calls: bursts in and out on topics per path and talkgroup. With it, MQTT can be the only network (no HBlink) |

A box with `mqtt` and `report` only, and no other network, reports but carries no calls. With `calls`, a path whose
`net` is the MQTT network sends what it carries as DMRD packets on `{root}/calls/{tg|pc}/{dst}/{box}` and takes
the other boxes' from `{root}/calls/#` ([mqtt.md](mqtt.md) §10).

## 8. Policies

A policy says which calls a path carries. It is a set of these keys, on a frequency (for its path), in a `slots` entry
(for a slot's path), or in a named policy:

| Key | Type | Default | Meaning |
|---|---|---|---|
| `tg` | ID set | none | talkgroups carried, always on |
| `ptt` | ID set | none | talkgroups carried once an RF user keys one, for `ptt_hold_s` after the last call |
| `ptt_hold_s` | int | 600 | |
| `pc` | ID set | `"*"` | private calls carried, by the ID called (rf→net) or the ID calling (net→rf) |
| `apps` | `"*"`, a list of app names, or `false` | `"*"` | the apps this path may reach |
| `net` | network name | the first network | where this path's calls go and come from |
| `dir` | `both` \| `rf>net` \| `net>rf` | `both`; `rf>net` when the frequency has `tx: false` | |

Where a path gets a key, first match wins:

1. the path's own keys (the frequency, or the `slots` entry);
2. the named policy it uses (a `slots` value or `"policy": "<name>"` on a frequency);
3. `defaults` (frequencies only; a repeater slot doesn't take `defaults`);
4. the table above.

A `slots` value is a policy name or an inline policy: `{"1": "open", "2": {"tg": 3100}}`.

A named policy:

```jsonc
"policies": [
  {"name": "open",   "tg": "*,!9,!4000-4099", "apps": false},
  {"name": "closed", "tg": "3100,3120,3177"}
]
```

## 9. How calls move

### 9.1 RF to network

A call heard on a path:

1. A group call to a TG claimed by an app the path may reach goes to that app. Same for a private call to a claimed ID.
2. Else, a group call whose TG is in the path's `tg`, or in its `ptt` (which starts or renews the hold), goes to the
   path's network, if `dir` allows.
3. Else, a private call whose called ID is in `pc` goes to the network, if `dir` allows.
4. Else it is not sent. It still shows in last heard and the call JSON (`report`).

A repeater repeats locally first, whatever its policy.

### 9.2 Network to RF, group calls

A group call from a network goes to every path on that network whose `tg`, or held `ptt`, holds the TG, and whose
`dir` allows. That fan-out is by design: a TG on two paths is sent on both.

When one path is wanted by two calls:

- A TG listed by ID or range beats one matched only by `*`.
- Between equals, the call already on the path keeps it; the newer one is refused (and counted).
- An emergency call beats both.

### 9.3 Network to RF, private calls

A private call from a network goes to the path where the called radio was last heard (within `pc_hold_s`, default
900 s), if that path's `pc` holds the calling ID and its `dir` allows. A radio not heard within the hold: the call is
dropped and counted. The last-heard table is per box.

With `identity: per_path` the master picks the peer, so a private call sent to a peer goes to that peer's path whether
or not the box heard the radio there.

### 9.4 Transmitting

A path's transmission goes to a device that `transmits` on its frequency and is free, preferring one already there:
a repeater app's repeater, an SFR or DCDM frequency's repeater, or a transceiver tuned to the frequency (its `cc` and
`power`) for the call. None free: the call isn't sent on that path (counted). The receiver drops our own transmission
heard back, so it never goes back to the network.

## 10. Scenarios

### 10.1 Commercial: 16 simplex frequencies

- 16 frequencies, all simplex; one TG each, all slot 1.
- Each TG both ways; every private call both ways.
- Parrot on 9990 and the IP gateway on 9991, from any frequency.

```jsonc
{
  "version": 3,
  "name": "plant",
  "id": 9800100,
  "defaults": {"cc": 2, "ts": 1},
  "devices": [
    {"kind": "sdr", "driver": "iio", "match": {"uri": "local:"}}
  ],
  "frequencies": [
    {"freq": 438.0625, "tg": 201},
    {"freq": 438.0875, "tg": 202},
    {"freq": 438.1125, "tg": 203},
    {"freq": 438.1375, "tg": 204},
    {"freq": 438.1625, "tg": 205},
    {"freq": 438.1875, "tg": 206},
    {"freq": 438.2125, "tg": 207},
    {"freq": 438.2375, "tg": 208},
    {"freq": 438.2625, "tg": 209},
    {"freq": 438.2875, "tg": 210},
    {"freq": 438.3125, "tg": 211},
    {"freq": 438.3375, "tg": 212},
    {"freq": 438.3625, "tg": 213},
    {"freq": 438.3875, "tg": 214},
    {"freq": 438.4875, "tg": 215},
    {"freq": 438.7125, "tg": 216}
  ],
  "apps": [
    {"type": "parrot", "tg": 9990},
    {"type": "ipgw",   "tg": 9991}
  ],
  "networks": [
    {"type": "hblink", "host": "hb.example.net", "port": 62031}
  ]
}
```

- No policies: `pc` and `apps` default to all, and `dir` to both.
- The SDR hears all 16 at once (438.0625 to 438.7125 fits one window) and transmits on whichever a network call
  needs, one at a time.

### 10.2 Two repeaters on split radios

- Two repeaters, each on its own pair of radios: one receives, one transmits.
- TS1: every TG both ways except a deny list; every private call.
- TS2: only a few TGs; every private call; parrot and the IP gateway.

```jsonc
{
  "version": 3,
  "name": "hilltop",
  "id": 9800200,
  "defaults": {"cc": 1},
  "devices": [
    {"name": "r1rx", "kind": "modem", "port": {"path": "/dev/ttyACM0"}, "hears": {"idle": "r1 in"}, "transmits": false},
    {"name": "r1tx", "kind": "modem", "port": {"path": "/dev/ttyACM1"}, "hears": false, "transmits": ["r1 out"]},
    {"name": "r2rx", "kind": "modem", "port": {"path": "/dev/ttyACM2"}, "hears": {"idle": "r2 in"}, "transmits": false},
    {"name": "r2tx", "kind": "modem", "port": {"path": "/dev/ttyACM3"}, "hears": false, "transmits": ["r2 out"]}
  ],
  "frequencies": [
    {"name": "r1 in",  "freq": 438.25},
    {"name": "r1 out", "freq": 438.75},
    {"name": "r2 in",  "freq": 438.3},
    {"name": "r2 out", "freq": 438.8}
  ],
  "policies": [
    {"name": "open",   "tg": "*,!9,!4000-4099", "apps": false},
    {"name": "closed", "tg": "3100,3120,3177"}
  ],
  "apps": [
    {"name": "R1", "type": "repeater", "in": "r1 in", "out": "r1 out", "slots": {"1": "open", "2": "closed"}},
    {"name": "R2", "type": "repeater", "in": "r2 in", "out": "r2 out", "slots": {"1": "open", "2": "closed"}},
    {"type": "parrot", "tg": 9990},
    {"type": "ipgw",   "tg": 9991}
  ],
  "networks": [
    {"type": "hblink", "host": "hb.example.net", "port": 62031}
  ]
}
```

- `open` has `apps: false`, so 9990 keyed on TS1 is an ordinary TG and goes to the network.
- A network call on TG 3100 goes to R1/2 and R2/2 (listed) and to R1/1 and R2/1 (`*`). Where it meets another call,
  the listed slots win (§9.2).
- `pc` is the default (all) on both slots.

### 10.3 One full-duplex SDR: a repeater and four forwarders

- One full-duplex SDR: five receive frequencies, one transmit frequency.
- A repeater using the SDR for its input and output.
- Four forwarders: one per remaining frequency, receive only, sending everything to a network.

```jsonc
{
  "version": 3,
  "name": "tower",
  "id": 9800300,
  "defaults": {"cc": 1},
  "devices": [
    {"kind": "sdr", "driver": "iio", "match": {"uri": "local:"}}
  ],
  "frequencies": [
    {"name": "rpt in",  "freq": 438.25},
    {"name": "rpt out", "freq": 438.75},
    {"name": "fwd A", "freq": 438.9375, "tx": false, "tg": "*", "net": "mqtt"},
    {"name": "fwd B", "freq": 438.9625, "tx": false, "tg": "*", "net": "mqtt"},
    {"name": "fwd C", "freq": 438.9875, "tx": false, "tg": "*", "net": "mqtt"},
    {"name": "fwd D", "freq": 438.6, "tx": false, "tg": "*", "net": "mqtt"}
  ],
  "apps": [
    {"name": "R1", "type": "repeater", "in": "rpt in", "out": "rpt out",
     "slots": {"1": {"tg": 3100}, "2": {"tg": "3101-3199", "ptt": "3200-3299", "ptt_hold_s": 600}}}
  ],
  "networks": [
    {"type": "hblink", "host": "hb.example.net", "port": 62031},
    {"type": "mqtt", "host": "broker.example.net", "root": "DMR", "calls": true}
  ]
}
```

- The SDR transmits only on "rpt out": the forwarders have `tx: false`.
- The forwarders' `dir` is `rf>net` (from `tx: false`) and `pc` is all (the default), so they pass every call, both
  slots.
- The repeater uses HBlink, the first network.

### 10.4 Monitoring only: eight frequencies to MQTT

- Eight frequencies: four repeater outputs and four simplex frequencies.
- Receive only; every call on every frequency goes to MQTT; no apps.

```jsonc
{
  "version": 3,
  "name": "listener",
  "id": 9800400,
  "defaults": {"tx": false, "tg": "*"},
  "devices": [
    {"kind": "sdr", "driver": "rtlsdr", "match": {"serial": "00000001"}, "rx": {"gain_db": 30}}
  ],
  "frequencies": [
    {"name": "city rpt",   "freq": 438.5000, "cc": 1},
    {"name": "county rpt", "freq": 438.5500, "cc": 3},
    {"name": "fire rpt",   "freq": 438.6250, "cc": 1},
    {"name": "water rpt",  "freq": 438.6750, "cc": 7},
    {"name": "yard 1",     "freq": 438.0625},
    {"name": "yard 2",     "freq": 438.0875},
    {"name": "yard 3",     "freq": 438.1125},
    {"name": "yard 4",     "freq": 438.1375}
  ],
  "networks": [
    {"type": "mqtt", "host": "broker.example.net", "root": "DMR", "calls": true}
  ]
}
```

- `defaults` makes every frequency receive only, carrying every TG; `pc` is all by default. So every call goes to
  MQTT.
- Nothing transmits anywhere: every frequency has `tx: false`, and the RTL-SDR can't transmit anyway.
- The simplex frequencies leave out `cc`, so any color code is taken.
- With `calls: false`, the same file still sends every burst on the report topics (raw and call JSON), as the monitor
  does now.

## 11. The resolved view

`GET /api/resolved` ends, for a version 3 file, with the config as it runs (`rdof_site_resolved_text`, JSON with a
line per entry):

- `frequencies`: each with every key filled in from `defaults`, and the devices that hear it and may send on it;
- `paths`: each with its policy expanded (ID sets in their canonical form), its peer, and the source of each key
  (`own`, `policy <name>`, `defaults`, `default`);
- `talkgroups`: the reverse view, each TG or range with the path that carries it and how (`static`, `ptt`, `*`,
  `ptt *`);
- `apps`, `networks` and `devices`.

A file the parser refuses doesn't run; the page's settings save says why (the field's path and the rule).

## 12. How a box runs it

A version 3 file is parsed (`rdof_site_parse`) and lowered to the version 2 roles the box already runs
(`rdof_site_lower`, which `rdof_setup_parse` calls on its own); the router then sits between those roles' receivers
and the networks:

| Version 3 | Runs as |
|---|---|
| `frequencies` | `channels`, numbered in list order, each with its `tg` when the path carries one talkgroup |
| a `repeater` app | a `repeater` role on its `in` and `out` |
| an `sfr` or `dcdm` frequency | an `sfr` or `dcdm` role on the device that may send there |
| a device that transmits on simplex frequencies | a `roamer` role named after the device: those frequencies are its channels, `hears.idle` (else the first) its idle channel, `hears.scan` its scan list |
| each SDR | a `monitor` role `<device>-rx` of every frequency it hears that no repeater takes; with `per_path`, each channel its frequency's peer |
| a `parrot` app | the `parrot` of each role whose paths may reach it |
| an `ipgw` app | `ip_gateway` |
| `networks` | `hblink` and `mqtt`; each role's `peer_id` is its path's peer (§7.1) |

The router (`rdof_siteroute_*`) is asked at each stream's start, by every sink that would send it (the HBlink peer,
MQTT calls), and once for the stream: the network it goes to, or none. A network's burst goes to the transmitters the
router picks (§9.4). The parrot's admit is the path's `apps`.

With `identity: single` the first roamer or repeater owns the box's HBlink peer and every other role and channel
sends through it. HBlink4 credits a stream to the peer's last DMRC (its frequency), so a stream another role sends
shows under the owner's frequency on the master.

Version 1 and 2 files load and save as before: nothing is upgraded.

## 13. Limits

| What | At most |
|---|---|
| frequencies | 48 |
| devices | 16 |
| apps | 16 |
| networks | 2: one `hblink`, one `mqtt` |
| named policies | 16 |
| talk paths | 64 |
| ranges in an ID set (plain and `!` together, after merging) | 12 |
| radios in the last-heard table | 256 |
| talkgroups held by `ptt` on a path at once | 4 |
| transceivers the router drives | 4 |
| a name | 39 characters, no `/`, `"` or `\` |

## 14. Later

- **Bridges:** for what policies on paths don't do: TG rewrites (TG 9 on RF is TG 3100 on the network), one call on
  several paths as one (simulcast), and network-to-network links. A `bridges` list of members (path, network or app),
  each with its own TG and activation. Added beside policies, not instead.
- **A system file:** one file for several boxes, with `site` on each device; each box runs what's local to it. Remote
  receivers (`kind: remote`, `via`, `site`) and voting need it.
- **A shared last-heard table** (over MQTT), so private calls find a radio heard at another box.
- **`hears.scan` on an SDR:** scanning frequencies outside its window.
- **`pc_home`:** a path for private calls to a radio never heard here (today: dropped, with `identity: single`).
- **SFR as an app:** the box itself as the SFR repeater (its SDR, or its SDR with another transmitter or a modem),
  beside today's SFR frequency served by a connected radio.
- **An emergency takes a transceiver:** today a transceiver sending one network call isn't taken by an emergency on
  another path (a repeater's slot is).
- **`apps` on SFR / DCDM paths** and the parrot on several transmitters at once.
- **Scheduled talkgroups** (`mode: scheduled`).

## 15. Decisions taken

- Policy on the path, not a route table. The common sites are allow lists, deny lists and private calls on or off per
  path; those are cleanest as a property of the path. The reverse view (§11) answers "where does this TG go".
- Lists with optional names for every section; a frequency's name defaults to its frequency.
- One ID-set syntax for talkgroups and private-call IDs.
- A group call from a network goes to every path that carries it (fan-out). A listed TG beats `*`.
- A private call from a network goes where the radio was last heard; never heard here: dropped. With `per_path`,
  where the master sent it.
- `tx: false` makes a path receive only, with `dir: rf>net`.
- HBlink `identity` defaults to `single`.
- No upgrade: version 1 and 2 files load and save as they are; a version 3 file runs as version 2 roles plus the
  router (§12).
- Apps claim IDs; `apps` on a path says which it may reach.
