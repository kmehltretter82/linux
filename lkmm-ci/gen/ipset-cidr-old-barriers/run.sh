#!/bin/sh

date
LITMUSOPTS="${@:-$LITMUSOPTS}"
SLEEP=0
if [ ! -f ipset-cidr-old-barriers.no ]; then
cat <<'EOF'
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
% Results for ipset-cidr-old-barriers.litmus %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
C ipset-cidr-old-barriers

{
}
P0(int* cidr, int* count) {

	WRITE_ONCE(*count, 1);
	smp_wmb();
	WRITE_ONCE(*cidr, 1);

}

P1(int* cidr, int* count) {

	int r0;
	int r1;

	r0 = READ_ONCE(*cidr);
	smp_rmb();
	r1 = READ_ONCE(*count);

}


exists (1:r0=1 /\ 1:r1=0)
Generated assembler
EOF
cat ipset-cidr-old-barriers.t
./ipset-cidr-old-barriers.exe -q $LITMUSOPTS
fi
sleep $SLEEP

cat <<'EOF'
Revision exported, version 7.58
Command line: litmus7 -set-libdir /home/karl/linux-work/herdtools7-lib/litmus/libdir -carch AArch64 -o gen/ipset-cidr-old-barriers ipset-cidr-old-barriers.litmus
Parameters
#define SIZE_OF_TEST 100000
#define NUMBER_OF_RUN 10
#define AVAIL 1
#define STRIDE (-1)
#define MAX_LOOP 0
/* gcc options: -Wall -std=gnu99  -pthread */
/* barrier: user */
/* launch: changing */
/* affinity: none */
/* memory: direct */
/* safer: write */
/* preload: random */
/* speedcheck: no */
/* alloc: dynamic */
EOF
sed '2q;d' comp.sh
echo "LITMUSOPTS=$LITMUSOPTS"
date
