# gnss

A GNSS receiver's NMEA on a UART, for ESP-IDF: the fix, position and satellites for a status, and the time of
each second for a clock. No other component needed; [netclock](../netclock/README.md) takes the time
(`gnss_start(netclock_gnss_time)`), and its pulse per second then sets the clock with no NTP or browser.

| Part | Files | Builds on |
|---|---|---|
| Portable (plain C11) | `include/gnss_nmea.h`, `src/gnss_nmea.c` (parser, burst timing), `src/gnss_json.c` | any; the static library `gnss_portable` when `ESP_PLATFORM` is unset |
| ESP-IDF glue | `include/gnss.h`, `src/gnss.c` (the UART reader task) | ESP-IDF 5.x (`esp_driver_uart`) |

Tests: `test/test_gnss.c` (in the gateway's host tests; standalone: `cmake -S components/gnss -B build/gnss &&
cmake --build build/gnss && ctest --test-dir build/gnss`).

## Use

```c
gnss_start(netclock_gnss_time);       // pins and baud from Kconfig; ESP_ERR_NOT_SUPPORTED with no RX pin
char buf[900];
size_t n = gnss_status_json(buf, sizeof buf);
```

## The parser (`gnss_nmea.h`)

One line at a time into a running state. NMEA 0183: `$`, the address (talker + type), comma fields, `*`, two hex
digits of the XOR of everything between. A line with a bad checksum, over 100 characters, or a field that doesn't
parse counts as `bad`.

| Sentence | Used |
|---|---|
| RMC | time, status (A: a fix), position, speed (knots), course, date (ddmmyy, years 2000+) |
| GGA | time, quality (0 none, 1 GPS, 2 DGPS, 4/5 RTK, 6 estimated), satellites used, HDOP, position, altitude (MSL) and geoid height |
| GSA | mode (1 none, 2 2D, 3 3D), PDOP, HDOP, VDOP |
| GSV | satellites in view, per talker (GP, GL, GA, BD/GB, GQ, GI; 6 kept) |
| ZDA | time and date |
| TXT | the receiver's own messages (the last kept: an L76K's `ANTENNA OPEN` / `OK`) |

Others count as `other`. Coordinates are ddmm.mmmm with a hemisphere; south and west are negative.

`gnss_nmea_utc` gives a time only when the last RMC had status A and its date is 2025 or later
(`GNSS_NMEA_MIN_YEAR`): before a fix, a receiver gives its own clock's time (an L76K: 1980-01-06), and an old date
means a week-number rollover. A GGA fix alone isn't enough: its time has no date with it.

## The time of a second (`gnss_burst_t`)

A receiver sends one burst of sentences a second, some ms after the second starts (and after its pulse). The burst's
first byte is the reference. Bytes arrive in chunks: each chunk's first byte is worked back from when its read
ended, its size and the baud (10 bits a character). A gap over 100 ms (`GNSS_BURST_GAP_US`) starts a new burst;
`gnss_burst_time` gives each burst's second once, after its first time sentence. A second that changes with no gap
before it (a link too slow for its sentences) counts as `no_gap` and isn't given.

The reader takes UART events with an RX threshold of 8 bytes and an idle timeout of 2 characters, so a chunk's time
is within a few ms. With a pulse, netclock measures the lag itself (the W12's L76K: 29 ms).

## The reader (`gnss.c`)

A task (`gnss`, 3.5 KB stack, priority 9, core 0 by default) on the UART. Its state and buffers are in PSRAM when
there is some. With `GNSS_AUTODETECT`, when no good sentence comes for 3 s it tries the next of: the pins as set
and swapped, at the configured baud, 9600, 115200, 38400, 57600 and 4800 (`tries` counts them). Once found it stays;
3 s of silence later starts the search again.

## Status (`gnss_json`)

| Field | |
|---|---|
| `running`, `found` | the reader is up; good sentences on the current settings |
| `rx_gpio`, `tx_gpio`, `baud` | what it's on now |
| `fix` | `none`, `fix` (no GSA yet), `2d`, `3d` |
| `quality`, `sats_used`, `sats_in_view`, `constellations` | GGA's quality and count; GSV's in view, total and by talker |
| `hdop`, `pdop`, `vdop` | null until given |
| `lat`, `lon` (7 decimals), `alt_m`, `geoid_m`, `speed_kmh`, `course` | null without a fix |
| `utc_s`, `utc` | the fix's second (`gnss_nmea_utc`), and as ISO 8601; null without |
| `txt` | the receiver's last TXT |
| `last_s` | since the last good sentence, -1 never |
| `bytes`, `lines`, `sentences`, `bad`, `other` | counters |
| `bursts`, `no_gap`, `overflows`, `tries` | bursts timed; seconds with no gap; UART overflows; autodetect steps |

The short form, `gnss_json_append` (`gnss_status_append` with the reader's status), puts
`,"gnss":{"fix","sats","lat","lon","alt_m","hdop"}` into another JSON object before its closing brace, when the
receiver is found; the gateway adds it to its retained MQTT status messages, so the boards' positions can go on a
map.

## Kconfig

| Option | Default | |
|---|---|---|
| `GNSS_UART_RX_GPIO` | -1 | the ESP's receive pin (the receiver's TX); -1 builds the reader out |
| `GNSS_UART_TX_GPIO` | -1 | the ESP's send pin (nothing is sent yet) |
| `GNSS_UART_NUM` | 1 | |
| `GNSS_BAUD` | 9600 | most receivers' default |
| `GNSS_AUTODETECT` | y | swapped pins and other bauds when nothing parses |
| `GNSS_TASK_PRIORITY`, `GNSS_TASK_CORE` | 9, 0 | |

The days-from-date arithmetic is Howard Hinnant's public-domain `days_from_civil`.
