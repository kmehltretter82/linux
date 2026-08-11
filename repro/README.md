# RISC-V Vector warning evidence

This branch contains reproduction material only.  It is an investigation
record, not a claim that a minimized syzkaller reproducer exists.

## Bases

The warning quoted in the original mail was found while fuzzing
`db03c02b8fcc` (`kcov-pause-v2`), which is five KCOV patches on
`8ba098e6b6ff` (v7.2-rc5).  That exact branch is published separately as
`kcov-pause-v2` in this fork.

This branch is based directly on clean `b9b3e33b70b7` (v7.2-rc6).  It adds
only the files under `repro/`; there are no kernel changes.

## Syzkaller record

`syzkaller/` contains the complete saved record from cycle 20 of a normal
TCG run on the KCOV branch:

* QEMU: two vCPUs, `-machine virt -cpu max`;
* syzkaller: `procs=4`, `cover=true`, `reproduce=false`;
* configuration: `config/riscv64-syzkaller.config`;
* result: the Vector warning in `report0` from the TCP vector-usercopy
  path.

Because `reproduce` was false, syzkaller did not save a minimized `.prog`
or C reproducer.  `log0` records the programs that were executing near the
warning, but none is established as its trigger.

The absolute paths and SSH-key location from the live manager configuration
are deliberately omitted; `syzkaller/manager.cfg` preserves its relevant
settings.

## Standalone stress test

`vector-stress.c` is the manual pipe-usercopy stress program used for the
QEMU experiments.  It enables KCOV per worker and verifies separate source
and destination buffers after each round trip.

On clean `b9b3e33b70b7`, with `CONFIG_PREEMPT_LAZY=y`,
`CONFIG_RISCV_ISA_V_PREEMPTIVE=y`, and `CONFIG_KCOV=y`, the following QEMU
mode produced 18 warnings:

```
qemu-system-riscv64 -machine virt -cpu max -smp 1 -nographic \
  -icount shift=7 -kernel arch/riscv/boot/Image -initrd initramfs.cpio.gz \
  -append 'console=ttyS0 earlycon=sbi rdinit=/init'
```

The same workload at normal TCG timing did not produce a warning.  All
completed runs reported `failures=0`; no data corruption was observed.
