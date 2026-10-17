// SPDX-License-Identifier: GPL-2.0
#include <linux/ftrace.h>
#include <linux/hardirq.h>
#include <linux/init.h>
#include <linux/irqflags.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/rv.h>
#include <linux/tracepoint.h>
#include <rv/instrumentation.h>

#define MODULE_NAME "irqexit"

#include <trace/events/sched.h>
#include <rv_trace.h>
#include <monitors/irqentry/irqentry.h>

#define RV_MON_TYPE RV_MON_PER_TASK
#include "irqexit.h"
#include <rv/ha_monitor.h>

static u64 ha_get_env(struct ha_monitor *ha_mon, enum envs_irqexit env, u64 time_ns)
{
	switch (env) {
	case hardirq_irqexit:
		return hardirq_count();
	case irq_off_irqexit:
		return irqs_disabled();
	case owner_current_irqexit:
		return ha_get_target(ha_mon) == rv_get_current();
	default:
		return ENV_INVALID_VALUE;
	}
}

static inline bool ha_verify_guards(struct ha_monitor *ha_mon,
				    enum states curr_state, enum events event,
				    enum states next_state, u64 time_ns)
{
	bool res = true;

	if (curr_state == idle_irqexit && event == resched_entry_irqexit)
		res = ha_get_env(ha_mon, hardirq_irqexit, time_ns) == 0ull &&
		      ha_get_env(ha_mon, irq_off_irqexit, time_ns) == 1ull &&
		      ha_get_env(ha_mon, owner_current_irqexit, time_ns) == 1ull;
	else if (curr_state == idle_irqexit && event == switch_in_irqexit)
		res = ha_get_env(ha_mon, hardirq_irqexit, time_ns) == 0ull &&
		      ha_get_env(ha_mon, irq_off_irqexit, time_ns) == 1ull;
	else if (curr_state == idle_irqexit && event == switch_out_irqexit)
		res = ha_get_env(ha_mon, hardirq_irqexit, time_ns) == 0ull &&
		      ha_get_env(ha_mon, irq_off_irqexit, time_ns) == 1ull;
	else if (curr_state == rescheduling_irqexit && event == resched_exit_irqexit)
		res = ha_get_env(ha_mon, hardirq_irqexit, time_ns) == 0ull &&
		      ha_get_env(ha_mon, irq_off_irqexit, time_ns) == 1ull &&
		      ha_get_env(ha_mon, owner_current_irqexit, time_ns) == 1ull;
	else if (curr_state == rescheduling_irqexit && event == switch_out_irqexit)
		res = ha_get_env(ha_mon, hardirq_irqexit, time_ns) == 0ull &&
		      ha_get_env(ha_mon, irq_off_irqexit, time_ns) == 1ull &&
		      ha_get_env(ha_mon, owner_current_irqexit, time_ns) == 1ull;
	else if (curr_state == suspended_irqexit && event == switch_in_irqexit)
		res = ha_get_env(ha_mon, hardirq_irqexit, time_ns) == 0ull &&
		      ha_get_env(ha_mon, irq_off_irqexit, time_ns) == 1ull;
	return res;
}

static bool ha_verify_constraint(struct ha_monitor *ha_mon,
				 enum states curr_state, enum events event,
				 enum states next_state, u64 time_ns)
{
	return ha_verify_guards(ha_mon, curr_state, event, next_state, time_ns);
}

static void handle_resched_entry(void *data, struct task_struct *owner)
{
	da_handle_start_run_event(owner, resched_entry_irqexit);
}

static void handle_resched_exit(void *data, struct task_struct *owner)
{
	/* Do not hide an invalid return behind enable-mid-invocation sync. */
	if (owner != rv_get_current() || hardirq_count() || !irqs_disabled())
		da_handle_start_run_event(owner, resched_exit_irqexit);
	else
		da_handle_start_event(owner, resched_exit_irqexit);
}

static void handle_sched_switch(void *data, bool preempt,
				struct task_struct *prev,
				struct task_struct *next,
				unsigned int prev_state)
{
	/* current is still prev; the incoming task must not use that guard. */
	da_handle_event(prev, switch_out_irqexit);
	da_handle_event(next, switch_in_irqexit);
}

static int enable_irqexit(void)
{
	int retval;

	retval = ha_monitor_init();
	if (retval)
		return retval;

	rv_attach_trace_probe("irqexit", sched_irq_preempt_entry, handle_resched_entry);
	rv_attach_trace_probe("irqexit", sched_irq_preempt_exit, handle_resched_exit);
	rv_attach_trace_probe("irqexit", sched_switch, handle_sched_switch);

	return 0;
}

static void disable_irqexit(void)
{
	rv_this.enabled = 0;

	rv_detach_trace_probe("irqexit", sched_irq_preempt_entry, handle_resched_entry);
	rv_detach_trace_probe("irqexit", sched_irq_preempt_exit, handle_resched_exit);
	rv_detach_trace_probe("irqexit", sched_switch, handle_sched_switch);

	ha_monitor_destroy();
}

static struct rv_monitor rv_this = {
	.name = "irqexit",
	.description = "Verify IRQ-exit scheduling ownership.",
	.enable = enable_irqexit,
	.disable = disable_irqexit,
	.reset = da_monitor_reset_all,
	.enabled = 0,
};

static int __init register_irqexit(void)
{
	return rv_register_monitor(&rv_this, &rv_irqentry);
}

static void __exit unregister_irqexit(void)
{
	rv_unregister_monitor(&rv_this);
}

module_init(register_irqexit);
module_exit(unregister_irqexit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Karl Mehltretter <kmehltretter@gmail.com>");
MODULE_DESCRIPTION("irqexit: verify IRQ-exit scheduling ownership");

#if IS_ENABLED(CONFIG_RV_MONITORS_KUNIT_TEST)
#include <kunit/visibility.h>
#include "irqexit_kunit.h"

const struct rv_irqexit_ops rv_irqexit_ops = {
	.mon = RV_MON_OPS_INIT(),
	.handle_resched_entry = handle_resched_entry,
	.handle_resched_exit = handle_resched_exit,
	.handle_sched_switch = handle_sched_switch,
};
EXPORT_SYMBOL_IF_KUNIT(rv_irqexit_ops);
#endif
