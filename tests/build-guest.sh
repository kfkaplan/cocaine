#!/bin/sh
# Rebuilds the guest test programs in tests/bin/.
#
# The built programs are kept in the repository so the tests run without any
# extra tools. Rebuild them only after changing something in tests/guest/.
#
# Needs:
#   - Apple clang, which still compiles and links 32-bit Intel Mach-O.
#   - Homebrew LLVM (brew install llvm) for PowerPC: it compiles to ELF
#     objects, which tests/tools/elf2macho.py links into Mach-O.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
src=$here/guest
out=$here/bin
llvm=${LLVM:-/opt/homebrew/opt/llvm/bin}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# -fno-math-errno lets builtins like fma and sqrt become single instructions
# instead of calls into a C library the guests don't have.
common="-ffreestanding -fno-stack-protector -fno-common -fsigned-char -ffp-contract=off \
  -fno-math-errno -fno-unwind-tables -fno-asynchronous-unwind-tables -Wall -I$src"

ppc_cc="$llvm/clang -target powerpc-unknown-elf $common"
i386_cc="clang -target i386-apple-macosx10.5 -static $common"

g3="-mcpu=750"             # no AltiVec
g4="-mcpu=7400 -maltivec"  # AltiVec, which -O3 then uses on its own

rm -rf "$out"
mkdir -p "$out/ppc" "$out/i386"

$ppc_cc $g3 -c "$src/crt_ppc.S" -o "$tmp/crt_ppc.o"
$ppc_cc $g3 -O1 -fno-builtin -c "$src/rt.c" -o "$tmp/rt_ppc.o"
$i386_cc -c "$src/crt_i386.S" -o "$tmp/crt_i386.o"
$i386_cc -O1 -fno-builtin -c "$src/rt.c" -o "$tmp/rt_i386.o"

# ppc <source> <output name> <compiler flags...>
ppc() {
  source=$1 name=$2
  shift 2
  $ppc_cc "$@" -c "$src/$source.c" -o "$tmp/$name.ppc.o"
  python3 "$here/tools/elf2macho.py" -o "$out/ppc/$name" \
    "$tmp/crt_ppc.o" "$tmp/rt_ppc.o" "$tmp/$name.ppc.o"
  chmod +x "$out/ppc/$name"
  echo "built ppc/$name"
}

# i386 <source> <output name> <compiler flags...>
i386() {
  source=$1 name=$2
  shift 2
  $i386_cc "$@" -c "$src/$source.c" -o "$tmp/$name.i386.o"
  $i386_cc -nostdlib -Wl,-e,start -o "$out/i386/$name" \
    "$tmp/crt_i386.o" "$tmp/rt_i386.o" "$tmp/$name.i386.o" 2>&1 |
    grep -v "i386 architecture is deprecated" || true
  chmod +x "$out/i386/$name"
  echo "built i386/$name"
}

ppc hello hello $g3 -O1
i386 hello hello -O1

for opt in O0 O2 Os; do
  ppc t_int t_int_$opt $g3 -$opt
  i386 t_int t_int_$opt -$opt
done
ppc t_int t_int_O3_g4 $g4 -O3
i386 t_int t_int_O3 -O3
# Integer instructions only, the way compilers wrote code before SSE.
i386 t_int t_int_O2_nosse -O2 -mno-sse -mno-mmx

for opt in O0 O2; do
  ppc t_fp t_fp_$opt $g3 -$opt
  i386 t_fp t_fp_$opt -$opt
  ppc t_fma t_fma_$opt $g3 -$opt
  i386 t_x87 t_x87_$opt -$opt
  i386 t_sse t_sse_$opt -$opt
  ppc t_signal t_signal_$opt $g3 -$opt
  i386 t_signal t_signal_$opt -$opt
done
ppc t_fp t_fp_O3_g4 $g4 -O3
i386 t_fp t_fp_O3 -O3
ppc t_altivec t_altivec_O0 $g4 -O0
ppc t_altivec t_altivec_O2 $g4 -O2

# A dynamically linked test, against the real libSystem. It can only be built
# (and run) once a system root has been imported.
root=$here/../sysroots/10.5
if [ -f "$root/usr/lib/libSystem.B.dylib" ]; then
  mkdir -p "$here/bin-dynamic/i386"
  clang -target i386-apple-macosx10.5 -isysroot "$root" -O1 -fno-stack-protector -nostdlib \
    -Wl,-e,_main "$src/t_threads.c" "$root/usr/lib/libSystem.B.dylib" \
    -o "$here/bin-dynamic/i386/t_threads" 2>&1 | grep -v "i386 architecture is deprecated" || true
  echo "built i386/t_threads (dynamic)"
  clang -target i386-apple-macosx10.5 -isysroot "$root" -O1 -fno-stack-protector -nostdlib \
    -Wl,-e,_main "$src/t_ipc.c" "$root/usr/lib/libSystem.B.dylib" \
    -o "$here/bin-dynamic/i386/t_ipc" 2>&1 | grep -v "i386 architecture is deprecated" || true
  echo "built i386/t_ipc (dynamic)"
fi
