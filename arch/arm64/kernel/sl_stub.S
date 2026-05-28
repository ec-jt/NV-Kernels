/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ARM64 DRTM Secure Launch entry stub.
 *
 * DLME entry point after DRTM dynamic launch. D-CRTM (TF-A) measures
 * the kernel image and ERETs here per DEN0113 v1.2 §4.6.1 Table 33
 * (boot PE requirements), with:
 *   X0 = DLME region physical address
 *   X1 = DLME data offset within region
 *   X2-X30 = 0
 *   MMU OFF, D-cache OFF, I-cache ON or OFF, EL2, DAIF masked
 *
 * The stub saves DRTM state, recovers the DTB pointer (written by
 * the EFI stub at region_base + data_offset - 8 before the DRTM
 * SMC), and branches to primary_entry for normal kernel boot.
 */

#include <linux/linkage.h>
#include <asm/assembler.h>
#include <asm/drtm.h>

	.section ".idmap.text","a"

/*
 * sl_entry — DRTM dynamic launch entry point
 *
 * Only reached via DRTM ERET (entry_point_offset in DRTM_PARAMETERS
 * points here). Never reached during normal boot.
 */
SYM_CODE_START(sl_entry)
	/*
	 * Save DLME region PA for slaunch_setup(). MMU is off, D-cache
	 * is off per DEN0113 v1.2 §4.6.1 Table 33 — the str goes
	 * directly to DRAM. The cache line covering this address may
	 * still hold stale (pre-DRTM) data though, and would be returned
	 * to the kernel once it enables caches. dc ivac invalidates the
	 * cache line so the kernel's first read misses cache and pulls
	 * the freshly-written value from DRAM.
	 *
	 * Invalidate (ivac) rather than clean (cvac): cleaning would
	 * push the stale cached value back to DRAM and overwrite the
	 * str just made. The same reasoning applies whenever the kernel
	 * post-MMU reads a location that TF-A or this stub wrote with
	 * caches off.
	 */
	adr_l	x9, sl_dlme_region_pa
	str	x0, [x9]
	dc	ivac, x9

	/* Same for DLME data offset. */
	adr_l	x9, sl_dlme_data_offset
	str	x1, [x9]
	dc	ivac, x9
	dsb	sy

	/*
	 * Load DTB PA from the Preamble->DLME convention slot at
	 * (region_base + data_offset + SL_DLME_DTB_SLOT_OFFSET).
	 * X0 = DLME region PA, X1 = DLME data offset (still live).
	 */
	add	x9, x0, x1
	ldr	x0, [x9, #SL_DLME_DTB_SLOT_OFFSET]	/* X0 = DTB PA */

	/* Clear X1-X3 per arm64 boot protocol */
	mov	x1, xzr
	mov	x2, xzr
	mov	x3, xzr

	/* Enter normal kernel boot with X0 = DTB PA */
	b	primary_entry
SYM_CODE_END(sl_entry)

	.section .data
	.align 3
SYM_DATA(sl_dlme_region_pa,	.quad 0)
SYM_DATA(sl_dlme_data_offset,	.quad 0)
