// SPDX-License-Identifier: GPL-2.0
#include <linux/memory.h>
#include <linux/static_call.h>

#include <asm/text-patching.h>

void arch_static_call_transform(void *site, void *tramp, void *func, bool tail)
{
	/*
	 * Only the out-of-line trampolines are patched, so site is always
	 * NULL here and tail does not matter.  The literal sits one word
	 * after the single "ldr pc" instruction of the trampoline.
	 */
	if (!func)
		func = &__static_call_return0;

	patch_text_word((void *)((unsigned long)tramp + 4), (u32)(unsigned long)func);
}
EXPORT_SYMBOL_GPL(arch_static_call_transform);
