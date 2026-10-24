# Our LR2021 patch RAM image

`rdof_pram.S` is a patch RAM image for the LR2021's core (ARCv2 EM, ROM 1.24), loaded **in place of Semtech's**.
It lets the host tune the FSK receive demodulator, 4-level FSK for DMR first, without the ROM writing its own
values back over ours. MIT, written here. It calls ROM functions at fixed addresses but copies nothing from the
ROM or from Semtech's image. Without Semtech's patch, its fixes (DCDC retune for sub-GHz RX, RTToF, BLE, FLRC,
WM-Bus N, Z-Wave, O-QPSK) are gone.

Status: v2.0 (version word `0x52440200`). It builds and the host tests pass against a model of the chip. It
hasn't run on a chip yet.

## Why bank 1 only

Bench, W12, 2026-10-24: code in SRAM bank 2 never runs. Mapping bank 2 as code (aux 0x80000008 [8:6] = 001) works,
and a data load from 0x102000 then reads the word the host wrote at 0x802000. But executing at 0x102000 hangs the
chip, even after an instruction cache invalidate, and so does Semtech's entry 1 with any bank-2 image. Code at the
end of bank 1 (0x101F00) runs. So only bank 1 is executable, and bank 2 is data only.

Bank 1 is 4 KB (0x101000–0x101FFF). Hooking a ROM call means relocating the whole JLI table (460 entries, 1840
bytes) into executable memory, at 0x1018C0 as Semtech does. That leaves **2240 bytes for the header and code**
(0x101000–0x1018BF). v2.0 uses 1076 of them.

## The core: 16 registers, no caches

Bench, W12, 2026-10-24: the ARC core has the reduced register file (RF_BUILD, aux 0x6E, = 0x00004B02). Only r0–r3,
r10–r15, gp (r26, the ROM's 0x806000: never change it), fp, sp, ilink, r30, blink and lp_count exist. An
instruction naming r4–r9 or r16–r25 traps and hangs the chip (BUSY stuck high), even with the register saved
first. So:

- Arguments in r0–r3, result in r0; r0–r3 and r10–r12 are scratch; r13–r15 and fp are callee-saved.
- `push_s` takes only blink on this core. Save r13–r15 with `enter_s` / `leave_s` (or `st.a` / `ld.ab`).
- `build.sh` refuses the build if the listing names r4–r9 or r16–r25 after the header.
- No instruction or data cache (both BCRs read 0), so 0x0850 returns at once; it's called anyway, as the ROM does.
- About 28–32 MHz: `add` 35–45 ns, `mpy` (32×32) about 100 ns, `mpyw` (16×16) about 105 ns.

## What it does

- **Header (0x101000).** Magic 0x600DB002, info word 0x00520201 (type 0x52, version 0x02, one entry), entry 0.
  What EnablePram 0 checks: the magic at 0x101000, a nonzero low byte in the info word (the entry count) and a
  nonzero entry 0. It calls entry 0 and marks the patch active only if it returns 2. EnablePram n > 0 calls entry n
  only if n < the entry count, so `01 2D 01` fails cleanly here.
- **Init (entry 0).** Asks the ROM whether this is a wake from sleep with retention (0x7CC4, JLI 0xBF).
  - Cold: 0xB3A4(460, 0x1018C0) copies the JLI table into bank 1, flushes the instruction cache and sets
    jli_base. 0xBC88 points four slots at our code; 0x0850 flushes again. It sets up the state in bank 2 if the
    magic isn't there, writes the marker (0x800FF8 = magic, 0x800FFC = info), and returns 2.
  - Wake: sets jli_base back to 0x1018C0 (the table was kept with bank 1), flushes the cache, checks the state in
    bank 2 (an empty table if it was lost), returns 2.
  - Enabled twice: the table is copied onto itself and the same slots are repointed. Harmless.
- **JLI 0x00 (ROM 0x0D14).** Writes 0xF30C30, 0xF30858 and 0xF30C24 [25:24]. The FSK modulation setter calls it
  just before JLI 0x01, and JLI 0x16A (0xE298) calls it from about 20 ROM sites with no JLI 0x01 after.
- **JLI 0x01 (ROM 0x0E70).** The FSK sync and preamble detector setup, from the modulation, preamble and sync word
  setters. It writes 0xF30C14–0xF30C3C.
- **JLI 0x31 (ROM 0x1B9C).** Runs on every frequency change (SetRfFrequency, and the ROM's frequency set 0xFAE8 on
  the SetRx and RX start paths). It rewrites bits of 0xF30C10, 0xF30C20 and 0xF30C28.
- Each of these three wrappers calls the ROM function with r0–r7 untouched, counts the run, applies the table if
  enabled, and returns the ROM function's r0.
- **JLI 0x3D (ROM 0x24E0).** The group-2 command dispatcher. 0x02F0 is handled here; every other group-2 opcode
  jumps to the ROM's dispatcher unchanged.
- **The override table.** Up to 32 entries of (address, mask, value), applied in order. Mask 0 writes the value as
  it is, so several entries in a row can load a port register such as 0xF30C3C. Otherwise only the masked bits
  change. Addresses must be word aligned, in RAM 0x800000–0x80FFFF or registers 0xF00000–0xF4FFFF.

Nothing else is wrapped. A call-graph walk from SetRx (handler 0xFCFC), RX start (JLI 0x10B) and RX analog start
(JLI 0x19E) finds writes to 0xF30C14–0xF30C3C, 0xF30904, 0xF30930 and 0xF30934 only through these three slots. The
walk follows direct calls and JLI only; a function pointer could hide another path.

## State in bank 2

Plain data at 0x802000, never mapped as code: the host reads it with ReadRegMem32.

| Address | Word |
|---|---|
| 0x802000 | Version, 0x52440200 |
| 0x802004 | Magic, 0x52445354 ("RDST"): the state is set up |
| 0x802008 | Enabled (0 or 1) |
| 0x80200C | Entry count |
| 0x802010 | JLI 0x01 wrapper runs |
| 0x802014 | JLI 0x31 wrapper runs |
| 0x802018 | JLI 0x00 wrapper runs |
| 0x80201C | 0x02F0 commands handled |
| 0x802020–0x80219F | The table: 32 × (address, mask, value) |

Bank 2 is also where the SRAM capture route's layouts 2–5 write: don't use those while the table matters. Sleep
with retention keeps bank 1; whether bank 2 is kept depends on the sleep command's retention bits, so the wake path
checks the magic.

## Loading it

1. SetStandby, WriteRegMem32 the image to 0x801000 (blocks of 32 words), EnablePram `01 2D 00`.
2. Check the marker: 0x800FF8 = 0x600DB002, 0x800FFC type (bits 23:16) = 0x52.
3. `02 F0 00` (clear: bank 2 may hold an old table), then read the state at 0x802000 and check the version.

`lr2021_patch_load_rdof()` in `src/lr2021_patch_load.c` does exactly this. `lr2021_patch_load()` still loads
Semtech's image as the general option. Only one can run; a reset removes it.

## Command 0x02F0

Payload byte 0 is a sub-command; numbers are big-endian.

| Sub | Payload after it | What it does | Status |
|---|---|---|---|
| 0x00 | | Empty the table and disable it (the run counts stay; only EnablePram zeroes them) | OK |
| 0x01 | n × (address u32, mask u32, value u32) | Append entries. All or none: a partial entry, a full table or a bad address fails the command | OK or FAIL |
| 0x02 | | Enable, and apply the table now | OK |
| 0x03 | | Disable. What was written stays until the ROM rewrites it | OK |
| 0x04 | | Report, 18 bytes: version u32, count u8, enabled u8, then the JLI 0x01, 0x31 and 0x00 run counts, u32 each | DATA |

The chip passes the payload length as a byte, so an append takes up to 21 entries; the driver sends 10 per frame.

## Rebuilding

Needs GNU binutils for ARC (`arc-elf32-as`, `-ld`, `-objcopy`, `-objdump`; binutils 2.44 was used):

```sh
RDOF_ARC_BIN=/path/to/arc-binutils/bin components/lr2021/pram/build.sh
```

It assembles for `-mcpu=em4_dmips -mcode-density`, links at 0x101000, checks the header is the first word and that
the code ends below 0x1018C0 and uses only registers the core has, and writes `src/lr2021_rdof_pram_image.c` (committed, with the source's SHA-256 in its
header). The ELF, binary and a disassembly listing go to `build/pram/`. Read the listing after a change: ROM calls
must be `jl [r12]` (an absolute address), never a `bl`.

## Room for more

A multi-channel 4-level demodulator on the core has to fit in about 1.1 KB more of code, with its buffers in bank 2
(data) or bank 0. Ways to get more room, none tried:

- Reuse ROM code: the FSK demodulator and correlator are hardware blocks the ROM only configures. On-chip
  multi-channel work would be reprogramming those blocks per channel (time-sliced), not demodulating in software.
- Shrink the JLI table cost: the table must hold all 460 entries, since every ROM `jli_s` goes through jli_base.
  The slots past the highest one the ROM calls in our modes could hold code, if a scan shows they're never used;
  risky.
- Hook without JLI: the interrupt vector table (int_vector_base) can be moved instead, if the work can run from an
  interrupt (the RX FIFO threshold). That costs only the vectors used, freeing about 1.8 KB, but loses the command
  dispatcher hook; parameters would then come through RAM the host writes directly (bank 2).
- Overlays: the host can write new code into bank 1's data alias only while it isn't mapped, so swapping code at
  run time needs a reset. Not practical.

## Confirmed and inferred

Confirmed from the ROM (docs/spec/lr2021-patch.local.md, not published): the header checks, entry 0's call and its
return code, JLI 0xBF, 0xB3A4, 0xBC88, 0x0850, the dispatcher's arguments and response layout (word 0 the answer
length, word 1 the answer buffer), and which ROM functions write 0xF30C14–0xF30C3C.

Bench, W12, 2026-10-24: it loads and starts (marker 0x00520201); its state reads and writes at 0x802000 while bank
2 isn't mapped as code; 0x02F0 answers and every other group-2 command still reaches the ROM; FSK packet RX works
with ours alone; the run counts rise (SetRfFrequency, the sync setup, JLI 0x00 during RX); the table holds through
16 s RX sessions (the DMR 4-FSK setup of `lr2021_fsk4_dmr_rx`, docs/spec/lr2021-iq.md R3.40 to R3.42). A packet RX
that ends in its timeout hasn't left BUSY high with ours, as it sometimes does with Semtech's.

Inferred, to check on a chip:

- That the ROM calls entry 0 again after a retention wake (Semtech's init has a wake path, so it should).
- That nothing else rewrites the overridden registers in other setups: the call-graph walk can miss calls through
  function pointers, and the hardware may change some fields itself.
