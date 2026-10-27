# Memory plan (firmware)

What: how the firmware places what each role needs of internal, DMA and PSRAM memory: the claims, the solver, the boot allocation, the margin, giving memory back and claiming it at run time, the status, and the DMA and alignment rules the drivers need.
Sources: `common/include/rdof/memplan.h`, `common/src/memplan.c` (portable), `esp_idf/main/mem.{c,h}`, each role's `*_mem_claims` (`sdr_task.c`, `fe_lr2021.c`, `fe_rtlsdr.c`, `roamer.c`, `ipgw_task.c`), `sdr.c` (the hot arena), `components/lr2021/src/lr2021.c` (the SPI frames).
Related: [overview.md](overview.md), [interfaces-http.md](interfaces-http.md#35-memory-memcmem_status_json-memplancrdof_memplan_json), [sdr.md](sdr.md), [roamer.md](roamer.md), [ipgw.md](ipgw.md), [../../../components/lr2021/README.md](../../../components/lr2021/README.md).

## 1. Why

The S3 has about 320 KB of internal RAM for the heap, and Wi-Fi, lwIP, the DSP and the drivers' DMA buffers all
want it. Before the plan, each role took what it needed when it started. Wi-Fi had carved the heap up by then, and
a role that started late found the memory but not in one piece. On the W12 (LR2021, 384 kS/s, five channels) about
10 KB stayed free. When an LR2021 FIFO read of odd length then made the IDF's `spi_master` allocate a DMA bounce
buffer and the allocation failed, the IDF's cleanup copied from NULL (`LoadProhibited`).

The plan fixes this two ways:

- Every role declares what it needs at boot, before Wi-Fi and before any role starts. One solver places it, checks
  the total against what is free with a margin kept for Wi-Fi and lwIP, and the board allocates it at once, the
  largest piece first. A role that doesn't fit is refused and says why, in the log and in the status.
- The drivers no longer allocate on their own at run time where that can be avoided: the LR2021's SPI frames are
  4-byte aligned and sized (§6), the RTL-SDR's USB transfers are kept across restarts, task stacks and control
  blocks are planned.

A board without an SDR declares no SDR claims: that memory goes to the others (a deeper IP gateway queue, the second
radio's stacks in internal RAM).

## 2. Claims

A claim is (role, name, size, alignment, caps, how). Roles are short names: `sdr`, `sdr_fe`, `sdr_net`, `radio1`,
`radio2`, `ipgw`, `usb`, `board`.

Caps (`RDOF_MEM_*`):

| Cap | Placed in |
|---|---|
| `INTERNAL` | internal RAM |
| `DMA` | internal DMA-capable RAM, 4-byte aligned at least |
| `PSRAM` | PSRAM; internal on a board without it |
| `INTERNAL \| FALLBACK` | internal while the spare above the margin holds it, else PSRAM |

How (`rdof_mem_how_t`):

| How | Meaning | Used for |
|---|---|---|
| `ALLOC` | allocated in the role's region for its kind of memory; the role takes the pointer (`mem_take`) | buffers the firmware owns, task stacks and control blocks (`mem_task`) |
| `HOLD` | allocated on its own, one block per count, and freed just before a driver allocates the same size (`mem_hand_over`) | buffers a driver allocates itself: the UART driver's rings, the RTL-SDR's USB transfers |
| `BUDGET` | counted, not allocated: something allocates it soon after | the web server's stack, MQTT clients, CDC-ACM, lwIP sockets, SCTP |

An elastic claim (`mem_elastic`) gets its size at least and grows in steps up to a maximum from what is spare once
every role has its share. An elastic `FALLBACK` claim grows in its turn among the `FALLBACK` claims instead, ahead of
the ones declared after it (the SDR's hot arena before the roamer's stacks).

A HOLD keeps the room until the driver needs it. Between the free and the driver's allocation nothing else runs on
that heap (the role's start is one task), so the driver gets the same block back.

## 3. Solving

`rdof_memplan_solve(plan, avail)` takes the heap as measured (internal, DMA and PSRAM free, each with its largest
block) and, role by role in the order declared:

1. Sums the role's internal, DMA and PSRAM needs. A claim's cost is its size times its count, plus `align − 4`
   when aligned to more than 4 (the worst-case padding).
2. PSRAM short: the role is refused.
3. Internal or DMA over what is free above the margin: with `CONFIG_RDOF_MEM_STRICT` the role is refused; else the
   plan's `error` says so and the role runs (its `why` starts "over the margin").
4. Otherwise the role's claims are placed and their bytes taken off what is left.

Then the `FALLBACK` claims are placed (internal while the spare holds them, else PSRAM; an elastic one grows there
from the spare), and the other elastic claims share the spare in the order declared. The spare left is `spare`. Last, each role's `ALLOC` claims of each kind are laid
out in one region, each claim at its alignment.

The declaration order is the priority: the SDR first (its DSP buffers want the heap in one piece), then the radios,
the IP gateway, the USB host, and the board's own.

`mem_plan_boot` (called by `app_main` right after the self-test, before Wi-Fi) declares, solves and allocates: the
internal regions largest first, then PSRAM, each with `heap_caps_aligned_calloc` and explicit caps; then the holds.
A region that fails to allocate refuses its role and frees what it had. The log has one line per role and a summary:

```
I mem: planned with 228 KB internal free (180 KB in one piece), 80 KB margin: 118 KB free now, 12 KB spare
```

## 4. The margin

`CONFIG_RDOF_MEM_MARGIN_KB` (default 80, 8..200) is internal RAM the plan leaves for what it doesn't count: Wi-Fi
and lwIP's buffers, mbedTLS on a board without PSRAM, the log, small allocations everywhere. On the W12, Wi-Fi,
lwIP and what isn't planned took about 77 KB beyond the budgets; 80 KB keeps that and a little more. A board that
runs short (`heap_free` near zero) wants a bigger margin; one with a lot left over (`spare` large and `heap_free`
well above the margin after an hour) can take a smaller one.

`internal.outside_plan` in the status is what was taken since the plan by everything but the plan's regions: the
budgets' owners, Wi-Fi, lwIP and what isn't planned. Compare it with `budget` + `margin`.

## 5. The SDR's hot arena

The receiver's buffers touched for every sample (the channelizer's FFT input and output, its states, the FIR
states) and esp-dsp's FFT plans and filter states are several times slower in PSRAM. Their sizes depend on the
sample rate and the channels, so `sdr_mem_claims` measures them with a dry run: `rdof_sdr_rx_init` (and
`rdof_chz_reserve` with two DSP cores) with the allocators counting, in PSRAM, then freed. The total is the `hot`
claim.

The DSP task opens the arena (`sdr_hot_arena_open`) only while it builds a receiver and closes it after; each
receiver starts it over. Anything else that allocates meanwhile (the page's spectrum, from another task) doesn't
land in it. A free inside the arena does nothing.

On a board with PSRAM the arena is two claims: `hot`, internal and elastic from what the plan has spare (ahead of
the roamer's `FALLBACK` stacks), and `hot_ps`, the whole size in PSRAM. The receiver's buffers fill `hot` first, in
the order it builds them (the channelizer's first), then `hot_ps`. On the W12 at 384 kS/s and 5 channels that's 17
KB internal and 11 KB in PSRAM. Without PSRAM, `hot` is the whole size, internal. Past both, the receiver's buffers
come from the internal heap as before. The SDR status shows `hot_arena`, `hot_arena_used` and `hot_arena_spill`;
the log says how much the receiver used.

The DSP's other buffers (trackers, demod history, channelizer output) still come from `fast_alloc`: internal while
`CONFIG_RDOF_SDR_INTERNAL_RESERVE_KB` stays free, else PSRAM. Measured on five busy channels, they cost nothing in
PSRAM.

## 6. DMA and alignment

| Where | Rule | How it's met |
|---|---|---|
| SPI master on a DMA bus (the LR2021) | the S3's GDMA writes received data 4 bytes at a time: an RX buffer whose address or length isn't a multiple of 4 makes `spi_master` allocate a DMA bounce buffer for every transaction, polled ones too | the driver's handle (`lr2021_dev`, planned in DMA RAM, 4-aligned) holds every frame; a transaction reads a length rounded up to 4 within the frame, or nothing (`rx_buffer` NULL) when only the write matters. The status read is 6 bytes out, 2 in (rounded to 4). If a bounce would still be needed and the largest DMA block is short, the driver refuses the transfer instead of letting the IDF fail; `lr2021_bounces()` counts the bounces that happened |
| LR2021 FIFO reads | the read's frame (2 status bytes and the samples) must be a multiple of 4 | `fe_lr2021.c:drain` reads n − 2 bytes, n a multiple of 4; a sample cut in two waits for the next read (`carry_n`), so the ring gets whole samples only |
| USB host transfers (RTL-SDR, CDC-ACM) | `usb_host_transfer_alloc` takes DMA, cache-aligned internal memory | the RTL-SDR's transfers are planned as a HOLD (each rounded up to 64 bytes), handed over before `rtl_usb_start` allocates them, and kept across restarts and unplugs; CDC-ACM's are a budget |
| UART driver | `uart_driver_install` allocates its rings in internal RAM | a HOLD of each, handed over just before the install |
| Task stacks | internal unless PSRAM is asked for; a control block always internal | `mem_add_task` plans the stack (16-aligned) and `StaticTask_t`; `mem_task` starts the task on them with `xTaskCreateStaticPinnedToCore`, or on the heap when not planned |

## 7. Inventory

Per role, what is declared (bytes; "×n" a count). Sizes are the code's; the SDR's depend on its settings.

| Role | Claim | Size | Where | How | When |
|---|---|---|---|---|---|
| `sdr` | `iq` | `CONFIG_RDOF_SDR_CHUNK` × 8 (32 KB at 4096) | internal, 16-aligned | alloc | an SDR in `run` (not "either") |
| | `raw` | chunk × the front end's sample bytes (8 KB for the LR2021's int8) | internal, 16-aligned; PSRAM for the LR2021 | alloc | |
| | `hot` | measured (§5); with PSRAM, from 16 bytes up to that (elastic) | internal, 16-aligned (fallback) | alloc | |
| | `hot_ps` | measured (§5) | PSRAM, 16-aligned | alloc | a board with PSRAM |
| | `dsp` | 8192 stack + control block | internal | alloc | |
| | `dsp_helper` | `CONFIG_RDOF_SDR_HELPER_STACK` (6144) | internal | alloc | `dsp_cores` 2 and the self-test hasn't started it |
| `sdr_net` | `sdr_status` | 4096 | internal | alloc | |
| | `mqtt` | `MQTT_CLIENT_INTERNAL` (9728) | internal | budget | |
| | `sdr_hblink` | 4096 | internal | alloc | a channel has an HBlink4 peer |
| `sdr_fe` (LR2021) | `lr2021_dev` | `lr2021_handle_size()` (the frames) | DMA | alloc | |
| | `lr2021` | 4096 (the reader) | internal | alloc | |
| | `spi_bus` | 1536 | DMA | budget | |
| | `ring` | `CONFIG_RDOF_SDR_RING_KB` KB + 1 | PSRAM | alloc | |
| | `blocks` | the converter's blocks | PSRAM, 16-aligned | alloc | |
| `sdr_fe` (RTL-SDR) | `usb_xfer` | `usb_transfer_kb` KB (to 64) × transfers | DMA | hold | |
| | `usb_client`, `usb_open` | 4096, 6144 | internal | hold | |
| | `ring` | `CONFIG_RDOF_SDR_RING_KB` KB + 1 | PSRAM | budget | |
| `radio1` | `roam_t` | `sizeof(roam_t)` | internal | alloc | a radio configured (not "either") |
| | `radio`, `rx`, `modem_rx`, `roam_hblink`, `roam_status` | 8192, 8192, 6144, 4096, 3072 | internal | alloc | |
| | `uart_rx`; `usb_open` + `cdc`; `sdrmodem` | 3072; 4096 + 1536 DMA budget; 4096 | internal | alloc | per port by its `modem_port` |
| | `split` | 4096 | internal | alloc | two ports |
| | `uart_rx_buf`, `uart_tx_buf` | 4096, 2048 × UART ports | internal | hold | a UART port |
| | `mqtt`, `heap` | 9728, 4096 | internal | budget | |
| | `sctp` | 11264 | internal | budget | `transport: sctp` |
| `radio2` | as `radio1`, no `heap` | | `roam_t` PSRAM; stacks internal or PSRAM (fallback) | | a second radio |
| `ipgw` | `ipgw` | 6144 | internal | alloc | the IP gateway configured |
| | `queue` | 64 bursts, up to 256 (elastic) | PSRAM | alloc | |
| | `sockets` | (`max_flows` + 1) × 512 | internal | budget | |
| `usb` | `usb_lib`, `usb_kick` | 4096, 2560 | internal | alloc | anything planned on USB |
| | `host` | 4096 | DMA | budget | |
| `board` | `httpd`, `wifilist`, `netclock` | 9216, 4096, 6144 | internal | budget | always |
| | `oled` | 4096 | internal | budget | `CONFIG_RDOF_OLED` and a role |

Not planned: Wi-Fi and lwIP (the margin), mbedTLS (PSRAM on 16mb), the DSP's other buffers (§5), the page's spectrum,
the bench, the GNSS reader and the status LED (running before the plan, so in the heap it measures), the log ring
and the web page's buffers (PSRAM BSS).

"Either" boards (a radio on USB and an RTL-SDR on the one port, no hub) plan neither role: what is plugged in
decides the role after the plan. Those roles allocate from the heap as before.

## 8. Giving memory back, claiming at run time

- `mem_release(role)`: a stopped role's regions and its holds not yet handed over are freed; the role shows
  `released`. Its tasks must be deleted and its DMA idle first. The SDR releases `sdr` when its front end fails to
  start, so the DSP's memory goes back to the others.
- `mem_claim(role, name, size, align, caps, why, cap)`: a claim at run time, checked against the heap now with the
  margin kept. It reuses a released claim's slot. NULL and `why` if it doesn't fit.
- A role refused at boot doesn't start: `mem_role_state` is −1 and `mem_role_why` says why (the SDR's `rtl_error`,
  the radio's `radio_error` "not started: memory plan: ").

## 9. Status

`GET /api/status` has a `memory` object ([interfaces-http.md](interfaces-http.md#35-memory-memcmem_status_json-memplancrdof_memplan_json)):
the plan's verdict, internal, DMA and PSRAM free with their largest blocks, the margin and spare, and each role's
claims. The page's Maintenance tab shows it as the Memory card.

## 10. Tests

`host-tests/test_memplan.c` (`rdof_memplan_tests`): placement by caps, alignment and padding, HOLD and BUDGET,
refusal (PSRAM short, internal and DMA over the margin, strict and not), fallback, elastic sharing, the layout,
release, run-time claims reusing released slots, the JSON and the log text. `components/lr2021/test`
(`lr2021_driver`): the SPI frames' read lengths, bounces, the caller's memory and a misaligned one refused.

## 11. Config

| Kconfig | Default | Meaning |
|---|---|---|
| `CONFIG_RDOF_MEM_MARGIN_KB` | 80 | internal RAM kept free for Wi-Fi, lwIP and what isn't planned |
| `CONFIG_RDOF_MEM_STRICT` | n | a role over the margin is refused, not started with an error |
