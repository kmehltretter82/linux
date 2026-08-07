# Weak-memory litmus CI (ipset cidr bookkeeping)

These litmus tests model the netfilter **ipset** cidr-bookkeeping data race that
was reworked in mainline commit `8e5fd2a55e246` ("netfilter: ipset: rework cidr
bookkeeping"), originally flagged as unsafe on weakly-ordered architectures.

| File | Models | herd7 verdict |
|------|--------|---------------|
| `ipset-cidr-old-inplace.litmus` | old in-place update, lockless reader | **Sometimes** (bug reachable) |
| `ipset-cidr-old-barriers.litmus` | same + `smp_wmb`/`smp_rmb` | **Never** |
| `ipset-cidr-new-rcu.litmus` | the merged `rcu_assign_pointer`/`rcu_dereference` fix | **Never** |

## What the CI does

`.github/workflows/weakmem-litmus.yml` runs on a **free GitHub-hosted arm64
runner** (4-vCPU Azure Cobalt 100 = Arm Neoverse N2, ARMv9.0-A). That is real
Arm silicon, so it genuinely reorders memory -- unlike QEMU-TCG, which cannot
exhibit the reordering at all. The job uses `litmus7` (userspace, no kernel
module, no root) to build and hammer each test across all cores, then reports
whether the "torn read" state `1:r0=1; 1:r1=0` was actually observed.

Expected result: `old-inplace` -> **Sometimes** (the bug is real on hardware),
`old-barriers` -> **Never** (the ordering fix holds).

## Reproduce locally

Formal model (any host, exhaustive proof):

    herd7 -conf linux-kernel.cfg ipset-cidr-old-inplace.litmus   # -> Sometimes

Empirical, on real weak hardware (e.g. a Raspberry Pi 400 / any arm64 box):

    mkdir out && litmus7 -carch AArch64 -a "$(nproc)" -o out ipset-cidr-old-inplace.litmus
    cd out && sh comp.sh && ./ipset-cidr-old-inplace.exe -a "$(nproc)" -s 1000000 -r 400
