#!/bin/sh
# time.sh <program> <part...>: the seconds each part takes, translated and
# interpreted, and what it printed (which must be the same both ways).
# For maths: mandel singles doubles convert extended.
prog=$1
shift
for part in "$@"; do
  for jit in 1 0; do
    s=$(python3 -c "import time; print(time.time())")
    out=$(COCAINE_NO_VIEWER=1 COCAINE_JIT=$jit ./build/cocaine "$prog" "$part" 2>&1 | tr '\n' ' ')
    e=$(python3 -c "import time; print(time.time())")
    python3 -c "print('%-9s translator %-3s %6.2f s   %s' % ('$part', 'on' if $jit else 'off', $e-$s, '$out'))"
  done
done
