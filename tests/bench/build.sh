#!/bin/sh
# build.sh <name>: builds tests/bench/<name>.c for 32-bit Intel into
# build/bench/<name>, the way tests/build-guest.sh builds the Intel tests.
# Run it from the top of the source tree.
set -eu
src=tests/bench
out=build/bench
mkdir -p $out
cc="clang -target i386-apple-macosx10.5 -static -ffreestanding -fno-stack-protector -fno-common -fsigned-char -ffp-contract=off -fno-math-errno -fno-unwind-tables -fno-asynchronous-unwind-tables -Itests/guest"
$cc -c tests/guest/crt_i386.S -o $out/crt.o
$cc -O1 -fno-builtin -c tests/guest/rt.c -o $out/rt.o
$cc -O2 -c $src/$1.c -o $out/$1.o
$cc -nostdlib -Wl,-e,start -o $out/$1 $out/crt.o $out/rt.o $out/$1.o 2>&1 | grep -v "i386 architecture is deprecated" || true
