// SPDX-License-Identifier: GPL-2.0
#include <linux/ftrace.h>
#include <linux/hardirq.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/rv.h>
#include <linux/tracepoint.h>
#include <rv/instrumentation.h>

#define MODULE_NAME "irqstate"

#include <trace/events/irq.h>
#include <rv_trace.h>
#include <monitors/irqentry/irqentry.h>

#define RV_MON_TYPE RV_MON_PER_CPU
#include "irqstate.h"
#include <rv/ha_monitor.h>

static u64 ha_get_env(struct ha_monitor *ha_mon, enum envs_irqstate env,
		      u64 time_ns)
{
	switch (env) {
	case hardirq_irqstate:
		return hardirq_count();
	case serving_softirq_irqstate:
		return !!in_serving_softirq();
	default:
		return ENV_INVALID_VALUE;
	}
}

static inline bool ha_verify_guards(struct ha_monitor *ha_mon,
				    enum states curr_state, enum events event,
				    enum states next_state, u64 time_ns)
{
	bool res = true;

	if (curr_state == in_hardirq_irqstate && event == hardirq_exit_irqstate)
		res = ha_get_env(ha_mon, hardirq_irqstate, time_ns) > 0 &&
		      ha_get_env(ha_mon, serving_softirq_irqstate, time_ns) == 0;
	else if (curr_state == idle_irqstate && event == hardirq_entry_irqstate)
		res = ha_get_env(ha_mon, hardirq_irqstate, time_ns) > 0 &&
		      ha_get_env(ha_mon, serving_softirq_irqstate, time_ns) == 0;
	else if (curr_state == idle_irqstate && event == softirq_entry_irqstate)
		res = ha_get_env(ha_mon, hardirq_irqstate, time_ns) == 0 &&
		      ha_get_env(ha_mon, serving_softirq_irqstate, time_ns) == 1;
	else if (curr_state == in_softirq_irqstate && event == hardirq_entry_irqstate)
		res = ha_get_env(ha_mon, hardirq_irqstate, time_ns) > 0 &&
		      ha_get_env(ha_mon, serving_softirq_irqstate, time_ns) == 1;
	else if (curr_state == in_softirq_irqstate && event == softirq_exit_irqstate)
		res = ha_get_env(ha_mon, hardirq_irqstate, time_ns) == 0 &&
		      ha_get_env(ha_mon, serving_softirq_irqstate, time_ns) == 1;
	else if (curr_state == hardirq_from_softirq_irqstate &&
		 event == hardirq_exit_irqstate)
		res = ha_get_env(ha_mon, hardirq_irqstate, time_ns) > 0 &&
		      ha_get_env(ha_mon, serving_softirq_irqstate, time_ns) == 1;

	return res;
}

static bool ha_verify_constraint(struct ha_monitor *ha_mon,
				 enum states curr_state, enum events event,
				 enum states next_state, u64 time_ns)
{
	return ha_verify_guards(ha_mon, curr_state, event, next_state, time_ns);
}

static void handle_hardirq_entry(void *data, unsigned int depth)
{
	if (!hardirq_count()) {
		da_handle_start_run_event(hardirq_entry_irqstate);
		return;
	}

	/* Nested hardirqs leave the outer context state unchanged. */
	if (depth > 1)
		return;

	if (in_serving_softirq())
		da_handle_event(hardirq_entry_irqstate);
	else
		da_handle_start_run_event(hardirq_entry_irqstate);
}

static void handle_hardirq_exit(void *data, unsigned int depth)
{
	if (!hardirq_count()) {
		da_handle_start_run_event(hardirq_exit_irqstate);
		return;
	}

	/* Nested hardirqs leave the outer context state unchanged. */
	if (depth > 1)
		return;

	if (in_serving_softirq())
		da_handle_event(hardirq_exit_irqstate);
	else
		da_handle_start_event(hardirq_exit_irqstate);
}

static void handle_softirq_entry(void *data, bool serving)
{
	da_handle_start_run_event(softirq_entry_irqstate);
}

static void handle_softirq_exit(void *data, bool serving)
{
	if (in_serving_softirq())
		da_handle_start_event(softirq_exit_irqstate);
	else
		da_handle_start_run_event(softirq_exit_irqstate);
}

static void attach_irqstate(void)
{
	rv_attach_trace_probe("irqstate", hardirq_context_entry, handle_hardirq_entry);
	rv_attach_trace_probe("irqstate", hardirq_context_exit, handle_hardirq_exit);
	rv_attach_trace_probe("irqstate", softirq_context_entry, handle_softirq_entry);
	rv_attach_trace_probe("irqstate", softirq_context_exit, handle_softirq_exit);
}

static void detach_irqstate(void)
{
	rv_detach_trace_probe("irqstate", hardirq_context_entry, handle_hardirq_entry);
	rv_detach_trace_probe("irqstate", hardirq_context_exit, handle_hardirq_exit);
	rv_detach_trace_probe("irqstate", softirq_context_entry, handle_softirq_entry);
	rv_detach_trace_probe("irqstate", softirq_context_exit, handle_softirq_exit);
}

static int enable_irqstate(void)
{
	int retval;

	retval = ha_monitor_init();
	if (retval)
		return retval;

	attach_irqstate();

	return 0;
}

static void disable_irqstate(void)
{
	rv_this.enabled = 0;

	detach_irqstate();

	ha_monitor_destroy();
}

static struct rv_monitor rv_this = {
	.name = "irqstate",
	.description = "IRQ context boundaries agree with execution state.",
	.enable = enable_irqstate,
	.disable = disable_irqstate,
	.reset = da_monitor_reset_all,
	.enabled = 0,
};

static int __init register_irqstate(void)
{
	return rv_register_monitor(&rv_this, &rv_irqentry);
}

static void __exit unregister_irqstate(void)
{
	rv_unregister_monitor(&rv_this);
}

module_init(register_irqstate);
module_exit(unregister_irqstate);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Karl Mehltretter <kmehltretter@gmail.com>");
MODULE_DESCRIPTION("irqstate: verify IRQ event and execution context agreement");

#if IS_ENABLED(CONFIG_RV_MONITORS_KUNIT_TEST)
#include <kunit/visibility.h>
#include "irqstate_kunit.h"

const struct rv_irqstate_ops rv_irqstate_ops = {
	.mon = RV_MON_OPS_INIT(),
	.attach = attach_irqstate,
	.detach = detach_irqstate,
	.handle_hardirq_entry = handle_hardirq_entry,
	.handle_hardirq_exit = handle_hardirq_exit,
	.handle_softirq_entry = handle_softirq_entry,
	.handle_softirq_exit = handle_softirq_exit,
};
EXPORT_SYMBOL_IF_KUNIT(rv_irqstate_ops);
#endif
