#!/bin/sh
# MAME's validity checks, as MAME's CI, and our math (libm-check.sh), on every build. Allowed: the
# errors of building part of MAME, clones whose parent is in a file that sources.txt leaves out (in
# segas16a.cpp and gaelco.cpp, of no game in games.txt), and nothing else, a crash neither.
# $1: the program; $2: the build directory.
s=0; "$1" -validate > validate.txt 2>&1 || s=$?
cat validate.txt
n=$(grep -c 'clone of nonexistent driver' validate.txt)
[ $s = 0 ] || { [ $s = 1 ] && grep -qF "Validity check failed ($n errors," validate.txt; } || { echo "validity checks: exit $s"; exit 1; }
"$(dirname "$0")/libm-check.sh" "$2"
