/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __IRQEXIT_KUNIT_H
#define __IRQEXIT_KUNIT_H

#if IS_ENABLED(CONFIG_RV_MONITORS_KUNIT_TEST)

#include <linux/rv.h>
#include <rv/kunit.h>

extern const struct rv_irqexit_ops {
	struct rv_kunit_mon mon;
	void (*handle_resched_entry)(void *data, struct task_struct *owner);
	void (*handle_resched_exit)(void *data, struct task_struct *owner);
	void (*handle_sched_switch)(void *data, bool preempt,
				    struct task_struct *prev,
				    struct task_struct *next,
				    unsigned int prev_state);
} rv_irqexit_ops;
#endif

#endif /* __IRQEXIT_KUNIT_H */
