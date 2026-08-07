#!/bin/sh
GCC=gcc
GCCOPTS="-Wall -std=gnu99  -pthread"
LINKOPTS=""
/bin/rm -f *.exe *.s
$GCC $GCCOPTS -O2 -c outs.c
$GCC $GCCOPTS -O2 -c utils.c
$GCC $GCCOPTS -O2 -c litmus_rand.c
$GCC $GCCOPTS $LINKOPTS -o ipset-cidr-old-barriers.exe outs.o utils.o litmus_rand.o ipset-cidr-old-barriers.c
$GCC $GCCOPTS -S ipset-cidr-old-barriers.c && awk -f show.awk ipset-cidr-old-barriers.s > ipset-cidr-old-barriers.t && /bin/rm ipset-cidr-old-barriers.s
