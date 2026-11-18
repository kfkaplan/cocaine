/* Fused multiply-add: a * b + c with one rounding instead of two.
 *
 * PowerPC has this as an instruction (fmadd), and so does this Mac, so the
 * two must agree bit for bit. The first Intel Macs had no such instruction,
 * so this test is PowerPC only. */
#include "rt.h"

static const volatile double dv[] = {
    0.0,  -0.0, 1.0,   -1.0,  0.5,   2.0,     3.141592653589793,      -2.718281828459045,
    0.1,  1e-300, 1e300, 1.7976931348623157e308, 4.9e-324, 123456789.125, -987654.321,
    1e15 + 0.3, 16777217.0, 1e-40, 3.4028234663852886e38, 1.0 / 3.0,
};
#define NDV (sizeof dv / sizeof dv[0])

static u64 dbits(double d) {
  u64 b;
  __builtin_memcpy(&b, &d, 8);
  if ((b & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (b & 0x000FFFFFFFFFFFFFull)) {
    return 0x7FF8000000000000ull;
  }
  return b;
}

static u32 fbits(float f) {
  u32 b;
  __builtin_memcpy(&b, &f, 4);
  if ((b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu)) return 0x7FC00000u;
  return b;
}

/* -(a * b + c). On PowerPC, writing it this way gets the fnmadd and fnmsub
 * instructions, which is the point of the test. The native reference must
 * not write it this way: the ARM compiler folds the negation into an ARM
 * multiply-add that negates before adding, and a sum that cancels then comes
 * out as +0 instead of -0. So the reference flips the sign bit out of line. */
#ifdef RT_HOST
static NOINLINE double negated(double r) {
  u64 b;
  __builtin_memcpy(&b, &r, 8);
  b ^= 0x8000000000000000ull;
  __builtin_memcpy(&r, &b, 8);
  return r;
}
#define NEG_FMA(a, b, c) negated(__builtin_fma((a), (b), (c)))
#else
#define NEG_FMA(a, b, c) (-__builtin_fma((a), (b), (c)))
#endif

int test_main(int argc, char** argv, char** envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  u32 h_double = 1, h_single = 2, h_neg = 3;
  for (u32 i = 0; i < NDV; i++) {
    for (u32 j = 0; j < NDV; j++) {
      const double a = dv[i], b = dv[j], c = dv[(i + j) % NDV], d = dv[(i * 7 + j) % NDV];
      h_double = mix64(h_double, dbits(__builtin_fma(a, b, c)));
      h_double = mix64(h_double, dbits(__builtin_fma(a, b, -d)));
      h_neg = mix64(h_neg, dbits(NEG_FMA(a, b, c)));
      h_neg = mix64(h_neg, dbits(NEG_FMA(a, b, -d)));
      h_single = mix(h_single, fbits(__builtin_fmaf((float)a, (float)b, (float)c)));
      h_single = mix(h_single, fbits(__builtin_fmaf((float)a, (float)b, -(float)d)));
    }
  }
  put_kv("fma.double", h_double);
  put_kv("fma.negated", h_neg);
  put_kv("fma.single", h_single);
  put_str("done\n");
  return 0;
}
