/* SPDX-License-Identifier: GPL-2.0 */
/* Host harness for lab-rebar-test.py, not a kernel translation unit. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef uint64_t resource_size_t;

#define MIB (1ULL << 20)
#define GIB (1ULL << 30)
#define max(a, b) ((a) > (b) ? (a) : (b))
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define kzalloc_obj(x) calloc(1, sizeof(x))
#define kfree(x) free(x)
#define panic(...) abort()
#define pci_warn(dev, fmt, name, res) ((void)(name))

struct list_head {
	struct list_head *next, *prev;
};

#define LIST_HEAD(name) struct list_head name = { &(name), &(name) }
#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))
#define list_for_each_entry(pos, head, member) \
	for (struct list_head *node = (head)->next; \
	     node != (head) && ((pos) = container_of(node, __typeof__(*(pos)), member), 1); \
	     node = node->next)
#define list_for_each_entry_safe(pos, tmp, head, member) \
	for (struct list_head *node = (head)->next, *next = node->next; \
	     node != (head) && ((pos) = container_of(node, __typeof__(*(pos)), member), \
			       (tmp) = NULL, (void)(tmp), 1); \
	     node = next, next = node->next)

static void list_add_tail(struct list_head *entry, struct list_head *head)
{
	entry->prev = head->prev;
	entry->next = head;
	head->prev->next = entry;
	head->prev = entry;
}

static void list_del(struct list_head *entry)
{
	entry->prev->next = entry->next;
	entry->next->prev = entry->prev;
}

struct resource {
	resource_size_t start, end, align, extra;
	unsigned long flags;
	bool assigned, optional, disabled, fixed;
	struct resource *window;
};

struct pci_dev {
	struct list_head bus_list;
	struct resource resource;
	bool assignable;
};

struct pci_bus {
	struct list_head devices;
};

struct pci_dev_resource {
	struct list_head list;
	struct resource *res;
	struct pci_dev *dev;
	resource_size_t start, end;
	unsigned long flags;
};

#define pci_dev_for_each_resource(dev, r, i) \
	for ((i) = 0; (i) < 1 && ((r) = &(dev)->resource, 1); (i)++)

static bool pdev_resources_assignable(struct pci_dev *dev)
{
	return dev->assignable;
}

static bool pdev_resource_should_fit(struct pci_dev *dev, struct resource *r)
{
	(void)dev;
	return r->flags && !r->assigned && !r->disabled && !r->fixed;
}

static const char *pci_resource_name(struct pci_dev *dev, int i)
{
	(void)dev;
	(void)i;
	return "test";
}

static resource_size_t pci_resource_alignment(struct pci_dev *dev,
					       struct resource *r)
{
	(void)dev;
	return r->align;
}

static int pci_resource_num(struct pci_dev *dev, struct resource *r)
{
	assert(r == &dev->resource);
	return 0;
}

static struct resource *pbus_select_window(struct pci_bus *bus,
					 struct resource *r)
{
	(void)bus;
	return r->window;
}

static resource_size_t resource_size(struct resource *r)
{
	return r->end - r->start + 1;
}

static resource_size_t get_res_add_size(struct list_head *head,
					      struct resource *r)
{
	(void)head;
	return r->extra;
}

static bool pci_resource_is_optional(struct pci_dev *dev, int i)
{
	(void)i;
	return dev->resource.optional;
}

/* INSERT_KERNEL_FUNCTIONS */

static struct pci_dev child(resource_size_t size, resource_size_t align,
			    struct resource *window)
{
	return (struct pci_dev) {
		.assignable = true,
		.resource = { .end = size - 1, .align = align,
			      .flags = 1, .window = window },
	};
}

static void check(struct pci_dev *children, int count, struct resource *window,
		  bool optional_pass, resource_size_t expected_required,
		  resource_size_t expected_optional)
{
	struct pci_bus bus;
	resource_size_t required = 0, optional = 0, min_align = 0, add_align = 0;
	LIST_HEAD(realloc_head);

	bus.devices.next = bus.devices.prev = &bus.devices;
	for (int i = 0; i < count; i++)
		list_add_tail(&children[i].bus_list, &bus.devices);
	lab_bus_mem_floor(&bus, window, optional_pass ? &realloc_head : NULL,
			  &required, &optional, &min_align, &add_align);
	assert(required == expected_required);
	assert(optional == expected_optional);
}

int main(void)
{
	struct resource np = {0}, pref = {0};
	struct pci_dev c[8];

	/* Same mixed switch, either downstream port ordering: 89, not 129 MiB. */
	c[0] = child(24 * MIB, MIB, &np);
	c[1] = child(65 * MIB, 64 * MIB, &np);
	check(c, 2, &np, false, 89 * MIB, 89 * MIB);
	c[0] = child(65 * MIB, 64 * MIB, &np);
	c[1] = child(24 * MIB, MIB, &np);
	check(c, 2, &np, false, 89 * MIB, 89 * MIB);

	/* Management BAR is beside the upstream bridge, even if enumerated first. */
	c[0] = child(4 * MIB, 4 * MIB, &np);
	c[1] = child(89 * MIB, 64 * MIB, &np);
	check(c, 2, &np, false, 96 * MIB, 96 * MIB);

	/* Preserve the old two-5090 and two-4090 alignment-gap protection. */
	c[0] = c[1] = child(65 * MIB, 64 * MIB, &np);
	check(c, 2, &np, false, 193 * MIB, 193 * MIB);
	c[0] = child(193 * MIB, 64 * MIB, &np);
	c[1] = child(4 * MIB, 4 * MIB, &np);
	check(c, 2, &np, false, 200 * MIB, 200 * MIB);
	c[0] = c[1] = child(24 * MIB, 16 * MIB, &np);
	check(c, 2, &np, false, 56 * MIB, 56 * MIB);

	/* PF BAR1 + BAR3; optional VF space must not become mandatory. */
	c[0] = child(16 * GIB, 16 * GIB, &pref);
	c[1] = child(32 * MIB, 32 * MIB, &pref);
	c[2] = child(256 * MIB, 256 * MIB, &pref);
	c[2].resource.optional = true;
	check(c, 3, &pref, true, 16 * GIB + 32 * MIB,
	      16 * GIB + 288 * MIB);
	c[0] = child(32 * GIB, 32 * GIB, &pref);
	check(c, 3, &pref, false, 32 * GIB + 288 * MIB,
	      32 * GIB + 288 * MIB);

	/* Two large child bridge windows still need their alignment gap. */
	c[0] = c[1] = child(32 * GIB + 32 * MIB, 32 * GIB, &pref);
	check(c, 2, &pref, false, 96 * GIB + 32 * MIB,
	      96 * GIB + 32 * MIB);

	/* Optional growth on a bridge is counted exactly once. */
	c[0] = child(64 * MIB, 64 * MIB, &np);
	c[0].resource.extra = MIB;
	check(c, 1, &np, true, 64 * MIB, 65 * MIB);

	/* An optional-only resource must not enlarge the mandatory window. */
	c[0] = child(256 * MIB, 256 * MIB, &pref);
	c[0].resource.optional = true;
	check(c, 1, &pref, true, 0, 256 * MIB);

	/* Ignore fixed/assigned/disabled/zero-alignment/wrong-window resources. */
	for (int i = 0; i < 8; i++)
		c[i] = child(64 * MIB, 64 * MIB, &np);
	c[1].resource.assigned = true;
	c[2].resource.fixed = true;
	c[3].resource.disabled = true;
	c[4].resource.align = 0;
	c[5].resource.window = &pref;
	c[6].assignable = false;
	c[7].resource.flags = 0;
	check(c, 8, &np, false, 64 * MIB, 64 * MIB);
	puts("PASS: 12 C resource-sorting and alignment-floor cases");
	return 0;
}
