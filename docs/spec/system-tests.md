# System tests

How the gateway is tested end to end: a RadioFoundry radio, the gateway, and the network it reports to (an MQTT
broker, an HBlink4 master), as one system. The unit tests (`host-tests/`) check each module against golden vectors;
these check that the pieces work together, over the same links they use in the field, including when those links
misbehave.

## Goals

- **The real software on every side.** The radio runs RadioFoundry's own MMDVM and hotspot core (`core/hotspot`), not
  a fake that answers the way we think it does. The gateway runs the same code the ESP32 runs, not a copy of its
  wiring written for the tests. The network side is a real mosquitto and a real HBlink4 where it matters.
- **Deterministic first.** A failure must reproduce: the fast level runs on a virtual clock, with every byte between
  the processes routed by the test, and seeded randomness.
- **Faults on purpose.** Loss, delay, reordering, outages and disconnects on each link, from a seed.
- **One scenario format** for every level, down to a board and a radio on the bench.

## The system under test

```
  simulated air  ──  RadioFoundry radio  ── MMDVM serial ──  gateway  ── UDP (DMRD, SCTP optional) ──  HBlink4 master
  (calls in, what       (hotspot core on                       (node: modem, roamer /                     │
   it transmits out)     the simulated modem)                   repeater, tracker)  ── MQTT ──  broker   peers
```

What crosses each link is what a test checks:

| Link | Out of the gateway | Into the gateway |
|---|---|---|
| serial (MMDVM) | the modem's commands: mode, frequency, DMR data to transmit | version, status, received bursts, ACK / NAK |
| HBlink4 (DMRD, DMRC, DMRT, DMRK) | bursts heard on RF, the peer's config, keepalives | network calls, tune requests |
| MQTT | call start / end JSON, raw bursts, status, the receiver message | (nothing yet) |
| air | bursts the radio transmits, with their slot and timing | calls from simulated subscriber radios |

## Levels

### Level 1: simulated, virtual time

Separate processes, one test driver.

- **`radiosim`**: one RadioFoundry radio as a process. RadioFoundry's real `core/hotspot` (mmdvm.c, hotspot.c,
  hotspotUplink.c, hotspotData.c, and their DMR code) on its simulated modem (`tests/host/sim/dmr_sim_modem.c`),
  driven over stdin / stdout as RadioFoundry's own `splitSim` is: set and advance the modem clock, MMDVM frames in,
  bursts from the air in; frames out to the host, bursts on air out. A process, because the core is a singleton and
  because the gateway carries its own copies of some of RadioFoundry's files (`common/src/radiofoundry/`), which
  would collide in one binary.
- **`nodesim`**: the gateway's node (see "The node" below) as a process, on a virtual clock, with its network
  through the driver: MMDVM bytes in and out, DMRD datagrams in and out, MQTT publishes out (topic, payload, QoS,
  retain), the connection states set by the driver.
- **The driver** (Python, `systest/`): owns the clock and advances both processes in 5 ms steps; carries the serial
  bytes between them through a link model (latency, jitter, drops); plays the network: an in-memory broker that
  records publishes, and an HBlink4 master model that routes DMRD between the gateway and test peers; plays the
  air: subscriber radios that key up with a given ID, talkgroup and slot, and a receiver that decodes what the
  radio transmits with an independent decoder (`tools/bench/benchapi.py`'s, and the ETSI patterns).

Fast (seconds per scenario) and exact: the same seed gives the same bytes. Runs in CI on every commit, Linux and
macOS.

### Level 2: real processes, real time, real services

- the same `radiosim`, serving MMDVM on a pseudo-terminal as a radio's USB serial port appears;
- the POSIX gateway app (`posix/`), with the real node, opening that terminal, its real sockets and MQTT client;
- a real mosquitto and a real HBlink4, each started per test on free ports with a generated config;
- fault proxies in each path: UDP (gateway to HBlink4: loss, delay, reorder, outage), TCP (gateway to the broker:
  disconnects, the reconnect backlog), serial (bytes dropped, late, the cable pulled);
- test peers: an MQTT subscriber, and an HBlink4 peer of our own that sends and receives calls.

Slower and timing-tolerant (checks allow for scheduling); catches what the simulation can't: socket behavior,
reconnects, HBlink4's real routing and timeouts, broker flow control. Runs per pull request and nightly.

### Level 3: hardware in the loop

The same scenarios against a board and a radio on the bench: the board's bench API (`docs/spec/bench-api.md`)
to drive and watch it, `hil-tests/` for the board's own checks, and an SDR to hear the air.

## The node

The wiring between the modem, the roamer or repeater, the tracker, the HBlink4 peer and MQTT is today in
`esp_idf/main` (`roamer.c`, `radio_task.c`, `roam_hblink.c`, `roam_mqtt.c`), on FreeRTOS tasks and ESP sockets.
It moves to a portable layer, `node/`: plain C with a platform interface (time, sending a datagram, publishing,
the serial port, a lock), driven by calls (bytes in, a datagram in, a poll at a time). The ESP32 app, the POSIX
app and `nodesim` each supply the platform; the node is the same code in all three. That is what lets these tests
say something about the firmware.

## Scenarios

A scenario is a file: the box config, the radios on the air, the faults, a timeline, and what must be seen.

```yaml
name: rf call up to the network
config: roamer-simplex.json        # a box config, as the board takes it
seed: 7
air:
  - at: 1.0
    radio: {id: 9990003, slot: 1, tg: 9, cc: 1}
    voice: 3.0                      # seconds
expect:
  mqtt:
    - {topic: "DMR/{name}/.../json", kind: call_start, src: 9990003, dst: 9, slot: 1}
    - {kind: call_end, src: 9990003, duration: [2.9, 3.2]}
  hblink:
    - {stream: 1, src: 9990003, dst: 9, slot: 1, frames: [50, 52], lc: same}
  air: none                         # the radio transmits nothing back
```

The first set:

| Scenario | What it checks |
|---|---|
| RF call up | a subscriber's call: tracked, published (start, end, raw bursts), sent to HBlink4 whole and in order |
| network call down | a call from a peer through HBlink4: tuned, transmitted, decoded on the air bit for bit |
| both slots | two calls at once, one per slot, each whole on its own slot |
| roaming | a DMRT tune: the radio retuned, the call carried, back to idle |
| repeater | duplex: RF in, repeated out, hang time, a network call yielding to RF |
| SFR | single frequency repeater framing and slot use |
| split duplex | two radios as TX and RX halves (with RadioFoundry's `splitSim` and its clock model) |
| radio unplugged | the serial link drops mid-call and returns: the call ends cleanly, the modem comes back |
| broker away | MQTT down for 30 s: what's kept, what's dropped and counted, the backlog on reconnect |
| HBlink4 away | the master restarts: the peer logs back in, calls resume |
| lossy network | 15% loss and reordering to HBlink4: calls still whole where the protocol allows |

## radiosim

`systest/radiosim/radiosim.c` is our glue (MIT) around RadioFoundry's sources, compiled from `RADIOFOUNDRY_DIR`
at build time (the list is in `systest/CMakeLists.txt`); nothing of RadioFoundry's is copied here. Its protocol, one
request per line on stdin, each answered by its output lines and a line `.`:

| Request | |
|---|---|
| `t <ms>` | advance the clock to ms (absolute; it starts at 1000): a poll every 5 ms, a modem slot every 30 ms |
| `h <hex>` | bytes from the host on the serial port, in any split (framed by RadioFoundry's framer) |
| `r <ts> <kind> <index> <sync> <cc> <payload hex> [<lc hex>]` | a burst heard on air (RadioFoundry's `dmr_burst_t`) |
| `k <knob> <0\|1>` | a simulated modem knob: `refuse_tx`, `repeater`, `stalled` |
| `q` | quit |

| Output | |
|---|---|
| `o <ms> <hex>` | a frame the radio wrote to the host |
| `a <ms> <slot> <kind> <index> <sync> <lcss> <emb hex> <payload hex>` | a burst it sent on air |
| `e <ms> <what>` | the radio core was asked: `enter <rx_hz>`, `freq <rx_hz> <tx_hz>`, `power <level>`, `leave` |

Its one argument is the GET_VERSION description (default `RadioFoundry RadioSim 00000000`).

`systest/radiolink.c` runs it from a test: requests, and callbacks for what comes back. `test_radiosim.c` gives
the gateway's `rdof_modem` a port whose writes go to radiosim and whose waits advance its clock, then checks:

- detection: GET_VERSION, and the gateway calls it a `radio_hotspot`;
- tuning as the roamer does (`rdof_radio_tune`: SET_FREQ, SET_CONFIG, SET_MODE), then a status; the radio core
  was asked to enter the hotspot and tuned to the frequency;
- a call down: a voice LC header, two superframes of voice and a terminator as DMR_DATA2; on air, the header and
  terminator carry the call's LC and the twelve voice bursts go out in order, A to F;
- a call up: a subscriber's header, a superframe and a terminator heard; the gateway gets them all on slot 2, the
  header's and terminator's LC intact.

## Dependencies

- **RadioFoundry** (MIT, <https://github.com/RadioFoundry/RadioFoundry>): its `core/` and `tests/host/sim/` are built into `radiosim`, from a
  checkout given as `RADIOFOUNDRY_DIR`, at the commit in `systest/radiofoundry.ref` (CI checks out the same one).
  Without it the tests are skipped. Test only: the gateway itself doesn't need it.
- **HBlink4** (GPL-3.0): level 2 runs it as a separate process; nothing is copied from it. Test only.
- **mosquitto** (EPL-2.0 / EDL-1.0): level 2 runs it as a separate process. Test only.

## Order of work

1. `radiosim`, with a smoke test: the gateway's modem code talks to it and identifies it as a RadioFoundry radio.
   Done: `systest/radiosim/radiosim.c`, `systest/radiolink.c`, `systest/test_radiosim.c` (below).
2. `node/`: the wiring moved out of `esp_idf/main`, the ESP32 app on it, checked on a board.
3. `nodesim` and the driver: level 1, with the first scenarios.
4. The POSIX app, then level 2 with mosquitto, HBlink4 and the fault proxies.
5. The scenario format against the bench (level 3).
