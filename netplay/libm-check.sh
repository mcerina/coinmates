#!/bin/sh
# The elementary functions that the build calls must all be ours (src/lib/util/libm.c, CORE-MATH,
# musl's fma): one of the system's C library gives other bits on another system, and the players'
# states differ. Checked on the objects, not on the program: on Windows the C library's are also
# linked in, where an import list doesn't show them; a call through __imp_, to a DLL, isn't ours.
# The long double ones aren't checked: only the netlist's pfunction<long double> has them, never used.
# $1: the build directory. NM: the toolchain's nm.
set -e
nm=${NM:-nm}
# macOS puts _ before C names
case $(uname) in Darwin) strip='s/^_//' ;; *) strip= ;; esac
elementary() {
  awk 'NF { print $NF }' | sed "$strip" | sort -u |
    grep -xE '(__imp_)?((a?(sin|cos|tan)h?|atan2|exp(2|10|m1)?|log(2|10|1p)?|pow|hypot|cbrt|sincos|erfc?|[lt]gamma|fma)f?|__sincosf?_stret)' || true
}
ours=$(find "$1" -name libm.o -o -name libcoremath.a)
[ -n "$ours" ] || { echo "no libm.o in $1"; exit 1; }
ours=$($nm -U $ours | elementary)
echo "$ours" | grep -qx sin || { echo "no sin in libm.o: $nm can't read the objects"; exit 1; }
called=$(find "$1" -name '*.a' -o -name '*.o' | xargs $nm -u 2>/dev/null | elementary)
missing=$(printf '%s\n' "$called" | grep -vxF "$ours" || true)
[ -z "$missing" ] || { echo "from the C library, add them to src/lib/util/libm.c:" $missing; exit 1; }
echo "elementary functions, all ours:" $called
