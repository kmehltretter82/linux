// SPDX-License-Identifier: GPL-2.0
#include <linux/completion.h>
#include <linux/hardirq.h>
#include <linux/irqflags.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/rv.h>
#include <linux/sched.h>
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
	struct completion worker_done;
	unsigned int entries;
	unsigned int exits;
	unsigned int switches_out;
	unsigned int switches_in;
	bool observer_attached;
	bool monitor_enabled;
};

static int irqexit_live_worker(void *data)
{
	struct irqexit_live_ctx *live = data;

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
	if (!strcmp(event, "resched_entry"))
		live->entries++;
	else if (!strcmp(event, "resched_exit"))
		live->exits++;
	else if (!strcmp(event, "switch_out") && !strcmp(state, "rescheduling"))
		live->switches_out++;
	else if (!strcmp(event, "switch_in") && !strcmp(state, "suspended"))
		live->switches_in++;
}

static void irqexit_live_cleanup(void *data)
{
	struct irqexit_live_ctx *live = data;
	struct rv_monitor *mon = rv_irqexit_ops.mon.rv_this;

	mon->enabled = 0;
	if (live->monitor_enabled)
		mon->disable();
	if (live->observer_attached)
		unregister_trace_event_irqexit(irqexit_observe_event, live);
	tracepoint_synchronize_unregister();
	if (live->worker)
		kthread_stop(live->worker);

	RV_KUNIT_EXPECT_NO_REACTION(kunit_get_current_test(), active_ctx);
	mon->react = mon->reactor ? mon->reactor->react : NULL;
	active_ctx = NULL;
	rv_mock_current(NULL);
}

static void rv_test_irqexit_live(struct kunit *test)
{
	struct rv_monitor *mon = rv_irqexit_ops.mon.rv_this;
	struct rv_kunit_ctx *ctx = test->priv;
	struct irqexit_live_ctx *live;
	struct task_struct *worker;
	unsigned int entries, exits, switches_out, switches_in;
	unsigned int first_entries, first_switches;
	unsigned long flags;
	bool worker_done;

	KUNIT_ASSERT_FALSE(test, mon->enabled);
	live = kunit_kzalloc(test, sizeof(*live), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, live);
	live->owner = current;
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

	/* The suite holds rv_interface_lock: allocate and reset a real slot. */
	KUNIT_ASSERT_EQ(test, mon->enable(), 0);
	live->monitor_enabled = true;
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
}
#else
static void rv_test_irqexit_live(struct kunit *test)
{
	/* preempt_schedule_irq() is deliberately not exported to modules. */
	kunit_skip(test, "Requires built-in RV tests");
}
#endif

#else
#define rv_test_irqexit rv_test_stub
#define rv_test_irqexit_invalid rv_test_stub
#define rv_test_irqexit_sync rv_test_stub
#define rv_test_irqexit_live rv_test_stub
#endif
