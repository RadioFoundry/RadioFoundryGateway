# Device: status LED, OLED, self-test, log ring, watchdog and faults

Covers: the board-level indicators and diagnostics of the ESP32-S3 RF gateway (`esp_idf`): the RGB status LED, the OLED status screen, the CPU self-test and USB spike, the log ring, and watchdog, crash and reset handling.
Sources: `esp_idf/main/{status_led,oled_task,selftest,selftest_core,kbench,logring,sdr,web,main,settings,rx_path,radio_task}.c`, `selftest/clip.h`, `Kconfig.projbuild`, `sdkconfig.defaults*`, `partitions-*.csv`; `common/src/{oled,oled_sdr,config}.c`.
Related: [radio.md](radio.md) (modem readiness), [overview.md](overview.md), [config.md](config.md), [interfaces-http.md](interfaces-http.md) (`/api/status`, `/api/log`, `/api/selftest`), [mqtt.md](mqtt.md) (the retained `status` message), [sdr.md](sdr.md), [bench-api.md](../bench-api.md).

## 1. Status LED

One WS2812 RGB LED, driven by RMT. Absent when `CONFIG_RDOF_STATUS_LED_GPIO` is -1 (`status_led_start` is a no-op).

| Constant | Value | Where |
|---|---|---|
| GPIO | Kconfig default -1; 48 in `4mb` and `16mb`; 46 in `w12` | `Kconfig.projbuild:RDOF_STATUS_LED_GPIO`, `sdkconfig.defaults.{4mb,16mb,w12}` |
| Brightness | 24 of 255 per lit channel | `status_led.c:LEVEL` |
| Poll period | 500 ms; the LED is written only on a color change | `status_led.c:led_task` |
| RMT resolution | 10 MHz (0.1 µs ticks), 48 symbols, queue depth 2 | `status_led.c:status_led_start` |
| WS2812 bit timing | 0 = 0.3 µs high / 0.9 µs low; 1 = 0.9 / 0.3; MSB first; byte order G, R, B | `status_led.c:status_led_start`, `show` |
| Task | `led`, 2048 B stack, priority 1 | `status_led.c:status_led_start` |

### States

Solid colors only; no blinking. Evaluated top to bottom every 500 ms.

| Color | GRB bytes | Condition |
|---|---|---|
| red | `00 18 00` | `g_board.wifi_up` false (no Wi-Fi station link). Also the boot color: shown by `status_led_start`, the first call in `app_main`, so it stays red through settings load and a boot self-test. |
| orange | `08 18 00` | Wi-Fi up, `devices_ok()` false |
| green | `18 00 00` | Wi-Fi up, `devices_ok()` true |

`devices_ok()` (`status_led.c`):

| Role in the settings | Working means |
|---|---|
| radio (`g_roles.has_radio`, `CONFIG_RDOF_ROAMER`) | every roamer / repeater in `g_roam[]` has `st.started` and `st.radio_ready` (modem probed and ready, [radio.md](radio.md)), and there is at least one |
| SDR (`g_roles.has_sdr`, `CONFIG_RDOF_SDR`) | `g_sstat.started && g_sstat.rtl_open && g_sstat.receiving` |
| both on one USB port (`g_board.usb_either`) | only the role that started counts: the SDR's test if `g_sstat.started`, else at least one radio started and all ready. Nothing plugged in = orange. |
| neither | green |

A radio that failed to start (for example `roamer.c` refusing a second radio on low internal RAM) has `started` false, so the LED stays orange.

## 2. OLED

A 128×64 SSD1306 or SH1106 on I2C. A board that runs an SDR shows the SDR's screen (its channels and the calls on them, with the first roamer's TX row when there is one too); otherwise it shows the **first** roamer or repeater. `main.c` calls `oled_start` once, after the web server and before the roles start; until a role has started (one USB port, nothing plugged in) the screen says `waiting for a device`. Built with `CONFIG_RDOF_OLED` (default y, depends on `CONFIG_RDOF_ROAMER` or `CONFIG_RDOF_SDR`).

On when the box config's `oled` key is there and not `null` or `false`. With no `oled` key, a board with `CONFIG_RDOF_OLED_BUILTIN` (its display is built in: the W12) has it on; others have it off. The settings are read by `oled_sdr.c:rdof_oled_settings_parse` whatever radios run, and checked when settings are saved (`settings.c:resolve`). A roamer's `config.c:oled` parses the same key for `/api/resolved`.

Board options (Kconfig):

| Option | Default | |
|---|---|---|
| `CONFIG_RDOF_OLED_BUILTIN` | n | on with no `oled` key |
| `CONFIG_RDOF_OLED_SDA` / `_SCL` | 8 / 9 | the pins when the settings don't name them |
| `CONFIG_RDOF_OLED_POWER_GPIO` | -1 | a power switch, driven to `CONFIG_RDOF_OLED_POWER_LEVEL` (default 0) before the bus starts; the task waits 100 ms before the first init |

### Settings

| Key | Default | Notes | Where |
|---|---|---|---|
| `oled.address` (or `oled.i2c.address`) | `0x3C` | 7-bit | `config.c:oled` |
| `oled.sh1106` (or `oled.i2c.sh1106`) | false | page addressing, column offset 2 | `config.c:oled` |
| `oled.rotate180` (or `oled.i2c.rotate180`) | false | | `config.c:oled` |
| `oled.refresh_s` | 0.5 | floored at 500 ms | `config.c:oled`, `oled_task.c:oled_task` |
| `oled.sda` / `oled.scl` | `CONFIG_RDOF_OLED_SDA` / `_SCL` (8 / 9; W12 17 / 18) | internal pull-ups on; 0..48, not the same pin | `oled_sdr.c:rdof_oled_settings_parse` |

`rdof_oled_settings_parse` also refuses an address outside 0x03..0x77. Numbers may be written as strings (`"60"`), as `config.c` takes them.

I2C: port `I2C_NUM_0`, 400 kHz, `glitch_ignore_cnt` 7, timeouts 50 ms for commands and 100 ms for data (`oled_task.c`). Task `oled`, 4096 B, priority 3, pinned to core 0; it installs the I2C bus itself, so the driver's interrupt is on core 0 too. Core 1 is the SDR's DSP, which at 576 kS/s has almost no slack: the first version (not pinned, a byte per I2C write) came with ring overflows during calls on the W12.

### Panel protocol

Commands go as one I2C write, `00` and the bytes (`oled_task.c:cmds`). Data goes as one write, `40` and the bytes (`oled_task.c:data`). Each refresh sends the pages from the first changed one to the last (`oled_task.c:send`): an SSD1306 as one command write (`21 00 7F 22 first last`) and one data write of up to 1024 bytes (about 23 ms at 400 kHz); an SH1106, which can't run on from page to page, a page at a time.

Init (`oled.c:rdof_oled_init_cmds`, contrast `0x7F`):

```
AE  D5 80  A8 3F  D3 00  40  8D 14  20 00  <seg> <com>  DA 12  81 7F  D9 F1  DB 40  A4  A6  AF
seg/com = A1 C8 (normal) | A0 C0 (rotate180)
```

Page address before each page's 128 data bytes (`oled.c:rdof_oled_page_cmds`):

| Panel | Bytes |
|---|---|
| SSD1306 | `21 00 7F 22 first last` (column range 0..127, the changed pages) |
| SH1106 | `B0+p 02 10` (page p, column 2) |

Framebuffer: 8 pages × 128 column bytes, LSB at the top (`oled.h:rdof_frame_buf_t`; index `(y>>3)*128 + x`, bit `y&7`). Only pages that differ from the last frame sent are written (for an SSD1306, the run from the first to the last). On any I2C failure the panel is reopened (full init, all pages resent) on the next refresh; if init fails it retries every 5 s (`oled_task.c:oled_task`).

### Font

Adafruit GFX 5×7 (`oled.c:GLYPHS`), ASCII 0x20..0x7E; anything else draws `?`. A character is its 5 columns plus one blank column (6 px). `tight`: a character that is not alphanumeric takes only its first lit column (none for a space) plus the gap (`oled.c:columns`). `invert` draws all 8 rows of every column, gap included, inverted.

### The SDR's screen (`oled_sdr.c:rdof_oled_render_sdr`)

A receiver box's screen, for one board. 21 columns of 6 px; the top 16 rows are the network and the flags, so on a two-color panel (the W12's SSD1315: yellow rows 0..15) they are the yellow band.

```
 ((( 192.0.2.8                     y 0   Wi-Fi bars (4 at -55 dBm, 3 -65, 2 -75, 1 -85; an x without Wi-Fi), "AP " in softAP mode, the IP or "no address"
 [MQ] -HB- [SDR]        12:41:07   y 8   flags; the clock right-aligned (UTC, blank until netclock is synced)
 -------------------------------   y 18
 RX [4] 13                         y 20  a badge per channel, inverted while it hears a call, struck through when it's out of the tuner's window
  4 T9<9990123                     y 29  the calls being heard (badge, T group / P private, dst<src), 3 lines; "+N more" on the last when they don't fit
 13 461.0375 Short Eve             y 37  with no call: each channel, badge, MHz, name; with the front end's error: the error, wrapped
 -------------------------------   y 54
 461.0475 576k cpu 19%             y 56  the tuner: center MHz, rate, DSP load; "starting" while not receiving. With a roamer: TX row
```

| Element | Rule | Source (`oled_task.c:render_sdr`) |
|---|---|---|
| flags | `MQ`, `HB`, `SDR`, `HAT`: inverted box when up, struck through when down, left out when not used | MQ: `g_sstat.mqtt_up`, left out with no `mqtt` host. HB: every SDR peer up (`hb_up == hb_peers`), left out with no peers. SDR: `rtl_open && receiving`. HAT: the first roamer's `radio_ready`, left out without one. With a roamer too, MQ and HB are up only when both roles' are |
| badge | the receiver id's trailing number (`SDR04` → `4`); other ids keep their first letter (`T1`, `ROAM2` → `R2`); no number: the first three characters (`rdof_oled_badge`) | `g_scfg.ch[c].id`; in the window: `g_rx_ready && g_sstat.in_window[c]` |
| `RX` label | left out when the badges need the room; a badge that doesn't fit is a `+` | |
| call | the first active slot of the channel's tracker (`trackers[k].slots[s].call`: `src`, `dst`, `group`), read without `rx_lock` (the DSP takes it for every block): a bench retune frees the trackers after `g_rx_ready` goes false, into memory that stays readable, so at worst one frame is wrong | receiver channel k → config channel through `sdr_rx_cfg_channel`; bench taps are left out |
| error | `g_sstat.rtl_error`, 21 characters a line, when no call is heard | |
| bottom | `rdof_oled_tuner_line`: `g_sstat.center_hz` (MHz, trailing zeros off), `g_scfg.sample_rate / 1000` `k`, `cpu NN%` from `g_sstat.cpu_load` | with a roamer: `TX` (inverted on air), then `st.sending`, or `idle <MHz>` (its channel, a repeater's `tx_freq`) |

### The roamer's screen (`oled.c:rdof_oled_render`)

| y | Content |
|---|---|
| 0 | `WiFi <ip>` or `no WiFi <ip>` (tight) |
| 9 | Tags `MQ`, `HB`, then `RPT` (repeater) or `HAT` (roamer): inverted when up; normal text with a line through row y+3 when down. Next tag 2 px after the previous one's gap column. Right-aligned: clock `HH:MM:SS` |
| 18 | full-width rule |
| 21 | roamer: `<name> <MHz %.4f> CC<cc>`; repeater: `<name> <rx MHz>/<tx MHz>` (tight) |
| 30 | state text (tight, below) |
| 41 | when a call is heard: `RX` (inverted while hearing) then, 3 px on, the call (tight) |
| 51 | full-width rule |
| 54 | `TX` (inverted while on air), 3 px on, the call being sent or `idle` |

Inputs (`oled_task.c:oled_task`):

| Field | Source |
|---|---|
| tags | `g_board.wifi_up`, `g_board.ip`, `R->st.mqtt_up`, `R->st.hblink_up`, `R->st.radio_ready` |
| clock | `netclock_utc_ns`; blank until netclock is synced. `localtime_r` with no `TZ` set anywhere, so it is UTC |
| frequency, CC | roamer: `radio_channel_now` (the transceiver's current channel; 0 / 0 in a build or config with no transceiver); repeater: `cfg->rx_freq`, `cfg->tx_freq`, `cfg->cc` |
| state | `ready` / `radio error` (`st.radio_error` set) / `no radio`. A roamer with a transceiver and a ready radio: `rdof_txvr_state_name` (`idle`, `pending`, `listening`, `on_air`, `done`) plus ` scanning` or ` on idle` |
| RX call | `st.heard` = `"<src>><dst>"` (decimal), set on tracker `start` / `late_entry`; `st.hearing` cleared on any other call action (`rx_path.c:on_call`) |
| TX call | `st.sending` = `"T<src>><dst>"` or `"P<src>><dst>"`; `st.on_air` true between transmit `start` and the next action (`radio_task.c:on_tx`) |

The same `heard` and `sending` strings appear in `/api/status` (`roamer.c`).

This screen shows the board's first roamer or repeater only (a board without an SDR); the clock is UTC.

## 3. Self-test

Built with `CONFIG_RDOF_SDR_SELFTEST` (default y, depends on `CONFIG_RDOF_SDR`; the clip is about 338 KB of flash). A roamer-only image has none. Without it `selftest_run` writes `not in this build (CONFIG_RDOF_SDR_SELFTEST)` to the verdict.

### Triggers

| Trigger | How |
|---|---|
| `POST /api/selftest` | sets NVS flag `rdof/selftest` = 1, replies `restarting to run the self-test (about 20 s; then see the log)`, restarts after 1 s (`web.c:post_selftest`, `restart_task`) |
| setting | `"selftest": true` in the SDR's section (`g_scfg.selftest`), at every boot |

`sdr_boot_selftest` (`sdr.c`) runs from `app_main` after `settings_load` and **before** Wi-Fi, the web server and the roles start. The flag is erased as it is read (`settings.c:settings_take_flag`). It runs in task `selftest` (16384 B stack, priority 5, pinned to core 1) and `app_main` blocks until it ends. Core 1's idle task is removed from the task watchdog for the duration and re-added after.

### Steps (`selftest.c:selftest_run`)

| # | Step | Pass rule / output |
|---|---|---|
| 0 | header line | clip length, channels, rate, CPU MHz, esp-dsp FFT / FIR on or off, DSP code in IRAM or flash, data cache KB |
| 1a | the clip through the receiver on one core (`sdr_selftest_check`, split 0) | PASS when messages = 53, bursts = 39 and CRC = `0x283d7439` |
| 1b | the same on two cores (`SDR_SPLIT_AUTO`), when `CONFIG_RDOF_SDR_TWO_CORES` | same rule |
| 1c | the clip with each fault (`sdr_selftest_check_variant`): `offset` (+4 kHz rotation), `noise` (about 12 dB C/N), `hot` (overload ×4) | PASS on message and burst counts only (53 / 39); CRC reported as matching or `differs (bits in noise)`. A failure fails the one-core result |
| 2 | stage table (`rdof_sdr_bench`, 2 reps) | Mcycles per second of IQ per stage: cu8 to float; channelizer forward FFT; channelizer per channel; discriminator; RRC matched filter + power; squelch, sync search, slicing, flywheel; call trackers; total; search without squelch |
| 3 | FFT timings (200 calls each) and `kbench_run` | cycles per call: 1280, 384, 512, 64 points built-in against the S3 backends; kbench (needs `CONFIG_RDOF_SDR_ESP_DSP`, best of 50): FFT 64/128/384/512 (built-in, fc32 radix-2/4, sc16), FIR, int16 SIMD FIR, discriminator, modulator |
| 4 | load test (`load_test`) | N busy channels (a voice LC header burst every slot, with CACH), 2.0 s of IQ (`LOAD_SECONDS`), one core then two, then two taking turns; `% of one core` and `% of the wall clock`. Settings `selftest_load {channels, rates}` at the top level or in any `radios` entry; defaults 8 channels, `[288000]`; up to 4 rates; a rate must be a multiple of 48000, at least 96000, and fit the channels at 25 kHz (12.5 kHz if needed) within 0.75 × rate |
| 5 | transmit loopbacks (`rdof_sdrmodem_loopback`, half and full duplex), when `RDOF_FEATURE_TX && RDOF_FEATURE_SDR_RECEIVER` | PASS / FAIL each |
| 6 | verdict | see below |

`prof` log lines follow each clip and load run (Mcycles per second of IQ per core and per step; `sdr_prof.h`).

The output check (`selftest_core.c`) is portable and also runs on the host (`sdr_selftest_tests`). Per channel it keeps a zlib CRC-32 (`sdr_crc32`, poly `0xEDB88320` reflected, init and xorout `0xFFFFFFFF`) over each message as `"<channel> <json|raw> <text>\n"`, in order. The total CRC is the CRC-32 over each channel's CRC as 4 bytes big-endian, channels in `clip_channels` order. The clip (`selftest/clip.h`, `clip.cu8`; a committed recording with its expected results; do not edit):

| Constant | Value |
|---|---|
| `CLIP_RATE` | 288000 S/s, cu8, 0.6 s |
| `CLIP_CENTER` | 461.1225 MHz |
| `CLIP_CHANNELS` | 5: `SDR13` 461.0375, `SDR04` 461.0875, `SDR06` 461.1375, `SDR08` 461.1875, `SDR10` 461.2375 MHz; all CC 2, slot 1 |
| `CLIP_CHUNK` | 4096 samples |
| `CLIP_RSSI_OFFSET` | -55.0 |
| `CLIP_T0` | 1760000000.0 (Unix s) |
| `CLIP_EXPECT_MESSAGES` / `_BURSTS` / `_CRC` | 53 / 39 / `0x283d7439` |
| `CLIP_NOISE_SEED` / `CLIP_NOISE_GAIN` | 2463534242 / 40 |
| `clip_variants` | `offset` 53 / `0xce0cc52e` / 39; `noise` 53 / `0x67ab9510` / 39; `hot` 53 / `0x75002f1d` / 39 |

Fault generation (`selftest_core.c:sdr_clip_impair`, bit-exact with the variants' expected CRCs), on cu8 pairs with `i = 2·b−255`:

| Fault | Per sample |
|---|---|
| `offset` | rotate (i, q) by `clip_rot[(at/2 + k) % CLIP_ROT_PERIOD]` (Q14 cos/sin, period 72 = +4 kHz); out = `(v + 255·16384 + 16384) >> 15`, 0 if negative, clamped 0..255 |
| `noise` | per byte, xorshift32 (`<<13, >>17, <<5`) from the seed; `r` = sum of its 4 bytes − 510; out = clamp(b + (r·40 >> 8)) |
| `hot` | out = clamp((4·i + 256) >> 1) |

### Verdict

Stored in `g_sstat.selftest` (200 chars, `app.h`), shown in `/api/status` as `selftest` and the page's Self-test row, and logged as `self-test: …`:

```
output <pass|FAIL> / <pass|FAIL|->, transmit <pass|FAIL|->; <total> Mcycles/s of <MHz> on one core (<pct>%): <verdict>
```

| Stage-table total vs CPU MHz (240) | Verdict |
|---|---|
| < 0.70 | `one core keeps up` |
| < 0.95 | `one core is tight: set "dsp_cores": 2` |
| else | `one core can't keep up: set "dsp_cores": 2, or fewer channels` |

### USB spike (`selftest.c:spike_run`, `CONFIG_RDOF_SDR_SPIKE`)

Triggered by `POST /api/spike` (NVS flag `rdof/spike`, restart) or `"spike": true` in the SDR section; runs from the DSP task once the RTL-SDR is open (`sdr_task.c`). Rates 288000, 250000, 240000, 300000, 960000 S/s × transfer plans 8×8192, 4×16384, 16×4096 B, 5 s each at 461.1625 MHz. Results in `g_sstat.spike` (384 chars) as `rate:NxB pct%; …`, in `/api/status` as `spike`. The log line says `Full Speed keeps up` when 288 kS/s reached ≥ 99.5 %, else `SAMPLES LOST at 288 kS/s: try 240000`.

## 4. Log ring

Every ESP-IDF log line (`esp_log_set_vprintf` hook) is copied into a RAM ring as well as printed on the console (UART0, GPIO43 TX / GPIO44 RX).

| Constant | Value | Where |
|---|---|---|
| Size | `CONFIG_RDOF_LOG_RING_KB` KB: Kconfig default 16 (range 0..1024, 0 = off); 16 in `4mb`, 256 in `16mb` | `Kconfig.projbuild`, `sdkconfig.defaults.*` |
| Memory | PSRAM, else internal RAM; no ring if both fail | `logring.c:logring_init` |
| Line cap | 255 bytes per log call (`line[256]`); longer text is cut | `logring.c:ring_vprintf` |
| Default log level | INFO | `sdkconfig.defaults:CONFIG_LOG_DEFAULT_LEVEL_INFO` |

Format: the console text exactly as `vprintf` formats it (the ESP-IDF prefix `I (<ms>) <tag>: `, ANSI color codes if enabled, the trailing newline). Not persistent: lost on any reset. `logring_init` runs right after the LED, before NVS and settings, so it catches the boot.

Read: `GET /api/log` (`web.c:get_log`) returns `text/plain`, `Cache-Control: no-store`, the oldest to newest bytes held (whole ring once wrapped; the first line may start mid-line). Writes and reads take a spinlock (`portMUX`).

## 5. Watchdogs, faults and resets

The firmware sets no watchdog of its own; ESP-IDF's (values from the built `sdkconfig`, IDF defaults, not set in `sdkconfig.defaults*`):

| Item | Value |
|---|---|
| Task watchdog | on, 5 s, watches both idle tasks (CPU0, CPU1); `CONFIG_ESP_TASK_WDT_PANIC` off: a timeout prints the offending tasks, it does not reset |
| Interrupt watchdog | on, 300 ms, both CPUs: resets |
| Panic | print and reboot, 0 s delay (`CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT`) |
| Brownout | detector on, level 7. `4mb` caps Wi-Fi TX power at 10 dBm (`CONFIG_ESP_PHY_MAX_WIFI_TX_POWER`) because the SuperMini's regulator browns out at 15 dBm or more |
| Core dump | to flash, ELF, CRC32, every task (`CONFIG_ESP_COREDUMP_MAX_TASKS_NUM` 64, no DRAM capture): partition `coredump`, 128 KB, at `0x820000` (`16mb`) or `0x3E0000` (`4mb`). Measured: 25 tasks, 46 KB (each task its TCB, the used part of its stack and a register note, about 1.8 KB). A dump that doesn't fit isn't written (the console says `Not enough space to save core dump!`) and the one before stays |

Only the self-test touches the task watchdog (removes core 1's idle task while it runs, §3).

### Reset reason and crash report

`GET /api/status` (`web.c:get_status`) carries:

| Field | Content |
|---|---|
| `uptime_s` | seconds since boot |
| `reset` | `esp_reset_reason()` named: `power on`, `reset pin`, `restart`, `panic`, `interrupt watchdog`, `task watchdog`, `watchdog`, `deep sleep`, `brownout`, `sdio`, `usb`, `jtag`, `unknown` (`web.c:reset_name`) |
| `crash` | only with a core dump in flash: `{"size": <bytes>, "elf": <hex>, "this_image": bool, "task": <15 chars>, "pc": "0x%08x", "exccause": <int>, "cause": <name>, "excvaddr": "0x%08x", "backtrace": "0x… 0x…", "backtrace_corrupted": bool, "details": <abort or watchdog message, when there is one>}` (up to 16 frames) from `esp_core_dump_get_summary` and `esp_core_dump_get_panic_reason`, or `{"size", "error"}` when the dump fails its CRC; read once at boot (`crash.c:crash_init`, which logs it in one line, `W crash: core dump in flash (...)`), cleared by `DELETE /api/coredump`; the dump stays until the next crash overwrites it, so it may come from an earlier image. `elf` is the start of the crashed image's ELF SHA-256 (`CONFIG_APP_RETRIEVE_LEN_ELF_SHA` hex digits; `shasum -a 256` of a build's `.elf`), `this_image` whether that is the running one. Decode the backtrace with `xtensa-esp32s3-elf-addr2line` against that `.elf`, or the whole dump (`GET /api/coredump`) with `esp-coredump` ([reference.md](../../reference.md), "Crashes") |
| `heap_free`, `psram_free` | free internal and PSRAM bytes |

### Firmware-initiated restarts

| Cause | Delay | Where |
|---|---|---|
| settings saved (`POST /api/config`), `POST /api/restart`, `/api/selftest`, `/api/spike`, OTA done | 1 s (`restart_task`) | `web.c` |
| one shared USB port (`usb_either`): a device of the other kind is plugged in while one role runs | 500 ms | `usb_host.c` |
| `ESP_ERROR_CHECK` failure in NVS init, Wi-Fi start, USB host / CDC-ACM install, UART driver install | panic → reboot | `main.c`, `wifi.c`, `usb_host.c`, `uart_modem.c` |

NVS init that finds no free pages or a new version erases NVS and retries (`main.c:app_main`): the stored settings are lost.

Other failures do not reset: a missing OLED retries every 5 s; a missing LED is logged once (`no status LED on GPIO<n>`); a radio or SDR fault is reported through `radio_error` / `rtl_error`, the LED (orange) and MQTT status ([radio.md](radio.md), [sdr.md](sdr.md)).
