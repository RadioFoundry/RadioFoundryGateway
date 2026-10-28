# hostweb

The firmware's web page for a host (Linux / macOS) tool: watch a running `rdofradio` live in a browser, and edit its
settings. Off by default. POSIX sockets and pthreads, no other dependency. The firmware doesn't use it; it has its
own server (esp_http_server). Both route the shared API to the same handlers in `common/src/webapi.c`
(`rdof/webapi.h`); each gives it a platform: a struct of optional function pointers and capability flags.

## What it serves

- `/`, `/netclock.js`, `/wifilist.js`: the firmware's own files (`esp_idf/main/web/index.html` and the two
  components' scripts), embedded at build time by `embed.cmake`. Not copies. The page loads the two scripts only
  when the status says the server has them, so here it doesn't.
- The shared routes (`rdof_webapi_handle`):
  - `GET /api/status`: the board (name, uptime, `version` "host", `reset` "start" or "restart"), the process's and
    the system's memory (`rss`, `rss_peak`, `mem_total`, `mem_avail`: mach on macOS, `/proc` on Linux), the system
    clock as the OS's NTP client keeps it (`clock` "os" and `clock_os`, read with `ntp_adjtime`), the radio as a
    repeater or roamer card, extra cards `host`, `slots` (per slot call src / dst / group / BER / RSSI) and
    `parrot`, and `caps`: what this server has (below).
  - `GET /api/lastheard` and `GET /api/log`: the last heard list and a 64 KB log ring.
  - `GET / POST /api/config`, `GET /api/resolved`, `POST /api/restart`: when the tool gives settings hooks
    (`rdof_hostweb_settings`). Read only, a POST answers 403. A POST is checked by the tool's own `save`; saved,
    the answer is "saved; restarting" and the tool's `restart` hook runs after it goes out.
- `GET /api/spectrum?since=N`: the spectrum and waterfall, as the board's: the rows newer than row N (512 bins, base64,
  dBFS times -2), plus `channels`, the status's receive, transmit and monitored channels with `in` set for those in
  the span. The tool makes rows (`rdof_hostweb_spectrum_row`) only while `rdof_hostweb_spectrum_wanted`: the page
  asked in the last 10 s.
- Every other `/api/` path, and every other POST / DELETE, answers 501 with `{"error":"not on this platform", ...}`:
  OTA, core dump, Wi-Fi, the firmware's clock, tasks.

`caps` in the status: `ota`, `restart`, `wifi`, `tasks`, `coredump`, `board_mem`, `sys_mem`, `netclock`,
`os_clock`, `links`, `settings`, `settings_edit`, `spectrum`. The page shows what's set and hides the rest (a status without
`caps` is firmware with everything). Here: `sys_mem` and `os_clock`; `settings`, `settings_edit` (writable) and
`restart` with the settings hooks; `spectrum` when the status says the tool makes one.

One thread serves one connection at a time, bound to 127.0.0.1 unless told otherwise (there is no
authentication: bind to another address only on a network you trust). A client that stalls holds it up to 2 s.

## API (`rdof/hostweb.h`)

| Call | What |
|---|---|
| `rdof_hostweb_start(addr, port, name)` | start; port 0 picks a free one; returns the port or -1 |
| `rdof_hostweb_stop()` | stop and join |
| `rdof_hostweb_status(&st)` | publish a snapshot: copied under a `trylock`, dropped if the server is reading (the next one replaces it) |
| `rdof_hostweb_logf(fmt, ...)` | a line for the log, stamped UTC |
| `rdof_hostweb_tracker_json(role, radio, channel, json)` | a call tracker's JSON line, for last heard |
| `rdof_hostweb_net_call(role, radio, action, call, duration)` | a call going out ("start" / "end"), for last heard |
| `rdof_hostweb_spectrum_wanted()` / `rdof_hostweb_spectrum_row(row, bins, center, rate, row_ms)` | the page's spectrum: make rows only while wanted |
| `rdof_hostweb_settings(&hooks)` | the settings: `get`, `save`, `resolved`, `restart`, `writable`, `source` |
| `rdof_hostweb_save_file(path, text, len, err, cap)` | write a file whole or not at all (a temporary file, fsync, rename) |
| `rdof_hostweb_reexec(argv)` | start the program again with the same command line (after it has stopped cleanly) |

The publishing calls are thread safe and do nothing when the server isn't running. None does I/O or waits on a
socket: the real-time loop can call them. The settings hooks run on the server's thread. `restart` should only ask
(set a flag); the tool stops its loop, puts its transmitter down, stops the server and calls `rdof_hostweb_reexec`.
The listening socket is close-on-exec, so the new process can bind the same port.

## Build

```sh
cmake -S host-tests -B build/web -DRDOF_HOST_WEB=ON && cmake --build build/web
```

`RDOF_HOST_WEB=ON` links `rdof_hostweb` into `rdofradio` and defines `RDOF_HOST_WEB` for it (`sdr/cli/tools.cmake`),
which turns on `--web [ADDR:]PORT`.
`rdof_hostweb_tests` always builds and runs under ctest. `build/web/rdof_hostweb_tests --serve [PORT] [FILE]` serves
moving fake state for a browser; with FILE, its settings are that file (editable; a save logs the restart it would
do), without one there are none.
