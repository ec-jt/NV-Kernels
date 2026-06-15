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
   - In `pbus_size_mem()`: releases the firmware-assigned NP window so it can be
     re-sized, and enforces a **96 MiB NP floor** on lab bridges.
   - In `pci_bus_distribute_available_resources()`: "grow but never shrink" the
     NP MEM window on lab bridges.

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

## 8. NVIDIA Device ID Coverage (quirk_presize_rebar)

| GPU | PCI ID | Status |
|-----|--------|--------|
| RTX 4090 (AD102) | `2684` | ✅ |
| RTX 5090 (GB202) | `2b85` | ✅ |
| GB203 variant | `2b87` | ✅ |
| GB20x variant | `2b8f` | ✅ |
| RTX 6000 Ada | `26b1` | not added |
| RTX PRO 6000 Blackwell | `2bb1` | not added |

Add `DECLARE_PCI_FIXUP_EARLY` + `DECLARE_PCI_FIXUP_RESUME_EARLY` pairs for any
additional GPU IDs as needed.
