// SPDX-License-Identifier: GPL-2.0
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/hardirq.h>
#include <linux/interrupt.h>
#include <linux/irqflags.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/rv.h>
#include <linux/sched.h>
#include <linux/tick.h>
#include <linux/wait.h>
#include <rv/kunit.h>
#include <rv_trace.h>
#include <trace/events/irq.h>
#include <trace/events/sched.h>
#include <trace/events/timer.h>

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
		rv_irqstate_ops.handle_softirq_entry(NULL, true, false);
		rv_irqstate_ops.mon.rv_this->reset();
		rv_irqstate_ops.mon.rv_this->enabled = 1;
		if (softirq_ctx->test == IRQSTATE_SOFTIRQ_SYNC_INTERRUPTED) {
			__irq_enter_raw();
			rv_irqstate_ops.handle_hardirq_entry(NULL, 1);
			rv_irqstate_ops.handle_hardirq_exit(NULL, 1);
			__irq_exit_raw();
		}
		/* The enclosing softirq exit establishes idle without an entry. */
		rv_irqstate_ops.handle_softirq_exit(NULL, true, false);
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

	rv_irqstate_ops.handle_softirq_entry(NULL, true, false);

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
		rv_irqstate_ops.handle_softirq_entry(NULL, true, false);
		goto out;
	case IRQSTATE_SOFTIRQ_DOUBLE_EXIT:
		rv_irqstate_ops.handle_softirq_exit(NULL, true, false);
		break;
	case IRQSTATE_SOFTIRQ_VALID:
		break;
	case IRQSTATE_SOFTIRQ_TRACEPOINT_ONLY:
	case IRQSTATE_SOFTIRQ_SYNC:
	case IRQSTATE_SOFTIRQ_SYNC_INTERRUPTED:
		WARN_ON_ONCE(1);
		break;
	}

	rv_irqstate_ops.handle_softirq_exit(NULL, true, false);
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

#ifndef MODULE
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

#define IRQSTATE_LIVE_CYCLES	2
#define IRQSTATE_LIVE_BATCH	2

struct irqstate_live_toggle_ctx {
	struct task_struct *owner;
	int pairs;
	int model_entries;
	int model_exits;
	int enable_overlaps;
	int disable_overlaps;
	int pair_state_errors;
	bool monitor_initialized;
	bool observer_attached;
	bool hook_installed;
	bool pair_active;
};

static void irqstate_live_emit_pair(struct irqstate_live_toggle_ctx *live)
{
	unsigned int preempt_count_before;
	int lockdep_context_before;
	bool irqs_disabled_before;
	unsigned long flags;

	irqs_disabled_before = irqs_disabled();
	preempt_count_before = preempt_count();
	if (irqs_disabled_before || in_interrupt())
		live->pair_state_errors++;

	/* Emit only the state and events whose probe lifetime is under test. */
	local_irq_save(flags);
	lockdep_context_before = lockdep_hardirq_context();
	WRITE_ONCE(live->pair_active, true);
	__irq_enter_raw();
	if (hardirq_count() != HARDIRQ_OFFSET || !irqs_disabled() ||
	    (IS_ENABLED(CONFIG_TRACE_IRQFLAGS) && !lockdep_hardirq_context()))
		live->pair_state_errors++;
	trace_hardirq_context_entry(hardirq_count() >> HARDIRQ_SHIFT);
	trace_hardirq_context_exit(hardirq_count() >> HARDIRQ_SHIFT);
	__irq_exit_raw();
	WRITE_ONCE(live->pair_active, false);

	if (preempt_count() != preempt_count_before || !irqs_disabled() ||
	    lockdep_hardirq_context() != lockdep_context_before)
		live->pair_state_errors++;
	local_irq_restore(flags);
	if (irqs_disabled() != irqs_disabled_before)
		live->pair_state_errors++;
	live->pairs++;
}

static void irqstate_live_emit_pairs(struct irqstate_live_toggle_ctx *live,
				     unsigned int nr)
{
	while (nr--)
		irqstate_live_emit_pair(live);
}

static void irqstate_live_lifecycle_hook(void *data, bool enabling)
{
	struct irqstate_live_toggle_ctx *live = data;

	irqstate_live_emit_pair(live);
	if (enabling)
		live->enable_overlaps++;
	else
		live->disable_overlaps++;
}

static void irqstate_live_toggle_observe(void *data, char *state,
					 char *event, char *next_state,
					 bool final_state)
{
	struct irqstate_live_toggle_ctx *live = data;

	if (current != READ_ONCE(live->owner) ||
	    !READ_ONCE(live->pair_active))
		return;
	/*
	 * Count only the synthetic boundary pair emitted while the lifecycle
	 * hook has stopped probe attachment or detachment at its midpoint.
	 */
	if (!strcmp(state, "idle") &&
	    !strcmp(event, "hardirq_entry") &&
	    !strcmp(next_state, "in_hardirq"))
		live->model_entries++;
	else if (!strcmp(state, "in_hardirq") &&
		 !strcmp(event, "hardirq_exit") &&
		 !strcmp(next_state, "idle"))
		live->model_exits++;
}

static void irqstate_live_toggle_cleanup(void *data)
{
	struct irqstate_live_toggle_ctx *live = data;
	struct rv_monitor *mon = rv_irqstate_ops.mon.rv_this;

	if (live->hook_installed) {
		rv_irqstate_ops.set_lifecycle_hook(NULL, NULL);
		live->hook_installed = false;
	}
	mon->enabled = 0;
	if (live->monitor_initialized) {
		mon->disable();
		live->monitor_initialized = false;
	}
	if (live->observer_attached) {
		unregister_trace_event_irqstate(irqstate_live_toggle_observe, live);
		live->observer_attached = false;
	}
	tracepoint_synchronize_unregister();

	RV_KUNIT_EXPECT_NO_REACTION(kunit_get_current_test(), active_ctx);
	mon->react = mon->reactor ? mon->reactor->react : NULL;
	active_ctx = NULL;
	rv_mock_current(NULL);
}

static void rv_test_irqstate_live_toggle(struct kunit *test)
{
	struct rv_monitor *mon = rv_irqstate_ops.mon.rv_this;
	struct rv_kunit_ctx *ctx = test->priv;
	struct irqstate_live_toggle_ctx *live;
	int disabled_entries, disabled_exits, cycle;
	int ret;

	KUNIT_ASSERT_FALSE(test, mon->enabled);
	live = kunit_kzalloc(test, sizeof(*live), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, live);
	live->owner = current;
	active_ctx = ctx;
	mon->react = rv_kunit_mock_react;
	rv_mock_current(NULL);
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test,
						  irqstate_live_toggle_cleanup,
						  live));

	ret = register_trace_event_irqstate(irqstate_live_toggle_observe, live);
	KUNIT_ASSERT_EQ(test, ret, 0);
	live->observer_attached = true;

	rv_irqstate_ops.set_lifecycle_hook(irqstate_live_lifecycle_hook, live);
	live->hook_installed = true;
	irqstate_live_emit_pairs(live, IRQSTATE_LIVE_BATCH);
	KUNIT_EXPECT_EQ(test, live->model_entries, 0);
	KUNIT_EXPECT_EQ(test, live->model_exits, 0);

	for (cycle = 0; cycle < IRQSTATE_LIVE_CYCLES; cycle++) {
		disabled_entries = live->model_entries;
		disabled_exits = live->model_exits;
		ret = mon->enable();
		if (!ret) {
			live->monitor_initialized = true;
			mon->enabled = 1;
		}
		KUNIT_ASSERT_EQ(test, ret, 0);
		KUNIT_EXPECT_EQ(test, live->model_entries,
				disabled_entries);
		KUNIT_EXPECT_EQ(test, live->model_exits,
				disabled_exits);

		irqstate_live_emit_pairs(live, IRQSTATE_LIVE_BATCH);
		KUNIT_EXPECT_EQ(test, live->model_entries,
				disabled_entries + IRQSTATE_LIVE_BATCH);
		KUNIT_EXPECT_EQ(test, live->model_exits,
				disabled_exits + IRQSTATE_LIVE_BATCH);

		disabled_entries = live->model_entries;
		disabled_exits = live->model_exits;
		mon->enabled = 0;
		mon->disable();
		live->monitor_initialized = false;
		tracepoint_synchronize_unregister();
		KUNIT_EXPECT_EQ(test, live->model_entries,
				disabled_entries);
		KUNIT_EXPECT_EQ(test, live->model_exits,
				disabled_exits);
		irqstate_live_emit_pairs(live, IRQSTATE_LIVE_BATCH);
		KUNIT_EXPECT_EQ(test, live->model_entries,
				disabled_entries);
		KUNIT_EXPECT_EQ(test, live->model_exits,
				disabled_exits);
	}

	KUNIT_EXPECT_EQ(test, live->pairs,
			IRQSTATE_LIVE_BATCH + IRQSTATE_LIVE_CYCLES *
			(2 + 2 * IRQSTATE_LIVE_BATCH));
	KUNIT_EXPECT_GT(test, live->model_entries, 0);
	KUNIT_EXPECT_GT(test, live->model_exits, 0);
	KUNIT_EXPECT_EQ(test, live->enable_overlaps,
			IRQSTATE_LIVE_CYCLES);
	KUNIT_EXPECT_EQ(test, live->disable_overlaps,
			IRQSTATE_LIVE_CYCLES);
	KUNIT_EXPECT_EQ(test, live->pair_state_errors, 0);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
	kunit_info(test, "live toggle cycles=%d pairs=%d model=%d/%d overlap=%d/%d state_errors=%d\n",
		   cycle,
		   live->pairs,
		   live->model_entries,
		   live->model_exits,
		   live->enable_overlaps,
		   live->disable_overlaps,
		   live->pair_state_errors);
}
#else
static void rv_test_irqstate_live_toggle(struct kunit *test)
{
	kunit_skip(test, "Requires built-in RV tests");
}
#endif

#ifndef MODULE
struct irqstate_shared_actions_ctx {
	struct irqaction actions[2];
	struct task_struct *owner;
	int owner_cpu;
	int irq;
	int results[2];
	unsigned int entries;
	unsigned int exits;
	unsigned int entry_mask;
	unsigned int exit_mask;
	unsigned int event_index;
	bool armed;
	bool bad_order;
	bool bad_context;
	bool bad_owner;
	bool bad_cpu;
	bool bad_irq;
	bool bad_result;
	bool entry_attached;
	bool exit_attached;
};

static int irqstate_shared_action_index(struct irqstate_shared_actions_ctx *shared_ctx,
					struct irqaction *action)
{
	if (action == &shared_ctx->actions[0])
		return 0;
	if (action == &shared_ctx->actions[1])
		return 1;

	return -1;
}

static void irqstate_shared_action_entry(void *data, int irq,
					 struct irqaction *action)
{
	struct irqstate_shared_actions_ctx *shared_ctx = data;
	int index;

	/* Filter globally registered callbacks by exact action identity first. */
	index = irqstate_shared_action_index(shared_ctx, action);
	if (index < 0 || !READ_ONCE(shared_ctx->armed))
		return;

	shared_ctx->entries++;
	shared_ctx->entry_mask |= BIT(index);
	if (shared_ctx->event_index != index * 2)
		shared_ctx->bad_order = true;
	shared_ctx->event_index++;
	if (!in_hardirq() || hardirq_count() != HARDIRQ_OFFSET ||
	    !irqs_disabled())
		shared_ctx->bad_context = true;
	if (current != shared_ctx->owner)
		shared_ctx->bad_owner = true;
	if (raw_smp_processor_id() != shared_ctx->owner_cpu)
		shared_ctx->bad_cpu = true;
	if (irq != shared_ctx->irq)
		shared_ctx->bad_irq = true;
}

static void irqstate_shared_action_exit(void *data, int irq,
					struct irqaction *action, int ret)
{
	struct irqstate_shared_actions_ctx *shared_ctx = data;
	int index;

	/* Filter globally registered callbacks by exact action identity first. */
	index = irqstate_shared_action_index(shared_ctx, action);
	if (index < 0 || !READ_ONCE(shared_ctx->armed))
		return;

	shared_ctx->exits++;
	shared_ctx->exit_mask |= BIT(index);
	if (shared_ctx->event_index != index * 2 + 1)
		shared_ctx->bad_order = true;
	shared_ctx->event_index++;
	if (!in_hardirq() || hardirq_count() != HARDIRQ_OFFSET ||
	    !irqs_disabled())
		shared_ctx->bad_context = true;
	if (current != shared_ctx->owner)
		shared_ctx->bad_owner = true;
	if (raw_smp_processor_id() != shared_ctx->owner_cpu)
		shared_ctx->bad_cpu = true;
	if (irq != shared_ctx->irq)
		shared_ctx->bad_irq = true;
	if (ret != shared_ctx->results[index])
		shared_ctx->bad_result = true;
}

static void irqstate_shared_actions_detach(void *data)
{
	struct irqstate_shared_actions_ctx *shared_ctx = data;

	WRITE_ONCE(shared_ctx->armed, false);
	if (shared_ctx->exit_attached)
		unregister_trace_irq_handler_exit(irqstate_shared_action_exit,
						  shared_ctx);
	if (shared_ctx->entry_attached)
		unregister_trace_irq_handler_entry(irqstate_shared_action_entry,
						   shared_ctx);
	tracepoint_synchronize_unregister();
}
#endif

static void rv_test_irqstate(struct kunit *test)
{
	struct rv_kunit_ctx *ctx = test->priv;
#ifndef MODULE
	struct irqaction action = {
		.name = "rv-kunit",
	};
	int ret;
#endif
	unsigned long flags;
	int reactions[8];
	unsigned int i;
	bool completed;

	prepare_test(test, &rv_irqstate_ops.mon);
	/* Fatal KUnit exits do not unwind this migration guard. */
	guard(migrate)();

	/* Context-boundary events must agree with the live context state. */
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqstate_ops.handle_hardirq_entry(NULL, 0);
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqstate_ops.handle_hardirq_exit(NULL, 0);
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqstate_ops.handle_softirq_entry(NULL, false, false);
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqstate_ops.handle_softirq_exit(NULL, false, false);

	/* Disabling BH does not mean that a softirq is being served. */
	local_bh_disable();
	reactions[0] = READ_ONCE(ctx->reactions);
	rv_irqstate_ops.handle_softirq_entry(NULL, false, false);
	reactions[1] = READ_ONCE(ctx->reactions);
	reactions[2] = READ_ONCE(ctx->reactions);
	rv_irqstate_ops.handle_softirq_exit(NULL, false, false);
	reactions[3] = READ_ONCE(ctx->reactions);
	local_bh_enable();
	for (i = 0; i < 4; i++)
		RV_KUNIT_EXPECT_OBSERVED(test, ctx, reactions[i], i & 1);

	/* Nested hardirqs leave the outer hardirq automaton state unchanged. */
	local_irq_save(flags);
	__irq_enter_raw();
	rv_irqstate_ops.handle_hardirq_entry(NULL, 1);
	__irq_enter_raw();
	rv_irqstate_ops.handle_hardirq_entry(NULL, 2);
	rv_irqstate_ops.handle_hardirq_exit(NULL, 2);
	__irq_exit_raw();
	rv_irqstate_ops.handle_hardirq_exit(NULL, 1);
	reactions[0] = READ_ONCE(ctx->reactions);
	__irq_exit_raw();
	local_irq_restore(flags);
	RV_KUNIT_EXPECT_OBSERVED(test, ctx, reactions[0], 0);

	/* PREEMPT_RT never serves softirqs from irq_exit(). */
	if (!IS_ENABLED(CONFIG_PREEMPT_RT)) {
		/* Softirq processing replaces and then restores hardirq state. */
		local_irq_save(flags);
		__irq_enter_raw();
		rv_irqstate_ops.handle_hardirq_entry(NULL, 1);
		__preempt_count_sub(HARDIRQ_OFFSET - SOFTIRQ_OFFSET);
		rv_irqstate_ops.handle_softirq_entry(NULL, true, true);
		__irq_enter_raw();
		rv_irqstate_ops.handle_hardirq_entry(NULL, 1);
		rv_irqstate_ops.handle_hardirq_exit(NULL, 1);
		__irq_exit_raw();
		rv_irqstate_ops.handle_softirq_exit(NULL, true, true);
		__preempt_count_add(HARDIRQ_OFFSET - SOFTIRQ_OFFSET);
		rv_irqstate_ops.handle_hardirq_exit(NULL, 1);
		reactions[0] = READ_ONCE(ctx->reactions);
		__irq_exit_raw();
		local_irq_restore(flags);
		RV_KUNIT_EXPECT_OBSERVED(test, ctx, reactions[0], 0);

		/* Provenance selects the transition which returns to hardirq. */
		local_irq_save(flags);
		__irq_enter_raw();
		rv_irqstate_ops.handle_hardirq_entry(NULL, 1);
		__preempt_count_sub(HARDIRQ_OFFSET - SOFTIRQ_OFFSET);
		reactions[0] = READ_ONCE(ctx->reactions);
		rv_irqstate_ops.handle_softirq_entry(NULL, true, false);
		reactions[1] = READ_ONCE(ctx->reactions);
		__preempt_count_add(HARDIRQ_OFFSET - SOFTIRQ_OFFSET);
		rv_irqstate_ops.handle_hardirq_exit(NULL, 1);
		reactions[2] = READ_ONCE(ctx->reactions);
		__irq_exit_raw();
		local_irq_restore(flags);
		RV_KUNIT_EXPECT_OBSERVED(test, ctx, reactions[0], 0);
		RV_KUNIT_EXPECT_OBSERVED(test, ctx, reactions[1], 1);
		RV_KUNIT_EXPECT_OBSERVED(test, ctx, reactions[2], 0);
	}

	/* A second outer entry is not a nested interrupt. */
	local_irq_save(flags);
	__irq_enter_raw();
	rv_irqstate_ops.handle_hardirq_entry(NULL, 1);
	reactions[0] = READ_ONCE(ctx->reactions);
	rv_irqstate_ops.handle_hardirq_entry(NULL, 1);
	reactions[1] = READ_ONCE(ctx->reactions);
	rv_irqstate_ops.handle_hardirq_exit(NULL, 1);
	reactions[2] = READ_ONCE(ctx->reactions);
	__irq_exit_raw();
	local_irq_restore(flags);
	RV_KUNIT_EXPECT_OBSERVED(test, ctx, reactions[0], 0);
	RV_KUNIT_EXPECT_OBSERVED(test, ctx, reactions[1], 1);
	RV_KUNIT_EXPECT_OBSERVED(test, ctx, reactions[2], 0);

	/* An exit without its corresponding entry is invalid after sync. */
	local_irq_save(flags);
	__irq_enter_raw();
	reactions[0] = READ_ONCE(ctx->reactions);
	rv_irqstate_ops.handle_hardirq_exit(NULL, 1);
	reactions[1] = READ_ONCE(ctx->reactions);
	__irq_exit_raw();
	local_irq_restore(flags);
	RV_KUNIT_EXPECT_OBSERVED(test, ctx, reactions[0], 0);
	RV_KUNIT_EXPECT_OBSERVED(test, ctx, reactions[1], 1);

	/* Exercise valid and deliberately invalid serving-softirq sequences. */
	completed = exercise_irqstate_softirq(IRQSTATE_SOFTIRQ_VALID);
	KUNIT_EXPECT_TRUE(test, completed);
	if (!completed)
		return;
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
	completed = exercise_irqstate_softirq(IRQSTATE_SOFTIRQ_INTERRUPTED);
	KUNIT_EXPECT_TRUE(test, completed);
	if (!completed)
		return;
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
	completed = exercise_irqstate_softirq(IRQSTATE_SOFTIRQ_DOUBLE_ENTRY);
	KUNIT_EXPECT_TRUE(test, completed);
	if (!completed)
		return;
	RV_KUNIT_EXPECT_REACTION(test, ctx);
	completed = exercise_irqstate_softirq(IRQSTATE_SOFTIRQ_DOUBLE_EXIT);
	KUNIT_EXPECT_TRUE(test, completed);
	if (!completed)
		return;
	RV_KUNIT_EXPECT_REACTION(test, ctx);

#ifndef MODULE
	/* Exercise the actual generic tracepoint attachments as well. */
	attach_irqstate_test_probes();
	ret = kunit_add_action_or_reset(test, detach_irqstate_test_probes, NULL);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		return;

	/* IRQ action events no longer drive the context-boundary monitor. */
	trace_irq_handler_entry(0, &action);
	trace_irq_handler_exit(0, &action, IRQ_HANDLED);
	trace_softirq_entry(0);
	trace_softirq_exit(0);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);

	/* Manually invalid boundary events prove that all probes are attached. */
	local_irq_save(flags);
	reactions[0] = READ_ONCE(ctx->reactions);
	trace_hardirq_context_entry(0);
	reactions[1] = READ_ONCE(ctx->reactions);
	reactions[2] = READ_ONCE(ctx->reactions);
	trace_hardirq_context_exit(0);
	reactions[3] = READ_ONCE(ctx->reactions);
	reactions[4] = READ_ONCE(ctx->reactions);
	trace_softirq_context_entry(false, false);
	reactions[5] = READ_ONCE(ctx->reactions);
	reactions[6] = READ_ONCE(ctx->reactions);
	trace_softirq_context_exit(false, false);
	reactions[7] = READ_ONCE(ctx->reactions);
	local_irq_restore(flags);
	for (i = 0; i < ARRAY_SIZE(reactions); i++)
		RV_KUNIT_EXPECT_OBSERVED(test, ctx, reactions[i], i & 1);

	/* These helpers emit valid hardirq context-boundary tracepoints. */
	local_irq_save(flags);
	irq_enter_rcu();
	irq_exit_rcu();
	reactions[0] = READ_ONCE(ctx->reactions);
	local_irq_restore(flags);
	RV_KUNIT_EXPECT_OBSERVED(test, ctx, reactions[0], 0);

	/* This tasklet relies only on the live softirq context tracepoints. */
	completed = exercise_irqstate_softirq(IRQSTATE_SOFTIRQ_TRACEPOINT_ONLY);
	KUNIT_EXPECT_TRUE(test, completed);
	if (!completed)
		return;
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
#endif
}

#ifndef MODULE
static void rv_test_irqstate_shared_actions(struct kunit *test)
{
	struct irqstate_shared_actions_ctx *shared_ctx;
	struct rv_kunit_ctx *ctx = test->priv;
	unsigned int saved_count, saved_softirq;
	unsigned long flags;
	int ret;

	prepare_test(test, &rv_irqstate_ops.mon);
	shared_ctx = kunit_kzalloc(test, sizeof(*shared_ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, shared_ctx);
	shared_ctx->actions[0].name = "rv-kunit-none";
	shared_ctx->actions[1].name = "rv-kunit-handled";
	shared_ctx->results[0] = IRQ_NONE;
	shared_ctx->results[1] = IRQ_HANDLED;
	shared_ctx->irq = 42;

	ret = register_trace_irq_handler_entry(irqstate_shared_action_entry,
					       shared_ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);
	shared_ctx->entry_attached = true;
	ret = kunit_add_action_or_reset(test, irqstate_shared_actions_detach,
					shared_ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = register_trace_irq_handler_exit(irqstate_shared_action_exit,
					      shared_ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);
	shared_ctx->exit_attached = true;
	attach_irqstate_test_probes();
	ret = kunit_add_action_or_reset(test, detach_irqstate_test_probes, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);

	scoped_guard(migrate) {
		shared_ctx->owner = current;
		shared_ctx->owner_cpu = raw_smp_processor_id();
		saved_count = preempt_count();
		saved_softirq = softirq_count();
		WRITE_ONCE(shared_ctx->armed, true);
		local_irq_save(flags);
		irq_enter_rcu();
		trace_irq_handler_entry(shared_ctx->irq,
					&shared_ctx->actions[0]);
		trace_irq_handler_exit(shared_ctx->irq,
				       &shared_ctx->actions[0],
				       shared_ctx->results[0]);
		trace_irq_handler_entry(shared_ctx->irq,
					&shared_ctx->actions[1]);
		trace_irq_handler_exit(shared_ctx->irq,
				       &shared_ctx->actions[1],
				       shared_ctx->results[1]);
		irq_exit_rcu();
		WRITE_ONCE(shared_ctx->armed, false);
		local_irq_restore(flags);
	}

	KUNIT_EXPECT_EQ(test, shared_ctx->entries, 2U);
	KUNIT_EXPECT_EQ(test, shared_ctx->exits, 2U);
	KUNIT_EXPECT_EQ(test, shared_ctx->entry_mask, 3U);
	KUNIT_EXPECT_EQ(test, shared_ctx->exit_mask, 3U);
	KUNIT_EXPECT_EQ(test, shared_ctx->event_index, 4U);
	KUNIT_EXPECT_FALSE(test, shared_ctx->bad_order);
	KUNIT_EXPECT_FALSE(test, shared_ctx->bad_context);
	KUNIT_EXPECT_FALSE(test, shared_ctx->bad_owner);
	KUNIT_EXPECT_FALSE(test, shared_ctx->bad_cpu);
	KUNIT_EXPECT_FALSE(test, shared_ctx->bad_irq);
	KUNIT_EXPECT_FALSE(test, shared_ctx->bad_result);
	KUNIT_EXPECT_EQ(test, preempt_count(), saved_count);
	KUNIT_EXPECT_EQ(test, softirq_count(), saved_softirq);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
	kunit_info(test,
		   "shared actions events=%u/%u order=%u context=%u same_task=%u same_cpu=%u restored=%u\n",
		   shared_ctx->entries, shared_ctx->exits,
		   !shared_ctx->bad_order, !shared_ctx->bad_context,
		   !shared_ctx->bad_owner, !shared_ctx->bad_cpu,
		   preempt_count() == saved_count &&
		   softirq_count() == saved_softirq);
}
#else
static void rv_test_irqstate_shared_actions(struct kunit *test)
{
	kunit_skip(test, "Requires built-in RV tests");
}
#endif

static void rv_test_irqstate_depth(struct kunit *test)
{
	struct rv_kunit_ctx *ctx = test->priv;
	unsigned int max_depth = HARDIRQ_MASK >> HARDIRQ_SHIFT;
	unsigned int observed_depth, saved_count;
	unsigned long flags;
	int reactions[3];

	prepare_test(test, &rv_irqstate_ops.mon);
	/* Use only nonfatal checks and normal returns after pinning current. */
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
	reactions[0] = READ_ONCE(ctx->reactions);
	/* A silent adapter cannot pass: the completed outer pair armed idle. */
	__irq_enter_raw();
	reactions[1] = READ_ONCE(ctx->reactions);
	rv_irqstate_ops.handle_hardirq_exit(NULL, 1);
	reactions[2] = READ_ONCE(ctx->reactions);
	__irq_exit_raw();
	local_irq_restore(flags);
	RV_KUNIT_EXPECT_OBSERVED(test, ctx, reactions[0], 0);
	RV_KUNIT_EXPECT_OBSERVED(test, ctx, reactions[1], 0);
	RV_KUNIT_EXPECT_OBSERVED(test, ctx, reactions[2], 1);
	KUNIT_EXPECT_EQ(test, observed_depth, max_depth);
	KUNIT_EXPECT_EQ(test, preempt_count(), saved_count);
	kunit_info(test, "hardirq max_depth=%u observed=%u restored=%u\n",
		   max_depth, observed_depth, preempt_count() == saved_count);
}

static void rv_test_irqstate_depth_overflow(struct kunit *test)
{
	struct rv_kunit_ctx *ctx = test->priv;
	unsigned int max_depth = HARDIRQ_MASK >> HARDIRQ_SHIFT;
	unsigned int observed_depth, saved_count, saved_nmi;
	unsigned int nmi_at_max, nmi_at_overflow;
	int reactions_before, reactions_before_overflow;
	int reactions_at_overflow, reactions_after_unwind;
	unsigned long flags;

	prepare_test(test, &rv_irqstate_ops.mon);
	guard(migrate)();
	saved_count = preempt_count();
	saved_nmi = nmi_count();
	reactions_before = ctx->reactions;

	local_irq_save(flags);
	for (unsigned int depth = 1; depth <= max_depth; depth++) {
		__irq_enter_raw();
		rv_irqstate_ops.handle_hardirq_entry(NULL, depth);
	}
	observed_depth = hardirq_count() >> HARDIRQ_SHIFT;
	nmi_at_max = nmi_count();
	reactions_before_overflow = ctx->reactions;

	/*
	 * A real increment here would carry into NMI_MASK. Feed the resulting
	 * wrapped depth directly to the adapter without corrupting preempt_count.
	 */
	rv_irqstate_ops.handle_hardirq_entry(NULL, 0);
	nmi_at_overflow = nmi_count();
	reactions_at_overflow = ctx->reactions;
	for (unsigned int depth = max_depth; depth; depth--) {
		rv_irqstate_ops.handle_hardirq_exit(NULL, depth);
		__irq_exit_raw();
	}
	reactions_after_unwind = ctx->reactions;
	local_irq_restore(flags);

	KUNIT_EXPECT_EQ(test, observed_depth, max_depth);
	KUNIT_EXPECT_EQ(test, reactions_before_overflow, reactions_before);
	KUNIT_EXPECT_EQ(test, reactions_at_overflow,
			reactions_before_overflow + 1);
	KUNIT_EXPECT_EQ(test, reactions_after_unwind, reactions_at_overflow);
	RV_KUNIT_EXPECT_REACTION(test, ctx);
	KUNIT_EXPECT_EQ(test, nmi_at_max, saved_nmi);
	KUNIT_EXPECT_EQ(test, nmi_at_overflow, saved_nmi);
	KUNIT_EXPECT_EQ(test, nmi_count(), saved_nmi);
	KUNIT_EXPECT_EQ(test, preempt_count(), saved_count);
	kunit_info(test,
		   "hardirq overflow max_depth=%u observed=%u reactions=%d restored=%u\n",
		   max_depth, observed_depth,
		   reactions_after_unwind - reactions_before,
		   preempt_count() == saved_count && nmi_count() == saved_nmi);
}

static void rv_test_irqstate_sync(struct kunit *test)
{
	struct rv_kunit_ctx *ctx = test->priv;
	struct rv_monitor *mon = rv_irqstate_ops.mon.rv_this;
	unsigned int saved_count;
	unsigned long flags;
	int expected, rx_at_sync;
	int reactions[3];
	bool completed;

	prepare_test(test, &rv_irqstate_ops.mon);
	/* Use only nonfatal checks and normal returns after pinning current. */
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
		reactions[0] = READ_ONCE(ctx->reactions);
		/* The first unmatched exit after synchronization must now fail. */
		__irq_enter_raw();
		reactions[1] = READ_ONCE(ctx->reactions);
		rv_irqstate_ops.handle_hardirq_exit(NULL, 1);
		reactions[2] = READ_ONCE(ctx->reactions);
		__irq_exit_raw();
		local_irq_restore(flags);
		RV_KUNIT_EXPECT_OBSERVED(test, ctx, reactions[0], 0);
		RV_KUNIT_EXPECT_OBSERVED(test, ctx, reactions[1], 0);
		RV_KUNIT_EXPECT_OBSERVED(test, ctx, reactions[2], 1);
	}

	if (!IS_ENABLED(CONFIG_PREEMPT_RT)) {
		/* A mid-hardirq attach ignores softirq and syncs at H-exit. */
		local_irq_save(flags);
		mon->enabled = 0;
		__irq_enter_raw();
		rv_irqstate_ops.handle_hardirq_entry(NULL, 1);
		mon->reset();
		mon->enabled = 1;
		__preempt_count_sub(HARDIRQ_OFFSET - SOFTIRQ_OFFSET);
		rv_irqstate_ops.handle_softirq_entry(NULL, true, true);
		rv_irqstate_ops.handle_softirq_exit(NULL, true, true);
		__preempt_count_add(HARDIRQ_OFFSET - SOFTIRQ_OFFSET);
		rv_irqstate_ops.handle_hardirq_exit(NULL, 1);
		__irq_exit_raw();
		reactions[0] = READ_ONCE(ctx->reactions);
		__irq_enter_raw();
		reactions[1] = READ_ONCE(ctx->reactions);
		rv_irqstate_ops.handle_hardirq_exit(NULL, 1);
		reactions[2] = READ_ONCE(ctx->reactions);
		__irq_exit_raw();
		local_irq_restore(flags);
		RV_KUNIT_EXPECT_OBSERVED(test, ctx, reactions[0], 0);
		RV_KUNIT_EXPECT_OBSERVED(test, ctx, reactions[1], 0);
		RV_KUNIT_EXPECT_OBSERVED(test, ctx, reactions[2], 1);
	}

	expected = ctx->expected;
	completed = run_irqstate_softirq_case(IRQSTATE_SOFTIRQ_SYNC, &rx_at_sync);
	KUNIT_EXPECT_TRUE(test, completed);
	if (!completed)
		return;
	KUNIT_EXPECT_EQ(test, rx_at_sync, expected);
	RV_KUNIT_EXPECT_REACTION(test, ctx);
	expected = ctx->expected;
	completed = run_irqstate_softirq_case(IRQSTATE_SOFTIRQ_SYNC_INTERRUPTED, &rx_at_sync);
	KUNIT_EXPECT_TRUE(test, completed);
	if (!completed)
		return;
	KUNIT_EXPECT_EQ(test, rx_at_sync, expected);
	RV_KUNIT_EXPECT_REACTION(test, ctx);
	KUNIT_EXPECT_EQ(test, preempt_count(), saved_count);
	kunit_info(test, "sync hardirq=1 nested=2 softirq=1 interrupted_softirq=1 restored=%u\n",
		   preempt_count() == saved_count);
}

#ifndef MODULE
struct irqstate_preempt_ctx {
	struct tasklet_struct tasklet;
	struct task_struct *worker;
	struct completion tasklet_done;
	struct completion worker_done;
	bool preempted;
	bool worker_in_softirq;
};

struct irqstate_inline_bh_ctx {
	struct tasklet_struct tasklet;
	struct tasklet_struct entry_tasklet;
	struct tasklet_struct exit_tasklet;
	struct task_struct *owner;
	struct task_struct *softirqd;
	int owner_cpu;
	unsigned int entries;
	unsigned int exits;
	unsigned int hardirq_entries;
	unsigned int hardirq_exits;
	unsigned int event_index;
	unsigned int tasklet_runs;
	unsigned int nested_hardirq_runs;
	unsigned int entry_tasklet_runs;
	unsigned int exit_tasklet_runs;
	bool armed;
	bool raise_on_entry;
	bool raise_on_exit;
	bool inject_nested_hardirq;
	bool expected_from_hardirq;
	bool entry_from_hardirq;
	bool exit_from_hardirq;
	bool exit_tasklet_pending;
	bool softirqd_already_runnable;
	bool bad_boundary_context;
	bool bad_boundary_cpu;
	bool bad_provenance;
	bool bad_order;
	bool tasklet_serving;
	bool tasklet_same_task;
	bool tasklet_same_cpu;
	bool tasklet_irqs_enabled;
	bool tasklet_in_hardirq;
	bool tasklet_bracketed;
	bool entry_attached;
	bool exit_attached;
	bool hardirq_entry_attached;
	bool hardirq_exit_attached;
};

struct irqstate_empty_worker_ctx {
	struct task_struct *owner;
	int owner_cpu;
	unsigned int entries;
	unsigned int exits;
	unsigned int action_entries;
	unsigned int action_exits;
	unsigned int event_index;
	bool armed;
	bool real_worker;
	bool inside_pair;
	bool bad_order;
	bool bad_boundary_context;
	bool bad_boundary_cpu;
	bool actions_at_exit;
	bool entry_attached;
	bool exit_attached;
	bool action_entry_attached;
	bool action_exit_attached;
};

struct irqstate_hardirq_exit_ctx {
	struct tasklet_struct tasklet;
	struct task_struct *owner;
	struct task_struct *softirqd;
	int owner_cpu;
	bool armed;
	bool callback_seen;
	bool tasklet_pending;
	bool softirqd_already_runnable;
	bool bad_context;
	bool attached;
};

#ifdef CONFIG_NO_HZ_FULL
struct irqstate_tick_exit_ctx {
	struct kunit *test;
	struct tasklet_struct tasklet;
	struct task_struct *owner;
	struct task_struct *softirqd;
	cpumask_t saved_mask;
	unsigned int tick_dep_before;
	unsigned int tick_dep_after;
	int owner_cpu;
	int tick_success;
	int tick_dependency;
	bool armed;
	bool callback_seen;
	bool tasklet_pending;
	bool softirqd_already_runnable;
	bool bad_context;
	bool tick_attached;
	bool tick_dep_added;
	bool tick_dep_sampled;
	bool affinity_changed;
	bool hotplug_disabled;
};
#endif

static bool irqstate_empty_worker_matches(struct irqstate_empty_worker_ctx *empty_ctx)
{
	/* Pairs with owner publication in both empty-worker test paths. */
	if (!smp_load_acquire(&empty_ctx->armed) ||
	    current != READ_ONCE(empty_ctx->owner))
		return false;

	return !empty_ctx->real_worker || softirq_kunit_empty_worker_active();
}

static void irqstate_inline_hardirq_entry(void *data, unsigned int depth)
{
	struct irqstate_inline_bh_ctx *inline_ctx = data;

	/* Inline softirq processing enables IRQs; ignore a nested real IRQ. */
	if (!READ_ONCE(inline_ctx->armed) || current != inline_ctx->owner ||
	    in_serving_softirq() || depth != 1)
		return;
	inline_ctx->hardirq_entries++;
	if (inline_ctx->event_index != 0)
		inline_ctx->bad_order = true;
	inline_ctx->event_index++;
	if (hardirq_count() != HARDIRQ_OFFSET || in_serving_softirq() ||
	    !irqs_disabled())
		inline_ctx->bad_boundary_context = true;
}

static void irqstate_inline_hardirq_exit(void *data, unsigned int depth)
{
	struct irqstate_inline_bh_ctx *inline_ctx = data;

	if (!READ_ONCE(inline_ctx->armed) || current != inline_ctx->owner ||
	    in_serving_softirq() || depth != 1)
		return;
	inline_ctx->hardirq_exits++;
	if (inline_ctx->event_index != 3)
		inline_ctx->bad_order = true;
	inline_ctx->event_index++;
	if (hardirq_count() != HARDIRQ_OFFSET || in_serving_softirq() ||
	    !irqs_disabled())
		inline_ctx->bad_boundary_context = true;
}

static void irqstate_inline_bh_entry(void *data, bool serving,
				     bool from_hardirq)
{
	struct irqstate_inline_bh_ctx *inline_ctx = data;

	if (!READ_ONCE(inline_ctx->armed) || current != inline_ctx->owner)
		return;
	inline_ctx->entries++;
	if (inline_ctx->event_index !=
	    (inline_ctx->expected_from_hardirq ? 1 : 0))
		inline_ctx->bad_order = true;
	inline_ctx->event_index++;
	inline_ctx->entry_from_hardirq = from_hardirq;
	if (!serving || !in_serving_softirq() || !irqs_disabled())
		inline_ctx->bad_boundary_context = true;
	if (from_hardirq != inline_ctx->expected_from_hardirq)
		inline_ctx->bad_provenance = true;
	if (raw_smp_processor_id() != inline_ctx->owner_cpu)
		inline_ctx->bad_boundary_cpu = true;
	if (READ_ONCE(inline_ctx->raise_on_entry)) {
		WRITE_ONCE(inline_ctx->raise_on_entry, false);
		tasklet_hi_schedule(&inline_ctx->entry_tasklet);
	}
}

static void irqstate_inline_bh_exit(void *data, bool serving,
				    bool from_hardirq)
{
	struct irqstate_inline_bh_ctx *inline_ctx = data;

	if (!READ_ONCE(inline_ctx->armed) || current != inline_ctx->owner)
		return;
	inline_ctx->exits++;
	if (inline_ctx->event_index !=
	    (inline_ctx->expected_from_hardirq ? 2 : 1))
		inline_ctx->bad_order = true;
	inline_ctx->event_index++;
	inline_ctx->exit_from_hardirq = from_hardirq;
	if (!serving || !in_serving_softirq() || !irqs_disabled())
		inline_ctx->bad_boundary_context = true;
	if (from_hardirq != inline_ctx->expected_from_hardirq)
		inline_ctx->bad_provenance = true;
	if (raw_smp_processor_id() != inline_ctx->owner_cpu)
		inline_ctx->bad_boundary_cpu = true;
	if (READ_ONCE(inline_ctx->raise_on_exit)) {
		WRITE_ONCE(inline_ctx->raise_on_exit, false);
		tasklet_schedule(&inline_ctx->exit_tasklet);
	}
}

static void irqstate_inline_bh_tasklet(struct tasklet_struct *tasklet)
{
	struct irqstate_inline_bh_ctx *inline_ctx;
	unsigned long flags;

	inline_ctx = from_tasklet(inline_ctx, tasklet, tasklet);
	inline_ctx->tasklet_runs++;
	inline_ctx->tasklet_serving = !!in_serving_softirq();
	inline_ctx->tasklet_same_task = current == inline_ctx->owner;
	inline_ctx->tasklet_same_cpu = raw_smp_processor_id() ==
					inline_ctx->owner_cpu;
	inline_ctx->tasklet_irqs_enabled = !irqs_disabled();
	inline_ctx->tasklet_in_hardirq = !!in_hardirq();
	inline_ctx->tasklet_bracketed = inline_ctx->entries == 1 &&
					 inline_ctx->exits == 0;
	if (inline_ctx->inject_nested_hardirq) {
		local_irq_save(flags);
		irq_enter_rcu();
		irq_exit_rcu();
		local_irq_restore(flags);
		inline_ctx->nested_hardirq_runs++;
	}
}

static void irqstate_inline_bh_exit_tasklet(struct tasklet_struct *tasklet)
{
	struct irqstate_inline_bh_ctx *inline_ctx;

	inline_ctx = from_tasklet(inline_ctx, tasklet, exit_tasklet);
	inline_ctx->exit_tasklet_runs++;
}

static void irqstate_inline_bh_entry_tasklet(struct tasklet_struct *tasklet)
{
	struct irqstate_inline_bh_ctx *inline_ctx;

	inline_ctx = from_tasklet(inline_ctx, tasklet, entry_tasklet);
	inline_ctx->entry_tasklet_runs++;
}

static void irqstate_inline_bh_kill(void *data)
{
	struct irqstate_inline_bh_ctx *inline_ctx = data;

	tasklet_kill(&inline_ctx->tasklet);
}

static void irqstate_inline_bh_kill_all(void *data)
{
	struct irqstate_inline_bh_ctx *inline_ctx = data;

	tasklet_kill(&inline_ctx->tasklet);
	tasklet_kill(&inline_ctx->entry_tasklet);
	tasklet_kill(&inline_ctx->exit_tasklet);
}

static void irqstate_inline_bh_detach(void *data)
{
	struct irqstate_inline_bh_ctx *inline_ctx = data;

	WRITE_ONCE(inline_ctx->armed, false);
	if (inline_ctx->hardirq_exit_attached)
		unregister_trace_hardirq_context_exit(
			irqstate_inline_hardirq_exit, inline_ctx);
	if (inline_ctx->hardirq_entry_attached)
		unregister_trace_hardirq_context_entry(
			irqstate_inline_hardirq_entry, inline_ctx);
	if (inline_ctx->entry_attached)
		unregister_trace_softirq_context_entry(irqstate_inline_bh_entry, inline_ctx);
	if (inline_ctx->exit_attached)
		unregister_trace_softirq_context_exit(irqstate_inline_bh_exit, inline_ctx);
	tracepoint_synchronize_unregister();
}

static void irqstate_hardirq_exit_tasklet(struct tasklet_struct *tasklet)
{
	(void)tasklet;
}

static void irqstate_hardirq_exit_raise(void *data, unsigned int depth)
{
	struct irqstate_hardirq_exit_ctx *exit_ctx = data;

	if (!READ_ONCE(exit_ctx->armed) || current != exit_ctx->owner)
		return;
	exit_ctx->callback_seen = true;
	if (depth != 1 || hardirq_count() != HARDIRQ_OFFSET ||
	    in_serving_softirq() || !irqs_disabled() ||
	    raw_smp_processor_id() != exit_ctx->owner_cpu)
		exit_ctx->bad_context = true;
	tasklet_schedule(&exit_ctx->tasklet);
}

static void irqstate_hardirq_exit_cleanup(void *data)
{
	struct irqstate_hardirq_exit_ctx *exit_ctx = data;

	WRITE_ONCE(exit_ctx->armed, false);
	if (exit_ctx->attached)
		unregister_trace_hardirq_context_exit(
			irqstate_hardirq_exit_raise, exit_ctx);
	tracepoint_synchronize_unregister();
	tasklet_kill(&exit_ctx->tasklet);
}

#ifdef CONFIG_NO_HZ_FULL
static void irqstate_tick_exit_tasklet(struct tasklet_struct *tasklet)
{
	(void)tasklet;
}

static void irqstate_tick_exit_raise(void *data, int success, int dependency)
{
	struct irqstate_tick_exit_ctx *tick_ctx = data;

	if (!READ_ONCE(tick_ctx->armed) ||
	    current != READ_ONCE(tick_ctx->owner) ||
	    READ_ONCE(tick_ctx->callback_seen))
		return;

	WRITE_ONCE(tick_ctx->callback_seen, true);
	tick_ctx->tick_success = success;
	tick_ctx->tick_dependency = dependency;
	if (success || !dependency ||
	    hardirq_count() != HARDIRQ_OFFSET ||
	    irq_count() != HARDIRQ_OFFSET || in_serving_softirq() ||
	    !irqs_disabled() ||
	    raw_smp_processor_id() != tick_ctx->owner_cpu)
		tick_ctx->bad_context = true;
	tasklet_schedule(&tick_ctx->tasklet);
}

static void irqstate_tick_exit_restore_dep(struct irqstate_tick_exit_ctx *tick_ctx)
{
	if (tick_ctx->tick_dep_added) {
		tick_dep_clear_task(tick_ctx->owner, TICK_DEP_BIT_POSIX_TIMER);
		tick_ctx->tick_dep_added = false;
	}
	if (!tick_ctx->tick_dep_sampled) {
		tick_ctx->tick_dep_after =
			atomic_read(&tick_ctx->owner->tick_dep_mask);
		tick_ctx->tick_dep_sampled = true;
	}
}

static void irqstate_tick_exit_cleanup(void *data)
{
	struct irqstate_tick_exit_ctx *tick_ctx = data;
	int ret;

	WRITE_ONCE(tick_ctx->armed, false);
	if (tick_ctx->tick_attached)
		unregister_trace_tick_stop(irqstate_tick_exit_raise, tick_ctx);
	tracepoint_synchronize_unregister();
	irqstate_tick_exit_restore_dep(tick_ctx);
	tasklet_kill(&tick_ctx->tasklet);
	if (tick_ctx->affinity_changed &&
	    !(READ_ONCE(tick_ctx->owner->flags) & PF_EXITING)) {
		ret = set_cpus_allowed_ptr(tick_ctx->owner,
					   &tick_ctx->saved_mask);
		KUNIT_EXPECT_EQ(tick_ctx->test, ret, 0);
		if (!ret)
			tick_ctx->affinity_changed = false;
	}
	if (tick_ctx->hotplug_disabled) {
		cpu_hotplug_enable();
		tick_ctx->hotplug_disabled = false;
	}
	put_task_struct(tick_ctx->owner);
	tick_ctx->owner = NULL;
}
#endif

/* Work first raised at the final hardirq boundary must reach ksoftirqd. */
static void rv_test_irqstate_hardirq_exit_handoff(struct kunit *test)
{
	struct irqstate_hardirq_exit_ctx *exit_ctx;
	struct rv_kunit_ctx *ctx = test->priv;
	const char *skip_reason = NULL;
	unsigned long flags;
	int ret;

	prepare_test(test, &rv_irqstate_ops.mon);
	exit_ctx = kunit_kzalloc(test, sizeof(*exit_ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, exit_ctx);
	tasklet_setup(&exit_ctx->tasklet, irqstate_hardirq_exit_tasklet);
	ret = register_trace_hardirq_context_exit(
		irqstate_hardirq_exit_raise, exit_ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);
	exit_ctx->attached = true;
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test,
						  irqstate_hardirq_exit_cleanup,
						  exit_ctx));
	attach_irqstate_test_probes();
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, detach_irqstate_test_probes,
						  NULL));

	migrate_disable();
	exit_ctx->owner = current;
	exit_ctx->owner_cpu = raw_smp_processor_id();
	exit_ctx->softirqd = this_cpu_ksoftirqd();
	if (!exit_ctx->softirqd || local_softirq_pending() ||
	    softirq_kunit_local_bh_blocked() ||
	    !wait_task_inactive(exit_ctx->softirqd, TASK_INTERRUPTIBLE)) {
		skip_reason = "Requires an idle local ksoftirqd";
		goto out_enable_migration;
	}

	local_irq_save(flags);
	if (local_softirq_pending() || softirq_kunit_local_bh_blocked() ||
	    task_is_running(exit_ctx->softirqd)) {
		local_irq_restore(flags);
		skip_reason = "Local ksoftirqd stopped being idle";
		goto out_enable_migration;
	}
	WRITE_ONCE(exit_ctx->armed, true);
	irq_enter_rcu();
	irq_exit_rcu();
	exit_ctx->tasklet_pending =
		!!(local_softirq_pending() & BIT(TASKLET_SOFTIRQ));
	exit_ctx->softirqd_already_runnable =
		!wake_up_process(exit_ctx->softirqd);
	WRITE_ONCE(exit_ctx->armed, false);
	local_irq_restore(flags);

out_enable_migration:
	migrate_enable();
	if (skip_reason) {
		kunit_skip(test, "%s", skip_reason);
		return;
	}

	kunit_release_action(test, irqstate_hardirq_exit_cleanup, exit_ctx);
	KUNIT_EXPECT_TRUE(test, exit_ctx->callback_seen);
	KUNIT_EXPECT_TRUE(test, exit_ctx->tasklet_pending);
	KUNIT_EXPECT_TRUE(test, exit_ctx->softirqd_already_runnable);
	KUNIT_EXPECT_FALSE(test, exit_ctx->bad_context);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
}

#ifdef CONFIG_NO_HZ_FULL
/* Tick-exit instrumentation must run before the final hardirq handoff. */
static void rv_test_irqstate_tick_exit_handoff(struct kunit *test)
{
	struct irqstate_tick_exit_ctx *tick_ctx;
	struct rv_kunit_ctx *ctx = test->priv;
	const char *skip_reason = NULL;
	unsigned long flags;
	unsigned int cpu;
	int ret;

	prepare_test(test, &rv_irqstate_ops.mon);
	tick_ctx = kunit_kzalloc(test, sizeof(*tick_ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, tick_ctx);
	tick_ctx->test = test;
	tick_ctx->owner = current;
	get_task_struct(tick_ctx->owner);
	tasklet_setup(&tick_ctx->tasklet, irqstate_tick_exit_tasklet);
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test,
						  irqstate_tick_exit_cleanup,
						  tick_ctx));
	ret = register_trace_tick_stop(irqstate_tick_exit_raise, tick_ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);
	tick_ctx->tick_attached = true;
	attach_irqstate_test_probes();
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, detach_irqstate_test_probes,
						  NULL));

	cpu_hotplug_disable();
	tick_ctx->hotplug_disabled = true;
	for_each_online_cpu(cpu) {
		if (tick_nohz_full_cpu(cpu))
			break;
	}
	if (cpu >= nr_cpu_ids) {
		skip_reason = "Requires an online full-NOHZ CPU";
		goto out;
	}

	cpumask_copy(&tick_ctx->saved_mask, current->cpus_ptr);
	ret = set_cpus_allowed_ptr(current, cpumask_of(cpu));
	if (ret) {
		skip_reason = "Could not run on a full-NOHZ CPU";
		goto out;
	}
	tick_ctx->affinity_changed = true;
	if (raw_smp_processor_id() != cpu) {
		skip_reason = "Did not migrate to the full-NOHZ CPU";
		goto out;
	}

	tick_ctx->owner_cpu = cpu;
	tick_ctx->softirqd = per_cpu(ksoftirqd, cpu);
	if (!tick_ctx->softirqd ||
	    !wait_task_inactive(tick_ctx->softirqd, TASK_INTERRUPTIBLE)) {
		skip_reason = "Requires an idle local ksoftirqd";
		goto out;
	}

	local_irq_save(flags);
	if (local_softirq_pending() || softirq_kunit_local_bh_blocked() ||
	    task_is_running(tick_ctx->softirqd)) {
		skip_reason = "Local ksoftirqd stopped being idle";
		goto out_restore_irqs;
	}

	tick_ctx->tick_dep_before = atomic_read(&current->tick_dep_mask);
	if (!(tick_ctx->tick_dep_before & TICK_DEP_MASK_POSIX_TIMER)) {
		tick_dep_set_task(current, TICK_DEP_BIT_POSIX_TIMER);
		tick_ctx->tick_dep_added = true;
	}
	WRITE_ONCE(tick_ctx->armed, true);
	irq_enter_rcu();
	irq_exit_rcu();
	WRITE_ONCE(tick_ctx->armed, false);
	irqstate_tick_exit_restore_dep(tick_ctx);
	tick_ctx->tasklet_pending =
		!!(local_softirq_pending() & BIT(TASKLET_SOFTIRQ));
	/* A zero return proves that the final handoff already woke the worker. */
	tick_ctx->softirqd_already_runnable =
		!wake_up_process(tick_ctx->softirqd);

out_restore_irqs:
	local_irq_restore(flags);
out:
	if (skip_reason) {
		kunit_mark_skipped(test, "%s", skip_reason);
		return;
	}

	kunit_release_action(test, irqstate_tick_exit_cleanup, tick_ctx);
	KUNIT_EXPECT_TRUE(test, tick_ctx->callback_seen);
	KUNIT_EXPECT_EQ(test, tick_ctx->tick_success, 0);
	KUNIT_EXPECT_NE(test, tick_ctx->tick_dependency, 0);
	KUNIT_EXPECT_EQ(test, tick_ctx->tick_dep_after,
			tick_ctx->tick_dep_before);
	KUNIT_EXPECT_TRUE(test, tick_ctx->tasklet_pending);
	KUNIT_EXPECT_TRUE(test, tick_ctx->softirqd_already_runnable);
	KUNIT_EXPECT_FALSE(test, tick_ctx->bad_context);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
}
#else
static void rv_test_irqstate_tick_exit_handoff(struct kunit *test)
{
	kunit_skip(test, "Requires full NO_HZ support");
}
#endif

#ifdef CONFIG_IRQ_FORCED_THREADING
struct irqstate_timer_exit_ctx {
	struct tasklet_struct tasklet;
	struct task_struct *owner;
	struct task_struct *softirqd;
	struct task_struct *timersd;
	int owner_cpu;
	bool armed;
	bool callback_seen;
	bool timer_wakeup_seen;
	bool timer_pending;
	bool tasklet_pending;
	bool softirqd_already_runnable;
	bool timersd_already_runnable;
	bool bad_context;
	bool hardirq_exit_attached;
	bool sched_waking_attached;
};

static void irqstate_timer_exit_tasklet(struct tasklet_struct *tasklet)
{
	(void)tasklet;
}

static void irqstate_timer_exit_raise(void *data, unsigned int depth)
{
	struct irqstate_timer_exit_ctx *timer_ctx = data;

	if (!READ_ONCE(timer_ctx->armed) || current != timer_ctx->owner)
		return;
	timer_ctx->callback_seen = true;
	if (depth != 1 || hardirq_count() != HARDIRQ_OFFSET ||
	    in_serving_softirq() || !irqs_disabled() ||
	    raw_smp_processor_id() != timer_ctx->owner_cpu)
		timer_ctx->bad_context = true;
	raise_ktimers_thread(TIMER_SOFTIRQ);
}

static void irqstate_timer_sched_waking(void *data, struct task_struct *task)
{
	struct irqstate_timer_exit_ctx *timer_ctx = data;

	if (!READ_ONCE(timer_ctx->armed) || task != timer_ctx->timersd)
		return;
	timer_ctx->timer_wakeup_seen = true;
	if (current != timer_ctx->owner || !irqs_disabled() ||
	    raw_smp_processor_id() != timer_ctx->owner_cpu)
		timer_ctx->bad_context = true;
	tasklet_schedule(&timer_ctx->tasklet);
}

static void irqstate_timer_exit_detach(void *data)
{
	struct irqstate_timer_exit_ctx *timer_ctx = data;

	WRITE_ONCE(timer_ctx->armed, false);
	if (timer_ctx->sched_waking_attached)
		unregister_trace_sched_waking(irqstate_timer_sched_waking,
					      timer_ctx);
	if (timer_ctx->hardirq_exit_attached)
		unregister_trace_hardirq_context_exit(
			irqstate_timer_exit_raise, timer_ctx);
	tracepoint_synchronize_unregister();
	tasklet_kill(&timer_ctx->tasklet);
}

/* Late timer-wakeup work must reach ktimerd and then ksoftirqd. */
static void rv_test_irqstate_hardirq_exit_timer_handoff(struct kunit *test)
{
	struct irqstate_timer_exit_ctx *timer_ctx;
	struct rv_kunit_ctx *ctx = test->priv;
	const char *skip_reason = NULL;
	unsigned long flags;
	int ret;

	if (!force_irqthreads()) {
		kunit_skip(test, "Requires forced IRQ threading");
		return;
	}

	prepare_test(test, &rv_irqstate_ops.mon);
	timer_ctx = kunit_kzalloc(test, sizeof(*timer_ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, timer_ctx);
	tasklet_setup(&timer_ctx->tasklet, irqstate_timer_exit_tasklet);
	ret = register_trace_hardirq_context_exit(
		irqstate_timer_exit_raise, timer_ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);
	timer_ctx->hardirq_exit_attached = true;
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, irqstate_timer_exit_detach,
						  timer_ctx));
	ret = register_trace_sched_waking(irqstate_timer_sched_waking, timer_ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);
	timer_ctx->sched_waking_attached = true;
	attach_irqstate_test_probes();
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, detach_irqstate_test_probes,
						  NULL));

	migrate_disable();
	timer_ctx->owner = current;
	timer_ctx->owner_cpu = raw_smp_processor_id();
	timer_ctx->softirqd = this_cpu_ksoftirqd();
	timer_ctx->timersd = this_cpu_read(ktimerd);
	if (!timer_ctx->softirqd || !timer_ctx->timersd ||
	    local_softirq_pending() || local_timers_pending_force_th() ||
	    softirq_kunit_local_bh_blocked() ||
	    !wait_task_inactive(timer_ctx->softirqd, TASK_INTERRUPTIBLE) ||
	    !wait_task_inactive(timer_ctx->timersd, TASK_INTERRUPTIBLE)) {
		skip_reason = "Requires idle local softirq workers";
		goto out_enable_migration;
	}

	local_irq_save(flags);
	if (local_softirq_pending() || local_timers_pending_force_th() ||
	    softirq_kunit_local_bh_blocked() ||
	    task_is_running(timer_ctx->softirqd) ||
	    task_is_running(timer_ctx->timersd)) {
		local_irq_restore(flags);
		skip_reason = "Local softirq worker stopped being idle";
		goto out_enable_migration;
	}
	WRITE_ONCE(timer_ctx->armed, true);
	irq_enter_rcu();
	irq_exit_rcu();
	timer_ctx->timer_pending =
		!!(local_timers_pending_force_th() & BIT(TIMER_SOFTIRQ));
	timer_ctx->tasklet_pending =
		!!(local_softirq_pending() & BIT(TASKLET_SOFTIRQ));
	timer_ctx->timersd_already_runnable =
		!wake_up_process(timer_ctx->timersd);
	timer_ctx->softirqd_already_runnable =
		!wake_up_process(timer_ctx->softirqd);
	WRITE_ONCE(timer_ctx->armed, false);
	local_irq_restore(flags);

out_enable_migration:
	migrate_enable();
	if (skip_reason) {
		kunit_skip(test, "%s", skip_reason);
		return;
	}

	kunit_release_action(test, irqstate_timer_exit_detach, timer_ctx);
	KUNIT_EXPECT_TRUE(test, timer_ctx->callback_seen);
	KUNIT_EXPECT_TRUE(test, timer_ctx->timer_wakeup_seen);
	KUNIT_EXPECT_TRUE(test, timer_ctx->timer_pending);
	KUNIT_EXPECT_TRUE(test, timer_ctx->tasklet_pending);
	KUNIT_EXPECT_TRUE(test, timer_ctx->timersd_already_runnable);
	KUNIT_EXPECT_TRUE(test, timer_ctx->softirqd_already_runnable);
	KUNIT_EXPECT_FALSE(test, timer_ctx->bad_context);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
}
#else
static void rv_test_irqstate_hardirq_exit_timer_handoff(struct kunit *test)
{
	kunit_skip(test, "Requires forced IRQ threading");
}
#endif

/* Exercise non-RT inline softirq processing from a real hardirq exit. */
static void rv_test_irqstate_irq_exit_inline(struct kunit *test)
{
	struct irqstate_inline_bh_ctx *inline_ctx;
	struct rv_kunit_ctx *ctx = test->priv;
	unsigned int saved_count, saved_softirq;
	unsigned long flags;
	bool ran_inline;
	int ret;

	if (IS_ENABLED(CONFIG_PREEMPT_RT)) {
		kunit_skip(test, "Requires non-RT IRQ-exit softirq processing");
		return;
	}

	prepare_test(test, &rv_irqstate_ops.mon);
	inline_ctx = kunit_kzalloc(test, sizeof(*inline_ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, inline_ctx);
	tasklet_setup(&inline_ctx->tasklet, irqstate_inline_bh_tasklet);
	tasklet_setup(&inline_ctx->entry_tasklet,
		      irqstate_inline_bh_entry_tasklet);
	tasklet_setup(&inline_ctx->exit_tasklet,
		      irqstate_inline_bh_exit_tasklet);
	inline_ctx->expected_from_hardirq = true;
	inline_ctx->inject_nested_hardirq = true;
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, irqstate_inline_bh_kill_all,
						  inline_ctx));

	ret = register_trace_softirq_context_entry(irqstate_inline_bh_entry,
						   inline_ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);
	inline_ctx->entry_attached = true;
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, irqstate_inline_bh_detach,
						  inline_ctx));
	ret = register_trace_hardirq_context_entry(
		irqstate_inline_hardirq_entry, inline_ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);
	inline_ctx->hardirq_entry_attached = true;
	ret = register_trace_hardirq_context_exit(
		irqstate_inline_hardirq_exit, inline_ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);
	inline_ctx->hardirq_exit_attached = true;
	ret = register_trace_softirq_context_exit(irqstate_inline_bh_exit,
						  inline_ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);
	inline_ctx->exit_attached = true;
	attach_irqstate_test_probes();
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, detach_irqstate_test_probes,
						  NULL));

	KUNIT_ASSERT_FALSE(test, in_interrupt());
	KUNIT_ASSERT_FALSE(test, irqs_disabled());
	KUNIT_ASSERT_EQ(test, softirq_count(), 0U);
	/* Use only nonfatal checks and normal returns after pinning current. */
	guard(migrate)();
	inline_ctx->owner = current;
	inline_ctx->owner_cpu = raw_smp_processor_id();
	inline_ctx->softirqd = this_cpu_ksoftirqd();
	KUNIT_EXPECT_NOT_NULL(test, inline_ctx->softirqd);
	if (!inline_ctx->softirqd)
		return;
	if (local_softirq_pending() ||
	    !wait_task_inactive(inline_ctx->softirqd, TASK_INTERRUPTIBLE)) {
		kunit_mark_skipped(test, "Requires an idle local ksoftirqd");
		return;
	}
	saved_count = preempt_count();
	saved_softirq = softirq_count();
	local_irq_save(flags);
	if (local_softirq_pending() || task_is_running(inline_ctx->softirqd)) {
		local_irq_restore(flags);
		kunit_mark_skipped(test, "Local ksoftirqd stopped being idle");
		return;
	}
	WRITE_ONCE(inline_ctx->armed, true);
	WRITE_ONCE(inline_ctx->raise_on_entry, true);
	WRITE_ONCE(inline_ctx->raise_on_exit, true);
	irq_enter_rcu();
	tasklet_schedule(&inline_ctx->tasklet);
	irq_exit_rcu();
	ran_inline = inline_ctx->tasklet_runs != 0;
	inline_ctx->exit_tasklet_pending =
		!!(local_softirq_pending() & BIT(TASKLET_SOFTIRQ));
	/*
	 * A zero return means the exit handoff already made ksoftirqd runnable.
	 * A one return also makes the failure path safe to drain.
	 */
	inline_ctx->softirqd_already_runnable =
		!wake_up_process(inline_ctx->softirqd);
	WRITE_ONCE(inline_ctx->armed, false);
	local_irq_restore(flags);

	/* Drain a tasklet diverted to ksoftirqd before inspecting or skipping. */
	kunit_release_action(test, irqstate_inline_bh_kill_all, inline_ctx);
	if (!ran_inline) {
		kunit_mark_skipped(test,
				   "IRQ exit diverted pending softirq to ksoftirqd");
		return;
	}

	KUNIT_EXPECT_EQ(test, inline_ctx->tasklet_runs, 1U);
	KUNIT_EXPECT_EQ(test, inline_ctx->nested_hardirq_runs, 1U);
	KUNIT_EXPECT_EQ(test, inline_ctx->entry_tasklet_runs, 1U);
	KUNIT_EXPECT_TRUE(test, inline_ctx->tasklet_serving);
	KUNIT_EXPECT_TRUE(test, inline_ctx->tasklet_same_task);
	KUNIT_EXPECT_TRUE(test, inline_ctx->tasklet_same_cpu);
	KUNIT_EXPECT_TRUE(test, inline_ctx->tasklet_irqs_enabled);
	KUNIT_EXPECT_FALSE(test, inline_ctx->tasklet_in_hardirq);
	KUNIT_EXPECT_TRUE(test, inline_ctx->tasklet_bracketed);
	KUNIT_EXPECT_TRUE(test, inline_ctx->exit_tasklet_pending);
	KUNIT_EXPECT_TRUE(test, inline_ctx->softirqd_already_runnable);
	KUNIT_EXPECT_EQ(test, inline_ctx->exit_tasklet_runs, 1U);
	KUNIT_EXPECT_EQ(test, inline_ctx->entries, 1U);
	KUNIT_EXPECT_EQ(test, inline_ctx->exits, 1U);
	KUNIT_EXPECT_EQ(test, inline_ctx->hardirq_entries, 1U);
	KUNIT_EXPECT_EQ(test, inline_ctx->hardirq_exits, 1U);
	KUNIT_EXPECT_EQ(test, inline_ctx->event_index, 4U);
	KUNIT_EXPECT_TRUE(test, inline_ctx->entry_from_hardirq);
	KUNIT_EXPECT_TRUE(test, inline_ctx->exit_from_hardirq);
	KUNIT_EXPECT_FALSE(test, inline_ctx->bad_boundary_context);
	KUNIT_EXPECT_FALSE(test, inline_ctx->bad_boundary_cpu);
	KUNIT_EXPECT_FALSE(test, inline_ctx->bad_provenance);
	KUNIT_EXPECT_FALSE(test, inline_ctx->bad_order);
	KUNIT_EXPECT_EQ(test, preempt_count(), saved_count);
	KUNIT_EXPECT_EQ(test, softirq_count(), saved_softirq);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
	kunit_info(test,
		   "IRQ-exit inline tasklets=%u/%u handoff=%u events=%u/%u restored=%u\n",
		   inline_ctx->tasklet_runs, inline_ctx->exit_tasklet_runs,
		   inline_ctx->softirqd_already_runnable,
		   inline_ctx->entries, inline_ctx->exits,
		   preempt_count() == saved_count &&
		   softirq_count() == saved_softirq);
}

/* Force the RT BH-disabled to inline-serving transition. */
static void rv_test_irqstate_rt_inline_bh(struct kunit *test)
{
	struct irqstate_inline_bh_ctx *inline_ctx;
	struct rv_kunit_ctx *ctx = test->priv;
	unsigned int saved_count, saved_softirq;
	int ret;

	if (!IS_ENABLED(CONFIG_PREEMPT_RT)) {
		kunit_skip(test, "Requires RT inline softirq processing");
		return;
	}

	prepare_test(test, &rv_irqstate_ops.mon);
	inline_ctx = kunit_kzalloc(test, sizeof(*inline_ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, inline_ctx);
	inline_ctx->owner = current;
	inline_ctx->expected_from_hardirq = false;
	tasklet_setup(&inline_ctx->tasklet, irqstate_inline_bh_tasklet);
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, irqstate_inline_bh_kill,
						  inline_ctx));

	ret = register_trace_softirq_context_entry(irqstate_inline_bh_entry, inline_ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);
	inline_ctx->entry_attached = true;
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, irqstate_inline_bh_detach,
						  inline_ctx));
	ret = register_trace_softirq_context_exit(irqstate_inline_bh_exit, inline_ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);
	inline_ctx->exit_attached = true;
	attach_irqstate_test_probes();
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, detach_irqstate_test_probes,
						  NULL));

	KUNIT_ASSERT_TRUE(test, preemptible());
	KUNIT_ASSERT_EQ(test, softirq_count(), 0U);
	saved_count = preempt_count();
	saved_softirq = softirq_count();
	local_bh_disable();
	inline_ctx->owner_cpu = raw_smp_processor_id();
	WRITE_ONCE(inline_ctx->armed, true);
	tasklet_schedule(&inline_ctx->tasklet);
	local_bh_enable();
	WRITE_ONCE(inline_ctx->armed, false);
	kunit_release_action(test, irqstate_inline_bh_kill, inline_ctx);

	KUNIT_EXPECT_EQ(test, inline_ctx->tasklet_runs, 1U);
	KUNIT_EXPECT_TRUE(test, inline_ctx->tasklet_serving);
	KUNIT_EXPECT_TRUE(test, inline_ctx->tasklet_same_task);
	KUNIT_EXPECT_TRUE(test, inline_ctx->tasklet_same_cpu);
	KUNIT_EXPECT_TRUE(test, inline_ctx->tasklet_irqs_enabled);
	KUNIT_EXPECT_TRUE(test, inline_ctx->tasklet_bracketed);
	KUNIT_EXPECT_EQ(test, inline_ctx->entries, 1U);
	KUNIT_EXPECT_EQ(test, inline_ctx->exits, 1U);
	KUNIT_EXPECT_EQ(test, inline_ctx->event_index, 2U);
	KUNIT_EXPECT_FALSE(test, inline_ctx->entry_from_hardirq);
	KUNIT_EXPECT_FALSE(test, inline_ctx->exit_from_hardirq);
	KUNIT_EXPECT_FALSE(test, inline_ctx->bad_boundary_context);
	KUNIT_EXPECT_FALSE(test, inline_ctx->bad_boundary_cpu);
	KUNIT_EXPECT_FALSE(test, inline_ctx->bad_provenance);
	KUNIT_EXPECT_FALSE(test, inline_ctx->bad_order);
	KUNIT_EXPECT_EQ(test, preempt_count(), saved_count);
	KUNIT_EXPECT_EQ(test, softirq_count(), saved_softirq);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
	kunit_info(test,
		   "RT inline BH tasklet=%u same_task=%u same_cpu=%u serving=%u events=%u/%u restored=%u\n",
		   inline_ctx->tasklet_runs, inline_ctx->tasklet_same_task,
		   inline_ctx->tasklet_same_cpu, inline_ctx->tasklet_serving,
		   inline_ctx->entries, inline_ctx->exits,
		   preempt_count() == saved_count &&
		   softirq_count() == saved_softirq);
}

static void irqstate_empty_worker_entry(void *data, bool serving,
					bool from_hardirq)
{
	struct irqstate_empty_worker_ctx *empty_ctx = data;

	if (!irqstate_empty_worker_matches(empty_ctx))
		return;
	empty_ctx->entries++;
	if (empty_ctx->event_index != 0 || empty_ctx->inside_pair)
		empty_ctx->bad_order = true;
	empty_ctx->event_index++;
	empty_ctx->inside_pair = true;
	if (!serving || from_hardirq || !in_serving_softirq() ||
	    !irqs_disabled())
		empty_ctx->bad_boundary_context = true;
	if (raw_smp_processor_id() != READ_ONCE(empty_ctx->owner_cpu))
		empty_ctx->bad_boundary_cpu = true;
}

static void irqstate_empty_worker_exit(void *data, bool serving,
				       bool from_hardirq)
{
	struct irqstate_empty_worker_ctx *empty_ctx = data;

	if (!irqstate_empty_worker_matches(empty_ctx))
		return;
	empty_ctx->exits++;
	if (empty_ctx->event_index != 1 || !empty_ctx->inside_pair)
		empty_ctx->bad_order = true;
	empty_ctx->event_index++;
	if (!serving || from_hardirq || !in_serving_softirq() ||
	    !irqs_disabled())
		empty_ctx->bad_boundary_context = true;
	if (raw_smp_processor_id() != READ_ONCE(empty_ctx->owner_cpu))
		empty_ctx->bad_boundary_cpu = true;
	empty_ctx->actions_at_exit = empty_ctx->action_entries ||
				     empty_ctx->action_exits;
	empty_ctx->inside_pair = false;
}

static void irqstate_empty_worker_action_entry(void *data, unsigned int vec_nr)
{
	struct irqstate_empty_worker_ctx *empty_ctx = data;

	if (!irqstate_empty_worker_matches(empty_ctx) ||
	    !empty_ctx->inside_pair)
		return;
	(void)vec_nr;
	empty_ctx->action_entries++;
	if (raw_smp_processor_id() != empty_ctx->owner_cpu)
		empty_ctx->bad_boundary_cpu = true;
}

static void irqstate_empty_worker_action_exit(void *data, unsigned int vec_nr)
{
	struct irqstate_empty_worker_ctx *empty_ctx = data;

	if (!irqstate_empty_worker_matches(empty_ctx) ||
	    !empty_ctx->inside_pair)
		return;
	(void)vec_nr;
	empty_ctx->action_exits++;
	if (raw_smp_processor_id() != empty_ctx->owner_cpu)
		empty_ctx->bad_boundary_cpu = true;
}

static void irqstate_empty_worker_detach(void *data)
{
	struct irqstate_empty_worker_ctx *empty_ctx = data;

	WRITE_ONCE(empty_ctx->armed, false);
	if (empty_ctx->action_exit_attached)
		unregister_trace_softirq_exit(irqstate_empty_worker_action_exit,
					      empty_ctx);
	if (empty_ctx->action_entry_attached)
		unregister_trace_softirq_entry(irqstate_empty_worker_action_entry,
					       empty_ctx);
	if (empty_ctx->exit_attached)
		unregister_trace_softirq_context_exit(irqstate_empty_worker_exit,
						      empty_ctx);
	if (empty_ctx->entry_attached)
		unregister_trace_softirq_context_entry(irqstate_empty_worker_entry,
						       empty_ctx);
	tracepoint_synchronize_unregister();
}

static void run_irqstate_rt_empty_worker(struct kunit *test, bool real_worker)
{
	struct irqstate_empty_worker_ctx *empty_ctx;
	struct rv_kunit_ctx *ctx = test->priv;
	unsigned int saved_count, saved_softirq;
	bool order_ok, restored;
	int ret;

	if (!IS_ENABLED(CONFIG_PREEMPT_RT)) {
		kunit_skip(test, "Requires RT worker serving boundaries");
		return;
	}

	prepare_test(test, &rv_irqstate_ops.mon);
	empty_ctx = kunit_kzalloc(test, sizeof(*empty_ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, empty_ctx);
	empty_ctx->real_worker = real_worker;

	ret = register_trace_softirq_context_entry(irqstate_empty_worker_entry,
						   empty_ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);
	empty_ctx->entry_attached = true;
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test,
						  irqstate_empty_worker_detach,
						  empty_ctx));
	ret = register_trace_softirq_context_exit(irqstate_empty_worker_exit,
						  empty_ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);
	empty_ctx->exit_attached = true;
	ret = register_trace_softirq_entry(irqstate_empty_worker_action_entry,
					   empty_ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);
	empty_ctx->action_entry_attached = true;
	ret = register_trace_softirq_exit(irqstate_empty_worker_action_exit,
					  empty_ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);
	empty_ctx->action_exit_attached = true;
	attach_irqstate_test_probes();
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, detach_irqstate_test_probes,
						  NULL));

	KUNIT_ASSERT_TRUE(test, preemptible());
	KUNIT_ASSERT_FALSE(test, irqs_disabled());
	KUNIT_ASSERT_EQ(test, softirq_count(), 0U);
	saved_count = preempt_count();
	saved_softirq = softirq_count();
	if (real_worker) {
		cpus_read_lock();
		ret = softirq_kunit_run_empty_ksoftirqd(&empty_ctx->owner,
							&empty_ctx->owner_cpu,
							&empty_ctx->armed);
		cpus_read_unlock();
	} else {
		migrate_disable();
		WRITE_ONCE(empty_ctx->owner_cpu, smp_processor_id());
		WRITE_ONCE(empty_ctx->owner, current);
		/* Publish the owner before globally registered callbacks match it. */
		smp_store_release(&empty_ctx->armed, true);
		softirq_kunit_run_empty_worker();
		WRITE_ONCE(empty_ctx->armed, false);
		migrate_enable();
		ret = 0;
	}
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_NOT_NULL(test, empty_ctx->owner);

	order_ok = empty_ctx->event_index == 2 && !empty_ctx->inside_pair &&
		   !empty_ctx->bad_order;
	restored = preempt_count() == saved_count &&
		   softirq_count() == saved_softirq && !irqs_disabled();
	KUNIT_EXPECT_EQ(test, empty_ctx->entries, 1U);
	KUNIT_EXPECT_EQ(test, empty_ctx->exits, 1U);
	KUNIT_EXPECT_EQ(test, empty_ctx->action_entries, 0U);
	KUNIT_EXPECT_EQ(test, empty_ctx->action_exits, 0U);
	KUNIT_EXPECT_TRUE(test, order_ok);
	KUNIT_EXPECT_FALSE(test, empty_ctx->bad_boundary_context);
	KUNIT_EXPECT_FALSE(test, empty_ctx->bad_boundary_cpu);
	KUNIT_EXPECT_FALSE(test, empty_ctx->actions_at_exit);
	KUNIT_EXPECT_EQ(test, preempt_count(), saved_count);
	KUNIT_EXPECT_EQ(test, softirq_count(), saved_softirq);
	KUNIT_EXPECT_FALSE(test, irqs_disabled());
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
	if (real_worker)
		kunit_info(test,
			   "RT smpboot empty worker events=%u/%u actions=%u/%u order=%u serving=%u same_cpu=%u restored=%u\n",
			   empty_ctx->entries, empty_ctx->exits,
			   empty_ctx->action_entries, empty_ctx->action_exits,
			   order_ok, !empty_ctx->bad_boundary_context,
			   !empty_ctx->bad_boundary_cpu, restored);
	else
		kunit_info(test,
			   "RT empty worker events=%u/%u actions=%u/%u order=%u serving=%u same_cpu=%u restored=%u\n",
			   empty_ctx->entries, empty_ctx->exits,
			   empty_ctx->action_entries, empty_ctx->action_exits,
			   order_ok, !empty_ctx->bad_boundary_context,
			   !empty_ctx->bad_boundary_cpu, restored);
}

/* Exercise an RT worker interval which executes no softirq action. */
static void rv_test_irqstate_rt_empty_worker(struct kunit *test)
{
	run_irqstate_rt_empty_worker(test, false);
}

/* Exercise the real ksoftirqd task and its smpboot predicate. */
static void rv_test_irqstate_rt_smpboot_empty_worker(struct kunit *test)
{
	run_irqstate_rt_empty_worker(test, true);
}

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
	int ret;

	if (!IS_ENABLED(CONFIG_PREEMPT_RT)) {
		kunit_skip(test, "Requires preemptible softirq processing");
		return;
	}

	prepare_test(test, &rv_irqstate_ops.mon);
	preempt_ctx = kunit_kzalloc(test, sizeof(*preempt_ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, preempt_ctx);
	init_completion(&preempt_ctx->tasklet_done);
	init_completion(&preempt_ctx->worker_done);
	worker = kthread_create(irqstate_preempt_worker, preempt_ctx,
				"irqstate-preempt");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, worker);
	preempt_ctx->worker = worker;
	ret = kunit_add_action_or_reset(test, stop_irqstate_preempt_worker,
					worker);
	KUNIT_ASSERT_EQ(test, ret, 0);

	attach_irqstate_test_probes();
	ret = kunit_add_action_or_reset(test, detach_irqstate_test_probes, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);

	scoped_guard(migrate) {
		kthread_bind(worker, smp_processor_id());
		sched_set_fifo(worker);
		tasklet_setup(&preempt_ctx->tasklet, irqstate_preempt_tasklet);
		tasklet_schedule(&preempt_ctx->tasklet);
		completed = wait_for_completion_timeout(&preempt_ctx->tasklet_done,
							HZ);
		tasklet_kill(&preempt_ctx->tasklet);
		if (completed)
			completed = wait_for_completion_timeout(&preempt_ctx->worker_done,
								HZ);
	}
	KUNIT_ASSERT_NE(test, completed, 0UL);
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
	int reactions[3];
	int ret;
	bool serving;

	if (!IS_ENABLED(CONFIG_PREEMPT_RT)) {
		kunit_skip(test, "Requires RT serving-softirq accounting");
		return;
	}

	prepare_test(test, &rv_irqstate_ops.mon);
	attach_irqstate_test_probes();
	ret = kunit_add_action_or_reset(test, detach_irqstate_test_probes, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	saved_count = preempt_count();
	saved_softirq = softirq_count();

	scoped_guard(migrate) {
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

		reactions[0] = READ_ONCE(ctx->reactions);
		local_irq_save(flags);
		__irq_enter_raw();
		reactions[1] = READ_ONCE(ctx->reactions);
		rv_irqstate_ops.handle_hardirq_exit(NULL, 1);
		reactions[2] = READ_ONCE(ctx->reactions);
		__irq_exit_raw();
		local_irq_restore(flags);
	}

	KUNIT_EXPECT_TRUE(test, serving);
	KUNIT_EXPECT_EQ(test, preempt_count(), saved_count);
	KUNIT_EXPECT_EQ(test, softirq_count(), saved_softirq);
	kunit_info(test, "RT preparation IRQ serving=%u restored=%u\n",
		   serving, preempt_count() == saved_count &&
		   softirq_count() == saved_softirq);
	RV_KUNIT_EXPECT_OBSERVED(test, ctx, reactions[0], 0);
	RV_KUNIT_EXPECT_OBSERVED(test, ctx, reactions[1], 0);
	RV_KUNIT_EXPECT_OBSERVED(test, ctx, reactions[2], 1);
}

/* A real task switch is legal before handle_softirqs(), too. */
static void rv_test_irqstate_rt_prepare_switch(struct kunit *test)
{
	struct rv_kunit_ctx *ctx = test->priv;
	struct irqstate_preempt_ctx *preempt_ctx;
	struct task_struct *worker;
	unsigned int saved_count, saved_softirq;
	unsigned long completed, flags;
	int reactions[3];
	int ret;
	bool preempted, serving;

	if (!IS_ENABLED(CONFIG_PREEMPT_RT)) {
		kunit_skip(test, "Requires RT serving-softirq accounting");
		return;
	}

	prepare_test(test, &rv_irqstate_ops.mon);
	preempt_ctx = kunit_kzalloc(test, sizeof(*preempt_ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, preempt_ctx);
	init_completion(&preempt_ctx->worker_done);
	worker = kthread_create(irqstate_preempt_worker, preempt_ctx,
				"irqstate-prepare");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, worker);
	ret = kunit_add_action_or_reset(test, stop_irqstate_preempt_worker,
					worker);
	KUNIT_ASSERT_EQ(test, ret, 0);
	attach_irqstate_test_probes();
	ret = kunit_add_action_or_reset(test, detach_irqstate_test_probes, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	saved_count = preempt_count();
	saved_softirq = softirq_count();

	scoped_guard(migrate) {
		kthread_bind(worker, smp_processor_id());
		sched_set_fifo(worker);
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

		completed = wait_for_completion_timeout(&preempt_ctx->worker_done,
							HZ);
		reactions[0] = READ_ONCE(ctx->reactions);
		local_irq_save(flags);
		__irq_enter_raw();
		reactions[1] = READ_ONCE(ctx->reactions);
		rv_irqstate_ops.handle_hardirq_exit(NULL, 1);
		reactions[2] = READ_ONCE(ctx->reactions);
		__irq_exit_raw();
		local_irq_restore(flags);
	}
	KUNIT_EXPECT_NE(test, completed, 0);
	KUNIT_EXPECT_TRUE(test, serving);
	KUNIT_EXPECT_TRUE(test, preempted);
	KUNIT_EXPECT_FALSE(test, preempt_ctx->worker_in_softirq);
	KUNIT_EXPECT_EQ(test, preempt_count(), saved_count);
	KUNIT_EXPECT_EQ(test, softirq_count(), saved_softirq);
	kunit_info(test, "RT preparation switch serving=%u preempted=%u worker_serving=%u restored=%u\n",
		   serving, preempted, preempt_ctx->worker_in_softirq,
		   preempt_count() == saved_count && softirq_count() == saved_softirq);
	RV_KUNIT_EXPECT_OBSERVED(test, ctx, reactions[0], 0);
	RV_KUNIT_EXPECT_OBSERVED(test, ctx, reactions[1], 0);
	RV_KUNIT_EXPECT_OBSERVED(test, ctx, reactions[2], 1);
}

#else
static void rv_test_irqstate_hardirq_exit_handoff(struct kunit *test)
{
	kunit_skip(test, "Requires built-in RV tests");
}

static void rv_test_irqstate_tick_exit_handoff(struct kunit *test)
{
	kunit_skip(test, "Requires built-in RV tests");
}

static void rv_test_irqstate_hardirq_exit_timer_handoff(struct kunit *test)
{
	kunit_skip(test, "Requires built-in RV tests");
}

static void rv_test_irqstate_irq_exit_inline(struct kunit *test)
{
	kunit_skip(test, "Requires built-in RV tests");
}

static void rv_test_irqstate_rt_inline_bh(struct kunit *test)
{
	kunit_skip(test, "Requires built-in RV tests");
}

static void rv_test_irqstate_rt_empty_worker(struct kunit *test)
{
	kunit_skip(test, "Requires built-in RV tests");
}

static void rv_test_irqstate_rt_smpboot_empty_worker(struct kunit *test)
{
	kunit_skip(test, "Requires built-in RV tests");
}

static void rv_test_irqstate_preempt(struct kunit *test)
{
	kunit_skip(test, "Requires built-in RV tests");
}

static void rv_test_irqstate_rt_prepare_irq(struct kunit *test)
{
	kunit_skip(test, "Requires built-in RV tests");
}

static void rv_test_irqstate_rt_prepare_switch(struct kunit *test)
{
	kunit_skip(test, "Requires built-in RV tests");
}
#endif

#else
#define rv_test_irqstate rv_test_stub
#define rv_test_irqstate_shared_actions rv_test_stub
#define rv_test_irqstate_depth rv_test_stub
#define rv_test_irqstate_depth_overflow rv_test_stub
#define rv_test_irqstate_sync rv_test_stub
#define rv_test_irqstate_live_toggle rv_test_stub
#define rv_test_irqstate_hardirq_exit_handoff rv_test_stub
#define rv_test_irqstate_tick_exit_handoff rv_test_stub
#define rv_test_irqstate_hardirq_exit_timer_handoff rv_test_stub
#define rv_test_irqstate_irq_exit_inline rv_test_stub
#define rv_test_irqstate_rt_inline_bh rv_test_stub
#define rv_test_irqstate_rt_empty_worker rv_test_stub
#define rv_test_irqstate_rt_smpboot_empty_worker rv_test_stub
#define rv_test_irqstate_preempt rv_test_stub
#define rv_test_irqstate_rt_prepare_irq rv_test_stub
#define rv_test_irqstate_rt_prepare_switch rv_test_stub
#endif
