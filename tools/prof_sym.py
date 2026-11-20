#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
"""A board's /api/prof text, by task and function: prof_sym.py ELF PROF.txt [--top N] [--task NAME] [--rom ROM_ELF].

The running image's ELF names the PCs (xtensa-esp32s3-elf-addr2line from ESP-IDF's export.sh; --rom names the ROM's,
esp32s3_rev0_rom.elf from ~/.espressif/tools/esp-rom-elfs). Each task's share of its core's ticks, then its functions
by where the tick landed (self) and by the first caller outside the RTOS's own code (charged): a tick that comes while
interrupts are off is taken where they come back on, so 'self' puts a critical section's time on its exit."""
import argparse
import collections
import subprocess

# where a tick lands after a critical section, or the RTOS's own plumbing: charged to the first caller past these
PLUMBING = {"_xtos_set_intlevel", "vPortClearInterruptMaskFromISR", "vPortExitCriticalSafe", "vPortEnterCriticalSafe",
            "vPortExitCritical", "vPortEnterCritical", "xPortEnterCriticalTimeout",
            "spinlock_release", "spinlock_acquire", "xTaskResumeAll", "vTaskSuspendAll", "xQueueGenericSend",
            "xQueueSemaphoreTake", "xQueueReceive", "xQueueGiveFromISR", "xTaskGenericNotify", "prvNotifyQueueSetContainer",
            "esp_cpu_wait_for_intr", "vPortYield", "xTaskRemoveFromEventList", "??"}


def names(elf, rom, pcs):
    out = {}
    pcs = sorted(pcs)
    if not pcs:
        return out
    for e in [elf] + ([rom] if rom else []):
        todo = [p for p in pcs if out.get(p, "??") == "??"]
        if not todo:
            break
        res = subprocess.run(["xtensa-esp32s3-elf-addr2line", "-f", "-e", e] + ["0x" + p for p in todo],
                             capture_output=True, text=True, check=True).stdout.splitlines()
        for i, p in enumerate(todo):
            if res[2 * i] != "??":
                out[p] = res[2 * i]
            else:
                out.setdefault(p, "??")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("prof")
    ap.add_argument("--rom")
    ap.add_argument("--top", type=int, default=12)
    ap.add_argument("--task")
    a = ap.parse_args()
    rows = []
    with open(a.prof) as f:
        head = f.readline().strip()
        for line in f:
            p = line.split()
            rows.append((int(p[0]), p[1], p[2], [x for x in p[3:] if x != "00000000"]))
    fn = names(a.elf, a.rom, {pc for r in rows for pc in r[3]})
    total = sum(r[0] for r in rows)
    by_task = collections.Counter()
    self_ = collections.defaultdict(collections.Counter)
    charged = collections.defaultdict(collections.Counter)
    cores = {}
    for n, task, core, pcs in rows:
        by_task[task] += n
        cores[task] = core
        chain = [fn.get(pc, "??") for pc in pcs] or ["??"]
        self_[task][chain[0]] += n
        c = next((x for x in chain if x not in PLUMBING), chain[0])
        if c != chain[0]:
            c = f"{c} <- {chain[0]}" if chain[0] in PLUMBING else c
        charged[task][c] += n
    print(head)
    for task, n in by_task.most_common():
        if a.task and task != a.task:
            continue
        print(f"{task} (core {cores[task]}): {n} ticks, {100 * n / total * 2:.1f}% of a core")
        print("  self:")
        for f_, m in self_[task].most_common(a.top):
            print(f"    {m:6d} {100 * m / n:5.1f}%  {f_}")
        print("  charged:")
        for f_, m in charged[task].most_common(a.top):
            print(f"    {m:6d} {100 * m / n:5.1f}%  {f_}")


if __name__ == "__main__":
    main()
