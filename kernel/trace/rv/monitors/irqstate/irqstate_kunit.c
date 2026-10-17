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

#include "irqstate_kunit.h"

#if IS_REACHABLE(CONFIG_RV_MON_IRQSTATE)

enum irqstate_softirq_test {
	IRQSTATE_SOFTIRQ_VALID,
	IRQSTATE_SOFTIRQ_INTERRUPTED,
	IRQSTATE_SOFTIRQ_DOUBLE_ENTRY,
	IRQSTATE_SOFTIRQ_DOUBLE_EXIT,
	IRQSTATE_SOFTIRQ_TRACEPOINT_ONLY,
};

struct irqstate_softirq_ctx {
	struct tasklet_struct tasklet;
	struct completion done;
	enum irqstate_softirq_test test;
};

static void run_irqstate_softirq(struct tasklet_struct *tasklet)
{
	struct irqstate_softirq_ctx *softirq_ctx;
	unsigned long flags;

	softirq_ctx = from_tasklet(softirq_ctx, tasklet, tasklet);

	if (softirq_ctx->test == IRQSTATE_SOFTIRQ_TRACEPOINT_ONLY)
		goto out;

	rv_irqstate_ops.handle_softirq_entry(NULL, true);

	switch (softirq_ctx->test) {
	case IRQSTATE_SOFTIRQ_INTERRUPTED:
		local_irq_save(flags);
		__irq_enter_raw();
		rv_irqstate_ops.handle_hardirq_entry(NULL, 1);
		rv_irqstate_ops.handle_hardirq_exit(NULL, 1);
		__irq_exit_raw();
		local_irq_restore(flags);
		break;
	case IRQSTATE_SOFTIRQ_DOUBLE_ENTRY:
		rv_irqstate_ops.handle_softirq_entry(NULL, true);
		goto out;
	case IRQSTATE_SOFTIRQ_DOUBLE_EXIT:
		rv_irqstate_ops.handle_softirq_exit(NULL, true);
		break;
	case IRQSTATE_SOFTIRQ_VALID:
		break;
	case IRQSTATE_SOFTIRQ_TRACEPOINT_ONLY:
		unreachable();
	}

	rv_irqstate_ops.handle_softirq_exit(NULL, true);
out:
	complete(&softirq_ctx->done);
}

static bool exercise_irqstate_softirq(enum irqstate_softirq_test test)
{
	struct irqstate_softirq_ctx softirq_ctx = {
		.test = test,
	};
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
	/* Per-CPU monitor, make sure we do not change CPU mid-test. */
	guard(migrate)();

	/* Context-boundary events must agree with the live context state. */
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqstate_ops.handle_hardirq_entry(NULL, 0);
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqstate_ops.handle_hardirq_exit(NULL, 0);
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqstate_ops.handle_softirq_entry(NULL, false);
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqstate_ops.handle_softirq_exit(NULL, false);

	/* Disabling BH does not mean that a softirq is being served. */
	local_bh_disable();
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqstate_ops.handle_softirq_entry(NULL, false);
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqstate_ops.handle_softirq_exit(NULL, false);
	local_bh_enable();

	/* Nested hardirqs leave the outer hardirq automaton state unchanged. */
	local_irq_save(flags);
	__irq_enter_raw();
	rv_irqstate_ops.handle_hardirq_entry(NULL, 1);
	__irq_enter_raw();
	rv_irqstate_ops.handle_hardirq_entry(NULL, 2);
	rv_irqstate_ops.handle_hardirq_exit(NULL, 2);
	__irq_exit_raw();
	rv_irqstate_ops.handle_hardirq_exit(NULL, 1);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
	__irq_exit_raw();
	local_irq_restore(flags);

	/* A second outer entry is not a nested interrupt. */
	local_irq_save(flags);
	__irq_enter_raw();
	rv_irqstate_ops.handle_hardirq_entry(NULL, 1);
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqstate_ops.handle_hardirq_entry(NULL, 1);
	rv_irqstate_ops.handle_hardirq_exit(NULL, 1);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
	__irq_exit_raw();
	local_irq_restore(flags);

	/* An exit without its corresponding entry is invalid after sync. */
	local_irq_save(flags);
	__irq_enter_raw();
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqstate_ops.handle_hardirq_exit(NULL, 1);
	__irq_exit_raw();
	local_irq_restore(flags);

	/* Exercise valid and deliberately invalid serving-softirq sequences. */
	KUNIT_ASSERT_TRUE(test, exercise_irqstate_softirq(IRQSTATE_SOFTIRQ_VALID));
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
	KUNIT_ASSERT_TRUE(test,
			  exercise_irqstate_softirq(IRQSTATE_SOFTIRQ_INTERRUPTED));
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
	KUNIT_ASSERT_TRUE(test,
			  exercise_irqstate_softirq(IRQSTATE_SOFTIRQ_DOUBLE_ENTRY));
	RV_KUNIT_EXPECT_REACTION(test, ctx);
	KUNIT_ASSERT_TRUE(test,
			  exercise_irqstate_softirq(IRQSTATE_SOFTIRQ_DOUBLE_EXIT));
	RV_KUNIT_EXPECT_REACTION(test, ctx);

	/* Exercise the actual generic tracepoint attachments as well. */
	rv_irqstate_ops.attach();
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, detach_irqstate_test_probes,
						  NULL));

	/* IRQ action events no longer drive the context-boundary monitor. */
	trace_irq_handler_entry(0, &action);
	trace_irq_handler_exit(0, &action, IRQ_HANDLED);
	trace_softirq_entry(0);
	trace_softirq_exit(0);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);

	/* Manually invalid boundary events prove that all probes are attached. */
	local_irq_save(flags);
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		trace_hardirq_context_entry(0);
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		trace_hardirq_context_exit(0);
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		trace_softirq_context_entry(false);
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		trace_softirq_context_exit(false);
	local_irq_restore(flags);

	/* These helpers emit valid hardirq context-boundary tracepoints. */
	local_irq_save(flags);
	irq_enter_rcu();
	irq_exit_rcu();
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
	local_irq_restore(flags);

	/* This tasklet relies only on the live softirq context tracepoints. */
	KUNIT_ASSERT_TRUE(test,
			  exercise_irqstate_softirq(IRQSTATE_SOFTIRQ_TRACEPOINT_ONLY));
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
}

#else
#define rv_test_irqstate rv_test_stub
#endif
