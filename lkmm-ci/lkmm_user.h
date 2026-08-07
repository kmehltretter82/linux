#ifndef LKMM_USER_H
#define LKMM_USER_H
/*
 * Userspace stand-ins for the Linux-kernel memory-model macros that litmus7's
 * C output emits. litmus7 (unlike the kernel-module klitmus7) does not define
 * these, so we force-include this header when compiling the harness.
 *
 * Semantics match LKMM for this demonstration: plain accesses go through
 * volatile (the compiler keeps them in order, the CPU may still reorder them
 * on a weakly-ordered machine), and smp_wmb()/smp_rmb() are one-way fences.
 */
#ifndef ACCESS_ONCE
#define ACCESS_ONCE(x) (*(volatile __typeof__(x) *)&(x))
#endif
#ifndef WRITE_ONCE
#define WRITE_ONCE(x, v) (ACCESS_ONCE(x) = (v))
#endif
#ifndef READ_ONCE
#define READ_ONCE(x) ACCESS_ONCE(x)
#endif
#ifndef smp_mb
#define smp_mb()  __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif
#ifndef smp_wmb
#define smp_wmb() __atomic_thread_fence(__ATOMIC_RELEASE)
#endif
#ifndef smp_rmb
#define smp_rmb() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#endif
#ifndef barrier
#define barrier() __asm__ __volatile__("" ::: "memory")
#endif
#endif /* LKMM_USER_H */
