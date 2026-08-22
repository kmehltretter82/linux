// SPDX-License-Identifier: GPL-2.0-only
/*
 * Debug helper to dump the current kernel pagetables of the system
 * so that we can see what the various memory ranges are set to.
 *
 * Derived from x86 implementation:
 * (C) Copyright 2008 Intel Corporation
 *
 * Author: Arjan van de Ven <arjan@linux.intel.com>
 */
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/ptdump.h>
#include <linux/seq_file.h>

#include <asm/domain.h>
#include <asm/fixmap.h>
#include <asm/page.h>
#include <asm/ptdump.h>

static struct addr_marker address_markers[] = {
#ifdef CONFIG_KASAN
	{ KASAN_SHADOW_START,	"Kasan shadow start"},
	{ KASAN_SHADOW_END,	"Kasan shadow end"},
#endif
	{ MODULES_VADDR,	"Modules" },
	{ PAGE_OFFSET,		"Kernel Mapping" },
	{ 0,			"vmalloc() Area" },
	{ FDT_FIXED_BASE,	"FDT Area" },
	{ FIXADDR_START,	"Fixmap Area" },
	{ VECTORS_BASE,	"Vectors" },
	{ VECTORS_BASE + PAGE_SIZE * 2, "Vectors End" },
	{ -1,			NULL },
};

#define pt_dump_seq_printf(m, fmt, args...) \
({                      \
	if (m)					\
		seq_printf(m, fmt, ##args);	\
})

#define pt_dump_seq_puts(m, fmt)    \
({						\
	if (m)					\
		seq_printf(m, fmt);	\
})

struct pg_state {
	struct ptdump_state ptdump;
	struct mm_struct *mm;
	struct seq_file *seq;
	const struct addr_marker *marker;
	unsigned long start_address;
	unsigned level;
	u64 current_prot;
	bool check_wx;
	unsigned long wx_pages;
	const char *current_domain;
	/* Domain of the pmd whose page table is being walked. */
	const char *pmd_domain;
};

struct prot_bits {
	u64		mask;
	u64		val;
	const char	*set;
	const char	*clear;
	bool		ro_bit;
	bool		nx_bit;
};

static const struct prot_bits pte_bits[] = {
	{
		.mask	= L_PTE_USER,
		.val	= L_PTE_USER,
		.set	= "USR",
		.clear	= "   ",
	}, {
		.mask	= L_PTE_RDONLY,
		.val	= L_PTE_RDONLY,
		.set	= "ro",
		.clear	= "RW",
		.ro_bit	= true,
	}, {
		.mask	= L_PTE_XN,
		.val	= L_PTE_XN,
		.set	= "NX",
		.clear	= "x ",
		.nx_bit	= true,
	}, {
		.mask	= L_PTE_SHARED,
		.val	= L_PTE_SHARED,
		.set	= "SHD",
		.clear	= "   ",
	}, {
		.mask	= L_PTE_MT_MASK,
		.val	= L_PTE_MT_UNCACHED,
		.set	= "SO/UNCACHED",
	}, {
		.mask	= L_PTE_MT_MASK,
		.val	= L_PTE_MT_BUFFERABLE,
		.set	= "MEM/BUFFERABLE/WC",
	}, {
		.mask	= L_PTE_MT_MASK,
		.val	= L_PTE_MT_WRITETHROUGH,
		.set	= "MEM/CACHED/WT",
	}, {
		.mask	= L_PTE_MT_MASK,
		.val	= L_PTE_MT_WRITEBACK,
		.set	= "MEM/CACHED/WBRA",
#ifndef CONFIG_ARM_LPAE
	}, {
		.mask	= L_PTE_MT_MASK,
		.val	= L_PTE_MT_MINICACHE,
		.set	= "MEM/MINICACHE",
#endif
	}, {
		.mask	= L_PTE_MT_MASK,
		.val	= L_PTE_MT_WRITEALLOC,
		.set	= "MEM/CACHED/WBWA",
	}, {
		.mask	= L_PTE_MT_MASK,
		.val	= L_PTE_MT_DEV_SHARED,
		.set	= "DEV/SHARED",
#ifndef CONFIG_ARM_LPAE
	}, {
		.mask	= L_PTE_MT_MASK,
		.val	= L_PTE_MT_DEV_NONSHARED,
		.set	= "DEV/NONSHARED",
#endif
	}, {
		.mask	= L_PTE_MT_MASK,
		.val	= L_PTE_MT_DEV_WC,
		.set	= "DEV/WC",
	}, {
		.mask	= L_PTE_MT_MASK,
		.val	= L_PTE_MT_DEV_CACHED,
		.set	= "DEV/CACHED",
	},
};

static const struct prot_bits section_bits[] = {
#ifdef CONFIG_ARM_LPAE
	{
		.mask	= PMD_SECT_USER,
		.val	= PMD_SECT_USER,
		.set	= "USR",
	}, {
		.mask	= L_PMD_SECT_RDONLY | PMD_SECT_AP2,
		.val	= L_PMD_SECT_RDONLY | PMD_SECT_AP2,
		.set	= "ro",
		.clear	= "RW",
		.ro_bit	= true,
#elif __LINUX_ARM_ARCH__ >= 6
	{
		.mask	= PMD_SECT_APX | PMD_SECT_AP_READ | PMD_SECT_AP_WRITE,
		.val	= PMD_SECT_APX | PMD_SECT_AP_WRITE,
		.set	= "    ro",
		.ro_bit	= true,
	}, {
		.mask	= PMD_SECT_APX | PMD_SECT_AP_READ | PMD_SECT_AP_WRITE,
		.val	= PMD_SECT_AP_WRITE,
		.set	= "    RW",
	}, {
		.mask	= PMD_SECT_APX | PMD_SECT_AP_READ | PMD_SECT_AP_WRITE,
		.val	= PMD_SECT_AP_READ,
		.set	= "USR ro",
	}, {
		.mask	= PMD_SECT_APX | PMD_SECT_AP_READ | PMD_SECT_AP_WRITE,
		.val	= PMD_SECT_AP_READ | PMD_SECT_AP_WRITE,
		.set	= "USR RW",
#else /* ARMv4/ARMv5  */
	/* These are approximate */
	{
		.mask   = PMD_SECT_AP_READ | PMD_SECT_AP_WRITE,
		.val    = 0,
		.set    = "    ro",
		.ro_bit	= true,
	}, {
		.mask   = PMD_SECT_AP_READ | PMD_SECT_AP_WRITE,
		.val    = PMD_SECT_AP_WRITE,
		.set    = "    RW",
	}, {
		.mask   = PMD_SECT_AP_READ | PMD_SECT_AP_WRITE,
		.val    = PMD_SECT_AP_READ,
		.set    = "USR ro",
	}, {
		.mask   = PMD_SECT_AP_READ | PMD_SECT_AP_WRITE,
		.val    = PMD_SECT_AP_READ | PMD_SECT_AP_WRITE,
		.set    = "USR RW",
#endif
	}, {
		.mask	= PMD_SECT_XN,
		.val	= PMD_SECT_XN,
		.set	= "NX",
		.clear	= "x ",
		.nx_bit	= true,
	}, {
		.mask	= PMD_SECT_S,
		.val	= PMD_SECT_S,
		.set	= "SHD",
		.clear	= "   ",
	},
};

struct pg_level {
	const char *name;
	const struct prot_bits *bits;
	size_t num;
	u64 mask;
	const struct prot_bits *ro_bit;
	const struct prot_bits *nx_bit;
};

static struct pg_level pg_level[] = {
	{
	}, { /* pgd */
	}, { /* p4d */
	}, { /* pud */
	}, { /* pmd */
		.name	= (CONFIG_PGTABLE_LEVELS > 2) ? "PMD" : "PGD",
		.bits	= section_bits,
		.num	= ARRAY_SIZE(section_bits),
	}, { /* pte */
		.name	= "PTE",
		.bits	= pte_bits,
		.num	= ARRAY_SIZE(pte_bits),
	},
};

static void dump_prot(struct pg_state *st, const struct prot_bits *bits, size_t num)
{
	unsigned i;

	for (i = 0; i < num; i++, bits++) {
		const char *s;

		if ((st->current_prot & bits->mask) == bits->val)
			s = bits->set;
		else
			s = bits->clear;

		if (s)
			pt_dump_seq_printf(st->seq, " %s", s);
	}
}

static void note_prot_wx(struct pg_state *st, unsigned long addr)
{
	if (!st->check_wx)
		return;
	if ((st->current_prot & pg_level[st->level].ro_bit->mask) ==
				pg_level[st->level].ro_bit->val)
		return;
	if ((st->current_prot & pg_level[st->level].nx_bit->mask) ==
				pg_level[st->level].nx_bit->val)
		return;

	WARN_ONCE(1, "arm/mm: Found insecure W+X mapping at address %pS\n",
			(void *)st->start_address);

	st->wx_pages += (addr - st->start_address) / PAGE_SIZE;
}

static void note_page(struct pg_state *st, unsigned long addr,
		      unsigned int level, u64 val, const char *domain)
{
	static const char units[] = "KMGTPE";
	u64 prot = val & pg_level[level].mask;

	if (!st->level) {
		st->level = level;
		st->current_prot = prot;
		st->current_domain = domain;
		pt_dump_seq_printf(st->seq, "---[ %s ]---\n", st->marker->name);
	} else if (prot != st->current_prot || level != st->level ||
		   domain != st->current_domain ||
		   addr >= st->marker[1].start_address) {
		const char *unit = units;
		unsigned long delta;

		if (st->current_prot) {
			note_prot_wx(st, addr);
			pt_dump_seq_printf(st->seq, "0x%08lx-0x%08lx   ",
				   st->start_address, addr);

			delta = (addr - st->start_address) >> 10;
			while (!(delta & 1023) && unit[1]) {
				delta >>= 10;
				unit++;
			}
			pt_dump_seq_printf(st->seq, "%9lu%c %s", delta, *unit,
					   pg_level[st->level].name);
			if (st->current_domain)
				pt_dump_seq_printf(st->seq, " %s",
							st->current_domain);
			if (pg_level[st->level].bits)
				dump_prot(st, pg_level[st->level].bits, pg_level[st->level].num);
			pt_dump_seq_printf(st->seq, "\n");
		}

		if (addr >= st->marker[1].start_address) {
			st->marker++;
			pt_dump_seq_printf(st->seq, "---[ %s ]---\n",
							st->marker->name);
		}
		st->start_address = addr;
		st->current_prot = prot;
		st->current_domain = domain;
		st->level = level;
	}
}

static const char *get_domain_name(pmdval_t pmdval)
{
#ifndef CONFIG_ARM_LPAE
	switch (pmdval & PMD_DOMAIN_MASK) {
	case PMD_DOMAIN(DOMAIN_KERNEL):
		return "KERNEL ";
	case PMD_DOMAIN(DOMAIN_USER):
		return "USER   ";
	case PMD_DOMAIN(DOMAIN_IO):
		return "IO     ";
	case PMD_DOMAIN(DOMAIN_VECTORS):
		return "VECTORS";
	default:
		return "unknown";
	}
#endif
	return NULL;
}

static void note_page_pte(struct ptdump_state *pt_st, unsigned long addr,
			  pte_t pte)
{
	struct pg_state *st = container_of(pt_st, struct pg_state, ptdump);

	note_page(st, addr, 5, pte_val(pte), st->pmd_domain);
}

static void note_page_pmd(struct ptdump_state *pt_st, unsigned long addr,
			  pmd_t pmd)
{
	struct pg_state *st = container_of(pt_st, struct pg_state, ptdump);

	note_page(st, addr, 4, pmd_val(pmd), get_domain_name(pmd_val(pmd)));

#ifndef CONFIG_ARM_LPAE
	/*
	 * In the classic page table format a pmd is a pair of 1MB section
	 * entries.  The walker only passes the first one, and a page table
	 * under it covers both, so only the second of a pair of sections
	 * needs showing here.
	 */
	if (SECTION_SIZE < PMD_SIZE) {
		pgd_t *pgd = pgd_offset(st->mm, addr);
		p4d_t *p4d = p4d_offset(pgd, addr);
		pud_t *pud = pud_offset(p4d, addr);
		pmd_t second = pmd_offset(pud, addr)[1];

		if (pmd_leaf(second))
			note_page(st, addr + SECTION_SIZE, 4, pmd_val(second),
				  get_domain_name(pmd_val(second)));
	}
#endif
}

static void note_page_pud(struct ptdump_state *pt_st, unsigned long addr,
			  pud_t pud)
{
	struct pg_state *st = container_of(pt_st, struct pg_state, ptdump);

	note_page(st, addr, 3, pud_val(pud), NULL);
}

static void note_page_p4d(struct ptdump_state *pt_st, unsigned long addr,
			  p4d_t p4d)
{
	struct pg_state *st = container_of(pt_st, struct pg_state, ptdump);

	note_page(st, addr, 2, p4d_val(p4d), NULL);
}

static void note_page_pgd(struct ptdump_state *pt_st, unsigned long addr,
			  pgd_t pgd)
{
	struct pg_state *st = container_of(pt_st, struct pg_state, ptdump);

	note_page(st, addr, 1, pgd_val(pgd), NULL);
}

static void note_page_flush(struct ptdump_state *pt_st)
{
	struct pg_state *st = container_of(pt_st, struct pg_state, ptdump);

	note_page(st, 0, 0, 0, NULL);
}

/*
 * Called for every pmd before it is either noted as a section or walked,
 * so the ptes under it know which domain they are in.
 */
static void effective_prot_pmd(struct ptdump_state *pt_st, pmd_t pmd)
{
	struct pg_state *st = container_of(pt_st, struct pg_state, ptdump);

	st->pmd_domain = get_domain_name(pmd_val(pmd));
}

static const struct ptdump_state ptdump_ops = {
	.note_page_pte = note_page_pte,
	.note_page_pmd = note_page_pmd,
	.note_page_pud = note_page_pud,
	.note_page_p4d = note_page_p4d,
	.note_page_pgd = note_page_pgd,
	.note_page_flush = note_page_flush,
	.effective_prot_pmd = effective_prot_pmd,
};

void ptdump_walk(struct seq_file *m, struct ptdump_info *info)
{
	struct pg_state st = {
		.ptdump = ptdump_ops,
		.mm = info->mm,
		.seq = m,
		.marker = info->markers,
		.check_wx = false,
	};

	st.ptdump.range = (struct ptdump_range[]) {
		{ info->base_addr, ~0UL },
		{ 0, 0 },
	};
	ptdump_walk_pgd(&st.ptdump, info->mm, NULL);
}

static void __init ptdump_initialize(void)
{
	unsigned i, j;

	for (i = 0; i < ARRAY_SIZE(pg_level); i++)
		if (pg_level[i].bits)
			for (j = 0; j < pg_level[i].num; j++) {
				pg_level[i].mask |= pg_level[i].bits[j].mask;
				if (pg_level[i].bits[j].ro_bit)
					pg_level[i].ro_bit = &pg_level[i].bits[j];
				if (pg_level[i].bits[j].nx_bit)
					pg_level[i].nx_bit = &pg_level[i].bits[j];
			}
#ifdef CONFIG_KASAN
	address_markers[4].start_address = VMALLOC_START;
#else
	address_markers[2].start_address = VMALLOC_START;
#endif
}

static struct ptdump_info kernel_ptdump_info = {
	.mm = &init_mm,
	.markers = address_markers,
	.base_addr = 0,
};

bool ptdump_check_wx(void)
{
	struct pg_state st = {
		.ptdump = ptdump_ops,
		.mm = &init_mm,
		.seq = NULL,
		.marker = (struct addr_marker[]) {
			{ 0, NULL},
			{ -1, NULL},
		},
		.check_wx = true,
	};

	st.ptdump.range = (struct ptdump_range[]) {
		{ 0, ~0UL },
		{ 0, 0 },
	};
	ptdump_walk_pgd(&st.ptdump, &init_mm, NULL);

	if (st.wx_pages) {
		pr_warn("Checked W+X mappings: FAILED, %lu W+X pages found\n",
			st.wx_pages);
		return false;
	}
	pr_info("Checked W+X mappings: passed, no W+X pages found\n");
	return true;
}

static int __init ptdump_init(void)
{
	ptdump_initialize();
	ptdump_debugfs_register(&kernel_ptdump_info, "kernel_page_tables");
	return 0;
}
__initcall(ptdump_init);
