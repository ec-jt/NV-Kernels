# AGENTS.md — NV-Kernels Lab Build & Debug Guide

**Repo:** `/home/ubuntu/NV-Kernels`
**Branch:** `24.04_linux-nvidia-bos-7.0-next-port`
**Kernel:** 7.0.0 (`Ubuntu-nvidia-bos-7.0.0-2008.8`)

This file is the working reference for AI agents (and humans) maintaining the
local PCI ReBAR / NP-budget lab patches. Read this before touching
`drivers/pci/quirks.c` or `drivers/pci/setup-bus.c`.

---

## 1. What These Patches Do

The goal is to get large NVIDIA GPU framebuffer apertures (BAR1, e.g. 32GB on
RTX 5090) sized correctly on a Gigabyte AMD platform that fronts a Switchtec
PCIe fabric. There are two cooperating pieces:

1. **`drivers/pci/quirks.c`**
   - `quirk_presize_rebar_nvidia_gb202()` — pre-sizes ReBAR BAR1 to the largest
     supported size for NVIDIA GB202/AD102/GB203 GPUs *before* bridge sizing
     runs, so the bridge windows account for the full aperture.
   - `quirk_lab_free_np_early()` / `quirk_lab_free_np_header()` — free 32-bit
     non-prefetchable (NP) MMIO budget by zeroing BARs / disabling decode on a
     match table of "non-essential" lab devices, giving the GPUs room.

2. **`drivers/pci/setup-bus.c`**
   - `lab_bridge_target()` — identifies the AMD GPP root ports that front the
     Switchtec subtree (gated by DMI vendor = GIGABYTE).
   - `lab_np_floor_target()` — same as above, plus Switchtec upstream and
     downstream ports.  Used for NP window release and floor enforcement across
     the entire Switchtec subtree, not just the AMD GPP root port.
   - In `pbus_size_mem()`: releases the firmware-assigned NP window on ALL
     `lab_np_floor_target()` bridges so they can be re-sized; enforces a
     **24 MiB NP floor** per DSP; computes an **alignment-aware NP child bridge
     floor** (`ALIGN(floor, child_align) + child_size`) to account for
     inter-child alignment gaps that the standard accumulation misses.
   - In `pci_bus_distribute_available_resources()`: "grow but never shrink" the
     NP MEM window on lab bridges.

3. **NP Alignment Gap + Per-DSP Floor Fix** (2025-06-15, updated 2025-06-17)
   - The 6.9 kernel relied on `calculate_memsize(old_size)` to preserve
     firmware-assigned window sizes (192 MiB).  The 7.0 port lost this
     parameter, so after `release_child_resources()` wiped the hierarchy the
     standard `size += max(r_size, align)` loop underestimated NP windows.
   - **Fix**: post-accumulation NP child bridge floor that walks child bridge
     windows with sequential packing (`ALIGN(floor, align) + size`) as a
     clamp, NOT in the primary accumulation loop (see §2).
   - **Per-DSP floor**: 24 MiB static minimum.  Why 24 MiB?
     * RTX 5090 BAR0 = 64 MiB with 64 MiB alignment → DSP needs ~64 MiB
     * RTX 4090 BAR0 = 16 MiB with 16 MiB alignment → DSP needs ~16 MiB
     * 24 MiB covers both: 4090 gets 24 MiB/DSP, 5090 gets 64 MiB/DSP (raw > floor)
     * With 24 MiB floor, 4090 USP child-align = ALIGN(0,16)+24 + ALIGN(24+16,16)+24
       = 32 + 24 = **56 MiB** → fits in 56 MiB firmware GPP ✅
     * With 24 MiB floor, 5090 USP child-align = ALIGN(0,64)+64 + ALIGN(128,64)+64
       = **192 MiB** → fits in 200+ MiB firmware GPP ✅
   - **PREVIOUS 96 MiB floor was too large**: it inflated 4090 DSPs to 96 MiB,
     pushing the USP child-align to 192 MiB — the root bus couldn't fit it.
   - **Result**: 5090 WS: 8 GPUs (224 MiB GPP).  4090 node: 4 GPUs (56 MiB GPP).
   - **Tracing**: LAB|NP-* prefix traces at key sizing points — grep `LAB|NP` in
     dmesg for live diagnostics.

---

## 2. ⚠️ Known Boot-Breaker (FIXED)

A change in `pbus_size_mem()` altered the size accumulation for child bridge
windows:

```c
/* BROKEN — caused boot failure */
if (pci_resource_is_bridge_win(i))
    size = ALIGN(size, align) + r_size;
else
    size += max(r_size, align);
```

This sequential-packing logic massively overestimated bridge window size
(e.g. two 32GB-aligned windows → ~64GB instead of ~33GB), leading to resource
allocation failure and a broken boot.

**Reverted to the upstream behavior:**

```c
r_size = resource_size(r);
size += max(r_size, align);
```

> RULE: Do not reintroduce the `ALIGN(size, align) + r_size` accumulation
> without a full boot test. If sibling alignment waste needs accounting, do it
> in `calculate_memsize()` / window alignment, not in the per-resource sum.
>
> **Allowed exception**: `ALIGN(floor, align) + size` is safe when used as a
> **post-computation floor** (clamp, not accumulation loop). This pattern is
> used in the NP child-align floor (§1.3) and the prefetchable floor. Both
> compute a floor value from child bridge windows after the primary accumulation
> loop and only clamp `size0`/`size1` up — they never drive the accumulation.

---

## 3. AST / BMC Console for Debugging

To see boot output on the ASPEED BMC VGA, two things must be true:

1. **The AST must NOT be in the NP-free match table.** The ASPEED entry
   `[1a03:2000]` in `lab_match_tbl[]` is **commented out** so its BARs and
   IO/MEM decode are left intact:
   ```c
   /* Keep AST BMC VGA alive for boot debugging — do NOT free its BARs */
   /* { PCI_DEVICE(0x1a03, 0x2000), .driver_data = LAB_DEV_BMC_VGA_ASPEED }, */
   ```
2. **The AST driver is built-in** (`CONFIG_DRM_AST=y`, `CONFIG_DRM=y`) so the
   framebuffer console is available early in boot rather than waiting for a
   module load from initramfs.

> If you re-enable the AST match entry to reclaim its NP budget, you LOSE the
> BMC console. Only do that once the boot issue is resolved.

> NOTE: Boot flags in `PORTING_PLAN.md` previously included
> `modprobe.blacklist=ast video=efifb:off`. For a debug build you want the AST
> ALIVE — drop `modprobe.blacklist=ast` from the kernel cmdline.

---

## 4. Build Procedure

### 4.1 Config tweaks (already applied via `scripts/config`)

```bash
# Optionally start from the running config:
# cp -v /boot/config-$(uname -r) .config

scripts/config --disable SYSTEM_TRUSTED_KEYS
scripts/config --disable SYSTEM_REVOCATION_KEYS
scripts/config --disable DEBUG_INFO_BTF
scripts/config --disable DEBUG_INFO_BTF_MODULES
scripts/config --enable  PCI_QUIRKS
scripts/config --enable  PCI_RESIZABLE_BAR
scripts/config --enable  NVME_CORE
scripts/config --enable  BLK_DEV_NVME
scripts/config --enable  NVME_PCI

# Debug visibility: AST built-in (not a module)
scripts/config --enable  DRM
scripts/config --enable  DRM_AST

yes "" | make olddefconfig
```

### 4.2 Compile

```bash
make -j"$(nproc)"
make modules_prepare
make modules_install install
```

### 4.2.1 ⚠️ LOCALVERSION / kernelrelease gotcha

`make kernelrelease` reads the CACHED `include/config/auto.conf`, not `.config`
directly. After changing `CONFIG_LOCALVERSION` with `scripts/config`, the old
name (e.g. `7.0.0-p2p+`) can persist and you risk OVERWRITING a known-good
kernel. Always regenerate and verify:

```bash
scripts/config --set-str LOCALVERSION "-rebar-debug"
touch .scmversion          # suppress the trailing '+' from a dirty git tree
make -s syncconfig         # refresh include/config/auto.conf from .config
make -s kernelrelease      # MUST show 7.0.0-rebar-debug (verify it is UNIQUE)
ls /boot/vmlinuz-*         # confirm the name does NOT collide with an existing kernel
```

> If `kernelrelease` still shows `+` despite `.scmversion`, override it:
> ```bash
> LOCALVERSION= make -s kernelrelease          # 7.0.0-rebar-debug (no +)
> LOCALVERSION= make bindeb-pkg -j"$(nproc)"  # builds .deb without +
> ```
> The `+` comes from `scripts/setlocalversion` detecting a dirty git tree.
> Setting `LOCALVERSION=` (empty) at make invocation forces a clean suffix.

> Keep the debug build's name distinct (e.g. `-rebar-debug`) so the prior
> `-p2p` / `-rebar` kernels survive as bootable fallbacks.

### 4.3 Install artifacts manually (if not using `install` target)

```bash
KREL=$(make -s kernelrelease)
sudo make modules_install
sudo cp arch/x86/boot/bzImage /boot/vmlinuz-"$KREL"
sudo cp System.map            /boot/System.map-"$KREL"
sudo cp .config               /boot/config-"$KREL"
sudo depmod "$KREL"
```

### 4.4 initramfs — keep it small with `MODULES=dep`

The default `MODULES=most` initramfs pulls in nearly every module and is huge.
Use `dep` so only modules for the current hardware are included:

```bash
# Preferred: set it persistently in initramfs-tools
sudo sed -i 's/^MODULES=.*/MODULES=dep/' /etc/initramfs-tools/initramfs.conf
update-initramfs -c -k "$KREL"

# After it exists, rebuild in place with -u instead of -c
# update-initramfs -u -k "$KREL"
```

#### How `MODULES=dep` actually works

`initramfs-tools` has four `MODULES=` policies that decide WHICH modules get
copied into the initramfs:

| Policy | What it includes | Size | Risk |
|--------|------------------|------|------|
| `most` (default) | Almost all storage/fs/net modules, for ANY hardware | huge | safest for imaging/portability |
| `dep` | Only modules whose devices are CURRENTLY present | small | breaks if you later add new HW the initrd can't drive |
| `netboot` | network + nfs/root-over-net | medium | for diskless |
| `list` | exactly what you list in `/etc/initramfs-tools/modules` | tiny | fully manual |

`dep` works by walking `/sys/devices` for the **running** system, mapping each
present device to its driver via modalias, and pulling in only those modules
(plus their dependency closure) needed to mount the root FS. It runs
`/usr/share/initramfs-tools/hooks/*` which call helper logic that inspects
`/sys/block`, `/sys/bus/pci`, etc.

> ⚠️ **What `dep` can "miss":** because it keys off currently-present hardware,
> the initramfs is portable ONLY to identical hardware. If you move the disk to
> a box with a different storage controller, or hot-add a new HBA, the initrd
> may lack that driver and fail to find root. For this lab box (fixed hardware)
> that is fine. If you change storage topology, rebuild the initrd or
> temporarily switch back to `MODULES=most`.

#### Why our initrd was still ~456M

`MODULES=dep` reduced the module COUNT, but each `.ko` still carried full
`CONFIG_DEBUG_INFO` symbols (we only disabled `DEBUG_INFO_BTF`, not
`DEBUG_INFO` itself). The size is dominated by debug symbols, not module count.
For a debugging kernel this is acceptable. To shrink dramatically:

```bash
scripts/config --disable DEBUG_INFO          # or set DEBUG_INFO_NONE
scripts/config --enable  DEBUG_INFO_DWARF5   # (only if you still want some)
```

> Built-ins do NOT need to be in the initramfs at all. Because
> `CONFIG_DRM_AST=y` and NVMe are built-in, they are already in `vmlinuz` and
> are available before the initrd even unpacks — that's exactly why the BMC
> console shows early boot output. The 456M is modules for the *root-mount*
> stage, not the console path.

#### The `ast_dp501_fw.bin` warning is harmless

```
W: Possible missing firmware /lib/firmware/ast_dp501_fw.bin for built-in driver ast
```

That firmware is only needed for ASPEED **DP501** display-port output chips.
The BMC VGA console path does not require it; the warning can be ignored.

### 4.5 Update GRUB (creates the new boot entry)

```bash
sudo update-grub
```

This generates a NEW menu entry for `vmlinuz-7.0.0-rebar-debug+` while leaving
the existing `-p2p` / `-rebar` / `-nv` entries intact as fallbacks. Verify the
new entry and initrd were found in the `update-grub` output before rebooting.

---

## 5. Validation After Reboot

```bash
dmesg | grep -c 'Pre-sized ReBAR on BAR1'          # == GPU count
lspci -vv | grep -c 'BAR 1: current size: 32GB'    # == GPU count
dmesg | grep 'LAB: targeting bridge'                # lab_bridge_target fired
dmesg | grep 'NP window floor'                      # floor applied
dmesg | grep 'LAB: matched NP target'               # which devices got freed
nvidia-smi --query-gpu=name --format=csv,noheader | wc -l  # == GPU count
```

## 5.1 — Recommended GRUB Cmdline

This kernel has `CONFIG_PCI_REALLOC_ENABLE_AUTO=y` so `pci=realloc` is
automatic.  For GPU passthrough (VFIO) or 8-GPU lab operation, the
following `/etc/default/grub` settings are recommended:

```bash
# Lab 8-GPU / GPU passthrough cmdline
GRUB_CMDLINE_LINUX="pcie_aspm=off iommu=pt iommu.strict=0 pci=noaer,realloc log_buf_len=64M loglevel=7 pcie_port_pm=off nvme_core.default_ps_max_latency_us=0 printk.devkmsg=on video=efifb:off"

# For nodes where the BIOS assigns tight NP MMIO windows (e.g., 4 MiB per GPP),
# pci=realloc is CRITICAL — the kernel needs to do a full reallocation to find
# space for the re-sized 192-224 MiB GPP NP windows.  Without it, the kernel
# correctly sizes the windows but fails at assignment: "can't assign; no space".

# After changing, always:
sudo update-grub
# Then reboot (cold power-cycle preferred for GPU PERST reset)
```

### Key parameters

| Parameter | Purpose |
|-----------|---------|
| `pcie_aspm=off` | Disable ASPM — prevents GPU link power-state hangs |
| `iommu=pt` | AMD IOMMU passthrough mode — best perf for GPU DMA |
| `iommu.strict=0` | Lazy IOMMU invalidation — reduces GPU DMA overhead |
| `vfio-pci.ids=...` | Bind GPU + audio function to vfio-pci at boot (adjust for your GPU IDs) |

> ⚠️ The `vfio-pci.ids` must match YOUR GPU PCI IDs.  Common NVIDIA GPU + audio pairs:
>
> | GPU | GPU ID | Audio ID |
> |-----|--------|----------|
> | RTX 5090 | `10de:2b85` | `10de:22e8` |
> | RTX 5080 | `10de:2b87` | `10de:22e8` |
> | RTX 5070 Ti | `10de:2b89` | `10de:22e8` |
> | RTX 5070 | `10de:2b8b` | `10de:22e8` |
> | RTX A5000 | `10de:2231` | TBD |
> | RTX A6000 | `10de:2230` | TBD |
>
> Verify with `lspci -nn \| grep NVIDIA`.  Remove this parameter if the host
> needs nvidia.ko to drive GPUs (e.g. for `nvidia-smi` / MIG config).  For
> CH passthrough, vfio-pci MUST own the GPU.

### VFIO verification after reboot

```bash
ls /sys/bus/pci/drivers/vfio-pci/ | wc -l   # must match GPU count × 2
sudo dmesg | grep -i 'iommu.*domain\|AMD-Vi\|vfio-pci.*bound'
```

### DKMS workaround for `install-rebar-kernel.sh`

DKMS modules with kernel version allowlists (Mellanox OFED, kernel-mft, knem,
xpmem) will fail to build for this custom kernel name.  The in-tree `mlx5_core`,
`mlx5_ib`, and `ib_core` modules are sufficient for RDMA passthrough.  Add
`|| true` to the `dpkg -i` line in your install script:

```bash
dpkg -i linux-headers-*.deb linux-image-*.deb linux-libc-dev-*.deb || true
```

---

## 6. lab_match_tbl Reference (current state)

| Device | PCI ID | Match | Status |
|--------|--------|-------|--------|
| ASPEED BMC VGA | `1a03:2000` | device | **COMMENTED OUT** (keep console alive) |
| ASM1042A xHCI | `1b21:1142` | device | active |
| AMD xHCI | `1022:148c` | device | active |
| Switchtec mgmt | `11f8:4052` | device + class | active (the NP target) |
| AMD SATA AHCI | `1022:7901` | device | active |
| any AHCI | class `0106` | class wildcard | active (⚠ nukes all SATA BARs) |
| any xHCI | class `0c03` | class wildcard | active (⚠ nukes all xHCI BARs) |

> ⚠ The two class wildcards are aggressive. If a future boot issue points at
> lost storage (SATA root) or lost USB input/serial-debug, suspect these first.
> Root FS on NVMe is unaffected by the AHCI wildcard.

---

## 7. Editing Rules for Agents

- **Never** reintroduce the `ALIGN(size, align) + r_size` accumulation (§2).
- **Never** uncomment the AST `[1a03:2000]` match while a debug build is in
  flight (§3).
- Keep `CONFIG_DRM_AST=y` (built-in) for any debug kernel.
- After any change to `quirks.c` or `setup-bus.c`, do a full build + boot test;
  these files run during early PCI enumeration and a bug = unbootable system.
- Cross-reference `PORTING_PLAN.md` for the 6.9→7.0 API mapping when porting
  additional lab logic.

---

## 8. Key Functions Reference

| Function | File | Purpose |
|----------|------|---------|
| `lab_bridge_target()` | setup-bus.c | AMD GPP RPs with Switchtec below |
| `lab_np_floor_target()` | setup-bus.c | `lab_bridge_target()` + Switchtec USP/DSP |
| `lab_np_target()` | quirks.c | Device match against `lab_match_tbl[]` |
| `quirk_presize_rebar_nvidia_gb202()` | quirks.c | Pre-size GPU BAR1 ReBAR |
| `quirk_lab_free_np_early()` | quirks.c | Zero BARs / disable decode (EARLY) |
| `quirk_lab_free_np_header()` | quirks.c | Nuke kernel-side resource flags (HEADER) |

---

## 9. NVIDIA Device ID Coverage (quirk_presize_rebar)

| GPU | PCI ID | Status |
|-----|--------|--------|
| RTX A5000 (GA102) | `2231` | ✅ added |
| RTX A6000 (GA102) | `2230` | ✅ added |
| RTX 4090 (AD102) | `2684` | ✅ |
| RTX 6000 Ada (AD102) | `26b1` | ✅ added |
| RTX 5090 (GB202) | `2b85` | ✅ |
| RTX 5080 (GB203) | `2b87` | ✅ |
| RTX 5070 Ti (GB205) | `2b89` | ✅ added |
| RTX 5070 (GB205) | `2b8b` | ✅ added |
| GB20x variant | `2b8f` | ✅ |
| RTX PRO 5000 Blackwell | `2bb3` | ✅ added |
| RTX PRO 6000 Blackwell | `2bb1` | ✅ added |

> ⚠️ Verify `2b89`, `2b8b`, `2bb3` with `lspci -nn` when the cards arrive.
> The quirk is safe: it only acts on function 0 with ReBAR capability,
> so wrong IDs are harmless no-ops.

Add `DECLARE_PCI_FIXUP_EARLY` + `DECLARE_PCI_FIXUP_RESUME_EARLY` pairs for any
additional GPU IDs as needed.
