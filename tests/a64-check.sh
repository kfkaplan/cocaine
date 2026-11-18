#!/bin/sh
# Checks the ARM64 instruction encodings in src/cpu/ppc/a64.h against the
# system's assembler: every instruction the PowerPC translator can emit is
# written out both ways and the two must match bit for bit.
#
# usage: a64-check.sh [work directory]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
work=${1:-$(mktemp -d)}
mkdir -p "$work"
c++ -std=c++20 -I"$here/../src" "$here/tools/a64-check.cpp" -o "$work/a64-check"
"$work/a64-check" > "$work/a64.txt"
cut -f2 "$work/a64.txt" > "$work/a64.s"
clang -c -arch arm64 "$work/a64.s" -o "$work/a64.o"
# otool prints the section as words, four to a line after an address.
otool -t "$work/a64.o" | tail -n +3 | cut -f2- -d' ' | tr ' \t' '\n\n' | grep -E '^[0-9a-f]{8}$' > "$work/assembled.txt"
cut -f1 "$work/a64.txt" > "$work/ours.txt"
if paste "$work/ours.txt" "$work/assembled.txt" "$work/a64.s" | awk -F'\t' '$1 != $2 { print "wrong: " $3 ": have " $1 ", the assembler says " $2; bad = 1 } END { exit bad }'; then
  echo "ok    $(wc -l < "$work/ours.txt" | tr -d ' ') ARM64 encodings match the assembler"
else
  exit 1
fi
