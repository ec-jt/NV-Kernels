# PCI BAR1 Porting Plan: Linux 6.9 → NV-Kernel 7.0 BOS

**Target:** `/home/ubuntu/NV-Kernels` branch `24.04_linux-nvidia-bos-7.0-next-port`
**Kernel:** 7.0.0 (`Ubuntu-nvidia-bos-7.0.0-2008.8`)
**Source:** `/home/ubuntu/linux-6.9.0/` — actual working tree (NOT the stale README)

---

## ⚠️ README vs Actual Code Discrepancies Found

| Aspect | README says | Actual 6.9 code |
|--------|------------|-----------------|
| LAB NP matching | Hardcoded BDFs (0x84:00.0 etc.) | **Vendor/device/class match table** via `pci_match_id()` |
| Bridge detection | `bridge_is_c0_01_1()` — hardcoded BDFs | **`lab_bridge_target()`** — DMI Gigabyte + Switchtec subtree scan |
| NP floor | 228 MiB | **96 MiB** minimum, not 228 |
| AMD GPP hotplug | Active (DECLARE_PCI_FIXUP_HEADER) | **Commented out** (lines 393-423) |
| NVIDIA devices | 0x2b85 only | **0x2b85 + 0x2684** (5090 + 4090) |

---

## API Changes (6.9 → 7.0)

| API | 6.9 | 7.0 |
|-----|-----|-----|
| `pci_rebar_get_possible_sizes()` | `u32`, `BIT()` | **`u64`, `BIT_ULL()`** |
| `pci_rebar_init(dev)` | Not needed | **Must call explicitly** before rebar functions |
| `pbus_size_mem()` | `(bus, mask, type, type2, type3, min_size, add_size, realloc_head)` — finds b_res | `(bus, b_res, add_size, realloc_head)` — b_res passed in |
| `pci_bus_distribute_available_resources()` | `(bus, add_list, io, mmio, mmio_pref)` | `(bus, add_list, available[2])` |
| `b_res->parent` check | `b_res->parent` | `resource_assigned(b_res)` |
| `b_res->start/end` | Direct assignment | `resource_set_range(b_res, ...)` |
| Window alignment | `window_alignment(bus, b_res->flags)` | `win_align` computed same way |

---

## Phase 1 — quirks.c (compile test only)

### P1.1 — NVIDIA ReBAR Presize (devices 0x2b85 + 0x2684)

Based on actual 6.9 lines 351-387. Insert anywhere in quirks.c.

```c
/*
 * Pre-size ReBAR for NVIDIA GB202 (RTX 5090) and AD102 (RTX 4090)
 * so bridge sizing sees the full framebuffer aperture (BAR1).
 *
 * 7.0: pci_rebar_init() must be called explicitly before rebar functions.
 *      sizes is now u64 — use BIT_ULL().
 */
static void quirk_presize_rebar_nvidia_gb202(struct pci_dev *dev)
{
    int bar = 1, idx, max = 15;
    u64 sizes;

    if (PCI_FUNC(dev->devfn) != 0)
        return;
    if (!pci_is_pcie(dev))
        return;
    if (!pci_find_ext_capability(dev, PCI_EXT_CAP_ID_REBAR))
        return;
    pci_rebar_init(dev);  /* 7.0: populate dev->rebar_cap */

    sizes = pci_rebar_get_possible_sizes(dev, bar);
    if (!sizes)
        return;

    for (idx = min(31, max); idx >= 0; idx--)
        if (sizes & BIT_ULL(idx))
            break;
    if (idx < 0)
        return;

    if (!pci_rebar_set_size(dev, bar, idx))
        pci_info(dev, "Pre-sized ReBAR on BAR%d to index %d\n", bar, idx);
}
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_NVIDIA, 0x2b85, quirk_presize_rebar_nvidia_gb202);
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_NVIDIA, 0x2b85, quirk_presize_rebar_nvidia_gb202);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_NVIDIA, 0x2684, quirk_presize_rebar_nvidia_gb202);
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_NVIDIA, 0x2684, quirk_presize_rebar_nvidia_gb202);
```

**NVIDIA device ID coverage:**
| Device | PCI ID | Status |
|--------|--------|--------|
| RTX 4090 (AD102) | 0x2684 | ✅ in code |
| RTX 6000 Ada | 0x26b1 | Not yet — add if needed |
| RTX 5090 (GB202) | 0x2b85 | ✅ in code |
| RTX PRO 6000 Blackwell | 0x2bb1 | Not yet — add if needed |
| RTX 5080 (GB203) | 0x2b87? | **Need to verify** |
| RTX 5070 Ti (GB205) | 0x2b8x? | **Need to verify** |

> ⚠️ **User action needed**: confirm PCI IDs for RTX 5080/5070 Ti and add DECLARE_PCI_FIXUP lines if needed.

### P1.2 — LAB NP Budget Freer (vendor/device/class matching)

Based on actual 6.9 lines 241-348. Uses `pci_match_id()` table — NOT BDFs.

```c
/*
 * LAB: free 32-bit NP budget ASAP (EARLY + HEADER)
 *
 * Match table built from lspci -nnvv:
 *  - ASM1042A xHCI          [1b21:1142]
 *  - AMD xHCI               [1022:148c]   (class 0c0330)
 *  - Switchtec mgmt (mem)   [11f8:4052]   (class 0580)
 *  - AMD SATA AHCI          [1022:7901]   (class 0106)
 *  - ASPEED BMC VGA         [1a03:2000]
 *
 * Plus fallbacks for "any xHCI" and "any AHCI" by class.
 */
enum lab_dev_tag {
    LAB_DEV_ANY = 0,
    LAB_DEV_BMC_VGA_ASPEED,
    LAB_DEV_USB_ASM1042A,
    LAB_DEV_USB_XHCI_AMD,
    LAB_DEV_SWITCHTEC_MGMT,
    LAB_DEV_SATA_AHCI_AMD,
    LAB_DEV_USB_XHCI_ANY,
};
static const struct pci_device_id lab_match_tbl[] = {
    { PCI_DEVICE(0x1a03,   0x2000), .driver_data = LAB_DEV_BMC_VGA_ASPEED },
    { PCI_DEVICE(PCI_VENDOR_ID_ASMEDIA,  0x1142), .driver_data = LAB_DEV_USB_ASM1042A },
    { PCI_DEVICE(PCI_VENDOR_ID_AMD,      0x148c), .driver_data = LAB_DEV_USB_XHCI_AMD },
    { PCI_DEVICE(PCI_VENDOR_ID_MICROSEMI,0x4052), .driver_data = LAB_DEV_SWITCHTEC_MGMT },
    { PCI_VENDOR_ID_MICROSEMI, PCI_ANY_ID, PCI_ANY_ID, PCI_ANY_ID,
      (PCI_CLASS_MEMORY_OTHER << 8), 0xFFFF00, .driver_data = LAB_DEV_SWITCHTEC_MGMT },
    { PCI_DEVICE(PCI_VENDOR_ID_AMD,      0x7901), .driver_data = LAB_DEV_SATA_AHCI_AMD },
    { PCI_DEVICE_CLASS((PCI_CLASS_STORAGE_SATA_AHCI << 8), ~0),
      .driver_data = LAB_DEV_SATA_AHCI_AMD },
    { PCI_DEVICE_CLASS((PCI_CLASS_SERIAL_USB_XHCI << 8), ~0),
      .driver_data = LAB_DEV_USB_XHCI_ANY },
    { 0, }
};

static bool lab_np_target(struct pci_dev *d)
{
    const struct pci_device_id *id = pci_match_id(lab_match_tbl, d);
    if (id)
        pci_info(d, "LAB: matched NP target (vendor=%04x device=%04x class=%06x tag=%lu)\n",
                 d->vendor, d->device, d->class >> 8, id->driver_data);
    return id != NULL;
}

static void quirk_lab_free_np_early(struct pci_dev *dev)
{
    u16 cmd;

    if (!lab_np_target(dev))
        return;

    pci_read_config_word(dev, PCI_COMMAND, &cmd);
    if (cmd & (PCI_COMMAND_IO | PCI_COMMAND_MEMORY)) {
        pci_info(dev, "LAB: EARLY disable IO/MEM decode for NP relief\n");
        cmd &= ~(PCI_COMMAND_MASTER | PCI_COMMAND_IO | PCI_COMMAND_MEMORY);
        pci_write_config_word(dev, PCI_COMMAND, cmd);
    }

    for (int i = 0; i < PCI_STD_NUM_BARS; i++) {
        u32 bar;
        pci_read_config_dword(dev, PCI_BASE_ADDRESS_0 + 4*i, &bar);
        if (!bar) continue;
        pci_write_config_dword(dev, PCI_BASE_ADDRESS_0 + 4*i, 0);
        if ((bar & PCI_BASE_ADDRESS_SPACE) == PCI_BASE_ADDRESS_SPACE_MEMORY &&
            (bar & PCI_BASE_ADDRESS_MEM_TYPE_MASK) == PCI_BASE_ADDRESS_MEM_TYPE_64) {
            i++;
            pci_write_config_dword(dev, PCI_BASE_ADDRESS_0 + 4*i, 0);
        }
    }
    pci_write_config_dword(dev, PCI_ROM_ADDRESS, 0);
}
DECLARE_PCI_FIXUP_EARLY(PCI_ANY_ID, PCI_ANY_ID, quirk_lab_free_np_early);

static void quirk_lab_free_np_header(struct pci_dev *dev)
{
    if (!lab_np_target(dev))
        return;

    for (int i = 0; i < PCI_STD_NUM_BARS; i++) {
        struct resource *r = &dev->resource[i];
        if (r->flags & (IORESOURCE_MEM | IORESOURCE_IO)) {
            r->start = 0; r->end = 0; r->flags = 0;
        }
    }
}
DECLARE_PCI_FIXUP_HEADER(PCI_ANY_ID, PCI_ANY_ID, quirk_lab_free_np_header);
```

### P1.3 — AMD GPP Hotplug Force

**NOTE: This quirk is COMMENTED OUT in the 6.9 working tree (lines 393-423).**
It is included here for reference — decide whether to activate for 7.0.

```c
/*
 * Force selected AMD GPP Root Ports to be treated as hot-plug bridges.
 * COMMENTED OUT in 6.9 working tree — uncomment if needed for 7.0.
 */
static bool quirk_match_bdf(struct pci_dev *pdev, u8 bus, u8 devfn_slot, u8 func)
{
    return pdev->bus->number == bus &&
           PCI_SLOT(pdev->devfn) == devfn_slot &&
           PCI_FUNC(pdev->devfn) == func;
}

static void quirk_force_hotplug_amd_gpp(struct pci_dev *pdev)
{
    if (!pci_is_pcie(pdev)) return;
    if (pci_pcie_type(pdev) != PCI_EXP_TYPE_ROOT_PORT) return;

    if (quirk_match_bdf(pdev, 0x00, 0x03, 0x01) ||
        quirk_match_bdf(pdev, 0x40, 0x01, 0x01) ||
        quirk_match_bdf(pdev, 0x80, 0x03, 0x01)) {
        pdev->is_hotplug_bridge = true;
        pci_info(pdev, "forcing hot-plug bridge (lab quirk) for sizing\n");
    }
}
#define PCI_DEVICE_ID_AMD_GPP_ROOT_PORT 0x1483
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_AMD, PCI_DEVICE_ID_AMD_GPP_ROOT_PORT,
                         quirk_force_hotplug_amd_gpp);
```

---

## Phase 2 — setup-bus.c Helper Functions

Based on actual 6.9 lines 1009-1053.

**⚠️ Requires `#include <linux/dmi.h>` at top of setup-bus.c** (not currently included).

```c
/* Lab: identify target bridges — DMI Gigabyte + Switchtec subtree scan */
static bool lab_platform_ok(void)
{
#ifdef CONFIG_DMI
    static const struct dmi_system_id lab_dmi[] = {
        { .matches = { DMI_MATCH(DMI_SYS_VENDOR, "GIGABYTE"), } },
        { }
    };
    return dmi_check_system(lab_dmi);
#else
    return false;
#endif
}

static bool bus_has_vendor_device(struct pci_bus *bus, u16 vendor, u16 device)
{
    struct pci_dev *d;
    list_for_each_entry(d, &bus->devices, bus_list) {
        if (d->vendor == vendor && d->device == device)
            return true;
        if (d->subordinate && bus_has_vendor_device(d->subordinate, vendor, device))
            return true;
    }
    return false;
}

/* Target only AMD GPP RPs that front GPU/Switchtec fabric */
static inline bool lab_bridge_target(struct pci_dev *dev)
{
    if (!dev) return false;
    if (!lab_platform_ok()) return false;
    if (!dev->subordinate) return false;
    if (!bus_has_vendor_device(dev->subordinate, PCI_VENDOR_ID_MICROSEMI, 0x4052))
        return false;

    pci_info(dev, "LAB: targeting bridge %04x:%02x:%02x.%d (Switchtec subtree)\n",
             pci_domain_nr(dev->bus), dev->bus->number,
             PCI_SLOT(dev->devfn), PCI_FUNC(dev->devfn));
    return true;
}
```

---

## Phase 3 — setup-bus.c pbus_size_mem() Override

### 6.9 behavior (lines 1092-1193):
1. Releases firmware-assigned NP MEM window on lab bridges
2. Sets NP floor to **96 MiB** minimum

### 7.0 port (insert in the 7.0 `pbus_size_mem` at correct point):

The 7.0 function already receives `b_res` as parameter. Insert after `resource_assigned(b_res)` check (line 1293) and before the child sizing loop (line 1299):

```c
    /* 7.0 pbus_size_mem: line 1289-1294 area */
    if (!b_res)
        return;

    /*
     * Lab override: release firmware-assigned NP MEM window on
     * lab bridges so we can re-size it (mirrors 6.9 order: release
     * BEFORE the resource_assigned check).
     */
    if (bus->self &&
        lab_bridge_target(bus->self) &&
        b_res == &bus->self->resource[PCI_BRIDGE_MEM_WINDOW] &&
        resource_assigned(b_res)) {
        pci_info(bus->self,
                 "releasing firmware-assigned NP MEM window %pR to re-size\n",
                 b_res);
        release_child_resources(b_res);
        if (!release_resource(b_res))
            pci_dbg(bus->self, "released existing window\n");
    }
    if (resource_assigned(b_res))
        return;

    max_order = 0;
    size = 0;

    // ... existing child sizing loop (lines 1299-1340) ...

    // After size0 calculation (line 1346):
    /*
     * Lab override: enforce 96 MiB NP floor on lab bridges.
     */
    if (bus->self &&
        lab_bridge_target(bus->self) &&
        b_res == &bus->self->resource[PCI_BRIDGE_MEM_WINDOW]) {
        resource_size_t floor_np = 96ULL << 20;
        if (size0 < floor_np) size0 = floor_np;
        if (size1 < floor_np) size1 = floor_np;
        /* 6.9 parity: add_align is always >= min_align in 6.9 (line 1177);
           in 7.0 it stays 0 when the size1 block doesn't run (line 1353-1357).
           Force it so the add_size path has correct alignment if floor triggers. */
        add_align = max(add_align, min_align);
        pci_info(bus->self, "NP window floor %llu MiB; sized %llu/%llu MiB\n",
                 (unsigned long long)(floor_np >> 20),
                 (unsigned long long)(size0 >> 20),
                 (unsigned long long)(size1 >> 20));
    }
```

**Note:** The 7.0 `pbus_size_mem` calls `resource_set_range()` not direct start/end assignment. The override must be placed between `calculate_memsize()` (line 1346) and `resource_set_range()` (line 1349).

---

## Phase 4 — setup-bus.c Distribution Guard

### 6.9 behavior (lines 1989-1997):
Uses "grow but never shrink" for lab bridges:

```c
if (!lab_bridge_target(bridge)) {
    adjust_bridge_window(bridge, mmio_res, add_list, resource_size(&mmio));
} else {
    resource_size_t cur = resource_size(mmio_res);
    resource_size_t want = resource_size(&mmio);
    if (want > cur)
        adjust_bridge_window(bridge, mmio_res, add_list, want);
}
```

### 7.0 port (replace the `adjust_bridge_window` call at lines 1940-1942):

Replace:
```c
        adjust_bridge_window(bridge, res, add_list,
                             resource_size(&available[i]));
```

With:
```c
        /*
         * Lab override: grow but never shrink NP MEM on lab bridges.
         */
        if (!lab_bridge_target(bridge) ||
            res != &bridge->resource[PCI_BRIDGE_MEM_WINDOW]) {
            adjust_bridge_window(bridge, res, add_list,
                                 resource_size(&available[i]));
        } else {
            resource_size_t cur = resource_size(res);
            resource_size_t want = resource_size(&available[i]);
            if (want > cur)
                adjust_bridge_window(bridge, res, add_list, want);
        }
```

---

## Phase 5 — Config + Boot Flags

### Kernel config
```bash
scripts/config --enable PCI_QUIRKS
# PCI_RESIZABLE_BAR is unconditional in 7.0 — no config needed
```

### Boot flags (GRUB_CMDLINE_LINUX_DEFAULT)
```
pcie_aspm=off intel_iommu=off amd_iommu=off video=efifb:off
modprobe.blacklist=ast loglevel=7 pcie_ports=native
pci=use_crs,realloc=on,assign-busses,big_root_window,hpmmiosize=0
```

### Build
```bash
make -j$(nproc) bindeb-pkg LOCALVERSION=-rebar
dpkg -i linux-image-7.0.0-rebar_*.deb linux-headers-7.0.0-rebar_*.deb
```

### Validation (after reboot)
```bash
dmesg | grep -c 'Pre-sized ReBAR on BAR1'          # == GPU count
lspci -vv | grep -c 'BAR 1: current size: 32GB'    # == GPU count
dmesg | grep 'LAB: targeting bridge'                # lab_bridge_target fired
dmesg | grep 'NP window floor'                      # floor applied
nvidia-smi --query-gpu=name --format=csv,noheader | wc -l  # == GPU count
```

---

## Phase Testing Order

| Phase | Files | Test | Risk |
|-------|-------|------|------|
| 1 | quirks.c only | `make drivers/pci/quirks.o` | Low |
| 2 | setup-bus.c helpers only | `make drivers/pci/setup-bus.o` | Low |
| 3 | pbus_size_mem override | Full build + boot | Medium |
| 4 | Distribution guard | Full build + boot | Medium |
| 5 | Config + flags | Full build + validation | Sum of all |

---

## Open Questions

1. **NVIDIA device IDs**: Need to confirm 5080/5070 Ti PCI IDs and add if desired
2. **AMD GPP hotplug**: Commented out in 6.9 — enable for 7.0?
3. **NP floor**: 96 MiB in 6.9 — is this sufficient for your 8×5090 config or does it need to be higher?
