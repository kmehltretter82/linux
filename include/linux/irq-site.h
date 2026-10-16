/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_IRQ_SITE_H
#define _LINUX_IRQ_SITE_H

#include <linux/compiler_types.h>

/* Version-one values shared with the host-side IRQ-site decoder. */
enum linux_irq_site_kind {
	LINUX_IRQ_SITE_SOFTIRQ_ENTRY = 10,
	LINUX_IRQ_SITE_SOFTIRQ_EXIT = 11,
};

/*
 * The build controller must define __RISCV_LINUX_IRQ_SITE_MAP__ only while
 * loading the matching Clang map plugin.  Normal builds compile these markers
 * away before their arguments can affect generated code.
 */
#ifdef __RISCV_LINUX_IRQ_SITE_MAP__
#if !defined(CONFIG_CC_HAS_RISCV_LINUX_IRQ_SITE) || \
	!__has_builtin(__builtin_riscv_linux_irq_site)
#error "RISC-V Linux IRQ site map requested without compiler builtin support"
#endif
#define linux_irq_site(kind, ordinal) \
	__builtin_riscv_linux_irq_site((kind), (ordinal))
#else
#define linux_irq_site(...) do { } while (0)
#endif

#endif /* _LINUX_IRQ_SITE_H */
