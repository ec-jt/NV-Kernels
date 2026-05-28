/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ARM64 DRTM (Dynamic Root of Trust for Measurement) definitions
 *
 * Based on DEN0113 v1.2 — Arm DRTM Architecture Specification
 */
#ifndef _ASM_ARM64_DRTM_H
#define _ASM_ARM64_DRTM_H

#ifndef __ASSEMBLY__
#include <linux/types.h>
#endif

/* DRTM SMC Function IDs (DEN0113 v1.2 §3.2-3.11) */
#define DRTM_SMC_FN_BASE		0xC4000110UL
#define DRTM_SMC_VERSION		(DRTM_SMC_FN_BASE + 0x00)
#define DRTM_SMC_FEATURES		(DRTM_SMC_FN_BASE + 0x01)
#define DRTM_SMC_UNPROTECT_MEMORY	(DRTM_SMC_FN_BASE + 0x03)
#define DRTM_SMC_DYNAMIC_LAUNCH		(DRTM_SMC_FN_BASE + 0x04)
#define DRTM_SMC_CLOSE_LOCALITY		(DRTM_SMC_FN_BASE + 0x05)
#define DRTM_SMC_GET_ERROR		(DRTM_SMC_FN_BASE + 0x06)
#define DRTM_SMC_SET_ERROR		(DRTM_SMC_FN_BASE + 0x07)
#define DRTM_SMC_SET_TCB_HASH		(DRTM_SMC_FN_BASE + 0x08)
#define DRTM_SMC_LOCK_TCB_HASH		(DRTM_SMC_FN_BASE + 0x09)

/* DRTM Return Codes (DEN0113 v1.2 §3.18, Table 20) */
#define DRTM_SUCCESS			0
#define DRTM_NOT_SUPPORTED		(-1)
#define DRTM_INVALID_PARAMETERS		(-2)
#define DRTM_DENIED			(-3)
#define DRTM_INTERNAL_ERROR		(-5)

#define DRTM_PARAMS_REVISION		1

/* Launch features */
#define DRTM_LAUNCH_FEAT_MEM_PROT_ALL	(0x0 << 3)

/* DRTM page size */
#define DRTM_PAGE_SIZE			0x1000

/*
 * Convention slot for the Preamble -> DLME DTB-PA handoff. The
 * Preamble writes the device-tree physical address into the 8 bytes
 * immediately preceding the DLME data region (kernel_addr +
 * dlme_data_offset + SL_DLME_DTB_SLOT_OFFSET). sl_entry reads it back
 * via the same offset from X0 + X1 after the D-CRTM ERET. Not a
 * DEN0113 construct -- private contract between our Preamble and our
 * DLME; the DTB PA itself is untrusted in either direction and is
 * validated against the D-CRTM address map by slaunch_early_init
 * before the kernel parses the FDT header.
 */
#define SL_DLME_DTB_SLOT_OFFSET		(-8)

/* Full-range memory protection sentinel entry
 * (DEN0113 v1.2 §3.14 Table 11 + §4.6.2 sentinel for complete DMA
 * protection): region type = 0 (normal), start = 0, page count =
 * 2^52 - 1.
 */
#define DRTM_MEM_PROT_FULL_RANGE	\
	((0x0ULL << 55) | (0x0ULL << 52) | ((1ULL << 52) - 1ULL))

#ifndef __ASSEMBLY__
/*
 * DRTM_PARAMETERS (DEN0113 v1.2 §3.13, Table 9)
 * Passed to DRTM_DYNAMIC_LAUNCH SMC in X1.
 */
struct drtm_parameters {
	u16	revision;
	u16	reserved;
	u32	launch_features;
	u64	dlme_region_address;
	u64	dlme_region_size;
	u64	dlme_image_start;
	u64	dlme_entry_point_offset;
	u64	dlme_image_size;
	u64	dlme_data_offset;
	u64	nw_dce_region_address;
	u64	nw_dce_region_size;
	u64	mem_prot_table_address;
	u64	mem_prot_table_size;
} __packed;

/* Memory Region Descriptor Table (DEN0113 v1.2 §3.14, Table 11) */
struct drtm_mem_region_hdr {
	u16	revision;
	u16	reserved;
	u32	num_regions;
} __packed;

struct drtm_mem_region {
	u64	start_address;
	u64	size_and_type;
} __packed;

/*
 * Address map region types (DEN0113 v1.2 §3.14, Table 11)
 * Encoded in bits [54:52] of size_and_type (3 bits).
 *   0 = normal, usable memory
 *   1 = normal memory with cacheability attribute requirements
 *       (cacheability encoded in bits [56:55])
 *   2 = device memory / MMIO
 *   3 = non-volatile memory
 *   4 = reserved
 */
#define DRTM_REGION_TYPE_NORMAL			0
#define DRTM_REGION_TYPE_NORMAL_CACHED		1
#define DRTM_REGION_TYPE_DEVICE			2
#define DRTM_REGION_TYPE_NV			3
#define DRTM_REGION_TYPE_RSVD			4

/*
 * Bit field helpers for drtm_mem_region.size_and_type per
 * DEN0113 v1.2 §3.14, Table 11:
 *   bits [51:0]  : page count
 *   bits [54:52] : region type (3 bits)
 *   bits [56:55] : cacheability (2 bits, valid when type == NORMAL_CACHED)
 *   bits [63:57] : reserved
 */
#define DRTM_MEM_REGION_PAGE_COUNT(x)	((x) & ((1ULL << 52) - 1))
#define DRTM_MEM_REGION_TYPE(x)		(((x) >> 52) & 0x7)
#define DRTM_MEM_REGION_CACHEABILITY(x)	(((x) >> 55) & 0x3)

/* DLME Data Header (DEN0113 v1.2 §3.15, Table 14) — populated by D-CRTM */
struct dlme_data_header {
	__le16	version;
	__le16	this_hdr_size;
	__le32	reserved;
	__le64	dlme_data_size;
	__le64	protected_regions_size;
	__le64	address_map_size;
	__le64	drtm_event_log_size;
	__le64	tcb_hash_table_size;
	__le64	acpi_table_region_size;
	__le64	impl_defined_region_size;
};

#ifdef CONFIG_ARM64_SECURE_LAUNCH
extern unsigned long sl_dlme_region_pa;
extern unsigned long sl_dlme_data_offset;

void slaunch_early_init(void);
void slaunch_setup(void);
void slaunch_exit(void);
void slaunch_measure_post_efi(void);
#else
static inline void slaunch_early_init(void) { }
static inline void slaunch_setup(void) { }
static inline void slaunch_exit(void) { }
static inline void slaunch_measure_post_efi(void) { }
#endif

#endif /* __ASSEMBLY__ */

#endif /* _ASM_ARM64_DRTM_H */
