# NV-Kernels PCI lab maintenance guide

Current work: rebar-aer-bmc, based on the validated USB/BMC and AER test commits.
Release kernel: 7.0.0-rebar-aer-bmc.
The opt-in Switchtec hotplug experiment is documented in
[SWITCHTEC-HOTPLUG.md](SWITCHTEC-HOTPLUG.md). It must not be enabled globally
or treated as permission for powered physical GPU replacement. Retain the
released kernel as the default during candidate testing.
Use configs/config-7.0.0-rebar-aer-bmc for this release; see AER-TEST.md for the
AER boot validation and remaining GPU root-port capability limitation.
The optional upstream AER software injector is built as a module; it is not
loaded by default. See AER-INJECTION-TEST.md for the corrected-event test.
See [USB-BMC-CONSOLE.md](USB-BMC-CONSOLE.md) for scope and validation status.

Read [REBAR-MIXED-TOPOLOGY.md](REBAR-MIXED-TOPOLOGY.md) before changing
[`drivers/pci/quirks.c`](drivers/pci/quirks.c) or
[`drivers/pci/setup-bus.c`](drivers/pci/setup-bus.c). It records the current
topology, implementation, build procedure, boot settings, and completed
hardware validation. [PORTING_PLAN.md](PORTING_PLAN.md) is historical design
material, not a patch recipe or a statement of the current implementation.

## Current behavior

- The early NVIDIA ReBAR quirk selects the largest advertised BAR1 size up to
  32 GiB. Verified IDs on the mixed node are 10de:2c05 for RTX 5070 Ti (16 GiB)
  and 10de:2b85 for RTX 5090 (32 GiB). Their audio IDs are 22e9 and 22e8.
- A boot-only prepass releases the prefetchable hierarchy above unassigned
  large NVIDIA BAR1s before bottom-up bridge sizing. It requires the GIGABYTE
  lab platform, enabled PCI reallocation, and 64-bit prefetchable forwarding
  throughout the ancestor chain. It respects the host preserve-config flag.
- Assigned firmware non-prefetchable windows are preserved. Normal retry
  logic can release a window when an actual resource request fails.
- Unassigned lab windows use descending-alignment post-sizing floors, with
  separate required and optional resource accounting. The legacy 24 MiB NP
  minimum remains. The primary upstream size accumulation is unchanged.
- GPU branches are detected by their devices and ancestry, not fixed bus
  addresses or a fixed number of GPUs per switch.
- The NP-relief quirks retain their SATA entries. USB controllers are excluded
  to preserve physical input and BMC virtual keyboard/mouse. The remaining
  quirk is not DMI-gated and must not be broadened casually.

## Resource and recovery rules

1. Do not replace the primary size sum with sequential alignment packing.
   Sibling alignment accounting belongs in a post-sizing floor. Use the
   allocator's descending-alignment ordering, not bus enumeration order.
2. Do not release parents during the bottom-up sizing pass. Release the
   appropriate hierarchy before sizing so previously sized children are not
   invalidated.
3. Do not turn optional VF BARs, disabled ROMs, or bridge growth into required
   space on the final allocation pass.
4. Preserve the ASPEED BMC VGA resources and built-in AST driver. Never add
   1a03:2000 to the NP-relief match table during development.
5. Preserve Switchtec management BAR0. Its 4 MiB non-prefetchable allocation
   provides the mailbox and switch-side reset path. Do not add 11f8:4052
   management endpoints back to the NP-relief match table.
6. Keep fallback kernels installed. Use a unique release name, verify the
   image, modules, initramfs, and GRUB entry, then use a one-time test boot.
7. Changes to either PCI source file require a full build and hardware boot
   test. Host regression tests alone cannot establish early-boot safety.
8. Do not reset GPUs, remove bridges, or write live BAR registers over the
   only management connection as a substitute for a controlled boot test.

## Existing NP-relief targets

| Device | Match | State |
| --- | --- | --- |
| ASPEED BMC VGA | 1a03:2000 | Excluded, preserve console |
| Switchtec management | 11f8:4052, memory-controller class | Excluded, preserve management |
| ASM1042A xHCI | 1b21:1142 | Excluded, preserve USB |
| AMD xHCI | 1022:148c | Excluded, preserve USB |
| AMD SATA AHCI | 1022:7901 | Active |
| Other AHCI controllers | Legacy shifted class entry | Retained unchanged; not a valid generic AHCI match |
| Other USB controllers | No match | Excluded, preserve USB |

Do not deploy without reviewing the SATA entries if the system uses SATA root
storage. The AHCI constant already includes the programming interface; its
legacy extra shift is deliberately not corrected here, as that would disable
additional storage controllers. NVMe is not matched.

## Configuration and boot parameters

Use the tested configuration in
[`configs/config-7.0.0-rebar-mixed-5070ti-5090`](configs/config-7.0.0-rebar-mixed-5070ti-5090).
Preserve BTF, IOMMUFD, and the built-in AST driver. Check the generated release
after changing the local-version setting; cached build configuration can retain
an older name. Pass an empty local-version override to suppress the dirty-tree
suffix when building this named release.

```sh
make -s -C NV-Kernels-rebar-mixed O=../NV-Kernels-rebar-mixed-build \
    LOCALVERSION= kernelrelease
```

The mixed topology requires PCI reallocation. A later disable option overrides
an earlier enable, so inspect the effective boot command line and all GRUB
drop-ins. Do not suppress AER, discard ACPI root windows, or disable the IOMMU
as an incidental part of a ReBAR fix. The test node's existing IOMMU-off setup
is distinct from normal isolated VFIO/IOMMUFD passthrough.

```text
pci=realloc=on
```

NVIDIA modules must match the target kernel. Review third-party DKMS failures
rather than masking them. The tested node uses a separate NVIDIA open-module
installer and in-tree Mellanox network drivers.

## Validation

```sh
python3 tools/testing/selftests/pci/lab-rebar-test.py
python3 tools/testing/selftests/pci/lab-console-test.py
python3 tools/testing/selftests/pci/validate-rebar.py
nvidia-smi --query-gpu=pci.bus_id,name,memory.total --format=csv
nvidia-smi -q -d MEMORY
sudo dmesg | grep -E 'Pre-sized ReBAR|LAB\|REBAR|LAB\|MEM-FLOOR|failed to assign|no space|NVRM: Xid'
```

Require assigned BAR resources, correctly sized and aligned, and complete
forwarding through every ancestor bridge. A ReBAR capability value alone is
not proof of a usable allocation. Verify all twelve GPUs, all four Switchtec
management devices, NVMe, AST, and the management network. Run a small CUDA
memory test before declaring the boot successful. Peer-to-peer and RDMA
workloads need separate validation.

Keep comments and maintenance documents factual and current. Do not use emoji,
decorative separators, or unverified GPU model labels. Preserve upstream
copyright and author information when editing surrounding code.
