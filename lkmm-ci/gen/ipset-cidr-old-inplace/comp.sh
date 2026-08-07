#!/bin/sh
GCC=gcc
GCCOPTS="-Wall -std=gnu99  -pthread"
LINKOPTS=""
/bin/rm -f *.exe *.s
$GCC $GCCOPTS -O2 -c outs.c
$GCC $GCCOPTS -O2 -c utils.c
$GCC $GCCOPTS -O2 -c litmus_rand.c
$GCC $GCCOPTS $LINKOPTS -o ipset-cidr-old-inplace.exe outs.o utils.o litmus_rand.o ipset-cidr-old-inplace.c
$GCC $GCCOPTS -S ipset-cidr-old-inplace.c && awk -f show.awk ipset-cidr-old-inplace.s > ipset-cidr-old-inplace.t && /bin/rm ipset-cidr-old-inplace.s
