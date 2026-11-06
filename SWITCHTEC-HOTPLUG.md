# Experimental Switchtec GPU hotplug

Initial test candidate: `7.0.0-rebar-hotplug-test1`.
Kernel name: `7.0.0-hotplug`. Default: **disabled**.
Guarded package revision: `7.0.0-2+hotplug` (kernel build #2).

This experiment allows Linux pciehp to manage explicitly selected downstream
ports on the GIGABYTE G292-Z20-00 / MZ22-G20-00 with Switchtec 11f8:4052.
Firmware reports Slot+ and Link Active reporting, but HotPlug- and PwrCtrl-.
The quirk sets the software hotplug flags; it does not rewrite the hardware
capability registers or manufacture a slot power controller.

## Scope

The intended operations are logical removal, re-enumeration after a device's
link returns, and the existing PCI slot reset path. Successful removal does
not preserve CUDA allocations, running applications, RDMA registrations, GDS
registrations, or distributed jobs. Drain those users first and recreate them
after recovery. GPU indices may change; identify GPUs by UUID and PCI address.

Physical GPU or auxiliary-power-cable replacement still requires the machine
to be powered off and disconnected from AC. The kernel cannot repair missing
12 V, a damaged connector, a failed endpoint, or a link that cannot train.
The sysfs file named `power` is a logical enable/disable interface on these
ports: it does not remove slot/auxiliary power, and its readback is not proof
that an endpoint has been removed.

Testing started on one GPU-facing port on idle `.107`, then covered all eight.
No whole-switch reset is involved. Native HPC hotplug slots retain their
existing handling.

The guarded revision refuses a requested removal with `EBUSY` while a display
class GPU driver remains bound below an opted-in non-HPC port. It also leaves
such a bound GPU in place when link/presence events arrive, logging a warning
instead of entering driver teardown. An ignored event is not automatically
replayed after driver unload: perform an explicit maintenance remove/re-add.
This does not repair a lost link or make live NVIDIA fault recovery work.
Prevent new driver binds and jobs during maintenance; the guard is not a
userspace maintenance lock. Direct PCI `remove`/`reset` files and whole-switch
resets are outside this hotplug-controller guard.

## Boot option

Example for the first port on the recorded `.107` topology:

```text
lab_switchtec_hotplug=0000:03:00.0
```

The value accepts up to 16 comma-separated, full domain:bus:device.function
addresses. A malformed address, duplicate, oversized list, or repeated option
disables the entire override. Domain, bus, device and function must be hex,
with widths 4:2:2.1. No wildcard or upstream-port matching is allowed. A
matching address is still rejected unless the exact platform, device ID,
downstream PCI bridge type, Slot flag and Link Active reporting all match.

Addresses are machine-specific and may change after firmware/topology changes.
Verify the allowlist against the actual tree before enabling it. Do not copy
`.107` addresses onto another server. The quirk can also match a selected port
on a later rescan; its allowlist storage persists after boot.

Keep the existing `pcie_ports=native` ownership setting on the test host.
No global hotplug bus/MMIO reservations or IOMMU changes are added. Existing
bridge windows must accommodate the reinserted device. A larger replacement
or a device in a slot empty at boot may still need a reboot/resource allocation
change; this experiment does not promise arbitrary expansion without reboot.

Opted-in ports without the hardware Hot-Plug Capable bit use pciehp's polling
worker because the observed Switchtec ports ignore Hot-Plug Interrupt Enable.
Polling mode is selected per controller at initialization; normal HPC ports
retain interrupt handling. The existing `pciehp.pciehp_poll_time` controls the
poll interval (normally two seconds). Polling is not an instantaneous fault
handler and still requires working hardware link-change status reporting.

## Validation sequence

1. Run `lab-hotplug-test.py`, `lab-rebar-test.py` and `lab-console-test.py` in
   `tools/testing/selftests/pci/`. The first compiles the actual parser and
   quirk with synthetic PCI devices to test its fail-closed scope.
2. Build the full kernel in a separate output directory, retain the released
   kernel, build matching NVIDIA and nvidia-fs modules, and inspect initramfs
   and the GRUB entry. Use a one-time boot; keep the released kernel default.
3. Check all GPUs and BAR forwarding, AST/BMC, USB, NVMe, NIC and Switchtec
   management. Confirm only the selected GPU port gained a pciehp service.
4. Map `/sys/bus/pci/slots/*/address` to the selected port's downstream bus.
   Slot names can be disambiguated by Linux and must not be guessed.
5. With workloads stopped and GPU drivers fully unloaded, logically disable
   that slot, verify the GPU and its audio function disappear, and re-enable
   it. Verify the same UUID, usable BARs, CUDA correctness and GDS direct I/O.
   Check neighbouring devices and kernel logs throughout.
6. Separately test a quiesced slot reset and controlled link-change handling.
   Logical remove/add alone does not validate asynchronous surprise-removal
   events, NVIDIA teardown after a real fault, or power-loss recovery.
7. Expand the allowlist only after the single-port tests succeed. Do not
   deploy on the faulty `.122` or change the public release based only on a
   successful build.

Remove the boot option or boot the retained released kernel to disable this
experiment. Preserve logs and stop on a timeout, missing neighbour, resource
allocation error, Xid, or kernel warning rather than repeatedly resetting.

## Validation status

- Local parser/quirk boundary tests: passed.
- Existing ReBAR and USB/BMC scope regression tests: passed.
- Full initial candidate kernel and matching NVIDIA/GDS module builds: passed.
- Initial single-port boot: all eight 5090s and their 32 GiB BARs passed;
  CUDA and all-GPU direct NVFS reads passed with zero POSIX fallback.
- A neighbour CUDA test opened the selected GPU too. Logical removal waited
  in NVIDIA's `nv_pci_remove_helper()` for a nonzero usage count. The neighbour
  completed 506 correctness iterations over 60 seconds, but removal remained
  blocked after process exit and unloading unused UVM/display modules.
  This is a failed live-driver removal test, not a hotplug success.
- Polling kernel: all eight ports passed logical remove/re-add, with peer BAR
  allocations unchanged. Cycles took 1.92–2.16 seconds at a one-second polling
  interval; the default poll interval is two seconds.
- Controlled link disable/enable on `03:00.0`: polling detected both edges,
  removed and re-added the unbound GPU without a manual rescan, and retained
  all eight valid 32 GiB BARs.
- GPU `04:00.0` function-level reset: passed with its driver unbound. Restore
  reset policy with `default`, not a cached list: the earlier `flr bus` list
  included a bus method no longer available once both functions existed.
- NVIDIA reload, all-eight-GPU CUDA correctness and all-eight-GPU direct NVFS
  byte checks: passed after hotplug/reset; zero POSIX fallback.
- After those workloads completed, unloading all NVIDIA modules, cycling a
  slot and reloading the driver also passed. All eight UUIDs returned.
- Guarded revision: build, unchanged exported-symbol table, and Ubuntu 22.04
  headers module-build test passed. It booted with normal NVIDIA autoload and
  all eight original GPU UUIDs. All eight slot-disable requests returned EBUSY
  in under 0.2 ms with NVIDIA bound; a neighbour CUDA context completed 68
  verified iterations over eight seconds. GPU/audio devices and BARs remained.
- On that guarded boot, a full NVIDIA module unload, GPU FLR, slot remove/add,
  and reload passed. The cycle including normal udev autoload took 27.28 s.
  All eight original UUIDs, 32 GiB BARs, CUDA byte checks and direct NVFS reads
  passed afterward; all eight GDS runs reported zero POSIX fallback.
- Native HPC ports retained interrupt threads; the eight opted-in ports used
  polling. AST framebuffer, xHCI controllers, NVMe and in-tree mlx5 bindings
  remained present. No new Xid, BUG or call trace appeared in the final tests.
  Existing legacy I/O-window allocation warnings and failed openibd/network
  wait-online services were also present before this experiment.

Until driver teardown is separately resolved, drain all GPU applications and
unload the NVIDIA modules **before** requesting removal or reset. Merely
checking compute processes on the selected GPU is insufficient: CUDA/P2P/GDS
clients can hold references to other GPUs. Do not bypass NVIDIA's reference
checks or claim automatic surprise-removal recovery from this kernel quirk.

These tests do not establish support for powered physical hot-swap.

## OFED headers metadata

The final headers package includes the matching source file
`drivers/infiniband/core/Makefile`. OFED 25.10 reads it when deciding whether
legacy standalone `ib_sa`/`ib_mad`/`ib_addr` dummy modules are needed. This file
is metadata; installing it does not enable InfiniBand, change the running
RDMA stack, or by itself make OFED compatible with Linux 7.0. The OFED probe
handles a negative grep result and also checks `modinfo`; its missing-file
message alone is not proof that it caused the build to abort.

Verify the shipped file against this source tree and inspect the first actual
compiler/configure error in a complete OFED build. Do not replace working
in-tree mlx5/IB modules merely to silence a missing-metadata warning.

The isolated OFED 25.10 build against the fixed headers produced no missing
Makefile message, but still failed with `mlx5_devm_params_publish` redefinition,
missing `mlx5_devm_device_get`, and missing `devm_lock`/`devm_unlock` APIs.
That is a separate OFED porting issue. No OFED module from this test was
installed; the host retains the working in-tree mlx5/IB stack.

## Headers host-tool compatibility

The tested packages retain the glibc 2.35-compatible builds of `modpost`,
`objtool`, `resolve_btfids`, and `insert-sys-cert` from the earlier headers
repair. Their sources are unchanged by this patch. All headers ELF files
were checked for GLIBC requirements above 2.35; an external module built
successfully inside Ubuntu 22.04 using GCC 12. Kernel/GPU runtime tests use
the Ubuntu 24.04 host. This is not an Ubuntu 26 kernel-boot qualification.

A normal `bindeb-pkg` build on Ubuntu 24.04 does not automatically produce
glibc 2.35-compatible host tools. Build those tools on Ubuntu 22.04 or repeat
the documented compatibility repack and package validation before publishing.

## Built artifacts and deployment status

The final candidate packages are:

```text
linux-headers-7.0.0-hotplug_7.0.0-2+hotplug_amd64.deb
linux-image-7.0.0-hotplug_7.0.0-2+hotplug_amd64.deb
linux-libc-dev_7.0.0-2+hotplug_amd64.deb
```

SHA-256:

```text
15302efdc4835d09f12464db57084b62973c9a131c61ad7b7b13c7f5de3067ee  linux-headers-7.0.0-hotplug_7.0.0-2+hotplug_amd64.deb
1789bd67db2242143f741fdf3c16e193efb75e0ea9fa7400e239bee23ddf2c88  linux-image-7.0.0-hotplug_7.0.0-2+hotplug_amd64.deb
181976744d46da6997208309dcb56933bdcdceec8a8bf914f85de7e58a7ec68c  linux-libc-dev_7.0.0-2+hotplug_amd64.deb
```

Packages, build logs, OFED failure logs and hardware validation logs are retained
on the test host under `/home/ubuntu/rebar-hotplug-test-20261106/`.
The final boot's results are under `validation-guard/`; earlier polling tests
are under `validation-final/`, and the failed live-driver removal evidence is
under `validation-single/`. Matching NVIDIA 615.78.08 and nvidia-fs 2.31.5
modules are installed for this kernel on the test host; they are not embedded
in the generic kernel Debian packages.

Published as the experimental pre-release
[hotplug-7.0.0](https://github.com/ec-jt/NV-Kernels/releases/tag/hotplug-7.0.0),
from source commit `acdfeb600e3b4255a87ad08b06b532dbbf1efbc8`. GitHub's asset
SHA-256 digests match the locally verified packages above. This does not
replace the stable rebar-aer-bmc release. The test host is running it through
a one-time GRUB entry; its persistent default remains `7.0.0-rebar-aer-bmc`.
Production hosts and the faulted `.122` have not been changed by this experiment.

## Follow-up: power control and remaining recovery work

Read-only investigation on the test host found Switchtec PFX Gen4 device
0x4052, hardware REVB, firmware 3.50 B03E. GPU-facing ports report PwrCtrl-
and HotPlug-. The upstream management utility at commit
`99da3bc799b84ff53abc726c2fdea01379b3c687` can read GPIO status, but its topology
command that labels Power Controller/DSP PERST signals belongs to the PAX
fabric extension. No documented PFX GPIO-to-GPU power mapping was established.
No GPIO values, switch firmware, or BMC power controls were changed.

BMC firmware 12.61 exposes 12V_GPU current sensors. Their presence does not
establish a switching circuit, and their zero-amp readings must not be treated
as evidence that these operating GPUs have no power. The G292-Z20 vendor
manual, page 33, requires shutdown and disconnection of all power sources for
GPU installation. Its public cable-routing diagrams do not establish a
per-GPU power-cycle interface.

PCIe link disable, FLR, downstream bus reset, PERST assertion and physical
power isolation are different operations. The tested FLR/logical cycle can
recover a responsive, quiesced endpoint. None proves that the chassis can
remove the GPU's slot supplies and auxiliary 12 V. Even a switch chip with
hotplug control logic needs the board's power-switching circuitry wired and
configured; a software capability override cannot create it.

Next development gates:

1. Obtain the exact GPU riser/power-board schematic or a vendor-supported
   control mapping covering slot 12 V, 3.3 V/auxiliary supplies, GPU auxiliary
   12 V, reset sequencing and power-good feedback. Generic GPIO writes and
   undocumented IPMI commands are not a valid discovery method.
2. Trace NVIDIA's residual usage_count separately in an instrumented test
   driver. Its nv_pci_remove_helper() deliberately waits when an internal GPU
   still has references; removing the wait or zeroing the counter risks
   corrupting state. Test CUDA/UVM, peer mappings, DMA-BUF and NVFS registration
   teardown individually before attempting faulted-driver removal again.
3. Only after both gates pass, test controlled loss/recovery on one idle port,
   verify neighbour DMA isolation and resource forwarding, then test workloads
   with explicit failure/restart semantics. Running CUDA state is not preserved
   across a reset. Do not enable an automatic switch-reset watchdog meanwhile.

Sources:
- [G292-Z20/Z22 vendor manual](https://download.gigabyte.com/FileList/Manual/server_system_manual_G292-Z20_Z22_e_A00.pdf), pages 33 and 47–50.
- [Upstream Switchtec management utility](https://github.com/Microsemi/switchtec-user), cli/fabric.c and cli/main.c.
- [Microchip slot power-controller example](https://www.microchip.com/content/dam/mchp/documents/OTH/ProductDocuments/DataSheets/mic2591b.pdf): external power switching, current limiting and rail supervision are separate from PCIe packet routing. This example is not evidence that this controller is fitted in the G292-Z20.
