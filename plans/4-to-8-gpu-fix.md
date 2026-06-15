# Root Cause Analysis: 4 GPUs vs 8 GPUs — NV-Kernels 7.0 Port

## Topology

4 Switchtec PFX 52xG4 switches, each with 2x RTX 5090. One GPU per switch works; the second fails.

## The Smoking Gun: NP Window Sizes

### Working 6.9 kernel

| Level | Bridge | NP Window | Pref Window |
|-------|--------|-----------|-------------|
| AMD GPP RP | `00:03.1` | **224 MiB** | 96 GiB |
| Switchtec USP | `03:00.0` | **192 MiB** | 96 GiB |
| Switchtec DSP1 | `04:00.0` | **96 MiB** | 48 GiB |
| Switchtec DSP2 | `04:01.0` | **96 MiB** | 48 GiB |
| GPU1 BAR0 | `05:00.0` | 64 MiB @ cc000000 ✅ | 32 GiB |
| GPU2 BAR0 | `06:00.0` | 64 MiB @ d4000000 ✅ | 32 GiB |

### Broken 7.0 kernel

| Level | Bridge | NP Window | Pref Window |
|-------|--------|-----------|-------------|
| AMD GPP RP | `00:03.1` | **130 MiB** | 98625 MiB |
| Switchtec USP | `02:00.0` | **130 MiB** | 98625 MiB |
| Switchtec DSP1 | `03:00.0` | **65 MiB** | 33089 MiB |
| Switchtec DSP2 | `03:01.0` | **DISABLED** ❌ | 33089 MiB |
| GPU1 BAR0 | `04:00.0` | 64 MiB @ cc000000 ✅ | 32 GiB |
| GPU2 BAR0 | `05:00.0` | `<ignored>` ❌ | 32 GiB |

---

## Root Cause: Inter-Child Alignment Gap Not Accounted For

### The sizing chain failure

The lab override in [`pbus_size_mem()`](drivers/pci/setup-bus.c:1344) releases the AMD GPP root port NP window AND ALL its children via `release_child_resources()`. This forces a complete bottom-up re-sizing:

1. **DSP level** — Each DSP sizes its GPU's BAR0 (64 MiB, 64 MiB-aligned) + audio (16 KiB):
   - `size = max(64M, 64M) + max(16K, 16K) = 65 MiB` with `min_align = 64 MiB`
   - DSP window set to 65 MiB with alignment 64 MiB ✅

2. **USP level** — Two DSP child bridge windows, each 65 MiB with alignment 64 MiB:
   - `size += max(65M, 64M) = 65M` (first child)
   - `size += max(65M, 64M) = 65M` (second child)
   - **Total: 130 MiB** ← THIS IS WRONG

3. **Why 130 MiB is not enough** — When assigning top-down:
   - USP window starts at `cc000000` (64 MiB aligned)
   - DSP1 at `cc000000`: 65 MiB ending at `d00fffff` ✅
   - DSP2 needs 64 MiB alignment → next boundary: `d4000000`
   - DSP2 at `d4000000`: 65 MiB would end at `d80fffff`
   - But USP window ends at `d41fffff` (130 MiB total)
   - **`d4000000 > d41fffff` → DOESN'T FIT → DISABLED** ❌

4. **Actual space needed**: `ALIGN(65M, 64M) + 65M = 128M + 65M = 193 MiB`

### Why 6.9 works

In 6.9, [`calculate_memsize()`](../linux-6.9.0/drivers/pci/setup-bus.c:824) has an `old_size` parameter:
```c
if (size < old_size) size = old_size;   // 6.9 — preserves firmware sizes
```

Firmware assigned 192 MiB to the USP window. The kernel computes 130 MiB but then `max(130, 192) = 192`. Firmware's sizing was correct.

### Why 7.0 breaks

In 7.0, [`calculate_memsize()`](drivers/pci/setup-bus.c:1067) has NO `old_size` parameter:
```c
size = max(size, min_size) + children_add_size;   // 7.0 — no firmware preservation
return ALIGN(size, align);
```

And the lab override calls `release_child_resources(b_res)` which wipes the firmware-assigned windows at ALL levels. Then re-sizing computes 130 MiB — not enough.

### The `ALIGN(size, align) + r_size` trap

The AGENTS.md §2 documents a prior attempt to fix this with:
```c
size = ALIGN(size, align) + r_size;   // boot-breaker — DO NOT USE globally
```
This was reverted because it wildly overestimates for large aligned children (two 32 GiB-aligned pref windows → ~64 GiB instead of ~33 GiB).

**However**, this exact formula is already used successfully in the prefetchable floor code at [line 1461](drivers/pci/setup-bus.c:1461):
```c
pref_floor = ALIGN(pref_floor, child_align) + child_size;
```

The key difference: it's used as a **floor** (post-computation clamp), not as the primary accumulation formula. This is safe.

---

## Fix: Apply Alignment-Aware NP Floor at All Lab Bridge Levels

### Approach

Mirror the existing [pref floor logic](drivers/pci/setup-bus.c:1438) but for NP MEM windows, and apply it at **all bridges in the Switchtec subtree**, not just AMD GPP root ports.

### New helper: `lab_np_floor_target()`

```c
static bool lab_np_floor_target(struct pci_dev *dev)
{
    if (!dev) return false;
    /* AMD GPP root ports fronting Switchtec subtree */
    if (lab_bridge_target(dev)) return true;
    /* Switchtec upstream/downstream ports */
    if (dev->vendor == PCI_VENDOR_ID_MICROSEMI &&
        (pci_pcie_type(dev) == PCI_EXP_TYPE_UPSTREAM ||
         pci_pcie_type(dev) == PCI_EXP_TYPE_DOWNSTREAM))
        return true;
    return false;
}
```

### New NP floor block in `pbus_size_mem()`

After the existing NP floor block at [line 1412](drivers/pci/setup-bus.c:1412), add alignment-aware NP child bridge accumulation:

```c
/*
 * Lab override: alignment-aware NP floor for bridges in Switchtec subtree.
 * Two 65 MiB child windows with 64 MiB alignment need 193 MiB, not 130 MiB.
 * Walk child bridge NP windows and compute: ALIGN(floor, child_align) + child_size
 */
if (bus->self &&
    lab_np_floor_target(bus->self) &&
    b_res == &bus->self->resource[PCI_BRIDGE_MEM_WINDOW]) {
    resource_size_t np_child_floor = 0;
    struct pci_dev *child;

    list_for_each_entry(child, &bus->devices, bus_list) {
        struct resource *cr;
        int j;
        pci_dev_for_each_resource(child, cr, j) {
            resource_size_t child_align, child_size;
            if (!pci_resource_is_bridge_win(j))
                continue;
            if (cr->flags & IORESOURCE_PREFETCH)
                continue;
            if (!(cr->flags & IORESOURCE_MEM))
                continue;
            if (cr->flags & IORESOURCE_DISABLED)
                continue;
            child_align = pci_resource_alignment(child, cr);
            child_size = resource_size(cr);
            if (realloc_head)
                child_size += get_res_add_size(realloc_head, cr);
            np_child_floor = ALIGN(np_child_floor, child_align) + child_size;
        }
    }

    if (np_child_floor > 0) {
        if (size0 < np_child_floor) size0 = np_child_floor;
        if (size1 < np_child_floor) size1 = np_child_floor;
        if (size < np_child_floor) size = np_child_floor;
        add_align = max(add_align, min_align);
        pci_info(bus->self,
                 "NP child-align floor %llu MiB; sized %llu/%llu MiB\n",
                 (unsigned long long)(np_child_floor >> 20),
                 (unsigned long long)(size0 >> 20),
                 (unsigned long long)(size1 >> 20));
    }
}
```

### Also release NP windows on Switchtec ports

The release block at [line 1344](drivers/pci/setup-bus.c:1344) must also trigger on Switchtec ports, since their firmware-assigned windows were already released by `release_child_resources()` from the parent. Change the condition to use `lab_np_floor_target()`:

```c
if (bus->self &&
    lab_np_floor_target(bus->self) &&    // was: lab_bridge_target()
    b_res == &bus->self->resource[PCI_BRIDGE_MEM_WINDOW] &&
    resource_assigned(b_res)) {
```

### Debug tracing to add

Add `pci_info` at these decision points:

1. **`pbus_size_mem` early return** — when `resource_assigned(b_res)` causes early return
2. **`pbus_size_mem` final size** — after all overrides, what `size0`/`size1` are
3. **`pci_bus_distribute_available_resources`** — what sizes are being distributed to child bridges
4. **Resource assignment failures** — when a resource ends up `<ignored>`

---

## Implementation Checklist

- [ ] Add `lab_np_floor_target()` helper to `setup-bus.c`
- [ ] Add alignment-aware NP child bridge floor in `pbus_size_mem()` for NP windows
- [ ] Broaden NP window release condition to include Switchtec upstream/downstream ports
- [ ] Add debug `pci_info` at `pbus_size_mem` early return for `resource_assigned`
- [ ] Add debug `pci_info` at `pbus_size_mem` final computed size
- [ ] Add debug `pci_info` at `pci_bus_distribute_available_resources` bridge window sizing
- [ ] Keep existing 96 MiB static NP floor as safety net
- [ ] Build kernel
- [ ] Boot test — verify 8x `Pre-sized ReBAR on BAR1`
- [ ] Verify `nvidia-smi` shows 8 GPUs
- [ ] Verify no `<ignored>` BARs in `lspci -v`

## Expected Result After Fix

| Level | Bridge | NP Window |
|-------|--------|-----------|
| AMD GPP RP | `00:03.1` | ~193 MiB |
| Switchtec USP | `02:00.0` | ~193 MiB |
| Switchtec DSP1 | `03:00.0` | 65 MiB |
| Switchtec DSP2 | `03:01.0` | 65 MiB ✅ |
| GPU1 BAR0 | `04:00.0` | 64 MiB ✅ |
| GPU2 BAR0 | `05:00.0` | 64 MiB ✅ |

## Safety Notes

- The `ALIGN + size` formula is used as a **post-computation floor**, NOT in the primary accumulation. This avoids the boot-breaker described in AGENTS.md §2.
- The ASPEED BMC VGA entry remains commented out per AGENTS.md §3.
- `CONFIG_DRM_AST=y` must remain for debug console.
- The existing pref floor logic at [line 1438](drivers/pci/setup-bus.c:1438) already uses this same pattern successfully.
