/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __IRQSTATE_KUNIT_H
#define __IRQSTATE_KUNIT_H

#if IS_ENABLED(CONFIG_RV_MONITORS_KUNIT_TEST)

#include <linux/rv.h>
#include <rv/kunit.h>

struct irqaction;

extern const struct rv_irqstate_ops {
	struct rv_kunit_mon mon;
	void (*attach)(void);
	void (*detach)(void);
	void (*handle_hardirq_entry)(void *data, int irq,
				     struct irqaction *action);
	void (*handle_hardirq_exit)(void *data, int irq,
				    struct irqaction *action, int ret);
	void (*handle_schedule_entry)(void *data, bool preempt);
	void (*handle_softirq_entry)(void *data, unsigned int vec_nr);
	void (*handle_softirq_exit)(void *data, unsigned int vec_nr);
} rv_irqstate_ops;
#endif

#endif /* __IRQSTATE_KUNIT_H */
