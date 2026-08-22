/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2014 ARM Ltd. */
#ifndef __ASM_PTDUMP_H
#define __ASM_PTDUMP_H

#ifdef CONFIG_PTDUMP

#include <linux/mm_types.h>
#include <linux/seq_file.h>

struct addr_marker {
	unsigned long start_address;
	char *name;
};

struct ptdump_info {
	struct mm_struct		*mm;
	const struct addr_marker	*markers;
	unsigned long			base_addr;
};

void ptdump_walk(struct seq_file *s, struct ptdump_info *info);
#ifdef CONFIG_PTDUMP_DEBUGFS
#define EFI_RUNTIME_MAP_END	SZ_1G
void ptdump_debugfs_register(struct ptdump_info *info, const char *name);
#else
static inline void ptdump_debugfs_register(struct ptdump_info *info,
					   const char *name) { }
#endif /* CONFIG_PTDUMP_DEBUGFS */


#endif /* CONFIG_PTDUMP */


#endif /* __ASM_PTDUMP_H */
