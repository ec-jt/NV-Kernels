# Mixed RTX 5070 Ti / RTX 5090 ReBAR allocation

## Scope and diagnosis

Base revision: 18749cca9cb1d48e31f54877ddc0c1d4beae6a66, from the
rebar-iommufd-btf-7.0.0 branch. The patch is maintained on
rebar-mixed-5070ti-5090. The test machine is ubuntu@10.100.8.134, a GIGABYTE
G292-Z20-00 / MZ22-G20-00 with an EPYC 7402P and BIOS R23.

Verified PCI IDs and physical BAR1 capabilities from the running machine:

| GPU | GPU ID | Audio ID | Desired BAR1 | ReBAR index |
| --- | --- | --- | --- | --- |
| RTX 5070 Ti, GB203 | 10de:2c05 | 10de:22e9 | 16 GiB | 14 |
| RTX 5090, GB202 | 10de:2b85 | 10de:22e8 | 32 GiB | 15 |

The older guide guessed 2b89 for the 5070 Ti. That does not match this machine.
The existing registrations for other IDs remain unchanged; their marketing
names should not be inferred from those old guesses.

| Root bus | Direct GPUs | Switchtec GPUs | GPU BAR1 total |
| --- | --- | --- | --- |
| 0000:00 | 5070 Ti at 01, 02, 03, 04 | 5090 at 08, NIC at 07 | 96 GiB |
| 0000:40 | 5070 Ti at 45, 46, 47, 48 | 5090 at 43, NIC at 44 | 96 GiB |
| 0000:80 | none | 5090 at 88, NIC at 89 | 32 GiB |
| 0000:c0 | none | 5090 at c4, NIC at c3 | 32 GiB |

All GPU addresses above are function 00.0 within the indicated bus. These
addresses are observations, not matching rules in the patch. Total GPU BAR1
space is 256 GiB, the same as eight 32 GiB 5090s. Bridge windows additionally
need BAR3, alignment padding, and any admitted VF resources and NIC BARs.
Each root advertises approximately 1.75 TiB of high MMIO space.

Three distinct problems were observed:

1. The 5070 Ti ID was absent from the early ReBAR quirk, leaving all eight at
   256 MiB despite supporting 16 GiB.
2. All four 5090s had their ReBAR control programmed to 32 GiB, but the firmware
   prefetchable windows remained about 577 MiB at the GPU leaf. The enlarged
   BAR ranges could not be claimed or assigned. The command line contained an
   earlier reallocation enable and a later disable; the latter wins.
3. The old NP override discarded otherwise-working firmware allocations.
   Its bus-order packing counted the NIC's 24 MiB floor before the GPU's
   65 MiB, 64 MiB-aligned window. This requested a 129 MiB upstream window,
   then 136 MiB including management BAR0 at the root port. Root c0 has only
   128 MiB of below-4-GiB space in total, making the request impossible.

The live baseline validator reported 12 failures: eight small BAR1s, four
missing BAR1s, and additionally missing BAR0 on c4:00.0. NVIDIA listed only the
eight 5070 Ti cards. USB controller probe failures are a separate consequence
of the existing NP-relief quirk.

## Existing mechanisms and what changed

These are cooperating mechanisms, not a set of pre-existing runtime modes.

### Early endpoint ReBAR sizing

The existing early and resume-early quirk in
[`drivers/pci/quirks.c`](drivers/pci/quirks.c) initializes ReBAR discovery and
chooses the largest advertised BAR1 size up to index 15 (32 GiB). This patch
adds the verified 2c05 registration. The existing supported-size scan chooses
index 14 on the 5070 Ti and retains index 15 on the 5090. It does not force an
unsupported 32 GiB size onto a 16 GiB device.

This is the existing globally registered device quirk, not DMI-gated. The
capability itself must be present. Audio and VF functions are not targets.
The pre-existing resume hook behavior is unchanged by this patch.

### Prefetchable bridge allocation

The new boot-only prepass in
[`drivers/pci/setup-bus.c`](drivers/pci/setup-bus.c) identifies NVIDIA display
devices with ReBAR and an unassigned, 64-bit prefetchable BAR1 larger than
256 MiB. On the existing GIGABYTE lab platform, with reallocation enabled and
without a host preserve-configuration restriction, it follows the actual
ancestor chain to the top bridge and releases that prefetchable hierarchy.
All ancestors must support a 64-bit prefetchable window.

The release runs before bottom-up sizing. Releasing a parent during sizing
would invalidate previously sized children. Sibling NIC prefetchable BARs
sharing the released hierarchy are also reassigned; their required space is
included by the normal allocator. Non-prefetchable and I/O windows are not
released by this prepass. Other root-port branches, including the NVMe and
BMC branches, are left alone.

The prepass is not a runtime resize or hotplug operation. Explicitly disabling
reallocation is respected and produces a diagnostic for unassigned large
NVIDIA BAR1s. Remove the disabling option for this topology.

### Non-prefetchable allocation and alignment floors

Assigned firmware NP windows are now preserved. On this machine that keeps
the existing 72 MiB Switchtec root-port windows and the working endpoint BAR0,
audio, and management allocations. Normal reallocation retry logic can still
release a window if an actual resource request fails.

For unassigned windows, the legacy 24 MiB NP minimum remains. The NP and
prefetchable alignment floors now use the allocator's descending-alignment
resource sort rather than enumeration order. The upstream primary resource
sum is unchanged. Required and optional requests are packed separately so
disabled ROMs, VF BARs, and optional bridge growth do not become mandatory on
the final allocation pass.

For the mixed switch, the fallback calculation is 65 + 24 = 89 MiB at the
upstream port, then 96 MiB at the root port including aligned management
BAR0, independent of whether the NIC or GPU is enumerated first. The old
two-5090 case retains its 193 MiB child floor and 200 MiB management-inclusive
floor. These are fallback sizing examples, not a requirement to enlarge
already valid firmware windows.

The existing grow-only NP rule during optional-resource distribution remains.
Bridge matching includes direct 5090/5070 Ti branches as well as Switchtec.
The Switchtec floor predicate now consistently requires the GIGABYTE platform
and the specific 4052 switch, rather than matching every Microsemi switch on
every platform.

### Legacy NP relief

The existing early/header NP-relief table in
[`drivers/pci/quirks.c`](drivers/pci/quirks.c) is unchanged. It disables decode,
clears hardware BARs, and clears kernel resources for selected USB and SATA
controllers. Its class wildcards are aggressive and not DMI-gated.

ASPEED VGA and Switchtec management are excluded from that table and must stay
excluded. The BMC provides recovery visibility, and the management BAR provides
the switch-side reset path. NVMe is not matched. Do not deploy this kernel with
a SATA root disk or assume USB will work without separately reviewing those
legacy quirks. ReBAR allocation is not a reason to expand that disable table.

## Boot settings

Use exactly one effective PCI reallocation setting:

```text
pci=realloc=on
```

Remove every conflicting reallocation-disable token, including tokens appended
by GRUB configuration fragments. Inspect both GRUB command-line variables and
drop-ins rather than replacing unrelated root, storage, console, or networking
options. Verify the effective command line after boot.

```sh
grep -R -nE 'GRUB_CMDLINE|pci=.*realloc' /etc/default/grub /etc/default/grub.d
cat /proc/cmdline
```

Existing ASPM and port-power-management disable settings can remain. ReBAR does
not require disabling the IOMMU, suppressing AER, ignoring ACPI root windows,
or setting giant global hotplug reservations. Avoid introducing those changes
as part of this fix. The current IOMMU-off setup does not provide the normal
isolated VFIO/IOMMUFD passthrough path; that is a separate configuration issue.

The 5070 Ti links at 46:00.0 and 48:00.0 trained at PCIe 4.0 x2, while the other
5070 Ti links trained at x4. ReBAR sizing will not fix link-width limitations.

## Build and recovery

The server source checkout and separate build directory are under the Ubuntu
account. Start from the running configuration and keep BTF, IOMMUFD, and the
built-in AST driver enabled. Use a unique release name, never the running
kernel's release name.

```sh
mkdir -p NV-Kernels-rebar-mixed-build
cp /boot/config-$(uname -r) NV-Kernels-rebar-mixed-build/.config
NV-Kernels-rebar-mixed/scripts/config \
    --file NV-Kernels-rebar-mixed-build/.config \
    --set-str LOCALVERSION '-rebar-mixed-5070ti-5090' \
    --disable LOCALVERSION_AUTO
make -C NV-Kernels-rebar-mixed O=../NV-Kernels-rebar-mixed-build \
    LOCALVERSION= olddefconfig
make -s -C NV-Kernels-rebar-mixed O=../NV-Kernels-rebar-mixed-build \
    LOCALVERSION= kernelrelease
make -C NV-Kernels-rebar-mixed O=../NV-Kernels-rebar-mixed-build \
    LOCALVERSION= -j16
```

Installation and reboot require a maintenance window and working BMC access.
Keep both the current custom kernel and the distribution fallback installed.
Check the new release, kernel image, matching modules, initramfs, and GRUB entry
before selecting a one-time test boot. Also build and verify the NVIDIA driver
for the new release. Existing modules from the old release cannot simply be
reused. Check Mellanox OFED compatibility or explicitly plan to use the in-tree
network drivers before risking the SSH path.

If the test boot fails, select the previous kernel through the BMC. If GPUs
are wedged after a failed enumeration/reset, a controlled cold power cycle may
be necessary. Do not attempt live BAR-register writes, GPU FLR, or bridge
removal over the only management connection.

## Validation

Host regression test, runnable on macOS or Linux:

```sh
python3 tools/testing/selftests/pci/lab-rebar-test.py
```

This compiles the actual patched C sorting and floor helpers in a synthetic
resource harness. It covers both mixed-switch orders, management BAR padding,
old two-GPU layouts, 16/32 GiB apertures, optional resources, and excluded
resources. Source assertions also check the 5070 Ti registration, preservation
of assigned NP windows, and ordering of the boot prepass. It is not a PCI
hardware emulator or a substitute for the required full build and boot test.

Read-only post-boot validation:

```sh
python3 tools/testing/selftests/pci/validate-rebar.py
sudo lspci -Dvv -d 10de:2c05
sudo lspci -Dvv -d 10de:2b85
sudo dmesg | grep -E 'Pre-sized ReBAR|LAB\|REBAR|LAB\|MEM-FLOOR|failed to assign|no space'
nvidia-smi --query-gpu=pci.bus_id,name,memory.total --format=csv
nvidia-smi -q -d MEMORY
```

Success requires eight assigned 16 GiB BAR1 resources and four assigned
32 GiB BAR1 resources, each correctly aligned, non-overlapping, and covered by
every ancestor bridge's prefetchable window. All GPUs also need assigned
64 MiB BAR0s with complete NP forwarding. A 32 GiB ReBAR capability value alone
is not success. The validator checks resource allocation and forwarding, not
driver initialization or GPU DMA.

Additionally verify all twelve GPUs in NVIDIA, all four Switchtec management
devices bound with their 4 MiB BAR0s, working Mellanox interfaces, NVMe access,
and the AST console. Check a small workload before declaring the topology
validated. Earlier transient claim failures during enumeration are not decisive;
the final assigned resources and driver initialization are what matter.

## Completed hardware validation, 2026-09-28

The full kernel and modules built successfully on the test server using the
configuration in
[`configs/config-7.0.0-rebar-mixed-5070ti-5090`](configs/config-7.0.0-rebar-mixed-5070ti-5090).
The server booted the new kernel without BMC intervention. The existing
NVIDIA 610.43.02 open-module installer was run against the new kernel, followed
by an initramfs refresh for that release only.

Verified results:

- 12/12 GPUs passed BAR0, BAR1, alignment, non-overlap, and ancestor-window
  checks, both before and after binding to NVIDIA.
- NVIDIA reports eight 16384 MiB BAR1 apertures and four 32768 MiB apertures.
- All twelve GPUs passed an 8 MiB CUDA device allocation, device-memory fill,
  device-to-host copy, and byte-for-byte read-back check. This is a memory/DMA
  smoke test, not an exhaustive compute, peer-to-peer, or RDMA stress test.
- All four Switchtec management functions are bound to the Switchtec driver,
  including c1:00.1, which had failed before the patch.
- All four Switchtec root-port NP windows remained at their firmware 72 MiB
  size. The prefetchable windows grew to 33196 MiB on roots 00/40 and
  33184 MiB on roots 80/c0, including GPU, NIC, and VF space.
- The management Ethernet interface remained reachable over SSH. AST is bound
  and the NVMe root filesystem is mounted read/write. Idle/unconfigured NIC
  ports were not used for a traffic test.
- No resource-assignment failures, no-space errors, or NVIDIA Xid errors were
  found in the validated boot and smoke-test logs.
- All 12 host C regression cases passed on both macOS and the Linux server.
- Kernel patch style checking reported zero errors and zero warnings.

At the user's request, the validated custom GRUB entry is now the persistent
default. Its stable entry ID is shown below. The previous custom kernel and
the distribution kernel remain installed as fallbacks. The GRUB menu is
visible for ten seconds. The custom validated entry carries a single
reallocation-enable option; the older entries retain their original settings.
Do not use the automatically generated new-kernel entry, which still inherits
the old conflicting command line.

```sh
# Validated kernel default (already applied on the test server):
sudo grub-set-default lab-rebar-mixed-test

# Select the previous custom kernel for one boot, if required:
sudo grub-reboot 'gnulinux-advanced-9ad701ac-3c55-4310-90e8-4e50a6ce460f>gnulinux-7.0.0-rebar-iommufd-btf-advanced-9ad701ac-3c55-4310-90e8-4e50a6ce460f'
```

Server-only boot configuration and logs:

```text
/etc/grub.d/41_rebar_mixed_test
/etc/default/grub.d/99-rebar-mixed-test.cfg
/home/ubuntu/NV-Kernels-rebar-mixed-build/boot-backup-20260928-212243/
/home/ubuntu/NV-Kernels-rebar-mixed-build/build.log
/home/ubuntu/NV-Kernels-rebar-mixed-build/nvidia-install.log
/home/ubuntu/NV-Kernels-rebar-mixed-build/test-boot-dmesg.log
/home/ubuntu/NV-Kernels-rebar-mixed-build/cuda-smoke.log
```

## Debian package build

Build image, kernel headers, and userspace headers from the same configuration.
The package profile omits the separate debug-symbol package, not BTF support.
The DWARF development library is a declared packaging dependency.

```sh
sudo apt-get install libdw-dev debhelper fakeroot rsync
DEB_BUILD_PROFILES=pkg.linux-upstream.nokerneldbg \
KDEB_COMPRESS=xz KDEB_PKGVERSION=7.0.0-1 LOCALVERSION= \
make -C NV-Kernels-rebar-mixed O=../NV-Kernels-rebar-mixed-build \
    -j16 bindeb-pkg
```

The installable image and headers do not bundle NVIDIA's separately built
modules. Rebuild NVIDIA against the target kernel as on the test server.
Userspace-header installation is not required merely to boot the new kernel.
Package installation can invoke existing DKMS hooks, so examine their results
rather than masking failures. Never remove a bootable fallback as part of
package installation.
