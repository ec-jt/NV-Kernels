# 7.0.0-hotplug-barfix1

Experimental follow-up to 7.0.0-hotplug. Package version: 7.0.0-1+barfix1.

Controlled, driver-detached GPU resets now preserve the PCI topology and
restore the tested 32 GiB BAR1 mappings. Invalid/all-ones ReBAR reads are
checked before using BAR indices and sizes. Intentional reset link changes
are coordinated with PCI hotplug until save/reset/restore completes.

The NVMe Gen3 initramfs helper no longer depends on the kernel release name.
Only `lab_nvme_gen3=1` enables it. Absent, zero or invalid values do nothing;
the last explicit value wins. Existing G292 platform, unbound-controller
and dedicated-parent checks remain. The helper does not change AER settings.

## Validation

- Packaged kernel image is byte-for-byte identical to the boot-tested image.
- Controlled resets of both individual GPU links and their whole switch
  branch passed, followed by actual BAR/window checks on all eight GPUs.
- All eight RTX 5090s passed CUDA memory correctness; the affected pair passed
  verified peer copies in both directions.
- The helper capped four data NVMe links to Gen3 x4 before driver binding;
  the Gen3 x2 boot device remained unchanged. All five drives enumerated,
  and the root filesystem remained writable.
- AER and root-port DPC remained enabled. No storage PCIe errors were logged
  after the helper completed during the short recovery observation.
- Ubuntu 22.04 / glibc 2.35 build. Headers retain the IB core Makefile for
  OFED's source-layout probe. This does not establish full OFED compatibility.
- Existing NVFS DMA hooks are retained. NVIDIA and nvidia-fs kernel modules
  are separate builds, not included in these packages.

## Remaining fault

A spontaneous receiver/fatal error recurred after the successful controlled
reset tests. Root-port DPC contained both GPUs, and automatic recovery failed
where drivers lacked AER recovery callbacks. This release does **not** fix
that spontaneous fault, provide automatic recovery of running GPU jobs, or
make physical powered GPU replacement safe. Further recovery work is separate.

The recovery boot also logged one NVIDIA `0xbadf2100` initialization warning;
the subsequent short CUDA checks passed. Long-term stability is unproven.

## Installation

Keep a working fallback and use a one-time test boot. The release includes
`install-hotplug-barfix1.sh`, which checks hashes, backs up the old helper,
installs the new helper and its setpci hook, installs image/headers, and
regenerates the target initramfs. It requires `wget`, `pciutils`, and
`initramfs-tools`. It does not reboot or choose a GRUB entry.

The `linux-libc-dev` asset is optional and is not installed by the script;
ordinary external GPU-module builds need the matching kernel headers.

Before booting, build/install matching NVIDIA and any required nvidia-fs
modules for `7.0.0-hotplug-barfix1`. Preserve the host's AER settings. Add
`lab_nvme_gen3=1` to the test entry only when the Gen3 helper is wanted.
Installing Debian packages without the helper does not install that helper.
On hosts already using it, changing the source script takes effect only
after regenerating the selected kernel's initramfs.

Verify assets against SHA256SUMS. Ubuntu 26 boot compatibility has not been
tested. This pre-release does not replace the existing hotplug release.

Detailed evidence: [controlled reset report](../plans/bar-restore-139-20261117.md).
