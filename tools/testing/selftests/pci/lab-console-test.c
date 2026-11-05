/* SPDX-License-Identifier: GPL-2.0 */
/* Userspace harness, not a kernel translation unit. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint16_t u16;
typedef uint32_t u32;
#define PCI_VENDOR_ID_AMD 0x1022
#define PCI_VENDOR_ID_ASMEDIA 0x1b21
#define PCI_CLASS_STORAGE_SATA_AHCI 0x010601
#define PCI_CLASS_SERIAL_USB_XHCI 0x0c0330
#define PCI_ANY_ID (~0U)
#define PCI_COMMAND 4
#define PCI_COMMAND_IO 1
#define PCI_COMMAND_MEMORY 2
#define PCI_COMMAND_MASTER 4
#define PCI_BASE_ADDRESS_0 0x10
#define PCI_BASE_ADDRESS_SPACE 1
#define PCI_BASE_ADDRESS_SPACE_MEMORY 0
#define PCI_BASE_ADDRESS_MEM_TYPE_MASK 6
#define PCI_BASE_ADDRESS_MEM_TYPE_64 4
#define PCI_ROM_ADDRESS 0x30
#define PCI_STD_NUM_BARS 6
#define IORESOURCE_IO 0x100
#define IORESOURCE_MEM 0x200
#define PCI_DEVICE(v, d) .vendor = (v), .device = (d)
#define PCI_DEVICE_CLASS(c, m) .vendor = PCI_ANY_ID, .device = PCI_ANY_ID, \
	.class = (c), .class_mask = (m)
#define pci_info(...) ((void)0)

struct resource { uint64_t start, end; unsigned long flags; };
struct pci_dev {
	u32 vendor, device, class, regs[64], writes;
	struct resource resource[6];
};
struct pci_device_id {
	u32 vendor, device, class, class_mask;
	unsigned long driver_data;
};

static const struct pci_device_id *pci_match_id(const struct pci_device_id *id,
					      struct pci_dev *dev)
{
	for (; id->vendor || id->device || id->class_mask; id++)
		if ((id->vendor == PCI_ANY_ID || id->vendor == dev->vendor) &&
		    (id->device == PCI_ANY_ID || id->device == dev->device) &&
		    !((id->class ^ dev->class) & id->class_mask))
			return id;
	return NULL;
}

static void pci_read_config_word(struct pci_dev *dev, int reg, u16 *value)
{
	*value = dev->regs[reg / 4];
}
static void pci_read_config_dword(struct pci_dev *dev, int reg, u32 *value)
{
	*value = dev->regs[reg / 4];
}
static void pci_write_config_dword(struct pci_dev *dev, int reg, u32 value)
{
	dev->regs[reg / 4] = value;
	dev->writes++;
}
static void pci_write_config_word(struct pci_dev *dev, int reg, u16 value)
{
	pci_write_config_dword(dev, reg, value);
}

/* INSERT_KERNEL_FUNCTIONS */

static void check(u32 vendor, u32 device, u32 class, bool preserve)
{
	struct pci_dev dev = { .vendor = vendor, .device = device, .class = class };
	struct pci_dev before;
	int i;

	dev.regs[PCI_COMMAND / 4] = 7;
	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		dev.regs[PCI_BASE_ADDRESS_0 / 4 + i] = 0x100000 + i * 0x10000;
		dev.resource[i] = (struct resource) {
			.start = 0x100000 + i * 0x10000,
			.end = 0x10ffff + i * 0x10000,
			.flags = IORESOURCE_MEM,
		};
	}
	before = dev;
	quirk_lab_free_np_early(&dev);
	quirk_lab_free_np_header(&dev);
	if (preserve) {
		assert(!dev.writes);
		assert(!memcmp(dev.regs, before.regs, sizeof(dev.regs)));
		assert(!memcmp(dev.resource, before.resource, sizeof(dev.resource)));
	} else {
		assert(dev.writes);
		assert(!dev.regs[PCI_COMMAND / 4]);
		for (i = 0; i < PCI_STD_NUM_BARS; i++)
			assert(!dev.resource[i].flags);
	}
}

int main(void)
{
	check(0x1022, 0x148c, 0x0c0330, true);
	check(0x1b21, 0x1142, 0x0c0330, true);
	check(0x8086, 0x1234, 0x0c0330, true);
	check(0x8086, 0x1234, 0x0c0320, true);
	check(0x8086, 0x1234, 0x0c0310, true);
	check(0x8086, 0x1234, 0x0c0300, true);
	check(0x1a03, 0x2000, 0x030000, true);
	check(0x11f8, 0x4052, 0x058000, true);
	check(0x1022, 0x7901, 0x010601, false);
	puts("PASS: USB, AST and Switchtec resources preserved; legacy AMD SATA target unchanged");
	return 0;
}
