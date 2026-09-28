#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Read-only BAR and ancestor-window validation for the mixed NVIDIA lab node."""

import argparse
from pathlib import Path
import re
import sys


MIB = 1 << 20
GIB = 1 << 30
MEM = 0x200
PREFETCH = 0x2000
MEM64 = 0x100000
UNSET = 0x20000000
DISABLED = 0x10000000
TARGETS = {0x2C05: ("RTX 5070 Ti", 16 * GIB), 0x2B85: ("RTX 5090", 32 * GIB)}
BDF = re.compile(r"[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]$")


def resources(device):
    return [tuple(int(word, 16) for word in line.split())
            for line in (device / "resource").read_text().splitlines()]


def assigned(resource):
    start, end, flags = resource
    return start > 0 and end >= start and flags & MEM and not flags & (UNSET | DISABLED)


def covers(window, bar):
    return assigned(window) and window[0] <= bar[0] <= bar[1] <= window[1]


def check_gpu(device, expected):
    errors = []
    bars = resources(device)
    for index, size in ((0, 64 * MIB), (1, expected)):
        bar = bars[index]
        if not assigned(bar) or bar[1] - bar[0] + 1 != size:
            errors.append(f"BAR{index}: expected assigned {size // MIB} MiB, got {bar}")
        if index == 1 and (bar[2] & (MEM | PREFETCH | MEM64)) != (MEM | PREFETCH | MEM64):
            errors.append("BAR1: missing 64-bit prefetchable memory flags")
        if index == 1 and bar[0] % expected:
            errors.append("BAR1: base is not aligned to the requested size")
        for parent in device.resolve().parents:
            if not BDF.fullmatch(parent.name):
                continue
            # sysfs exposes bridge windows after the six BARs and ROM.
            windows = resources(parent)[7:]
            suitable = [w for w in windows if bool(w[2] & PREFETCH) == (index == 1)]
            if not any(covers(w, bar) for w in suitable):
                errors.append(f"BAR{index}: not covered by ancestor {parent.name}")
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sysfs", type=Path, default=Path("/sys/bus/pci/devices"))
    parser.add_argument("--expect-5070ti", type=int, default=8)
    parser.add_argument("--expect-5090", type=int, default=4)
    args = parser.parse_args()
    counts = dict.fromkeys(TARGETS, 0)
    failures = 0
    bar1_ranges = []
    for device in sorted(args.sysfs.iterdir()):
        if int((device / "vendor").read_text(), 16) != 0x10DE:
            continue
        ident = int((device / "device").read_text(), 16)
        if ident not in TARGETS or not device.name.endswith(".0"):
            continue
        counts[ident] += 1
        name, size = TARGETS[ident]
        errors = check_gpu(device, size)
        bar = resources(device)[1]
        if assigned(bar):
            for other_name, other in bar1_ranges:
                if bar[0] <= other[1] and other[0] <= bar[1]:
                    errors.append(f"BAR1: overlaps GPU {other_name}")
            bar1_ranges.append((device.name, bar))
        driver = device / "driver"
        owner = driver.resolve().name if driver.exists() else "unbound"
        print(f"{'FAIL' if errors else 'PASS'} {device.name} {name} "
              f"BAR1={max(0, bar[1] - bar[0] + 1) // MIB}MiB driver={owner}")
        for error in errors:
            print("  " + error)
        failures += bool(errors)
    for ident, expected in ((0x2C05, args.expect_5070ti), (0x2B85, args.expect_5090)):
        if counts[ident] != expected:
            print(f"FAIL: expected {expected} {TARGETS[ident][0]}, found {counts[ident]}")
            failures += 1
    print(f"Resource validation: {sum(counts.values())} GPUs, {failures} failures")
    return int(bool(failures))


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"Cannot read PCI resources: {error}", file=sys.stderr)
        sys.exit(2)
