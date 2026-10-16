// SPDX-License-Identifier: GPL-2.0
#include <linux/completion.h>
#include <linux/hardirq.h>
#include <linux/interrupt.h>
#include <linux/irqflags.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/rv.h>
#include <rv/kunit.h>
#include <trace/events/irq.h>
#include <trace/events/sched.h>

#include "irqstate_kunit.h"

#if IS_REACHABLE(CONFIG_RV_MON_IRQSTATE)

struct irqstate_softirq_ctx {
	struct tasklet_struct tasklet;
	struct completion done;
};

static void run_irqstate_softirq(struct tasklet_struct *tasklet)
{
	struct irqstate_softirq_ctx *softirq_ctx;

	softirq_ctx = from_tasklet(softirq_ctx, tasklet, tasklet);
	rv_irqstate_ops.handle_softirq_entry(NULL, 0);
	rv_irqstate_ops.handle_softirq_exit(NULL, 0);
	complete(&softirq_ctx->done);
}

static bool exercise_irqstate_softirq(void)
{
	struct irqstate_softirq_ctx softirq_ctx;
	unsigned long completed;

	init_completion(&softirq_ctx.done);
	tasklet_setup(&softirq_ctx.tasklet, run_irqstate_softirq);
	tasklet_schedule(&softirq_ctx.tasklet);
	completed = wait_for_completion_timeout(&softirq_ctx.done, HZ);
	tasklet_kill(&softirq_ctx.tasklet);

	return completed != 0;
}

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

	/* Disabling BH does not mean that a softirq is being served. */
	local_bh_disable();
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqstate_ops.handle_softirq_entry(NULL, 0);
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqstate_ops.handle_softirq_exit(NULL, 0);
	local_bh_enable();

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

	/* Run the handlers from a real serving-softirq context. */
	KUNIT_ASSERT_TRUE(test, exercise_irqstate_softirq());
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);

	/* Exercise the actual generic tracepoint attachments as well. */
	rv_irqstate_ops.attach();
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, detach_irqstate_test_probes,
						  NULL));

	trace_sched_entry_tp(false);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);

	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		trace_irq_handler_entry(0, &action);

	local_bh_disable();
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		trace_softirq_entry(0);
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		trace_softirq_exit(0);
	local_bh_enable();

	local_irq_save(flags);
	__irq_enter_raw();
	trace_irq_handler_entry(0, &action);
	trace_irq_handler_exit(0, &action, IRQ_HANDLED);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
	__irq_exit_raw();
	local_irq_restore(flags);

	/* This also exercises the attached generic softirq tracepoints. */
	KUNIT_ASSERT_TRUE(test, exercise_irqstate_softirq());
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
}

#else
#define rv_test_irqstate rv_test_stub
#endif
