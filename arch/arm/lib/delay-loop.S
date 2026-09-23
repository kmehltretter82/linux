/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  linux/arch/arm/lib/delay.S
 *
 *  Copyright (C) 1995, 1996 Russell King
 */
#include <linux/linkage.h>
#include <linux/cfi_types.h>
#include <asm/assembler.h>
#include <asm/delay.h>

#if defined(CONFIG_ARCH_RPC) && !defined(CONFIG_CPU_32v3)
		.arch	armv4
#endif

		.text

.LC0:		.word	loops_per_jiffy
.LC1:		.word	UDELAY_MULT

/*
 * loops = r0 * HZ * loops_per_jiffy / 1000000
 *
 * r0  <= 2000
 * HZ  <= 1000
 */

SYM_TYPED_FUNC_START(__loop_udelay)
		ldr	r2, .LC1
		mul	r0, r2, r0		@ r0 = delay_us * UDELAY_MULT
		b	__loop_const_udelay
SYM_FUNC_END(__loop_udelay)

SYM_TYPED_FUNC_START(__loop_const_udelay)	@ 0 <= r0 <= 0xfffffaf0
		ldr	r2, .LC0
		ldr	r2, [r2]
#ifdef CONFIG_CPU_32v3
		@ ARMv3 has 32-bit MUL but not the ARMv3M UMULL instruction.
		@ Split both inputs into 16-bit halves and form the same 64-bit
		@ product in r0:r1.  ip holds the initial low word, r1/r3 the
		@ two cross products, and r2 the high-half product.
		mov	r1, r0, lsl #16
		mov	r1, r1, lsr #16
		mov	r0, r0, lsr #16
		mov	r3, r2, lsl #16
		mov	r3, r3, lsr #16
		mov	r2, r2, lsr #16
		mul	ip, r1, r3
		mul	r1, r2, r1
		mul	r3, r0, r3
		mul	r2, r0, r2
		mov	r0, r1, lsr #16
		add	r0, r0, r3, lsr #16
		add	r0, r0, r2
		adds	ip, ip, r1, lsl #16
		adc	r0, r0, #0
		adds	r1, ip, r3, lsl #16
		adc	r0, r0, #0
#else
		umull	r1, r0, r2, r0		@ r0-r1 = r0 * loops_per_jiffy
#endif
		adds	r1, r1, #0xffffffff	@ rounding up ...
		adcs	r0, r0, r0		@ and right shift by 31
		reteq	lr
		b	__loop_delay
SYM_FUNC_END(__loop_const_udelay)

		.align 3

@ Delay routine
SYM_TYPED_FUNC_START(__loop_delay)
		subs	r0, r0, #1
#if 0
		retls	lr
		subs	r0, r0, #1
		retls	lr
		subs	r0, r0, #1
		retls	lr
		subs	r0, r0, #1
		retls	lr
		subs	r0, r0, #1
		retls	lr
		subs	r0, r0, #1
		retls	lr
		subs	r0, r0, #1
		retls	lr
		subs	r0, r0, #1
#endif
		bhi	__loop_delay
		ret	lr
SYM_FUNC_END(__loop_delay)
