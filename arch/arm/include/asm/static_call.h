/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_ARM_STATIC_CALL_H
#define _ASM_ARM_STATIC_CALL_H

/*
 * The trampoline loads its target from a literal that follows it and jumps
 * there.  A load to PC interworks on ARMv5T and later, so this works for a
 * Thumb-2 kernel too, and going through a literal imposes no branch range
 * limit, which a direct branch would: a static call may target a module
 * placed far from the kernel by CONFIG_ARM_MODULE_PLTS.
 * arch_static_call_transform() only has to rewrite the literal at name+4.
 */
#define __ARCH_DEFINE_STATIC_CALL_TRAMP(name, target)			\
	asm(".pushsection .text, \"ax\"\n"				\
	    ".align 2\n"						\
	    ".globl " name "\n"						\
	    name ":\n"							\
	    "ldr pc, 0f\n"						\
	    "0: .word " target "\n"					\
	    ".type " name ", %function\n"				\
	    ".size " name ", . - " name "\n"				\
	    ".popsection\n")

#define ARCH_DEFINE_STATIC_CALL_TRAMP(name, func)			\
	__ARCH_DEFINE_STATIC_CALL_TRAMP(STATIC_CALL_TRAMP_STR(name), #func)

#define ARCH_DEFINE_STATIC_CALL_NULL_TRAMP(name)			\
	__ARCH_DEFINE_STATIC_CALL_TRAMP(STATIC_CALL_TRAMP_STR(name),	\
					"__static_call_return0")

#define ARCH_DEFINE_STATIC_CALL_RET0_TRAMP(name)			\
	ARCH_DEFINE_STATIC_CALL_NULL_TRAMP(name)

#endif /* _ASM_ARM_STATIC_CALL_H */
