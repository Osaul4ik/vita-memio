#!/bin/sh
# tools/check_novfp.sh OBJDUMP ELF... - fails if any ELF holds a VFP/NEON
# instruction (kernel code must not touch those registers: they hold the
# waiting user thread's state). Prints how many, by function, if so.
set -e
od=$1
shift
bad=0
for f in "$@"; do
	test -s "$f" || { echo "check-novfp: missing $f"; exit 1; }
	n=$("$od" -d "$f" | awk '
		/^[0-9a-f]+ <.*>:$/ { fn = $2 }
		/^ +[0-9a-f]+:\t/ { split($0, a, "\t"); if (a[3] ~ /^v[a-z]/) { c[fn]++; t++ } }
		END { for (k in c) print c[k], k > "/dev/stderr"; print t + 0 }')
	echo "check-novfp: $f: $n VFP/NEON instructions"
	[ "$n" -eq 0 ] || bad=1
done
exit $bad
