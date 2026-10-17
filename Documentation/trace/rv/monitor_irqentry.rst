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

The ``irqstate`` monitor checks the ordering of the complete generic hardirq
and softirq contexts.  Each boundary must agree with the corresponding
preempt-count state:

.. list-table:: Event and required context
   :header-rows: 1

   * - Kernel event
     - Model event
     - Required environment
   * - ``hardirq_context_entry``
     - ``hardirq_entry``
     - ``hardirq > 0``; serving-softirq state selects the source state
   * - ``hardirq_context_exit``
     - ``hardirq_exit``
     - ``hardirq > 0``; serving-softirq state selects the destination state
   * - ``softirq_context_entry``
     - ``softirq_entry``
     - ``hardirq == 0 && serving_softirq == 1``
   * - ``softirq_context_exit``
     - ``softirq_exit``
     - ``hardirq == 0 && serving_softirq == 1``
   * - ``sched_switch`` from a serving-softirq task (PREEMPT_RT)
     - ``softirq_suspend``
     - ``hardirq == 0``
   * - ``sched_switch`` to a serving-softirq task (PREEMPT_RT)
     - ``softirq_resume``
     - ``hardirq == 0``

``serving_softirq`` uses ``in_serving_softirq()``.  It therefore distinguishes
actual softirq execution from a task which merely disabled bottom halves.  The
hardirq condition uses the hardirq component of ``preempt_count``.

The model has four states: ``idle``, ``in_hardirq``, ``in_softirq``, and
``hardirq_from_softirq``.  It accepts these outer-context sequences::

  idle -> in_hardirq -> idle
  idle -> in_softirq -> idle
  in_softirq -> hardirq_from_softirq -> in_softirq

On PREEMPT_RT, serving-softirq state is task-local and softirq processing is
preemptible.  A switch away from that task suspends its CPU context
(``in_softirq -> idle``); a switch back resumes it (``idle -> in_softirq``).
The adapter reads the outgoing and incoming task's softirq count at
``sched_switch``.  Interrupts are disabled there, so no hardirq boundary can
occur between representing the incoming task and the actual context switch.
A hardirq in a task which preempted a softirq therefore starts in ``idle``.

Hardirq depth greater than one is collapsed into the surrounding hardirq
state.  The depth comes from the existing hardirq component, so the monitor
does not maintain a second counter which could wrap independently.

The boundary events run after entry state has been established and before it
is removed.  A false transition or guard invokes the selected RV reactor and
emits the monitor's error trace event.

Synchronization
---------------

Enabling a per-CPU monitor does not imply that the CPU is in task context.
An outer hardirq or softirq entry from a known idle context starts the model
and consumes the event.  An outer exit can instead establish the idle state
without consuming an unmatched event when monitoring starts in the middle of
that context.  If monitoring starts while a hardirq interrupts a softirq, the
hardirq pair is ignored until the enclosing softirq exit establishes the idle
state.  On PREEMPT_RT, a softirq suspension also establishes ``idle``, and a
resumption can start monitoring directly in ``in_softirq``.

Relationship to sched/sts
-------------------------

The scheduler ``sts`` monitor models the ordering of IRQ disable/enable,
scheduler entry/exit, and task switches.  It uses ``irq_handler_entry`` and
selected x86 vector events to refine that scheduler model.

``irqstate`` does not consume IRQ action or scheduler entry/exit events.  It
observes generic context boundaries, so multiple actions in one interrupt and
an action dispatched through ``generic_handle_irq_safe()`` do not change its
state.  On PREEMPT_RT it uses task switches only to follow the active softirq
context.  IRQ-exit scheduling requires a separate monitor and event contract.

.. list-table:: Monitor coverage
   :header-rows: 1

   * - Property or event
     - ``sched/sts``
     - ``irqentry/irqstate``
   * - IRQ disable and enable ordering
     - yes
     - no
   * - IRQ action entry
     - yes
     - no
   * - scheduler entry and exit
     - yes
     - no
   * - task switch
     - yes
     - PREEMPT_RT softirq context only
   * - complete generic hardirq context
     - no
     - yes
   * - complete serving-softirq context
     - no
     - yes
   * - hardirq interruption of a softirq
     - no
     - yes

Coverage limits
---------------

The context tracepoints are emitted by ``irq_enter_rcu()`` and
``irq_exit_rcu()`` and by the generic softirq processing loop.  Consequently
this monitor cannot observe:

- the early architecture entry interval before ``irq_enter_rcu()``;
- architecture or special-vector paths which use only
  ``__irq_enter_raw()`` and ``__irq_exit_raw()`` (including x86 simple system
  vectors);
- NMI entry and exit; or
- the complete ordering of generic IRQ teardown, inline softirq processing,
  and IRQ-exit rescheduling.

The standard hardirq tracepoints describe individual IRQ actions rather than
physical interrupt boundaries.  They are deliberately not used: synthetic
dispatch and software resend can execute an action outside hardirq context,
and one interrupt can execute several actions.

These limitations are intentional.  A later architecture hook may provide a
deferred raw-entry marker for the very-early interval.  Such an addition needs
a separate event contract and does not change the meaning of ``irqstate``.

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
