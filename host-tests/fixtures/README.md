# Fixtures

Golden data for the SDR tests. A reference implementation of the same receive and transmit paths made them, run on
the same inputs. That implementation is the code this project was ported from; it isn't published, so nothing in
this project regenerates these files (`PROVENANCE.md`, "Origin"), except the data call and the AGC cases below.
The tests check the C (and the Rust) against them. `host-tests/vectors.h` and `host-tests/scenarios.h` are the same
kind of data for the DMR core.

| Folder | Used by | What |
|---|---|---|
| `sdr-vectors/` | `rdof_sdr_tests`, `rdof_sdr_tests_min`, `rdof_sdr_edge_tests` | `sdr_vectors.h` (tables, configs, expected bursts and tracker output) and `sdr-data/`: cu8 recordings in, the channelizer's cf32 and the FM discriminator's f32 out |
| `mod-vectors/` | `rdof_mod_tests` | `mod_vectors.h` and `mod-data/`: the DMR modulator's expected output, and round trips through the receiver |

The recordings in `sdr-data/` are synthetic: DMR bursts built and modulated by the reference, with added noise.

## Test IDs

Every radio and peer ID the fixtures and tests pick is in 9990000-9999999 (999xxxx), a range RadioID has never
assigned, so none is a real ham's. Talkgroups, protocol values (0, 0xFFFFFF) and the small IDs of the hand-written
edge cases stay as they are (RadioID gives users 7 digits and repeaters 6), and random IDs from a seeded generator
(in `vectors.h`) stay random. On
2026-10-24 the earlier test IDs (310xxxx and others) moved here, everything else kept: the reference rerun with the
new IDs, its changed rows merged into these files, so they are still the reference's. The exceptions are
regression vectors from the C: the data call and the e2e case (below, rebuilt by `gen_data_call.c` around the
reference's new bursts) and the extra modem writes in `scenarios.h` `r_second_data_header` and its `_split`.

## The data call

`demod_sms_ms_slot2.cu8` and `e2e_288k.cu8` come from `gen_data_call.c`, which builds them with this project's
encoders (`rdof_data_burst`, `rdof_ipgw_build_dmr`) and modulator (`sdr/src/mod.c`), adds noise from a fixed seed,
and prints the expected values from the C:

- The call: an MS on slot 2 sends 16 preamble CSBKs, then an unconfirmed data header and two rate 1/2 blocks
  (SAP 3, ten octets of ASCII text), from test ID 9990001 to 9990002.
- `vectors.h`: the `sms_data_call` scenario's steps (the call on color code 5) and the tracker and HBlink4 sink's
  output for them.
- `sdr_vectors.h`: the `sms_ms_slot2` demod case (the call at 48 kS/s, color code 2, +127 Hz, about 13 dB SNR;
  its 19 rows of `v_dbursts`), and the e2e case: `e2e_288k.cu8` holds four channels at 288 kS/s, the call's first
  12 preambles on SDR08 and, on the other three, the same bursts, positions, levels and frequency errors the
  reference's recording had (an MS voice call, a repeater's outbound with a late entry, a direct mode call).
  `v_e2e_chans`' AFC values, `v_e2e_outs` and `V_STATUS_JSON` are the C receiver's output on it.

To regenerate (after a change to the encoders, the modulator or the receiver that is meant to change them):

```sh
cmake --build build/host --target rdof_gen_data_call
build/host/rdof_gen_data_call host-tests/fixtures/sdr-vectors/sdr-data > tables.txt
```

It rewrites both recordings; paste the tables it prints over the same tables in `vectors.h` and `sdr_vectors.h`
(the demod rows are `v_dbursts` 108 to 126).

## The AGC cases

The AGC cases in `sdr_vectors.h` (`v_agc_steps`, `v_agc_events`, `v_agc`) are regression vectors from the C, not
the reference. The reference ran them on a gain table taken from GPL code; they now run on the R820T's measured
steps (`v_agc_steps`, the same as `rtlsdr/src/r820.c` `gain_steps[]`), with the expected decisions recomputed by
`sdr/src/agc.c`. The inputs (configs and level sequences) are the reference's, except two `max_gain`s moved onto
the new steps (46.2, 40.1). The decisions keep their shape: the same steps down on clipping, steps up, ceiling
changes and holds, at the same updates. The law is also checked against a plain model on random runs
(`test_sdr_edges.c`, `test_agc_reference`).

When behavior changes on purpose, change the fixture with it, in the same commit, and say why in the message.
