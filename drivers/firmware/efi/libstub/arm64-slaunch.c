// SPDX-License-Identifier: GPL-2.0
/*
 * ARM64 DRTM Secure Launch — EFI stub component
 *
 * Called from efi_boot_kernel() after ExitBootServices when the kernel
 * command line contains "drtm=on". Stores the DTB PA for recovery after
 * DRTM, builds DRTM_PARAMETERS, and issues DRTM_DYNAMIC_LAUNCH SMC.
 *
 * The SMC does not return on success — D-CRTM measures the kernel image,
 * populates DLME data, and ERETs to sl_entry (in sl_stub.S).
 *
 * Copyright (c) 2025, NVIDIA Corporation. All rights reserved.
 */

#include <linux/efi.h>
#include <asm/drtm.h>
#include <asm/efi.h>
#include <asm/sections.h>

#include "efistub.h"

/* DRTM SMC IDs — duplicated here for EFI stub isolation */
#define SL_DRTM_SMC_FEATURES		0xC4000111UL
#define SL_DRTM_SMC_DYNAMIC_LAUNCH	0xC4000114UL
#define SL_DRTM_NOT_SUPPORTED		(-1L)
#define SL_DRTM_PAGE_SIZE		0x1000
#define SL_DLME_DATA_RESERVE_DEFAULT	(8 * SL_DRTM_PAGE_SIZE) /* 32KB fallback */
#define SL_ROUND_UP_4K(x)		(((x) + SL_DRTM_PAGE_SIZE - 1) & \
					 ~(SL_DRTM_PAGE_SIZE - 1ULL))

/*
 * Preamble<->DLME DTB-PA convention slot (mirrors SL_DLME_DTB_SLOT_OFFSET
 * in arch/arm64/include/asm/drtm.h — keep them in sync).
 */
#define SL_DLME_DTB_SLOT_OFFSET		(-8)

/* Full-range memory protection entry */
#define SL_MEM_PROT_FULL_RANGE		\
	((0x0ULL << 55) | (0x0ULL << 52) | ((1ULL << 52) - 1ULL))

/* From sl_stub.S — accessible via __efistub_ alias in image-vars.h */
extern char sl_entry[];

/*
 * DRTM Parameters (DEN0113 Section 3.3 / Table 4)
 * Struct must be packed — passed directly to TF-A via SMC.
 */
struct sl_drtm_params {
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

/* Memory protection table: header + 1 full-range region */
struct sl_mem_prot {
	u16	revision;
	u16	reserved;
	u32	num_regions;
	u64	start_address;
	u64	size_and_type;
} __packed;

static u64 sl_smc_ret(u64 fn, u64 arg1)
{
	register u64 x0 __asm__("x0") = fn;
	register u64 x1 __asm__("x1") = arg1;
	register u64 x2 __asm__("x2") = 0;
	register u64 x3 __asm__("x3") = 0;

	asm volatile("smc #0"
		: "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
		:
		: "x4", "x5", "x6", "x7", "x8", "x9", "x10",
		  "x11", "x12", "x13", "x14", "x15", "x16", "x17",
		  "memory");
	return x0;
}

static void sl_smc(u64 fn, u64 arg1)
{
	sl_smc_ret(fn, arg1);
}

static inline void sl_dc_cvac(unsigned long addr)
{
	asm volatile("dc cvac, %0" : : "r"(addr) : "memory");
}

/* Read CTR_EL0.DminLine and return cache line size in bytes. */
static inline unsigned int sl_dcache_line_size(void)
{
	u64 ctr;

	asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
	return 4U << ((ctr >> 16) & 0xfU);
}

/*
 * Clean (cvac) a range to PoC at architectural line granularity.
 * EFI stub is the writer here, so cleaning pushes our freshly-written
 * value into DRAM where TF-A can see it from EL3.
 */
static inline void sl_dc_cvac_range(unsigned long start, unsigned long len)
{
	unsigned int line = sl_dcache_line_size();
	unsigned long mask = (unsigned long)line - 1UL;
	unsigned long end = start + len;
	unsigned long addr;

	start &= ~mask;
	for (addr = start; addr < end; addr += line)
		asm volatile("dc cvac, %0" : : "r"(addr) : "memory");
}

/*
 * DLME data reserve size — queried from D-CRTM via DRTM_FEATURES.
 * Set by efi_slaunch_get_dlme_data_size(), used by arm64-stub.c
 * (reserve_size) and efi_slaunch_drtm() (dlme_region_size).
 */
unsigned long sl_dlme_data_reserve = SL_DLME_DATA_RESERVE_DEFAULT;

/*
 * Query DRTM_FEATURES for minimum DLME data size (DEN0113 §3.3, Table 6).
 * Feature ID 0x2: bits [31:0] = min DLME data in 4KB pages.
 * Called from arm64-stub.c before ExitBootServices.
 */
void efi_slaunch_get_dlme_data_size(void)
{
	register u64 x0 __asm__("x0") = SL_DRTM_SMC_FEATURES;
	register u64 x1 __asm__("x1") = (1ULL << 63) | 0x2;
	register u64 x2 __asm__("x2") = 0;
	register u64 x3 __asm__("x3") = 0;
	u32 min_pages;

	/*
	 * Query feature 0x2 (Minimum memory requirement). TF-A's SMC
	 * handler returns SMC_RET2: x0 = "supported" flag (1 on
	 * success, DRTM_NOT_SUPPORTED otherwise); x1 = the actual
	 * min_mem_req bitfield, low 32 bits = min DLME data in 4 KB
	 * pages, high 32 bits = NWd DCE size (unused here).
	 */
	asm volatile("smc #0"
		: "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
		:
		: "x4", "x5", "x6", "x7", "x8", "x9", "x10",
		  "x11", "x12", "x13", "x14", "x15", "x16", "x17",
		  "memory");

	if ((s64)x0 == SL_DRTM_NOT_SUPPORTED) {
		efi_info("DRTM: FEATURES not supported, using default DLME data reserve (%lu KB)\n",
			 sl_dlme_data_reserve / 1024);
		return;
	}

	min_pages = (u32)(x1 & 0xFFFFFFFF);
	if (min_pages == 0) {
		efi_info("DRTM: min DLME data size is 0, using default (%lu KB)\n",
			 sl_dlme_data_reserve / 1024);
		return;
	}

	sl_dlme_data_reserve = (unsigned long)min_pages * SL_DRTM_PAGE_SIZE;
	efi_info("DRTM: min DLME data size from D-CRTM: %lu KB (%u pages)\n",
		 sl_dlme_data_reserve / 1024, min_pages);
}

/*
 * Token-aware cmdline match. Returns true iff `tok` appears in
 * `cmdline` as a standalone whitespace-delimited word — NOT as a
 * substring of another option. Without this, strstr("drtm=on")
 * would match "nodrtm=on" (intended OFF -> reads as ON), or
 * "root=UUID=drtm=on_xxx" (unrelated option value).
 */
static bool sl_cmdline_token(const char *cmdline, const char *tok)
{
	size_t toklen = strlen(tok);
	const char *p = cmdline;

	while ((p = strstr(p, tok)) != NULL) {
		bool start_ok = (p == cmdline) || p[-1] == ' ' || p[-1] == '\t';
		bool end_ok = p[toklen] == '\0' || p[toklen] == ' ' ||
			      p[toklen] == '\t';

		if (start_ok && end_ok)
			return true;
		p += toklen;
	}
	return false;
}

/*
 * Direct PL011 UART emit. Used in the gate's halt path because
 * efi_err / efi_info go to the EFI ConOut which is gone after
 * ExitBootServices (which has already happened by the time we run).
 *
 * FVP PL011 lives at 0x1c090000 — this is the same address as
 * "earlycon=pl011,0x1c090000" on our cmdline. Real silicon will need
 * a platform-defined address; track this as a portability TODO.
 */
static void sl_uart_puts(const char *s)
{
	volatile u32 *uart = (volatile u32 *)0x1c090000;

	while (*s)
		*uart = (u32)(unsigned char)*s++;
}

bool efi_slaunch_enabled(const char *cmdline)
{
	if (!cmdline)
		return false;
	if (!sl_cmdline_token(cmdline, "drtm=on"))
		return false;

	/*
	 * Gate: drtm=on requires efi=noruntime. Without efi=noruntime,
	 * the post-DRTM kernel would have UEFI runtime service pointers
	 * (unmeasured pre-DRTM code) callable — a remote-code-execution
	 * primitive. Refuse to launch.
	 *
	 * Defense-in-depth: slaunch_validate_efi also programmatically
	 * clears EFI_RUNTIME_SERVICES from efi.flags so the kernel
	 * cannot dispatch RT calls even if the cmdline flag is missing.
	 * This gate exists so the operator gets a clear "you typoed
	 * the cmdline" signal rather than a silent capability strip.
	 */
	if (!sl_cmdline_token(cmdline, "efi=noruntime")) {
		sl_uart_puts("\n\nDRTM: refusing to launch.\n"
			     "DRTM: 'drtm=on' requires 'efi=noruntime' on the kernel cmdline.\n"
			     "DRTM: halting.\n\n");
		for (;;)
			asm volatile("wfi");
	}
	return true;
}

/*
 * TF-A requires DRTM parameters and mem prot table to be 4KB-aligned.
 * We're past ExitBootServices so can't allocate — use static buffers.
 */
static struct sl_drtm_params sl_params __aligned(SL_DRTM_PAGE_SIZE);
static struct sl_mem_prot sl_memprot __aligned(SL_DRTM_PAGE_SIZE);

void __noreturn efi_slaunch_drtm(unsigned long kernel_addr,
				 unsigned long fdt_addr)
{
	struct sl_drtm_params *params = &sl_params;
	struct sl_mem_prot *mem_prot = &sl_memprot;
	unsigned long image_size, kernel_memsize;
	unsigned long dlme_data_offset;
	unsigned long sl_entry_offset;

	/*
	 * Store DTB PA just before the DLME data area (at dlme_data_offset - 8).
	 * sl_entry can find it using X0 (region PA) + X1 (data offset) - 8
	 * from D-CRTM. This avoids relying on EFI stub symbol resolution
	 * for kernel .data variables which can fail due to PIC/GOT issues.
	 */

	/* Compute sl_entry offset from kernel image base */
	sl_entry_offset = (unsigned long)sl_entry - (unsigned long)_text;

	/*
	 * DLME region layout:
	 *   [kernel image: _text to _edata (measured)]
	 *   [kernel BSS:   _edata to _end (not measured, zeroed by kernel)]
	 *   [DLME data:    D-CRTM populates after _end]
	 *
	 * FDT is separate — wherever UEFI's efi_allocate_pages() put it.
	 */
	image_size = (unsigned long)(_edata - _text);
	kernel_memsize = (unsigned long)(_end - _text);
	dlme_data_offset = SL_ROUND_UP_4K(kernel_memsize);

	/*
	 * Write DTB PA into the Preamble->DLME convention slot at
	 * (kernel_addr + dlme_data_offset + SL_DLME_DTB_SLOT_OFFSET).
	 * sl_entry reads it via X0 + X1 + SL_DLME_DTB_SLOT_OFFSET after
	 * the D-CRTM ERET. Direct physical write — no symbol resolution.
	 */
	*(volatile u64 *)(kernel_addr + dlme_data_offset +
			  SL_DLME_DTB_SLOT_OFFSET) = fdt_addr;

	/* Memory protection table: full-range DMA protection */
	mem_prot->revision = DRTM_PARAMS_REVISION;
	mem_prot->reserved = 0;
	mem_prot->num_regions = 1;
	mem_prot->start_address = 0;
	mem_prot->size_and_type = SL_MEM_PROT_FULL_RANGE;

	/* Build DRTM_PARAMETERS */
	params->revision = DRTM_PARAMS_REVISION;
	params->reserved = 0;
	params->launch_features = 0; /* full DMA protection */
	params->dlme_region_address = kernel_addr;
	params->dlme_region_size = dlme_data_offset + sl_dlme_data_reserve;
	params->dlme_image_start = 0;
	params->dlme_entry_point_offset = sl_entry_offset;
	params->dlme_image_size = SL_ROUND_UP_4K(image_size);
	params->dlme_data_offset = dlme_data_offset;
	params->nw_dce_region_address = 0;
	params->nw_dce_region_size = 0;
	params->mem_prot_table_address = (u64)mem_prot;
	params->mem_prot_table_size = sizeof(*mem_prot);

	/*
	 * Flush modified data to DRAM — D-CRTM reads from physical memory
	 * (via its own EL3 mappings) after the SMC entry. As the writer
	 * we clean (cvac) what we just wrote so DRAM holds the fresh
	 * values. Walk at architectural cache line size (CTR_EL0.DminLine)
	 * rather than assuming 64 bytes; ARMv8 allows 16/32/64/128.
	 *
	 * Three regions:
	 *   - sl_drtm_params (88 bytes) — DRTM_PARAMETERS passed in X1.
	 *   - sl_mem_prot (24 bytes) — memory protection table.
	 *   - DTB PA convention slot (8 bytes) at kernel_addr +
	 *     dlme_data_offset - 8. This sits in the gap between the
	 *     kernel image extent (cleaned by TF-A as part of DLME image
	 *     measurement) and the DLME data extent (which TF-A
	 *     populates), so it is NOT covered by TF-A's own cache flush.
	 *     Without this clean the post-DRTM sl_entry can read stale
	 *     bytes for the DTB PA.
	 */
	sl_dc_cvac_range((unsigned long)params, sizeof(*params));
	sl_dc_cvac_range((unsigned long)mem_prot, sizeof(*mem_prot));
	sl_dc_cvac_range(kernel_addr + dlme_data_offset +
			 SL_DLME_DTB_SLOT_OFFSET, sizeof(u64));
	asm volatile("dsb sy" : : : "memory");

	/*
	 * DRTM_DYNAMIC_LAUNCH — does not return on success.
	 * D-CRTM: measures kernel, populates DLME data, ERETs to sl_entry
	 */
	sl_smc(SL_DRTM_SMC_DYNAMIC_LAUNCH, (u64)params);

	/* If we reach here, the SMC failed. Halt. */
	for (;;)
		asm volatile("wfi");
}
