#ifndef _ASM_ARM_VMALLOC_H
#define _ASM_ARM_VMALLOC_H

#include <asm/page.h>

#ifdef CONFIG_HAVE_ARCH_HUGE_VMAP

#define arch_vmap_pmd_supported arch_vmap_pmd_supported
static inline bool arch_vmap_pmd_supported(pgprot_t prot)
{
	/* Only LPAE, where a PMD maps a 2MB section, selects HAVE_ARCH_HUGE_VMAP. */
	return true;
}

#define arch_vmap_pud_supported arch_vmap_pud_supported
static inline bool arch_vmap_pud_supported(pgprot_t prot)
{
	/* The PUD is folded onto the PGD on LPAE, so no PUD-sized blocks. */
	return false;
}

#endif /* CONFIG_HAVE_ARCH_HUGE_VMAP */

#endif /* _ASM_ARM_VMALLOC_H */
