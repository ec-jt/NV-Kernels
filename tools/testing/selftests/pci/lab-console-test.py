#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise the real NP-relief table and hooks using synthetic PCI devices.

Does not emulate resource allocation, USB enumeration, or a BMC console.
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
    extract = runpy.run_path(str(HERE / "lab-rebar-test.py"))["function"]
    source = (ROOT / "drivers/pci/quirks.c").read_text()
    start = source.index("enum lab_dev_tag {")
    end = source.index("static bool lab_np_target", start)
    code = source[start:end] + "\n" + "\n".join(extract(source, name) for name in (
        "lab_np_target", "quirk_lab_free_np_early", "quirk_lab_free_np_header"))
    harness = (HERE / "lab-console-test.c").read_text().replace(
        "/* INSERT_KERNEL_FUNCTIONS */", code)
    with tempfile.TemporaryDirectory(prefix="lab-console-test-") as directory:
        cfile, executable = Path(directory) / "test.c", Path(directory) / "test"
        cfile.write_text(harness)
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(cfile),
            "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
