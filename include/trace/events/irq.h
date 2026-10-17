/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM irq

#if !defined(_TRACE_IRQ_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_IRQ_H

#include <linux/tracepoint.h>

struct irqaction;
struct softirq_action;

#define SOFTIRQ_NAME_LIST				\
			 softirq_name(HI)		\
			 softirq_name(TIMER)		\
			 softirq_name(NET_TX)		\
			 softirq_name(NET_RX)		\
			 softirq_name(BLOCK)		\
			 softirq_name(IRQ_POLL)		\
			 softirq_name(TASKLET)		\
			 softirq_name(SCHED)		\
			 softirq_name(HRTIMER)		\
			 softirq_name_end(RCU)

#undef softirq_name
#undef softirq_name_end

#define softirq_name(sirq) TRACE_DEFINE_ENUM(sirq##_SOFTIRQ);
#define softirq_name_end(sirq)  TRACE_DEFINE_ENUM(sirq##_SOFTIRQ);

SOFTIRQ_NAME_LIST

#undef softirq_name
#undef softirq_name_end

#define softirq_name(sirq) { sirq##_SOFTIRQ, #sirq },
#define softirq_name_end(sirq) { sirq##_SOFTIRQ, #sirq }

#define show_softirq_name(val)				\
	__print_symbolic(val, SOFTIRQ_NAME_LIST)

DECLARE_EVENT_CLASS(hardirq_context,
		    TP_PROTO(unsigned int depth),
		    TP_ARGS(depth),
		    TP_STRUCT__entry(__field(unsigned int, depth)),
		    TP_fast_assign(__entry->depth = depth;),
		    TP_printk("depth=%u", __entry->depth));

/**
 * hardirq_context_entry - generic hardirq state has been established
 * @depth: hardirq nesting depth, including the context being entered
 *
 * This event is emitted after generic hardirq preempt-count, lockdep, timer,
 * and accounting state has been established. It does not describe the
 * architecture-specific interval before irq_enter() or irq_enter_rcu().
 */
DEFINE_EVENT(hardirq_context, hardirq_context_entry,
	     TP_PROTO(unsigned int depth),
	     TP_ARGS(depth));

/**
 * hardirq_context_exit - generic hardirq state is about to be removed
 * @depth: hardirq nesting depth, including the context being exited
 *
 * This event is emitted with interrupts disabled before generic hardirq
 * accounting and preempt-count state is removed.
 */
DEFINE_EVENT(hardirq_context, hardirq_context_exit,
	     TP_PROTO(unsigned int depth),
	     TP_ARGS(depth));

DECLARE_EVENT_CLASS(softirq_context,
		    TP_PROTO(bool serving),
		    TP_ARGS(serving),
		    TP_STRUCT__entry(__field(bool, serving)),
		    TP_fast_assign(__entry->serving = serving;),
		    TP_printk("serving=%d", __entry->serving));

/**
 * softirq_context_entry - serving-softirq state has been established
 * @serving: whether the current task is serving a softirq
 *
 * This event describes the complete softirq processing context rather than
 * an individual softirq action.
 */
DEFINE_EVENT(softirq_context, softirq_context_entry,
	     TP_PROTO(bool serving),
	     TP_ARGS(serving));

/**
 * softirq_context_exit - serving-softirq state is about to be removed
 * @serving: whether the current task is serving a softirq
 */
DEFINE_EVENT(softirq_context, softirq_context_exit,
	     TP_PROTO(bool serving),
	     TP_ARGS(serving));

/**
 * irq_handler_entry - called immediately before the irq action handler
 * @irq: irq number
 * @action: pointer to struct irqaction
 *
 * The struct irqaction pointed to by @action contains various
 * information about the handler, including the device name,
 * @action->name, and the device id, @action->dev_id. When used in
 * conjunction with the irq_handler_exit tracepoint, we can figure
 * out irq handler latencies.
 */
TRACE_EVENT(irq_handler_entry,

	TP_PROTO(int irq, struct irqaction *action),

	TP_ARGS(irq, action),

	TP_STRUCT__entry(
		__field(	int,	irq		)
		__string(	name,	action->name	)
	),

	TP_fast_assign(
		__entry->irq = irq;
		__assign_str(name);
	),

	TP_printk("irq=%d name=%s", __entry->irq, __get_str(name))
);

/**
 * irq_handler_exit - called immediately after the irq action handler returns
 * @irq: irq number
 * @action: pointer to struct irqaction
 * @ret: return value
 *
 * If the @ret value is set to IRQ_HANDLED, then we know that the corresponding
 * @action->handler successfully handled this irq. Otherwise, the irq might be
 * a shared irq line, or the irq was not handled successfully. Can be used in
 * conjunction with the irq_handler_entry to understand irq handler latencies.
 */
TRACE_EVENT(irq_handler_exit,

	TP_PROTO(int irq, struct irqaction *action, int ret),

	TP_ARGS(irq, action, ret),

	TP_STRUCT__entry(
		__field(	int,	irq	)
		__field(	int,	ret	)
	),

	TP_fast_assign(
		__entry->irq	= irq;
		__entry->ret	= ret;
	),

	TP_printk("irq=%d ret=%s",
		  __entry->irq, __entry->ret ? "handled" : "unhandled")
);

DECLARE_EVENT_CLASS(softirq,

	TP_PROTO(unsigned int vec_nr),

	TP_ARGS(vec_nr),

	TP_STRUCT__entry(
		__field(	unsigned int,	vec	)
	),

	TP_fast_assign(
		__entry->vec = vec_nr;
	),

	TP_printk("vec=%u [action=%s]", __entry->vec,
		  show_softirq_name(__entry->vec))
);

/**
 * softirq_entry - called immediately before the softirq handler
 * @vec_nr:  softirq vector number
 *
 * When used in combination with the softirq_exit tracepoint
 * we can determine the softirq handler routine.
 */
DEFINE_EVENT(softirq, softirq_entry,

	TP_PROTO(unsigned int vec_nr),

	TP_ARGS(vec_nr)
);

/**
 * softirq_exit - called immediately after the softirq handler returns
 * @vec_nr:  softirq vector number
 *
 * When used in combination with the softirq_entry tracepoint
 * we can determine the softirq handler routine.
 */
DEFINE_EVENT(softirq, softirq_exit,

	TP_PROTO(unsigned int vec_nr),

	TP_ARGS(vec_nr)
);

/**
 * softirq_raise - called immediately when a softirq is raised
 * @vec_nr:  softirq vector number
 *
 * When used in combination with the softirq_entry tracepoint
 * we can determine the softirq raise to run latency.
 */
DEFINE_EVENT(softirq, softirq_raise,

	TP_PROTO(unsigned int vec_nr),

	TP_ARGS(vec_nr)
);

DECLARE_EVENT_CLASS(tasklet,

	TP_PROTO(struct tasklet_struct *t, void *func),

	TP_ARGS(t, func),

	TP_STRUCT__entry(
		__field(	void *,	tasklet)
		__field(	void *,	func)
	),

	TP_fast_assign(
		__entry->tasklet = t;
		__entry->func = func;
	),

	TP_printk("tasklet=%ps function=%ps", __entry->tasklet, __entry->func)
);

/**
 * tasklet_entry - called immediately before the tasklet is run
 * @t: tasklet pointer
 * @func: tasklet callback or function being run
 *
 * Used to find individual tasklet execution time
 */
DEFINE_EVENT(tasklet, tasklet_entry,

	TP_PROTO(struct tasklet_struct *t, void *func),

	TP_ARGS(t, func)
);

/**
 * tasklet_exit - called immediately after the tasklet is run
 * @t: tasklet pointer
 * @func: tasklet callback or function being run
 *
 * Used to find individual tasklet execution time
 */
DEFINE_EVENT(tasklet, tasklet_exit,

	TP_PROTO(struct tasklet_struct *t, void *func),

	TP_ARGS(t, func)
);

#endif /*  _TRACE_IRQ_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
