#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Compile the actual opt-in parser and quirk with synthetic PCI devices.

Checks the fail-closed boundary, not PCIe events or driver removal safety.
"""
import os
from pathlib import Path
import runpy
import shlex
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]


def main():
    source = (ROOT / "drivers/pci/quirks.c").read_text()
    start = source.index("#define LAB_SWITCHTEC_HP_MAX")
    end = source.index("static bool pcie_lbms_seen", start)
    harness = (HERE / "lab-hotplug-test.c").read_text().replace(
        "/* INSERT_KERNEL_FUNCTIONS */", source[start:end])
    extract = runpy.run_path(str(HERE / "lab-rebar-test.py"))["function"]
    ctrl = (ROOT / "drivers/pci/hotplug/pciehp_ctrl.c").read_text()
    harness = harness.replace("/* INSERT_BOUND_GUARD */",
                              extract(ctrl, "pciehp_non_hpc_gpu_bound"))
    with tempfile.TemporaryDirectory(prefix="lab-hotplug-test-") as directory:
        cfile, executable = Path(directory) / "test.c", Path(directory) / "test"
        cfile.write_text(harness)
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(cfile),
            "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
