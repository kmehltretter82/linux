// SPDX-License-Identifier: GPL-2.0
#include <linux/completion.h>
#include <linux/hardirq.h>
#include <linux/interrupt.h>
#include <linux/irqflags.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/rv.h>
#include <linux/sched.h>
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
	IRQSTATE_SOFTIRQ_SYNC,
	IRQSTATE_SOFTIRQ_SYNC_INTERRUPTED,
};

struct irqstate_softirq_ctx {
	struct tasklet_struct tasklet;
	struct completion done;
	enum irqstate_softirq_test test;
	int reactions_after_sync;
};

static void run_irqstate_softirq(struct tasklet_struct *tasklet)
{
	struct irqstate_softirq_ctx *softirq_ctx;
	unsigned long flags;

	softirq_ctx = from_tasklet(softirq_ctx, tasklet, tasklet);

	if (softirq_ctx->test == IRQSTATE_SOFTIRQ_SYNC ||
	    softirq_ctx->test == IRQSTATE_SOFTIRQ_SYNC_INTERRUPTED) {
		local_irq_save(flags);
		rv_irqstate_ops.mon.rv_this->enabled = 0;
		rv_irqstate_ops.handle_softirq_entry(NULL, true);
		rv_irqstate_ops.mon.rv_this->reset();
		rv_irqstate_ops.mon.rv_this->enabled = 1;
		if (softirq_ctx->test == IRQSTATE_SOFTIRQ_SYNC_INTERRUPTED) {
			__irq_enter_raw();
			rv_irqstate_ops.handle_hardirq_entry(NULL, 1);
			rv_irqstate_ops.handle_hardirq_exit(NULL, 1);
			__irq_exit_raw();
		}
		/* The enclosing softirq exit establishes idle without an entry. */
		rv_irqstate_ops.handle_softirq_exit(NULL, true);
		softirq_ctx->reactions_after_sync = active_ctx->reactions;
		/* Once synchronized, an unmatched hardirq exit must be rejected. */
		__irq_enter_raw();
		rv_irqstate_ops.handle_hardirq_exit(NULL, 1);
		__irq_exit_raw();
		local_irq_restore(flags);
		goto out;
	}

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
	case IRQSTATE_SOFTIRQ_SYNC:
	case IRQSTATE_SOFTIRQ_SYNC_INTERRUPTED:
		unreachable();
	}

	rv_irqstate_ops.handle_softirq_exit(NULL, true);
out:
	complete(&softirq_ctx->done);
}

static bool run_irqstate_softirq_case(enum irqstate_softirq_test test, int *sync_rx)
{
	struct irqstate_softirq_ctx softirq_ctx = {
		.test = test,
		.reactions_after_sync = -1,
	};
	unsigned long completed;

	init_completion(&softirq_ctx.done);
	tasklet_setup(&softirq_ctx.tasklet, run_irqstate_softirq);
	tasklet_schedule(&softirq_ctx.tasklet);
	completed = wait_for_completion_timeout(&softirq_ctx.done, HZ);
	tasklet_kill(&softirq_ctx.tasklet);
	if (sync_rx)
		*sync_rx = softirq_ctx.reactions_after_sync;

	return completed != 0;
}

static bool exercise_irqstate_softirq(enum irqstate_softirq_test test)
{
	return run_irqstate_softirq_case(test, NULL);
}

static void attach_irqstate_test_probes(void)
{
	/* Match RV core: do not process an incomplete set of probes. */
	rv_irqstate_ops.mon.rv_this->enabled = 0;
	rv_irqstate_ops.attach();
	rv_irqstate_ops.mon.rv_this->reset();
	rv_irqstate_ops.mon.rv_this->enabled = 1;
}

static void detach_irqstate_test_probes(void *unused)
{
	rv_irqstate_ops.mon.rv_this->enabled = 0;
	rv_irqstate_ops.detach();
	tracepoint_synchronize_unregister();
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
	attach_irqstate_test_probes();
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

static void rv_test_irqstate_depth(struct kunit *test)
{
	struct rv_kunit_ctx *ctx = test->priv;
	unsigned int max_depth = HARDIRQ_MASK >> HARDIRQ_SHIFT;
	unsigned int observed_depth, saved_count;
	unsigned long flags;

	prepare_test(test, &rv_irqstate_ops.mon);
	guard(migrate)();
	saved_count = preempt_count();
	local_irq_save(flags);
	for (unsigned int depth = 1; depth <= max_depth; depth++) {
		__irq_enter_raw();
		rv_irqstate_ops.handle_hardirq_entry(NULL, depth);
	}
	observed_depth = hardirq_count() >> HARDIRQ_SHIFT;
	for (unsigned int depth = max_depth; depth; depth--) {
		rv_irqstate_ops.handle_hardirq_exit(NULL, depth);
		__irq_exit_raw();
	}
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
	/* A silent adapter cannot pass: the completed outer pair armed idle. */
	__irq_enter_raw();
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqstate_ops.handle_hardirq_exit(NULL, 1);
	__irq_exit_raw();
	local_irq_restore(flags);
	KUNIT_EXPECT_EQ(test, observed_depth, max_depth);
	KUNIT_EXPECT_EQ(test, preempt_count(), saved_count);
	kunit_info(test, "hardirq max_depth=%u observed=%u restored=%u\n",
		   max_depth, observed_depth, preempt_count() == saved_count);
}

static void rv_test_irqstate_sync(struct kunit *test)
{
	struct rv_kunit_ctx *ctx = test->priv;
	struct rv_monitor *mon = rv_irqstate_ops.mon.rv_this;
	unsigned int saved_count;
	unsigned long flags;
	int expected, rx_at_sync;
	bool completed;

	prepare_test(test, &rv_irqstate_ops.mon);
	guard(migrate)();
	saved_count = preempt_count();
	for (unsigned int depth = 1; depth <= 2; depth++) {
		local_irq_save(flags);
		mon->enabled = 0;
		for (unsigned int level = 1; level <= depth; level++) {
			__irq_enter_raw();
			rv_irqstate_ops.handle_hardirq_entry(NULL, level);
		}
		mon->reset();
		mon->enabled = 1;
		for (unsigned int level = depth; level; level--) {
			rv_irqstate_ops.handle_hardirq_exit(NULL, level);
			__irq_exit_raw();
		}
		RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
		/* The first unmatched exit after synchronization must now fail. */
		__irq_enter_raw();
		RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
			rv_irqstate_ops.handle_hardirq_exit(NULL, 1);
		__irq_exit_raw();
		local_irq_restore(flags);
	}

	expected = ctx->expected;
	completed = run_irqstate_softirq_case(IRQSTATE_SOFTIRQ_SYNC, &rx_at_sync);
	KUNIT_ASSERT_TRUE(test, completed);
	KUNIT_EXPECT_EQ(test, rx_at_sync, expected);
	RV_KUNIT_EXPECT_REACTION(test, ctx);
	expected = ctx->expected;
	completed = run_irqstate_softirq_case(IRQSTATE_SOFTIRQ_SYNC_INTERRUPTED, &rx_at_sync);
	KUNIT_ASSERT_TRUE(test, completed);
	KUNIT_EXPECT_EQ(test, rx_at_sync, expected);
	RV_KUNIT_EXPECT_REACTION(test, ctx);
	KUNIT_EXPECT_EQ(test, preempt_count(), saved_count);
	kunit_info(test, "sync hardirq=1 nested=2 softirq=1 interrupted_softirq=1 restored=%u\n",
		   preempt_count() == saved_count);
}

struct irqstate_preempt_ctx {
	struct tasklet_struct tasklet;
	struct task_struct *worker;
	struct completion tasklet_done;
	struct completion worker_done;
	bool preempted;
	bool worker_in_softirq;
};

static int irqstate_preempt_worker(void *arg)
{
	struct irqstate_preempt_ctx *preempt_ctx = arg;
	unsigned long flags;

	preempt_ctx->worker_in_softirq = !!in_serving_softirq();
	local_irq_save(flags);
	irq_enter_rcu();
	irq_exit_rcu();
	local_irq_restore(flags);
	complete(&preempt_ctx->worker_done);

	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop())
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static void irqstate_preempt_tasklet(struct tasklet_struct *tasklet)
{
	struct irqstate_preempt_ctx *preempt_ctx;

	preempt_ctx = from_tasklet(preempt_ctx, tasklet, tasklet);
	wake_up_process(preempt_ctx->worker);
	preempt_ctx->preempted = completion_done(&preempt_ctx->worker_done);
	complete(&preempt_ctx->tasklet_done);
}

static void stop_irqstate_preempt_worker(void *worker)
{
	kthread_stop(worker);
}

/* A hardirq in another task must not inherit a suspended RT softirq. */
static void rv_test_irqstate_preempt(struct kunit *test)
{
	struct rv_kunit_ctx *ctx = test->priv;
	struct irqstate_preempt_ctx *preempt_ctx;
	struct task_struct *worker;
	unsigned long completed;

	if (!IS_ENABLED(CONFIG_PREEMPT_RT)) {
		kunit_skip(test, "Requires preemptible softirq processing");
		return;
	}

	prepare_test(test, &rv_irqstate_ops.mon);
	guard(migrate)();

	preempt_ctx = kunit_kzalloc(test, sizeof(*preempt_ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, preempt_ctx);
	init_completion(&preempt_ctx->tasklet_done);
	init_completion(&preempt_ctx->worker_done);
	worker = kthread_create(irqstate_preempt_worker, preempt_ctx,
				"irqstate-preempt");
	KUNIT_ASSERT_FALSE(test, IS_ERR(worker));
	preempt_ctx->worker = worker;
	kthread_bind(worker, smp_processor_id());
	sched_set_fifo(worker);
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, stop_irqstate_preempt_worker,
						  worker));

	attach_irqstate_test_probes();
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, detach_irqstate_test_probes,
						  NULL));
	tasklet_setup(&preempt_ctx->tasklet, irqstate_preempt_tasklet);
	tasklet_schedule(&preempt_ctx->tasklet);
	completed = wait_for_completion_timeout(&preempt_ctx->tasklet_done, HZ);
	tasklet_kill(&preempt_ctx->tasklet);
	KUNIT_ASSERT_NE(test, completed, 0);
	KUNIT_ASSERT_NE(test,
			wait_for_completion_timeout(&preempt_ctx->worker_done, HZ), 0);
	KUNIT_EXPECT_TRUE(test, preempt_ctx->preempted);
	KUNIT_EXPECT_FALSE(test, preempt_ctx->worker_in_softirq);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
}

/* An RT serving interval starts before handle_softirqs() is entered. */
static void rv_test_irqstate_rt_prepare_irq(struct kunit *test)
{
	struct rv_kunit_ctx *ctx = test->priv;
	unsigned int saved_count, saved_softirq;
	unsigned long flags;
	bool serving;

	if (!IS_ENABLED(CONFIG_PREEMPT_RT)) {
		kunit_skip(test, "Requires RT serving-softirq accounting");
		return;
	}

	prepare_test(test, &rv_irqstate_ops.mon);
	guard(migrate)();
	attach_irqstate_test_probes();
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, detach_irqstate_test_probes,
						  NULL));
	saved_count = preempt_count();
	saved_softirq = softirq_count();

	/*
	 * Keep an outer BH-disable reference so the matching enable of the
	 * serving reference cannot dispatch pending softirqs. Both references
	 * use the real RT helpers; no task or per-CPU counter is fabricated.
	 */
	local_bh_disable();
	local_irq_save(flags);
	irq_enter_rcu();
	irq_exit_rcu();
	local_irq_restore(flags);

	/* The same serving-bit transition used by ksoftirqd_run_begin(). */
	__local_bh_disable_ip(_RET_IP_, SOFTIRQ_OFFSET);
	serving = !!in_serving_softirq();
	local_irq_save(flags);
	irq_enter_rcu();
	irq_exit_rcu();
	local_irq_restore(flags);
	__local_bh_enable_ip(_RET_IP_, SOFTIRQ_OFFSET);
	local_bh_enable();

	KUNIT_EXPECT_TRUE(test, serving);
	KUNIT_EXPECT_EQ(test, preempt_count(), saved_count);
	KUNIT_EXPECT_EQ(test, softirq_count(), saved_softirq);
	kunit_info(test, "RT preparation IRQ serving=%u restored=%u\n",
		   serving, preempt_count() == saved_count &&
		   softirq_count() == saved_softirq);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
	local_irq_save(flags);
	__irq_enter_raw();
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqstate_ops.handle_hardirq_exit(NULL, 1);
	__irq_exit_raw();
	local_irq_restore(flags);
}

/* A real task switch is legal before handle_softirqs(), too. */
static void rv_test_irqstate_rt_prepare_switch(struct kunit *test)
{
	struct rv_kunit_ctx *ctx = test->priv;
	struct irqstate_preempt_ctx *preempt_ctx;
	struct task_struct *worker;
	unsigned int saved_count, saved_softirq;
	unsigned long completed, flags;
	bool preempted, serving;

	if (!IS_ENABLED(CONFIG_PREEMPT_RT)) {
		kunit_skip(test, "Requires RT serving-softirq accounting");
		return;
	}

	prepare_test(test, &rv_irqstate_ops.mon);
	guard(migrate)();
	preempt_ctx = kunit_kzalloc(test, sizeof(*preempt_ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, preempt_ctx);
	init_completion(&preempt_ctx->worker_done);
	worker = kthread_create(irqstate_preempt_worker, preempt_ctx,
				"irqstate-prepare");
	KUNIT_ASSERT_FALSE(test, IS_ERR(worker));
	kthread_bind(worker, smp_processor_id());
	sched_set_fifo(worker);
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, stop_irqstate_preempt_worker,
						  worker));
	attach_irqstate_test_probes();
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, detach_irqstate_test_probes,
						  NULL));
	saved_count = preempt_count();
	saved_softirq = softirq_count();

	local_bh_disable();
	local_irq_save(flags);
	irq_enter_rcu();
	irq_exit_rcu();
	local_irq_restore(flags);
	__local_bh_disable_ip(_RET_IP_, SOFTIRQ_OFFSET);
	serving = !!in_serving_softirq();
	wake_up_process(worker);
	preempted = completion_done(&preempt_ctx->worker_done);
	__local_bh_enable_ip(_RET_IP_, SOFTIRQ_OFFSET);
	local_bh_enable();

	completed = wait_for_completion_timeout(&preempt_ctx->worker_done, HZ);
	KUNIT_EXPECT_NE(test, completed, 0);
	KUNIT_EXPECT_TRUE(test, serving);
	KUNIT_EXPECT_TRUE(test, preempted);
	KUNIT_EXPECT_FALSE(test, preempt_ctx->worker_in_softirq);
	KUNIT_EXPECT_EQ(test, preempt_count(), saved_count);
	KUNIT_EXPECT_EQ(test, softirq_count(), saved_softirq);
	kunit_info(test, "RT preparation switch serving=%u preempted=%u worker_serving=%u restored=%u\n",
		   serving, preempted, preempt_ctx->worker_in_softirq,
		   preempt_count() == saved_count && softirq_count() == saved_softirq);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
	local_irq_save(flags);
	__irq_enter_raw();
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqstate_ops.handle_hardirq_exit(NULL, 1);
	__irq_exit_raw();
	local_irq_restore(flags);
}

#else
#define rv_test_irqstate rv_test_stub
#define rv_test_irqstate_depth rv_test_stub
#define rv_test_irqstate_sync rv_test_stub
#define rv_test_irqstate_preempt rv_test_stub
#define rv_test_irqstate_rt_prepare_irq rv_test_stub
#define rv_test_irqstate_rt_prepare_switch rv_test_stub
#endif
