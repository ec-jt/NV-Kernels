# USB and BMC console recovery

Candidate branch: `fix/rebar-usb-bmc-console`, based on working release commit
`53405a42326142c0f8f7b2402274730d82a770d7`. This retains the mixed-topology
ReBAR fixes in `setup-bus.c`. AER configuration and recovery are out of scope.

## Observed baseline, 2026-11-05

The G292-Z20 test host runs `7.0.0-rebar-iommufd-btf`, with eight idle RTX 5090s
visible in NVIDIA 615.78.08. Its boot command line includes `pci=noaer`.

- The NP-relief quirk explicitly disables AMD 1022:148c at 07:00.3 and 47:00.3,
  and ASMedia 1b21:1142 at 81:00.0. Each xHCI probe then fails with error -16.
- No USB devices are enumerated. The only input device is the ACPI power button.
- AST2500 at 84:00.0 is bound; `astdrmfb` provides fb0 at 1024x768, fbcon is
  bound, and getty@tty1 is active. This establishes the Linux framebuffer path,
  not that the remote BMC viewer displays it or delivers keyboard events.

## Candidate change

Remove both explicit USB IDs and the legacy USB class entry from the destructive
NP-relief table. Neither the EARLY hardware BAR clearing nor the HEADER resource
clearing then runs on those controllers. Keep ASPEED and Switchtec management
excluded. Leave SATA behavior, GPU ReBAR sizing, and bridge allocation unchanged.

The USB and AHCI class constants already contain the programming interface.
The old table shifted these full class codes again, so its purported generic
USB/AHCI matches did not work as documented. Do not correct the SATA expression
as part of this change: doing so would disable additional storage devices.

The config fragment builds HID, generic HID, and USB HID into the kernel so
ordinary keyboard/mouse support does not depend on initramfs module inclusion.
AST and framebuffer console stay built in. No change to the AST driver is
justified by the baseline logs.

## Build

Use a full Linux source checkout (the package-artifact `debs` directory is not
needed), an out-of-tree build directory, and the tested baseline config:

```sh
mkdir -p ../build
sh scripts/kconfig/merge_config.sh -m -O ../build \
    configs/config-7.0.0-rebar-mixed-5070ti-5090 configs/rebar-usb-bmc.config
make O=../build LOCALVERSION= olddefconfig
make O=../build LOCALVERSION= syncconfig
make -s O=../build LOCALVERSION= kernelrelease
# Must be 7.0.0-rebar-usb-bmc-test1, distinct from installed fallbacks.
make O=../build LOCALVERSION= -j24
```

Preserve BTF and IOMMUFD. Review the generated config, build matching NVIDIA
modules, and verify initramfs and the one-time GRUB entry before a test boot.
Keep the old kernel and its boot parameters available. Check existing AST/HID
blacklists and console routing before changing boot flags; do not rewrite the
entire command line or modify the AER settings during this test.

## Validation

```sh
python3 tools/testing/selftests/pci/lab-console-test.py
python3 tools/testing/selftests/pci/lab-rebar-test.py
# On this eight-5090 host after boot:
python3 tools/testing/selftests/pci/validate-rebar.py --expect-5070ti 0 --expect-5090 8
lsusb -t
cat /proc/bus/input/devices
cat /sys/class/graphics/fb0/name
sudo dmesg | grep -Ei 'xhci|usb|hid|ast|fbcon|LAB: matched NP target'
```

The new host harness compiles the actual NP-relief table and hooks against
synthetic devices. It checks that USB, AST and Switchtec resources are untouched,
while the existing explicit AMD SATA target still exercises the destructive
path. It does not emulate PCI resource allocation or prove USB functionality.

Hardware acceptance requires all three controllers bound with assigned MMIO,
USB enumeration, a BMC virtual keyboard/mouse where enabled, visible remote
console output and a real input-event check, plus eight usable 32 GiB GPU BAR1
allocations. Verify the management network, NVMe, Switchtec management and a
small CUDA workload. Restoring USB consumes MMIO that the old quirk deliberately
removed; only a boot test can confirm the complete allocation still fits.

## Build results, 2026-11-05

- Host console and ReBAR regression tests passed on macOS and the test server.
- Full Linux kernel/modules build passed with release
  `7.0.0-rebar-usb-bmc-test1`. AST, generic HID and USB HID are built in;
  BTF, IOMMUFD and the existing AER build option are retained.
- The installed NVIDIA 615.78.08 module is byte-identical to the pre-existing
  driver-source build artifact. An isolated copy of that source rebuilt the
  four installed GPU modules for the new kernel. GDS nvidia-fs 2.31.5 also
  rebuilt successfully against the new kernel and NVIDIA symbols.
- Select `NV_KERNEL_MODULES="nvidia nvidia-uvm nvidia-modeset nvidia-drm"` and
  `NV_EXCLUDE_KERNEL_MODULES=nvidia-peermem` when rebuilding this host's GPU
  stack. The first attempt used an incorrect exclusion variable, selected
  peermem, and imported incompatible OFED symbols. The corrected build passed.
- Target-specific build logs and staging scripts are under
  `/home/ubuntu/rebar-usb-bmc-20261105/` on the test host. The installation
  script preserves the old default kernel and does not schedule a reboot.
- BMC address discovered through local IPMI: `10.100.1.107`. Remote viewer and
  input delivery still require confirmation. Installing ipmitool retried a
  pending configuration of the old kernel package, exposing existing OFED DKMS
  failures. That package remains half-configured; no DKMS failure was ignored
  as a successful build. Its regenerated initramfs contains NVMe and mlx5.

## Hardware test boot, 2026-11-05

The user authorized installation and a one-time reboot. The host returned on
`7.0.0-rebar-usb-bmc-test1`. The normal GRUB default remains
`7.0.0-rebar-iommufd-btf`; the one-time entry was consumed. The previous image
and initramfs matched their saved checksums after installation.

Verified:

- All three previously disabled USB controllers bind to `xhci_hcd` and expose
  six USB root hubs. The AMD controllers have assigned 1 MiB BAR0s at
  0xdc900000 and 0x9c900000; the ASMedia controller has an assigned 32 KiB BAR0
  at 0xb4900000. The previous xHCI probe failures are gone.
- AST2500 remains the primary `astdrmfb` framebuffer at 1024x768 with an active
  tty1 login service. This confirms the host-side video path.
- All eight RTX 5090s pass BAR0/BAR1, alignment, non-overlap, and ancestor-window
  validation with 32 GiB BAR1s. All eight pass a 1 MiB CUDA device-memory fill
  and byte-verified host readback.
- A manual 16 MiB GDS smoke test on GPU 0 passes the existing integrity
  verifier, with NVFS activity and zero POSIX reads. This is a single-GPU
  regression check, not a throughput benchmark or all-GPU GDS certification.
  The boot-time GDS verifier was skipped because its pending marker was absent.
- The new modules are installed with root ownership. NVMe, mlx5, Switchtec,
  NVIDIA and nvidia-fs remain available.

Remaining:

- Only USB root hubs enumerate so far; no BMC virtual keyboard or mouse has
  appeared in `/proc/bus/input/devices`. A remote-console session and actual
  input-event test are still needed. Do not describe keyboard/mouse as fixed
  based only on host-controller initialization.
- Browser integration was unavailable, but native Safari control reached the
  BMC address. Safari stopped at its insecure-connection warning, which requires
  the user's direct action before the agent may continue. Redfish had rejected
  the supplied login; no BMC account or access policy was changed.
- `openibd.service` and `systemd-networkd-wait-online.service` are failed after
  this boot. In-tree mlx5 and management SSH work. Boot timing includes about
  1m42s firmware and 2m29s userspace; SSH did not listen until 14:52:15 UTC.
  These services need separate diagnosis; their pre-test failure state was not
  recorded, so they are not classified as either new or pre-existing failures.
- The explicit legacy AMD SATA BAR-clearing target remains and AHCI still
  fails to probe, as expected from that unchanged policy. This host boots NVMe.
- `pci=noaer` remains unchanged, per the USB/BMC-first scope.

Post-boot diagnostic files are retained under
`/home/ubuntu/rebar-usb-bmc-20261105/postboot-logs/`; GDS logs are in
`gds-smoke/verified.ZxFqBm/` beneath that task directory. The initial raw
diagnostics were in `/tmp` and did not survive reboot; the baseline findings
above were recorded before reboot.

## Driver reinstall validation, 2026-11-05

The user reinstalled NVIDIA 615.78.08 userspace with `--no-kernel-modules`,
then ran `install.sh` from `/home/ubuntu/open-gpu-kernel-modules`
(`615.78.08-gds`, commit eb91be68). The installed libcuda SHA256 matches
the installer's patched-library record. Its modules have the same source
versions and kernel vermagic as the earlier staged modules.

The duplicate `updates/console-test` modules were moved out of the module
search path into `module-override-backup/` in the remote task directory.
After depmod and regenerating this test kernel's initramfs, the idle host's
NVIDIA modules were unloaded and the user's installer modules loaded without
a reboot. All eight GPUs remained visible. Validation then passed:

- 1 MiB CUDA VRAM fill and byte-verified readback on every GPU.
- All 56 directed CUDA peer-access capability queries. These queries are not
  peer-transfer correctness or bandwidth tests.
- Three 16 MiB GDS reads on each GPU, with byte verification, NVFS activity,
  and zero POSIX fallback reads. The verifier set its success marker at
  2026-11-05T15:09:58Z. These are integrity checks, not sustained benchmarks.

Remote log: `driver-reinstall-gds.log` under the task directory. Detailed
per-GPU records are under `/var/lib/geforce-gds-prototype/results/`.
No Xid, API mismatch, or unknown-symbol messages were found in the boot log
at the time of these checks. The normal GRUB fallback remains unchanged.

Status: kernel boot, USB controller restoration, CUDA and all-GPU GDS smoke
checks passed. Remote BMC video and keyboard/mouse interaction remain unverified.
