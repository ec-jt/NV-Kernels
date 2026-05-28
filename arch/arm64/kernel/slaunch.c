// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM64 DRTM Secure Launch support
 *
 * Processes DRTM state when the kernel has been launched as a DLME
 * via DRTM dynamic launch. Called early in setup_arch().
 *
 * Copyright (c) 2025, NVIDIA Corporation. All rights reserved.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/memblock.h>
#include <linux/io.h>
#include <linux/efi.h>
#include <linux/of_fdt.h>
#include <linux/libfdt.h>
#include <linux/arm-smccc.h>
#include <linux/overflow.h>
#include <crypto/sha2.h>
#include <asm/drtm.h>
#include <asm/setup.h>

/* FDT magic number (big-endian 0xd00dfeed at offset 0) */
#define FDT_HEADER_MAGIC	0xd00dfeed
#define FDT_MAX_SIZE		(2 * 1024 * 1024)	/* 2 MB */

/*
 * D-CRTM address map — lives in the DLME data region populated by
 * D-CRTM (memblock_reserve'd by slaunch_setup()). Mapped once in
 * slaunch_early_init() via early_memremap_ro and the pointer is
 * kept for all subsequent range-validation calls. No copy into a
 * fixed-size array, no arbitrary region-count cap; the platform's
 * MEMORY_REGION_DESCRIPTOR_TABLE (DEN0113 v1.2 §3.14 Table 11) is
 * consumed in place, sized solely by num_regions in its header.
 */
static struct drtm_mem_region *dcrtm_regions;
static u32 dcrtm_num_regions;

/*
 * DLME data extent saved at slaunch_setup() time so that
 * slaunch_validate_efi() can re-reserve the region after efi_init()'s
 * memblock_remove(0, PHYS_ADDR_MAX) wipes our earlier reservation.
 */
static phys_addr_t sl_dlme_data_pa;
static u64 sl_dlme_data_size;

/*
 * Close TPM locality 2 — the DLME's locality, per DEN0113 v1.2 §4.6.1.
 * Locality 3 is the DCE's; it's closed by the DCE, not the DLME.
 */
static void __init slaunch_tpm_setup(void)
{
	struct arm_smccc_res res;

	arm_smccc_smc(DRTM_SMC_CLOSE_LOCALITY, 2, 0, 0, 0, 0, 0, 0, &res);
	if (res.a0 == (unsigned long)DRTM_NOT_SUPPORTED)
		pr_warn("slaunch: CLOSE_LOCALITY not supported (no TPM backend)\n");
	else if (res.a0 != DRTM_SUCCESS)
		pr_err("slaunch: CLOSE_LOCALITY failed: %ld\n", (long)res.a0);
	else
		pr_info("slaunch: TPM locality 2 closed\n");
}

/*
 * Parse D-CRTM address map from DLME data into static storage.
 *
 * The address map sits inside the DLME data region at offset:
 *   header_size + protected_regions_size
 *
 * It describes the platform's real physical memory layout as seen by
 * the D-CRTM (EL3). This is a trusted input -- the D-CRTM populates it
 * before launching the DLME. We store it for later validation of
 * untrusted data such as the DTB and EFI memory map.
 *
 * Returns true on success, false on failure.
 */
static bool __init slaunch_parse_address_map(phys_addr_t dlme_data_pa,
					     struct dlme_data_header *hdr)
{
	phys_addr_t map_pa;
	u64 map_size;
	u64 hdr_size, prot_size;
	struct drtm_mem_region_hdr *map_hdr;
	struct drtm_mem_region *regions;
	u32 num_regions, i;

	/* Every offset arithmetic below assumes the v1 layout of
	 * dlme_data_header. A future revision would shift fields and
	 * every dcrtm_range_in_normal check and event-log append would
	 * target wrong bytes. Reject anything other than v1.
	 */
	if (le16_to_cpu(hdr->version) != 1)
		panic("slaunch: DLME data header version %u; only v1 supported\n",
		      le16_to_cpu(hdr->version));

	hdr_size = le16_to_cpu(hdr->this_hdr_size);
	prot_size = le64_to_cpu(hdr->protected_regions_size);
	map_size = le64_to_cpu(hdr->address_map_size);

	if (map_size == 0) {
		pr_err("slaunch: D-CRTM address map is empty\n");
		return false;
	}

	if (map_size < sizeof(struct drtm_mem_region_hdr)) {
		pr_err("slaunch: address map too small (%llu bytes)\n",
		       map_size);
		return false;
	}

	map_pa = dlme_data_pa + hdr_size + prot_size;

	/* Map temporarily to validate and log */
	map_hdr = early_memremap(map_pa, (size_t)map_size);
	if (!map_hdr) {
		pr_err("slaunch: failed to map address map at 0x%llx\n",
		       (u64)map_pa);
		return false;
	}

	num_regions = le32_to_cpu(map_hdr->num_regions);
	pr_info("slaunch: D-CRTM address map: revision=%u, %u regions\n",
		le16_to_cpu(map_hdr->revision), num_regions);

	if (sizeof(struct drtm_mem_region_hdr) +
	    (u64)num_regions * sizeof(struct drtm_mem_region) > map_size) {
		pr_err("slaunch: address map regions overflow map size\n");
		early_memunmap(map_hdr, (size_t)map_size);
		return false;
	}

	/* Log regions for debug */
	regions = (struct drtm_mem_region *)((u8 *)map_hdr +
					     sizeof(struct drtm_mem_region_hdr));
	for (i = 0; i < num_regions; i++) {
		u64 addr = le64_to_cpu(regions[i].start_address);
		u64 st = le64_to_cpu(regions[i].size_and_type);
		u64 pages = DRTM_MEM_REGION_PAGE_COUNT(st);
		u32 type = DRTM_MEM_REGION_TYPE(st);

		pr_info("slaunch:   [%u] 0x%012llx - 0x%012llx  %s (%llu pages)\n",
			i, addr, addr + pages * DRTM_PAGE_SIZE,
			type == DRTM_REGION_TYPE_NORMAL        ? "NORMAL" :
			type == DRTM_REGION_TYPE_NORMAL_CACHED ? "NORMAL_CACHED" :
			type == DRTM_REGION_TYPE_DEVICE        ? "DEVICE" :
			type == DRTM_REGION_TYPE_NV            ? "NV" :
			type == DRTM_REGION_TYPE_RSVD          ? "RSVD" :
			"UNKNOWN", pages);
	}

	early_memunmap(map_hdr, (size_t)map_size);

	/*
	 * Map regions and keep the pointer. The physical memory is in
	 * DLME data (D-CRTM populated, memblock_reserve'd later).
	 * All validation functions use this pointer directly.
	 */
	dcrtm_regions = early_memremap_ro(map_pa + sizeof(struct drtm_mem_region_hdr),
					  num_regions * sizeof(struct drtm_mem_region));
	if (!dcrtm_regions) {
		pr_err("slaunch: failed to map address map regions\n");
		return false;
	}
	dcrtm_num_regions = num_regions;
	return true;
}

/*
 * Verify D-CRTM published full-range DMA protection per
 * DEN0113 v1.2 §4.6.2.
 *
 * protected_regions (DLME data sub-region populated by D-CRTM from
 * DRTM_PARAMETERS.mem_prot_table_address) is what SMMU actually
 * protects. The Preamble unconditionally requests a single
 * full-range entry; verify D-CRTM published exactly that. Partial
 * lockdown breaks the measure-then-parse soundness model.
 *
 * Walks protected_regions sub-region of DLME data (DCE-populated from
 * DRTM_PARAMETERS.mem_prot_table) and requires the spec-conformant
 * "single entry, start=0, full-range" encoding. Called from
 * slaunch_setup (after parse_early_param) so panic messages reach
 * earlycon.
 */
static void __init slaunch_assert_full_lockdown(phys_addr_t dlme_data_pa,
						u64 hdr_size, u64 prot_size)
{
	const struct drtm_mem_region_hdr *phdr;
	const struct drtm_mem_region *regs;
	phys_addr_t prot_pa;
	u32 num;
	u64 start, st;

	if (prot_size == 0)
		panic("slaunch: DCE published empty protected_regions; cannot verify SMMU lockdown\n");
	if (prot_size < sizeof(*phdr) + sizeof(*regs))
		panic("slaunch: protected_regions size %llu too small for 1 entry\n",
		      prot_size);

	prot_pa = dlme_data_pa + hdr_size;
	phdr = early_memremap_ro(prot_pa, (size_t)prot_size);
	if (!phdr)
		panic("slaunch: cannot map protected_regions at 0x%llx\n",
		      (u64)prot_pa);

	num = le32_to_cpu(phdr->num_regions);
	regs = (const struct drtm_mem_region *)((const u8 *)phdr + sizeof(*phdr));
	start = le64_to_cpu(regs[0].start_address);
	st = le64_to_cpu(regs[0].size_and_type);

	/* Strict spec match (DEN0113 v1.2 §3.15 R314110 + §4.6.2):
	 * single entry, start=0, size_and_type = DRTM_MEM_PROT_FULL_RANGE
	 * (type NORMAL, cacheability 0, page count = 2^52 - 1). Anything
	 * else is either partial lockdown or a non-conformant DCE
	 * encoding — both fatal for the design.
	 */
	if (num != 1 || start != 0 || st != DRTM_MEM_PROT_FULL_RANGE)
		panic("slaunch: SMMU lockdown not full-range (num=%u start=0x%llx st=0x%llx; want 1/0/0x%llx); design requires full coverage\n",
		      num, start, st, (u64)DRTM_MEM_PROT_FULL_RANGE);

	early_memunmap(phdr, (size_t)prot_size);
	pr_info("slaunch: SMMU lockdown verified: full NS-DRAM coverage\n");
}

/*
 * Check if a physical address range falls entirely within normal
 * usable memory regions of the D-CRTM address map.
 *
 * Per DEN0113 v1.2 §3.14 Table 11 + R314100, normal memory is encoded
 * either as type 0 (plain normal) or type 1 (normal with cacheability
 * attributes). Both qualify; only DEVICE, NV, and RSVD are rejected.
 *
 * Supports ranges that span multiple adjacent/overlapping normal
 * regions. Returns true if every byte of [start, start+size) is
 * covered by one or more normal regions, false otherwise.
 */
static bool __init dcrtm_range_in_normal(u64 start, u64 size)
{
	u64 pos = start;
	u64 end;

	if (!size || !dcrtm_regions)
		return false;
	/* Reject ranges that wrap u64 (attacker-controlled inputs). */
	if (check_add_overflow(start, size, &end))
		return false;

	while (pos < end) {
		bool advanced = false;
		u32 i;

		for (i = 0; i < dcrtm_num_regions; i++) {
			u64 st = le64_to_cpu(dcrtm_regions[i].size_and_type);
			u32 type = DRTM_MEM_REGION_TYPE(st);
			u64 pages = DRTM_MEM_REGION_PAGE_COUNT(st);
			u64 rstart = le64_to_cpu(dcrtm_regions[i].start_address);
			u64 rsize, rend;

			if (type != DRTM_REGION_TYPE_NORMAL &&
			    type != DRTM_REGION_TYPE_NORMAL_CACHED)
				continue;

			/* Skip malformed regions whose size or end wraps. */
			if (check_mul_overflow(pages, (u64)DRTM_PAGE_SIZE, &rsize))
				continue;
			if (check_add_overflow(rstart, rsize, &rend))
				continue;

			if (pos >= rstart && pos < rend) {
				pos = (rend < end) ? rend : end;
				advanced = true;
				break;
			}
		}

		if (!advanced)
			return false;
	}
	return true;
}

/*
 * Check if a physical address range overlaps any non-normal region
 * in the D-CRTM address map (DEVICE, NV, RSVD). Normal memory
 * (type 0) and normal with cacheability attributes (type 1) are
 * skipped per DEN0113 v1.2 §3.14 Table 11.
 *
 * Returns the region type if overlap found, -1 otherwise.
 */
static int __init dcrtm_range_overlaps_non_normal(u64 start, u64 size)
{
	u32 i;
	u64 end;

	if (!dcrtm_regions)
		return -1;
	/* Wrapping range — fail closed: pretend it overlaps an RSVD region. */
	if (check_add_overflow(start, size, &end))
		return DRTM_REGION_TYPE_RSVD;

	for (i = 0; i < dcrtm_num_regions; i++) {
		u64 st = le64_to_cpu(dcrtm_regions[i].size_and_type);
		u32 type = DRTM_MEM_REGION_TYPE(st);
		u64 pages = DRTM_MEM_REGION_PAGE_COUNT(st);
		u64 rstart = le64_to_cpu(dcrtm_regions[i].start_address);
		u64 rsize, rend;

		if (type == DRTM_REGION_TYPE_NORMAL ||
		    type == DRTM_REGION_TYPE_NORMAL_CACHED)
			continue;

		/* Malformed region whose size or end wraps: fail closed
		 * (assume it overlaps).
		 */
		if (check_mul_overflow(pages, (u64)DRTM_PAGE_SIZE, &rsize))
			return type;
		if (check_add_overflow(rstart, rsize, &rend))
			return type;

		if (start < rend && end > rstart)
			return type;
	}
	return -1;
}

static const char * __init dcrtm_type_name(int type)
{
	switch (type) {
	case DRTM_REGION_TYPE_NORMAL:		return "NORMAL";
	case DRTM_REGION_TYPE_NORMAL_CACHED:	return "NORMAL_CACHED";
	case DRTM_REGION_TYPE_DEVICE:		return "DEVICE";
	case DRTM_REGION_TYPE_NV:		return "NV";
	case DRTM_REGION_TYPE_RSVD:		return "RSVD";
	default: return "UNKNOWN";
	}
}

/*
 * Check if two EFI memory regions overlap.
 */
static bool __init efi_regions_overlap(u64 s1, u64 sz1, u64 s2, u64 sz2)
{
	u64 e1, e2;

	/* Either range wraps u64 -> fail closed (report overlap). */
	if (check_add_overflow(s1, sz1, &e1) ||
	    check_add_overflow(s2, sz2, &e2))
		return true;
	return (s1 < e2) && (s2 < e1);
}

/*
 * slaunch_early_init() -- called BEFORE setup_machine_fdt()
 *
 * This is the earliest DRTM validation point. It runs after
 * early_ioremap_init() so early_memremap is available, but before
 * the kernel consumes the DTB.
 *
 * Steps:
 *  1. Map DLME data header, parse D-CRTM address map
 *  2. Validate DTB PA is in a NORMAL region
 *  3. Map DTB, verify FDT magic and size
 *  4. Panic on any failure -- the DTB is untrusted until validated
 */
void __init slaunch_early_init(void)
{
	struct dlme_data_header *hdr;
	phys_addr_t dlme_data_pa;
	phys_addr_t dtb_pa;
	u32 *dtb_hdr;
	u32 fdt_magic, fdt_size;

	if (!sl_dlme_region_pa)
		return;

	pr_info("slaunch: DRTM early init -- validating DTB before consumption\n");
	pr_info("slaunch: DLME region PA: 0x%lx, data offset: 0x%lx\n",
		sl_dlme_region_pa, sl_dlme_data_offset);

	/* Step 1: Map DLME data header and parse address map */
	dlme_data_pa = sl_dlme_region_pa + sl_dlme_data_offset;
	hdr = early_memremap(dlme_data_pa, sizeof(*hdr));
	if (!hdr)
		panic("slaunch: failed to map DLME data header at 0x%llx\n",
		      (u64)dlme_data_pa);

	if (!slaunch_parse_address_map(dlme_data_pa, hdr))
		panic("slaunch: failed to parse D-CRTM address map -- cannot validate untrusted data\n");

	early_memunmap(hdr, sizeof(*hdr));

	/* Step 2: Validate DTB PA is in a NORMAL region */
	dtb_pa = __fdt_pointer;
	if (!dtb_pa)
		panic("slaunch: no DTB physical address available for validation\n");

	pr_info("slaunch: DTB PA: 0x%llx\n", (u64)dtb_pa);

	/*
	 * Check that at least the FDT header (8 bytes for magic + totalsize)
	 * falls in a NORMAL region. The full size check follows once we
	 * read fdt_totalsize.
	 */
	if (!dcrtm_range_in_normal(dtb_pa, sizeof(u32) * 2))
		panic("slaunch: DTB PA 0x%llx is NOT in a D-CRTM NORMAL region\n",
		      (u64)dtb_pa);

	/* Step 3: Map DTB header and verify FDT magic + size */
	dtb_hdr = early_memremap(dtb_pa, sizeof(u32) * 2);
	if (!dtb_hdr)
		panic("slaunch: failed to map DTB header at 0x%llx\n",
		      (u64)dtb_pa);

	fdt_magic = be32_to_cpu(dtb_hdr[0]);
	fdt_size = be32_to_cpu(dtb_hdr[1]);

	early_memunmap(dtb_hdr, sizeof(u32) * 2);

	if (fdt_magic != FDT_HEADER_MAGIC)
		panic("slaunch: DTB at 0x%llx has invalid FDT magic: 0x%08x (expected 0x%08x)\n",
		      (u64)dtb_pa, fdt_magic, FDT_HEADER_MAGIC);

	if (fdt_size == 0 || fdt_size > FDT_MAX_SIZE)
		panic("slaunch: DTB at 0x%llx has invalid size: %u bytes (max %u)\n",
		      (u64)dtb_pa, fdt_size, FDT_MAX_SIZE);

	/* Validate the full DTB range is in NORMAL memory */
	if (!dcrtm_range_in_normal(dtb_pa, fdt_size))
		panic("slaunch: DTB range [0x%llx - 0x%llx] extends outside D-CRTM NORMAL region\n",
		      (u64)dtb_pa, (u64)(dtb_pa + fdt_size));

	pr_info("slaunch: DTB validated: magic=0x%08x, size=%u bytes, in NORMAL region\n",
		fdt_magic, fdt_size);
}

/*
 * Validate raw EFI inputs against D-CRTM map before efi_init()
 * consumes them. efi_init() runs after slaunch_setup() and:
 *   - reads systab->nr_tables/tables and publishes efi.acpi20 etc.
 *   - walks the EFI memory map and drives memblock_add/remove.
 * Doing the validation post-efi_init only catches problems after the
 * damage. The early validators below operate on the raw firmware
 * buffers (NOT efi.memmap, which doesn't exist yet at this stage).
 */
struct sl_efi_info {
	bool present;
	u64  systab_pa;
	u64  mmap_pa;
	u64  mmap_size;
	u32  desc_size;
	u32  desc_ver;
};

/* Forward decls — bodies defined later (or stubbed out by #ifdef
 * gating below).
 */
#ifdef CONFIG_ARM64_SECURE_LAUNCH_FAULT_INJECT
static void __init slaunch_inject_fault(struct sl_efi_info *info);
#else
static inline void slaunch_inject_fault(struct sl_efi_info *info) { }
#endif
#ifdef CONFIG_ARM64_SECURE_LAUNCH_SELFTEST
static void __init slaunch_selftest(void);
#else
static inline void slaunch_selftest(void) { }
#endif

static void __init slaunch_read_chosen_efi(struct sl_efi_info *info)
{
	const void *fdt = initial_boot_params;
	const __be64 *p64;
	const __be32 *p32;
	int node, len;

	memset(info, 0, sizeof(*info));
	if (!fdt)
		return;
	node = fdt_path_offset(fdt, "/chosen");
	if (node < 0)
		return;

#define _GET64(name, field)							\
	do {									\
		p64 = fdt_getprop(fdt, node, name, &len);			\
		if (!p64 || len < (int)sizeof(__be64))				\
			return;							\
		info->field = be64_to_cpu(*p64);				\
	} while (0)
#define _GET32(name, field)							\
	do {									\
		p32 = fdt_getprop(fdt, node, name, &len);			\
		if (!p32 || len < (int)sizeof(__be32))				\
			return;							\
		info->field = be32_to_cpu(*p32);				\
	} while (0)
	_GET64("linux,uefi-system-table",   systab_pa);
	_GET64("linux,uefi-mmap-start",     mmap_pa);
	_GET32("linux,uefi-mmap-size",      mmap_size);
	_GET32("linux,uefi-mmap-desc-size", desc_size);
	_GET32("linux,uefi-mmap-desc-ver",  desc_ver);
#undef _GET64
#undef _GET32
	info->present = true;
}

/* Per-GUID expected minimum size for known EFI ConfigurationTable
 * entries. The validator checks dcrtm_range_in_normal against the
 * actual structure size pointed at by each GUID, instead of a
 * one-size-fits-all 4 KB. Unknown GUIDs fall back to
 * SL_CFGTBL_UNKNOWN_BOUND.
 *
 * Sizes are spec entry-point structure minimums (RSDP v2.0+ = 36 B,
 * SMBIOS3 entry point = 24 B, etc.). The kernel only consumes the
 * entry-point header for these tables; later parsing chases internal
 * pointers which are validated separately (e.g. slaunch_measure_acpi
 * for ACPI tables under RSDP).
 */
struct sl_cfgtbl_size_entry {
	efi_guid_t	guid;
	u32		min_size;
};

static const struct sl_cfgtbl_size_entry sl_cfgtbl_sizes[] __initconst = {
	{ ACPI_20_TABLE_GUID,			36 },	/* RSDP v2.0+ */
	{ SMBIOS3_TABLE_GUID,			24 },	/* SMBIOS3 entry point */
	{ EFI_RT_PROPERTIES_TABLE_GUID,		8  },	/* version + flags */
	{ LINUX_EFI_MEMRESERVE_TABLE_GUID,	32 },	/* header struct */
	{ LINUX_EFI_RANDOM_SEED_TABLE_GUID,	32 },	/* header struct */
};

#define SL_CFGTBL_UNKNOWN_BOUND		EFI_PAGE_SIZE

static u32 __init sl_cfgtbl_min_size(const efi_guid_t *guid)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sl_cfgtbl_sizes); i++) {
		if (efi_guidcmp(*guid, sl_cfgtbl_sizes[i].guid) == 0)
			return sl_cfgtbl_sizes[i].min_size;
	}
	return SL_CFGTBL_UNKNOWN_BOUND;
}

/* Validate System Table + ConfigurationTable pointers against D-CRTM
 * map. Runs pre-efi_init, panics on bad input.
 */
static void __init slaunch_validate_raw_systab(u64 systab_pa)
{
	efi_system_table_t *systab;
	efi_config_table_t *cfgtbl;
	unsigned long tables_pa;
	unsigned long nr_tables;
	size_t tbl_size;
	unsigned long j;

	if (!dcrtm_range_in_normal(systab_pa, sizeof(efi_system_table_t)))
		panic("slaunch: EFI System Table PA 0x%llx NOT in NORMAL region\n",
		      systab_pa);

	systab = early_memremap_ro(systab_pa, sizeof(efi_system_table_t));
	if (!systab)
		panic("slaunch: failed to map EFI System Table at 0x%llx\n",
		      systab_pa);

	nr_tables = systab->nr_tables;
	tables_pa = (unsigned long)systab->tables;
	early_memunmap(systab, sizeof(efi_system_table_t));

	if (nr_tables == 0 || !tables_pa) {
		pr_info("slaunch: EFI System Table has no ConfigurationTable entries\n");
		return;
	}
	/* Bound nr_tables before multiplication so a malicious systab
	 * cannot produce a huge tbl_size that fails early_memremap
	 * silently or wraps. 256 covers realistic platforms.
	 */
	if (nr_tables > 256)
		panic("slaunch: EFI System Table nr_tables=%lu exceeds bound (256)\n",
		      nr_tables);

	tbl_size = nr_tables * sizeof(efi_config_table_t);

	if (!dcrtm_range_in_normal(tables_pa, tbl_size))
		panic("slaunch: EFI ConfigurationTable array at 0x%lx NOT in NORMAL region\n",
		      tables_pa);

	cfgtbl = early_memremap_ro(tables_pa, tbl_size);
	if (!cfgtbl)
		panic("slaunch: failed to map ConfigurationTable at 0x%lx\n",
		      tables_pa);

	for (j = 0; j < nr_tables; j++) {
		unsigned long tbl_ptr = (unsigned long)cfgtbl[j].table;
		u32 size;

		if (!tbl_ptr)
			continue;
		size = sl_cfgtbl_min_size(&cfgtbl[j].guid);
		if (!dcrtm_range_in_normal(tbl_ptr, size))
			panic("slaunch: EFI ConfigurationTable[%lu] 0x%lx [size %u] NOT in NORMAL region\n",
			      j, tbl_ptr, size);
	}
	early_memunmap(cfgtbl, tbl_size);
	pr_info("slaunch: early EFI System Table validation PASSED (%lu entries)\n",
		nr_tables);
}

/* Validate the raw EFI memory map at /chosen/linux,uefi-mmap-start.
 * Walks descriptors at desc-size stride (NOT sizeof(efi_memory_desc_t),
 * which can differ from desc-size for forward compatibility). Runs
 * pre-efi_init so memblock_add never sees an unvalidated descriptor.
 */
static void __init slaunch_validate_raw_mmap(const struct sl_efi_info *info)
{
	void *mmap;
	u64 offset;
	u32 ndesc, nchecked = 0;

	/* Validate desc-size / desc-ver / mmap-size sanity. */
	if (info->desc_ver != 1)
		panic("slaunch: linux,uefi-mmap-desc-ver=%u (expected 1)\n",
		      info->desc_ver);
	if (info->desc_size < sizeof(efi_memory_desc_t) || info->desc_size > 128)
		panic("slaunch: linux,uefi-mmap-desc-size=%u out of sane range\n",
		      info->desc_size);
	if (info->mmap_size == 0 ||
	    info->mmap_size % info->desc_size != 0)
		panic("slaunch: linux,uefi-mmap-size=%llu not a multiple of desc-size=%u\n",
		      info->mmap_size, info->desc_size);
	if (info->mmap_size > 256UL * SZ_1K)
		panic("slaunch: linux,uefi-mmap-size=%llu exceeds 256 KiB\n",
		      info->mmap_size);

	/* The full mmap buffer must be in NORMAL memory. */
	if (!dcrtm_range_in_normal(info->mmap_pa, info->mmap_size))
		panic("slaunch: linux,uefi-mmap [0x%llx+0x%llx] NOT entirely in NORMAL\n",
		      info->mmap_pa, info->mmap_size);

	mmap = early_memremap_ro(info->mmap_pa, info->mmap_size);
	if (!mmap)
		panic("slaunch: failed to map raw EFI mmap at 0x%llx (size %llu)\n",
		      info->mmap_pa, info->mmap_size);

	ndesc = (u32)(info->mmap_size / info->desc_size);
	for (offset = 0; offset < info->mmap_size; offset += info->desc_size) {
		efi_memory_desc_t *md = (efi_memory_desc_t *)((u8 *)mmap + offset);
		u64 phys = md->phys_addr;
		u64 region_size, phys_end;
		int otype;

		if (md->type == EFI_MEMORY_MAPPED_IO ||
		    md->type == EFI_MEMORY_MAPPED_IO_PORT_SPACE)
			continue;

		/* num_pages * page_size overflow guard. */
		if (check_mul_overflow(md->num_pages, (u64)EFI_PAGE_SIZE,
				       &region_size))
			panic("slaunch: raw EFI mmap[%llu]: type=%u num_pages=%llu overflows u64\n",
			      offset / info->desc_size, md->type, md->num_pages);
		/* phys_addr + region_size address-wrap guard. */
		if (region_size &&
		    check_add_overflow(phys, region_size, &phys_end))
			panic("slaunch: raw EFI mmap[%llu]: [0x%012llx + 0x%llx] type=%u wraps u64\n",
			      offset / info->desc_size, phys, region_size,
			      md->type);

		if (!dcrtm_range_in_normal(phys, region_size))
			panic("slaunch: raw EFI mmap[%llu]: region [0x%012llx-0x%012llx] type=%u NOT in NORMAL\n",
			      offset / info->desc_size, phys,
			      phys + region_size, md->type);

		otype = dcrtm_range_overlaps_non_normal(phys, region_size);
		if (otype >= 0)
			panic("slaunch: raw EFI mmap[%llu]: region [0x%012llx-0x%012llx] type=%u OVERLAPS %s\n",
			      offset / info->desc_size, phys,
			      phys + region_size, md->type,
			      dcrtm_type_name(otype));
		nchecked++;
	}
	early_memunmap(mmap, info->mmap_size);
	pr_info("slaunch: early raw EFI mmap validation PASSED (%u of %u descriptors checked)\n",
		nchecked, ndesc);
}

static void __init slaunch_validate_efi_early(const struct sl_efi_info *info)
{
	if (!info->present) {
		pr_info("slaunch: /chosen does not have all linux,uefi-* properties — skipping early EFI validation\n");
		return;
	}
	slaunch_validate_raw_systab(info->systab_pa);
	slaunch_validate_raw_mmap(info);
}

/*
 * Called early in setup_arch() to process DRTM state.
 *
 * At this point the address map has already been parsed by
 * slaunch_early_init(). This function handles:
 *  - memblock reservation of DLME data
 *  - CLOSE_LOCALITY SMC
 *  - Validation of DTB /chosen EFI pointers
 */
void __init slaunch_setup(void)
{
	struct dlme_data_header *hdr;
	phys_addr_t dlme_data_pa;

	if (!sl_dlme_region_pa)
		return;

	pr_info("slaunch: DRTM Secure Launch detected\n");

	/* Map DLME data header for reservation */
	dlme_data_pa = sl_dlme_region_pa + sl_dlme_data_offset;
	hdr = early_memremap(dlme_data_pa, sizeof(*hdr));
	if (!hdr) {
		pr_err("slaunch: failed to map DLME data header at 0x%llx\n",
		       (u64)dlme_data_pa);
		return;
	}

	pr_info("slaunch: DLME data version: %u\n",
		le16_to_cpu(hdr->version));
	pr_info("slaunch: DLME data size: %llu\n",
		le64_to_cpu(hdr->dlme_data_size));
	pr_info("slaunch: Event log size: %llu\n",
		le64_to_cpu(hdr->drtm_event_log_size));

	/* Enforce full-lockdown assumption per DEN0113 v1.2 §4.6.2.
	 * Called from slaunch_setup (not slaunch_early_init) so panic
	 * prints — earlycon is registered by this point.
	 */
	slaunch_assert_full_lockdown(dlme_data_pa,
				     le16_to_cpu(hdr->this_hdr_size),
				     le64_to_cpu(hdr->protected_regions_size));

	/* Reserve DLME data region in memblock so kernel won't reuse it.
	 * NOTE: efi_init() runs after us and calls memblock_remove(0,
	 * PHYS_ADDR_MAX) which wipes this reservation. slaunch_validate_efi
	 * re-reserves using the saved values below.
	 */
	sl_dlme_data_pa = dlme_data_pa;
	sl_dlme_data_size = le64_to_cpu(hdr->dlme_data_size);
	memblock_reserve(sl_dlme_data_pa, sl_dlme_data_size);

	early_memunmap(hdr, sizeof(*hdr));

	slaunch_tpm_setup();

	/*
	 * Programmatically disable EFI RuntimeServices regardless of
	 * cmdline. RT function pointers originate from untrusted pre-DRTM
	 * firmware; calling them would execute unvalidated code. The EFI
	 * stub also refuses to launch if `drtm=on` is present without
	 * `efi=noruntime` (see efi_slaunch_enabled), so this is a
	 * defense-in-depth backstop — critical security property must
	 * not depend on attacker-controllable cmdline alone.
	 *
	 * Clearing EFI_RUNTIME_SERVICES makes efi_enabled() return false
	 * for callers that gate runtime dispatch on it; zeroing
	 * runtime_supported_mask ensures fine-grained checks also see
	 * "nothing supported".
	 */
	clear_bit(EFI_RUNTIME_SERVICES, &efi.flags);
	efi.runtime_supported_mask = 0;
	pr_info("slaunch: EFI runtime services unconditionally disabled\n");

	/*
	 * Validate all untrusted EFI inputs BEFORE efi_init() gets to
	 * consume them. Read /chosen properties, optionally inject a
	 * test fault, then run the early validators.
	 */
	{
		struct sl_efi_info efi_info;

		slaunch_read_chosen_efi(&efi_info);
		slaunch_inject_fault(&efi_info);
		slaunch_validate_efi_early(&efi_info);
	}
}

/*
 * Measurement stub — log hash for now, replace with CCA HES interface later.
 *
 * In production, this extends the measurement into a hardware root of
 * trust (PSC/CCA HES) for inclusion in the attestation token. Until
 * that backend is wired up, the SHA-256 hash is logged so a remote
 * verifier can check dmesg.
 *
 * Accumulates DLME-side measurements so the event-log builder can
 * replay them. SLAUNCH_MAX_MEASUREMENTS bounds the count; exceeding
 * it is fatal because attestation evidence would be silently dropped.
 */
#define SLAUNCH_MAX_MEASUREMENTS	32
struct slaunch_measurement {
	char	desc[16];
	u8	hash[SHA256_DIGEST_SIZE];
};

static struct slaunch_measurement slaunch_measurements[SLAUNCH_MAX_MEASUREMENTS] __initdata;
static unsigned int slaunch_measurement_count __initdata;

static void __init slaunch_measure(const char *desc, const void *data,
				   size_t size)
{
	u8 hash[SHA256_DIGEST_SIZE];
	struct slaunch_measurement *m;

	sha256(data, size, hash);

	pr_info("slaunch: measured %s (%zu bytes) SHA-256: "
		"%*phN\n", desc, size, SHA256_DIGEST_SIZE, hash);

	/* TODO(CCA-HES): replace the pr_info above (or augment it) with
	 * an SMC into TF-A that extends the CCA HES tenant token / Realm
	 * claim with this hash. Until the HES backend is wired up, the
	 * hash is captured into slaunch_measurements[] and published only
	 * via the DRTM event log + dmesg.
	 */

	if (slaunch_measurement_count >= SLAUNCH_MAX_MEASUREMENTS)
		panic("slaunch: measurement table full (%u); raise SLAUNCH_MAX_MEASUREMENTS\n",
		      slaunch_measurement_count);

	m = &slaunch_measurements[slaunch_measurement_count++];
	strscpy(m->desc, desc, sizeof(m->desc));
	memcpy(m->hash, hash, SHA256_DIGEST_SIZE);
}

/*
 * Measure ACPI tables before DMA protection is released.
 *
 * Called during slaunch_validate_efi(), which runs after efi_init()
 * and acpi_boot_table_init() but well before the late_initcall
 * slaunch_unprotect_memory(). DMA protection is still active, so
 * no device can tamper with the tables during measurement.
 *
 * We measure RSDP, XSDT, and each ACPI table referenced by XSDT.
 * The hashes provide attestation evidence — a remote verifier can
 * compare them against known-good values for the platform/firmware.
 */
/*
 * RSDP layout (ACPI 2.0+): we only need xsdt_physical_address at offset 24
 * and length at offset 20. Avoid depending on <acpi/acpi.h> for the struct.
 */
/* Minimal ACPI table header — avoid full <acpi/acpi.h> dependency.
 * Full header is 36 bytes; XSDT entries follow after this.
 */
struct slaunch_acpi_hdr {
	char signature[4];
	u32 length;
	u8 revision;
	u8 checksum;
	char oem_id[6];
	char oem_table_id[8];
	u32 oem_revision;
	u32 creator_id;
	u32 creator_revision;
} __packed;

#define RSDP_SIZE_V1	20
#define RSDP_OFF_LEN	20	/* u32 length (ACPI 2.0+) */
#define RSDP_OFF_XSDT	24	/* u64 xsdt_physical_address */
#define RSDP_MIN_MAP	36	/* enough to read through xsdt_physical_address */

/* FADT field offsets (ACPI 6.x §5.2.9) — used to follow indirection
 * to DSDT and FACS. Local copies to avoid pulling in <acpi/actbl.h>.
 */
#define FADT_FIRMWARE_CTRL_OFF		36	/* u32 */
#define FADT_DSDT_OFF			40	/* u32 */
#define FADT_X_FIRMWARE_CTRL_OFF	132	/* u64, ACPI 2.0+ */
#define FADT_X_DSDT_OFF			140	/* u64, ACPI 2.0+ */

/* Validate PA + length against D-CRTM map, then measure. Every
 * failure is fatal — silent skip breaks attestation soundness.
 */
static void __init slaunch_measure_one_acpi(phys_addr_t pa, const char *desc)
{
	struct slaunch_acpi_hdr *tbl;
	u32 tbl_len;

	if (!pa)
		panic("slaunch: %s PA is 0 — cannot measure\n", desc);
	if (!dcrtm_range_in_normal(pa, sizeof(*tbl)))
		panic("slaunch: %s PA 0x%llx (header) NOT in NORMAL region\n",
		      desc, (u64)pa);

	tbl = early_memremap(pa, sizeof(*tbl));
	if (!tbl)
		panic("slaunch: %s header remap failed at 0x%llx\n",
		      desc, (u64)pa);
	tbl_len = tbl->length;
	early_memunmap(tbl, sizeof(*tbl));

	if (tbl_len < sizeof(*tbl))
		panic("slaunch: %s length %u < header size %zu\n",
		      desc, tbl_len, sizeof(*tbl));
	if (!dcrtm_range_in_normal(pa, tbl_len))
		panic("slaunch: %s [0x%llx+%u] NOT in NORMAL region\n",
		      desc, (u64)pa, tbl_len);

	tbl = early_memremap(pa, tbl_len);
	if (!tbl)
		panic("slaunch: %s full remap failed at 0x%llx (size %u)\n",
		      desc, (u64)pa, tbl_len);
	slaunch_measure(desc, tbl, tbl_len);
	early_memunmap(tbl, tbl_len);
}

/* Query DCE's TPM hash algorithm via DRTM_FEATURES feature 0x1 per
 * DEN0113 v1.2 §3.3, and refuse to proceed if it isn't what is
 * implemented here. Without this, a SHA-384 platform would silently
 * get SHA-256 digests in the event log and the chain would not
 * replay against the HES quote. Field layout:
 *   bits [15:0]: firmware_hash_algorithm (TPM_ALG_* — 0xB SHA-256,
 *                                         0xC SHA-384)
 *   bit  [32]:   tpm_based_hash_support
 *   bits [36:33]: pcr_schema
 */
#define SL_DRTM_FW_HASH_SHA256		0x000B
#define SL_DRTM_FW_HASH_SHA384		0x000C
#define SL_DRTM_FW_HASH_MASK		0xFFFFULL

static void __init slaunch_verify_hash_algo(void)
{
	struct arm_smccc_res res;
	u64 features;
	u32 algo;

	/* Feature 0x1 = TPM features. Bit 63 set per spec.
	 * Per TF-A's drtm_features_tpm() (SMC_RET2): a0 = 1 (supported)
	 * or DRTM_NOT_SUPPORTED; a1 = the actual tpm_features bitfield.
	 * Read a1 for the value.
	 */
	arm_smccc_smc(DRTM_SMC_FEATURES, (1ULL << 63) | 0x1,
		      0, 0, 0, 0, 0, 0, &res);
	if ((s64)res.a0 == DRTM_NOT_SUPPORTED) {
		pr_warn("slaunch: DRTM_FEATURES(TPM) not supported; assuming SHA-256\n");
		return;
	}

	features = res.a1;
	algo = features & SL_DRTM_FW_HASH_MASK;
	pr_info("slaunch: DCE firmware hash algorithm: 0x%x\n", algo);

	if (algo != SL_DRTM_FW_HASH_SHA256)
		panic("slaunch: DCE reports hash algo 0x%x; kernel only implements SHA-256 (0xB). Add SHA-384 path or use a SHA-256 DCE.\n",
		      algo);
}

static void __init slaunch_measure_acpi(void)
{
	struct slaunch_acpi_hdr *xsdt;
	phys_addr_t rsdp_pa, xsdt_pa;
	phys_addr_t dsdt_pa = 0, facs_pa = 0;
	u32 rsdp_len, xsdt_len, num_entries, i;
	u64 *entry_ptrs;
	void *rsdp;

	/* Every failure below is fatal. The "kernel acts on unmeasured
	 * bytes" case breaks the attestation-based trust model.
	 */
	rsdp_pa = efi.acpi20;
	if (rsdp_pa == EFI_INVALID_TABLE_ADDR || !rsdp_pa)
		panic("slaunch: no ACPI RSDP in EFI System Table (DRTM requires ACPI)\n");

	if (!dcrtm_range_in_normal(rsdp_pa, RSDP_MIN_MAP))
		panic("slaunch: RSDP PA 0x%llx NOT in NORMAL region\n",
		      (u64)rsdp_pa);

	rsdp = early_memremap(rsdp_pa, RSDP_MIN_MAP);
	if (!rsdp)
		panic("slaunch: RSDP header remap failed at 0x%llx\n",
		      (u64)rsdp_pa);

	rsdp_len = *(u32 *)((u8 *)rsdp + RSDP_OFF_LEN);
	if (!rsdp_len)
		rsdp_len = RSDP_SIZE_V1;
	xsdt_pa = *(u64 *)((u8 *)rsdp + RSDP_OFF_XSDT);
	early_memunmap(rsdp, RSDP_MIN_MAP);

	if (!dcrtm_range_in_normal(rsdp_pa, rsdp_len))
		panic("slaunch: RSDP [0x%llx+%u] NOT in NORMAL region\n",
		      (u64)rsdp_pa, rsdp_len);
	rsdp = early_memremap(rsdp_pa, rsdp_len);
	if (!rsdp)
		panic("slaunch: RSDP full remap failed at 0x%llx (size %u)\n",
		      (u64)rsdp_pa, rsdp_len);
	slaunch_measure("RSDP", rsdp, rsdp_len);
	early_memunmap(rsdp, rsdp_len);

	if (!dcrtm_range_in_normal(xsdt_pa, sizeof(*xsdt)))
		panic("slaunch: XSDT PA 0x%llx NOT in NORMAL region\n", xsdt_pa);
	xsdt = early_memremap(xsdt_pa, sizeof(*xsdt));
	if (!xsdt)
		panic("slaunch: XSDT header remap failed at 0x%llx\n", xsdt_pa);
	xsdt_len = xsdt->length;
	early_memunmap(xsdt, sizeof(*xsdt));

	if (xsdt_len < sizeof(*xsdt))
		panic("slaunch: XSDT length %u < header size %zu\n",
		      xsdt_len, sizeof(*xsdt));
	if (!dcrtm_range_in_normal(xsdt_pa, xsdt_len))
		panic("slaunch: XSDT [0x%llx+%u] NOT in NORMAL region\n",
		      xsdt_pa, xsdt_len);
	xsdt = early_memremap(xsdt_pa, xsdt_len);
	if (!xsdt)
		panic("slaunch: XSDT full remap failed at 0x%llx (size %u)\n",
		      xsdt_pa, xsdt_len);
	slaunch_measure("XSDT", xsdt, xsdt_len);

	/* Walk XSDT entries. Each is a 64-bit PA to a top-level ACPI table. */
	num_entries = (xsdt_len - sizeof(*xsdt)) / sizeof(u64);
	entry_ptrs = (u64 *)((u8 *)xsdt + sizeof(*xsdt));

	for (i = 0; i < num_entries; i++) {
		struct slaunch_acpi_hdr *tbl;
		u64 tbl_pa = entry_ptrs[i];
		u32 tbl_len;
		char desc[32];

		if (!dcrtm_range_in_normal(tbl_pa, sizeof(*tbl)))
			panic("slaunch: XSDT entry[%u] PA 0x%llx (hdr) NOT in NORMAL\n",
			      i, tbl_pa);
		tbl = early_memremap(tbl_pa, sizeof(*tbl));
		if (!tbl)
			panic("slaunch: XSDT entry[%u] header remap failed at 0x%llx\n",
			      i, tbl_pa);
		tbl_len = tbl->length;
		early_memunmap(tbl, sizeof(*tbl));

		if (tbl_len < sizeof(*tbl))
			panic("slaunch: XSDT entry[%u] length %u < header size %zu\n",
			      i, tbl_len, sizeof(*tbl));
		if (!dcrtm_range_in_normal(tbl_pa, tbl_len))
			panic("slaunch: XSDT entry[%u] [0x%llx+%u] NOT in NORMAL\n",
			      i, tbl_pa, tbl_len);
		tbl = early_memremap(tbl_pa, tbl_len);
		if (!tbl)
			panic("slaunch: XSDT entry[%u] full remap failed at 0x%llx (size %u)\n",
			      i, tbl_pa, tbl_len);

		snprintf(desc, sizeof(desc), "ACPI:%.4s", tbl->signature);
		slaunch_measure(desc, tbl, tbl_len);

		/* Capture FADT indirection while FADT is mapped. Prefer
		 * 64-bit X_* fields (ACPI 2.0+); fall back to 32-bit
		 * fields if FADT length is too short to carry them.
		 */
		if (!memcmp(tbl->signature, "FACP", 4)) {
			if (tbl_len >= FADT_X_DSDT_OFF + sizeof(u64))
				memcpy(&dsdt_pa,
				       (u8 *)tbl + FADT_X_DSDT_OFF,
				       sizeof(u64));
			if (!dsdt_pa &&
			    tbl_len >= FADT_DSDT_OFF + sizeof(u32)) {
				u32 d32;

				memcpy(&d32,
				       (u8 *)tbl + FADT_DSDT_OFF,
				       sizeof(u32));
				dsdt_pa = d32;
			}
			if (tbl_len >= FADT_X_FIRMWARE_CTRL_OFF + sizeof(u64))
				memcpy(&facs_pa,
				       (u8 *)tbl + FADT_X_FIRMWARE_CTRL_OFF,
				       sizeof(u64));
			if (!facs_pa &&
			    tbl_len >= FADT_FIRMWARE_CTRL_OFF + sizeof(u32)) {
				u32 f32;

				memcpy(&f32,
				       (u8 *)tbl + FADT_FIRMWARE_CTRL_OFF,
				       sizeof(u32));
				facs_pa = f32;
			}
		}
		early_memunmap(tbl, tbl_len);
	}

	early_memunmap(xsdt, xsdt_len);

	/* Follow FADT indirections — these tables are referenced from
	 * FADT, not XSDT.
	 *
	 * DSDT carries the platform's AML, executed by ACPICA in kernel
	 * context throughout system life. Mandatory measurement target.
	 * FACS (firmware ACPI control structure) holds the wake vector
	 * and global lock; optional on HW-reduced ACPI (X_FirmwareCtrl=0
	 * is allowed).
	 *
	 * Other indirect ACPI structures (BERT/ERST/HEST/EINJ/PCCT etc.)
	 * point at error-log or runtime-mailbox buffers — consumed as
	 * data, not executed; not boot-TCB. Deliberately not followed
	 * here.
	 */
	if (!dsdt_pa)
		panic("slaunch: FADT present but X_Dsdt/Dsdt both zero — DSDT cannot be measured\n");
	slaunch_measure_one_acpi(dsdt_pa, "DSDT");
	if (facs_pa)
		slaunch_measure_one_acpi(facs_pa, "FACS");
	else
		pr_info("slaunch: FACS absent (HW-reduced ACPI) — no measurement needed\n");
}

/*
 * Extend DRTM event log with DLME-side measurements.
 *
 * DCE (TF-A) writes a TCG-compliant event log into the DLME data
 * region before ERET — its events cover DCE image, DLME image,
 * separator, etc. The buffer lives at:
 *   dlme_data_pa + this_hdr_size + protected_regions_size + address_map_size
 * with current length in drtm_event_log_size of the DLME data
 * header. The DLME data region was sized by the Preamble via
 * DRTM_FEATURES; any space between event_log end and DLME data end
 * is slack the DLME can append into.
 *
 * One TCG_PCR_EVENT2 entry is appended per DLME-side measurement
 * (one per ACPI table hashed by slaunch_measure) directly into the
 * DLME data buffer, then drtm_event_log_size is bumped so the
 * header reflects the new total. A verifier reading the DLME data
 * event log sees DCE + DLME events as one cohesive chain.
 *
 * Format references:
 *   TCG_PCR_EVENT2 — TCG PC Client Platform Firmware Profile §10.2.2
 *   Event log structure — DEN0113 v1.2 §3.17
 *   Default PCR schema — DEN0113 v1.2 §4.8.4 (PCR 17 used here)
 */
#define SL_TPM_ALG_SHA256		0x000B
/*
 * TODO: DEN0113 v1.2 §3.17.2 Table 19 defines an Arm-specific event
 * type space (EVTYPE_ARM_BASE = 0x9000) with no entry that exactly
 * matches "platform-supplied measurement of ACPI table from the
 * DLME side". Generic TCG EV_PLATFORM_CONFIG_FLAGS (0x0A) is used
 * here because a verifier that follows TCG PFP can ingest it
 * without knowing the Arm event space. Switch to an
 * EVTYPE_ARM_NO_ACTION or a dedicated EVTYPE_ARM_* once one is
 * registered for DLME-side ACPI measurements.
 */
#define SL_EV_PLATFORM_CONFIG_FLAGS	0x0000000A
#define SL_DRTM_PCR_INDEX		17	/* DEN0113 v1.2 §4.8.4 */

/* Write one TCG_PCR_EVENT2 at `*off` in `buf`; advances `*off`.
 * Returns -ENOSPC if there isn't room within `max`.
 *
 * Packed little-endian layout:
 *   u32 PCRIndex
 *   u32 EventType
 *   u32 digest_count
 *   u16 hashAlg            } per digest (count=1 here)
 *   u8  hash[hash_size]    }
 *   u32 EventSize
 *   u8  Event[EventSize]
 */
static int __init sl_evlog_append_event2(u8 *buf, size_t *off, size_t max,
					 u32 pcr, u32 type,
					 const u8 hash[SHA256_DIGEST_SIZE],
					 const void *event_data,
					 u32 event_size)
{
	size_t need = 4 + 4 + 4 + 2 + SHA256_DIGEST_SIZE + 4 + event_size;
	u8 *p;

	if (*off + need > max)
		return -ENOSPC;

	p = buf + *off;
	*(__le32 *)(p +  0) = cpu_to_le32(pcr);
	*(__le32 *)(p +  4) = cpu_to_le32(type);
	*(__le32 *)(p +  8) = cpu_to_le32(1);
	*(__le16 *)(p + 12) = cpu_to_le16(SL_TPM_ALG_SHA256);
	memcpy(p + 14, hash, SHA256_DIGEST_SIZE);
	*(__le32 *)(p + 14 + SHA256_DIGEST_SIZE) = cpu_to_le32(event_size);
	if (event_size)
		memcpy(p + 14 + SHA256_DIGEST_SIZE + 4, event_data, event_size);
	*off += need;
	return 0;
}

static void __init slaunch_extend_drtm_event_log(void)
{
	phys_addr_t dlme_data_pa, evlog_pa;
	struct dlme_data_header *hdr;
	u64 hdr_size, prot_size, map_size, dlme_data_size;
	u64 evlog_size_initial;
	size_t evlog_max, evlog_off;
	u8 *evlog_va;
	unsigned int i;

	if (!sl_dlme_region_pa)
		return;

	dlme_data_pa = sl_dlme_region_pa + sl_dlme_data_offset;

	/* Read offsets from the DLME data header. Unmap before we touch
	 * the event log to avoid fixmap-slot overlap on the same page.
	 */
	hdr = early_memremap(dlme_data_pa, sizeof(*hdr));
	if (!hdr)
		panic("slaunch: event log: cannot map DLME data header\n");
	hdr_size            = le16_to_cpu(hdr->this_hdr_size);
	prot_size           = le64_to_cpu(hdr->protected_regions_size);
	map_size            = le64_to_cpu(hdr->address_map_size);
	dlme_data_size      = le64_to_cpu(hdr->dlme_data_size);
	evlog_size_initial  = le64_to_cpu(hdr->drtm_event_log_size);
	early_memunmap(hdr, sizeof(*hdr));

	if (evlog_size_initial == 0)
		panic("slaunch: DCE published empty DRTM event log\n");

	/* The event log buffer extends from its start (after header +
	 * protected_regions + address_map) to the end of the DLME data
	 * region the Preamble allocated. evlog_max is the addressable
	 * capacity; DCE's events occupy the first evlog_size_initial of
	 * those bytes; DLME appends into the slack.
	 */
	evlog_pa  = dlme_data_pa + hdr_size + prot_size + map_size;
	evlog_max = (size_t)(sl_dlme_data_size -
			     (hdr_size + prot_size + map_size));

	pr_info("slaunch: DRTM event log buffer: PA 0x%llx, capacity %zu B, DCE used %llu B, slack %zu B\n",
		(u64)evlog_pa, evlog_max, evlog_size_initial,
		evlog_max - (size_t)evlog_size_initial);

	evlog_va = early_memremap(evlog_pa, evlog_max);
	if (!evlog_va)
		panic("slaunch: cannot map DRTM event log at 0x%llx (%zu B)\n",
		      (u64)evlog_pa, evlog_max);

	/* Append DLME events after DCE's events, in-place in DLME data. */
	evlog_off = (size_t)evlog_size_initial;
	for (i = 0; i < slaunch_measurement_count; i++) {
		const struct slaunch_measurement *m = &slaunch_measurements[i];
		size_t dlen = strnlen(m->desc, sizeof(m->desc));

		if (sl_evlog_append_event2(evlog_va, &evlog_off, evlog_max,
					   SL_DRTM_PCR_INDEX,
					   SL_EV_PLATFORM_CONFIG_FLAGS,
					   m->hash, m->desc, (u32)dlen))
			panic("slaunch: event log overflow at DLME entry %u (%s) — bump Preamble's sl_dlme_data_reserve\n",
			      i, m->desc);
	}

	pr_info("slaunch: DRTM event log: appended %u DLME entries (%zu B); new total %zu B\n",
		slaunch_measurement_count,
		evlog_off - (size_t)evlog_size_initial, evlog_off);

	/* Dump the canonical event-log bytes — what a verifier replays.
	 * This is the spec-defined artifact; pretty-prints below are
	 * debug aids.
	 */
	pr_info("slaunch: DRTM event log canonical bytes (DCE + DLME, %zu B):\n",
		evlog_off);
	print_hex_dump(KERN_INFO, "slaunch:   ", DUMP_PREFIX_OFFSET,
		       32, 1, evlog_va, evlog_off, false);

	early_memunmap(evlog_va, evlog_max);

	/* Update the DLME data header so drtm_event_log_size reflects
	 * what we just wrote. Verifier reads (PA, size) from the header
	 * to know what to replay.
	 */
	hdr = early_memremap(dlme_data_pa, sizeof(*hdr));
	if (!hdr)
		panic("slaunch: cannot re-map DLME data header to update size\n");
	hdr->drtm_event_log_size = cpu_to_le64(evlog_off);
	early_memunmap(hdr, sizeof(*hdr));

	pr_info("slaunch: DLME event log entries (PCR %u, EV_PLATFORM_CONFIG_FLAGS):\n",
		SL_DRTM_PCR_INDEX);
	for (i = 0; i < slaunch_measurement_count; i++) {
		const struct slaunch_measurement *m = &slaunch_measurements[i];

		pr_info("slaunch:   [%2u] %-12s SHA-256: %*phN\n",
			i, m->desc, SHA256_DIGEST_SIZE, m->hash);
	}
}

#ifdef CONFIG_ARM64_SECURE_LAUNCH_FAULT_INJECT
/*
 * Negative-test fault injection. Gated by cmdline token
 *   slaunch_inject=<token>
 * Mutates the raw EFI memory map buffer in place before
 * slaunch_validate_efi_early runs. With the overflow guards in
 * place, the early validator is expected to panic with a specific
 * message. The harness treats that as a passing negative test.
 *
 * Tokens: mmap_wrap, mmap_pages_overflow, mmap_size_huge,
 * systab_nr_tables_huge.
 *
 * DO NOT enable CONFIG_ARM64_SECURE_LAUNCH_FAULT_INJECT in production.
 */
static bool __init sl_cmdline_has(const char *tok)
{
	return strstr(boot_command_line, tok);
}

/* Mutate one descriptor in the raw EFI mmap buffer. Returns true if
 * we found a CONVENTIONAL_MEMORY descriptor and applied the mutation.
 */
typedef void (*sl_md_mutator_t)(efi_memory_desc_t *md);

static bool __init slaunch_inject_raw_mmap(const struct sl_efi_info *info,
					   sl_md_mutator_t mutate)
{
	void *mmap;
	u64 offset;
	bool applied = false;

	mmap = early_memremap(info->mmap_pa, info->mmap_size);
	if (!mmap) {
		pr_warn("slaunch: INJECT: failed to map raw mmap\n");
		return false;
	}
	for (offset = 0; offset < info->mmap_size; offset += info->desc_size) {
		efi_memory_desc_t *md =
			(efi_memory_desc_t *)((u8 *)mmap + offset);

		if (md->type != EFI_CONVENTIONAL_MEMORY)
			continue;
		mutate(md);
		applied = true;
		break;
	}
	early_memunmap(mmap, info->mmap_size);
	return applied;
}

static void __init sl_mutate_mmap_wrap(efi_memory_desc_t *md)
{
	/* phys_addr + num_pages*4096 wraps past U64_MAX */
	md->num_pages = (~md->phys_addr / EFI_PAGE_SIZE) + 2;
	pr_warn("slaunch: INJECT mmap_wrap on phys=0x%llx -> num_pages=%llu (expect panic 'wraps u64')\n",
		md->phys_addr, md->num_pages);
}

static void __init sl_mutate_mmap_pages_overflow(efi_memory_desc_t *md)
{
	/* num_pages * EFI_PAGE_SIZE itself overflows */
	md->num_pages = (U64_MAX / EFI_PAGE_SIZE) + 2;
	pr_warn("slaunch: INJECT mmap_pages_overflow on phys=0x%llx -> num_pages=%llu (expect panic 'overflows u64')\n",
		md->phys_addr, md->num_pages);
}

static void __init slaunch_inject_fault(struct sl_efi_info *info)
{
	if (!info->present) {
		/* Nothing to inject into. */
		return;
	}

	if (sl_cmdline_has("slaunch_inject=mmap_wrap")) {
		if (!slaunch_inject_raw_mmap(info, sl_mutate_mmap_wrap))
			pr_warn("slaunch: INJECT mmap_wrap: no EFI_CONVENTIONAL_MEMORY descriptor found\n");
		return;
	}
	if (sl_cmdline_has("slaunch_inject=mmap_pages_overflow")) {
		if (!slaunch_inject_raw_mmap(info, sl_mutate_mmap_pages_overflow))
			pr_warn("slaunch: INJECT mmap_pages_overflow: no EFI_CONVENTIONAL_MEMORY descriptor found\n");
		return;
	}
	if (sl_cmdline_has("slaunch_inject=mmap_size_huge")) {
		/* Mutate our local copy of the size — the property in
		 * memory is read-only via libfdt but our sl_efi_info
		 * struct drives subsequent validation. The bound check
		 * in slaunch_validate_raw_mmap should panic.
		 */
		/* 288 KiB = 6144 * 48, divisible by stock desc_size=48 so we
		 * exercise the >256 KiB cap rather than the multiple-of-desc
		 * guard above it.
		 */
		info->mmap_size = 0x48000ULL;
		pr_warn("slaunch: INJECT mmap_size_huge: info->mmap_size=%llu (expect panic 'exceeds 256 KiB')\n",
			info->mmap_size);
		return;
	}
	if (sl_cmdline_has("slaunch_inject=systab_nr_tables_huge")) {
		/* Mutate the System Table in-place to inflate nr_tables.
		 * The nr_tables bound (256) must panic.
		 */
		efi_system_table_t *systab;

		systab = early_memremap(info->systab_pa,
					sizeof(efi_system_table_t));
		if (!systab) {
			pr_warn("slaunch: INJECT systab_nr_tables_huge: remap failed\n");
			return;
		}
		systab->nr_tables = 100000UL;
		pr_warn("slaunch: INJECT systab_nr_tables_huge: nr_tables=%lu (expect panic 'exceeds bound')\n",
			(unsigned long)systab->nr_tables);
		early_memunmap(systab, sizeof(efi_system_table_t));
		return;
	}
}
#endif /* CONFIG_ARM64_SECURE_LAUNCH_FAULT_INJECT */

#ifdef CONFIG_ARM64_SECURE_LAUNCH_SELFTEST
/*
 * Self-test of the validation guards. Runs at the end of
 * slaunch_measure_post_efi after the real validation has passed.
 * Calls the helpers directly with crafted inputs (wrap on
 * start+size, wrap on count*size). The helpers must reject the bad
 * inputs and accept the valid ones; mismatch panics with the
 * "selftest:" prefix and the harness flags it as a negative-test
 * PASS (panic) or positive-test FAIL (regression).
 */
static void __init slaunch_selftest(void)
{
	/* T1: dcrtm_range_in_normal MUST reject wrapping start+size */
	if (dcrtm_range_in_normal(0xFFFFFFFFFFFFE000ULL, 0x10000ULL))
		panic("selftest: dcrtm_range_in_normal accepted wrapping range\n");

	/* T2: dcrtm_range_in_normal MUST reject size==0 */
	if (dcrtm_range_in_normal(0x80000000ULL, 0))
		panic("selftest: dcrtm_range_in_normal accepted size=0\n");

	/* T3: dcrtm_range_in_normal MUST accept a known-good NORMAL range
	 * (one page in NS DRAM). Regression check.
	 */
	if (!dcrtm_range_in_normal(0x80000000ULL, EFI_PAGE_SIZE))
		panic("selftest: dcrtm_range_in_normal rejected known NORMAL range (regression)\n");

	/* T4: dcrtm_range_overlaps_non_normal MUST fail closed on wrap
	 * (returns RSVD instead of -1).
	 */
	if (dcrtm_range_overlaps_non_normal(0xFFFFFFFFFFFFE000ULL,
					    0x10000ULL) != DRTM_REGION_TYPE_RSVD)
		panic("selftest: dcrtm_range_overlaps_non_normal did not fail closed on wrap\n");

	/* T5: efi_regions_overlap MUST fail closed (true) on either wrap */
	if (!efi_regions_overlap(0xFFFFFFFFFFFFE000ULL, 0x10000ULL,
				 0x80000000ULL, EFI_PAGE_SIZE))
		panic("selftest: efi_regions_overlap did not fail closed on wrap\n");

	/* T6: efi_regions_overlap on disjoint ranges MUST return false */
	if (efi_regions_overlap(0x80000000ULL, 0x1000ULL,
				0x90000000ULL, 0x1000ULL))
		panic("selftest: efi_regions_overlap reported false overlap on disjoint ranges\n");

	/* T7: efi_regions_overlap on truly-overlapping ranges MUST return true */
	if (!efi_regions_overlap(0x80000000ULL, 0x10000ULL,
				 0x80008000ULL, 0x10000ULL))
		panic("selftest: efi_regions_overlap missed real overlap\n");

	/* T8: check_mul_overflow catches num_pages * EFI_PAGE_SIZE wrap
	 * (the macro behind the slaunch_validate_efi guard).
	 */
	{
		u64 out;

		if (!check_mul_overflow((u64)0x10000000000000ULL,
					(u64)EFI_PAGE_SIZE, &out))
			panic("selftest: check_mul_overflow missed num_pages*PAGE_SIZE wrap\n");
	}

	pr_info("slaunch: ALL SELFTESTS PASSED (8/8)\n");
}
#endif /* CONFIG_ARM64_SECURE_LAUNCH_SELFTEST */

/*
 * All validation of untrusted EFI inputs happens in slaunch_setup()
 * via slaunch_validate_efi_early() — before efi_init() ingests them.
 * This post-efi_init slot keeps only the jobs that require
 * efi_init's outputs:
 *
 *  - Re-reserve DLME data in memblock (efi_init's
 *    memblock_remove(0, PHYS_ADDR_MAX) wipes the slaunch_setup
 *    reservation).
 *  - Measure ACPI tables via efi.acpi20 (which efi_init populated
 *    from the now pre-validated ConfigurationTable).
 *  - Run the validation-helper self-test
 *    (CONFIG_ARM64_SECURE_LAUNCH_SELFTEST).
 */
void __init slaunch_measure_post_efi(void)
{
	if (!sl_dlme_region_pa)
		return;

	if (sl_dlme_data_size)
		memblock_reserve(sl_dlme_data_pa, sl_dlme_data_size);

	slaunch_selftest();
	slaunch_verify_hash_algo();
	slaunch_measure_acpi();
	slaunch_extend_drtm_event_log();

	/* Release the early_memremap_ro slot held by dcrtm_regions
	 * since slaunch_parse_address_map. No further validators consume
	 * it. Setting the pointer to NULL makes any stray post-init
	 * caller fail-fast in dcrtm_range_in_normal's !dcrtm_regions
	 * guard.
	 */
	if (dcrtm_regions) {
		early_memunmap(dcrtm_regions,
			       dcrtm_num_regions * sizeof(*dcrtm_regions));
		dcrtm_regions = NULL;
	}
}

/*
 * Release DRTM DMA protection after IOMMU/SMMU drivers have
 * established their own DMA isolation.
 */
static int __init slaunch_unprotect_memory(void)
{
	struct arm_smccc_res res;

	if (!sl_dlme_region_pa)
		return 0;

	pr_info("slaunch: Calling DRTM_UNPROTECT_MEMORY\n");
	arm_smccc_smc(DRTM_SMC_UNPROTECT_MEMORY, 0, 0, 0, 0, 0, 0, 0, &res);
	if (res.a0 != DRTM_SUCCESS) {
		pr_err("slaunch: UNPROTECT_MEMORY failed: %ld\n",
		       (long)res.a0);
		return -EIO;
	}

	pr_info("slaunch: DMA protection released\n");
	return 0;
}
late_initcall(slaunch_unprotect_memory);

/*
 * Clean up DRTM state before kexec or reboot.
 *
 * Do not call DRTM_SET_ERROR(0). Per DEN0113 v1.2 §3.8, SET_ERROR's
 * argument is the persisted error code; zero is reserved and the
 * spec does not define a "clear errors" semantics.
 */
void slaunch_exit(void)
{
	if (!sl_dlme_region_pa)
		return;

	pr_info("slaunch: Cleaning DRTM state before kexec/reboot\n");
	sl_dlme_region_pa = 0;
}
