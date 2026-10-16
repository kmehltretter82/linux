.. SPDX-License-Identifier: GPL-2.0

IRQ-entry monitors
==================

- Name: irqentry
- Type: container for IRQ-entry context monitors
- Author: Karl Mehltretter <kmehltretter@gmail.com>

Description
-----------

The ``irqentry`` container groups monitors which compare interrupt-related
events with the execution context recorded by the kernel.  The monitors are
architecture independent: they consume generic tracepoints and do not require
an architecture assembly hook.

The first monitor deliberately observes only safe, instrumentable events.  It
does not cover the interval between physical interrupt entry and establishment
of generic hardirq state.

Monitor irqstate
----------------

The ``irqstate`` monitor checks that independently observable kernel events
agree with the corresponding preempt-count context:

.. list-table:: Event and required context
   :header-rows: 1

   * - Kernel event
     - Model event
     - Required environment
   * - ``irq_handler_entry``
     - ``hardirq_entry``
     - ``hardirq > 0``
   * - ``irq_handler_exit``
     - ``hardirq_exit``
     - ``hardirq > 0``
   * - ``softirq_entry``
     - ``softirq_entry``
     - ``serving_softirq == 1``
   * - ``softirq_exit``
     - ``softirq_exit``
     - ``serving_softirq == 1``
   * - scheduler entry
     - ``schedule_entry``
     - ``hardirq == 0``

``serving_softirq`` uses ``in_serving_softirq()``.  It therefore distinguishes
actual softirq execution from a task which merely disabled bottom halves.  The
hardirq condition uses the hardirq component of ``preempt_count`` and is valid
for PREEMPT_RT as well as non-RT kernels.

The model is a one-state hybrid automaton.  Every event is a self-transition
guarded by the required execution context.  A false guard invokes the selected
RV reactor and emits the monitor's environment-error trace event.

Relationship to sched/sts
-------------------------

The scheduler ``sts`` monitor models the ordering of IRQ disable/enable,
scheduler entry/exit, and task switches.  It uses ``irq_handler_entry`` and
selected x86 vector events to refine that scheduler model.

``irqstate`` does not reproduce that ordering model.  It directly samples the
hardirq or serving-softirq component at an IRQ, softirq, or scheduler event.
It also observes IRQ-handler exits and both softirq action boundaries, which
``sts`` does not consume.

Coverage limits
---------------

The generic IRQ tracepoints describe individual IRQ actions rather than the
physical interrupt entry and return.  Consequently this first monitor cannot
observe:

- the early architecture entry interval before ``irq_enter_rcu()``;
- local vectors, IPIs, or timer interrupts which do not execute a normal IRQ
  action, except that an attempted schedule with hardirq state active is still
  checked; or
- the complete ordering of generic IRQ teardown, inline softirq processing,
  and IRQ-exit rescheduling.

Those limitations are intentional.  Future monitors can add generic safe
entry/exit boundary events, and an architecture may later provide a deferred
raw-entry marker.  Such additions require separate event contracts and do not
change the meaning of ``irqstate``.

Enabling the monitor
--------------------

With tracefs mounted at ``/sys/kernel/tracing``::

  # cd /sys/kernel/tracing/rv
  # grep -x irqentry:irqstate available_monitors
  # echo irqstate > enabled_monitors
  # echo printk > monitors/irqentry/irqstate/reactors

The trace-only reactor should be used during initial qualification.  The panic
reactor is appropriate only after unexplained violations have been excluded on
the target configuration.
