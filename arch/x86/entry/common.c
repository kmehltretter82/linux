/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/entry-common.h>
#include <linux/kvm_types.h>
#include <linux/hrtimer_rearm.h>
#include <asm/fred.h>
#include <asm/desc.h>

#if IS_ENABLED(CONFIG_KVM_INTEL)
/*
 * On VMX, NMIs and IRQs (as configured by KVM) are acknowledged by hardware as
 * part of the VM-Exit, i.e. the event itself is consumed as part the VM-Exit.
 * x86_entry_from_kvm() is invoked by KVM to effectively forward NMIs and IRQs
 * to the kernel for servicing.  On SVM, a.k.a. AMD, the NMI/IRQ VM-Exit is
 * purely a signal that an NMI/IRQ is pending, i.e. the event that triggered
 * the VM-Exit is held pending until it's unblocked in the host.
 */
noinstr void x86_entry_from_kvm(unsigned int event_type, unsigned int vector)
{
	if (event_type == EVENT_TYPE_EXTINT) {
		unsigned long tif_work;

#ifdef CONFIG_X86_64
		/*
		 * Use FRED dispatch, even when running IDT. The dispatch
		 * tables are kept in sync between FRED and IDT, and the FRED
		 * dispatch works well with CFI.
		 */
		fred_entry_from_kvm(event_type, vector);
#else
		idt_entry_from_kvm(vector);
#endif
		/*
		 * KVM/VMX will dispatch from IRQ-disabled but for a context
		 * that will have IRQs-enabled. This confuses the entry code
		 * and it will not have reprogrammed the timer. Preserve the
		 * interrupt's hardirq classification while doing so.
		 */
		if (IS_ENABLED(CONFIG_HRTIMER_REARM_DEFERRED)) {
			tif_work = read_thread_flags();
			if (unlikely(tif_work & _TIF_HRTIMER_REARM)) {
				struct irqentry_hardirq_cookie cookie;

				cookie = __irqentry_hardirq_tail_enter();

				instrumentation_begin();
				hrtimer_rearm_deferred_tif(tif_work);
				instrumentation_end();

				irqentry_hardirq_tail_exit(cookie);
			}
		}

		return;
	}

	WARN_ON_ONCE(event_type != EVENT_TYPE_NMI);

#ifdef CONFIG_X86_64
	if (cpu_feature_enabled(X86_FEATURE_FRED))
		return fred_entry_from_kvm(event_type, vector);
#endif

	/*
	 * Notably, we must use IDT dispatch for NMI when running in IDT mode.
	 * The FRED NMI context is significantly different and will not work
	 * right (specifically FRED fixed the NMI recursion issue).
	 */
	idt_do_nmi_irqoff();
}
EXPORT_SYMBOL_FOR_KVM(x86_entry_from_kvm);
#endif
