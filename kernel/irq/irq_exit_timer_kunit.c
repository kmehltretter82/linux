// SPDX-License-Identifier: GPL-2.0
/*
 * Development-only oracle for the retained hardirq timer-wakeup closure.
 *
 * A remote IPI callback arms its own outer IRQ exit. An fprobe return
 * handler then raises TIMER_SOFTIRQ as preempt_count_sub_prepare() returns
 * to irq_exit_rcu_preempt_counted_prepare(). A second fprobe accepts a
 * wake_up_process() only when it targets that CPU's ktimerd before a later
 * hardirq entry. The hardirq-entry probe closes that stale-observation
 * window.
 *
 * expect_same_exit_wake defaults to true, so the retained-exit kernel that
 * lacks a final timer-thread check is expected to fail. Setting it to false
 * gives an explicit expected-red control run; it does not turn either
 * outcome into an unconditional pass.
 */

#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/fprobe.h>
#include <linux/hardirq.h>
#include <linux/interrupt.h>
#include <linux/kallsyms.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <kunit/test.h>

#define IRQ_EXIT_TIMER_MAX_ATTEMPTS	8
#define IRQ_EXIT_TIMER_DRAIN_POLLS	100

static bool expect_same_exit_wake = true;
module_param(expect_same_exit_wake, bool, 0444);
MODULE_PARM_DESC(expect_same_exit_wake,
		 "expect ktimerd to be woken by the IRQ exit that injects timer work");

enum irq_exit_timer_phase {
	IRQ_EXIT_TIMER_IDLE,
	IRQ_EXIT_TIMER_WAIT_TRIGGER,
	IRQ_EXIT_TIMER_ARMED,
	IRQ_EXIT_TIMER_INJECTING,
	IRQ_EXIT_TIMER_INJECTED,
	IRQ_EXIT_TIMER_WAKING,
	IRQ_EXIT_TIMER_WOKEN,
	IRQ_EXIT_TIMER_STALE_RECORDING,
	IRQ_EXIT_TIMER_STALE_BEFORE_INJECT,
	IRQ_EXIT_TIMER_STALE_AFTER_INJECT,
};

struct irq_exit_timer_observation {
	int trigger_cpu;
	int inject_cpu;
	int wake_cpu;
	int stale_cpu;
	unsigned int trigger_irq_count;
	unsigned int inject_irq_count;
	unsigned int wake_irq_count;
	unsigned int stale_irq_count;
	unsigned long inject_timer_before;
	unsigned long inject_timer_after;
	unsigned int inject_softirq_before;
	unsigned int inject_softirq_after;
	unsigned long wake_timer_pending;
	struct task_struct *wake_task;
	bool trigger_irqs_disabled;
	bool inject_irqs_disabled;
	bool wake_irqs_disabled;
	bool stale_irqs_disabled;
	atomic_t trigger_count;
	atomic_t inject_count;
	atomic_t same_exit_wake_count;
	atomic_t wrong_task_count;
	atomic_t bad_args_count;
	atomic_t stale_count;
};

struct irq_exit_timer_ctx {
	atomic_t phase;
	int target_cpu;
	struct task_struct *target_ktimerd;
	unsigned long wrapper_start;
	unsigned long wrapper_end;
	struct irq_exit_timer_observation obs;
};

struct irq_exit_timer_remote {
	unsigned long timer_pending;
	unsigned int softirq_pending;
	int cpu;
};

static struct irq_exit_timer_ctx *irq_exit_timer_active_ctx;

static bool irq_exit_timer_retained_return(struct irq_exit_timer_ctx *ctx,
					   unsigned long ret_ip)
{
	return ret_ip >= ctx->wrapper_start && ret_ip < ctx->wrapper_end;
}

static notrace void irq_exit_timer_inject(struct fprobe *fp,
					  unsigned long entry_ip,
					  unsigned long ret_ip,
					  struct ftrace_regs *fregs,
					  void *entry_data)
{
	struct irq_exit_timer_ctx *ctx = READ_ONCE(irq_exit_timer_active_ctx);
	struct irq_exit_timer_observation *obs;

	if (!ctx || raw_smp_processor_id() != READ_ONCE(ctx->target_cpu) ||
	    irq_count() != HARDIRQ_OFFSET ||
	    !irq_exit_timer_retained_return(ctx, ret_ip) ||
	    atomic_cmpxchg(&ctx->phase, IRQ_EXIT_TIMER_ARMED,
			   IRQ_EXIT_TIMER_INJECTING) != IRQ_EXIT_TIMER_ARMED)
		return;

	obs = &ctx->obs;
	WRITE_ONCE(obs->inject_cpu, raw_smp_processor_id());
	WRITE_ONCE(obs->inject_irq_count, irq_count());
	WRITE_ONCE(obs->inject_irqs_disabled, irqs_disabled());
	WRITE_ONCE(obs->inject_timer_before, local_timers_pending_force_th());
	WRITE_ONCE(obs->inject_softirq_before, local_softirq_pending());

	raise_timer_softirq(TIMER_SOFTIRQ);

	WRITE_ONCE(obs->inject_timer_after, local_timers_pending_force_th());
	WRITE_ONCE(obs->inject_softirq_after, local_softirq_pending());
	atomic_inc(&obs->inject_count);
	atomic_set_release(&ctx->phase, IRQ_EXIT_TIMER_INJECTED);
}

static notrace int irq_exit_timer_wake(struct fprobe *fp,
				       unsigned long entry_ip,
				       unsigned long ret_ip,
				       struct ftrace_regs *fregs,
				       void *entry_data)
{
	struct irq_exit_timer_ctx *ctx = READ_ONCE(irq_exit_timer_active_ctx);
	struct irq_exit_timer_observation *obs;
	struct task_struct *task;
	int phase;

	if (!ctx || raw_smp_processor_id() != READ_ONCE(ctx->target_cpu))
		return 0;

	phase = atomic_read_acquire(&ctx->phase);
	if (phase != IRQ_EXIT_TIMER_INJECTED)
		return 0;

	if (!ftrace_regs_has_args(fregs)) {
		atomic_inc(&ctx->obs.bad_args_count);
		return 0;
	}

	task = (struct task_struct *)ftrace_regs_get_argument(fregs, 0);
	if (irq_count() != HARDIRQ_OFFSET || !irqs_disabled())
		return 0;

	if (task != READ_ONCE(ctx->target_ktimerd)) {
		atomic_inc(&ctx->obs.wrong_task_count);
		return 0;
	}

	if (atomic_cmpxchg(&ctx->phase, IRQ_EXIT_TIMER_INJECTED,
			   IRQ_EXIT_TIMER_WAKING) != IRQ_EXIT_TIMER_INJECTED)
		return 0;

	obs = &ctx->obs;
	WRITE_ONCE(obs->wake_cpu, raw_smp_processor_id());
	WRITE_ONCE(obs->wake_irq_count, irq_count());
	WRITE_ONCE(obs->wake_irqs_disabled, irqs_disabled());
	WRITE_ONCE(obs->wake_timer_pending, local_timers_pending_force_th());
	WRITE_ONCE(obs->wake_task, task);
	atomic_inc(&obs->same_exit_wake_count);
	atomic_set_release(&ctx->phase, IRQ_EXIT_TIMER_WOKEN);

	return 0;
}

static notrace int irq_exit_timer_enter(struct fprobe *fp,
					unsigned long entry_ip,
					unsigned long ret_ip,
					struct ftrace_regs *fregs,
					void *entry_data)
{
	struct irq_exit_timer_ctx *ctx = READ_ONCE(irq_exit_timer_active_ctx);
	struct irq_exit_timer_observation *obs;
	int stale_phase;
	int old;

	if (!ctx || raw_smp_processor_id() != READ_ONCE(ctx->target_cpu))
		return 0;

	old = atomic_cmpxchg(&ctx->phase, IRQ_EXIT_TIMER_ARMED,
			     IRQ_EXIT_TIMER_STALE_RECORDING);
	if (old != IRQ_EXIT_TIMER_ARMED) {
		old = atomic_cmpxchg(&ctx->phase, IRQ_EXIT_TIMER_INJECTED,
				     IRQ_EXIT_TIMER_STALE_RECORDING);
		if (old != IRQ_EXIT_TIMER_INJECTED)
			return 0;
	}
	stale_phase = old == IRQ_EXIT_TIMER_ARMED ?
		IRQ_EXIT_TIMER_STALE_BEFORE_INJECT :
		IRQ_EXIT_TIMER_STALE_AFTER_INJECT;

	obs = &ctx->obs;
	WRITE_ONCE(obs->stale_cpu, raw_smp_processor_id());
	WRITE_ONCE(obs->stale_irq_count, irq_count());
	WRITE_ONCE(obs->stale_irqs_disabled, irqs_disabled());
	atomic_inc(&obs->stale_count);
	atomic_set_release(&ctx->phase, stale_phase);

	return 0;
}

static void irq_exit_timer_trigger(void *data)
{
	struct irq_exit_timer_ctx *ctx = data;
	struct irq_exit_timer_observation *obs = &ctx->obs;

	WRITE_ONCE(obs->trigger_cpu, raw_smp_processor_id());
	WRITE_ONCE(obs->trigger_irq_count, irq_count());
	WRITE_ONCE(obs->trigger_irqs_disabled, irqs_disabled());
	atomic_inc(&obs->trigger_count);
	atomic_cmpxchg(&ctx->phase, IRQ_EXIT_TIMER_WAIT_TRIGGER,
		       IRQ_EXIT_TIMER_ARMED);
}

static void irq_exit_timer_snapshot(void *data)
{
	struct irq_exit_timer_remote *remote = data;

	remote->cpu = raw_smp_processor_id();
	remote->timer_pending = local_timers_pending_force_th();
	remote->softirq_pending = local_softirq_pending();
}

static void irq_exit_timer_close(void *data)
{
}

static void irq_exit_timer_reset_attempt(struct irq_exit_timer_ctx *ctx)
{
	memset(&ctx->obs, 0, sizeof(ctx->obs));
	ctx->obs.trigger_cpu = -1;
	ctx->obs.inject_cpu = -1;
	ctx->obs.wake_cpu = -1;
	ctx->obs.stale_cpu = -1;
	atomic_set(&ctx->phase, IRQ_EXIT_TIMER_IDLE);
}

static int irq_exit_timer_read_remote(struct irq_exit_timer_ctx *ctx,
				      struct irq_exit_timer_remote *remote)
{
	memset(remote, 0, sizeof(*remote));
	remote->cpu = -1;
	return smp_call_function_single(ctx->target_cpu, irq_exit_timer_snapshot,
					remote, 1);
}

static bool irq_exit_timer_drain(struct irq_exit_timer_ctx *ctx)
{
	struct irq_exit_timer_remote remote;
	int i;

	for (i = 0; i < IRQ_EXIT_TIMER_DRAIN_POLLS; i++) {
		if (irq_exit_timer_read_remote(ctx, &remote))
			return false;
		if (!remote.timer_pending && !remote.softirq_pending)
			return true;
		usleep_range(1000, 2000);
	}

	return false;
}

static bool irq_exit_timer_attempt(struct irq_exit_timer_ctx *ctx)
{
	struct irq_exit_timer_remote remote;
	int phase, ret;

	irq_exit_timer_reset_attempt(ctx);
	if (!irq_exit_timer_drain(ctx))
		return false;
	if (irq_exit_timer_read_remote(ctx, &remote) || remote.timer_pending ||
	    remote.softirq_pending || remote.cpu != ctx->target_cpu)
		return false;

	atomic_set(&ctx->phase, IRQ_EXIT_TIMER_WAIT_TRIGGER);
	ret = smp_call_function_single(ctx->target_cpu, irq_exit_timer_trigger,
				       ctx, 1);
	if (ret)
		return false;

	/*
	 * The IPI callback queue is detached before callbacks run. This second
	 * callback therefore needs a new hardirq entry, which cannot happen
	 * until the triggering IRQ and all of its exit probes have finished.
	 */
	ret = smp_call_function_single(ctx->target_cpu, irq_exit_timer_close,
				       NULL, 1);
	if (ret)
		return false;

	phase = atomic_read_acquire(&ctx->phase);
	if (phase != IRQ_EXIT_TIMER_WOKEN &&
	    phase != IRQ_EXIT_TIMER_STALE_AFTER_INJECT)
		return false;

	/* A dirty initial bitmap cannot distinguish this injection. Retry it. */
	if (ctx->obs.inject_timer_before || ctx->obs.inject_softirq_before ||
	    ctx->obs.inject_timer_after != BIT(TIMER_SOFTIRQ) ||
	    ctx->obs.inject_softirq_after ||
	    atomic_read(&ctx->obs.bad_args_count) ||
	    atomic_read(&ctx->obs.wrong_task_count))
		return false;

	return true;
}

static void irq_exit_timer_check_observation(struct kunit *test,
					     struct irq_exit_timer_ctx *ctx)
{
	struct irq_exit_timer_observation *obs = &ctx->obs;
	bool same_exit_wake;

	same_exit_wake = atomic_read_acquire(&ctx->phase) == IRQ_EXIT_TIMER_WOKEN;

	KUNIT_EXPECT_EQ(test, atomic_read(&obs->trigger_count), 1);
	KUNIT_EXPECT_EQ(test, obs->trigger_cpu, ctx->target_cpu);
	KUNIT_EXPECT_EQ(test, obs->trigger_irq_count, (unsigned int)HARDIRQ_OFFSET);
	KUNIT_EXPECT_TRUE(test, obs->trigger_irqs_disabled);

	KUNIT_EXPECT_EQ(test, atomic_read(&obs->inject_count), 1);
	KUNIT_EXPECT_EQ(test, obs->inject_cpu, ctx->target_cpu);
	KUNIT_EXPECT_EQ(test, obs->inject_irq_count, (unsigned int)HARDIRQ_OFFSET);
	KUNIT_EXPECT_TRUE(test, obs->inject_irqs_disabled);
	KUNIT_EXPECT_EQ(test, obs->inject_timer_before, 0UL);
	KUNIT_EXPECT_EQ(test, obs->inject_timer_after, BIT(TIMER_SOFTIRQ));
	KUNIT_EXPECT_EQ(test, obs->inject_softirq_after,
			obs->inject_softirq_before);

	KUNIT_EXPECT_EQ(test, atomic_read(&obs->bad_args_count), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&obs->wrong_task_count), 0);
	KUNIT_EXPECT_EQ(test, same_exit_wake, expect_same_exit_wake);

	if (same_exit_wake) {
		KUNIT_EXPECT_EQ(test, atomic_read(&obs->same_exit_wake_count), 1);
		KUNIT_EXPECT_EQ(test, obs->wake_cpu, ctx->target_cpu);
		KUNIT_EXPECT_EQ(test, obs->wake_irq_count,
				(unsigned int)HARDIRQ_OFFSET);
		KUNIT_EXPECT_TRUE(test, obs->wake_irqs_disabled);
		KUNIT_EXPECT_PTR_EQ(test, obs->wake_task, ctx->target_ktimerd);
		KUNIT_EXPECT_TRUE(test, !!(obs->wake_timer_pending &
					  BIT(TIMER_SOFTIRQ)));
		KUNIT_EXPECT_EQ(test, atomic_read(&obs->stale_count), 0);
	} else {
		KUNIT_EXPECT_EQ(test, atomic_read(&obs->same_exit_wake_count), 0);
		KUNIT_EXPECT_EQ(test, atomic_read(&obs->stale_count), 1);
		KUNIT_EXPECT_EQ(test, obs->stale_cpu, ctx->target_cpu);
		KUNIT_EXPECT_EQ(test, obs->stale_irq_count,
				(unsigned int)HARDIRQ_OFFSET);
		KUNIT_EXPECT_TRUE(test, obs->stale_irqs_disabled);
	}

	kunit_info(test, "target=%d phase=%d same_exit_wake=%d expected=%d\n",
		   ctx->target_cpu, atomic_read_acquire(&ctx->phase), same_exit_wake,
		   expect_same_exit_wake);
}

static void irq_exit_timer_closure_test(struct kunit *test)
{
	struct fprobe inject_probe = {
		.exit_handler = irq_exit_timer_inject,
	};
	struct fprobe wake_probe = {
		.entry_handler = irq_exit_timer_wake,
	};
	struct fprobe enter_probe = {
		.entry_handler = irq_exit_timer_enter,
	};
	struct irq_exit_timer_ctx ctx = {
		.target_cpu = -1,
	};
	unsigned long wrapper_size, wrapper_offset;
	bool accepted = false;
	int attempt, cpu, ret;
	bool inject_registered = false;
	bool wake_registered = false;
	bool enter_registered = false;

	if (!force_irqthreads()) {
		kunit_skip(test, "requires forced IRQ threading (boot with threadirqs)");
		return;
	}

	ctx.wrapper_start = (unsigned long)irq_exit_rcu_preempt_counted_prepare;
	if (!kallsyms_lookup_size_offset(ctx.wrapper_start, &wrapper_size,
					 &wrapper_offset) || wrapper_offset) {
		KUNIT_FAIL(test, "cannot resolve retained IRQ-exit wrapper bounds");
		return;
	}
	ctx.wrapper_end = ctx.wrapper_start + wrapper_size;
	irq_exit_timer_reset_attempt(&ctx);
	WRITE_ONCE(irq_exit_timer_active_ctx, &ctx);

	ret = register_fprobe(&inject_probe, "preempt_count_sub_prepare", NULL);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto out_clear;
	inject_registered = true;

	ret = register_fprobe(&wake_probe, "wake_up_process", NULL);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto out_unregister;
	wake_registered = true;

	ret = register_fprobe(&enter_probe, "irq_enter_rcu_preempt_counted", NULL);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto out_unregister;
	enter_registered = true;

	/* Probe registration and teardown deliberately stay outside this lock. */
	cpus_read_lock();
	migrate_disable();
	cpu = raw_smp_processor_id();
	ctx.target_cpu = cpumask_any_but(cpu_online_mask, cpu);
	if (ctx.target_cpu >= nr_cpu_ids) {
		kunit_mark_skipped(test, "requires two online CPUs");
		goto out_enable_migration;
	}

	ctx.target_ktimerd = per_cpu(ktimerd, ctx.target_cpu);
	if (!ctx.target_ktimerd) {
		kunit_mark_skipped(test, "target CPU has no ktimerd task");
		goto out_enable_migration;
	}

	for (attempt = 0; attempt < IRQ_EXIT_TIMER_MAX_ATTEMPTS; attempt++) {
		if (irq_exit_timer_attempt(&ctx)) {
			accepted = true;
			break;
		}
	}

	KUNIT_EXPECT_TRUE(test, accepted);
	if (accepted)
		irq_exit_timer_check_observation(test, &ctx);

	atomic_set(&ctx.phase, IRQ_EXIT_TIMER_IDLE);
	KUNIT_EXPECT_TRUE(test, irq_exit_timer_drain(&ctx));

out_enable_migration:
	migrate_enable();
	cpus_read_unlock();

out_unregister:
	if (enter_registered)
		KUNIT_EXPECT_EQ(test, unregister_fprobe(&enter_probe), 0);
	if (wake_registered)
		KUNIT_EXPECT_EQ(test, unregister_fprobe(&wake_probe), 0);
	if (inject_registered)
		KUNIT_EXPECT_EQ(test, unregister_fprobe(&inject_probe), 0);
out_clear:
	WRITE_ONCE(irq_exit_timer_active_ctx, NULL);
}

static struct kunit_case irq_exit_timer_test_cases[] = {
	KUNIT_CASE(irq_exit_timer_closure_test),
	{}
};

static struct kunit_suite irq_exit_timer_test_suite = {
	.name = "irq_exit_timer",
	.test_cases = irq_exit_timer_test_cases,
};

kunit_test_suites(&irq_exit_timer_test_suite);
