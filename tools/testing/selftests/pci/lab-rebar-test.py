#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Compile the actual lab PCI floor/sort functions with synthetic resources.

This host test does not emulate PCI config space or replace a kernel boot test.
It requires Python 3 and a C compiler, but no kernel headers or GPU hardware.
"""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]


def function(source, name):
    match = re.search(r"^static (?:void|bool) " + name + r"\(", source, re.M)
    if match is None:
        raise RuntimeError("missing function: " + name)
    start = source.index("{", match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def main():
    source = (ROOT / "drivers/pci/setup-bus.c").read_text()
    quirks = (ROOT / "drivers/pci/quirks.c").read_text()
    for phase in ("EARLY", "RESUME_EARLY"):
        assert re.search(r"DECLARE_PCI_FIXUP_" + phase +
                         r"\(PCI_VENDOR_ID_NVIDIA, 0x2c05,", quirks)
    sizing = function(source, "pbus_size_mem")
    assert "release_child_resources" not in sizing
    assert "size += max(r_size, align);" in sizing
    assignment = source[source.index("void pci_assign_unassigned_root_bus_resources("):]
    assert assignment.index("pci_walk_bus(bus, lab_release_rebar_windows") < \
        assignment.index("__pci_bus_size_bridges(bus, add_list)")

    functions = "\n\n".join(function(source, name) for name in (
        "pci_dev_res_free_list", "pdev_sort_resources", "lab_bus_mem_floor"))
    harness = (HERE / "lab-rebar-test.c").read_text()
    harness = harness.replace("/* INSERT_KERNEL_FUNCTIONS */", functions)
    with tempfile.TemporaryDirectory(prefix="lab-rebar-test-") as directory:
        cfile = Path(directory) / "test.c"
        executable = Path(directory) / "test"
        cfile.write_text(harness)
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(cfile),
            "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
    print("PASS: device registration, firmware NP preservation, pre-sizing order")


if __name__ == "__main__":
    main()
