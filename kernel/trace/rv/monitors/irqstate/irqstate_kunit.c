// SPDX-License-Identifier: GPL-2.0
#include <linux/hardirq.h>
#include <linux/interrupt.h>
#include <linux/irqflags.h>
#include <linux/kernel.h>
#include <linux/rv.h>
#include <rv/kunit.h>

#include "irqstate_kunit.h"

#if IS_REACHABLE(CONFIG_RV_MON_IRQSTATE)

static void detach_irqstate_test_probes(void *unused)
{
	rv_irqstate_ops.detach();
}

static void rv_test_irqstate(struct kunit *test)
{
	struct rv_kunit_ctx *ctx = test->priv;
	struct irqaction action = {
		.name = "rv-kunit",
	};
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

	/* Generic NMI accounting also carries a hardirq component. */
	local_irq_save(flags);
	__nmi_enter();
	rv_irqstate_ops.handle_hardirq_entry(NULL, 0, NULL);
	rv_irqstate_ops.handle_hardirq_exit(NULL, 0, NULL, 0);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
	__nmi_exit();
	local_irq_restore(flags);

	/* SOFTIRQ_OFFSET denotes execution, unlike BH-disable nesting. */
	__local_bh_disable_ip(_THIS_IP_, SOFTIRQ_OFFSET);
	rv_irqstate_ops.handle_softirq_entry(NULL, 0);
	rv_irqstate_ops.handle_softirq_exit(NULL, 0);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
	__local_bh_enable_ip(_THIS_IP_, SOFTIRQ_OFFSET);

	/* Exercise the actual generic tracepoint attachments as well. */
	rv_irqstate_ops.attach();
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, detach_irqstate_test_probes,
						  NULL));

	trace_sched_entry_tp(false);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);

	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		trace_irq_handler_entry(0, &action);

	local_irq_save(flags);
	__irq_enter_raw();
	trace_irq_handler_entry(0, &action);
	trace_irq_handler_exit(0, &action, IRQ_HANDLED);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
	__irq_exit_raw();
	local_irq_restore(flags);

	__local_bh_disable_ip(_THIS_IP_, SOFTIRQ_OFFSET);
	trace_softirq_entry(0);
	trace_softirq_exit(0);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
	__local_bh_enable_ip(_THIS_IP_, SOFTIRQ_OFFSET);
}

#else
#define rv_test_irqstate rv_test_stub
#endif
