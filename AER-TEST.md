# AER boot validation on the eight-5090 host

USB/ReBAR baseline commit: `0290350` on `fix/rebar-usb-bmc-console`.
AER investigation branch: `fix/rebar-aer`.
Host: `ubuntu@10.100.8.107`, GIGABYTE MZ22-G20-00, BIOS R23 (2021-09-06).
Kernel: `7.0.0-rebar-usb-bmc-test1`; NVIDIA open driver: 615.78.08.

## Baseline findings

- AER is built in (`CONFIG_PCIEAER=y`, `CONFIG_PCIEPORTBUS=y`). AER software
  injection is not enabled. The boot command line explicitly disables AER
  with `pci=noaer`, despite also specifying `pcie_ports=native`.
- In `drivers/pci/pcie/aer.c`, `pci_no_aer()` disables `pci_aer_available()`.
  This prevents registration of the native AER service. The ReBAR changes
  have not modified this source file.
- The GPU-facing root ports (00:03.1, 40:01.1, 80:03.1, c0:01.1) do not
  expose an AER extended capability in the current PCI configuration chain.
  The Switchtec ports and NVIDIA endpoints do expose AER. Other AMD internal
  root ports expose AER at offset 0x150. This is a separate potential limit
  on native GPU error reporting, not proof that ReBAR breaks AER.
- Firmware reports APEI firmware-first mode. Existing `pcie_ports=native`
  forces native PCIe service ownership; no further ownership overrides or
  capability-register writes are introduced by this test.

## Isolated boot test

The user confirmed that BMC video/keyboard and recovery controls now work,
and requested testing boot with AER. A separate entry was installed in
`/etc/grub.d/42_rebar_aer_test` and checked with `grub-script-check`.

Entry ID: `lab-rebar-usb-bmc-aer-test`.

The entry copies the successfully tested kernel's normal entry and removes
only `pci=noaer`. It preserves `pci=realloc=on`, `pcie_ports=native`, IOMMU
settings and all other boot parameters. No kernel code or PCI registers were
changed for this AER trial. The normal GRUB default remains the older
`7.0.0-rebar-iommufd-btf` fallback.

The host was checked for active CUDA processes, the one-time entry selected
with `grub-reboot`, and a reboot initiated. It booted successfully at
2026-11-05T15:17:57Z; SSH was available by 15:20:31Z. The one-time entry was
consumed and the normal fallback remains unchanged.

Preboot evidence is under `/home/ubuntu/rebar-usb-bmc-20261105/aer-preboot/`.
The PCI capability listing is in `postboot-logs/pre-aer-lspci.txt` under the
same task directory.

## Acceptance checks

1. Confirm the expected kernel boots without `pci=noaer` and SSH returns.
2. Check native AER service bindings, interrupt registration and error logs.
   Report GPU-facing root ports separately from internal AMD root ports.
3. Validate all eight GPU BAR allocations and bridge forwarding, CUDA memory
   operations and GDS integrity. Check USB, AST, NVMe and management networking.
4. Do not claim error recovery is validated from a clean boot alone. No fatal
   error injection, bridge removal, GPU reset or live BAR writes are planned.

## Boot results

- The expected kernel booted with `pci=realloc=on` and `pcie_ports=native`,
  and without `pci=noaer`.
- Native AER registered interrupts and bound to nine AMD internal root ports:
  00:07.1, 00:08.1, 40:07.1, 40:08.1, 80:07.1, 80:08.1, 80:08.2,
  c0:07.1 and c0:08.1 (all in PCI domain 0000).
- All eight RTX 5090 GPUs were visible with driver 615.78.08. The ReBAR
  validator passed all eight devices with 32 GiB BAR1 allocations, no overlap
  or ancestor-forwarding failures.
- All three xHCI controllers retained their six root hubs. The AST framebuffer
  remained `astdrmfb`. The user had confirmed working BMC keyboard and console
  before this reboot; virtual keyboard events were not separately tested here.
- GPU-facing root ports still do not expose an AER extended capability. The
  observed native AER bindings are not the four GPU root paths. Boot success
  does not establish GPU error delivery or fatal-error recovery.
- GDS integrity verification passed on all eight GPUs after this reboot:
  three 16 MiB reads per GPU, byte checks passed and zero POSIX fallback reads.
  No NVIDIA Xid, PCIe Bus Error, Hardware Error, API mismatch or unknown-symbol
  messages were found in the captured post-test kernel log. This is a short
  regression test, not sustained load or error-recovery validation.

Postboot evidence is in `aer-postboot/` under the remote task directory.

