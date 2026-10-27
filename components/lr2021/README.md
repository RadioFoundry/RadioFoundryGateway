# lr2021

A Semtech LR2021 transceiver on SPI, for any ESP-IDF project (and Linux, macOS and the host tests): reset, the BUSY
handshake, command and response frames, the FIFOs, the command encodings, and the chip as an SDR (a raw I/Q receive
session and sample transmit, `lr2021_iq.h`). Nothing specific to the gateway app: the pins come through a HAL
(`lr2021_hal.h`), the clock and waits through a port (`lr2021_port.h`).

Status (2026-10-27): runs on the Meshnology W12 (ROM 1.24): raw I/Q receive, and every transmit mode in
`lr2021_tx.h` checked on air at 920 MHz with an SDR (a DMR call decoded with no bit errors by frequency samples and
by the 4-FSK modem; a carrier and its live retune; phase steps; polar and phase-only I/Q).

The same code is SoapyLR20xx's `lib/` (github.com/RadioFoundry/SoapyLR20xx), renamed `lr20xx`: every file but this
README, `CMakeLists.txt` and the gateway's own (the patch RAM files, `pram/`, `lr2021_lora_extra.*`) is the same
there but for the rename, so a change on either side carries over (that project's `tools/upstream/`).

References: Semtech LR2021/LR2022/LR2012 datasheet, DS.LR20xx Rev. 2.2 (29 Jul 2026), cited below as "DS" with
its section or table. The raw I/Q commands are undocumented by Semtech; their facts are in
[docs/spec/lr2021-iq.md](../../docs/spec/lr2021-iq.md) (cited as "IQ R*n*"), which this README does not repeat.

| File | What |
|---|---|
| `include/lr2021_cmd.h`, `src/lr2021_cmd.c` | opcodes, frame builders, the status word, error names; portable C, no I/O |
| `include/lr2021.h`, `src/lr2021.c` | the driver over the HAL: reset, BUSY, command and answer frames, the FIFOs |
| `include/lr2021_hal.h`, `include/lr2021_hal_esp_idf.h`, `hal/esp_idf.c` | the pins: ESP-IDF's SPI master with DMA, BUSY and NRESET GPIOs, the IRQ pin's edge interrupt |
| `include/lr2021_hal_linux.h`, `hal/linux.c` | the pins on Linux: spidev and the GPIO character device |
| `include/lr2021_port.h`, `port/esp_idf.c`, `port/posix.c` | error codes (ESP-IDF's values), the clock, waits; on ESP-IDF a sleep under a tick by a one-shot timer |
| `include/lr2021_iq.h`, `src/lr2021_iq.c`, `src/lr2021_iq_plan.c` | the chip as an SDR: modes and rates, the receive session and its reader, a transmission with the receiver paused |
| `include/lr2021_board.h`, `src/lr2021_board.c` | the board around the chip (clock, RF switch, IRQ DIO, range): `lr2021_board_w12` and a text form |
| `include/lr2021_chips.h`, `src/lr2021_chips.c` | the ROMs the raw paths are known on |
| `linker.lf` | the receive and transmit paths in IRAM (6.7 KB): the reader 30% of core 0 on the W12 at 384 kS/s, 36% without |
| `include/lr2021_tx.h`, `src/lr2021_tx.c` | the transmit modes: setup sequences, sample formats, polar I/Q conditioning; portable, no I/O |
| `src/lr2021_tx_stream.c` | `lr2021_tx_stream`: feeds the TX FIFO from a callback and chains runs (on the IRQ pin, the next SetTx just ahead of TX_DONE), a timed key-up |
| `src/lr2021_packet.c` | `lr2021_packet_send` / `_receive`: one packet with the chip's modems |
| `include/lr2021_patch.h`, `src/lr2021_patch.c`, `src/lr2021_patch_image.c` | Semtech's patch RAM image (Clear BSD) and its info words; our patch's command frames and the DMR 4-FSK RX table; portable |
| `pram/`, `src/lr2021_rdof_pram_image.c` | our own patch RAM image: its source, build script and README, and the generated image |
| `src/lr2021_patch_load.c` | `lr2021_patch_load` / `_info`: load a patch into the chip and start it; ours, its override table, DMR 4-FSK RX |
| `include/lr2021_radio.h`, `src/lr2021_radio.c` | system and common radio commands: sleep, standby, FS, calibration, clocks, temperature, battery, random numbers, RSSI and its calibration, CAD, CCA, duty-cycled RX, timestamps, the IRQ table |
| `include/lr2021_lora.h`, `src/lr2021_lora.c` | LoRa (all the modulation, packet, sync word, CAD, side detector and hopping commands, packet status, time on air), LR-FHSS, ranging (RTToF) |
| `include/lr2021_lora_extra.h`, `src/lr2021_lora_extra.c` | the gateway's own: LoRa's 10-bit sync word commands and a ranging delay table (not in the datasheet) |
| `include/lr2021_fsk.h`, `src/lr2021_fsk.c` | (G)FSK, OOK, FLRC, BPSK TX: modulation, packet, sync words, CRC, whitening, addresses, packet status, time on air |
| `include/lr2021_proto.h`, `src/lr2021_proto.c` | the standard-protocol modems: Bluetooth LE, IEEE 802.15.4 O-QPSK, Z-Wave, Wireless M-Bus, Wi-SUN |
| `test/test_lr2021.c` | host tests of the encodings |
| `test/test_lr2021_driver.c`, `test/mock_hal.*` | host tests of the driver (`src/lr2021.c`) over a mock HAL, a fake clock and a scripted chip |
| `test/test_lr2021_tx.c` | host tests of the transmit setups and sample helpers |
| `test/test_lr2021_tx_stream.c` | host tests of `lr2021_tx_stream` over the mock and a model of the TX FIFO |
| `test/test_lr2021_iq.c` | host tests of the session (setup, reader, losses, gain) and a transmission over a model of the chip |
| `test/test_lr2021_packet.c` | host tests of the packet calls over the mock and a model of a packet modem |
| `test/test_lr2021_patch.c` | host tests of the patch loader over the mock and a chip model with memory |
| `test/test_lr2021_chips.c`, `test/test_lr2021_hal_linux.c`, `test/test_lr2021_lora_extra.c` | the ROM table and boards, the Linux HAL's arguments (Linux only), the LoRa extras |
| `test/test_lr2021_{radio,lora,fsk,proto}.c` | host tests of the documented commands: every frame byte for byte, every answer parser, time on air |

No Kconfig options; `CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD` lets the port's sleep under a tick wake from the
timer's ISR (else from esp_timer's task). CMake: an ESP-IDF component when `ESP_PLATFORM` is set (`PRIV_REQUIRES
esp_driver_spi esp_driver_gpio esp_timer`, `linker.lf`); otherwise static libraries: `lr2021_portable` (the
encodings, no I/O; links `m`), `lr2021_driver` (the driver, no port or HAL: the tests link the mock) and `lr2021`
(the driver, the POSIX port and on Linux the spidev HAL). `LR2021_TESTS` adds the tests.

## SPI link

| Item | Value | Source |
|---|---|---|
| SPI mode | 0 (CPOL 0, CPHA 0), MSB first | `hal/esp_idf.c:add_device` |
| Clock | `clock_hz`; ≤ 0 or > 16 MHz becomes 16 MHz | DS 3.7.2; `lr2021_hal_esp_open` |
| NSS | driven by the SPI peripheral, `cs_ena_pretrans = 1` | `add_device` |
| BUSY | GPIO input; polled before **every** frame | DS 5.2; `src/lr2021.c:wait_busy` |
| BUSY timeout | 200 ms (`src/lr2021.c:BUSY_TIMEOUT_US` = 200000) → `ESP_ERR_TIMEOUT` | |
| BUSY poll | 1 µs busy-wait for the first 50 µs, 5 µs to 2 ms, then 1 RTOS tick per poll (most commands hold BUSY a few µs: 5 µs steps added half of that again to every frame, IQ R2.34) | `wait_busy` |
| NRESET | GPIO output, high when idle | `lr2021_hal_esp_open` |
| Largest frame | commands and reads 260 bytes (`src/lr2021.c:MAX_FRAME`); FIFO transfers 1026 bytes (`MAX_FIFO_FRAME`: a 1 KB FIFO transfer, the extended FIFO, plus 2 status bytes). The bus's `max_transfer_sz` is 1026 | |
| Transfer | full duplex, one frame per NSS low; frames shorter than 32 bytes (`DMA_MIN`) by `spi_device_polling_transmit`, longer ones by `spi_device_transmit` (the task sleeps while the DMA moves them) | `hal/esp_idf.c:xfer` |
| Received bytes | only what the call needs, rounded up to 4 within the frame; none (`rx_buffer` NULL) when only the write matters (see DMA alignment) | `xfer_rx` |
| IRQ pin | `irq` ≥ 0: a GPIO interrupt on the rising edge; `lr2021_wait_irq` sleeps on it, `lr2021_irq_pin` reads its level | `hal/esp_idf.c:wait_irq` |

At 16 MHz a 258-byte FIFO read is about 130 µs on the wire, plus ESP-IDF's transaction overhead.

### DMA alignment

The bus has DMA, so every transaction goes through it, polled ones too. The S3's GDMA writes received data 4 bytes
at a time: an RX buffer whose address or length isn't a multiple of 4 makes ESP-IDF's `spi_master` allocate an
internal DMA bounce buffer for that transaction, and on IDF 5.5 a failed allocation there ends in a copy from NULL
(`LoadProhibited`). The driver avoids it:

- The handle, which holds the TX and RX frames, is internal DMA-capable RAM aligned to 4 (`lr2021_open`'s, or the
  caller's `cfg.mem`).
- A frame receives only the bytes the call reads, rounded up to 4 where the frame is that long: GetStatus sends 6
  and receives 4; a command, a write's frame and a TX FIFO write receive nothing.
- A FIFO read of n bytes is an n + 2 byte frame: keep n + 2 a multiple of 4 (n − 2 for n a multiple of 4) and there
  is no bounce.
- What can't be rounded (the IRQ word, register reads, a FIFO read of another length) still bounces. Each one counts
  in `lr2021_hal_esp_bounces`, and while the largest internal DMA block is under the transfer plus 64 bytes the transfer is
  refused (`ESP_ERR_NO_MEM`, "no internal DMA memory for an unaligned transfer") instead of letting the IDF fail.

### Frames

All multi-byte fields are big-endian (DS 5.4.1). Every frame the host sends clocks back, in its first two bytes,
the **status word of the previous command** (DS 6.7.1).

```
host → chip:  | opcode hi | opcode lo | arg0 | arg1 | ... |
chip → host:  | status hi | status lo | (ignored, or data on a direct read / GetStatus) |
```

Status word (DS table 6-38), decoded by `lr2021_status(hi, lo)`:

| Bits (16-bit word) | Field | Values |
|---|---|---|
| 11:9 (`hi` bits 3:1) | `cmd` | 0 FAIL, 1 PERR (bad opcode or arguments), 2 OK, 3 DATA |
| 8 (`hi` bit 0) | `irq` | 1: an interrupt is pending |
| 7:4 (`lo` bits 7:4) | `reset` | first reset source: 0 cleared, 1 POR / brown-out, 2 NRESET pin |
| 2:0 (`lo` bits 2:0) | `mode` | 0 sleep, 1 standby RC, 2 standby XOSC, 3 FS, 4 RX, 5 TX |

### Transactions

| Kind | Frames on the wire | Success rule |
|---|---|---|
| Write (`lr2021_write`) | 1. the command frame; 2. GetStatus (6 bytes: `01 00 00 00 00 00`) | GetStatus's `cmd` is not FAIL or PERR |
| Read (`lr2021_read`) | 1. the command frame; 2. `n + 2` zero bytes; answer = bytes 2 .. n+1 of frame 2 | frame 2's `cmd` is OK or DATA; else `ESP_FAIL` |
| GetStatus (`lr2021_get_status`, `lr2021_irq`) | `01 00` + 4 zero bytes; answer bytes 2..5 = IRQ status (32 bits) | transfer ok (status not checked) |
| Direct FIFO read (`lr2021_read_fifo`) | `00 01` + `n` zero bytes; data in bytes 2 .. n+1 of the same frame (DS 5.4.1.3) | transfer ok (status not checked) |
| Direct FIFO write (`lr2021_write_tx_fifo`) | `00 02` + `n` data bytes (DS 5.4.1.3) | transfer ok (status not checked) |

Each frame waits for BUSY low first; a failure at any step returns at once.

### Reset

`lr2021_reset`: NRESET low for 2 ms (`lr2021_sleep_ms(2)`, at least 1 tick; DS 4.2 asks ≥ 100 µs), high, wait 1
ms, then wait for BUSY low (boot and calibration), 200 ms at most.

## API: driver (`include/lr2021.h`)

Thread safety: none. A handle owns one TX and one RX buffer; serialize all calls on a handle. Several handles may
share a bus (the ESP-IDF HAL's `bus_initialized`), and ESP-IDF serializes the bus between devices. Every failure sets the text
`lr2021_last_error` returns and counts in `lr2021_failures` (except `ESP_ERR_INVALID_SIZE` and `lr2021_open`'s
own errors).

```c
typedef struct {
    lr2021_hal_t hal;      /* the pins: lr2021_hal_esp_open (ESP-IDF), lr2021_hal_linux_open, or a test's mock */
    void *mem;             /* lr2021_handle_size() bytes aligned to 4 (on an ESP32 internal DMA RAM), kept until close; NULL: allocated */
} lr2021_config_t;

typedef struct {           /* lr2021_hal_esp_idf.h */
    int spi_host;          /* SPI2_HOST or SPI3_HOST */
    int sck, mosi, miso, nss, busy, nreset;
    int irq;               /* the GPIO a DIO with IRQs drives, -1 for none */
    int pa_en;             /* a GPIO driven high while open (a front-end supply), -1 for none */
    int clock_hz;          /* 16 MHz at most */
    bool bus_initialized;  /* the app set up the bus already (shared) */
} lr2021_esp_cfg_t;
```

Errors are `lr2021_err_t` (`lr2021_port.h`): ESP-IDF's codes and values under the component's names
(`LR2021_ERR_TIMEOUT` is `ESP_ERR_TIMEOUT`), so on ESP-IDF they compare and print as `esp_err_t`. The table below
writes them as ESP-IDF's.

| Function | Behavior | Errors |
|---|---|---|
| `size_t lr2021_handle_size(void)` | Bytes a handle takes (its 1 KB TX and RX frames among them), for `cfg.mem` | – |
| `esp_err_t lr2021_hal_esp_open(const lr2021_esp_cfg_t *, lr2021_hal_t *)` | Configures BUSY (in), NRESET (out, high), `pa_en` (out, high) and the IRQ pin if any, initializes the bus unless `bus_initialized` (DMA auto, `max_transfer_sz` 1028), adds the device | `ESP_ERR_NO_MEM`; SPI and GPIO driver errors (what it set up is undone) |
| `esp_err_t lr2021_open(const lr2021_config_t *, lr2021_t **out)` | Takes the HAL, uses `cfg.mem` for the handle (else allocates it), resets | `ESP_ERR_INVALID_ARG` (no `xfer`, `busy` or `nreset`; `cfg.mem` not aligned to 4); `ESP_ERR_NO_MEM`; `ESP_ERR_TIMEOUT` from reset. On error the HAL is closed and nothing it allocated stays (the caller's `cfg.mem` is the caller's) |
| `void lr2021_close(lr2021_t *)` | NRESET low (chip held in reset), closes the HAL (the ESP-IDF one removes the device, frees the bus unless `bus_initialized`, drops `pa_en`), frees the handle unless it was `cfg.mem`. NULL is a no-op | – |
| `esp_err_t lr2021_reset(lr2021_t *)` | See Reset | `ESP_ERR_TIMEOUT` |
| `esp_err_t lr2021_wake(lr2021_t *)` | Wake from SetSleep: 120 µs of zeros without waiting for BUSY (DS 4.4.1 asks 100 µs of NSS low), then wait for BUSY. A sleeping chip only; not with a patch loaded (IQ R1.9) | `ESP_ERR_TIMEOUT` |
| `esp_err_t lr2021_write(lr2021_t *, const uint8_t *frame, size_t len)` | Write transaction | `ESP_ERR_INVALID_SIZE` (len > 260), `ESP_ERR_TIMEOUT`, SPI errors, `ESP_FAIL` (FAIL/PERR) |
| `esp_err_t lr2021_read(lr2021_t *, const uint8_t *frame, size_t len, uint8_t *out, size_t n)` | Read transaction; `n` answer bytes into `out` | `ESP_ERR_INVALID_SIZE` (len > 260 or n > 258), timeouts, `ESP_FAIL` |
| `esp_err_t lr2021_read_fifo(lr2021_t *, uint8_t *out, size_t n)` | Direct RX FIFO read of `n` bytes | `ESP_ERR_INVALID_SIZE` (n > 258) |
| `esp_err_t lr2021_write_tx_fifo(lr2021_t *, const uint8_t *data, size_t n)` | Direct TX FIFO write | `ESP_ERR_INVALID_SIZE` (n > 258) |
| `esp_err_t lr2021_get_status(lr2021_t *, lr2021_status_t *)` | GetStatus; the previous command's status | transfer errors |
| `esp_err_t lr2021_irq(lr2021_t *, uint32_t *irq)` | GetStatus's 32-bit IRQ status (0 on error) | transfer errors |
| `esp_err_t lr2021_version(lr2021_t *, uint8_t *maj, uint8_t *min)` | GetVersion (0x0101), 2 bytes. An LR2021 on ROM 1.24 answers `01 18` | read errors (outputs 0 on error) |
| `esp_err_t lr2021_errors(lr2021_t *, uint16_t *)` | GetErrors (0x0110), 16 bits, bits as in the table below | read errors |
| `esp_err_t lr2021_rx_fifo_level(lr2021_t *, uint16_t *)` | GetRxFifoLevel (0x011C), bytes | read errors |
| `esp_err_t lr2021_tx_fifo_level(lr2021_t *, uint16_t *)` | GetTxFifoLevel (0x011D), bytes | read errors |
| `esp_err_t lr2021_fifo_flags(lr2021_t *, uint8_t *rx, uint8_t *tx)` | GetAndClearFifoIrqFlags (0x012E): RX byte, TX byte; **clears** them | read errors |
| `esp_err_t lr2021_fifo_flags_peek(lr2021_t *, uint8_t *rx, uint8_t *tx)` | GetFifoIrqFlags (0x011B), does not clear (IQ R2.14) | read errors |
| `esp_err_t lr2021_read32(lr2021_t *, uint32_t addr, uint32_t *value)` | ReadRegMem32 (0x0106) of one word | read errors |
| `const char *lr2021_last_error(const lr2021_t *)` | Last failure text: `"BUSY stayed high"`, `"SPI transfer failed"`, `"command failed (chip in <mode>)"`, `"command not understood (chip in <mode>)"` | – |
| `uint32_t lr2021_failures(const lr2021_t *)` | Count of BUSY timeouts, SPI failures and FAIL/PERR statuses since open | – |
| `uint32_t lr2021_hal_esp_bounces(const lr2021_hal_t *)` | Transfers whose answer wasn't a multiple of 4 bytes, so ESP-IDF allocated a DMA bounce buffer (see DMA alignment) | – |
| `esp_err_t lr2021_set_clock(lr2021_t *, int hz, int input_delay_ns)` | The SPI clock, up to 80 MHz (past 16 MHz out of spec: experiments); the ESP-IDF HAL takes the device off the bus and adds it back | `ESP_ERR_INVALID_ARG`, `ESP_ERR_NOT_SUPPORTED` (a HAL without it) |
| `bool lr2021_wait_irq(lr2021_t *, int timeout_ms)`, `bool lr2021_has_irq(const lr2021_t *)`, `int lr2021_irq_pin(lr2021_t *)` | The IRQ pin: wait for it high, whether there is one, its level now (a GPIO read, no SPI) | – |

## API: documented modems (`lr2021_radio.h`, `lr2021_lora.h`, `lr2021_fsk.h`, `lr2021_proto.h`)

Every command in the datasheet, as frame builders in the style below (`lr2021_<group>_<what>(buf, ...)` writes the
frame and returns its length), and for each read command its answer length and a parser into real units (dBm, dB,
Hz, °C, V). The encodings are ported from Semtech's USP driver and checked against the datasheet; where USP,
TheClams' driver and the datasheet differ, the header says which was followed and why. Each header's comments name
what still needs checking on a chip.

A packet: set the packet type and the modem up with `lr2021_write` and the builders, then

| Function | Behavior |
|---|---|
| `esp_err_t lr2021_packet_send(dev, data, n, cfg, res)` | Clears the TX FIFO and IRQs, loads the payload (feeding the rest as the FIFO drains), SetTx, waits for TX_DONE |
| `esp_err_t lr2021_packet_receive(dev, buf, max, cfg, res)` | SetRx, waits for RX_DONE (draining the FIFO as it fills), reads the packet: `ESP_ERR_TIMEOUT` (none), `ESP_ERR_INVALID_CRC` (bad CRC, payload kept), `ESP_ERR_INVALID_RESPONSE` (header, length or address error), `ESP_ERR_INVALID_SIZE` (longer than `max`) |

`cfg`: the chip's timeout (32.768 kHz periods), the host's limit, the FIFO's size (256, or 1 KB once remapped) and
a poll interval for long payloads at fast rates. The packet status and statistics are the modem's own reads.

### Semtech's patch RAM

| Call | What |
|---|---|
| `esp_err_t lr2021_patch_load(dev, image, words)` | Standby, WriteRegMem32 in 32-word frames from 0x801000, EnablePram (0x012D 0x00), then checks the marker: `ESP_ERR_INVALID_STATE` (one runs already), `ESP_ERR_INVALID_RESPONSE` (it didn't start). `image` NULL loads Semtech's |
| `esp_err_t lr2021_patch_info(dev, info)` | Whether a patch runs, its type and version (0x800FF8 / 0x800FFC) |

Load it in standby, after the chip's setup; a reset removes it. Don't SetSleep with a patch loaded: the chip doesn't
wake from it, with retention or without, until NRESET (IQ R1.9). It fixes DCDC clocking for sub-GHz RX, RTToF, BLE and
FLRC details, and adds Z-Wave, O-QPSK and WM-Bus N 4GFSK extras. On ROM 1.24 FSK packet RX works only with a patch
running (IQ R3.35), so load it before receiving with the documented modems. The firmware keeps the raw I/Q paths
for sessions without one (IQ R1.2).

### Our patch RAM

Ours replaces Semtech's (one patch at a time): it fixes FSK RX the same way any running patch does, and adds a table
of register overrides the chip re-applies after its own setters (`pram/README.md`). It doesn't carry Semtech's
other fixes.

| Call | What |
|---|---|
| `esp_err_t lr2021_patch_load_rdof(dev, rep)` | Load ours, start it, empty the table, read its state |
| `esp_err_t lr2021_rdof_set(dev, entries, n, enable)` / `_enable(dev, on)` | Replace the override table (up to 32 entries of address, mask, value), turn it on or off |
| `esp_err_t lr2021_rdof_state(dev, rep)` / `_report(dev, rep)` | The table's size, whether it's on, and how often the hooked setters ran since EnablePram |
| `esp_err_t lr2021_fsk4_dmr_rx(dev, family)` | DMR 4-FSK reception with the chip's own modem: the FSK setup in the order that works and the correlator table for a sync family (`LR2021_DMR_BS` checked, `LR2021_DMR_MS` not yet). Each burst of the family is a 13-byte packet after its sync; `lr2021_fsk4_from_dmr` turns it into DMR dibits |

The DMR receiver finds the bursts reliably and gets about 6% of the symbols wrong (a median of 3 in 52), where a
demodulator on the same signal's I/Q gets about 1% (IQ R3.40 to R3.42): the chip's demodulator is the limit.

## API: the chip as an SDR (`include/lr2021_iq.h`)

A raw I/Q receive session (IQ section 2) and transmissions (section 3) on an open chip, single-threaded: the caller
runs `lr2021_iq_poll` in a loop on a task of its own, at a priority that keeps the 1 KB FIFO read (2 ms at 125 kS/s
int16, R2.18), and serializes every other call with it. Samples go to a sink as they come, at the chip's own rate;
buffering, resampling and scheduling are the caller's (the gateway's are in `esp_idf/main/fe_lr2021.c`).

| Mode | Format | Rates | Found |
|---|---|---|---|
| `LR2021_IQ_LORA` | cs16 | 125 000, 250 000 (4 × the LoRa bandwidth) | R2.1-R2.10 |
| `LR2021_IQ_FSK16` | cs16 | an FSK rx_bw code's, 32 MHz / (m × d), up to 260 000 | R2.19-R2.20 |
| `LR2021_IQ_CONV8` | cs8 | any whole rate up to 576 000, through the rate converter | R2.21, R2.26 |

| Call | What |
|---|---|
| `lr2021_iq_plan(rate, mode, &plan)`, `lr2021_iq_rates(mode, ...)` | The mode's settings for a rate (no I/O): sample size, exact rate, rx_bw or LoRa bandwidth, the filter's passband |
| `lr2021_iq_init(iq, dev, board, allow_untested)` | Checks the ROM (`lr2021_chips.h`) and that no patch runs (R1.2), then standby, the clock, the regulator, calibration |
| `lr2021_iq_start(iq, cfg)` | A session at `cfg`'s rate, center, crystal correction and gain step (0: the AGC); ends one running first |
| `lr2021_iq_poll(iq, sink, ctx, wait_ms)` | One step of the reader: waits for the RX FIFO IRQ (LORA), or for about 640 bytes by the level reckoned from the rate (a real query every fourth read, R2.34), sleeping meanwhile; reads them in one frame; hands whole samples to the sink with the time. A loss (R2.13-R2.15, R2.28) is handled here, counted and flagged to the sink |
| `lr2021_iq_set_gain(iq, step)`, `lr2021_iq_stop(iq)` | The gain now or for the next session; the session's end (standby, reset, setup: R2.16) |
| `lr2021_iq_transmit(iq, tx, pull, ctx, stats)` | One transmission, half duplex: the session paused (no reset), the mode set up (CalibFE only where the chip isn't calibrated already, R3.48), the board's front end held in TX between runs (R3.44), the samples streamed in runs from the pull, keyed up at `tx->at_us` if given, the session back after |
| `lr2021_iq_tx_calibrates(iq, tx)` | Whether that transmission will run CalibFE first (about 10.6 ms) |

## API: transmit (`include/lr2021_tx.h`, `lr2021_tx_stream` in `lr2021.h`)

Every way found to put a waveform on air, from the raw TX command 0x0293 and the FSK modem. The facts, the limits
and the measurements are in IQ section 3 (R3.9-R3.31).

| Mode | Samples | What goes out | Limits |
|---|---|---|---|
| `LR2021_TX_CW` | none | an unmodulated carrier (SetTxTestMode 2) | retune live with `lr2021_cmd_retune` (R3.30) |
| `LR2021_TX_FREQ` | int8 a sample | frequency: count × fdev / 64 Hz | exact from 31.25 to 250 kS/s (R3.9-R3.12) |
| `LR2021_TX_PHASE_STEP` | int8 a sample | a phase step a sample: count × rate / 128 Hz | R3.18 |
| `LR2021_TX_IQ_PHASE` | I, Q int8 | the phase of each sample, constant power | R3.22 |
| `LR2021_TX_IQ_POLAR` | I, Q int8 | phase and envelope (the PA's power, about 30 dB) | ≤ min(20 kHz, 0.31 × rate) instantaneous; condition with `lr2021_polar_iq8` (R3.23-R3.28) |
| `LR2021_TX_FSK4` | 4 symbols a byte | the chip's 4-FSK modem, its shaping filter | R3.20-R3.21 |

A transmission: `lr2021_tx_frames` gives the setup frames (send each with `lr2021_write`), then
`lr2021_tx_stream` keys up with `lr2021_tx_start_frame`'s frame and feeds the FIFO from a pull callback
(`lr2021_iq_transmit` does all of it). A run's length and its bytes are 16-bit counts: at most 32,767 samples for
I/Q, 65,535 otherwise (`lr2021_tx_max_run`, R3.43). The stream starts the next run as one ends, so the length has
no limit: with the IRQ pin on TX_DONE (`cfg.irq_pin`) it sends the next SetTx 35-45 µs ahead of the TX_DONE it
predicts, and the seams are 60-80 µs (R3.45, R3.47); polling, 0.2-0.5 ms (R3.15). `cfg.at_us` keys up at a time,
within ±5 µs (R3.48); `cfg.cut_end` lets the data end inside a run (a streamed transmission, R3.46); `cfg.stop`
ends it at once. A run that ends at once with nothing sent (R3.17) comes back as `ESP_ERR_INVALID_RESPONSE` with
`stats.early`: send it again.

| Function | Behavior |
|---|---|
| `int lr2021_tx_frames(const lr2021_tx_setup_t *, uint8_t f[][13], size_t *len)` | The setup frames, in order (at most 16); −1 for a bad setup (no rate or samples, no fdev for FREQ / FSK4, an unknown mode) |
| `size_t lr2021_tx_start_frame(const lr2021_tx_setup_t *, uint8_t *buf, uint32_t timeout)` | SetTx (or SetTxTestMode for CW) |
| `int lr2021_tx_sample_bytes(mode)` | FIFO bytes a sample: 2 for I/Q, 1 for FREQ, PHASE_STEP and FSK4 (a byte is 4 symbols there), 0 for CW |
| `uint32_t lr2021_tx_max_run(mode)` | The most samples a run takes: 32,767 for I/Q, 65,535 otherwise (R3.43) |
| `esp_err_t lr2021_tx_stream(dev, cfg, pull, ctx, stats)` | See `lr2021.h`; `cfg.run_bytes` 0 keys a carrier and returns |
| `int8_t lr2021_phase_step_sample(float hz, uint32_t rate)` | The count for a frequency in PHASE_STEP |
| `uint32_t lr2021_fsk4_fdev(float outer_hz)` | The fdev that puts the outer 4-FSK levels at `outer_hz` (DMR: 1944 → 2074) |
| `size_t lr2021_fsk4_pack(const int8_t *sym, size_t n, uint8_t *out)` | Symbols ±1, ±3 to the chip's dibits, MSB first |
| `void lr2021_fsk4_from_dmr(uint8_t *, size_t)` | DMR's dibits to the chip's (XOR 0xAA) |
| `float lr2021_iq_freq_limit(uint32_t rate)` | The highest instantaneous frequency I/Q follows at a rate |
| `void lr2021_polar_iq8(const float *iq, size_t n, float peak, float r_min, const lr2021_am_table_t *, int8_t *out)` | Float I/Q to polar samples: dips under `r_min` filled at that level with the phase in a straight line, magnitudes through the PA's measured curve and its AM-PM taken off (`lr2021_am_w12_920`: the W12 at 920 MHz, +22 dBm, PA duty 4 / slices 2) |
| `void lr2021_phase_iq8(const float *iq, size_t n, int8_t *out)` | Float I/Q to phase-only samples |
| `uint32_t lr2021_pll_word(uint32_t hz)`, `size_t lr2021_cmd_retune(buf, hz)` | The PLL's frequency word and the register write that retunes in TX with no dip (R3.30) |

## API: encodings (`include/lr2021_cmd.h`)

Portable, pure, reentrant. Each builder writes the whole frame (opcode first) into `buf` and returns its length.
`buf` must hold the frame: 13 bytes covers every builder.

| Function | Behavior |
|---|---|
| `lr2021_status_t lr2021_status(uint8_t hi, uint8_t lo)` | Decodes the status word (above) |
| `const char *lr2021_mode_name(uint8_t mode)` | `"sleep"`, `"standby RC"`, `"standby XOSC"`, `"FS"`, `"RX"`, `"TX"`; `"?"` for ≥ 6 |
| `const char *lr2021_error_name(uint16_t errors)` | Name of the lowest set error bit; `""` for 0 |
| `uint16_t lr2021_calib_fe_word(uint32_t hz, bool hf)` | `(hz / 4000000) & 0x7FFF`, rounded down, `| 0x8000` if `hf` |
| `int8_t lr2021_tx_power_code(double dbm)` | `floor(dbm·2 + 1e-9)` clamped to −19..44 (−9.5..+22 dBm, LF PA) |
| `double lr2021_tx_power_dbm(int8_t code)` | `code / 2` |
| `uint8_t lr2021_ramp_code(int us)` | Smallest code whose ramp ≥ `us`; 18 (304 µs) past the end |
| `int lr2021_ramp_us(uint8_t code)` | Ramp time of a code; −1 for ≥ 19 |
| `uint32_t lr2021_freq_corrected(double hz, double ppm)` | `llround(hz / (1 + ppm·1e-6))`; positive ppm = crystal fast (the RTL-SDR's sign) |
| `uint32_t lr2021_raw_rx_rate(uint8_t bw)` | Raw RX complex sample rate: BW_31 → 125000, BW_62 → 250000, else 0 (not measured; IQ R2.8) |
| `int8_t lr2021_raw_freq_sample(float hz, float fdev_hz)` | `roundf(hz·64 / fdev)` clamped to −128..127 (IQ R3.2) |
| `int lr2021_raw_sample_bytes(uint8_t source)` | Bytes a sample on the FIFO by route source: 2, 2, 4, 4, 4, 4, 2 for 0–6 (IQ R2.20) |
| `void lr2021_fsk_rx_rate(uint8_t rx_bw, uint32_t *num, uint32_t *den)` | The raw rate of an FSK rx_bw code, num / den S/s: 32 MHz / (mode × decimation) (IQ R2.19) |
| `double lr2021_fsk_filter_share(uint8_t rx_bw)` | The channel filter's width as a share of that rate, by bits 7:6: 0.37, 0.34, 0.30, 0.27 |
| `int lr2021_fsk_rx_bw_for(double rate, double max_rate)` | The lowest listed code at or above `rate` (and at most `max_rate`); of equal rates, the larger decimation; −1 if none |
| `int lr2021_conv_rx_bw_for(double out_hz, double share)` | The code for the rate converter to make `out_hz` from: the fastest listed code under 4 × `out_hz` whose filter stays within `share` of it (IQ R2.21) |

Ramp table (`src/lr2021_cmd.c:ramps`, DS table 7-24), code → µs:

| 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 | 18 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2 | 4 | 8 | 16 | 32 | 48 | 64 | 80 | 96 | 112 | 128 | 144 | 160 | 176 | 192 | 208 | 240 | 272 | 304 |

### Frame builders and byte layouts

`op` = 2-byte opcode. Fields big-endian. Length includes the opcode.

| Builder | Opcode | Len | Arguments after the opcode |
|---|---|---|---|
| `lr2021_cmd(buf, op)` | any | 2 | none |
| `lr2021_cmd_u8(buf, op, a)` | any | 3 | `a` |
| `lr2021_cmd_set_rf_frequency(buf, hz)` | 0x0200 | 6 | `hz` u32 |
| `lr2021_cmd_set_rx_path(buf, hf, boost)` | 0x0201 | 4 | `hf ? 1 : 0`, `boost & 7` |
| `lr2021_cmd_calib_fe(buf, hz[], n, hf)` | 0x0123 | 8 | three u16 `calib_fe_word`s; entries past `n` or with `hz == 0` are 0x0000 |
| `lr2021_cmd_set_tcxo(buf, tune, start)` | 0x0120 | 7 | `tune & 7`, `start` u32 (32 MHz periods) |
| `lr2021_cmd_set_dio_function(buf, dio, func, pull)` | 0x0112 | 4 | `dio`, `(func & 0xF) << 4 | (pull & 0xF)` |
| `lr2021_cmd_set_dio_rf_switch(buf, dio, modes)` | 0x0113 | 4 | `dio`, `modes & 0x1F` |
| `lr2021_cmd_set_dio_irq(buf, dio, irqs)` | 0x0115 | 7 | `dio`, `irqs` u32 |
| `lr2021_cmd_clear_fifo_flags(buf, rx, tx)` | 0x0114 | 4 | `rx`, `tx` |
| `lr2021_cmd_clear_irq(buf, irqs)` | 0x0116 | 6 | `irqs` u32 |
| `lr2021_cmd_config_fifo_irq(buf, rx_en, tx_en, rx_high, tx_low, rx_low, tx_high)` | 0x011A | 12 | `rx_en`, `tx_en`, then u16 `rx_high`, `tx_low`, `rx_low`, `tx_high` |
| `lr2021_cmd_set_rx(buf, timeout)` | 0x020C | 5 | `timeout & 0xFFFFFF` u24 (32 kHz RTC periods; 0xFFFFFF continuous, DS 6.3.5) |
| `lr2021_cmd_set_tx(buf, timeout)` | 0x020D | 5 | as SetRx |
| `lr2021_cmd_set_gain(buf, step)` | 0x021A | 3 | `min(step, 13)`; 0 = AGC, 1..13 fixed step (DS 7.3.9) |
| `lr2021_cmd_set_tx_params(buf, power, ramp)` | 0x0203 | 4 | `power` (int8 half-dB code), `ramp` code |
| `lr2021_cmd_write_reg32(buf, addr, value)` | 0x0104 | 9 | `addr` u24, `value` u32 |
| `lr2021_cmd_read_reg32(buf, addr, words)` | 0x0106 | 6 | `addr` u24, `words` u8 |
| `lr2021_cmd_lora_modulation(buf, sf, bw, cr, ldro)` | 0x0220 | 4 | `(sf & 0xF) << 4 | (bw & 0xF)`, `(cr & 0xF) << 4 | (ldro & 3)` (DS 9.9.1) |
| `lr2021_cmd_lora_packet(buf, preamble, len, flags)` | 0x0221 | 6 | `preamble` u16, `len`, `flags` (header / CRC / IQ byte as given; DS 9.9.3) |
| `lr2021_cmd_fsk_modulation(buf, bitrate, shape, rx_bw, fdev)` | 0x0240 | 11 | `bitrate & 0x7FFFFFFF` u32 (bit 31 clear: whole b/s), `shape`, `rx_bw` (DS table 11-4 code), `fdev & 0xFFFFFF` u24 Hz (DS 11.3.1) |
| `lr2021_cmd_fsk_packet(buf, preamble_bits, len)` | 0x0241 | 9 | `preamble_bits` u16, `00` (no preamble detect), `00` (fixed length, no address), `len` u16, `00` (no CRC, no whitening) (DS 11.3.2) |
| `lr2021_cmd_raw_route_fifo(buf)` | 0x0290 | 13 | `FF 00 00 00 00 0A 00 00 00 00 01` (IQ 2.2) |
| `lr2021_cmd_raw_route(buf, rate, source)` | 0x0290 | 13 | `rate`, then the RX FIFO and `source` (`LR2021_RAW_SRC_*`; IQ R2.20) |
| `lr2021_cmd_raw_route_rate(buf, rate, out_hz, source)` | 0x0290 | 13 | as above with bytes 1–4 = `out_hz` (BE32): the rate converter's output for sources 0 and 6 (IQ R2.21) |
| `lr2021_cmd_raw_tx(buf, n, rate, selector)` | 0x0293 | 9 | `n` u16 samples, `rate` u32 S/s, `selector` (0 I/Q, 1 frequency, 2 phase; IQ R3.1) |
| `lr2021_cmd_set_pa_config(buf, hf, lf_duty, lf_slices, hf_duty)` | 0x0202 | 5 | `hf ? 0x80 : 0`, `duty << 4 | slices`, `hf_duty & 0x1F` (DS 7.4.1) |
| `lr2021_cmd_ook_modulation(buf, bitrate, shape, rx_bw, depth)` | 0x0281 | 9 | `bitrate` u32, `shape`, `rx_bw`, `depth` (DS 12.3.1) |
| `lr2021_cmd_fsk_variant(buf, v)` | 0x023B | 3 | 1: 4-FSK (IQ R3.20) |
| `lr2021_cmd_retune(buf, hz)` | 0x0104 | 9 | WriteRegMem32 of `lr2021_pll_word(hz)` to 0xF40144 (IQ R3.30) |

Example: `lr2021_cmd_set_rf_frequency(buf, 461137500)` → `02 00 1B 7C 66 5C`.

### Opcodes (`include/lr2021_cmd.h`, DS 5.6 tables 5-4 to 5-8)

| Opcode | Name | Opcode | Name |
|---|---|---|---|
| 0x0001 | ReadRxFifo (direct) | 0x011E | ClearRxFifo |
| 0x0002 | WriteTxFifo (direct) | 0x011F | ClearTxFifo |
| 0x0100 | GetStatus | 0x0120 | SetTcxoMode |
| 0x0101 | GetVersion | 0x0121 | SetRegMode |
| 0x0104 | WriteRegMem32 | 0x0122 | Calibrate |
| 0x0105 | WriteRegMemMask32 | 0x0123 | CalibFE |
| 0x0106 | ReadRegMem32 | 0x0128 | SetStandby |
| 0x0110 | GetErrors | 0x0129 | SetFs |
| 0x0111 | ClearErrors | 0x012E | GetAndClearFifoIrqFlags |
| 0x0112 | SetDioFunction | 0x0200 | SetRfFrequency |
| 0x0113 | SetDioRfSwitchConfig | 0x0201 | SetRxPath |
| 0x0114 | ClearFifoIrqFlags | 0x0202 | SetPaConfig |
| 0x0115 | SetDioIrqConfig | 0x0203 | SetTxParams |
| 0x0116 | ClearIrq | 0x0206 | SetRxTxFallbackMode |
| 0x0117 | GetAndClearIrq | 0x0207 | SetPacketType |
| 0x011A | ConfigFifoIrq | 0x020B | GetRssiInst |
| 0x011B | GetFifoIrqFlags | 0x020C | SetRx |
| 0x011C | GetRxFifoLevel | 0x020D | SetTx |
| 0x011D | GetTxFifoLevel | 0x020F | SelPa |
| 0x020E | SetTxTestMode | 0x023B | 4-FSK variant (undocumented, IQ R3.20) |
| 0x0281 | SetOokModulationParams | | |
| 0x0220 | SetLoraModulationParams | 0x021A | SetAgcGainManual |
| 0x0221 | SetLoraPacketParams | 0x0240 | SetFskModulationParams |
| 0x0223 | SetLoraSyncword | 0x0241 | SetFskPacketParams |
| 0x0290 | raw route (undocumented, IQ 2.2) | 0x0293 | raw TX (undocumented, IQ R3.1) |

Opcodes with no builder are sent with `lr2021_cmd` / `lr2021_cmd_u8` or a caller-built frame.

### Constants (`include/lr2021_cmd.h`)

| Group | Values | Source |
|---|---|---|
| Packet type (SetPacketType) | LoRa 0x0, FSK 0x2, OOK 0xA | DS table 8-1 |
| LoRa BW codes | `BW_31` 0x2, `BW_62` 0x3, `BW_125` 0x4 | DS table 9-3 |
| IRQ | `LR2021_IRQ_TX_DONE` bit 19 | DS table 5-20 |
| Version | `LR2021_VERSION_MAJOR` 0x01, `_MINOR` 0x18 (ROM 1.24) | DS table 6-40 |
| Calibrate blocks | LF_RC 0x01, HF_RC 0x02, PLL 0x04, AAF 0x08, MU 0x20, PA_OFF 0x40 | DS table 6-28 |
| GetErrors bits 0..11 | HF XOSC start, LF XOSC start, PLL lock, LF RC cal, HF RC cal, PLL cal, AAF cal, image cal, chip busy, RX freq with no FE cal, meas. unit ADC cal, PA offset cal | DS table 6-43 |
| FIFO flags | EMPTY 0x01, LOW 0x02, HIGH 0x04, FULL 0x08, OVERFLOW 0x10, UNDERFLOW 0x20 | DS 6.10.1 |
| DIO functions | NONE 0, IRQ 1, RF_SWITCH 2, OUT_LOW 5, OUT_HIGH 6 | DS 6.8.1 |
| RF switch modes (DIO high in each set mode) | STANDBY 0x01, RX_LF 0x02, TX_LF 0x04, RX_HF 0x08, TX_HF 0x10 | DS table 6-45 |
| Patch marker | word at `0x00800FF8` = `0x600DB002` while a Semtech patch runs | IQ R1.2 |
| 1 KB FIFO registers | `RX_FIFO_BASE` 0x00F30028, `TX_FIFO_BASE` 0x00F3002C, `RX_FIFO_SIZE` 0x00F30030, `TX_FIFO_SIZE` 0x00F30034 | DS 5.3.3.1.1 |
| 1 KB FIFO values | RX base 0x00804400 size 0x3E8; TX base 0x00804000 size 0x3FC | DS 5.3.3.1.1 |

## Tests

Encodings (`test/test_lr2021.c`, 37 checks):

| Group | What it pins |
|---|---|
| Status word | Bit positions of `cmd`, `irq`, `reset`, `mode`; mode and error names (lowest bit wins, `""` for 0) |
| Frames | Exact bytes of GetVersion, SetStandby, Calibrate, SetRfFrequency, SetRxPath (LF and HF), SetTcxoMode, SetDioFunction, SetDioRfSwitchConfig, SetDioIrqConfig, ClearFifoIrqFlags, ConfigFifoIrq, SetRx (continuous), SetTx (24-bit truncation), SetAgcGainManual (and its clamp to 13), SetTxParams, WriteRegMem32, ReadRegMem32 |
| CalibFE | The datasheet's example (900 MHz → 0x00E1 LF, 0x80E1 HF); 461.1375 MHz rounds down to 115; unused slots 0 |
| Power, ramp | Clamps at −19 / 44, floor rounding, code ↔ µs, past-the-end ramp |
| Crystal correction | +10 ppm, 0, −2.5 ppm at 461.1375 MHz |
| LoRa, FSK | Modulation and packet frames |
| Raw paths | Route and raw TX frames, RX rates per BW, frequency sample rounding and clamps |

### Driver (`test/test_lr2021_driver.c`)

`src/lr2021.c` is built on the host over a mock HAL and a fake clock that only the waits move (`test/mock_hal.*`),
with a scripted chip that answers each SPI transfer by index. Not a model of the chip: it pins what the driver sends
and how it reads the answers. The ESP-IDF HAL itself (`hal/esp_idf.c`) is checked on the board.

| Group | What it pins |
|---|---|
| `opening` | Pin setup (BUSY in, NRESET out), the bus config (pins, 1026-byte transfers, DMA auto), the device (mode 0, clock, NSS, pre-transaction), the NRESET sequence, clamping of the clock to 16 MHz, a shared bus not initialized and not freed, `close` and `close(NULL)` |
| `open_failures` | No memory; bus init, add device and reset failing: the error passes on, the handle is freed, the device removed and the bus freed only if this call initialized it |
| `busy_handshake` | Waiting for BUSY in 5 µs steps for 2 ms and a tick at a time after; the 200 ms timeout, its text and count; every call waits first |
| `status_and_write` | The GetStatus frame and its decoding; `lr2021_irq`; a write is the frame, then GetStatus; FAIL and PERR give `ESP_FAIL` with the mode in the text and a count; DATA after a write is fine; SPI failure at each transfer; size limits (260 is the largest frame, 261 is `ESP_ERR_INVALID_SIZE` and not a failure) |
| `reading` | The two frames of a read; DATA and OK; FAIL, PERR and a reserved status give `ESP_FAIL` with the output untouched; SPI failure at each transfer; the 258-byte answer; size limits |
| `bounces` | The bytes each frame receives (GetStatus 4 of 6, commands and writes none, a 4-multiple FIFO frame all of it); bounces counted for the IRQ word and an odd FIFO read, and refused with `ESP_ERR_NO_MEM` while the largest DMA block is short; the caller's `cfg.mem` used and not freed by `close`; a misaligned one refused |
| `fifo_and_accessors` | Direct FIFO read and write frames and sizes; version, errors, FIFO levels and flags (clearing and peeking), `read32`: opcode out, big-endian answer back, 0 on error |

### Transmit (`test/test_lr2021_tx.c`, `test/test_lr2021_tx_stream.c`)

| Group | What it pins |
|---|---|
| Setups | Every mode's frames byte for byte (the ones that went on air), CalibFE's place, the CW and SetTx start frames, refused setups |
| Builders | PA config, the retune word (920 MHz at −28 ppm is 0x39806984, read back from the chip) |
| Samples | Phase-step counts and clamps, 4-FSK packing and DMR's mapping, the fdev for DMR, the I/Q frequency limit, phase-only I/Q |
| Polar | Hole punching (a two-tone's 180° jump spread over the dip), the W12 table's full scale, a mid level with its AM-PM taken off, the floor |
| Stream | Three chained runs with every byte aired and the task sleeping, an underflow on a short last run, R3.17's early end and a resend, a chip that never finishes (timeout, standby), small pulls, a carrier, bad arguments; on the IRQ pin, restarts on its edge and queued ahead of TX_DONE; a streamed end cut; a timed key-up and one too late |
| Session (`test/test_lr2021_iq.c`) | Each mode's setup frames, the reader by the IRQ and by the reckoned level, losses and restarts, the gain, a transmission with the session paused and resumed and its CalibFE kept |

```sh
cmake -S components/lr2021 -B build/lr2021 && cmake --build build/lr2021 && ctest --test-dir build/lr2021
```

(also built by `host-tests`, with its sanitizers: every test above).

## Provenance

The driver, the raw paths and `lr2021_cmd.*` are written from the DS and measurements on the chip. The documented
commands (`lr2021_radio.*`, `lr2021_lora.*`, `lr2021_fsk.*`, `lr2021_proto.*`) are ported from Semtech's USP driver
(Clear BSD; the notice is in each file). Two LoRa opcodes and the ranging delay table in `lr2021_lora_extra.*`
are from TheClams/lr2021 (MIT; its notice in those files). The patch image in `src/lr2021_patch_image.c` is Semtech's, unchanged, from
the same USP commit (Clear BSD; the notice in the file). Both are in `PROVENANCE.md` and `LICENSES/`. The raw I/Q commands
(`lr2021_cmd_raw_*`, `lr2021_raw_*`, the patch marker) are facts learned from the documentation of
SAMS0N1TE/lr2021-sdr (GPL-3.0), recorded in `docs/spec/lr2021-iq.md`; no code taken (`PROVENANCE.md`).

## Not yet

- The documented modems on a chip: TX of LoRa, FSK, OOK and FLRC and FSK RX are verified (IQ R3.32 to R3.35); each
  header lists what else to check (the RSSI calibration tables' signs, LoRa and FLRC RX, which need a transmitter).
- 4-FSK RX quality: DMR bursts come through our patch at about 6% symbol errors (IQ R3.42); the demodulator's other
  loop settings aren't understood yet. MS bursts aren't tried.
- R3.17's cause (a run that ends at once); the TX FIFO's 0.9 ms early stop.
- An AM table for other boards, bands and PA settings: fit one at the operating rate (IQ R3.24).
