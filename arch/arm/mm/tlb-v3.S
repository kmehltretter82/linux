/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ARMv3 TLB handling for ARM610.
 *
 * CP15 c5 invalidates the whole unified TLB; c6 purges the 4 KiB entry named
 * by the address.  Whole-TLB invalidation lives in tlbflush.h; these are the
 * range callbacks selected through the processor table.
 */
#include <linux/linkage.h>
#include <linux/init.h>
#include <asm/assembler.h>
#include <asm/asm-offsets.h>
#include <asm/tlbflush.h>
#include "proc-macros.S"

	.align	5
ENTRY(v3_flush_user_tlb_range)
	vma_vm_mm r2, r2
	act_mm	r3
	teq	r2, r3
	movne	pc, lr

ENTRY(v3_flush_kern_tlb_range)
	bic	r0, r0, #0x0ff
	bic	r0, r0, #0xf00
1:	mcr	p15, 0, r0, c6, c0, 0
	add	r0, r0, #PAGE_SZ
	cmp	r0, r1
	blo	1b
	mov	pc, lr
ENDPROC(v3_flush_user_tlb_range)
ENDPROC(v3_flush_kern_tlb_range)
