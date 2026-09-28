#!/bin/sh
set -eu

if [ "$#" -ne 4 ]; then
	echo "usage: $0 reservation|allocation BUSYBOX GEN_INIT_CPIO OUTPUT" >&2
	exit 2
fi

case_name=$1
busybox=$2
packtool=$3
output=$4
directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$directory/../../../../.." && pwd)

case "$case_name" in
reservation)
	init=$directory/init-reservation
	helper=$directory/early-cleanup-proof
	guest_helper=early-cleanup-proof
	;;
allocation)
	init=$directory/init-allocation
	helper=$directory/allocation-cleanup-proof
	guest_helper=allocation-cleanup-proof
	;;
*)
	echo "unknown case: $case_name" >&2
	exit 2
	;;
esac

[ -x "$busybox" ] || { echo "not executable: $busybox" >&2; exit 1; }
[ -x "$packtool" ] || { echo "not executable: $packtool" >&2; exit 1; }

make -C "$directory" LDFLAGS=-static all
if readelf -l "$helper" | grep -q INTERP; then
	echo "controller is dynamically linked: $helper" >&2
	exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
provenance=$tmp/test-provenance
list=$tmp/cpio.list
{
	printf 'case=%s\n' "$case_name"
	printf 'source_commit=%s\n' "$(git -C "$repo" rev-parse HEAD)"
	printf 'instrumentation=default-off test only\n'
	sha256sum "$init" "$helper" "$busybox"
} > "$provenance"

{
	printf 'dir /bin 0755 0 0\n'
	printf 'dir /dev 0755 0 0\n'
	printf 'dir /proc 0755 0 0\n'
	printf 'dir /sys 0755 0 0\n'
	printf 'dir /tmp 1777 0 0\n'
	printf 'dir /tests 0755 0 0\n'
	printf 'dir /etc 0755 0 0\n'
	printf 'dir /mnt 0755 0 0\n'
	printf 'dir /mnt/main 0755 0 0\n'
	printf 'dir /mnt/steal 0755 0 0\n'
	printf 'nod /dev/console 0600 0 0 c 5 1\n'
	printf 'nod /dev/null 0666 0 0 c 1 3\n'
	printf 'file /bin/busybox %s 0755 0 0\n' "$busybox"
	for applet in sh mount umount cat grep awk sed uname dmesg sync \
		poweroff sleep mkdir; do
		printf 'slink /bin/%s busybox 0777 0 0\n' "$applet"
	done
	printf 'file /init %s 0755 0 0\n' "$init"
	printf 'file /tests/%s %s 0755 0 0\n' "$guest_helper" "$helper"
	printf 'file /etc/test-provenance %s 0644 0 0\n' "$provenance"
} > "$list"

"$packtool" -t 1790409600 "$list" | gzip -n > "$output"
gzip -t "$output"
printf 'created %s\n' "$output"
