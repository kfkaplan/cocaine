/* Floating point test for the CPU cores.
 *
 * Prints hashes of result bit patterns, so the emulated output must match the
 * native output bit for bit. All builds use -ffp-contract=off so nothing is
 * fused behind the test's back. NaN results are folded to one pattern because
 * CPUs legitimately differ in the NaN they produce. */
#include "rt.h"

static const volatile double dv[] = {
    0.0,
    -0.0,
    1.0,
    -1.0,
    0.5,
    2.0,
    3.141592653589793,
    -2.718281828459045,
    0.1,
    1.0 / 3.0,
    1e-300,
    1e300,
    1.7976931348623157e308,
    4.9e-324,
    2.2250738585072014e-308,
    123456789.125,
    -987654.321,
    1e15 + 0.3,
    4294967296.0,
    2147483647.0,
    -2147483648.0,
    16777217.0,
    1e-40,
    3.4028234663852886e38,
};
#define NDV (sizeof dv / sizeof dv[0])

static const volatile s32 iv[] = {
    0, 1, -1, 2, 7, -100, 255, 65536, 16777216, 16777217, 123456789, -123456789,
    2147483647, -2147483647 - 1, 1000000000, -5,
};
#define NIV (sizeof iv / sizeof iv[0])

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

static void test_double(void) {
  u32 h_add = 1, h_mul = 2, h_div = 3, h_cmp = 4, h_misc = 5;
  const volatile double inf = __builtin_inf(), nan = __builtin_nan("");
  for (u32 i = 0; i < NDV + 2; i++) {
    for (u32 j = 0; j < NDV + 2; j++) {
      const double a = i < NDV ? dv[i] : (i == NDV ? inf : nan);
      const double b = j < NDV ? dv[j] : (j == NDV ? -inf : nan);
      h_add = mix64(h_add, dbits(a + b));
      h_add = mix64(h_add, dbits(a - b));
      h_mul = mix64(h_mul, dbits(a * b));
      h_div = mix64(h_div, dbits(a / b));
      h_cmp = mix(h_cmp, (u32)(a < b) | (u32)(a <= b) << 1 | (u32)(a == b) << 2 |
                             (u32)(a != b) << 3 | (u32)(a > b) << 4 | (u32)(a >= b) << 5);
      h_misc = mix64(h_misc, dbits(__builtin_fabs(a) - b));
      h_misc = mix64(h_misc, dbits(-a));
      h_misc = mix64(h_misc, dbits(a < b ? a : b));
    }
  }
  put_kv("double.add", h_add);
  put_kv("double.mul", h_mul);
  put_kv("double.div", h_div);
  put_kv("double.cmp", h_cmp);
  put_kv("double.misc", h_misc);
}

static void test_float(void) {
  u32 h_arith = 1, h_conv = 2;
  for (u32 i = 0; i < NDV; i++) {
    const float a = (float)dv[i];
    h_conv = mix(h_conv, fbits(a));
    h_conv = mix64(h_conv, dbits((double)a));
    for (u32 j = 0; j < NDV; j++) {
      const float b = (float)dv[j];
      h_arith = mix(h_arith, fbits(a + b));
      h_arith = mix(h_arith, fbits(a - b));
      h_arith = mix(h_arith, fbits(a * b));
      h_arith = mix(h_arith, fbits(a / b));
      h_arith = mix(h_arith, (u32)(a < b) | (u32)(a == b) << 1 | (u32)(a > b) << 2);
      /* Mixed precision: double maths rounded to single at the end. */
      h_arith = mix(h_arith, fbits((float)(dv[i] * dv[j] + 0.25)));
    }
  }
  put_kv("float.arith", h_arith);
  put_kv("float.conv", h_conv);
}

static void test_convert(void) {
  u32 h_to_fp = 1, h_to_int = 2;
  for (u32 i = 0; i < NIV; i++) {
    const s32 v = iv[i];
    h_to_fp = mix64(h_to_fp, dbits((double)v));
    h_to_fp = mix64(h_to_fp, dbits((double)(u32)v));
    h_to_fp = mix(h_to_fp, fbits((float)v));
    h_to_fp = mix(h_to_fp, fbits((float)(u32)v));
    h_to_fp = mix64(h_to_fp, dbits((double)v * 0.5 + (double)(s16)v));
  }
  for (u32 i = 0; i < NDV; i++) {
    const double d = dv[i];
    if (d > -2147483000.0 && d < 2147483000.0) {
      h_to_int = mix(h_to_int, (u32)(s32)d);
      h_to_int = mix(h_to_int, (u32)(s32)(d * 1000.0 > -2e9 && d * 1000.0 < 2e9 ? d * 1000.0 : 0.0));
      h_to_int = mix(h_to_int, (u32)(s32)(float)d);
    }
    if (d >= 0.0 && d < 4294967000.0) h_to_int = mix(h_to_int, (u32)d);
  }
  put_kv("convert.to_fp", h_to_fp);
  put_kv("convert.to_int", h_to_int);
}

/* Longer computations, where an error anywhere shows up in the last bits. */
static NOINLINE double newton_sqrt(double x) {
  double g = x > 1.0 ? x * 0.5 : 1.0;
  for (int i = 0; i < 40; i++) g = 0.5 * (g + x / g);
  return g;
}

static void test_chains(void) {
  u32 h = 1;
  double sum = 0.0, prod = 1.0;
  for (int i = 1; i <= 2000; i++) {
    const double x = (double)i;
    sum += 1.0 / (x * x);
    prod *= 1.0 + 1.0 / (x * 3.0);
    if ((i & 255) == 0) {
      h = mix64(h, dbits(sum));
      h = mix64(h, dbits(prod));
    }
  }
  put_kv64("chain.basel", dbits(sum));
  put_kv64("chain.product", dbits(prod));
  for (u32 i = 0; i < NDV; i++) {
    const double d = __builtin_fabs(dv[i]);
    if (d > 1e-100 && d < 1e100) h = mix64(h, dbits(newton_sqrt(d)));
  }
  put_kv("chain.sqrt", h);

  /* Horner evaluation in single precision. */
  float acc = 0.0f;
  const volatile float step = 0.37f;
  for (int i = 0; i < 300; i++) acc = acc * 0.99f + step * (float)(i % 17) - 0.125f;
  put_kv("chain.horner", fbits(acc));
}

int test_main(int argc, char** argv, char** envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  test_double();
  test_float();
  test_convert();
  test_chains();
  put_str("done\n");
  return 0;
}
