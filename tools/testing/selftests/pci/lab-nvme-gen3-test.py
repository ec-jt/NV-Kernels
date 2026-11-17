#!/usr/bin/env python3
"""Exercise the real boot helper against fake PCI sysfs and setpci."""
from pathlib import Path
import subprocess, tempfile
ROOT = Path(__file__).resolve().parents[4]
SOURCE = (ROOT/'scripts/lab-nvme-gen3-initramfs').read_text()
assert 'uname' not in SOURCE
cases = [('', False), ('lab_nvme_gen3=0', False),
         ('lab_nvme_gen3=1', True), ('lab_nvme_gen3=invalid', False),
         ('lab_nvme_gen3=1 lab_nvme_gen3=0', False),
         ('lab_nvme_gen3=0 lab_nvme_gen3=1', True)]
for cmdline, enabled in cases:
    with tempfile.TemporaryDirectory() as name:
        t=Path(name); sys=t/'sys'; parent=sys/'devices/pci0000:80/0000:80:01.5'
        endpoint=parent/'0000:85:00.0'; endpoint.mkdir(parents=True)
        (parent/'class').write_text('0x060400\n')
        (endpoint/'class').write_text('0x010802\n')
        devices=sys/'bus/pci/devices'; devices.mkdir(parents=True)
        (devices/'0000:85:00.0').symlink_to(endpoint)
        dmi=sys/'class/dmi/id';dmi.mkdir(parents=True)
        (dmi/'product_name').write_text('G292-Z20-00\n')
        (t/'cmdline').write_text(cmdline+'\n')
        stub=t/'setpci'
        stub.write_text('#!/bin/sh\n' +
            f'echo "$*" >> "{t}/calls"\n' +
            'case "$3" in\n' +
            'CAP_EXP+02.w) echo 0042;;\n' +
            'CAP_EXP+30.w) echo 0004;;\n' +
            f'CAP_EXP+12.w) if [ -e "{t}/trained" ]; then echo 0043; else echo 0044; fi;;\n' +
            'CAP_EXP+30.w=0003:000f) ;;\n' +
            f'CAP_EXP+10.w=0020:0020) touch "{t}/trained";;\n' +
            '*) exit 99;;\nesac\n')
        stub.chmod(0o755)
        script=t/'helper'
        script.write_text(SOURCE.replace('> /dev/kmsg','>> /dev/kmsg')
                          .replace('/proc/cmdline',str(t/'cmdline'))
                          .replace('/sys/',str(sys)+'/')
                          .replace('/sbin/setpci',str(stub))
                          .replace('/dev/kmsg',str(t/'kmsg')))
        subprocess.run(['sh',str(script)],check=True,timeout=10)
        assert (t/'calls').exists() == enabled,cmdline
        if enabled:
            assert (t/'trained').exists(),cmdline
            assert 'capped Gen3 negotiated Gen3 x4' in (t/'kmsg').read_text()
        # The same opt-in must never touch an already-bound controller.
        (endpoint/'driver').symlink_to(t/'driver')
        (t/'driver').mkdir()
        (t/'cmdline').write_text('lab_nvme_gen3=1\n')
        calls=(t/'calls').read_text() if (t/'calls').exists() else ''
        subprocess.run(['sh',str(script)],check=True,timeout=10)
        assert ((t/'calls').read_text() if (t/'calls').exists() else '') == calls
print('PASS: default-off, explicit opt-in, last-value precedence, Gen3 retrain, bound-device skip')
