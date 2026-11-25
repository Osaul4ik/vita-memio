#!/bin/sh
# tools/check_novfp.sh OBJDUMP ELF... - fails if any ELF holds a VFP/NEON
# instruction (kernel code must not touch those registers: they hold the
# waiting user thread's state). Prints how many, by function, if so; also
# fails if an ELF disassembles to (almost) nothing, so an empty listing can
# never pass as "none found".
set -e
od=$1
shift
cc=${od%objdump}gcc
if command -v "$cc" >/dev/null 2>&1; then
	echo "check-novfp: $cc defaults:" $("$cc" -Q --help=target 2>/dev/null |
		awk '$1 ~ /^-m(fpu|float-abi|cpu|arch|thumb)=?$/ { printf "%s%s ", $1, $2 }')
fi
bad=0
for f in "$@"; do
	test -s "$f" || { echo "check-novfp: missing $f"; exit 1; }
	set -- $("$od" -d "$f" | awk '
		/^[0-9a-f]+ <.*>:$/ { fn = $2 }
		/^ +[0-9a-f]+:\t/ {
			n = split($0, a, "\t")
			if (n < 3) next
			all++
			if (a[3] ~ /^v[a-z]/) { c[fn]++; t++ }
		}
		END { for (k in c) print c[k], k > "/dev/stderr"; print t + 0, all + 0 }')
	echo "check-novfp: $f: $1 VFP/NEON instructions of $2"
	[ "$2" -ge 1000 ] || { echo "check-novfp: $f: too few instructions listed"; bad=1; }
	[ "$1" -eq 0 ] || bad=1
done
exit $bad
