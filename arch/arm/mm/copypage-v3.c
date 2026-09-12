// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARMv3 page copy/clear primitives for the ARM610.
 *
 * Keep the implementation in ARM state: the ARM610 has neither Thumb nor the
 * ARMv4 halfword and branch-exchange instructions.
 */
#include <linux/init.h>
#include <linux/highmem.h>

static void v3_copy_user_page(void *kto, const void *kfrom)
{
	int tmp;

	asm volatile("\
	.syntax unified\n\
	ldmia	%1!, {r3, r4, ip, lr}\n\
1:	stmia	%0!, {r3, r4, ip, lr}\n\
	ldmia	%1!, {r3, r4, ip, lr}\n\
	stmia	%0!, {r3, r4, ip, lr}\n\
	ldmia	%1!, {r3, r4, ip, lr}\n\
	stmia	%0!, {r3, r4, ip, lr}\n\
	ldmia	%1!, {r3, r4, ip, lr}\n\
	subs	%2, %2, #1\n\
	stmia	%0!, {r3, r4, ip, lr}\n\
	ldmiane	%1!, {r3, r4, ip, lr}\n\
	bne	1b"
	: "+&r" (kto), "+&r" (kfrom), "=&r" (tmp)
	: "2" (PAGE_SIZE / 64)
	: "r3", "r4", "ip", "lr");
}

void v3_copy_user_highpage(struct page *to, struct page *from,
	unsigned long vaddr, struct vm_area_struct *vma)
{
	void *kto = kmap_atomic(to);
	void *kfrom = kmap_atomic(from);

	v3_copy_user_page(kto, kfrom);
	kunmap_atomic(kfrom);
	kunmap_atomic(kto);
}

void v3_clear_user_highpage(struct page *page, unsigned long vaddr)
{
	void *ptr;
	void *kaddr = kmap_atomic(page);

	asm volatile("\
	mov	r1, %2\n\
	mov	r2, #0\n\
	mov	r3, #0\n\
	mov	ip, #0\n\
	mov	lr, #0\n\
1:	stmia	%0!, {r2, r3, ip, lr}\n\
	stmia	%0!, {r2, r3, ip, lr}\n\
	stmia	%0!, {r2, r3, ip, lr}\n\
	stmia	%0!, {r2, r3, ip, lr}\n\
	subs	r1, r1, #1\n\
	bne	1b"
	: "=r" (ptr)
	: "0" (kaddr), "I" (PAGE_SIZE / 64)
	: "r1", "r2", "r3", "ip", "lr");
	kunmap_atomic(kaddr);
}

struct cpu_user_fns v3_user_fns __initdata = {
	.cpu_clear_user_highpage = v3_clear_user_highpage,
	.cpu_copy_user_highpage = v3_copy_user_highpage,
};
