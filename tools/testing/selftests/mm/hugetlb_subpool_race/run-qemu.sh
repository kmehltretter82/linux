#!/bin/sh
set -eu

if [ "$#" -ne 5 ]; then
	echo "usage: $0 reservation|allocation KERNEL INITRAMFS 1|4 LOG" >&2
	exit 2
fi

case_name=$1
kernel=$2
initramfs=$3
cpus=$4
log=$5
directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
qemu=${QEMU:-qemu-system-x86_64}

case "$cpus" in 1|4) ;; *) echo "vCPUs must be 1 or 4" >&2; exit 2 ;; esac
case "$case_name" in
reservation)
	case_arg=earlyproof_cpus=$cpus
	validator=$directory/validate-reservation.py
	;;
allocation)
	case_arg=allocproof_cpus=$cpus
	validator=$directory/validate-allocation.py
	;;
*)
	echo "unknown case: $case_name" >&2
	exit 2
	;;
esac

[ -f "$kernel" ] || { echo "missing kernel: $kernel" >&2; exit 1; }
[ -f "$initramfs" ] || { echo "missing initramfs: $initramfs" >&2; exit 1; }
command -v "$qemu" >/dev/null
command -v timeout >/dev/null

set +e
timeout --foreground -k 10 300 "$qemu" \
	-machine q35,accel=tcg -cpu max -smp "$cpus" -m 1G \
	-display none -serial stdio -monitor none -nic none -no-reboot \
	-kernel "$kernel" -initrd "$initramfs" \
	-append "console=ttyS0 rdinit=/init panic=-1 oops=panic quiet loglevel=4 default_hugepagesz=2M hugepagesz=2M hugepages=7 $case_arg" \
	> "$log" 2>&1
qemu_rc=$?
set -e

if [ "$qemu_rc" -ne 0 ]; then
	echo "QEMU failed with exit code $qemu_rc; console: $log" >&2
	exit 1
fi

normalized=$log.normalized
tr -d '\r' < "$log" > "$normalized"
python3 -B "$validator" "$normalized" "$cpus"
printf 'console=%s\nnormalized_console=%s\n' "$log" "$normalized"
