/* SPDX-License-Identifier: GPL-2.0 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define __init
#define __setup(a, b)
#define DECLARE_PCI_FIXUP_HEADER(a, b, c)
#define pr_warn(...) ((void)0)
#define pci_warn(...) ((void)0)
typedef uint32_t u32;
typedef uint16_t u16;
#define DMI_SYS_VENDOR 0
#define DMI_PRODUCT_NAME 1
#define DMI_BOARD_NAME 2
#define PCI_CLASS_BRIDGE_PCI 0x0604
#define PCI_BASE_CLASS_DISPLAY 0x03
#define PCI_EXP_SLTCAP_HPC 0x40
#define PCI_EXP_TYPE_DOWNSTREAM 6
#define PCI_EXP_FLAGS 2
#define PCI_EXP_LNKCAP 12
#define PCI_EXP_FLAGS_SLOT 0x100
#define PCI_EXP_LNKCAP_DLLLARC 0x100000
#define PCI_SLOT(n) ((n) >> 3)
#define PCI_FUNC(n) ((n) & 7)
struct pci_dev;
struct pci_bus { unsigned int number, domain; struct pci_dev *devices; };
struct pci_dev {
    unsigned int vendor, device, class, devfn, type;
    bool pcie, is_hotplug_bridge, is_pciehp;
    struct pci_bus *bus;
    struct pci_bus *subordinate;
    struct pci_dev *next;
    void *driver;
};
struct pcie_device { struct pci_dev *port; };
struct controller { u32 slot_cap; struct pcie_device *pcie; };
static int pci_bus_sem;
#define down_read(p) ((void)(p))
#define up_read(p) ((void)(p))
#define READ_ONCE(v) (v)
#define list_for_each_entry(pos, head, member) \
    for ((pos) = *(head); (pos); (pos) = (pos)->next)
static const char *dmi[] = {"GIGABYTE", "G292-Z20-00", "MZ22-G20-00"};
static u16 flags = PCI_EXP_FLAGS_SLOT;
static u32 link_cap = PCI_EXP_LNKCAP_DLLLARC;
static int read_error;
static bool dmi_match(int field, const char *s) { return !strcmp(dmi[field], s); }
static int pci_domain_nr(struct pci_bus *b) { return b->domain; }
static bool pci_is_pcie(struct pci_dev *d) { return d->pcie; }
static unsigned int pci_pcie_type(struct pci_dev *d) { return d->type; }
static int pcie_capability_read_word(struct pci_dev *d, int r, u16 *v)
{ (void)d; (void)r; *v = flags; return read_error; }
static int pcie_capability_read_dword(struct pci_dev *d, int r, u32 *v)
{ (void)d; (void)r; *v = link_cap; return read_error; }
static int hex_to_bin(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* INSERT_KERNEL_FUNCTIONS */
/* INSERT_BOUND_GUARD */
static void setup(const char *s)
{
    char *copy = s ? strdup(s) : NULL;
    assert(lab_switchtec_hotplug_setup(copy) == 1);
    free(copy);
}
static void clear(void)
{ lab_switchtec_hp_count = 0; lab_switchtec_hp_invalid = false; }
static void expect(struct pci_dev d, bool enabled)
{
    d.is_hotplug_bridge = d.is_pciehp = false;
    quirk_lab_switchtec_hotplug(&d);
    assert(d.is_pciehp == enabled && d.is_hotplug_bridge == enabled);
}
int main(void)
{
    struct pci_bus bus = {.number = 3, .domain = 0};
    struct pci_dev good = {.vendor = 0x11f8, .device = 0x4052,
        .class = 0x060400, .devfn = 0, .type = PCI_EXP_TYPE_DOWNSTREAM,
        .pcie = true, .bus = &bus}, bad;
    const char *invalid[] = {NULL, "", "0000:03:00.0,", ",0000:03:00.0",
        "0000:03:00.0,garbage", "0000:03:00.0,0000:03:00.0", "03:00.0",
        "0000:03:20.0", "0000:03:00.8", "0000:0g:00.0", "0000-03:00.0",
        "0000:03:00.0x", "0000: 3:00.0"};
    expect(good, false);
    setup("0000:03:00.0,0000:C2:01.0");
    assert(lab_switchtec_hp_count == 2);
    expect(good, true);
    bus.number = 0xc2; good.devfn = 8; expect(good, true);
    bus.number = 3; good.devfn = 0;
    /* Every independent boundary must reject an otherwise matching port. */
    bad = good; bad.vendor = 0xffff; expect(bad, false);
    bad = good; bad.device = 0x8532; expect(bad, false);
    bad = good; bad.class = 0x058000; expect(bad, false);
    bad = good; bad.type = 5; expect(bad, false);
    bad = good; bad.pcie = false; expect(bad, false);
    bad = good; bad.devfn = 1; expect(bad, false);
    bus.domain = 1; expect(good, false); bus.domain = 0;
    bus.number = 4; expect(good, false); bus.number = 3;
    for (unsigned int i = 0; i < 3; i++) {
        const char *saved = dmi[i]; dmi[i] = "other";
        expect(good, false); dmi[i] = saved;
    }
    flags = 0; expect(good, false); flags = PCI_EXP_FLAGS_SLOT;
    link_cap = 0; expect(good, false); link_cap = PCI_EXP_LNKCAP_DLLLARC;
    read_error = -1; expect(good, false); read_error = 0;
    setup("0000:03:00.0"); expect(good, false); /* repeated option */
    for (unsigned int i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        clear(); setup(invalid[i]);
        assert(lab_switchtec_hp_invalid && !lab_switchtec_hp_count);
        expect(good, false);
        setup("0000:03:00.0"); expect(good, false); /* cannot undo error */
    }
    char many[512] = "";
    for (unsigned int i = 0; i < 17; i++) {
        char entry[24]; snprintf(entry, sizeof(entry), "%s0000:03:%02x.0", i ? "," : "", i);
        strcat(many, entry);
    }
    clear(); setup(many); expect(good, false);
    assert(lab_switchtec_hp_invalid);
    *strrchr(many, ',') = '\0';
    clear(); setup(many); expect(good, true);
    assert(lab_switchtec_hp_count == 16);
    clear(); setup("0000:03:00.0"); expect(good, true);
    struct pci_dev audio = {.class = 0x040300, .driver = &bus};
    struct pci_dev gpu = {.class = 0x030000, .next = &audio};
    struct pci_bus child = {.devices = &gpu};
    struct pci_dev port = {.subordinate = &child};
    struct pcie_device pcie = {.port = &port};
    struct controller ctrl = {.pcie = &pcie};
    assert(!pciehp_non_hpc_gpu_bound(&ctrl)); /* bound audio alone is OK */
    gpu.driver = &bus;
    assert(pciehp_non_hpc_gpu_bound(&ctrl));
    gpu.class = 0x030200; /* 3D controller, not only legacy VGA */
    assert(pciehp_non_hpc_gpu_bound(&ctrl));
    ctrl.slot_cap = PCI_EXP_SLTCAP_HPC;
    assert(!pciehp_non_hpc_gpu_bound(&ctrl)); /* native hotplug unchanged */
    ctrl.slot_cap = 0;
    gpu.driver = NULL;
    audio.class = 0x030000;
    assert(pciehp_non_hpc_gpu_bound(&ctrl)); /* inspect every function */
    audio.driver = NULL;
    assert(!pciehp_non_hpc_gpu_bound(&ctrl));
    puts("PASS: real hotplug parser and quirk fail-closed boundary checks");
    puts("PASS: real non-HPC bound-GPU guard, including native HPC exclusion");
    return 0;
}
