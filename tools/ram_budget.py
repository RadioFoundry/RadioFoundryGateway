#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
"""A firmware build's internal RAM against its budget: ram_budget.py BUILD_DIR [--profile w12] [--update].

The firmware build runs it after each link (esp_idf/CMakeLists.txt), the profile the build dir's name (build/w12:
w12) or -D RDOF_RAM_BUDGET=NAME.

Internal RAM is the tight resource: what the image takes statically (.data, .bss and code in IRAM) comes out of the
heap that Wi-Fi, lwIP, the tasks' stacks and SPI DMA share at run time (about 10 KB free on a W12 while
streaming). Each profile's budget, in tools/ram_budget.json, is the internal bytes it may take and, per archive, what
it took when the budget was last set. A build over its budget fails (exit 1) and lists the archives that grew, so
the growth is a decision: move it to PSRAM (EXT_RAM_BSS_ATTR) or out of IRAM, or raise the budget with --update in
the same commit, saying why. The run-time floor is hil-tests/test_perf.py's (the heap during heavy jobs).

Run it from ESP-IDF's environment (export.sh): it reads the build's map with esp_idf_size.
"""
import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BUDGET = os.path.join(HERE, "ram_budget.json")
INTERNAL = (".dram0.bss", ".dram0.data", ".iram0.text", ".iram0.vectors")
SLACK = 1024  # --update sets the budget this far over the build


def size(build, *args):
    maps = [f for f in os.listdir(build) if f.endswith(".map")]
    if len(maps) != 1:
        sys.exit(f"{build}: expected one .map file, found {maps}")
    out = subprocess.run([sys.executable, "-m", "esp_idf_size", "--format", "json", *args, os.path.join(build, maps[0])],
                         capture_output=True, text=True, check=True).stdout
    return json.loads(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("build")
    ap.add_argument("--profile", default=None, help="the budget's name (default: the build dir's name)")
    ap.add_argument("--update", action="store_true", help="set the budget from this build (plus 1 KB)")
    ap.add_argument("--if-budgeted", action="store_true", help="a profile with no budget passes (the build's hook)")
    a = ap.parse_args()
    profile = a.profile or os.path.basename(os.path.normpath(a.build))
    total = size(a.build)
    used = total["used_diram"] + total["used_iram"] + total.get("used_dram", 0)
    archives = {k: sum(v.get(s, 0) for s in INTERNAL) for k, v in size(a.build, "--archives").items()}
    archives = {k: n for k, n in archives.items() if n}
    budgets = json.load(open(BUDGET)) if os.path.exists(BUDGET) else {}
    if a.update:
        budgets[profile] = {"internal_max": used + SLACK, "internal": used, "iram_free": total["iram_remain"],
                            "archives": dict(sorted(archives.items(), key=lambda kv: -kv[1]))}
        with open(BUDGET, "w") as f:
            json.dump(budgets, f, indent=1)
            f.write("\n")
        print(f"{profile}: budget {used + SLACK} bytes (the build takes {used})")
        return 0
    b = budgets.get(profile)
    if not b and a.if_budgeted:
        print(f"{profile}: no internal RAM budget (tools/ram_budget.json)")
        return 0
    if not b:
        sys.exit(f"{profile}: no budget in {BUDGET} (--update sets one)")
    grew = sorted(((n - b["archives"].get(k, 0), k) for k, n in archives.items() if n > b["archives"].get(k, 0)),
                  reverse=True)
    print(f"{profile}: internal RAM {used} bytes of a {b['internal_max']} budget "
          f"({used - b['internal']:+d} since it was set); IRAM free {total['iram_remain']}")
    for d, k in grew[:12]:
        print(f"  {k}: +{d}")
    if used > b["internal_max"]:
        print(f"{profile}: OVER BUDGET by {used - b['internal_max']} bytes: move it to PSRAM or out of IRAM, or raise "
              "the budget (--update) in the same commit, saying why")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
