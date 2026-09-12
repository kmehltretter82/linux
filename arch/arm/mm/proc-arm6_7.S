/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Low-level ARM610 support, restored from the final upstream ARMv3 path.
 *
 * The ARM610 predates ARMv4: CP15 c5 invalidates the unified TLB and c6
 * purges one 4 KiB entry, while data aborts need the ARMv3 LDM writeback
 * fixup below.
 */
#include <linux/linkage.h>
#include <linux/init.h>
#include <linux/pgtable.h>
#include <asm/assembler.h>
#include <asm/asm-offsets.h>
#include <asm/hwcap.h>
#include <asm/pgtable-hwdef.h>
#include <asm/pgtable.h>
#include <asm/ptrace.h>

#include "proc-macros.S"

	.text

ENTRY(cpu_arm6_dcache_clean_area)
	mov	pc, lr
ENDPROC(cpu_arm6_dcache_clean_area)

/*
 * ARM610 data aborts can leave the base register advanced for a faulting
 * LDM/STM with writeback.  Undo that update before handing the fault to the
 * common MM code.  The entry ABI supplies r2 = pt_regs and r4 = fault PC.
 */
ENTRY(cpu_arm6_data_abort)
	mrc	p15, 0, r1, c5, c0, 0		@ fault status
	mrc	p15, 0, r0, c6, c0, 0		@ fault address
	ldr	r8, [r4]				@ aborted instruction
	tst	r8, #1 << 20			@ L = 0 -> write?
	orreq	r1, r1, #1 << 11
	and	r7, r8, #14 << 24
	teq	r7, #8 << 24			@ LDM/STM?
	bne	do_DataAbort

.data_arm_ldmstm:
	tst	r8, #1 << 21			@ writeback?
	beq	do_DataAbort
	mov	r7, #0x11
	orr	r7, r7, #0x1100
	and	r6, r8, r7
	and	r9, r8, r7, lsl #1
	add	r6, r6, r9, lsr #1
	and	r9, r8, r7, lsl #2
	add	r6, r6, r9, lsr #2
	and	r9, r8, r7, lsl #3
	add	r6, r6, r9, lsr #3
	add	r6, r6, r6, lsr #8
	add	r6, r6, r6, lsr #4
	and	r6, r6, #15			@ number of transferred registers
	and	r9, r8, #15 << 16		@ Rn
	ldr	r7, [r2, r9, lsr #14]
	tst	r8, #1 << 23			@ U bit
	subne	r7, r7, r6, lsl #2
	addeq	r7, r7, r6, lsl #2
	str	r7, [r2, r9, lsr #14]
	b	do_DataAbort

.data_arm_apply_r6_and_rn:
	and	r9, r8, #15 << 16		@ Rn
	ldr	r7, [r2, r9, lsr #14]
	tst	r8, #1 << 23			@ U bit
	subne	r7, r7, r6
	addeq	r7, r7, r6
	str	r7, [r2, r9, lsr #14]
	b	do_DataAbort

.data_arm_lateldrpreconst:
	tst	r8, #1 << 21			@ writeback?
	beq	do_DataAbort
.data_arm_lateldrpostconst:
	movs	r6, r8, lsl #20		@ immediate offset
	beq	do_DataAbort
	and	r9, r8, #15 << 16		@ Rn
	ldr	r7, [r2, r9, lsr #14]
	tst	r8, #1 << 23			@ U bit
	subne	r7, r7, r6, lsr #20
	addeq	r7, r7, r6, lsr #20
	str	r7, [r2, r9, lsr #14]
	b	do_DataAbort

.data_arm_lateldrprereg:
	tst	r8, #1 << 21			@ writeback?
	beq	do_DataAbort
.data_arm_lateldrpostreg:
	and	r7, r8, #15			@ Rm
	ldr	r6, [r2, r7, lsl #2]
	mov	r9, r8, lsr #7
	ands	r9, r9, #31
	and	r7, r8, #0x70
	orreq	r7, r7, #8			@ shift count = 0
	add	pc, pc, r7
	nop

	mov	r6, r6, lsl r9			@ LSL #!0
	b	.data_arm_apply_r6_and_rn
	b	.data_arm_apply_r6_and_rn	@ LSL #0
	nop
	b	.data_unknown			@ MUL
	nop
	b	.data_unknown			@ MUL
	nop
	mov	r6, r6, lsr r9			@ LSR #!0
	b	.data_arm_apply_r6_and_rn
	mov	r6, r6, lsr #32			@ LSR #32
	b	.data_arm_apply_r6_and_rn
	b	.data_unknown			@ MUL
	nop
	b	.data_unknown			@ MUL
	nop
	mov	r6, r6, asr r9			@ ASR #!0
	b	.data_arm_apply_r6_and_rn
	mov	r6, r6, asr #32			@ ASR #32
	b	.data_arm_apply_r6_and_rn
	b	.data_unknown			@ MUL
	nop
	b	.data_unknown			@ MUL
	nop
	mov	r6, r6, ror r9			@ ROR #!0
	b	.data_arm_apply_r6_and_rn
	mov	r6, r6, rrx			@ RRX
	b	.data_arm_apply_r6_and_rn
	b	.data_unknown			@ MUL
	nop
	b	.data_unknown			@ MUL

.data_unknown:
	mov	r0, r4
	mov	r1, r8
	b	baddataabort
ENDPROC(cpu_arm6_data_abort)

ENTRY(cpu_arm6_proc_init)
	mov	pc, lr
ENDPROC(cpu_arm6_proc_init)

ENTRY(cpu_arm6_proc_fin)
	mov	r0, #0x31			@ ....S..DP...M
	mcr	p15, 0, r0, c1, c0, 0
	mov	pc, lr
ENDPROC(cpu_arm6_proc_fin)

ENTRY(cpu_arm6_do_idle)
	mov	pc, lr
ENDPROC(cpu_arm6_do_idle)

/* Switch page tables and clear the ARM610's unified cache and TLB. */
ENTRY(cpu_arm6_switch_mm)
#ifdef CONFIG_MMU
	mov	r1, #0
	mcr	p15, 0, r1, c7, c0, 0
	mcr	p15, 0, r0, c2, c0, 0
	mcr	p15, 0, r1, c5, c0, 0
#endif
	mov	pc, lr
ENDPROC(cpu_arm6_switch_mm)

ENTRY(cpu_arm6_set_pte_ext)
#ifdef CONFIG_MMU
	armv3_set_pte_ext wc_disable=0
#endif
	mov	pc, lr
ENDPROC(cpu_arm6_set_pte_ext)

	.pushsection	.idmap.text, "ax"
ENTRY(cpu_arm6_reset)
	mov	r1, #0
	mcr	p15, 0, r1, c7, c0, 0
#ifdef CONFIG_MMU
	mcr	p15, 0, r1, c5, c0, 0
#endif
	mov	r1, #0x30
	mcr	p15, 0, r1, c1, c0, 0
	mov	pc, r0
ENDPROC(cpu_arm6_reset)
	.popsection

	.type	__arm6_setup, #function
__arm6_setup:
	mov	r0, #0
	mcr	p15, 0, r0, c7, c0
#ifdef CONFIG_MMU
	mcr	p15, 0, r0, c5, c0
	mov	r0, #0x3d			@ . ..RS BLDP WCAM
	orr	r0, r0, #0x100		@ . ..01 0011 1101
#else
	mov	r0, #0x3c			@ . ..RS BLDP WCA.
#endif
	mov	pc, lr
	.size	__arm6_setup, . - __arm6_setup

	__INITDATA

	define_processor_functions arm6, dabort=cpu_arm6_data_abort, pabort=legacy_pabort

	.section ".rodata"

	string	cpu_arch_name, "armv3"
	string	cpu_elf_name, "v3"
	string	cpu_arm610_name, "ARM610"

	.align

	.section ".proc.info.init", "a"

	.type	__arm610_proc_info, #object
__arm610_proc_info:
	.long	0x41560610
	.long	0xfffffff0
	.long	0x00000c1e
	.long	PMD_TYPE_SECT | \
		PMD_BIT4 | \
		PMD_SECT_AP_WRITE | \
		PMD_SECT_AP_READ
	initfn	__arm6_setup, __arm610_proc_info
	.long	cpu_arch_name
	.long	cpu_elf_name
	.long	HWCAP_SWP | HWCAP_26BIT
	.long	cpu_arm610_name
	.long	arm6_processor_functions
	.long	v3_tlb_fns
	.long	v3_user_fns
	.long	v3_cache_fns
	.size	__arm610_proc_info, . - __arm610_proc_info
