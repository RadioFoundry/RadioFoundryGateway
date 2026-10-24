# tinysctp

A compact SCTP: one-to-one associations over UDP encapsulation (RFC 6951), RFC 9260 for one peer address,
with timed partial reliability (PR-SCTP, RFC 3758). Interoperates with usrsctp (HBlink4's `sctp_encap: udp`).
The wire protocol, state machine, timers and congestion rules are in [SPEC.md](SPEC.md).

| Part | Files | Depends on |
|---|---|---|
| Core | `include/tinysctp.h`, `src/sctp.c`, `src/crc32c.c` | C11 and libc only: no sockets, threads or OS. The caller feeds packets and the time; it calls back to send packets and deliver messages |
| UDP glue | `include/tinysctp_udp.h`, `src/udp.c` | BSD sockets (lwIP, or POSIX) |
| ESP-IDF | `include/tinysctp_esp.h`, `src/esp.c` | FreeRTOS: one task per association, sends from any task |
| Tests | `test/` | host tests, a UDP peer, a usrsctp interop script |

## Using it

ESP-IDF: put `tinysctp/` in `components/` (or its parent in `EXTRA_COMPONENT_DIRS`) and add `tinysctp` to
`REQUIRES`.

```c
tsctp_esp_config_t cfg;
tsctp_esp_config_defaults(&cfg);
cfg.host = "server.example.org";        /* NULL: listen */
cfg.encap_port = 9899;                  /* the peer's UDP encapsulation port */
cfg.sctp.peer_port = 62031;             /* the SCTP port it listens on */
cfg.sctp.on_message = on_message;       /* (user, const tsctp_msg_t *) */
cfg.sctp.on_event = on_event;           /* (user, tsctp_event_t, tsctp_reason_t) */
cfg.sctp.reconnect_min_ms = 1000;
cfg.sctp.reconnect_max_ms = 30000;
tsctp_esp_t *a = tsctp_esp_start(&cfg);
tsctp_sendinfo_t si = {.stream = 0, .ppid = 0, .lifetime_ms = 300};
tsctp_esp_send(a, data, len, &si);      /* TSCTP_ERR_STATE until established */
```

Anywhere else (the core):

```c
tsctp_config_t c;
tsctp_config_defaults(&c);
c.local_port = 5000; c.peer_port = 62031;
c.output = my_send; c.output_ctx = sock;   /* one SCTP packet = one UDP payload */
c.random = my_random;                      /* unpredictable 32-bit values */
c.on_message = ...; c.on_event = ...; c.user = ...;
tsctp_t *s = tsctp_new(&c, now_ms());
tsctp_connect(s, now_ms());                /* or tsctp_listen(s) */
for (;;) {
    /* wait for a packet, at most until the deadline tsctp_poll returned */
    tsctp_input(s, pkt, len, now_ms());
    uint64_t next = tsctp_poll(s, now_ms());
}
```

Time is the caller's: milliseconds from any monotonic clock, `uint64_t`. With BSD sockets, `tsctp_udp_open`
then `tsctp_udp_output` / `tsctp_udp_random` in the config and `tsctp_udp_step` in a loop (`test/peer.c` is a
whole program). Host build: `add_subdirectory(path/to/tinysctp)`, link `tinysctp`; `-DTINYSCTP_UDP=OFF` builds
the core alone.

## API

### Core (`tinysctp.h`)

Thread safety: none. One caller at a time per `tsctp_t`. `tsctp_send`, `tsctp_shutdown` and `tsctp_abort` may
be called from inside `on_message` / `on_event` (the packet goes out when the callback returns; a close from a
callback stops the rest of the packet's processing). `tsctp_free` may not.

| Function | Behavior | Errors |
|---|---|---|
| `void tsctp_config_defaults(tsctp_config_t *)` | Zeroes the config and sets the defaults below | - |
| `tsctp_t *tsctp_new(const tsctp_config_t *, uint64_t now)` | Copies and clamps the config (below), allocates the state and a TX buffer of `max(mtu, 1500)` bytes, draws the 16-byte cookie key from `random`; state CLOSED. `now` is unused | NULL: `output` or `random` NULL, or out of memory |
| `void tsctp_free(tsctp_t *)` | Frees everything. No ABORT is sent. NULL is a no-op | - |
| `int tsctp_connect(tsctp_t *, uint64_t now)` | Sends INIT, state COOKIE-WAIT; marks the instance the connecting end (reconnect applies) | `TSCTP_ERR_STATE`: not CLOSED, or `peer_port` 0 |
| `int tsctp_listen(tsctp_t *)` | State LISTEN. After each association ends it returns to LISTEN | `TSCTP_ERR_STATE`: not CLOSED |
| `int tsctp_shutdown(tsctp_t *, uint64_t now)` | Disables reconnect. ESTABLISHED: SHUTDOWN-PENDING, SHUTDOWN once everything sent is acked. COOKIE-WAIT / COOKIE-ECHOED: `tsctp_abort` | `TSCTP_ERR_STATE` in any other state (reconnect is still disabled) |
| `void tsctp_abort(tsctp_t *, uint64_t now)` | Disables reconnect and listening. With a peer tag: sends ABORT; closes with `TSCTP_R_LOCAL_ABORT` (`TSCTP_EV_DOWN`). Without an association: state CLOSED, no event | - |
| `int tsctp_send(tsctp_t *, const void *, size_t, const tsctp_sendinfo_t *, uint64_t now)` | Copies the message into the send queue as DATA chunks of at most `(mtu - 28) & ~3` bytes, then transmits what the windows allow. `info` NULL: stream 0, PPID 0, ordered, reliable | `TSCTP_ERR_STATE`: not ESTABLISHED; `TSCTP_ERR_INVAL`: `len` 0 or `stream >= streams_out` negotiated; `TSCTP_ERR_NOBUFS`: queued + outstanding + `len` > `sndbuf`; `TSCTP_ERR_NOMEM` |
| `void tsctp_input(tsctp_t *, const uint8_t *pkt, size_t len, uint64_t now)` | One SCTP packet (the UDP payload). Packets under 16 bytes or with a bad CRC32c are dropped. Responses and data go out before it returns | - (counted in stats) |
| `uint64_t tsctp_poll(tsctp_t *, uint64_t now)` | Runs due timers, transmits. Returns the next deadline (absolute ms), `UINT64_MAX` if none | - |
| `tsctp_state_t tsctp_state(const tsctp_t *)` | Current state | - |
| `const char *tsctp_state_name(tsctp_state_t)` | `closed`, `listen`, `cookie-wait`, `cookie-echoed`, `established`, `shutdown-pending`, `shutdown-sent`, `shutdown-received`, `shutdown-ack-sent`; else `?` | - |
| `const char *tsctp_reason_name(tsctp_reason_t)` | `none`, `shutdown`, `aborted`, `init timeout`, `peer lost`, `local abort`; else `?` | - |
| `void tsctp_get_stats(const tsctp_t *, tsctp_stats_t *)` | Counters (since `tsctp_new`) and current path values | - |
| `size_t tsctp_status_json(const tsctp_t *, char *, size_t cap)` | One JSON object (SPEC.md, "Status JSON"); returns the length written, truncated to `cap - 1` | 0 on a format error |
| `uint32_t tsctp_crc32c(uint32_t crc, const void *, size_t)` | CRC32c (Castagnoli, reflected 0x82F63B78); `crc` 0 starts one, chainable | - |

Return codes (`int`): `TSCTP_OK` 0, `TSCTP_ERR_STATE` -1, `TSCTP_ERR_NOBUFS` -2, `TSCTP_ERR_INVAL` -3,
`TSCTP_ERR_NOMEM` -4.

| Type | Contents |
|---|---|
| `tsctp_state_t` | `TSCTP_CLOSED` 0, `_LISTEN`, `_COOKIE_WAIT`, `_COOKIE_ECHOED`, `_ESTABLISHED`, `_SHUTDOWN_PENDING`, `_SHUTDOWN_SENT`, `_SHUTDOWN_RECEIVED`, `_SHUTDOWN_ACK_SENT` 8 (ordered: code compares `>= ESTABLISHED`) |
| `tsctp_event_t` | `TSCTP_EV_UP` 1 (established), `TSCTP_EV_DOWN` (closed, with a reason), `TSCTP_EV_RESTART` (the peer restarted; queues dropped, RFC 9260 5.2.4 A) |
| `tsctp_reason_t` | `TSCTP_R_NONE` 0, `_SHUTDOWN` (graceful, either end), `_ABORTED` (peer's ABORT), `_INIT_TIMEOUT` (INIT / COOKIE ECHO unanswered, or too many stale cookies), `_PEER_LOST` (error counter exceeded), `_LOCAL_ABORT` |
| `tsctp_msg_t` | `data`, `len` (valid during the callback only), `stream`, `ppid` (host order), `unordered` |
| `tsctp_sendinfo_t` | `stream`, `ppid` (host order), `unordered`, `lifetime_ms` (0: reliable; else PR-SCTP, counted from `tsctp_send`) |
| `tsctp_stats_t` | Counters: `pkts_in/out`, `bad_checksum`, `bad_vtag`, `ootb`, `msgs_sent/received`, `bytes_sent/received`, `chunks_sent/received`, `dup_chunks`, `chunks_dropped_window`, `retransmits_t3/fast`, `t3_expiries`, `abandoned_msgs` (sent, given up), `abandoned_unsent` (dropped before any of it went out), `fwd_tsn_sent/received`, `skipped_msgs` (peer abandoned), `sacks_sent/received`, `hb_sent/acked/received`, `established`, `restarts`, `failures` (PEER_LOST or INIT_TIMEOUT), `aborts_received`, `send_rejected` (STATE or NOBUFS). Current: `srtt_ms`, `rttvar_ms`, `rto_ms`, `cwnd`, `ssthresh`, `flight`, `peer_rwnd`, `queued_bytes`, `queued_msgs`, `outstanding_chunks`, `error_count`, `local_vtag`, `peer_vtag`, `streams_out/in` (negotiated), `local_port`, `peer_port`, `peer_prsctp` |

`tsctp_config_t`:

| Field | Default | Clamp in `tsctp_new` | Meaning |
|---|---|---|---|
| `local_port`, `peer_port` | 0, 0 | `local_port` 0: random in 49152..65535 | SCTP common header ports. A listener learns `peer_port` from the COOKIE ECHO |
| `streams_out`, `streams_in` | 4, 4 | 0 → 1; `streams_out` ≤ `TSCTP_MAX_STREAMS` (`streams_in` capped when advertised) | Asked for; negotiated = min(ours, peer's, `TSCTP_MAX_STREAMS`) |
| `mtu` | 1200 | ≥ 512 | Largest SCTP packet, without IP and UDP headers. No PMTU discovery |
| `rto_initial_ms`, `rto_min_ms`, `rto_max_ms` | 1000, 1000, 60000 | min ≥ 1; max ≥ min | RTO (SPEC.md) |
| `hb_interval_ms` | 30000 | - | Idle heartbeat every interval + RTO ± RTO/2; 0: none |
| `max_retrans` | 10 | - | Association error limit: one counter for T3 expiries, T2 expiries and unanswered heartbeats |
| `max_init_retrans` | 8 | - | INIT / COOKIE ECHO retransmissions (and stale cookies) before `INIT_TIMEOUT` |
| `sack_delay_ms` | 200 | - | Delayed SACK |
| `sndbuf` | 65536 | - | Bytes queued plus outstanding; also the largest message that can be sent |
| `rcvbuf` | 65536 | ≥ 1500 | Advertised receive window, bytes held |
| `max_message` | 65536 | ≤ `rcvbuf` | Largest message delivered; a larger one is reassembled-and-dropped |
| `cookie_life_ms` | 60000 | - | State cookie lifetime |
| `reconnect_min_ms`, `reconnect_max_ms` | 0, 0 | - | Connecting end only: reconnect after a close (not after a local abort / shutdown), backoff doubling from min to max; min 0: off |
| `output`, `output_ctx` | - | required | Send one SCTP packet |
| `random`, `random_ctx` | - | required | Unpredictable 32-bit values (tags, TSNs, key, jitter) |
| `alloc`, `free` | NULL | - | NULL: `malloc` / `free` |
| `on_message`, `on_event`, `user` | NULL | - | Application callbacks |

### UDP glue (`tinysctp_udp.h`)

One non-blocking UDP socket per association. Not thread-safe; use `tsctp_udp_wait` outside a lock and
`tsctp_udp_drain` inside it, as `esp.c` does.

| Function | Behavior | Errors |
|---|---|---|
| `int tsctp_udp_open(tsctp_udp_t *u, const char *host, uint16_t port, uint16_t local_port)` | `host` set: resolves `host:port` (first `getaddrinfo` result, IPv4 or IPv6), binds `local_port` (0: any). `host` NULL: IPv4 socket on `local_port` that answers whoever sends to it (`learn_peer`; the peer address is taken from each packet until ESTABLISHED, then fixed) | -1: no resolution, socket or bind failure |
| `void tsctp_udp_close(tsctp_udp_t *)` | Closes the socket | - |
| `void tsctp_udp_output(void *ctx, const uint8_t *, size_t)` | `sendto` the peer; counts `sent` / `send_errors`. No peer yet: dropped | - |
| `uint32_t tsctp_udp_random(void *)` | `esp_random()` / `arc4random()` / `/dev/urandom` (falls back to a clock hash) | - |
| `uint64_t tsctp_udp_now_ms(void)` | `CLOCK_MONOTONIC` in ms | - |
| `bool tsctp_udp_wait(tsctp_udp_t *, uint32_t ms)` | `select` for readability, up to `ms` | false on timeout or closed socket |
| `void tsctp_udp_drain(tsctp_udp_t *, tsctp_t *)` | `recvfrom` until empty, `tsctp_input` each (receive buffer 1500 bytes: longer datagrams are truncated and fail the CRC) | - |
| `void tsctp_udp_step(tsctp_udp_t *, tsctp_t *, uint32_t max_wait_ms)` | poll, wait min(next deadline, `max_wait_ms`), drain, poll | - |

### ESP-IDF (`tinysctp_esp.h`)

A task runs the loop: poll, wait min(deadline, 100 ms), drain, poll, under a recursive mutex. All functions
are thread-safe (they take the mutex); callbacks run in the association's task with the mutex held and may
call `tsctp_esp_send`.

| Function | Behavior | Errors |
|---|---|---|
| `void tsctp_esp_config_defaults(tsctp_esp_config_t *)` | `tsctp_config_defaults`, `encap_port` 9899, `task_name` "tinysctp", stack and priority from Kconfig | - |
| `tsctp_esp_t *tsctp_esp_start(const tsctp_esp_config_t *)` | Opens the UDP socket (`host`, `encap_port`, `local_udp_port`), fills `sctp.output` / `random`, `tsctp_new`, then `tsctp_connect` (host set) or `tsctp_listen` (NULL), starts the task | NULL: resolve / socket / memory / task failure |
| `void tsctp_esp_stop(tsctp_esp_t *)` | Stops the task (it sends ABORT if associated), frees everything. Blocks up to one loop turn | - |
| `tsctp_esp_send`, `_shutdown`, `_state`, `_stats`, `_status_json` | The core calls under the mutex, with `tsctp_udp_now_ms()` | as the core |

## Kconfig (menu "tinysctp (SCTP over UDP)")

| Option | Default | Range | Effect |
|---|---|---|---|
| `CONFIG_TINYSCTP_MAX_STREAMS` | 8 | 1..1024 | `TSCTP_MAX_STREAMS`: streams each way, at most. 4 bytes of state each |
| `CONFIG_TINYSCTP_RX_WINDOW` | 64 | 8..4096 | `TSCTP_RX_WINDOW`: receive slots (TSNs) held for reordering / reassembly. 20 bytes each on a 32-bit target, plus the data |
| `CONFIG_TINYSCTP_TASK_STACK` | 4096 | - | The ESP task's stack, bytes |
| `CONFIG_TINYSCTP_TASK_PRIORITY` | 8 | 1..24 | The ESP task's priority |

Host builds take the same two as compile definitions; unset, `TSCTP_MAX_STREAMS` is 16 and `TSCTP_RX_WINDOW` 64
(`src/sctp.c`).

## Footprint

| What | Size (ESP32-S3) |
|---|---|
| Code, all three parts | about 14 KB |
| Static RAM | none |
| Heap per association | about 5 KB (state with a 64-slot window, a 1500-byte TX buffer, the UDP glue's 1500-byte RX buffer) plus queued and held data |

## Tests

```sh
cmake -S components/tinysctp/test -B build/tinysctp && cmake --build build/tinysctp && ctest --test-dir build/tinysctp
sh components/tinysctp/test/interop.sh     # against usrsctp (libusrsctp and an HBlink4 checkout)
```

Also built by `host-tests`. AddressSanitizer and UBSan on (`TINYSCTP_SANITIZE`). 11808 checks in `test_sctp.c`, 69 in `test_udp.c`, 219 in `test_edges.c`.
Line coverage of `src/` on the host (clang, all three): `sctp.c` 99.4%, `udp.c` 99.0%, `crc32c.c` 100%. The lines left are
unreachable: pk_chunk, add_sack and add_fwd checking room that is always there (a control queue of 256 bytes, a SACK of
112 and a FORWARD TSN of 72 fit the smallest packet, 512), a stream table that can't overflow, the `snprintf` failure
of `tsctp_status_json`; and in `udp.c` the check for an address that doesn't fit 28 bytes. `src/esp.c` needs FreeRTOS
and is not built on the host.

`test/test_sctp.c` runs two instances over a simulated link (10 ms one way; RTO 300 / 100 / 2000 ms, heartbeat
2 s, SACK delay 50 ms unless a test changes them) with tunable loss, duplication, reordering, jitter, outages
and one-way blocks:

| Test | Pins |
|---|---|
| `test_crc32c` | CRC32c: "123456789" → 0xE3069283; RFC 3720 B.4 vectors (32 zeros, ones, incrementing, decrementing) |
| `test_formats` | Byte by byte: INIT (44 bytes, tag 0, a_rwnd 65536, 4/4 streams, 0xC000 and 0x8008 params), INIT ACK (tag = initiate tag, cookie param 64 bytes), COOKIE ECHO (60 cookie bytes verbatim), COOKIE ACK; DATA (flags B+E 0x03, length 21, padding, stream / SSN / PPID); delayed SACK; a 2500-byte unordered message in three fragments (flags 0x06, 0x04, 0x05, consecutive TSNs) reassembled; HEARTBEAT (length 24, info param) and its ACK; RTT measured; `ERR_INVAL` for a bad stream and an empty message; a corrupt packet counted; the status JSON |
| `test_unknown_chunks` | Unknown chunk types: after 0x85 (skip) the following DATA is delivered; after 0x45 (stop, report) it isn't |
| `lossy_run` ×5 | Clean; 10% loss; 20% loss + dup + reorder; burst sending at 10% loss; unordered at 15% loss: every message once, per-stream order (ordered runs), nothing left queued or outstanding |
| `test_big_lossy` | 20 messages of 6000 bytes over 10% loss reassemble intact |
| `test_echo` | Echo both ways over a lossy link |
| `test_prsctp` | 250 ms lifetime at 30% loss: PR-SCTP negotiated; most delivered, some abandoned; FORWARD TSN sent and received; still ordered, no duplicates; nothing arrives later than lifetime + 400 ms; nothing stuck; 100 reliable messages after it all arrive |
| `test_expire_unsent` | Expiry drops unsent messages (`abandoned_unsent`) and abandons sent ones (`abandoned_msgs`); association stays up |
| `test_outage` | A 3 s outage is ridden out; a long one ends in `PEER_LOST` both ends (server by heartbeat), the server listens again, reconnects time out (`INIT_TIMEOUT`) during it, then the client reconnects and data flows |
| `test_init_timeout` | No answer: `INIT_TIMEOUT`, state CLOSED |
| `test_shutdown_abort` | Shutdown from either end delivers everything first, both see `SHUTDOWN`, sends rejected meanwhile; abort gives `LOCAL_ABORT` / `ABORTED`; the server listens again |
| `test_restart` | Client restarts from the same port: the server sees `EV_RESTART`, not a down; server restarts: the client gets an ABORT (T bit), then reconnects |
| `test_stale_cookie` | A cookie held past its lifetime: Stale Cookie error, a fresh INIT at once, up without waiting for T1 |
| `test_collision` | Both ends INIT at once: both up, tags agree, data both ways |
| `test_ootb` | Out of the blue to a fresh listener: ABORT with T bit and the packet's tag; wrong tag to a live association: dropped, `bad_vtag` counted |
| `test_callbacks_send` | `tsctp_send` from inside `on_event` and `on_message` works both ways |

`test/test_udp.c` (`tinysctp_udp_tests`) runs `src/udp.c` over real loopback sockets:

| Test | Pins |
|---|---|
| `test_open` | IPv4 and IPv6 literals, a name (`localhost`); no descriptor left (`RLIMIT_NOFILE`): -1; `peer`, `peer_len`, `learn_peer`; the struct cleared; the local port honored; bind failure on a port in use, an unresolvable host: -1, `fd` -1; a listener is IPv4 and has no peer yet; a second close is harmless; a packet reaches an IPv6 peer |
| `test_output` | The datagram is the packet byte for byte; `sent`; a 70000-byte packet is a `send_errors`; a listener with no peer, a closed socket: silent |
| `test_clock_random` | The clock follows real time; `random` varies |
| `test_wait_drain` | `wait` times out, then sees a datagram and keeps seeing it until read; a closed socket never waits |
| `test_association` | Two associations through the glue: a stray datagram before the INIT sets the listener's peer; established over loopback; the listener's peer becomes the connector's port; 300-byte and 4000-byte (fragmented) messages both ways; after ESTABLISHED a datagram from another port changes nothing (still counted and passed in); a 3000-byte datagram is cut to the 1500-byte buffer and fails the CRC (`bad_checksum`); `drain` empties the socket; a graceful shutdown |
| `test_step` | An idle step waits the cap; a step with a timer due ends at the timer and the INIT is resent; a datagram ends the wait |

`test/test_edges.c` (`tinysctp_edge_tests`) feeds hand-built packets to one or two endpoints (a manual clock, a counting
allocator that can fail):

| Test | Pins |
|---|---|
| `test_config`, `test_alloc_failures`, `test_no_leaks` | Config clamps (MTU 512, one stream, RTO limits, `rcvbuf` 1500 raises `a_rwnd`, `max_message` follows it, 16 streams, a drawn local port); `tsctp_new` without `output` / `random` or without memory; every allocation freed after a full session, a held cookie, and a send that fails halfway |
| `test_api_errors` | Return codes of connect / listen / shutdown / abort / send in each state; `tsctp_free(NULL)`; names for unknown reasons and states; the status JSON cut to `cap - 1` |
| `test_init_rejects`, `test_init_params` | INIT with a tag, too short, tag 0, no streams: dropped; INIT to an idle endpoint: ABORT; PR-SCTP from either parameter; unknown parameters (every pair of high bits) ignored, none reported; a length under 4 or past the end ends the list |
| `test_init_ack` | INIT ACK too short, tag 0, no streams, no cookie, empty or oversized cookie, wrong tag, a second one, no memory for the cookie |
| `test_cookie_echo_rejects`, `test_cookie_collisions` | Wrong length, magic, MAC, tag, port, another instance's cookie, a cookie from the future; the collision cases B (while echoing, and when already up), D and C |
| `test_input_filters` | Wrong ports, chunk lengths under 4 or past the end, ABORT / COMPLETE with the wrong tag (T bit too), SACK and HEARTBEAT ACK malformed or unsolicited, OOTB packets (no reply to ABORT / COMPLETE / COOKIE ACK / ERROR; COMPLETE for SHUTDOWN ACK; ABORT for the rest) |
| `test_init_in_states` | INIT from another port while associated; INIT while echoing (tie-tags in the cookie); a random source that returns 0 |
| `test_data_refused`, `test_data_alloc_fail`, `test_control_overflow` | DATA with no data, to a stream that isn't there (ERROR, still acknowledged), beyond the window, over `max_message`, an ordered message ahead of its SSN, no memory for the chunk or to join fragments; more HEARTBEATs or reports than the 256-byte control queue holds |
| `test_rx_edges` | A message of more fragments than the receive window can hold is never completed (SPEC.md, Receiving); a callback that closes the association stops the delivery; FORWARD TSN too short, at the cumulative ack, skipping a gap and freeing a dead fragment, and delivering a message whose callback closes |
| `test_send_edges`, `test_fast_retransmit_squeeze` | The peer's window closing; an expired unsent message of several fragments dropped whole; FORWARD TSN lists ordered streams once each and not unordered ones, and the peer follows; a forced fast retransmit that does not fit beside a pending SACK |
| `test_error_chunks` | ERROR causes while echoing: skip to the next, a broken length, a stale cookie with the INIT budget spent (`INIT_TIMEOUT`) |
| `test_shutdown_races` | Both ends shut down at once; data arriving in SHUTDOWN-SENT; SHUTDOWN and SHUTDOWN ACK repeated into silence until `PEER_LOST` |

Simulated results (this run of the host tests; RTO min 100 ms, 10 ms one way):

| Run | Msgs | SRTT | T3 / fast rtx | Max latency |
|---|---|---|---|---|
| clean | 500 | 20 ms | 0 / 0 | 10 ms |
| 10% loss | 2000 | 20 ms | 112 / 167 | 630 ms |
| 20% loss, dup, reorder | 2000 | 39 ms | 438 / 284 | 903 ms |
| burst, 10% loss | 3000 | 25 ms | 1088 / 222 | 1625 ms |
| unordered, 15% loss | 1500 | 35 ms | 252 / 177 | 568 ms |
| PR-SCTP 250 ms, 30% loss | 1400 of 1500 delivered | - | - | 353 ms |

`test/interop.sh` runs `usrsctp_echo.py` (HBlink4's `usrsctp_transport.UsrsctpListener`, echoing every message;
SCTP port 62099, UDP 9899) and `tinysctp_peer` (`test/peer.c`) against it three times: clean (500 messages), then
through `lossy_proxy.py` (15% drop, 3% dup, 5% reorder, 10 ± 10 ms, a 3 s outage at 6 s; 1000 messages), then the
same with a 300 ms lifetime. Pass: every message echoed (exit 0).

## Tuning

For real-time traffic over Wi-Fi: RTO min 200 to 300 ms, max a few seconds, fewer retransmissions and a message
lifetime. `mtu` 1200 suits Wi-Fi and most tunnels.

## License and provenance

The repository's license. No third-party code: SCTP from RFC 9260 / 3758 / 6951, SipHash-2-4 from its paper,
CRC32c from RFC 9260 appendix A (nibble table generated from the polynomial). `PROVENANCE.md` lists it as clean.
