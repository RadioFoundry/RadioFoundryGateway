# netclock

A UTC clock for ESP-IDF v5.x. It maps the local monotonic clock (`esp_timer`, µs) to UTC, disciplined by, in order
of precedence: a GNSS pulse per second (PPS, its seconds numbered by the receiver's NMEA when the app passes it on), NTP (up to 6 servers, chrony-style selection), a GNSS receiver's NMEA time alone, or a web browser's
clock. It learns the crystal's rate error, keeps it (and a rate-against-die-temperature table) in NVS, slews phase
corrections instead of stepping, and rejects outliers.

[SPEC.md](SPEC.md) is the behavior spec: the NTP wire format and poll, selection and combination, the clock model
and rate fit, the source rules, PPS, the temperature table, and every constant. This file covers use, the API,
storage, configuration, HTTP and tests.

| Part | Files | Platform |
|---|---|---|
| Portable core (plain C11, no ESP-IDF) | `include/netclock_{model,select,ntp,core,pps,tempcal,status}.h`, matching `src/*.c` | any; built as the static library `netclock_portable` when `ESP_PLATFORM` is unset |
| ESP-IDF glue | `include/netclock.h`, `src/netclock.c` (lock, NVS, NTP task, PPS ISR, die sensor), `src/netclock_settings.c` (cJSON) | ESP-IDF |
| Web | `include/netclock_http.h`, `src/netclock_http.c`, `web/netclock.js` (gzipped and embedded at build) | ESP-IDF, `CONFIG_NETCLOCK_HTTP` |

## Use

```c
#include "netclock.h"
#include "netclock_http.h"

nvs_flash_init();
netclock_init();                                       // settings, stored rate and table; polls start on the first IP
int64_t now_ns = netclock_now_ns();                    // UTC now
bool ok;
int64_t t_ns = netclock_utc_ns(esp_timer_get_time(), &ok); // UTC of an esp_timer stamp
netclock_http_register(httpd);                         // optional: JSON API and page section
```

In another project: copy the directory into `components/` (or add its parent to `EXTRA_COMPONENT_DIRS`).
Requirements (`CMakeLists.txt`): `REQUIRES esp_event esp_http_server`, `PRIV_REQUIRES esp_timer esp_netif lwip
nvs_flash json esp_driver_tsens esp_driver_gpio`. Host build of the portable part and its test:
`cmake -S . -B build && cmake --build build && ctest --test-dir build`.

Consumers in this repo: `esp_idf` (clock, settings, web page, PPS on GPIO 41 in one config; `sdr_task.c`
reuses `nc_settle_t` and `nc_tcal_t` for the SDR dongle's crystal), `sdr/include/rdof/sdr/rate.h`.

## API (`include/netclock.h`)

Types: `netclock_source_t` (`NETCLOCK_SRC_NONE` 0, `_BROWSER` 1, `_NTP` 2, `_PPS` 3, `_GNSS` 4; `include/netclock_status.h`),
`netclock_settings_t {char servers[NETCLOCK_SERVERS_LEN=128]; uint32_t poll_s;}`, `netclock_status_t` (fields as
the `/api/clock` JSON below).

Locking: the clock is read under a spinlock (`s_mux`, a copy of a few hundred bytes); writers (references,
temperature ticks, status) serialize on a mutex (`s_wlock`), work on a scratch copy and swap it in under the
spinlock. All calls are task context only (none from an ISR).

| Call | Behavior | Thread safety | Errors |
|---|---|---|---|
| `esp_err_t netclock_init(void)` | Once, after `nvs_flash_init()`. Loads settings, `rate_ppb`, `tcal`; installs the die sensor (range -10..80 C); sets the rate prior (SPEC "Boot prior"); starts the 30 s temperature timer if the die reads; starts PPS if `NETCLOCK_PPS_GPIO >= 0`; with `START_ON_IP` creates the default event loop (if none) and registers for `IP_EVENT`. Repeat calls return `ESP_OK`. | not reentrant; call once at boot | `ESP_ERR_NO_MEM`; event loop / handler errors |
| `void netclock_start(void)` | Starts the NTP task (6144 B stack, `NETCLOCK_TASK_PRIORITY`, `NETCLOCK_TASK_CORE`). Called on `IP_EVENT_STA_GOT_IP`, `_ETH_GOT_IP`, `_PPP_GOT_IP` when `START_ON_IP`. No-op if already running or before `netclock_init`. | any task | none (silent) |
| `int64_t netclock_now_ns(void)` | `netclock_utc_ns(esp_timer_get_time(), NULL)` | any task | none |
| `int64_t netclock_utc_ns(int64_t t_us, bool *synced)` | UTC ns at `esp_timer` time `t_us` from the model. Before any reference: `gettimeofday()` minus `(now - t_us)`. `*synced` (optional) = the model is valid. | any task | none |
| `netclock_source_t netclock_source(void)` | Source of the last reference passed to the model (any result) | any task | none |
| `bool netclock_synced(void)` | `netclock_source() != NETCLOCK_SRC_NONE` | any task | none |
| `bool netclock_quality(netclock_source_t *source, double *err_us)` | The source of the last reference (any, browser too: the caller decides what it trusts) and how far off UTC the clock may be now, µs: the fit's rms residual, plus the slew still to apply, plus the rate's standard error (1 ppm before a fit) times the time since the last reference. For a gate that needs a bound (the gateway's UTC slot timing: PPS under 20 µs, NTP under 2 ms). The PPS path's bound has not been checked against a real GNSS pulse yet. | any task | false: no reference yet (`*err_us` untouched) |
| `bool netclock_rate(double *ppm, double *skew_ppm, bool *learned)` | Rate being corrected, ppm (+ = local slow: true elapsed = local elapsed × (1 + ppm·1e-6)), its standard error (0 unless fitted this run), `*learned` = fitted this run. Includes the temperature compensation. | any task | false: no fit and no stored/table prior (outputs 0) |
| `float netclock_die_c(void)` | Die temperature, C; `NAN` without `CONFIG_NETCLOCK_DIE_TEMP` or on a read error | any task | NAN |
| `void netclock_gnss_time(int64_t start_us, int64_t utc_s)` | A GNSS receiver's second: the UTC second (whole) of the NMEA burst that began at local `start_us`. With a pulse it numbers the edge before the burst (so PPS sets the clock with no NTP or browser); with none, every `NETCLOCK_GNSS_EVERY`-th is a GNSS reference (SPEC "GNSS NMEA time"). Only times from a receiver with a fix. | any task | none |
| `bool netclock_set_from_client(int64_t utc_us)` | A browser reference stamped now. Taken only under the source rules (SPEC "Sources"): never once NTP or PPS leads. | any task | false: not taken |
| `void netclock_status(netclock_status_t *out)` | Snapshot: tracking, server rows (as of the last poll, `last_rx_s` aged to now), PPS counters, reference log. About 3 KB: not on a small stack. | any task (takes the mutex) | none |
| `void netclock_get_settings(netclock_settings_t *out)` | Current settings; `poll_s` 0 reads as `CONFIG_NETCLOCK_POLL_S` | any task | none |
| `esp_err_t netclock_set_settings(const netclock_settings_t *s, char *err, size_t cap)` | Validates, writes `servers` and `poll_s` to NVS, applies. A changed server list wakes the NTP task, which reparses, resets the poll count (iburst again) and re-resolves. `err` may be NULL. | any task | `ESP_ERR_INVALID_ARG` (message in `err`), NVS errors |
| `bool netclock_settings_stored(void)` | Settings were ever stored (else Kconfig defaults apply) | any task | none |
| `size_t netclock_settings_json(char *buf, size_t cap)` | `{"servers":"…","poll_s":N}`, compact | any task | 0: did not fit |
| `esp_err_t netclock_settings_from_json(const char *json, size_t len, char *err, size_t cap)` | An object (keys left out keep their value) or a bare JSON string of servers; then `netclock_set_settings` | any task | `ESP_ERR_INVALID_ARG` with message |
| `void netclock_set_name_map(const char *(*map)(const char *))` | Rewrites each parsed server name (after pool expansion) before DNS, e.g. `usb` to a USB-link host | call before `netclock_start` (unlocked) | none |
| `esp_err_t netclock_http_register(httpd_handle_t)` | Registers 5 URI handlers (`netclock_http.h`) | before serving | first `httpd_register_uri_handler` error |
| `const char *netclock_source_name(netclock_source_t)` | `"none"`, `"browser"`, `"ntp"`, `"pps"`, `"gnss"` | pure | — |
| `size_t netclock_status_json(const netclock_status_t*, char*, size_t)` | The status JSON (below). About 10 KB with 6 servers and a full log. | pure | 0 and `buf[0]=0`: did not fit |

The portable API (`nc_model_*`, `nc_select`, `nc_ntp_*`, `nc_src_*`, `nc_combine`, `nc_clock_*`, `nc_pps_*`,
`nc_settle_*`, `nc_tcal_*`) is in [SPEC.md](SPEC.md#portable-api). It is not thread safe: one owner, or the caller
locks.

## Settings and NVS

Namespace `CONFIG_NETCLOCK_NVS_NAMESPACE` (default `netclock`):

| Key | NVS type | Written by | Value |
|---|---|---|---|
| `servers` | string | `netclock_set_settings` | server list, ≤ 127 chars. Absent: `CONFIG_NETCLOCK_DEFAULT_SERVERS` |
| `poll_s` | u32 | `netclock_set_settings` | slow poll interval, s; 0 = `CONFIG_NETCLOCK_POLL_S` |
| `rate_ppb` | i32 | netclock | last stored fitted rate, `lround(rate × 1e9)` (ppb, + = local slow); the uncompensated fit (`fit_rate`) |
| `tcal` | blob, 296 B | netclock, with `rate_ppb`, only when the table changed | `nc_tcal_t` (layout in SPEC "Temperature table"). Rejected on load if the size differs or `nc_tcal_valid` fails |

When the rate is stored: SPEC "Storing the rate". Writes are `nvs_set_*` then `nvs_commit`.

Server list validation (`src/netclock.c:check_servers`): length ≤ `NETCLOCK_SERVERS_LEN - 1` (127); characters
`[A-Za-z0-9]` and `. - _ : , ;` and space only. `""` = no NTP (the task runs and sends nothing). Parsing
(`nc_parse_servers`) splits on space, comma, semicolon, tab; at most `NETCLOCK_MAX_SRC` (6) servers after pool
expansion; a token is truncated to 39 chars. `poll_s`: 0, or 16..3600.

Settings JSON (`netclock_settings_from_json`): not an object and not a string → "clock settings: an object
{servers, poll_s} or a string of servers"; `servers` not a string → "servers: a string"; `poll_s` not a number →
"poll_s: a number"; a negative `poll_s` becomes 1 (then rejected); fractions truncate.

## Kconfig (menu "netclock (NTP disciplined clock)")

| Option | Type, range | Default | Effect |
|---|---|---|---|
| `NETCLOCK_DEFAULT_SERVERS` | string | `pool.ntp.org` | server list until one is stored |
| `NETCLOCK_POLL_S` | int 16..3600 | 300 | slow poll interval (once the rate is settled) |
| `NETCLOCK_POLL_FAST_S` | int 16..3600 | 120 | poll interval while the rate is learned (capped at the slow one) |
| `NETCLOCK_START_ON_IP` | bool | y | start polling on the first IP; off: the app calls `netclock_start()` |
| `NETCLOCK_SET_SYSTEM_TIME` | bool | y | `settimeofday()` on every step (`time()`, log stamps); the model doesn't use it |
| `NETCLOCK_DIE_TEMP` | bool, needs `SOC_TEMP_SENSOR_SUPPORTED` | y | die temperature per reference, the temperature table and its 30 s rate tracking |
| `NETCLOCK_PPS_GPIO` | int -1..48 | -1 | PPS input GPIO (pull-down, IRAM ISR); -1 = none |
| `NETCLOCK_PPS_FALLING` | bool, needs `PPS_GPIO >= 0` | n | second starts on the falling edge (default rising) |
| `NETCLOCK_GNSS_EVERY` | int 1..3600 | 16 | with no pulse: every Nth GNSS NMEA second is a reference |
| `NETCLOCK_GNSS_LAG_MS` | int 0..999 | 0 | with no pulse ever seen: the NMEA burst's start after the second (measured as `gnss_lag_ms` when there is one) |
| `NETCLOCK_HTTP` | bool | y | build `netclock_http.c` and embed `netclock.js.gz` |
| `NETCLOCK_HTTP_BASE` | string, needs `HTTP` | `/api` | prefix of the JSON paths |
| `NETCLOCK_NVS_NAMESPACE` | string | `netclock` | NVS namespace |
| `NETCLOCK_TASK_PRIORITY` | int 1..24 | 8 | NTP and PPS task priority (above the web server: reply stamps want a prompt wakeup) |
| `NETCLOCK_TASK_CORE` | int -1..1 | 0 | NTP and PPS task core; -1 = no affinity |

`NETCLOCK_MAX_SRC` (6) is a C macro (`include/netclock_status.h`, `#ifndef`), not Kconfig.

## HTTP (`netclock_http_register`)

Paths under `CONFIG_NETCLOCK_HTTP_BASE` (`/api`). JSON responses: `Content-Type: application/json`,
`Cache-Control: no-store`.

| Method, path | Request | Response | Errors |
|---|---|---|---|
| `GET /api/clock` | — | status JSON (below), ≤ 12288 B | 500 `no memory`, 500 `too big` |
| `POST /api/time` | `{"utc_ms": <number>}`, body < 96 B | `{"applied":bool,"source":"<name>"}` | 400 (body too long / read error). `utc_ms` missing, not a number or ≤ 1.6e12 (before 2020-09-13): 200 with `applied:false` |
| `GET /api/clock/settings` | — | settings JSON | — |
| `POST /api/clock/settings` | settings JSON (object or string), body < 256 B | settings JSON after applying | 400 `too long`; 400 with the validation message as plain text |
| `GET /netclock.js` | — | the page section, `Content-Encoding: gzip`, `text/javascript`, `Cache-Control: no-cache` | — |

Five handlers: leave room in `httpd_config_t.max_uri_handlers`. `utc_ms` is converted as
`(int64_t)(utc_ms × 1000)` µs and stamped with `esp_timer_get_time()` when the handler runs.

### Browser clock exchange (`web/netclock.js`)

1. Measure: 3 × `GET {base}/clock/settings` (`cache: no-store`); `best` = the quickest round trip (ms,
   `performance.now()`).
2. Send: `POST {base}/time` with `{"utc_ms": Date.now() + best/2}`.
3. The device stamps it on receipt; the result is the browser's clock to within about ±`best/2`.

Sent automatically after each status load when `data-auto-time != "0"` and `source` is neither `ntp` nor `pps`, and
either `source == "none"`, or (`Date.now() - lastPush > 600000` and `since_sync_s > 600`). Also sent by the "Set
from this browser" button. One exchange at a time.

Page section: `<div data-netclock></div><script src="/netclock.js" defer></script>`. Div options: `data-base`
(default `/api`), `data-auto-time="0"`, `data-refresh` (s, default 5; reloads only while the page and the element
are visible). Colors from the page's `--mute`, `--line`, `--ok`, `--bad`, `--warn`, `--accent`. `window.netclock.mount(el)`
mounts it on another element.

### `GET /api/clock` JSON

One object, keys in this order. Floats are printed with the listed decimals, trailing zeros dropped, `-0` as `0`;
a non-finite float is `null`. "omitted" = the key is left out.

| Key | Type | Unit, decimals | Meaning |
|---|---|---|---|
| `source` | string | | `none`, `browser`, `ntp`, `pps`, `gnss`: source of the last reference given to the model |
| `pps_by_gnss`, `gnss_times`, `gnss_refs`, `gnss_lag_ms`, `gnss_last_s` | numbers | | PPS edges numbered by the receiver's NMEA; NMEA seconds given; taken as references alone (no pulse); the burst's lag after the pulse, ms (null: not measured); since the last, s (-1 never) |
| `synced` | bool | | `source != "none"` |
| `since_sync_s` | int | s | since the last accepted (non-outlier) reference; -1 = never |
| `poll_s` | int | s | current poll interval |
| `polls` | int | | NTP polls since start or the last server-list change |
| `no_majority` | int | | polls skipped for lack of a majority |
| `pps_gpio` | int | | PPS GPIO, -1 = none (then the next four are 0, -1) |
| `pps_edges` | int | | edges seen |
| `pps_bad` | int | | edges out of step with the previous good one |
| `pps_unlabeled` | int | | edges the clock could not number (not set, or > 300 ms from a whole second) |
| `pps_last_s` | int | s | since the last edge; -1 = never |
| `sources` | array | | one per configured server (below) |
| `ref_utc_ms` | int | ms | UTC of the last accepted reference; 0 = none |
| `utc_ms` | int | ms | UTC now (model, or the system clock before any reference) |
| `fit_ok` | bool | | a rate fit exists |
| `rate_ppm` | number | ppm, 3 | rate being corrected (+ = local slow), with temperature compensation |
| `skew_ppm` | number | ppm, 3 | standard error of the last fit; 0 before one |
| `rate_src` | string | | `learned` (fitted this run), `stored` (NVS rate or table), `none` |
| `stored_ppm` | number | ppm, 3 | rate in NVS (0 = none) |
| `refs` | int | | references in the model's history |
| `span_s` | number | s, 0 | time from the oldest history reference to the last fit |
| `last_err_ms` | number | ms, 3 | last reference minus the model before correction (0 after a step) |
| `resid_rms_ms` | number | ms, 3 | RMS residual of the kept references about the fit |
| `slew_left_ms` | number | ms, 3 | correction still to slew in (+ = model behind) |
| `outliers` | int | | references rejected as outliers |
| `die_c` | number | C, 1 | die temperature; omitted if unknown |
| `temp_comp_ppm` | number | ppm, 3 | what the temperature table adds to the base rate now |
| `base_die_c` | number | C, 1 | die temperature the base rate holds at; omitted = no compensation |
| `tcal` | array | | table bins, coldest first: `{"c": int C (bin center), "ppm": number 3, "n": int learnings (≤ 255)}` |
| `log` | array | | the last ≤ 32 references, oldest first (below) |

`sources[]` (as `chronyc sources`):

| Key | Type | Unit, decimals | Meaning |
|---|---|---|---|
| `mark` | string (1 char) | | `*` best, `+` combined, `x` falseticker, `~` delay spike (skipped), `?` no usable sample, `-` unresolved or duplicate address |
| `name` | string | | as configured (after pool expansion and the name map) |
| `ip` | string | | dotted quad; `""` until resolved |
| `stratum` | int | | from the last usable reply; 0 = none |
| `refid` | string | | stratum 1: up to 4 printable ASCII chars; stratum > 1: dotted quad; else `""` |
| `reach` | int | | 8-bit shift register, bit 0 = the last poll answered |
| `last_rx_s` | int | s | since the last usable reply; -1 = never |
| `offset_ms` | number | ms, 3 | last sample minus the model (+ = model behind) |
| `delay_ms` | number | ms, 3 | round trip less the server's hold time |
| `min_delay_ms` | number | ms, 3 | quickest of its last 8 usable round trips |
| `lambda_ms` | number | ms, 3 | root distance used for selection |
| `jitter_ms` | number | ms, 3 | sample std dev of its offsets vs the model |
| `root_delay_ms`, `root_disp_ms` | number | ms, 3 | from the server's reply |
| `np` | int | | offsets in its jitter history (≤ 8) |
| `kod` | bool | | backing off after a kiss-o'-death |

`log[]`:

| Key | Type | Unit, decimals | Meaning |
|---|---|---|---|
| `utc_ms` | int | ms | what the reference said |
| `t_s` | number | s, 3 | local (`esp_timer`) time of the reference |
| `src` | string | | source name |
| `peer` | string | | NTP: the best server that poll; else `""` |
| `result` | string | | `step`, `slew`, `outlier` |
| `err_ms` | number | ms, 3 | reference minus the model before correction; 0 for a step |
| `resid_ms` | number | ms, 3 | residual about the current fit; omitted before a fit |
| `in_fit` | bool | | used by the current fit |
| `rate_ppm` | number | ppm, 3 | rate after this reference |
| `die_c` | number | C, 1 | die temperature at the reference; omitted if unknown |

## Tests

`test/test_netclock.c` (host, `netclock_portable`; also run from `host-tests` as ctest `netclock`).
`host-tests` also runs `node --check` on `web/netclock.js` (ctest `js_netclock`).

| Test | Pins |
|---|---|
| `model_steps` | first reference steps; 1 s errors are outliers twice, the third steps; 5 ms error slews 30% at 2 µs/s (1300 µs left after 100 s); a new source steps; a prior ≥ 500 ppm is dropped |
| `selection` | marks and combined offset: 3 agree (`*++`, 1.48 ms); one falseticker of 3 (`*x+`); 2 disagree, the tighter wins (`x*`); 2 vs 2 no majority (`xxxx`, -1); one silent (`?*+`) |
| `ntp` | NTP ↔ µs round trip ±1 µs, era 0; pool expansion (`0.`/`1.`/`2.`), the 6-server cap, a numbered pool member is one server; request bytes (`0x23`, poll 8 for 300 s, cookie at 40..47); six polls of four servers: one copy per poll, a 60 ms slower round trip marked `~`, a 40 ms-off server `x`, combined offset within 0.3 ms, reach `0x3F`, the view (ip, refid `GPS`, delay 6 ms, root delay 10 ms); KoD sets the 16-poll skip; LI 3 unusable; wrong cookie ignored; iburst keeps the quicker reply |
| `core` | stored rate is the prior and reported `stored`; browser steps, NTP then steps as a new source; a 10 ppm rate is learned and stored; settled; status and log; status JSON head, escaping, number format, nesting, and 0 when the buffer is too small |
| `pps_edges` | unnumbered when the clock is unset or 400 ms off; one reference per 20 edges on the whole second (clock 120 ms off, crystal 50 ppm); a glitch is out of step and restarts the count; a missed pulse numbered 2 apart stays in step |
| `model_runs` | simulated 8 h runs (below): 60 s error and final rate within bounds |
| `tcal` | binning, in-bin averaging, interpolation, flat beyond the ends, a bad learning fading, least-recently-learned eviction, version check; `nc_settle_t` settles 420..660 s after a warm-up and restarts after a NAN |
| `temperature` | learns 30 and 50 C bins within 0.3 ppm; the next boot's prior comes from the table; 2 h holdover warming 30→50 C: < 3 ms off with the table, > 20 ms on a stored rate |
| `pps_run` | 1 h NTP, 2 h PPS, then NTP: NTP skipped while PPS leads, NTP leads 5 min after the pulse stops, PPS error bounds |
| `model_edges` | no residual or fit membership before a fit; compensation beyond 500 ppm, NaN or infinite refused (the model keeps its rate); a 600 ppm clock: references accepted, the fit refused, rate stays 0; three references at one instant and two far off the line leave the last fit in place |
| `core_edges`, `core_state` | `nc_clock_set_tcal`: an invalid table ignored, a valid one kept with no die reading, the prior taken from it, not applied once a reference has set the clock; a clock never set (status, quality, rate, offset, utc); a stored rate reported `stored`; source precedence (a browser never displaces NTP, GNSS or PPS, GNSS sits between, NTP or GNSS takes over from a silent pulse after 60 s); PPS edges numbered by a receiver's label with no clock and over a clock a second off, a stale or off-grid label ignored; quality counts no negative time; the log keeps the last 32 references, oldest first |
| `ntp_edges` | only separators, no list or no room, a name cut to 39 characters, a pool and a digit-first name; a reply with a negative delay (0), over 500 ms, transmit before receive, stratum 16, short, wrong mode, none outstanding; a missed poll; views of stratum 2 (refid as an address), stratum 1 (unprintable bytes dropped) and 0; `nc_combine` with no replies, two pairs that disagree (no majority, all `x`) and a list longer than the table |
| `select_edges` | nobody answered, no candidates, touching intervals, identical samples, more than `NC_SELECT_MAX` candidates (only the first 16 are looked at) |
| `tcal_edges` | `nc_tcal_valid` for another version, too many bins, NaN and infinite values, bins out of order; `nc_tcal_put` refuses NaN, infinite values and bins past an int8, accepts -128 and 126; binning rounds to the nearest even bin; `nc_tcal_get` empty, NaN, beyond either end, between each pair of three bins, at a bin |
| `status_edges` | `netclock_source_name` for each source and an unknown one; no buffer; NaN and infinity as `null`; no die temperature when unknown, the table, escaping of quote, backslash and control characters in a server name; a missing mark and rate source; `-0` as `0`; results `slew`, `outlier` and `?`; residual and die temperature of a log entry only when known; every shorter buffer refused, exact fit |

Measured (`ctest`, 2026-10-21; local clock 18 ppm off with a ±2 ppm 6 h wander unless noted; NTP 0..8 ms one-way
jitter, 5% slow round trips of 80..300 ms; scored after the first hour):

| Run | Max abs error | Max error over 60 s | Final rate vs true | Offset-only stepping, 60 s |
|---|---|---|---|---|
| 18 ppm + wander, 8 h | 14.6 ms | 238 µs | 18.64 / 19.73 ppm | 281 ms |
| 18 ppm steady, 8 h | 12.7 ms | 137 µs | 17.90 / 18.00 | 281 ms |
| -35 ppm steady, 8 h | 7.1 ms | 139 µs | -35.05 / -35.00 | 288 ms |
| 18 ppm, stored prior 17 ppm, 2 h | 6.5 ms | 135 µs | 18.20 / 18.00 | 10.4 ms |
| 18 ppm, 0..60 ms jitter, 8 h | 56.7 ms | 254 µs | 17.21 / 18.00 | 297 ms |
| PPS after 1 h NTP (2 µs stamp jitter) | 44 µs | 3 µs | — | — |
| Holdover 2 h warming 30→50 C, 0.2 ppm/C | 0.5 ms (table) | — | — | 22.4 ms on a stored rate |

## Provenance

Ported from smartlaps (same author): `timebase`, `clock_model`, `ntp_client`, `ntp_select` and the `sim_clock`
test. Algorithms follow chrony's documented behavior (iburst, delay filter, root distance, Marzullo selection,
skew) and RFC 5905 / RFC 4330 for the packet; no chrony or ntpd code. No third-party code; `PROVENANCE.md` lists it as clean.
