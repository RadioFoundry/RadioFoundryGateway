# esp_idf HTTP interface

Covers: every HTTP endpoint the board serves (pages, status, settings, maintenance, clock, Wi-Fi), the `/api/status` schema, the web page.
Sources: `esp_idf/main/web.c`, `heard.c`, `common/src/lastheard.c`, `settings.c`, `sdr.c`, `roamer.c`, `logring.c`, `fe_rtlsdr.c`, `web/index.html`, `web/mini.html`; `components/netclock/src/netclock_http.c`, `components/wifilist/src/wifilist_http.c`.
Related: [bench-api.md](../bench-api.md) (`/api/bench/v1`), [mqtt.md](mqtt.md), [hblink.md](hblink.md), [config.md](config.md) (the settings JSON), [netclock](../../../components/netclock/README.md), [wifilist](../../../components/wifilist/README.md).

## 1. Server

| Item | Value | Source |
|---|---|---|
| Server | ESP-IDF `esp_http_server`, plain HTTP on port 80 | `web.c:web_start` |
| URI match | `bench_uri_match` (a `*` matches one path segment) with `CONFIG_RDOF_BENCH`, else `httpd_uri_match_wildcard` | `web.c:web_start` |
| `max_uri_handlers` | 48 | `web.c:web_start` |
| `max_open_sockets` | 8, `lru_purge_enable` | `web.c:web_start` |
| Task | stack 8192, core 0 | `web.c:web_start` |
| Auth | none, on any endpoint. The board sits on a private network. | |
| TLS | none | |
| Errors | `httpd_resp_send_err`: status + `text/plain` body (the text in the tables below) | `web.c` |
| Out of memory | 500 `no memory` on any handler that allocates | `web.c` |

The setup access point is `radiofoundry-<suffix>` (format `"radiofoundry-%s"`), password `radiofoundry`, page at
`http://192.168.4.1/`. It comes up after 20 s with no station connection (`wifi.c:wifi_start`, `ap_after_ms = 20000`,
`CONFIG_RDOF_SOFTAP`). The station keeps trying meanwhile. Every endpoint is the same on both interfaces.

## 2. Endpoints

| Method | Path | Built when | Request | Response | Side effects |
|---|---|---|---|---|---|
| GET | `/` | always | none | the page, gzipped (`Content-Encoding: gzip`, `text/html`): `index.html` with `CONFIG_RDOF_WEB_FULL` (default y), else `mini.html` | none |
| GET | `/api/status` | always | none | `application/json`, `Cache-Control: no-store`, at most 6144 bytes (§3) | none |
| GET | `/api/config` | always | `?download` (any value) adds `Content-Disposition: attachment; filename="<box>.json"` | `application/json`: `settings_export()`, the stored JSONC text with `wifi` and `ntp` added (§4) | none |
| POST | `/api/config` | always | body: the settings JSON(C), 1 to 16384 bytes (`web.c:MAX_CONFIG`) | 200 `saved; restarting` | stores the settings in NVS, then restarts after 1 s (`web.c:restart_task`, `vTaskDelay(1000 ms)`) |
| GET | `/api/resolved` | always | none | `text/plain`, at most 5120 bytes (§5) | none |
| GET | `/api/log` | always | none | `text/plain`, `no-store`: the log ring, oldest first, the last `CONFIG_RDOF_LOG_RING_KB` KB (default 16; 0 = off, empty body) | none |
| GET | `/api/lastheard` | always | `?since=N` (u32, default 0) | JSON (§7a): the recent calls changed after N, newest first | none |
| GET | `/api/tasks` | `CONFIG_FREERTOS_USE_TRACE_FACILITY` and `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` | none | `text/plain`, `no-store`: the seconds since the last ask, then a line a task: name, core (0, 1 or any), priority and its share of a core in % since the last ask (the first ask after a restart shows 0) | none |
| GET | `/api/coredump` | always | none | `application/octet-stream`, `no-store`, `Content-Disposition: attachment; filename="<box>-coredump-<elf>.bin"`: the core dump as stored in the `coredump` partition (header, ELF core, CRC; `esp-coredump -t raw` reads it), or 404 `no core dump` (`no core dump (not in this build)` without `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`) | none |
| DELETE | `/api/coredump` | `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH` | none | 200 `erased`, or 500 with the error's name | erases the partition (`esp_core_dump_image_erase`); `crash` leaves `/api/status` |
| POST | `/api/crash` | `CONFIG_RDOF_CRASH_TEST` (default n; debug builds only) | none | 200 `crashing: the board saves a core dump and restarts` | 500 ms later a task `crash_test` stores to address 0 (StoreProhibited): core dump, reboot |
| POST | `/api/restart` | always | none (ignored) | 200 `restarting` | restart after 1 s |
| POST | `/api/selftest` | `CONFIG_RDOF_SDR` | none | 200 `restarting to run the self-test (about 20 s; then see the log)` | NVS `rd`/`selftest` = 1 (u8), restart after 1 s; the boot runs the DSP self-test and clears the flag; the verdict is `/api/status` `selftest` and the log |
| POST | `/api/spike` | `CONFIG_RDOF_SDR` | none | 200 `restarting to run the USB spike once the RTL-SDR is in (about a minute; see the log)` | NVS `rd`/`spike` = 1 (u8), restart after 1 s; results in `/api/status` `spike` and the log |
| GET | `/api/spectrum` | `CONFIG_RDOF_SDR` | `?since=N` (u32, default 0) | JSON (§6) | keeps the spectrum computed for 10 s (`sdr_task.c:sdr_spectrum_rows`, `spec_want_until`) |
| GET | `/api/rtl/regs` | `CONFIG_RDOF_SDR` | none | JSON (§7), or 404 `no dongle` | reads the tuner over I2C under the receiver lock |
| POST | `/api/ota` | `CONFIG_RDOF_OTA` (default y) | body: the app image (`.bin`), any length | 200 `updated; restarting` | writes the next OTA partition, sets it to boot, restart after 1 s |
| any | `/api/bench/v1/*` | `CONFIG_RDOF_BENCH` | see [bench-api.md](../bench-api.md) | | |
| GET | `/api/clock` | always (netclock) | none | JSON (§8.1) | none |
| POST | `/api/time` | always (netclock) | `{"utc_ms": n}` | `{"applied": bool, "source": "<clock source>"}`; 400 if the body is too long | sets the clock from the browser when `n > 1.6e12` and no better source holds it |
| GET | `/api/clock/settings` | always (netclock) | none | `{"servers": [...], "poll_s": n}` | none |
| POST | `/api/clock/settings` | always (netclock) | the same object, or a string | the settings; 400 + error text | stores them (netclock's NVS namespace); takes effect at once |
| GET | `/netclock.js` | always (netclock) | none | gzipped JS, `no-cache` | none |
| GET | `/api/wifi` | always (wifilist) | none | JSON (§8.2) | none |
| POST | `/api/wifi/networks` | always (wifilist) | 1 to 4096 bytes (§8.2) | the `/api/wifi` status; 400 + error text | stores the list (wifilist's NVS namespace), reconnects if the current network went |
| POST | `/api/wifi/scan` | always (wifilist) | none | `{"scanning": true}` | starts a scan; results in `/api/wifi` `seen` |
| GET | `/wifilist.js` | always (wifilist) | none | gzipped JS | none |

The netclock and wifilist paths sit under `CONFIG_NETCLOCK_HTTP_BASE` / `CONFIG_WIFILIST_HTTP_BASE` (both default
`/api`).

### Errors by endpoint

| Endpoint | Status | Body | When |
|---|---|---|---|
| POST `/api/config` | 400 | `settings: 1 to 16384 bytes of JSON` | empty body or more than `MAX_CONFIG` |
| POST `/api/config` | 500 | `read failed` | socket read error or short body |
| POST `/api/config` | 400 | the parser's or resolver's error text | `settings_save` refused it: nothing stored, no restart |
| POST `/api/ota` | 500 | `no OTA partition` | no next OTA partition |
| POST `/api/ota` | 500 | `write failed` | read or flash write error |
| POST `/api/ota` | 500 | `image rejected` | `esp_ota_end` or `esp_ota_set_boot_partition` failed (bad image) |
| GET `/api/rtl/regs` | 404 | `no dongle` | no RTL-SDR open |

## 3. `GET /api/status`

One JSON object, built by `web.c` with `rdof_json` (cap 14336). Non-finite numbers are `null`.

### 3.1 Board

| Field | Type | Unit | Meaning | Present |
|---|---|---|---|---|
| `name` | string | | the box name (`g_roles.box`) | always |
| `version` | string | | the app version (`git describe`) | always |
| `idf` | string | | ESP-IDF version | always |
| `uptime_s` | int | s | since boot | always |
| `reset` | string | | last reset: `power on`, `reset pin`, `restart`, `panic`, `interrupt watchdog`, `task watchdog`, `watchdog`, `deep sleep`, `brownout`, `sdio`, `usb`, `jtag`, `unknown` | always |
| `crash` | object | | the core dump in flash (the last crash, maybe of an earlier image): `size` (bytes); then either `error` (the dump fails its check: the IDF error's name) or `elf` (the start of the crashed image's ELF SHA-256, hex), `this_image` (bool: the running image), `task` (string), `pc` (`"0x%08lx"`), `exccause` (int: EXCCAUSE, or the IDF's own causes from 64), `cause` (its name: `LoadProhibited`, `StoreProhibited`, `CacheError`, ...; `unknown`), `excvaddr` (`"0x%08lx"`), `backtrace` (space-separated hex, up to 16), `backtrace_corrupted` (bool), `details` (abort()'s or the task watchdog's message; only when there is one) | a core dump exists (`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`, ELF) |
| `heap_free` | int | bytes | internal heap free | always |
| `psram_free` | int | bytes | PSRAM free | always |
| `cpu` | array of number | fraction | each core's busy share (1 less its idle task's), core 0 first, over at least the last 2 s | built with the FreeRTOS run-time stats (`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS`, on in `sdkconfig.defaults`); omitted on the first ask |
| `memory` | object | | the memory plan (§3.5) | always |
| `settings_ok` | bool | | the stored settings parsed and resolved; false: the board runs `DEFAULT_JSON` | always |
| `settings_error` | string | | why not (empty if ok) | always |
| `wifi` | bool | | station has an IP | always |
| `ap_mode` | bool | | setup access point up | always |
| `ip` | string | | station IP, `""` if none | always |
| `clock` | string | | netclock source: `ntp`, `browser`, `pps`, `gnss`, `none` | always |
| `gnss` | object | | the GNSS receiver: fix, position, satellites, counters ([components/gnss/README.md](../../../components/gnss/README.md), "Status") | `CONFIG_GNSS_UART_RX_GPIO` ≥ 0 |
| `usb_either` | bool | | both roles want the one USB port: whichever device is plugged in decides | always |
| `build_roamer` | bool | | built with `CONFIG_RDOF_ROAMER` | always |
| `build_sdr` | bool | | built with `CONFIG_RDOF_SDR` | `CONFIG_RDOF_SDR` |
| `selftest` | string | | the last self-test's verdict (≤ 200 chars) | `CONFIG_RDOF_SDR` |
| `spike` | string | | the last USB spike's results (≤ 384 chars) | `CONFIG_RDOF_SDR` |
| `roamer` | object | | the first radio (§3.2) | a radio is configured |
| `roamers` | array | | every radio (§3.2), the first again included | 2 or more radios started |
| `sdr` | object | | the SDR (§3.3) | the SDR role is configured |

### 3.2 `roamer` / `roamers[]` (`roamer.c:one_json`)

When no radio has started (both roles on USB and the SDR is plugged in), `roamer` shows the first radio's settings
with its counters at zero.

| Field | Type | Unit | Meaning | Present |
|---|---|---|---|---|
| `name` | string | | the radio's name | always |
| `mode` | string | | `roamer`, `repeater`, `sfr repeater` | always |
| `started` | bool | | its tasks run (false: not started, see `radio_error`) | always |
| `port` | string | | `usb`, `uart`, `sdr`, `split` | always |
| `split` | object | | `{ready, epoch, tx, rx, dropped, failed}`, owned by the split spec | port `split` |
| `modem_open` | bool | | the modem's port is open | always |
| `radio_ready` | bool | | the modem answered and is configured | always |
| `radio_error` | string | | the last radio error, `""` if none | always |
| `modem` | string | | the modem's description (`GET_VERSION`) | always |
| `protocol` | int | | MMDVM protocol version (1 or 2) | always |
| `radio` | string | | the radio kind, `""` until known | always |
| `requests` | int | | modem requests sent | always |
| `no_reply` | int | | requests with no reply | always |
| `refused` | int | | requests NAKed | always |
| `bursts_heard` | int | | bursts from the modem | always |
| `state` | string | | the transceiver's state name | roamer, TX build |
| `listening` | bool | | parked on the idle channel | roamer, TX build |
| `scanning` | bool | | scanning its list | roamer, TX build |
| `status_misses` | int | | modem status polls missed | roamer, TX build |
| `queue_dropped` | int | | network bursts dropped from the call queue | roamer, TX build |
| `keyed` | bool | | transmitter keyed | repeater build |
| `busy1`, `busy2` | bool | | slot busy | repeater build |
| `dropped` | int | | bursts dropped | repeater build |
| `jitter` | object | | the jitter buffer for network voice: `mode` (`auto` or `fixed`), `target_ms` (the prebuffer now, or the last call's; fixed: the setting), `jitter_ms` (RFC 3550 interarrival jitter), `late_ms` (the arrival window's second worst lateness), `min_ms` and `max_ms` (auto only), and counts since boot `streams`, `underruns`, `grown`, `shrunk`; [roamer.md](roamer.md), "Jitter buffer" | roamer (TX build) and repeater |
| `frequency` | int | Hz | where it is now (repeater: RX frequency) | always |
| `colorCode` | int | | its color code now | always |
| `hblink` | bool | | HBlink4 link up (a DMRP within 3 keepalives) | always |
| `hblink_tx`, `hblink_rx` | int | packets | to and from HBlink4 | always |
| `sctp` | object | | tinysctp association: `state, srtt_ms, rto_ms, cwnd, flight, peer_rwnd, queued, outstanding, errors, prsctp, streams[2], pkts_in, pkts_out, msgs_sent, msgs_received, retransmits, fast_retransmits, t3_expiries, abandoned, dropped_unsent, fwd_tsn_sent, fwd_tsn_received, dups, bad_checksum, hb_sent, hb_acked, established, restarts, failures, aborts, send_rejected` | `CONFIG_RDOF_HBLINK_SCTP` and transport `sctp` |
| `mqtt` | bool | | its MQTT connection is up | always |
| `heard` | string | | the call being heard, `""` if none | always |
| `sending` | string | | the call on air, `""` if none | always |

### 3.3 `sdr` (`sdr.c:sdr_status_json`)

| Field | Type | Unit | Meaning | Present |
|---|---|---|---|---|
| `name` | string | | the SDR's name | always |
| `started` | bool | | the role started | always |
| `rtl_open` | bool | | the front end is open (named for the RTL-SDR, true for any front end) | always |
| `sim` | bool | | the simulated front end | always |
| `frontend` | string | | front end name | always |
| `rtl` | string | | front end description | always |
| `rtl_error` | string | | last front end error | always |
| `receiving` | bool | | samples arriving | always |
| `sample_rate` | number | S/s | wideband rate | always |
| `center_hz` | number | Hz | tuner center | always |
| `gain_db` | number/null | dB | null: tuner AGC | always |
| `cpu_load` | number | 0–1+ | DSP time / wall time, last period | always |
| `dsp_split` | int | | channels on core 1 with `dsp_cores` 2; -1 one core | always |
| `dsp_stack_free`, `helper_stack_free` | int | bytes | stack high-water marks | always |
| `hot_arena` | int | bytes | the plan's hot arena in internal RAM, as much as was spare on a board with PSRAM: the receiver's per-sample buffers and esp-dsp's plans and filter states ([memory.md](memory.md#5-the-sdrs-hot-arena)); 0 not planned | always |
| `hot_arena_used` | int | bytes | of it, taken by the receiver now | always |
| `hot_arena_spill` | int | bytes | taken past it, in the arena's PSRAM part (`hot_ps`); past that, the heap | always |
| `cpu_stages` | object | 0–1 | `{input, channelize, demod, track}` | always |
| `demod_steps` | object | 0–1 | `{fm, search, flywheel, history}` | always |
| `chz_steps` | object | 0–1 | the channelizer on one core, in cycles over the period: `{fft, copy, noise, channels}` (the forward FFTs, the input copy and window move, the noise floor, the channels' bins, filter, inverse FFT and NCO) | always |
| `dsp_buffers` | string | | where the receiver's buffers are, each `internal` or `PSRAM` (`y PSRAM 21 KB, win PSRAM 4 KB, ...`) | the receiver is up |
| `usb_bytes_per_s` | number/null | B/s | measured since the last call (null on the first) | always |
| `usb_expected_per_s` | number | B/s | sample rate × bytes per sample of the front end's format (`rdof_fe_sample_bytes`; 2 for cu8 I/Q) | always |
| `usb_errors`, `usb_short` | int | | transfer errors, short transfers | always |
| `ring_overflows`, `bytes_lost`, `bytes_filled` | int | | sample ring overflows, bytes lost, filler bytes put in their place | always |
| `stream_restarts` | int | | stream stalled and restarted | always |
| `ppm` | object | | the frequency correction (below) | started |
| `ring_peak` | int | bytes | ring high-water | always |
| `mqtt` | bool | | MQTT up | always |
| `mqtt_dropped` | int | | posts dropped (`net_mqtt_t.dropped`) | always |
| `hblink_peers`, `hblink_up` | int | | peers configured, peers up | always |
| `esp_dsp_fft`, `esp_dsp_fir` | bool | | built with esp-dsp's FFT / FIR | always |
| `channels` | array | | `{id, name, freq (Hz, %.0f), in (in the tuner window), bursts, rssi (dBm int; 0 when none yet), afc (Hz, %.1f), idle (% of chunks the idle gate skipped, %.0f)}`; out of the window: bursts 0, rssi 0, afc 0, idle 0 | always |

`ppm`: `learning` (bool), `rtl` (configured ppm), `learned` (ppm), `by` (`clock` or `afc`), `channels`, `settled`,
`die_c`, `die_spread`, `die_tol`, `expected`, `table` (`[{c, ppm, n}]`, the temperature table), `steady_s`, `saved`,
`sim` (if set), and `clock` when the sample clock is on: `{ppm, err, trusted, raw, span_s, rms_us, steps, spread,
spread_tol, board, board_learned}`. The SDR spec ([sdr.md](sdr.md)) defines these.

### 3.4 `ip_gateway` (`ipgw.c:rdof_ipgw_status_json`)

Present when the IP gateway runs ([ipgw.md](ipgw.md)). Counters since boot, all int.

| Field | Meaning |
|---|---|
| `routes`, `nat`, `flows`, `max_flows` | routes configured, NAT on, flows open, the cap |
| `rx_messages` | packet data messages put together with good CRCs |
| `rx_bad`, `rx_incomplete`, `rx_dup` | a block, CRC-9 or CRC-32 failed; blocks missing; the same message again (or our own heard back) |
| `not_ip`, `bad_ip`, `fragment`, `too_big` | not SAP 4 / SAP 3 / IPv4; header lengths or checksum wrong; IPv4 fragments; past `max_bytes` |
| `not_udp`, `no_route`, `denied` | another protocol where UDP is needed; no route and NAT off; NAT destination denied |
| `up_rate`, `flows_full`, `unresolved`, `send_failed` | over `up_pps`; no flow or socket free; a route's name not looked up yet; `sendto` failed |
| `up_packets`, `up_bytes` | datagrams (payload octets) sent to the network |
| `down_packets`, `down_bytes` | datagrams (whole IPv4 octets) sent down to radios |
| `down_rate`, `down_too_big`, `down_stranger`, `down_bad_ip` | over `down_pps`; past `max_bytes`; from an address the flow didn't send to; payload `ip` not IPv4 or its checksum wrong |
| `no_tx` | replies or ACKs with nothing to transmit them (or it refused) |
| `acks` | confirmed data answered |
| `flows_opened`, `flows_expired` | flows opened; closed idle |
| `queue_full` | data bursts the receivers couldn't queue for the gateway |
| `sap3` | SAP 3 (compressed UDP/IPv4 header) messages expanded |
| `ttl_expired`, `down_ttl_expired` | up: TTL 1 or 0 where the gateway forwards; down: a whole datagram or ICMP message with TTL 1 or 0 |
| `icmp_up`, `icmp_down` | NAT'd echo requests sent; echo replies sent down |
| `icmp_errors`, `icmp_unmatched` | ICMP errors for a flow sent down, rewritten; ICMP that's no flow's |
| `down_own_source` | down datagrams from the radio's own address (it would drop them), not sent |
| `tx_busy` | down packets dropped: the repeater's slot stayed busy 2 s |

### 3.5 `memory` (`mem.c:mem_status_json`, `memplan.c:rdof_memplan_json`)

The boot memory plan and the heap now ([memory.md](memory.md)). Sizes in bytes.

| Field | Type | Meaning |
|---|---|---|
| `planned` | bool | a plan was made (false: nothing below but the heap) |
| `ok` | bool | every role's claims fit with the margin kept |
| `error` | string | why not (empty if ok) |
| `margin` | int | internal RAM kept for Wi-Fi, lwIP and what isn't planned (`CONFIG_RDOF_MEM_MARGIN_KB`) |
| `strict` | bool | a role over the margin is refused (`CONFIG_RDOF_MEM_STRICT`), else started with `error` set |
| `spare` | int | internal free at plan time less the plan and the margin (negative: short) |
| `internal` | object | `free`, `largest` (now); `at_plan` (free when planned); `planned` (allocated by the plan); `budget` (declared, allocated by others later); `outside_plan` (`at_plan` − `planned` − `free`: taken since by the budgets' owners, Wi-Fi, lwIP and what isn't planned; compare with `budget` + `margin`) |
| `dma` | object | `free`, `largest`: internal DMA-capable now |
| `psram` | object | `free`, `largest` now; `planned` |
| `roles` | object | keyed by role (`sdr`, `sdr_fe`, `sdr_net`, `radio1`, `radio2`, `ipgw`, `usb`, `board`): `state` (`planned`, `refused`, `released`), `why` (refused or released: why), `internal`, `dma`, `psram` (bytes planned in each), `budget`, `claims` |
| `roles.*.claims` | object | keyed by claim name: `bytes` (each × count), `in` (`internal`, `dma`, `psram`, `nowhere`), `how` (`alloc`, `hold`, `budget`), `align` (only when over 4), `taken` (alloc: the role took it) or `handed` (hold: blocks handed to their driver) |

## 4. Settings round trip

| Step | What | Source |
|---|---|---|
| Store | NVS namespace `rd`, blob key `config` (the text as posted, comments kept) | `settings.c` |
| Legacy | key `sdr` is read when `config` is missing, and erased on the next save | `settings.c` |
| Export | the stored text, with `"wifi"` (`wifilist_export_json`: networks without passwords) and `"ntp"` (`{servers, poll_s}`) put in | `settings.c:settings_export` |
| Import | `wifi` and `ntp` go to their components (`take_board_keys`) and are removed (`drop_board_keys`); the rest is parsed and resolved (`resolve()`: `run`, each radio, the ports, PSRAM for a second radio, the SDR front end, the SDR transceiver), then stored | `settings.c:settings_save` |
| Wi-Fi passwords | an exported network has no password; on import an entry without `password` keeps the stored one | wifilist |
| Defaults | nothing stored or it doesn't resolve: `DEFAULT_JSON`, `settings_ok` false | `settings.c` |

The schema itself is in [config.md](config.md).

## 5. `GET /api/resolved`

`text/plain`, lines:

```
<box> runs: <role>, <role>[ (both on USB: whichever is plugged in)]
<rdof_config_describe of radio 1, ';' → newline>
port: <usb|uart|sdr|split>[ serial <s>][ at port <p>]

<radio 2 name>:
<…>
<rdof_sdr_config_describe>
plan: <the channel plan>
```

For people. Not a stable machine format.

## 6. `GET /api/spectrum?since=N`

```json
{"seq": 1234, "bins": 512, "row_ms": 200, "rate": 288000, "center": 461100000, "rows": ["<base64>", ...]}
```

| Field | Meaning |
|---|---|
| `seq` | the newest row's number (u32). Pass it as `since` next time. 0 and no rows: nothing computed yet. |
| `bins` | bytes per row, `SPEC_BINS` = 512 (`sdr_task.c`) |
| `row_ms` | ms per row: about 200 (a row averages `0.2 × rate / CHUNK` FFTs) |
| `rate`, `center` | wideband rate (S/s), center (Hz), `%.0f` |
| `rows` | rows after `since`, oldest first, at most `MAX_ROWS` = 16 (`web.c`); the ring holds `SPEC_ROWS` = 64. Each row is `bins` bytes, base64; byte = -2 × dBFS (0 = 0 dBFS, 255 = -127.5 dBFS), lowest frequency first |

`since` greater than the board's `seq` (the board restarted) counts as 0. The spectrum runs only while asked for:
each request keeps it going 10 s.

## 7. `GET /api/rtl/regs`

```json
{"t_s": 123.4, "tuner_n": 32, "error": "", "tuner": "<hex>", "demod0": "<hex>", ..., "demod4": "<hex>", "sys": "<hex>"}
```

`tuner`: 32 registers, 2 hex digits each, `--` for unread. `tuner_n` is how many were read (-1 on failure, with
`error`). It tries 32, 16, 8, then 5 at once. `demod0`–`demod4`: 256 registers per demod page. `sys`: 256 system
registers. A register that fails is `--`. Debug only.

## 7a. `GET /api/lastheard?since=N`

The last 50 calls (`RDOF_LH_DEFAULT`), RF and NET, kept in a ring in PSRAM (`heard.c` over `rdof/lastheard.h`).

```json
{"seq": 44, "now_ms": 1792854252000, "max": 50, "calls": [{"id": 41, "seq": 44, "dir": "rf", "kind": "voice", ...}]}
```

| Field | Meaning |
|---|---|
| `seq` | the ring's change counter (u32): every start, end, BER / RSSI update or encrypted mark bumps it. Pass it as `since` next time |
| `now_ms` | the board's wall clock, ms since the epoch; 0 until the clock is set |
| `max` | how many calls the ring keeps |
| `calls` | the calls whose `seq` is after `since`, newest first (by start). A client merges them by `id` and keeps `max` |
| `more` | `true` when calls were left out to fit the 20 KB answer (not in practice: 50 calls fit) |

`since` past the board's `seq` (the board restarted) counts as 0. Each call:

| Field | Type | Meaning | Present |
|---|---|---|---|
| `id` | int | the ring's `seq` when the call started: unique until the board restarts | always |
| `seq` | int | the ring's `seq` when the call last changed | always |
| `dir` | string | `rf`: heard on the air; `net`: from the network (HBlink4), sent out over RF | always |
| `kind` | string | `voice`, `data`, `csbk` (a CSBK of its own: radio check, call alert, ...; preambles left out) | always |
| `role` | string | who heard or sent it: `sdr`, `roamer`, `repeater` | always |
| `radio` | string | the radio's or SDR's name | always |
| `channel` | string | the SDR channel's id | SDR calls |
| `slot` | int | 1 or 2 | always |
| `src`, `dst` | int | source id, destination id or talkgroup | always |
| `group` | bool | group call (talkgroup) or private | always |
| `freq` | int | Hz | known |
| `cc` | int | color code | known |
| `start_ms` | int | wall clock at the start, ms since the epoch | the clock was set |
| `age_s` | number | seconds since the start (monotonic, so right without a clock) | always |
| `active` | bool | the call is on now | always |
| `duration` | number | seconds | ended, not `csbk` |
| `end` | string | `end` (a terminator, or a data call's last block; NET: the transmission finished), `lost` (timed out, or another call took the slot) | ended, not `csbk` |
| `ber` | number | % (RF: the last second's while active, the call's at the end) | RF voice |
| `rssi` | int | dBm (the last second's while active, the average at the end) | RF, when measured |
| `rssi_min`, `rssi_max` | int | dBm | RF, ended |
| `frames` | int | the blocks a data call's header announces | data |
| `desc` | string | the CSBK's description | `csbk` |
| `late` | bool | `true`: joined after the start (late entry) | late entry |
| `emergency` | bool | `true`: an emergency call | emergency |
| `encrypted` | bool | `true`: a PI header was heard in the call | encrypted |

Sources: RF calls are the call trackers' JSON (the SDR's channels, `sdr_mqtt.c`; each radio's receive path,
`rx_path.c`), parsed by `rdof_lh_tracker_json`; a PI header burst marks the call on its slot encrypted. NET calls are
the transceiver's or repeater's `on_tx` start and end (`radio_task.c`, `rdof_lh_tx`). A bench retune's taps are left
out. Not kept: the talker alias (the tracker doesn't decode it).

## 8. Component endpoints

### 8.1 `GET /api/clock` (netclock, cap 12288)

| Field | Meaning |
|---|---|
| `source` | `ntp`, `browser`, `pps`, `gnss`, `none` |
| `synced`, `since_sync_s`, `poll_s`, `polls`, `no_majority` | sync state and NTP polling |
| `pps_gpio`, `pps_edges`, `pps_bad`, `pps_unlabeled`, `pps_last_s` | pulse-per-second input |
| `pps_by_gnss`, `gnss_times`, `gnss_refs`, `gnss_lag_ms`, `gnss_last_s` | GNSS NMEA time: edges it numbered, seconds given, references taken alone (no pulse), its lag after the pulse, since the last |
| `sources[]` | `{mark, name, ip, stratum, refid, reach, last_rx_s, offset_ms, delay_ms, min_delay_ms, lambda_ms, jitter_ms, root_delay_ms, root_disp_ms, np, kod}` |
| `ref_utc_ms`, `utc_ms` | the last reference and now |
| `fit_ok`, `rate_ppm`, `skew_ppm`, `rate_src`, `stored_ppm`, `refs`, `span_s`, `last_err_ms`, `resid_rms_ms`, `slew_left_ms`, `outliers` | the rate fit |
| `die_c`, `temp_comp_ppm`, `base_die_c`, `tcal[]` (`{c, ppm, n}`) | temperature compensation |
| `log[]` | `{utc_ms, t_s, src, peer, result (step, slew, outlier), err_ms, resid_ms, in_fit, rate_ppm, die_c}` |

The netclock README is the reference for these fields.

### 8.2 `/api/wifi` (wifilist, cap 6144)

Status: `state`, `connected`, `ssid`, and when connected `bssid`, `rssi`, `channel`, `connected_s`; `ip`, `ap_up`,
`ap_ssid`, `scans`, `connects`, `uptime_s`, `networks[]` (`{ssid, priority, hidden, password_set, rssi?}`), `seen[]`
(`{ssid, bssid, rssi, channel, open}`), `fails[]` (`{t_s, ssid, reason, why, rssi, lost}`).

`POST /api/wifi/networks` takes either the whole list,
`{"networks": [{"ssid", "priority", "hidden", "password", "was"}]}` (`was`: the entry's old SSID, so a rename keeps
its stored password; no `password`: keep it), or one network to add, `{"ssid", "password"}`. At most
`WIFILIST_MAX_NETWORKS` (default 8). The wifilist README is the reference.

## 9. The web page

| Page | Built when | Uses |
|---|---|---|
| `index.html` | `CONFIG_RDOF_WEB_FULL=y` (default) | `GET /api/status` every 2 s; `GET /api/lastheard?since=` every 2 s on Overview, Last heard and Radios, every 15 s on the others; `GET /api/tasks` and `POST /api/restart`, `DELETE /api/coredump` (buttons); nothing while the browser tab is hidden; `GET /api/spectrum?since=` every 400 ms while the SDR panel is shown and the tab is visible; `GET`/`POST /api/config` (the editor), `GET /api/config?download=1` (export); `GET /api/resolved`; `GET /api/log`; `GET /api/coredump` (a link by the crash row); `POST /api/ota`; `POST /api/selftest`; `POST /api/spike`; `/wifilist.js` and `/netclock.js`, which use `/api/wifi*`, `/api/clock*` and `/api/time` |
| `mini.html` | `CONFIG_RDOF_WEB_FULL=n` | the last ten calls from `/api/lastheard` every 3 s; links to `/api/status`, `/api/resolved`, `/api/log`, `/api/coredump`, `/api/config?download`; the config editor (`GET`/`POST /api/config`); `/wifilist.js`, `/netclock.js` |

The full page has a header (the board's name, a health pill counting problems and warnings, the board's UTC
clock and uptime) and six tabs, by URL hash:

| Tab | Shows |
|---|---|
| Overview (`#overview`) | what's wrong (settings error, Wi-Fi, clock, a core dump, a dirty build, a radio not ready or its error, HBlink4 or MQTT down, dropped bursts, SDR front end errors, ring overflows, DSP load over 90%, MQTT drops, HBlink4 peers down), the board (runs, settings, Wi-Fi, clock, memory, uptime and reset, crash, build), a card a radio and the SDR, the active and last calls, and a card for each `/api/status` member the page doesn't know (shown as it comes) |
| Last heard (`#heard`) | `/api/lastheard` as a table, newest first: when (relative; the UTC time on hover), RF or NET, from, to, slot, radio and channel, frequency and color code, length (counting while active), RSSI and BER, and data, CSBK, late entry, emergency, encrypted, lost; the active calls highlighted; filters All, RF, NET, Active |
| Radios (`#radios`) | each radio in full (modem, kind, channel, heard and sending, modem requests, bursts, transceiver or repeater counters, jitter buffer, split, HBlink4 and SCTP, MQTT), the SDR in full (front end, receiving, samples, ppm, DSP load by stage, slot timing, MQTT, HBlink4), its channels (the busy ones highlighted) and the spectrum and waterfall; each with "More fields" for any status field not shown otherwise |
| Network (`#network`) | the links (Wi-Fi, clock, each radio's HBlink4, SCTP and MQTT), the Wi-Fi networks (`/wifilist.js`), the clock (`/netclock.js`) |
| Settings (`#settings`) | the settings editor with save, export, import and reload, and the settings as resolved |
| Logs & maintenance (`#maint`) | the build, uptime, reset, memory, firmware update and restart, the crash (cause, backtrace, the core dump to download or erase), the SDR self-test and USB spike, the log (refresh, every 5 s), the tasks |

Counts of trouble (errors, overflows, dropped, refused, ...) show orange when not zero, and what's down or failed red.
Light and dark follow the browser's `prefers-color-scheme`. Below 640 px wide the less needed table columns go.

The page uses only the endpoints above. It holds no state of its own beyond what the board returns. A rewrite that
serves the same endpoints can serve the same page.
