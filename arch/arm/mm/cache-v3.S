/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Cache maintenance for the ARM610's ARMv3 CP15 layout.
 *
 * The ARM610 has a unified VIVT cache.  Its full cache operation is CP15
 * c7, c0, unlike the split-cache operations used by ARMv4 processors.
 */
#include <linux/linkage.h>
#include <linux/init.h>
#include <asm/assembler.h>
#include <asm/page.h>
#include "proc-macros.S"

/* The ARM610 has no separate I-cache invalidation operation. */
ENTRY(v3_flush_icache_all)
	mov	pc, lr
ENDPROC(v3_flush_icache_all)

SYM_FUNC_ALIAS(v3_flush_user_cache_all, v3_flush_kern_cache_all)

/* Clean and invalidate the complete unified cache. */
ENTRY(v3_flush_kern_cache_all)
ENTRY(v3_flush_user_cache_range)
	mov	ip, #0
	mcr	p15, 0, ip, c7, c0, 0
	mov	pc, lr
ENDPROC(v3_flush_kern_cache_all)
ENDPROC(v3_flush_user_cache_range)

/* The unified cache does not need separate I/D coherency work. */
ENTRY(v3_coherent_kern_range)
ENTRY(v3_coherent_user_range)
	mov	r0, #0
	mov	pc, lr
ENDPROC(v3_coherent_kern_range)
ENDPROC(v3_coherent_user_range)

/*
 * The architecture has no range operation.  A complete cache flush is the
 * conservative implementation for D-cache cleaning and DMA coherency.
 */
ENTRY(v3_flush_kern_dcache_area)
	b	v3_dma_flush_range
ENDPROC(v3_flush_kern_dcache_area)

ENTRY(v3_dma_flush_range)
	mov	r0, #0
	mcr	p15, 0, r0, c7, c0, 0
	mov	pc, lr
ENDPROC(v3_dma_flush_range)

ENTRY(v3_dma_unmap_area)
	teq	r2, #DMA_TO_DEVICE
	bne	v3_dma_flush_range
	mov	pc, lr
ENDPROC(v3_dma_unmap_area)

ENTRY(v3_dma_map_area)
	mov	pc, lr
ENDPROC(v3_dma_map_area)
