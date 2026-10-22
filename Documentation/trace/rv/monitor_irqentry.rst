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

The ``irqstate`` monitor checks the ordering of hardirq contexts whose entry
and exit pass through ``irq_enter()``/``irq_exit()`` or
``irq_enter_rcu()``/``irq_exit_rcu()``, and of complete serving-softirq
contexts. Each boundary must agree with the corresponding preempt-count state:

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
     - ``softirq_entry`` or ``softirq_entry_from_hardirq``
     - ``hardirq == 0 && serving_softirq == 1``
   * - ``softirq_context_exit``
     - ``softirq_exit`` or ``softirq_exit_to_hardirq``
     - ``hardirq == 0 && serving_softirq == 1``
   * - ``sched_switch`` from a serving-softirq task (PREEMPT_RT)
     - ``softirq_suspend``
     - ``hardirq == 0``
   * - ``sched_switch`` to a serving-softirq task (PREEMPT_RT)
     - ``softirq_resume``
     - ``hardirq == 0``

``serving_softirq`` uses ``in_serving_softirq()``.  It therefore distinguishes
a serving-softirq interval from a task which merely disabled bottom halves.
The hardirq condition uses the hardirq component of ``preempt_count``.
Softirq events also report whether hardirq state was replaced for inline
processing during interrupt return. The exit event reports whether hardirq
state will be restored.

On PREEMPT_RT the serving interval includes preparation before
``handle_softirqs()`` and teardown after it, not just handler execution or
CPU-time accounting. A worker can enter and leave this context even if its
pending-work check finds nothing. The RT entry event follows the counter
update that sets the serving bit, and the exit event precedes the update
that clears it. Processing inline from ``local_bh_enable()`` has the same
boundaries; ordinary BH-disable nesting does not emit context events.

RT worker entry acquires any blocking BH lock before the counter update.
When the entry tracepoint is enabled, the update and event execute together
with local IRQs disabled, then restore the original IRQ state. No new IRQ
disable/enable pair is introduced when that tracepoint is disabled. RT
decrements already execute with IRQs disabled. Non-RT retains its existing
IRQ-disabled processing-loop boundaries.

The model has six states: ``idle``, ``in_hardirq``, ``in_softirq``,
``hardirq_from_softirq``, ``softirq_from_hardirq``, and
``hardirq_from_softirq_from_hardirq``. It accepts these outer-context
sequences::

  idle -> in_hardirq -> idle
  idle -> in_softirq -> idle
  in_softirq -> hardirq_from_softirq -> in_softirq
  in_hardirq -> softirq_from_hardirq -> in_hardirq
  softirq_from_hardirq -> hardirq_from_softirq_from_hardirq
                        -> softirq_from_hardirq

The fourth sequence represents non-RT softirq processing during interrupt
return. The kernel replaces ``HARDIRQ_OFFSET`` with ``SOFTIRQ_OFFSET`` while
the handlers run, then restores hardirq state before completing interrupt
exit. The fifth sequence represents a nested hardirq during those handlers.

On PREEMPT_RT, serving-softirq state is task-local and the serving interval is
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

On hardirq exit, ``hardirq_context_exit`` runs near the end of the generic
exit path, after tick-exit work and before removal of ``HARDIRQ_OFFSET``.
Hardirq accounting ends earlier, before inline softirq and tick-exit work.
``irqstate`` returns to the outer model state at this boundary. When the event
is enabled, the outermost-exit handoff passes pending timer work to ktimerd
with forced IRQ threading. It also passes pending softirq work to ksoftirqd
when no enclosing softirq or bottom-half-disabled state suppresses the wake.
These handoffs catch work raised by tick-exit and event callbacks. The event
does not represent return from ``irq_exit_rcu()``, the later reschedule check,
or the architecture return tail.

Hardirq accounting and lockdep hardirq state have separate lifetimes. Lockdep
can also create a synthetic hardirq interval without changing
``HARDIRQ_OFFSET``. ``irqstate`` models the preempt-count context and does not
report those lockdep-only intervals.

Synchronization
---------------

Enabling a per-CPU monitor does not imply that the CPU is in task context.
An outer hardirq or softirq entry from a known idle context starts the model
and consumes the event. An outer exit can instead establish the idle state
without consuming an unmatched event when monitoring starts in the middle of
that context. Provenance-marked softirq boundaries do not start monitoring:
they belong to an enclosing hardirq. If attachment occurs during that
sequence, its final hardirq exit establishes ``idle``. If monitoring starts
while a hardirq interrupts an ordinary softirq, the hardirq pair is ignored
until the enclosing softirq exit establishes the idle state. On PREEMPT_RT, a
softirq suspension also establishes ``idle``, and a resumption can start
monitoring directly in ``in_softirq``.

On PREEMPT_RT, probe attachment waits for an RCU grace period before the
monitor can become enabled. This drains BH preparations which sampled the
entry tracepoint disabled before attachment. Otherwise an intervening IRQ
could synchronize the model to idle before a preparation publishes its
serving bit without an entry event. New preparations see the attached
entry probe and serialize the bit update with their event.

Monitor irqexit
---------------

The ``irqexit`` monitor follows each task's invocation of
``preempt_schedule_irq()``. It consumes ``sched_irq_preempt_entry``,
``sched_irq_preempt_exit``, and ``sched_switch``. Both boundary events must
observe zero hardirq count and disabled local interrupts. The owner saved
in the invocation's stack frame must match the current task at both
boundaries.

The boundary records carry preempt-count and interrupt-state snapshots
taken at the scheduler call site, before tracepoint dispatch or ring-buffer
reservation. Sampling in ``TP_fast_assign`` would instead include tracing's
own context changes. Disabled hooks do not take these extra snapshots.
The RV adapter still evaluates its original live-context guards; the raw
record is an independent diagnostic channel, not a replacement verdict.

The model has three states: ``idle``, ``rescheduling``, and ``suspended``.
Entry changes ``idle`` to ``rescheduling``; return changes it back to
``idle``. Switching out an active invocation changes ``rescheduling`` to
``suspended``; switching that task in resumes ``rescheduling``. The
incoming event cannot require its target to be current: ``sched_switch``
still executes in the outgoing task. Local interrupts remain disabled
until the incoming task runs.

Instances belong to tasks, not CPUs. Two tasks may have independently
suspended invocations, and resuming on another CPU must use the same task's
instance. An interrupt or IRQ-exit schedule in another task cannot return
on behalf of the suspended owner. Ordinary switches while ``idle`` do not
change its state.

An entry starts monitoring and consumes the event. If the monitor was
enabled during an existing invocation, its first valid exit instead
establishes ``idle`` without consuming an unmatched event. Switches alone
do not synchronize a task. An exit with an invalid context or owner must
not be skipped as a synchronization event.

The helper can also run on a kernel exception return. Its events therefore
do not prove that an invocation followed a physical interrupt, and they do
not describe architecture register restoration or context tracking after
the helper returns. The existing helper entry assertions can reject an
invalid caller before the entry event; such a failure is not a violation
detected by this monitor.

``irqexit`` does not associate an invocation with a preceding hardirq. It
cannot observe work between ``hardirq_context_exit`` and the helper entry or
between the helper exit and return to the interrupted context.

Relationship to sched/sts
-------------------------

The scheduler ``sts`` monitor models the ordering of IRQ disable/enable,
scheduler entry/exit, and task switches.  It uses ``irq_handler_entry`` and
selected x86 vector events to refine that scheduler model.

``irqstate`` does not consume IRQ action or scheduler entry/exit events.  It
observes generic context boundaries, so multiple actions in one interrupt and
an action dispatched through ``generic_handle_irq_safe()`` do not change its
state.  On PREEMPT_RT it uses task switches only to follow the active softirq
context. ``irqexit`` observes the surrounding IRQ-preemption loop and its
task owner, rather than inferring that protocol from individual scheduler
entry/exit events used by ``sts``.

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
   * - ``irq_enter[_rcu]()``/``irq_exit[_rcu]()`` hardirq context
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
``irq_exit_rcu()``. Softirq boundaries come from the RT serving-bit updates
or the non-RT generic processing loop. Consequently this monitor cannot
observe:

- the early architecture entry interval before ``irq_enter_rcu()``;
- paths which directly use ``__irq_enter()`` and ``__irq_exit()``, such as
  the arm64 APEI SEA irq-work interval;
- architecture or special-vector paths which use only
  ``__irq_enter_raw()`` and ``__irq_exit_raw()`` (including x86 simple system
  vectors);
- NMI entry and exit; or
- instrumentation reached while removing hardirq-count or non-RT
  serving-softirq state, work after those removals, return from
  ``irq_exit_rcu()``, the conditional reschedule check, and
  architecture-specific state restoration.

``irqexit`` checks ``preempt_schedule_irq()`` separately. It does not join
that helper to the preceding ``irqstate`` instance or model an architecture's
entire exit.

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
  # echo irqexit >> enabled_monitors
  # echo nop > monitors/irqentry/irqstate/reactors
  # echo nop > monitors/irqentry/irqexit/reactors

Use the ``nop`` reactor during initial qualification.  Select the ``panic``
reactor only after unexplained violations have been excluded on the target
configuration.
