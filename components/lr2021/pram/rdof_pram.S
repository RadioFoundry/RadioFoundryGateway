/* SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors */
/*
 * RadioFoundry Gateway's own LR2021 patch RAM image (v2.0), for ROM 1.24. It replaces Semtech's patch: load one or
 * the other, not both.
 *
 * It runs from SRAM bank 1: loaded at 0x801000, run at 0x101000 (the code window) by EnablePram (`01 2D 00`). Bank 2
 * isn't executable (bench, W12, 2026-10-24), so the code, and the relocated JLI table it needs, fit in bank 1's 4 KB;
 * its state and the override table live in bank 2 as plain data (0x802000), which the host can read directly.
 *
 * What it does: a table of register overrides (address, mask, value) that it applies after the ROM programs the
 * FSK sync and preamble detector, after every frequency change and after the ROM's sample-rate helper, so the host
 * can tune the demodulator (4-level FSK for DMR first) without the ROM undoing it. It hooks four JLI slots: three
 * demodulator setters and the group-2 command dispatcher, for command 0x02F0.
 *
 * Facts used (docs/spec/lr2021-patch.local.md, read from ROM 1.24; nothing of the ROM or of Semtech's patch is
 * copied here, only addresses that are called):
 *   EnablePram 0: the ROM maps bank 1 as code, checks the magic at 0x101000, needs the info word's low byte (the
 *     entry count) and entry 0 (0x101008) nonzero, calls entry 0, and marks the patch active if it returns 2.
 *     EnablePram n > 0 calls entry n if n < the entry count, else fails.
 *   0x7CC4 (JLI 0xBF): nonzero on a wake from sleep with retention.
 *   0xB3A4(count, base): copies the current JLI table (count entries) to base, flushes the instruction cache and
 *     sets jli_base (aux 0x290).
 *   0xBC88(index, target) repoints entry `index` of the table at jli_base. It doesn't flush the cache.
 *   0x0850 invalidates the instruction cache. It uses r0 and r1.
 *   JLI 0x00 (ROM 0x0D14): writes 0xF30C30, 0xF30858 and 0xF30C24 [25:24]. Called by the FSK modulation setter just
 *     before JLI 0x01, and from about 20 ROM paths through JLI 0x16A with no JLI 0x01 after.
 *   JLI 0x01 (ROM 0x0E70): the FSK sync and preamble detector setup, called by the FSK modulation, preamble and
 *     sync word setters. It writes 0xF30C14-0xF30C3C.
 *   JLI 0x31 (ROM 0x1B9C): run on every frequency change. It rewrites 0xF30C10, 0xF30C20 and 0xF30C28 bits.
 *   JLI 0x3D (ROM 0x24E0): the group-2 command dispatcher, called as (low opcode byte, payload length, payload,
 *     response). It returns the status: 2 OK, 1 error, 3 with an answer (response word 0 = answer length, word 1 =
 *     the answer buffer).
 *
 * The wrappers chain to the ROM's functions directly: no other patch runs, so the slots held the ROM's targets.
 * ROM functions are called through a register (jl): bl takes a PC-relative offset, not an absolute address.
 * The core has the reduced 16-register file (RF_BUILD 0x00004B02; bench, W12, 2026-10-24): r4-r9 and r16-r25 don't
 * exist and any instruction naming them traps and hangs the chip. Use r0-r3, r10-r15, fp, sp, blink only; build.sh
 * refuses a listing that names another. push_s takes only blink here: save r13-r15 with enter_s / leave_s.
 * Calling convention: arguments in r0-r3, result in r0, r0-r3 and r10-r12 free to use, r13-r15 and fp kept.
 * SPDX-License-Identifier: MIT
 */

        .equ    PRAM_MAGIC,     0x600DB002
        .equ    PRAM_INFO,      0x00520201      /* type 0x52 ('R'), version 0x02, 1 entry */
        .equ    MARK_MAGIC,     0x00800FF8      /* the "loaded" marker the host reads */
        .equ    MARK_INFO,      0x00800FFC

        .equ    ROM_IS_WAKE,    0x00007CC4
        .equ    ROM_JLI_COPY,   0x0000B3A4
        .equ    ROM_JLI_SET,    0x0000BC88
        .equ    ROM_IC_FLUSH,   0x00000850
        .equ    AUX_JLI_BASE,   0x290
        .equ    JLI_COUNT,      460
        .equ    JLI_TABLE,      0x001018C0      /* 1840 bytes, to 0x101FF0; the code must end below it */

        .equ    SLOT_SAMP,      0x00
        .equ    ROM_SAMP,       0x00000D14
        .equ    SLOT_SYNC,      0x01
        .equ    ROM_SYNC,       0x00000E70
        .equ    SLOT_RF,        0x31
        .equ    ROM_RF,         0x00001B9C
        .equ    SLOT_CMD,       0x3D
        .equ    ROM_CMD,        0x000024E0

        .equ    RDOF_VERSION,   0x52440200      /* "RD", v2.0 */
        .equ    STATE_MAGIC,    0x52445354      /* "RDST": the state below is ours */
        .equ    MAX_ENTRIES,    32

/* State in bank 2 as data, never mapped as code: the host reads it with ReadRegMem32 at 0x802000.
 * Bank 2 is kept through sleep only if the host's sleep command retains it; the wake path checks the magic. */
        .equ    STATE,          0x00802000
        .equ    ST_VERSION,     0x00
        .equ    ST_MAGIC,       0x04
        .equ    ST_ENABLED,     0x08
        .equ    ST_COUNT,       0x0C
        .equ    ST_RUNS_SYNC,   0x10            /* times each wrapper ran */
        .equ    ST_RUNS_RF,     0x14
        .equ    ST_RUNS_SAMP,   0x18
        .equ    ST_CMDS,        0x1C            /* 0x02F0 commands handled */
        .equ    ST_TABLE,       0x20            /* MAX_ENTRIES x {address, mask, value}, to 0x8021A0 */

        .equ    OP_RDOF,        0xF0            /* our command: 0x02F0 */

/* Load a big-endian word from byte address ptr+off (any alignment). */
        .macro  LDBE dst, ptr, off, tmp
        ldb     \dst, [\ptr, \off]
        ldb     \tmp, [\ptr, \off + 1]
        asl     \dst, \dst, 8
        or      \dst, \dst, \tmp
        ldb     \tmp, [\ptr, \off + 2]
        asl     \dst, \dst, 8
        or      \dst, \dst, \tmp
        ldb     \tmp, [\ptr, \off + 3]
        asl     \dst, \dst, 8
        or      \dst, \dst, \tmp
        .endm

/* Store a word big-endian at byte address ptr+off. */
        .macro  STBE src, ptr, off, tmp
        lsr     \tmp, \src, 24
        stb     \tmp, [\ptr, \off]
        lsr     \tmp, \src, 16
        stb     \tmp, [\ptr, \off + 1]
        lsr     \tmp, \src, 8
        stb     \tmp, [\ptr, \off + 2]
        stb     \src, [\ptr, \off + 3]
        .endm

        .text
        .global rdof_header

/* ── Header, at 0x101000: what EnablePram checks ── */
rdof_header:
        .word   PRAM_MAGIC
        .word   PRAM_INFO
        .word   rdof_init                       /* entry 0 */

/* ── Entry 0: init, run by `01 2D 00`, and by the ROM again after every wake from sleep with retention ──
 * Returns 2, or the ROM doesn't mark the patch active. */
        .align  4
rdof_init:
        push_s  blink
        mov     r12, ROM_IS_WAKE
        jl      [r12]
        breq    r0, 0, 1f
        mov     r0, JLI_TABLE           /* wake: bank 1 and its table were kept; point the core back at it */
        sr      r0, [AUX_JLI_BASE]
        mov     r12, ROM_IC_FLUSH
        jl      [r12]
        bl      rdof_state
        b       9f

1:      mov     r0, JLI_COUNT           /* cold: copy the current table into bank 1 and use it */
        mov     r1, JLI_TABLE
        mov     r12, ROM_JLI_COPY
        jl      [r12]
        mov     r0, SLOT_SAMP           /* point our slots at our wrappers (again, if enabled twice) */
        mov     r1, rdof_wrap_samp
        mov     r12, ROM_JLI_SET
        jl      [r12]
        mov     r0, SLOT_SYNC
        mov     r1, rdof_wrap_sync
        mov     r12, ROM_JLI_SET
        jl      [r12]
        mov     r0, SLOT_RF
        mov     r1, rdof_wrap_rf
        mov     r12, ROM_JLI_SET
        jl      [r12]
        mov     r0, SLOT_CMD
        mov     r1, rdof_dispatch
        mov     r12, ROM_JLI_SET
        jl      [r12]
        mov     r12, ROM_IC_FLUSH       /* the table changed under the instruction cache */
        jl      [r12]
        bl      rdof_state
        mov     r12, STATE              /* run counts are per EnablePram (RAM keeps old ones across a reset) */
        mov     r0, 0
        st      r0, [r12, ST_RUNS_SYNC]
        st      r0, [r12, ST_RUNS_RF]
        st      r0, [r12, ST_RUNS_SAMP]
        mov     r12, MARK_MAGIC         /* the marker the host checks: our header's first two words */
        mov     r0, PRAM_MAGIC
        st      r0, [r12]
        mov     r0, PRAM_INFO
        st      r0, [r12, 4]

9:      mov     r0, 2
        pop_s   blink
        j_s     [blink]

/* Set up the state on a first start, or when bank 2 lost it (power-up junk, or sleep without its retention):
 * an empty, disabled table. A second enable keeps it. Uses r0-r2 and r12. */
        .align  4
rdof_state:
        mov     r12, STATE
        ld      r0, [r12, ST_MAGIC]
        mov     r1, STATE_MAGIC
        breq    r0, r1, 1f
        mov     r2, 0
        st      r2, [r12, ST_ENABLED]
        st      r2, [r12, ST_COUNT]
        st      r2, [r12, ST_RUNS_SYNC]
        st      r2, [r12, ST_RUNS_RF]
        st      r2, [r12, ST_RUNS_SAMP]
        st      r2, [r12, ST_CMDS]
        st      r1, [r12, ST_MAGIC]
1:      mov     r0, RDOF_VERSION
        st      r0, [r12, ST_VERSION]
        j_s     [blink]

/* ── The wrappers: the ROM's function, then our table if enabled ──
 * Arguments pass through untouched; the ROM function's result in r0 is returned. Each stub saves r13, r14 and blink
 * (enter_s: push_s takes only blink on this core), puts the ROM target in r12 and its run counter's offset in r14,
 * and goes to the shared part, which returns with leave_s. */
        .align  4
rdof_wrap_sync:                         /* JLI 0x01: the sync / preamble detector setup */
        enter_s [r13-r14, blink]
        mov     r12, ROM_SYNC
        mov     r14, ST_RUNS_SYNC
        b       rdof_wrap
        .align  4
rdof_wrap_rf:                           /* JLI 0x31: the per-frequency demodulator update */
        enter_s [r13-r14, blink]
        mov     r12, ROM_RF
        mov     r14, ST_RUNS_RF
        b       rdof_wrap
        .align  4
rdof_wrap_samp:                         /* JLI 0x00: 0xF30C30 and 0xF30C24 [25:24] */
        enter_s [r13-r14, blink]
        mov     r12, ROM_SAMP
        mov     r14, ST_RUNS_SAMP
        b       rdof_wrap
        .align  4
rdof_wrap:
        jl      [r12]
        mov     r13, r0
        add     r14, r14, STATE
        ld      r1, [r14]
        add     r1, r1, 1
        st      r1, [r14]
        mov     r12, STATE
        ld      r1, [r12, ST_ENABLED]
        breq    r1, 0, 1f
        bl      rdof_apply
1:      mov     r0, r13
        leave_s [r13-r14, blink, pcl]

/* ── Apply the table, in order. Mask 0: a plain write (successive entries to a port such as 0xF30C3C load it
 * in order). Otherwise *addr = (*addr & ~mask) | (value & mask). Uses r0-r3 and r10-r12 only. */
        .align  4
rdof_apply:
        mov     r12, STATE
        ld      r0, [r12, ST_COUNT]
        add     r1, r12, ST_TABLE
1:      breq    r0, 0, 9f
        ld      r2, [r1, 0]             /* address */
        ld      r3, [r1, 4]             /* mask */
        ld      r10, [r1, 8]            /* value */
        breq    r3, 0, 2f
        ld      r11, [r2]
        bic     r11, r11, r3
        and     r10, r10, r3
        or      r10, r10, r11
2:      st      r10, [r2]
        add     r1, r1, 12
        sub     r0, r0, 1
        b       1b
9:      j_s     [blink]

/* ── JLI 0x3D: the group-2 dispatcher. 0x02F0 is ours; everything else goes to the ROM's, untouched ──
 * r0 = opcode low byte, r1 = payload length, r2 = payload, r3 = response. Uses r0-r3 and r10-r12 only.
 * Payload byte 0 is a sub-command:
 *   0x00 clear the table and disable                  0x02 enable, and apply now    0x03 disable
 *   0x01 append n x {address, mask, value}, each u32 big-endian (n <= 21 per command; all or none)
 *   0x04 report, 18 bytes, big-endian: version u32, count u8, enabled u8, then the times the JLI 0x01, 0x31
 *        and 0x00 wrappers ran since EnablePram, u32 each */
        .align  4
rdof_dispatch:
        breq    r0, OP_RDOF, 1f
        mov     r12, ROM_CMD
        j       [r12]                   /* the ROM's dispatcher returns to our caller */
1:      mov     r12, STATE
        ld      r10, [r12, ST_CMDS]
        add     r10, r10, 1
        st      r10, [r12, ST_CMDS]
        breq    r1, 0, 8f
        ldb     r10, [r2, 0]
        cmp     r10, 0
        beq     10f
        cmp     r10, 1
        beq     20f
        cmp     r10, 2
        beq     30f
        cmp     r10, 3
        beq     40f
        cmp     r10, 4
        beq     50f
8:      mov     r0, 1                   /* error */
        j_s     [blink]
9:      mov     r0, 2                   /* OK */
        j_s     [blink]

10:     mov     r10, 0                  /* clear; the run counts stay (only init zeroes them) */
        st      r10, [r12, ST_COUNT]
        st      r10, [r12, ST_ENABLED]
        b       9b

/* append: r1 bytes left, r2 the next entry, r0 the new count, r3 the next free table slot. Entries are copied as
 * they're checked; the count moves only when all are good, so a failed append leaves the table as it was. */
20:     sub     r1, r1, 1
        breq    r1, 0, 8b
        add     r2, r2, 1
        ld      r0, [r12, ST_COUNT]
        asl     r11, r0, 3              /* count x 12 */
        add     r3, r12, r11
        asl     r11, r0, 2
        add     r3, r3, r11
        add     r3, r3, ST_TABLE
21:     breq    r1, 0, 29f
        brlo    r1, 12, 8b              /* not a whole entry */
        brhs    r0, MAX_ENTRIES, 8b     /* table full */
        LDBE    r10, r2, 0, r11         /* address */
        and     r11, r10, 3
        brne    r11, 0, 8b              /* not word aligned */
        lsr     r11, r10, 16
        brlo    r11, 0x80, 8b           /* RAM 0x800000-0x80FFFF or registers 0xF00000-0xF4FFFF only */
        brlo    r11, 0x81, 22f
        brlo    r11, 0xF0, 8b
        brhs    r11, 0xF5, 8b
22:     st      r10, [r3, 0]
        LDBE    r10, r2, 4, r11         /* mask */
        st      r10, [r3, 4]
        LDBE    r10, r2, 8, r11         /* value */
        st      r10, [r3, 8]
        add     r2, r2, 12
        add     r3, r3, 12
        sub     r1, r1, 12
        add     r0, r0, 1
        b       21b
29:     st      r0, [r12, ST_COUNT]
        b       9b

30:     mov     r10, 1                  /* enable, and apply now */
        st      r10, [r12, ST_ENABLED]
        push_s  blink
        bl      rdof_apply
        pop_s   blink
        b       9b

40:     mov     r10, 0                  /* disable (what was written stays until the ROM rewrites it) */
        st      r10, [r12, ST_ENABLED]
        b       9b

50:     ld      r1, [r3, 4]             /* report: r1 = the answer buffer */
        ld      r10, [r12, ST_VERSION]
        STBE    r10, r1, 0, r11
        ld      r10, [r12, ST_COUNT]
        stb     r10, [r1, 4]
        ld      r10, [r12, ST_ENABLED]
        stb     r10, [r1, 5]
        ld      r10, [r12, ST_RUNS_SYNC]
        STBE    r10, r1, 6, r11
        ld      r10, [r12, ST_RUNS_RF]
        STBE    r10, r1, 10, r11
        ld      r10, [r12, ST_RUNS_SAMP]
        STBE    r10, r1, 14, r11
        mov     r10, 18
        st      r10, [r3, 0]            /* answer length */
        mov     r0, 3
        j_s     [blink]

        .global rdof_end
rdof_end:
