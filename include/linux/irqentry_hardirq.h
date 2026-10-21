/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_IRQENTRY_HARDIRQ_H
#define __LINUX_IRQENTRY_HARDIRQ_H

#include <linux/preempt.h>

struct irqentry_hardirq_cookie {
#if defined(CONFIG_DEBUG_PREEMPT) && \
	!defined(CONFIG_HAS_SEPARATE_PREEMPT_RESCHED_BITS)
	bool applied;
#endif
};

static __always_inline bool
irqentry_hardirq_count_applied(struct irqentry_hardirq_cookie cookie)
{
#if defined(CONFIG_DEBUG_PREEMPT) && \
	!defined(CONFIG_HAS_SEPARATE_PREEMPT_RESCHED_BITS)
	return cookie.applied;
#else
	return true;
#endif
}

static __always_inline bool irqentry_hardirq_count_valid(void)
{
#ifdef CONFIG_DEBUG_PREEMPT
	/*
	 * Without separate reschedule bits, preempt_count() excludes any
	 * shared reschedule bit. A negative result therefore denotes an
	 * existing underflow.
	 */
	if (!IS_ENABLED(CONFIG_HAS_SEPARATE_PREEMPT_RESCHED_BITS) &&
	    preempt_count() < 0)
		return false;
#endif
	return true;
}

#if defined(CONFIG_DEBUG_PREEMPT) && \
	!defined(CONFIG_HAS_SEPARATE_PREEMPT_RESCHED_BITS)
struct irqentry_hardirq_cookie noinstr
__irqentry_hardirq_count_start_slow(void);
#endif

/*
 * Attempt the non-instrumentable part of a staged hardirq-count addition and
 * record whether it was applied. The returned cookie must be passed to the
 * matching irqentry_hardirq_count_finish() and subtraction preparation. A
 * rejected cookie attempts deferred reporting and suppresses post-add
 * bookkeeping and the matching subtraction regardless of warning
 * suppression.
 */
static __always_inline struct irqentry_hardirq_cookie
irqentry_hardirq_count_start(void)
{
	struct irqentry_hardirq_cookie cookie = { };

#if defined(CONFIG_DEBUG_PREEMPT) && \
	!defined(CONFIG_HAS_SEPARATE_PREEMPT_RESCHED_BITS)
	cookie.applied = true;
	/* DEBUG_LOCKS_WARN_ON() permits the addition during an oops. */
	if (unlikely(!irqentry_hardirq_count_valid()))
		return __irqentry_hardirq_count_start_slow();
#endif
	__preempt_count_add(HARDIRQ_OFFSET);

	return cookie;
}

#if defined(CONFIG_PREEMPTION) && (defined(CONFIG_DEBUG_PREEMPT) || \
					defined(CONFIG_TRACE_PREEMPT_TOGGLE))
void irqentry_hardirq_count_finish(struct irqentry_hardirq_cookie cookie);
bool irqentry_hardirq_count_sub_prepare(void);
#else
static __always_inline void
irqentry_hardirq_count_finish(struct irqentry_hardirq_cookie cookie) { }
static __always_inline bool irqentry_hardirq_count_sub_prepare(void) { return true; }
#endif

#endif /* __LINUX_IRQENTRY_HARDIRQ_H */
