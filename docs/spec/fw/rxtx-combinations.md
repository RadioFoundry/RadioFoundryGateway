# Devices, resources and units: any receivers, any transmitters

What: how RadioFoundry Gateway runs a mix of receivers and transmitters (an SDR, split-duplex radios, an SDR plus a
hotspot, an SFR radio) and how bench requests share the same hardware: what's built, where the limits are, and
what's left (several SDRs on a board, an SDR sending two or more carriers, the bench picking hardware). The settings
are [config-v3.md](config-v3.md).
Sources: `common/src/siteplan.c` (the units and the checks), `common/src/{config,split,benchtx,benchcfg,txpool}.c`,
`common/include/rdof/{siteplan,config,modem,split}.h`, `sdr/src/{sdrmodem,mod}.c`,
`sdr/include/rdof/sdr/{frontend,sdrmodem,mod}.h`,
`esp_idf/main/{settings,roamer,split_port,modem_sdr,sdr_task,bench_api,bench_tx,bench_radio}.c`,
`sdr/cli/rdofradio.c`.
Related: [config-v3.md](config-v3.md), [transmitters.md](transmitters.md), [sdr-tx.md](sdr-tx.md),
[sdr-device-api.md](sdr-device-api.md), [../bench-api.md](../bench-api.md), [../repeater.md](../repeater.md),
[../slot-timing.md](../slot-timing.md) ("Remote sites").

## 1. Where the limits are today

| Limit | Where | Kind |
|---|---|---|
| 2 units that transmit and 1 SDR on a board | `RDOF_MAX_RADIOS` 2 (`config.h`); `rdof_roles_t` has one `sdr`; `rdof_plan_roles` | board |
| One SDR modem; one owner | `modem_sdr.c` (`mdm`, `owner`) | wiring (board) |
| One receiver and one front end | `sdr_task.c` (`g_rx`, `fe`) | wiring (board) |
| A split: one USB half | `split_port.c` (a USB half takes the repeater's USB device slot) | wiring (board) |
| The SDR as a split half on the board | `split_port.c:split_modem_start`: the sample clock isn't stamped against `esp_timer`. The open refuses an SDR as a split's TX half anywhere | missing feature |
| One TX source per front end | `frontend.h` `tx_set(src)` | API |
| One carrier per modulator | `mod.h` one `offset_hz` / `phase`; `sdrmodem.h` one `rdof_mod_t` | by design (keep) |
| One carrier per SDR | every driver's carriers are 1 (`siteplan.c:sdr_caps`); `tx.carriers` above it is refused | missing feature (§5) |
| RX and TX from two devices only through a split | `split.h`: `halves[2]` behind one `rdof_modem_t` | by design (keep) |
| The bench: one TX job, `via=sdr\|modem\|radio:NAME` | `bench_tx.c`, `benchtx.c` | API |
| Host tool: one SDR, one unit that transmits (on the SDR, a split, or an MMDVM modem with the SDR receiving), or up to 8 split repeaters on the SDR, each with its own radio | `rdofradio_cfg.c`; `--split-tx`, `--mmdvm`; `sdrsplit.c` | wiring (host) |

Already general, and kept:
- `rdof_sdr_rx_t` is multi-channel (up to `RDOF_SDR_MAX_CHANNELS` 16, channels plus taps), with one
  `on_burst(ctx, channel, ...)`.
- `rdof_sdrmodem_t` keeps all its state in its struct. Several can share one receiver, each on its own channel.
- `rdof_split_t` composes any RX-capable MMDVM half with any TX-capable one. The SDR modem works as either half.
- The repeater and the transceiver talk MMDVM to one duplex modem, whatever is behind it.
- The transmitter pool (`txpool.c`) hands a call's transmission to a free transmitter on its frequency, and an SDR
  transmitter is tuned per stream ([transmitters.md](transmitters.md)).
- The bench has sessions, leases on frequencies, a queue with `busy` and positions, and TX gates.

## 2. Model

**A device** is one piece of hardware the gateway opens, named once in the settings ([config-v3.md](config-v3.md)
§4): an `sdr` (a sample stream in, and out if it can transmit), a `modem` (anything that speaks MMDVM on a serial
port) or a `radio` (a RadioFoundry radio on its debug link, for the bench). What a modem can do (duplex, `SPLIT1`,
`DCDM`) comes from its description at run time.

**A resource** is one thing a device gives:

| Resource | From | How many | Shared? |
|---|---|---|---|
| RX window | `sdr` | 1: the tuned center and rate | yes, while no one retunes it |
| RX channel | `sdr` | up to `RDOF_SDR_MAX_CHANNELS` in the window (demod, taps) | yes: any number of listeners |
| TX carrier | `sdr` that transmits | `tx.carriers` (1 for now, §5) | no; the pool tunes it per stream |
| whole modem | `modem` | 1: duplex or simplex; its RX and TX can't be used apart | no |
| split half | `modem` with `SPLIT1` | RX or TX, one at a time | no |
| link | `radio` | 1 | no |

**A unit** is what runs. The open (`rdof_plan_open`) makes them from the settings; nothing in a file names them
([config-v3.md](config-v3.md) §12):

| Unit | Takes |
|---|---|
| repeater | one RX and one TX: one modem or the SDR, or a split (an RX half and a TX half) |
| SFR, DCDM | one hotspot-mode modem (the radio times its own slots); DCDM `duplex: full` on a full-duplex SDR |
| transceiver | one modem, or the SDR's carrier tuned per stream |
| receiver | RX channels on the SDR |
| trunked site (later) | a control channel's RX and TX plus a pool of RX / TX pairs |
| simulcast, voting (later) | one RX and several TX, or several RX and one TX ([../slot-timing.md](../slot-timing.md)) |

**How RX and TX meet.** A unit on one modem talks to it directly. A unit whose RX and TX are on two devices is a split
(`rdof_split_t`), built by the runtime from the devices' `hears` and `transmits`. The pairs:
- an SDR channel and a radio's TX half (the host, `rdofradio --split-tx`; not on the board yet);
- two radios (built);
- a radio's RX half and an SDR carrier (refused: the SDR's TX timing against the split's clock isn't done);
- a channel on one SDR and a carrier on another (later: one SDR a board).

A unit on one SDR's channel and its carrier runs the SDR modem in duplex. A transceiver on the SDR is the box's
transmitter: the router tunes it per stream, and the receiver unit reports what's heard.

### 2.1 Frequencies and how they're used

What uses a frequency comes from the apps and devices, not from a mode on the frequency, so one frequency may serve
several uses at once:

| Use | What happens on the frequency | Settings |
|---|---|---|
| simplex | heard and reported; the network's calls for it sent on it, half duplex | a frequency no app takes, that a device `transmits` on |
| receive only | heard and reported, never sent on (a repeater's output, monitored) | `"tx": false` |
| repeater input | a repeater hears its users there | a repeater app's `in` |
| repeater output | a repeater transmits there | a repeater app's `out` |
| SFR | one frequency, both directions, slots timed by the radio | `"mode": "sfr"` |
| DCDM | one frequency, two slots, two directions | `"mode": "dcdm"` |

On an SDR the receiver hears every frequency in its window at once, so an SDR that transmits is simplex on all of
them. An SDR has no scan for now: a scan would let it hear more than its window holds (config-v3.md §14).

## 3. The checks (the broker, dropped)

The plan was a resource broker (`rdof_broker_t`): a table of resources and owners, its dry run the settings check.
It was built for the version 2 roles and removed with them (2026-11-01). Its checks are the open's now, in
`siteplan.c` ([config-v3.md](config-v3.md) §13.1):

| Broker check | Now |
|---|---|
| two roles on one modem | `check_units`: "a modem serves one"; a bench modem serves nothing else |
| too many carriers | `check_caps:sdr_sends`: `tx.carriers` over the driver's, more transmitting units than carriers |
| a frequency plan that doesn't fit a window | `check_caps:sdr_hears`: the rate, the tuning range, the window when `center_hz` is set, 16 channels |
| a TX on a receive-only device | `sdr_sends`: an RTL-SDR can't transmit |
| a pairing with no clock | `check_units`: an SDR as a split's TX half (the board's RX half: `split_port.c`) |
| platform caps | `rdof_plan_check` (the board's UARTs, a host's paths and drivers), `rdof_plan_roles` (2 units that transmit, one SDR) |

Picking a transmitter at run time is the router's and the pool's ([transmitters.md](transmitters.md) §4): a free
transmitter already on the frequency first, an emergency over a call that isn't one.

### 3.1 The bench picking hardware (plan, not built)

The bench API keeps its shape (sessions, leases, gates). What would change is how a request names the hardware:
- **`via=device:NAME`** names a device. The current forms stay as aliases: `via=radio:NAME` (a `radio` or `modem`
  device), `via=modem` (the SDR modem, through the pool when a unit owns it), and `via=sdr`.
- **`via=auto`** picks the hardware for the job (TX bursts, a capture, a bursts stream, I/Q) by frequency and caps.
  The ack and `done` say which device ran it.
- **A unit's resources are off limits to the bench**, except shared RX channels and the pool's transmitters, which
  the bench uses as one more stream.
- **`GET /info`** gains `devices`: each device with its resources, caps and owners. The current `sdr`, `radios` and
  `channels` stay, built from that list.
- **A queued request** is a lease plus a job, waiting in line (`queued`, with its position) as the lease does today.

Nothing here is new protocol, only new values. A client that sends `via=auto` to an older board gets `bad_request`
and can fall back.

## 4. Settings

The settings are version 3: [config-v3.md](config-v3.md). The devices are §4, what runs §12, the checks §13.1 and
the schema §13.2.

## 5. SDR transmit: carriers (not built)

`rdof_mod_t` stays one carrier. Several carriers need a mixer between the modems and the front end:
**`rdof_sdr_tx_t`** (planned, `sdr/`), the front end's one TX source, holding up to 4 carriers, each a pull from one
SDR modem at an offset from the TX center:

```c
int  rdof_sdr_tx_claim(rdof_sdr_tx_t *t, double offset_hz, const rdof_fe_tx_source_t *src); /* carrier id, or < 0 */
int  rdof_sdr_tx_retune(rdof_sdr_tx_t *t, int id, double offset_hz);
void rdof_sdr_tx_release(rdof_sdr_tx_t *t, int id);
const rdof_fe_tx_source_t *rdof_sdr_tx_source(rdof_sdr_tx_t *t);  /* what tx_set gets */
```

- **One carrier** passes straight through, so output, timing and CPU stay the same. The golden tests stay byte for
  byte.
- **More than one:** each carrier renders into its own buffer and the mixer adds them at 1/N. A carrier that isn't
  keyed adds nothing; the sum is keyed when any carrier is.
- **The settings are ready:** `tx.carriers` takes 1 to 4, and the open refuses more than the driver's
  (`siteplan.c:sdr_caps`, 1 for every driver) and more transmitting units than carriers. Raising a driver's count is
  the only change there.
- **The SDR modem** would claim its carrier; its `tx_tune` becomes a retune of that carrier.

**Left for turning it on:**
- the mixer and its host tests;
- the power split: each carrier −10·log10(N) dB, plus PA back-off for the peak-to-average of N FSK carriers;
- the CPU budget for N modulators on the Pluto and the host;
- spacing and bandwidth checks;
- raising a driver's carriers once tested on air.

FREQ-mode front ends (the LR2021 tunes its own carrier) stay at 1.

## 6. Time

A unit's RX and TX need one slot clock.

| Pair | Clock | State |
|---|---|---|
| RX channel and TX carrier on one SDR | the device's sample clock | works |
| RX and TX on two SDRs | each SDR's stamped sample clock, tied to UTC or the host clock | design |
| SDR RX, radio TX half (host) | the split's TIME clock (`split_now48`) | works (`rdofradio --split-tx`) |
| SDR as a split half on the board | needs the sample clock stamped against `esp_timer` | not done (`split_port.c`) |
| two radios as halves | the split's clock fit | works |
| several TX for one RX (simulcast) | UTC ([../slot-timing.md](../slot-timing.md)) | design |

The open refuses an SDR as a split's TX half; the board refuses the SDR as either half (`split_port.c`).

## 7. Steps

Done:
1. **The format:** version 3, one parse into the units (`siteplan.c`), the schema and `tools/config_check.py`, the
   fixtures checked by both the schema and the C.
2. **The checks:** the open's unit and caps checks, the platform checks and the board's (§3).
3. **The transmitter pool:** an SDR is a transmitter tuned per stream; the parrot, the IP gateway and the bench send
   through it ([transmitters.md](transmitters.md)).
4. **The host tool** runs a version 3 file (one unit that transmits, or up to 8 split repeaters on its SDR); its flags build one in memory.

Left:
5. `GET /api/config/schema` and `POST /api/config?check=1` ([config-v3.md](config-v3.md) §13.2).
6. **`rdof_sdr_tx_t`:** one carrier live, two tested (§5).
7. **The board:** several SDR modems and receivers; `split_port.c` drops "one USB half"; caps follow RAM.
8. **The bench:** `via=device:NAME` and `via=auto`, `devices` in `/info` (§3.1).
9. **Later:** the SDR split half on the board (the `esp_timer` stamp), multi-carrier on air (§5), trunking.

## 8. Licensing

All of this is the project's own MIT code, with no new libraries in the firmware or the host tool.
- **The schema** is our own file, and so is its checker (`config/schema-check.js`).
- **`tools/config_check.py`** uses `jsonschema` (MIT), a tool dependency only.
- **The device layers already in use** stay as they are: SoapySDR is Boost 1.0; libiio and libad9361 are LGPL, linked
  dynamically, only in the host tool, and listed in `PROVENANCE.md`.

Nothing GPL comes in. Summing carriers is plain arithmetic in our own code.
