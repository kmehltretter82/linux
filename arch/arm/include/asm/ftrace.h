/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_ARM_FTRACE
#define _ASM_ARM_FTRACE

#define HAVE_FUNCTION_GRAPH_FP_TEST

#if defined(CONFIG_DYNAMIC_FTRACE_WITH_REGS) || \
	defined(CONFIG_DYNAMIC_FTRACE_WITH_ARGS)
#define ARCH_SUPPORTS_FTRACE_OPS 1
#endif

#ifdef CONFIG_FUNCTION_TRACER
#define MCOUNT_ADDR		((unsigned long)(__gnu_mcount_nc))
#define MCOUNT_INSN_SIZE	4 /* sizeof mcount call */

#ifndef __ASSEMBLY__
extern void __gnu_mcount_nc(void);

#ifdef CONFIG_DYNAMIC_FTRACE
struct dyn_arch_ftrace {
#ifdef CONFIG_ARM_MODULE_PLTS
	struct module *mod;
#endif
};

static inline unsigned long ftrace_call_adjust(unsigned long addr)
{
	/* With Thumb-2, the recorded addresses have the lsb set */
	return addr & ~1;
}
#endif

#endif

#endif

#ifndef __ASSEMBLY__

#if defined(CONFIG_FRAME_POINTER) && !defined(CONFIG_ARM_UNWIND)
/*
 * return_address uses walk_stackframe to do it's work.  If both
 * CONFIG_FRAME_POINTER=y and CONFIG_ARM_UNWIND=y walk_stackframe uses unwind
 * information.  For this to work in the function tracer many functions would
 * have to be marked with __notrace.  So for now just depend on
 * !CONFIG_ARM_UNWIND.
 */

void *return_address(unsigned int);

#else

static inline void *return_address(unsigned int level)
{
       return NULL;
}

#endif

#define ftrace_return_address(n) return_address(n)

#define ARCH_HAS_SYSCALL_MATCH_SYM_NAME

#ifdef CONFIG_DYNAMIC_FTRACE_WITH_ARGS
struct ftrace_ops;
struct ftrace_regs;
#define ftrace_graph_func ftrace_graph_func
void ftrace_graph_func(unsigned long ip, unsigned long parent_ip,
		       struct ftrace_ops *op, struct ftrace_regs *fregs);

/*
 * Both ftrace trampolines build a full pt_regs, but only the SAVE_REGS one
 * (ftrace_regs_caller) stores a real CPSR; the args-only ftrace_caller leaves
 * it zero.  Return the pt_regs only for a SAVE_REGS frame, as the generic
 * contract wants; args-only callers reach the registers through
 * ftrace_partial_regs() (HAVE_FTRACE_REGS_HAVING_PT_REGS).  A macro so
 * arch_ftrace_regs() resolves at the call site.
 */
#define arch_ftrace_get_regs(fregs)					\
	(arch_ftrace_regs(fregs)->regs.ARM_cpsr ?			\
	 &arch_ftrace_regs(fregs)->regs : NULL)

#define ftrace_regs_set_instruction_pointer(fregs, ip) \
	(arch_ftrace_regs(fregs)->regs.ARM_pc = (ip))

/*
 * A macro, not an inline, so arch_ftrace_regs() resolves at the call site:
 * it comes from <linux/ftrace_regs.h>, included after this header.
 */
#define ftrace_regs_get_return_address(fregs) \
	(arch_ftrace_regs(fregs)->regs.ARM_lr)
#endif

static inline bool arch_syscall_match_sym_name(const char *sym,
					       const char *name)
{
	if (!strcmp(sym, "sys_mmap2"))
		sym = "sys_mmap_pgoff";
	else if (!strcmp(sym, "sys_statfs64_wrapper"))
		sym = "sys_statfs64";
	else if (!strcmp(sym, "sys_fstatfs64_wrapper"))
		sym = "sys_fstatfs64";
	else if (!strcmp(sym, "sys_arm_fadvise64_64"))
		sym = "sys_fadvise64_64";

	/* Ignore case since sym may start with "SyS" instead of "sys" */
	return !strcasecmp(sym, name);
}

void prepare_ftrace_return(unsigned long *parent, unsigned long self,
			   unsigned long frame_pointer,
			   unsigned long stack_pointer);

#endif /* ifndef __ASSEMBLY__ */

#endif /* _ASM_ARM_FTRACE */
