// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014, The Linux Foundation. All rights reserved.
 */
#include <linux/mm.h>
#include <linux/module.h>

#include <asm/tlbflush.h>
#include <asm/set_memory.h>

struct page_change_data {
	pgprot_t set_mask;
	pgprot_t clear_mask;
};

static int change_page_range(pte_t *ptep, unsigned long addr, void *data)
{
	struct page_change_data *cdata = data;
	pte_t pte = *ptep;

	pte = clear_pte_bit(pte, cdata->clear_mask);
	pte = set_pte_bit(pte, cdata->set_mask);

	set_pte_ext(ptep, pte, 0);
	return 0;
}

static bool range_in_range(unsigned long start, unsigned long size,
	unsigned long range_start, unsigned long range_end)
{
	return start >= range_start && start < range_end &&
		size <= range_end - start;
}

/*
 * This function assumes that the range is mapped with PAGE_SIZE pages.
 */
static int __change_memory_common(unsigned long start, unsigned long size,
				pgprot_t set_mask, pgprot_t clear_mask,
				bool local_tlb_flush)
{
	struct page_change_data data;
	int ret;

	data.set_mask = set_mask;
	data.clear_mask = clear_mask;

	ret = apply_to_page_range(&init_mm, start, size, change_page_range,
				  &data);

	if (local_tlb_flush)
		local_flush_tlb_kernel_range(start, start + size);
	else
		flush_tlb_kernel_range(start, start + size);
	return ret;
}

static int change_memory_common(unsigned long addr, int numpages,
				pgprot_t set_mask, pgprot_t clear_mask)
{
	unsigned long start = addr & PAGE_MASK;
	unsigned long end = PAGE_ALIGN(addr) + numpages * PAGE_SIZE;
	unsigned long size = end - start;

	WARN_ON_ONCE(start != addr);

	if (!size)
		return 0;

	if (!range_in_range(start, size, MODULES_VADDR, MODULES_END) &&
	    !range_in_range(start, size, VMALLOC_START, VMALLOC_END))
		return -EINVAL;

	return __change_memory_common(start, size, set_mask, clear_mask, false);
}

int set_memory_ro(unsigned long addr, int numpages)
{
	return change_memory_common(addr, numpages,
					__pgprot(L_PTE_RDONLY),
					__pgprot(0));
}

int set_memory_rw(unsigned long addr, int numpages)
{
	return change_memory_common(addr, numpages,
					__pgprot(0),
					__pgprot(L_PTE_RDONLY));
}

int set_memory_nx(unsigned long addr, int numpages)
{
	return change_memory_common(addr, numpages,
					__pgprot(L_PTE_XN),
					__pgprot(0));
}

int set_memory_x(unsigned long addr, int numpages)
{
	return change_memory_common(addr, numpages,
					__pgprot(0),
					__pgprot(L_PTE_XN));
}

int set_memory_valid(unsigned long addr, int numpages, int enable)
{
	if (enable)
		return __change_memory_common(addr, PAGE_SIZE * numpages,
					      __pgprot(L_PTE_VALID),
					      __pgprot(0), false);
	else
		return __change_memory_common(addr, PAGE_SIZE * numpages,
					      __pgprot(0),
					      __pgprot(L_PTE_VALID), false);
}

#ifdef CONFIG_DEBUG_PAGEALLOC
void __kernel_map_pages(struct page *page, int numpages, int enable)
{
	unsigned long addr;

	if (PageHighMem(page))
		return;

	addr = (unsigned long)page_address(page);

	/*
	 * The kernel image is section mapped even with debug_pagealloc, see
	 * debug_pagealloc_split_lowmem() in mmu.c, so pages freed out of it
	 * cannot be unmapped.  A free block is order aligned and so never
	 * straddles a section boundary into one, which makes checking the
	 * first page enough.
	 */
	if (pmd_leaf(*pmd_off_k(addr)))
		return;

	/*
	 * Only flush the local TLB, as x86 does.  This is called from the
	 * page allocator, often with interrupts disabled, where the IPI an
	 * SMP broadcast may need would deadlock.  A stale entry on another
	 * CPU only means an access from there is caught a little later.
	 */
	if (enable)
		__change_memory_common(addr, (unsigned long)numpages << PAGE_SHIFT,
				       __pgprot(L_PTE_VALID), __pgprot(0),
				       true);
	else
		__change_memory_common(addr, (unsigned long)numpages << PAGE_SHIFT,
				       __pgprot(0), __pgprot(L_PTE_VALID),
				       true);
}
#endif /* CONFIG_DEBUG_PAGEALLOC */
