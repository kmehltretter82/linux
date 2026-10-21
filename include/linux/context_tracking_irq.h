/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CONTEXT_TRACKING_IRQ_H
#define _LINUX_CONTEXT_TRACKING_IRQ_H

#include <linux/types.h>

/**
 * struct ct_irq_exit_state - Opaque context-tracking IRQ-exit state
 * @exit_rcu: The interrupted context was in an RCU extended quiescent state
 *
 * This object is filled in by ct_irq_exit_prepare() and must be passed to the
 * matching ct_irq_exit_finish(). Callers must consider its members private.
 */
struct ct_irq_exit_state {
	bool exit_rcu;
};

#ifdef CONFIG_CONTEXT_TRACKING_IDLE
void ct_irq_enter(void);
struct ct_irq_exit_state ct_irq_exit_prepare(void);
void ct_irq_exit_finish(struct ct_irq_exit_state state);
void ct_irq_exit(void);
void ct_irq_enter_irqson(void);
void ct_irq_exit_irqson(void);
void ct_nmi_enter(void);
void ct_nmi_exit(void);
#else
static __always_inline void ct_irq_enter(void) { }
static __always_inline struct ct_irq_exit_state ct_irq_exit_prepare(void)
{
	return (struct ct_irq_exit_state) { };
}

static __always_inline void ct_irq_exit_finish(struct ct_irq_exit_state state)
{
	(void)state;
}

static __always_inline void ct_irq_exit(void)
{
	struct ct_irq_exit_state state = ct_irq_exit_prepare();

	ct_irq_exit_finish(state);
}
static inline void ct_irq_enter_irqson(void) { }
static inline void ct_irq_exit_irqson(void) { }
static __always_inline void ct_nmi_enter(void) { }
static __always_inline void ct_nmi_exit(void) { }
#endif

#endif
