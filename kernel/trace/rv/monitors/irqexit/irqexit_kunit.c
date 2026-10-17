// SPDX-License-Identifier: GPL-2.0
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/hardirq.h>
#include <linux/irqflags.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/rv.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <rv/kunit.h>
#include <rv_trace.h>

#include "irqexit_kunit.h"

#if IS_REACHABLE(CONFIG_RV_MON_IRQEXIT)

static void irqexit_test_switch(struct task_struct *prev, struct task_struct *next)
{
	rv_mock_current(prev);
	rv_irqexit_ops.handle_sched_switch(NULL, true, prev, next, TASK_RUNNING);
	rv_mock_current(next);
}

static void rv_test_irqexit(struct kunit *test)
{
	struct task_struct *owner = rv_kunit_alloc_mock_task(test);
	struct task_struct *other = rv_kunit_alloc_mock_task(test);
	struct rv_kunit_ctx *ctx = test->priv;
	unsigned long flags;

	owner->pid = 101;
	other->pid = 102;
	prepare_test(test, &rv_irqexit_ops.mon);
	rv_mock_current(owner);

	local_irq_save(flags);
	/* The scheduling loop may return without an actual task switch. */
	rv_irqexit_ops.handle_resched_entry(NULL, owner);
	rv_irqexit_ops.handle_resched_exit(NULL, owner);

	/* A different task's IRQ-exit loop cannot complete owner's loop. */
	rv_irqexit_ops.handle_resched_entry(NULL, owner);
	irqexit_test_switch(owner, other);
	rv_irqexit_ops.handle_resched_entry(NULL, other);
	__irq_enter_raw();
	__irq_exit_raw();
	rv_irqexit_ops.handle_resched_exit(NULL, other);
	irqexit_test_switch(other, owner);

	/* Both tasks can have suspended invocations at the same time. */
	irqexit_test_switch(owner, other);
	rv_irqexit_ops.handle_resched_entry(NULL, other);
	irqexit_test_switch(other, owner);
	rv_irqexit_ops.handle_resched_exit(NULL, owner);
	irqexit_test_switch(owner, other);
	rv_irqexit_ops.handle_resched_exit(NULL, other);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
	local_irq_restore(flags);
}

static void rv_test_irqexit_invalid(struct kunit *test)
{
	struct task_struct *owner = rv_kunit_alloc_mock_task(test);
	struct task_struct *other = rv_kunit_alloc_mock_task(test);
	struct rv_kunit_ctx *ctx = test->priv;
	unsigned long flags;

	owner->pid = 101;
	other->pid = 102;
	prepare_test(test, &rv_irqexit_ops.mon);
	rv_mock_current(owner);

	KUNIT_ASSERT_FALSE(test, irqs_disabled());
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqexit_ops.handle_resched_entry(NULL, owner);

	local_irq_save(flags);
	/* Scheduling must not start before the hardirq count is removed. */
	__irq_enter_raw();
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqexit_ops.handle_resched_entry(NULL, owner);
	__irq_exit_raw();

	/* A return with hardirq state still active is invalid as well. */
	rv_irqexit_ops.handle_resched_entry(NULL, owner);
	__irq_enter_raw();
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqexit_ops.handle_resched_exit(NULL, owner);
	__irq_exit_raw();

	rv_irqexit_ops.handle_resched_entry(NULL, owner);
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqexit_ops.handle_resched_entry(NULL, owner);

	/* The first exit synchronizes; the second unmatched exit must fail. */
	rv_irqexit_ops.handle_resched_exit(NULL, owner);
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqexit_ops.handle_resched_exit(NULL, owner);

	/* The state permits exit, but its task identity must still agree. */
	rv_irqexit_ops.handle_resched_entry(NULL, owner);
	rv_mock_current(other);
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqexit_ops.handle_resched_exit(NULL, owner);
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqexit_ops.handle_resched_entry(NULL, owner);

	/* Matching current is insufficient if the invocation is suspended. */
	rv_mock_current(owner);
	rv_irqexit_ops.handle_resched_entry(NULL, owner);
	irqexit_test_switch(owner, other);
	rv_mock_current(owner);
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqexit_ops.handle_resched_exit(NULL, owner);

	rv_irqexit_ops.handle_resched_entry(NULL, owner);
	local_irq_restore(flags);
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqexit_ops.handle_resched_exit(NULL, owner);

	/* Even an unsynchronized bad return must not be silently skipped. */
	rv_mock_current(other);
	RV_KUNIT_EXPECT_REACTION_HERE(test, ctx)
		rv_irqexit_ops.handle_resched_exit(NULL, owner);
}

static void rv_test_irqexit_sync(struct kunit *test)
{
	struct task_struct *owner = rv_kunit_alloc_mock_task(test);
	struct task_struct *other = rv_kunit_alloc_mock_task(test);
	struct rv_kunit_ctx *ctx = test->priv;
	unsigned long flags;

	owner->pid = 101;
	other->pid = 102;
	prepare_test(test, &rv_irqexit_ops.mon);
	rv_mock_current(owner);

	local_irq_save(flags);
	/* Switch events alone do not synchronize an already active loop. */
	irqexit_test_switch(owner, other);
	irqexit_test_switch(other, owner);
	rv_irqexit_ops.handle_resched_exit(NULL, owner);
	rv_irqexit_ops.handle_resched_entry(NULL, owner);

	/* Model disable/reset/re-enable while that invocation is suspended. */
	rv_irqexit_ops.mon.rv_this->enabled = 0;
	irqexit_test_switch(owner, other);
	rv_irqexit_ops.handle_resched_entry(NULL, other);
	rv_irqexit_ops.handle_resched_exit(NULL, other);
	rv_irqexit_ops.mon.task_reset(owner);
	rv_irqexit_ops.mon.task_reset(other);
	rv_irqexit_ops.mon.rv_this->enabled = 1;
	irqexit_test_switch(other, owner);
	rv_irqexit_ops.handle_resched_exit(NULL, owner);
	rv_irqexit_ops.handle_resched_entry(NULL, owner);
	rv_irqexit_ops.handle_resched_exit(NULL, owner);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
	local_irq_restore(flags);
}

#ifndef MODULE
struct irqexit_live_ctx {
	struct task_struct *owner;
	struct task_struct *worker;
	const char *skip_reason;
	struct completion worker_done;
	unsigned int entries;
	unsigned int exits;
	unsigned int switches_out;
	unsigned int switches_in;
	unsigned int entry_cpu;
	unsigned int exit_cpu;
	unsigned int switch_out_cpu;
	unsigned int switch_in_cpu;
	unsigned int target_cpu;
	int affinity_result;
	cpumask_t saved_mask;
	cpumask_t test_mask;
	bool migrate;
	bool affinity_changed;
	bool observer_attached;
};

/* Parent fixtures run in the suite runner, which owns rv_interface_lock. */
static int irqexit_live_fixture_init(struct kunit *test)
{
	struct rv_monitor *mon = rv_irqexit_ops.mon.rv_this;
	struct irqexit_live_ctx *live;
	int ret;

	if (mon->enabled)
		return -EBUSY;
	live = kunit_kzalloc(test, sizeof(*live), GFP_KERNEL);
	if (!live)
		return -ENOMEM;

	cpus_read_lock();
	ret = mon->enable();
	if (ret) {
		cpus_read_unlock();
		kunit_kfree(test, live);
		return ret;
	}
	kunit_register_params_array(test, live, 1, NULL);
	return 0;
}

static void irqexit_live_fixture_exit(struct kunit *test)
{
	const struct irqexit_live_ctx *live = test->params_array.params;
	struct rv_monitor *mon = rv_irqexit_ops.mon.rv_this;

	mon->enabled = 0;
	mon->disable();
	cpus_read_unlock();
	/* Retain the child's explicit skip reason on the parent case. */
	if (live->skip_reason)
		kunit_mark_skipped(test, "%s", live->skip_reason);
}

static int irqexit_live_worker(void *data)
{
	struct irqexit_live_ctx *live = data;

	if (live->migrate)
		live->affinity_result =
			set_cpus_allowed_ptr(live->owner, cpumask_of(live->target_cpu));
	complete(&live->worker_done);
	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (!kthread_should_stop())
			schedule();
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static void irqexit_observe_event(void *data, int id, char *state,
				  char *event, char *next_state, bool final_state)
{
	struct irqexit_live_ctx *live = data;

	if (id != live->owner->pid)
		return;
	if (!strcmp(event, "resched_entry")) {
		live->entry_cpu = raw_smp_processor_id();
		live->entries++;
	} else if (!strcmp(event, "resched_exit")) {
		live->exit_cpu = raw_smp_processor_id();
		live->exits++;
	} else if (!strcmp(event, "switch_out") && !strcmp(state, "rescheduling")) {
		if (!live->switches_out)
			live->switch_out_cpu = raw_smp_processor_id();
		live->switches_out++;
	} else if (!strcmp(event, "switch_in") && !strcmp(state, "suspended")) {
		if (!live->switches_in)
			live->switch_in_cpu = raw_smp_processor_id();
		live->switches_in++;
	}
}

static void irqexit_live_cleanup(void *data)
{
	struct irqexit_live_ctx *live = data;
	struct rv_monitor *mon = rv_irqexit_ops.mon.rv_this;

	mon->enabled = 0;
	if (live->observer_attached)
		unregister_trace_event_irqexit(irqexit_observe_event, live);
	tracepoint_synchronize_unregister();
	if (live->worker)
		kthread_stop(live->worker);
	if (live->affinity_changed && !(READ_ONCE(live->owner->flags) & PF_EXITING))
		KUNIT_EXPECT_EQ(kunit_get_current_test(),
				set_cpus_allowed_ptr(live->owner, &live->saved_mask), 0);
	put_task_struct(live->owner);
	live->owner = NULL;

	RV_KUNIT_EXPECT_NO_REACTION(kunit_get_current_test(), active_ctx);
	mon->react = mon->reactor ? mon->reactor->react : NULL;
	active_ctx = NULL;
	rv_mock_current(NULL);
}

static void rv_test_irqexit_live(struct kunit *test)
{
	struct rv_monitor *mon = rv_irqexit_ops.mon.rv_this;
	struct rv_kunit_ctx *ctx = test->priv;
	struct irqexit_live_ctx *live = (void *)test->param_value;
	struct task_struct *worker;
	unsigned int entries, exits, switches_out, switches_in;
	unsigned int first_entries, first_switches;
	unsigned long flags;
	bool worker_done;

	KUNIT_ASSERT_FALSE(test, mon->enabled);
	live->owner = current;
	get_task_struct(live->owner);
	init_completion(&live->worker_done);

	active_ctx = ctx;
	mon->react = rv_kunit_mock_react;
	rv_mock_current(NULL);
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, irqexit_live_cleanup, live));
	/* The worker must run on this CPU inside the explicit scheduling loop. */
	guard(migrate)();
	worker = kthread_create(irqexit_live_worker, live, "rv-irqexit-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, worker);
	live->worker = worker;
	kthread_bind(worker, smp_processor_id());
	sched_set_fifo(worker);
	KUNIT_ASSERT_EQ(test, 0,
			register_trace_event_irqexit(irqexit_observe_event, live));
	live->observer_attached = true;

	mon->enabled = 1;

	KUNIT_ASSERT_EQ(test, preempt_count(), 0);
	local_irq_save(flags);
	preempt_schedule_irq();
	first_entries = live->entries;
	first_switches = live->switches_out;
	wake_up_process(worker);
	preempt_schedule_irq();
	entries = live->entries;
	exits = live->exits;
	switches_out = live->switches_out;
	switches_in = live->switches_in;
	worker_done = completion_done(&live->worker_done);
	local_irq_restore(flags);

	/* Accepted model events prove that the real boundary probes ran. */
	KUNIT_EXPECT_GT(test, entries, 0);
	KUNIT_EXPECT_GT(test, first_entries, 0);
	KUNIT_EXPECT_GT(test, entries, first_entries);
	KUNIT_EXPECT_EQ(test, entries, exits);
	KUNIT_EXPECT_EQ(test, switches_out, switches_in);
	KUNIT_EXPECT_GT(test, switches_out, first_switches);
	KUNIT_EXPECT_TRUE(test, worker_done);
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
	kunit_info(test, "live IRQ-exit entries=%u exits=%u switches=%u\n",
		   entries, exits, switches_out);
	mon->enabled = 0;
}

static void rv_test_irqexit_migrate(struct kunit *test)
{
	struct rv_monitor *mon = rv_irqexit_ops.mon.rv_this;
	struct rv_kunit_ctx *ctx = test->priv;
	struct irqexit_live_ctx *live = (void *)test->param_value;
	struct task_struct *worker;
	unsigned int source_cpu, resumed_cpu;
	unsigned long flags;
	bool affinity_restored;
	int restore_result;

	KUNIT_ASSERT_FALSE(test, mon->enabled);
	live->owner = current;
	get_task_struct(live->owner);
	init_completion(&live->worker_done);
	active_ctx = ctx;
	mon->react = rv_kunit_mock_react;
	rv_mock_current(NULL);
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, irqexit_live_cleanup, live));
	if (num_online_cpus() < 2) {
		live->skip_reason = "Requires two online CPUs";
		kunit_skip(test, "%s", live->skip_reason);
		return;
	}
	cpumask_copy(&live->saved_mask, current->cpus_ptr);
	cpumask_and(&live->test_mask, &live->saved_mask, cpu_online_mask);
	if (cpumask_weight(&live->test_mask) < 2) {
		live->skip_reason = "Requires two allowed online CPUs";
		kunit_skip(test, "%s", live->skip_reason);
		return;
	}
	source_cpu = cpumask_first(&live->test_mask);
	live->target_cpu = cpumask_any_but(&live->test_mask, source_cpu);
	KUNIT_ASSERT_EQ(test, set_cpus_allowed_ptr(current, cpumask_of(source_cpu)), 0);
	live->affinity_changed = true;
	live->migrate = true;
	worker = kthread_create(irqexit_live_worker, live, "rv-irqexit-mig");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, worker);
	live->worker = worker;
	kthread_bind(worker, source_cpu);
	sched_set_fifo(worker);
	KUNIT_ASSERT_EQ(test, 0,
			register_trace_event_irqexit(irqexit_observe_event, live));
	live->observer_attached = true;
	mon->enabled = 1;

	KUNIT_ASSERT_EQ(test, preempt_count(), 0);
	local_irq_save(flags);
	live->entries = 0;
	live->exits = 0;
	live->switches_out = 0;
	live->switches_in = 0;
	/*
	 * The FIFO worker runs only after this task switches out inside the
	 * real helper. It moves the queued, non-running owner to another CPU.
	 * Do not migration-disable the owner: migration is the property tested.
	 */
	wake_up_process(worker);
	preempt_schedule_irq();
	resumed_cpu = raw_smp_processor_id();
	mon->enabled = 0;
	local_irq_restore(flags);
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&live->worker_done, 5 * HZ), 0UL);
	KUNIT_EXPECT_EQ(test, live->affinity_result, 0);
	KUNIT_EXPECT_EQ(test, live->entries, 1U);
	KUNIT_EXPECT_EQ(test, live->exits, 1U);
	KUNIT_EXPECT_GT(test, live->switches_out, 0U);
	KUNIT_EXPECT_EQ(test, live->switches_out, live->switches_in);
	KUNIT_EXPECT_EQ(test, live->entry_cpu, source_cpu);
	KUNIT_EXPECT_EQ(test, live->switch_out_cpu, source_cpu);
	KUNIT_EXPECT_EQ(test, live->switch_in_cpu, live->target_cpu);
	KUNIT_EXPECT_EQ(test, live->exit_cpu, live->target_cpu);
	/* Check actual resumed execution independently of the RV observer. */
	KUNIT_EXPECT_EQ(test, resumed_cpu, live->target_cpu);
	restore_result = set_cpus_allowed_ptr(current, &live->saved_mask);
	KUNIT_EXPECT_EQ(test, restore_result, 0);
	affinity_restored = !restore_result &&
		cpumask_equal(current->cpus_ptr, &live->saved_mask);
	KUNIT_EXPECT_TRUE(test, affinity_restored);
	if (affinity_restored)
		live->affinity_changed = false;
	RV_KUNIT_EXPECT_NO_REACTION(test, ctx);
	kunit_info(test, "live IRQ-exit migration cpu=%u->%u actual=%u entries=%u exits=%u switches=%u\n",
		   live->entry_cpu, live->exit_cpu, resumed_cpu, live->entries, live->exits,
		   live->switches_out);
	kunit_info(test, "live IRQ-exit affinity restored=%u\n", affinity_restored);
}
#else
static void rv_test_irqexit_live(struct kunit *test)
{
	/* preempt_schedule_irq() is deliberately not exported to modules. */
	kunit_skip(test, "Requires built-in RV tests");
}

static void rv_test_irqexit_migrate(struct kunit *test)
{
	kunit_skip(test, "Requires built-in RV tests");
}
#endif

#else
#define rv_test_irqexit rv_test_stub
#define rv_test_irqexit_invalid rv_test_stub
#define rv_test_irqexit_sync rv_test_stub
#define rv_test_irqexit_live rv_test_stub
#define rv_test_irqexit_migrate rv_test_stub
#endif
