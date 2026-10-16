// SPDX-License-Identifier: GPL-2.0
#include <linux/hardirq.h>
#include <linux/irqflags.h>
#include <linux/kernel.h>
#include <linux/rv.h>
#include <rv/kunit.h>

#include "irqstate_kunit.h"

#if IS_REACHABLE(CONFIG_RV_MON_IRQSTATE)

static void rv_test_irqstate(struct kunit *test)
{
	struct rv_kunit_ctx *ctx = test->priv;
	unsigned long flags;

	prepare_test(test, &rv_irqstate_ops.mon);
	guard(migrate)();

	/* Scheduling from task context is valid. */
	rv_irqstate_ops.handle_schedule_entry(NULL, false);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);

	/* IRQ action callbacks without hardirq context are invalid. */
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqstate_ops.handle_hardirq_entry(NULL, 0, NULL);
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqstate_ops.handle_hardirq_exit(NULL, 0, NULL, 0);

	/* Softirq action callbacks without serving-softirq state are invalid. */
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqstate_ops.handle_softirq_entry(NULL, 0);
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqstate_ops.handle_softirq_exit(NULL, 0);

	local_irq_save(flags);
	__irq_enter_raw();

	/* Both IRQ action boundaries observe established hardirq context. */
	rv_irqstate_ops.handle_hardirq_entry(NULL, 0, NULL);
	rv_irqstate_ops.handle_hardirq_exit(NULL, 0, NULL, 0);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);

	/* Entering the scheduler while hardirq context is active is invalid. */
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqstate_ops.handle_schedule_entry(NULL, true);

	__irq_exit_raw();
	local_irq_restore(flags);

	/* SOFTIRQ_OFFSET denotes execution, unlike BH-disable nesting. */
	__local_bh_disable_ip(_THIS_IP_, SOFTIRQ_OFFSET);
	rv_irqstate_ops.handle_softirq_entry(NULL, 0);
	rv_irqstate_ops.handle_softirq_exit(NULL, 0);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
	__local_bh_enable_ip(_THIS_IP_, SOFTIRQ_OFFSET);
}

#else
#define rv_test_irqstate rv_test_stub
#endif
