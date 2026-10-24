# ESP32 firmware: configuration

Covers: every box-config key the firmware reads, the firmware's own rules, Kconfig, NVS, and settings precedence.
Sources: `common/src/config.c`, `common/include/rdof/config.h`, `sdr/src/config.c`, `common/src/benchcfg.c`, `common/src/ipgwcfg.c`, `esp_idf/main/settings.c`, `Kconfig.projbuild`, component Kconfigs.
Related: [overview.md](overview.md), [interfaces-http.md](interfaces-http.md) (`/api/config`), [radio.md](radio.md), [sdr.md](sdr.md), [../repeater.md](../repeater.md) (`slot_timing`, split), [../bench-api.md](../bench-api.md), [../slot-timing.md](../slot-timing.md).

## 1. Document format

- One JSON document: the box config, plus board keys (`wifi`, `ntp`, `modem`, `uart`, `usb_hub`, `usb`, the `oled` pins). JSONC is taken: `//` and `/* */` comments, trailing commas (`rdof_jsonc_strip`).
- Parser `rdof_jdoc` (`common/src/jsonp.c`): at most `MAX_NODES` 1200 nodes and `STR_BUF` 8192 bytes of strings. Larger documents are refused.
- Two shapes:
  - **Box config**: has `radios`. Roles from `run` (`rdof_box_roles`).
  - **Plain SDR config**: no `radios`; one SDR's settings at the top level (2.9). The board is an SDR only (`rdof_roles_t.sdr_plain`).
- Frequencies: a number below 1e5 is MHz, else Hz (`config.c:box_hz`).
- `${VAR}` values come out as empty strings (no environment).

## 2. Box config keys

Role column: B = board / box, R = roamer, P = repeater, S = SDR, X = `sdr_transceiver`, all = every role.

### 2.1 Top level

| Key | Type | Default | Range / rule | Role | Meaning |
|---|---|---|---|---|---|
| `name` | str | `"radiofoundry"` (`rdof_roles_t.box`); `DEFAULT_JSON` sets `"esp32"` | < 32 chars | B | box name; Wi-Fi hostname; prefix of default radio names |
| `run` | str or [str] | — | ≤ `RDOF_MAX_RADIOS` (2) roamers / repeaters + ≤ 1 SDR; every name must exist in `radios` | B | which radios this board runs. Absent: the first roamer / repeater, else the first SDR |
| `radios` | object name → section | — | types: `sdr`, `repeater`, `sdr_transceiver`, `auto`, `radio_hotspot`, `opengd77`, `mmdvm_hs`, `mmdvm` | B | radio sections |
| `channels` | object number → freq or `{freq, cc, power}` | — | ≤ 64 entries; `cc` falls back to top `cc`, else error; `power` 255 | all | the channel plan |
| `cc` | int | — | 0..15 | all | default color code |
| `location` | `{latitude\|lat, longitude\|lon, height}` | none | lat ±90, lon ±180; height 0, clamped 0..999 | all | default position (a section's own wins) |
| `mqtt` | object | absent = off | §2.6 | all | broker |
| `hblink` | object | §2.7 | | all | HBlink4 master |
| `oled` | object, `true`, or `null`/`false` | on if present (no key: on with `CONFIG_RDOF_OLED_BUILTIN`) | §2.8 | all | display: the SDR's screen, else radio 0's |
| `slot_timing` | object | `free` | [../slot-timing.md](../slot-timing.md) | P, S | UTC slot grid; inherited by a non-SFR repeater on `split` / `auto`, and by the SDR |
| `modem` | `usb`\|`uart` | per radio type | | R, P | default modem port (board key) |
| `uart` | `{tx, rx, num}` | tx 5, rx 6, num 1 | num 1\|2 | R, P | default UART pins (board key) |
| `usb_hub` | bool | false | | B | a hub on the USB port: every USB role runs at once (board key) |
| `wifi` | `{networks: [{ssid, password, priority, hidden}]}` | — | ≤ `WIFILIST_MAX_NETWORKS` 8 | B | moved to wifilist NVS on save; never stored in `rdof/config` |
| `ntp` | `{servers, poll_s}` | — | §5 | B | moved to netclock NVS on save |
| `bench` | `{radios, tx}` | none | §2.10 | B | bench API |
| `ip_gateway` | object, or `false` / `null` | off | §2.13 | B | DMR packet data to and from the IP network ([ipgw.md](ipgw.md)) |
| `selftest_load` | `{channels 8, rates [288000]}` | | rates ≤ 4 | S | self-test load runs (top level or in any section) |

### 2.2 Radio section, common

| Key | Type | Default | Rule | Role |
|---|---|---|---|---|
| `type` | str | — | see 2.1 | all |
| `hblink` | bool | true | false: no peer for this radio | all |
| `location` | object | box's | as 2.1 | all |
| `peer_id` | int | — | required | R, P |
| `callsign` | str | `name[:8]` | ≤ 11 | R, P |
| Default name | | `"{box}-{type}{k+1}"` (k = index) | | all |

### 2.3 Roamer (`radio_hotspot`, `opengd77`, `mmdvm_hs`, `mmdvm`, `auto`)

| Key | Type | Default | Rule |
|---|---|---|---|
| `channels` | `"all"` or [n or `{ch, cc, power}`] | `"all"` | ≤ `RDOF_CFG_CHANNELS` 32; none → error |
| `idle` | plan number or `{freq, cc}` | — | required; added to `channels` if missing |
| `listen_s` | float | 0.3 | |
| `clear_s` | float | 2.0 | |
| `answer_s` | float | 0.75 | |
| `answer_max_s` | float | 5.0 | |
| `idle_after_s` | float | 0 | |
| `jitter_buffer` | `"auto"` or ms | `"auto"` | ms 0..3000, rounded up to whole 60 ms bursts (fixed size); not with `prebuffer` |
| `jitter_min_ms`, `jitter_max_ms` | ms | 180, 480 | auto's bounds; 60 ≤ min ≤ max ≤ 3000, each rounded up to whole 60 ms bursts |
| `prebuffer` | int | 6 | the older fixed size, in bursts; set, it turns auto off; not with `jitter_buffer` |
| `csbk_preambles` | int | 2 | |
| `cwid` | str | `""` | ≤ 40 |
| `cwid_every_s` | float | 900 | |
| `scan` | `true` or `{channels, dwell_s 0.45, hang_s 3.0, carrier_s 0}` | off | dwell > 0; hang ≥ 0; 0 ≤ carrier < dwell; `CONFIG_RDOF_SCAN` |
| covered (derived) | | every SDR section's channel freqs, deduped, sorted | the roamer leaves these to the SDR |

### 2.4 Repeater (`type: repeater`)

| Key | Type | Default | Rule |
|---|---|---|---|
| `mode` | `duplex`\|`sfr`\|`dcdm` | duplex | `dcdm`: dual capacity direct mode, both slots on one frequency, nothing repeated ([../repeater.md](../repeater.md) 13); needs a modem with `DCDM` in its description; `radio.type: split` and `slot_timing: utc` refused |
| `rx_freq`, `tx_freq` | freq | — | required for duplex |
| `freq` | freq | — | required for sfr and dcdm |
| `inbound_slot` | 1\|2 | — | sfr; slot1 = (inbound == 2), slot2 = (inbound == 1) |
| `cc` | int | box `cc`, else error | 0..15 |
| `power` | int | 255 | clamped 0..255 |
| `hang_s` | float | 5.0 | > 0 |
| `call_hang_s` | float | 3.0 | ≥ 0 |
| `cwid_every_s` | float | 900 | |
| `jitter_buffer` | `"auto"` or ms | `"auto"` | ms 0..3000, rounded up to whole 60 ms bursts (fixed size); not with `prebuffer` |
| `jitter_min_ms`, `jitter_max_ms` | ms | 180, 480 | auto's bounds; 60 ≤ min ≤ max ≤ 3000, each rounded up to whole 60 ms bursts |
| `prebuffer` | int | 6 | the older fixed size, in bursts; set, it turns auto off; not with `jitter_buffer` |
| `short_lc_ts1_low` | bool | false | |
| `slots` | [1, 2] | both | |
| `slot_timing` | object | box's (non-SFR, `split`/`auto`) | `utc` refused otherwise; owned by [../repeater.md](../repeater.md) |
| `radio.type` | `auto`, `radio_hotspot` (SFR only), `mmdvm_hs`, `mmdvm`, `split` | auto | `radio_hotspot` refused for duplex |
| oled channel (derived) | | plan number within 100 Hz of `rx_freq` | |

Split (`radio.type: split`): `radio.tx` and `radio.rx` each `{modem usb|uart|sdr (default usb), uart, speed, usb}`; ≤ 1 SDR half, ≤ 1 USB half; two UART halves need different `num` and pins. `radio.power_cap`, `prebuffer`, `lease_s`, `cooldown_s` (0..255) and `max_key_s` (0..600) go into SET_ROLE. Details: [../repeater.md](../repeater.md).

The jitter buffer (both types): with neither `jitter_buffer` nor `prebuffer` set it is auto. Auto sizes the
prebuffer per call from the network's measured lateness, between `jitter_min_ms` and `jitter_max_ms`
([roamer.md](roamer.md), "Jitter buffer"). `jitter_buffer: 300` is a fixed 5 bursts (300 / 60); `310` is 6, since
milliseconds round up to whole bursts. `prebuffer: 4` is the same as `jitter_buffer: 240`. Errors:
`radio: prebuffer and jitter_buffer: set one (prebuffer is a fixed size in bursts)`, `radio: jitter_buffer: "auto" or
milliseconds, 0 to 3000`, `radio: jitter_min_ms and jitter_max_ms: 60 <= min <= max <= 3000`.

### 2.5 Radio settings (R, P)

Read from the section's own keys, else its `radio` object.

| Key | Type | Default | Rule |
|---|---|---|---|
| `rx_invert`, `tx_invert`, `ptt_invert` | bool | false | |
| `tx_delay_ms` | int | 100 | |
| `rx_level`, `tx_level`, `cwid_level` | int | 50 | |
| `rx_dc_offset`, `tx_dc_offset`, `dmr_delay` | int | 0 | |
| `rx_offset_hz`, `tx_offset_hz` | int | 0 | |
| `headers`, `terminators` | int | −1 = kind default (hotspot 1/1, else 8/3) | |
| `tuner` | `"modem"`\|`"fixed"`\|`{type, freq, settle_ms}` | modem | `rigctld`, `command` refused |
| `rssi` | [[raw, dBm], …] | none | ≤ 16 points |
| `protocol` | `uart`\|`udp` | uart | `udp` refused ("not on this board") |
| `modem` | `usb`\|`uart` (`sdr` for X; `split` derived) | box `modem`, else usb for `radio_hotspot`/`auto`, uart otherwise | board key |
| `uart` | `{tx, rx, num}` | box `uart`, else 5 / 6 / 1 | num 1\|2; UART0 is the console |
| `speed` | int | 115200 | |
| `usb` | `{serial, port}` | any | serial ≤ 31; port = hub path `"2"`, `"1.3"` |

### 2.6 `mqtt` (box level; present = on)

| Key | Default | Notes |
|---|---|---|
| `host` | `localhost` | |
| `port` | 1883 | |
| `username` / `password` | `""` | SDR plain config: `user` / `pass` |
| `root` | `DMR` | trailing `/` stripped (SDR) |
| `tls` | false | needs `CONFIG_RDOF_MQTT_TLS` (16mb), else plain with a warning |
| app (derived) | `roaming` / `repeater` / `sdr` | not a key |

Client settings (`net.c`): keepalive 30 s, reconnect 2000 ms, last will QoS 1 retained. Roamer outbox 32 KB, queue 16 KB; SDR outbox 64 KB, queue 32 KB. At most 2 clients (`net.c:client_cfg[2]`). Topics: [mqtt.md](mqtt.md).

### 2.7 `hblink`

| Key | Default | Range | Role |
|---|---|---|---|
| `host` | `localhost` | | all |
| `port` | 62031 | | all |
| `transport` | `udp` | `udp`\|`sctp` (needs `CONFIG_RDOF_HBLINK_SCTP`) | R, P (SDR peers are UDP only) |
| `sctp_encap_port` | 9899 | 1..65535 | R, P |
| `sctp_heartbeat_ms` | 10000 | 0..600000 | R, P |
| `sctp_max_retransmit` | 3 | 1..20 | R, P |
| `sctp_ttl_ms` | 0 | 0..60000 | R, P |

SDR: peers are on when the box `hblink` object is non-empty and the section's `hblink` is not false. Protocol: [hblink.md](hblink.md).

### 2.8 `oled`

| Key | Default | Notes |
|---|---|---|
| `address` | 0x3C | also `i2c.address` |
| `sh1106` | false | also `i2c.sh1106` |
| `rotate180` | false | also `i2c.rotate180` |
| `refresh_s` | 0.5 | |
| `sda` / `scl` | 8 / 9 (`CONFIG_RDOF_OLED_SDA` / `_SCL`; 17 / 18 in `w12`) | GPIOs 0..48, not the same |

Read by `oled_sdr.c:rdof_oled_settings_parse` whatever radios run (a plain SDR config too); `address` is 0x03..0x77. See [device.md](device.md) §2.

### 2.9 SDR section (`type: sdr`)

The SDR in `run`, else the first. Default name `"{box}-sdr{k+1}"`.

| Key | Type | Default | Rule |
|---|---|---|---|
| `channels` | [n or `{ch, id, freq, name, cc, slot, tg, hblink, peer_id}`] | — | ≤ `RDOF_SDR_MAX_CHANNELS` 16; plan entry needs freq and cc; duplicate freq refused |
| `receiver_ids` | fmt | `"SDR{ch:02d}"` | `{ch}` or `{ch:Nd}` |
| `peer_ids` | int | base + plan number | |
| `callsign` | str | `SDR` | |
| `device.sample_rate` | float | 1.536e6 (plain config); `DEFAULT_JSON` 288000 | multiple of 48000 |
| `device.center` | freq or `auto` | auto | auto: `rdof_plan_center`, else best window |
| `device.gain` | dB or `auto` | auto | |
| `device.agc` | truthy or `{min 0, clip 1e-4, headroom_db 6, hold_s 10, relax_s 300, settle_s 0.3}` | off | needs a fixed gain |
| `device.type` | `rtlsdr`\|`lr2021`\|`sx1255`\|`sim` | rtlsdr | build must have it (`sdr_fe_pick`) |
| `device.settings.biastee` | bool | false | |
| `device.settings.iq_bits` | 8 or 16 | the front end's choice | LR2021 only: 16 takes int16 samples in the wide mode (about 50 dB more headroom than int8's 6 bits), up to 240000 S/s; else refused |
| `ppm` | int | 0 | |
| `rssi_offset` | dB | −gain − 25 (gain 30 if auto) | |
| `afc` | bool | true | |
| `status_seconds` | float | 30 | |
| `slot_timing` | object | box's | own over box's |
| `usb.transfers` | int | 8 | board key |
| `usb.transfer_kb` | int | 8 | board key |
| `dsp_cores` | 1\|2 | 1 | ≥ 2 → 2 |
| `spike` | bool | false | USB throughput test at start |
| `selftest` | bool | false | CPU self-test at boot |
| `sim` | bool | false | forces `device.type: sim` |
| `idle_gate` | bool | true | |
| `learn_ppm` | bool | true | |
| `sim_ppm` | float | 0 | with sim |
| `device.args`, `workers` | | | ignored |

Plain SDR config (no `radios`): `name` (`"sdr"`), `device`, `ppm`, `mqtt {host, port, user, pass, root, app "sdr", tls}`, `hblink {enabled false, host, port 62031, callsign SDR, peer_id_base}`, `location`, `defaults` merged into `channels [{id (required), freq, name, cc, slot 1, tg, hblink true, peer_id = base + index}]`, plus the same board-only keys.

### 2.10 `sdr_transceiver`

| Key | Default | Rule |
|---|---|---|
| `duplex` | half | `half` → roamer, `full` → repeater (needs `mode: duplex` or `dcdm`); `mode: dcdm` → the DCDM gateway, half or full |
| `tx_power_dbm` | 10 | within front-end caps |
| radio type | forced `auto`; `modem` forced `sdr` | |
| | | needs an SDR in `radios` and in `run`; front end can TX (full duplex where asked); every frequency within 100 Hz of an SDR channel; TX frequency within caps (`settings.c:resolve`) |

### 2.11 `bench` (`benchcfg.c`)

| Key | Type | Default | Rule |
|---|---|---|---|
| `radios[]` | ≤ 4 | | |
| `.name` | str | — | `[A-Za-z0-9_-]`, < 24, unique |
| `.proto` | `odr-debug`\|`mmdvm` | odr-debug | |
| `.port` (UART) | `{uart 1\|2, tx, rx, rts, cts, baud}` | baud 115200 | 1200..6e6; UART unique |
| `.port` (USB) | `{usb cdc-acm\|uart, vid, pid, serial, usb_port}` | | cdc-acm needs vid/pid; vid 0x0BDA refused; unknown bridge refused |
| `tx.freqs_hz` | [int] | [] | whole Hz, 30e6..3e9, ≤ `RDOF_BENCHTX_MAX_FREQS` |
| `tx.max_key_s` | float | 10 | 0..60 |
| `tx.rf_level` | int | 0 | 0..255 |
| `tx.arm_gpio` | int | −1 | |

### 2.12 Cross-radio checks (`rdof_config_check_ports`, `resolve`)

1. Each `run` radio parses on its own (`rdof_config_parse_radio`).
2. Two radios cannot share the SDR modem, a UART `num`, or any UART pin.
3. Two USB radios each need `usb.serial` or `usb.port`, and the two must not match the same device.
4. Two radios need PSRAM.
5. A role this build doesn't have is refused.
6. The SDR config must parse and fit its front end (`rdof_fe_check`, non-RTL).
7. `sdr_transceiver` rules (2.10).
8. `ip_gateway` parses (§2.13); its `tx` names a roamer or repeater in `run`; on in a build without `CONFIG_RDOF_IPGW` is refused.

### 2.13 `ip_gateway` (`ipgwcfg.c:rdof_ipgw_settings_parse`)

Box level, any roles. Absent, `null` or `false`: off. An object turns it on (`enabled: false` keeps it off); it
needs routes or `nat` ("nothing to do" otherwise). Unknown keys are refused, here and in `routes[]` and `nat`.
Errors start `ip_gateway:` and name the key (`ip_gateway: routes[1].private: a DMR id 1 to 16777215, or a range
"lo-hi"`). Behavior: [ipgw.md](ipgw.md).

| Key | Type | Default | Rule | Meaning |
|---|---|---|---|---|
| `enabled` | bool | true | | |
| `id` | int | 0 | 0..16777215 | the gateway's DMR id: set radios' IP gateway id (RadioFoundry's `ipGatewayId`) to it for off-net traffic; source of replies to talkgroup data (0: the talkgroup) ([ipgw.md](ipgw.md) §6) |
| `radio_net` | CIDR | `12.0.0.0/8` | prefix 8..30, host bits 0 | radio id <-> address (id in the host bits); the fleet's network: only this one is matched, not the id under any first octet |
| `tx` | str | the first in `run` | a roamer or repeater in `run`, < 24 | what transmits replies and ACKs |
| `ack` | bool | true | | answer confirmed data the gateway takes |
| `max_bytes` | int | 1500 | 68..1520 | longest datagram either way |
| `up_pps` | number | 10 | 0..1000 (0: no limit) | datagrams a second to the network; bursts of twice that |
| `down_pps` | number | 2 | 0..1000 | datagrams a second to the radios |
| `idle_s` | number | 120 | 5..86400 | a flow with nothing either way this long closes |
| `max_flows` | int | 8 | 1..16, cut to `CONFIG_RDOF_IPGW_MAX_SOCKETS` | flows (one UDP socket each) |
| `routes` | [route] | [] | ≤ 8 | tried in order; the first match wins |
| `nat` | bool or `{enabled, allow, deny}` | off | `true` = on with the defaults | NAT for UDP (and ICMP echo) not routed |

Route:

| Key | Type | Default | Rule |
|---|---|---|---|
| `name` | str | `route{n}` | < 24, unique |
| `all` | bool | false | alone: not with the three below |
| `private` | id or `"lo-hi"` | | data to a radio id (individual); not with `group` |
| `group` | id or `"lo-hi"` | | data to a talkgroup |
| `from` | id or `"lo-hi"` | | the source radio; with `private` / `group`, both must match |
| `to` | `"host:port"` or `"host"` | required | an IPv4 address (not 0.0.0.0, not 224.0.0.0 and up) or a host name (looked up on the board); no port: each datagram's own |
| `payload` | `udp` \| `ip` | `udp` | `udp`: the UDP payload (other protocols dropped); `ip`: the whole datagram, any protocol (needs a port) |

One of `all`, `private`, `group` or `from` is required. Ids are 1..16777215; a range's low end is at most its high.

`nat` object:

| Key | Type | Default | Rule |
|---|---|---|---|
| `enabled` | bool | true | |
| `allow` | [CIDR] | [] (any) | ≤ 8 |
| `deny` | [CIDR] | 10/8, 172.16/12, 192.168/16, 100.64/10, 13/8 | ≤ 8; replaces the defaults (`[]`: none) |

Always denied to NAT, whatever the lists: 0/8, 127/8, 169.254/16, 224/4, 240/4, and `radio_net`.

```jsonc
"ip_gateway": {
  "id": 9990,
  "routes": [
    {"name": "ars", "private": 9990, "to": "192.0.2.20:4005"},        // ARS to the dispatch server
    {"name": "tms", "private": "9991-9999", "to": "192.0.2.20"},      // keep each datagram's port
    {"name": "fleet", "from": "9995000-9995999", "to": "gw.example.com:7000", "payload": "ip"}
  ],
  "nat": {"allow": ["0.0.0.0/0"]}                                       // the rest of UDP to the Internet
}
```

## 3. Firmware rules

Points a box config written for another consumer may expect differently:

| Point | Firmware |
|---|---|
| Radios a board | 2 roamers / repeaters + 1 SDR |
| Box name default | `radiofoundry` (box), `sdr` (plain), `esp32` (`DEFAULT_JSON`) |
| Duplicate names, peer / receiver ids | not checked |
| `sdr_transceiver.sdr` | ignored; any SDR in `radios` |
| Roamer `hblink.transport` | `udp` or `sctp` |
| `${VAR}` | empty (no environment) |
| `protocol: udp`, `rigctld`, `command` tuners | refused |
| SDR `workers`, `args`, `file`, `antenna`, `cf32`, `hblink.transport` | ignored; SDR peers UDP |
| Channels outside the band, center auto | best window picked |
| Plain config `master_host` default | `localhost` |
| Default receiver name | the section name |
| `oled` | `sda` / `scl` GPIOs |
| Split SDR half | refused for now |

## 4. Kconfig

### 4.1 `main/Kconfig.projbuild` ("RadioFoundry Gateway")

| Option | Type | Default | Range |
|---|---|---|---|
| `RDOF_ROAMER` | bool | y | |
| `RDOF_SDR` | bool | y | |
| `RDOF_MQTT` | bool | y | |
| `RDOF_MQTT_RAW` | bool | y | |
| `RDOF_MQTT_TLS` | bool | n | selects CA bundle |
| `RDOF_WEB_FULL` | bool | y | |
| `RDOF_OTA` | bool | y | |
| `RDOF_SOFTAP` | bool | y | |
| `RDOF_STATUS_LED_GPIO` | int | −1 | −1..48 |
| `RDOF_GNSS_EN_GPIO` | int | −1 | −1..48 |
| `RDOF_GNSS_EN_LEVEL` | int | 0 | 0..1 |
| `RDOF_LOG_RING_KB` | int | 16 | 0..1024 |
| `RDOF_CRASH_TEST` | bool | n | `POST /api/crash` (debug builds only; needs `ESP_COREDUMP_ENABLE_TO_FLASH`) |
| `RDOF_MODEM_USB` | bool | y | |
| `RDOF_USB_VID` | hex | 0x1FC9 | |
| `RDOF_USB_PID` | hex | 0x0094 | |
| `RDOF_USB_INTERFACE` | int | 0 | |
| `RDOF_MODEM_UART` | bool | y | |
| `RDOF_MEM_MARGIN_KB` | int | 80 | 8..200; internal RAM the memory plan keeps free ([memory.md](memory.md#4-the-margin)) |
| `RDOF_MEM_STRICT` | bool | n | a role over the margin is refused |
| `RDOF_IPGW` | bool | y | needs `RDOF_FEATURE_DATA_BLOCKS` and `_TRACKER`; `ipgw_task.c` |
| `RDOF_IPGW_MAX_SOCKETS` | int | 8 | 1..16 |
| `RDOF_HBLINK_SCTP` | bool | y | |
| `RDOF_SCAN` | bool | y | |
| `RDOF_OLED` | bool | y | depends on `RDOF_ROAMER` or `RDOF_SDR` |
| `RDOF_OLED_BUILTIN` | bool | n | y in `w12`: on with no `oled` key |
| `RDOF_OLED_SDA` / `_SCL` | int | 8 / 9 | 17 / 18 in `w12` |
| `RDOF_OLED_POWER_GPIO` / `_LEVEL` | int | -1 / 0 | 45 / 0 in `w12` (VEXT_EN) |
| `RDOF_SDR_HBLINK` | bool | y | |
| `RDOF_SDR_FAST` | bool | y | |
| `RDOF_SDR_ESP_DSP` | bool | y | |
| `RDOF_SDR_TWO_CORES` | bool | y | |
| `RDOF_SDR_HELPER_STACK` | int | 6144 | 3072..16384 |
| `RDOF_SDR_CHUNK` | int | 4096 | 1024..32768 |
| `RDOF_SDR_RING_KB` | int | 256 | 32..1536 |
| `RDOF_SDR_INTERNAL_RESERVE_KB` | int | 40 | 0..256 |
| `RDOF_SDR_SELFTEST` | bool | y | |
| `RDOF_SDR_SPIKE` | bool | y | |
| `RDOF_BENCH` | bool | y | depends on `RDOF_SDR` |
| `RDOF_BENCH_TX` | bool | y | |
| `RDOF_SDR_LR2021` | bool | n | |

LR2021 submenu (`RDOF_LR2021_*`):

| Option | Default | Range |
|---|---|---|
| `SPI_HOST` | 1 | 1..2 |
| `SPI_HZ` | 16000000 | |
| `NSS` / `SCK` / `MOSI` / `MISO` | 8 / 9 / 10 / 11 | |
| `NRESET` / `BUSY` / `IRQ` | 12 / 13 / 7 | |
| `PA_EN_GPIO` | 4 | |
| `TCXO_TUNE` | −1 | −1..7 |
| `RX_BOOST` | 0 | 0..7 |
| `PA_LO_MHZ` / `PA_HI_MHZ` | 860 / 930 | |
| `DIO9_PA` / `DIO10_PA` / `DIO11_PA` | 0x04 / 0x06 / 0x06 | |
| `DIO9_BYPASS` / `DIO10_BYPASS` / `DIO11_BYPASS` | 0x04 / 0x00 / 0x06 | |

### 4.2 Portable-core and component Kconfig

| File | Option | Default |
|---|---|---|
| `common/Kconfig` | `RDOF_FEATURE_TX`, `RDOF_FEATURE_DATA_BLOCKS`, `RDOF_FEATURE_TRACKER`, `RDOF_FEATURE_REPEATER` (needs TX) | y |
| `sdr/Kconfig` | `RDOF_FEATURE_SDR`, `RDOF_SDR_AGC`, `RDOF_SDR_BUILTIN_FFT`, `RDOF_SDR_RECEIVER` (needs TRACKER) | y |
| | `RDOF_SDR_DSP_IRAM` | n (~33 KB IRAM) |
| `rdof/sdr/features.h` | `RDOF_SDR_FAST_ATAN2` | 1 |
| netclock | `DEFAULT_SERVERS` | `"pool.ntp.org"` |
| | `POLL_S` | 300 (16..3600) |
| | `POLL_FAST_S` | 120 |
| | `START_ON_IP`, `SET_SYSTEM_TIME`, `DIE_TEMP`, `HTTP` | y |
| | `PPS_GPIO` / `PPS_FALLING` | −1 / n |
| | `GNSS_EVERY` / `GNSS_LAG_MS` | 16 / 0 |
| | `HTTP_BASE` / `NVS_NAMESPACE` | `/api` / `netclock` |
| | `TASK_PRIORITY` / `TASK_CORE` | 8 / 0 |
| gnss | `UART_RX_GPIO` / `UART_TX_GPIO` | −1 / −1 (39 / 38 in `w12`) |
| | `UART_NUM` / `BAUD` / `AUTODETECT` | 1 / 9600 / y |
| | `TASK_PRIORITY` / `TASK_CORE` | 9 / 0 |
| wifilist | `MAX_NETWORKS` | 8 (1..32) |
| | `CONNECT_TIMEOUT_S` / `RESCAN_MAX_S` | 15 / 30 |
| | `SOFTAP`, `HTTP` | y |
| | `HTTP_BASE` / `NVS_NAMESPACE` / `TASK_PRIORITY` | `/api` / `wifilist` / 5 |
| tinysctp | `MAX_STREAMS` / `RX_WINDOW` / `TASK_STACK` / `TASK_PRIORITY` | 8 / 64 / 4096 / 8 |

Profile values (`sdkconfig.defaults.*`) are in [overview.md §3](overview.md#3-profiles-and-overlays).

## 5. NVS

| Namespace | Key | Type | Content | Written by / when |
|---|---|---|---|---|
| `rd` | `config` | blob | box JSON text, `wifi` / `ntp` removed | `settings.c:settings_save` on POST `/api/config` (then restart) |
| `rd` | `sdr` | blob | legacy SDR-only firmware's JSON | read when `config` is absent; erased on the next store |
| `rd` | `selftest` | u8 | one-shot flag | set by POST `/api/selftest`; taken (erased) at boot by `sdr.c:sdr_boot_selftest` |
| `rd` | `spike` | u8 | one-shot flag | set by POST `/api/spike`; taken at `sdr_role_start` |
| `rdof_ppm` | `learned` | blob `{char serial[64]; float ppm}` | legacy | read only |
| `rdof_ppm` | `cal` | blob `{char serial[64]; nc_tcal_t cal}` | dongle's ppm by die temperature | `sdr_task.c` when settled; a bin again at most every `CAL_AGAIN_S` 7200 s; sampled every `PPM_EVERY_US` 5 s; follows ppm within `CAL_FOLLOW_PPM` 0.25 |
| `netclock` | `servers` | str | ≤ `NETCLOCK_SERVERS_LEN` 128 | settings / `/api/clock/settings` |
| `netclock` | `poll_s` | u32 | 16..3600 | same |
| `netclock` | `rate_ppb` | i32 | board crystal rate | after ≥ `NC_RATE_SAVE_MIN_N` 8 refs, moved ≥ 0.3 ppm, ≤ once / 3600 s |
| `netclock` | `tcal` | blob `nc_tcal_t` | `NC_TCAL_N` 24 bins of 2 °C, version 1 | when a bin is learned, ≤ every `NC_TCAL_AGAIN_S` 7200 s |
| `wifilist` | `nets` | blob | byte 0 version 1; byte 1 count; then `rec_t {char ssid[33]; char password[65]; int8 priority; uint8 flags (bit0 hidden)}` × count | settings, `/api/wifi/networks` |

An NVS partition with no free pages or a new version is erased at boot (`main.c`), losing every key above.

## 6. Precedence

| Layer | What it sets | Wins over |
|---|---|---|
| Build (Kconfig / profile) | features, pins, sizes, USB VID/PID, defaults of components | nothing at run time; a setting needing a missing feature is refused (role) or warned (TLS) |
| `DEFAULT_JSON` (`settings.c`) | box config when NVS has none or the stored one fails | — |
| NVS `rdof/config` | box config | `DEFAULT_JSON` |
| NVS component namespaces | Wi-Fi list, NTP, clock rate, calibrations | `wifi` / `ntp` in an old stored config (migrated only when the component has nothing stored) |
| Config upload (POST `/api/config`) | whole box config; its `wifi` / `ntp` replace the components' | everything stored; checked by `resolve` first, refused whole on error, restart after |
| Web component pages (`/api/wifi/*`, `/api/clock/settings`) | their component's NVS directly | the last upload, for that component |
| One-shot flags | self-test / spike on next boot | `selftest` / `spike` keys (either turns it on) |

`DEFAULT_JSON`: name `esp32`; mqtt `192.0.2.115:1883` user `radios`; hblink `192.0.2.115:62031`; cc 2; channels 1 = 461.6875, 13 = 461.0375, 4 = 461.0875, 6 = 461.1375, 8 = 461.1875, 10 = 461.2375 MHz; run `["ROAM1", "s3-sdr"]` (one name in a one-role build); `ROAM1` (`radio_hotspot`, peer 9990301, callsign RT01, idle 1, channels all); `s3-sdr` (sample_rate 288000, gain 30, hblink false, peer_ids 9990100, channels [13, 4, 6, 8, 10]).

Export (GET `/api/config`) is the stored text with `wifi` (no passwords) and `ntp` added back (`rdof_jsonc_add`).
