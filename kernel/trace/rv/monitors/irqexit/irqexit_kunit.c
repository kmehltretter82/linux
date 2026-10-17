// SPDX-License-Identifier: GPL-2.0
#include <linux/hardirq.h>
#include <linux/irqflags.h>
#include <linux/kernel.h>
#include <linux/rv.h>
#include <rv/kunit.h>

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

#else
#define rv_test_irqexit rv_test_stub
#define rv_test_irqexit_invalid rv_test_stub
#define rv_test_irqexit_sync rv_test_stub
#endif
