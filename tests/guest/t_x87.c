/* x87 test: 80-bit long double arithmetic and the x87-only instructions.
 *
 * Intel only. The reference is this same source built for 64-bit Intel and
 * run under Rosetta, where long double is also the x87's 80-bit format.
 *
 * Results print as hashes of raw 80-bit patterns, so arithmetic must match to
 * the last bit. The transcendental instructions are the exception: cocaine
 * computes those in double precision, so they are compared after rounding to
 * single precision. */
#include "rt.h"

typedef long double ld;

struct raw80 {
  u64 man;
  u16 se;
};

static struct raw80 raw(ld v) {
  struct raw80 r;
  __builtin_memcpy(&r.man, &v, 8);
  __builtin_memcpy(&r.se, (const char*)&v + 8, 2);
  return r;
}

static u32 mix_ld(u32 h, ld v) {
  const struct raw80 r = raw(v);
  return mix(mix64(h, r.man), r.se);
}

static void put_ld(const char* key, ld v) {
  const struct raw80 r = raw(v);
  put_str(key);
  put_str(" = ");
  put_hex32(r.se);
  put_str(":");
  put_hex64(r.man);
  put_str("\n");
}

static u32 fbits(float f) {
  u32 b;
  __builtin_memcpy(&b, &f, 4);
  return b;
}

static u64 dbits(double d) {
  u64 b;
  __builtin_memcpy(&b, &d, 8);
  return b;
}

static const volatile ld lv[] = {
    0.0L,
    -0.0L,
    1.0L,
    -1.0L,
    0.5L,
    3.0L,
    1.0L / 3.0L,
    -2.0L / 7.0L,
    3.14159265358979323846264338327950288L,
    123456789.125L,
    9223372036854775808.0L,
    18446744073709551615.0L,
    1e4000L,
    -1e4000L,
    1e-4000L,
    1e-4940L,
    3e-4950L,
    1.18973149535723176502e+4932L,
    3.36210314311209350626e-4932L,
    0.1L,
    1e19L,
    16777217.0L,
    1e-30L,
};
#define NLV (sizeof lv / sizeof lv[0])

static ld value(u32 i) {
  if (i < NLV) return lv[i];
  if (i == NLV) return __builtin_infl();
  if (i == NLV + 1) return -__builtin_infl();
  return __builtin_nanl("");
}
#define NVAL (NLV + 3)

/* ---- control word --------------------------------------------------------- */

static NOINLINE void set_cw(u16 cw) { __asm__ volatile("fldcw %0" : : "m"(cw)); }

static NOINLINE u16 get_cw(void) {
  u16 cw;
  __asm__ volatile("fnstcw %0" : "=m"(cw));
  return cw;
}

/* ---- arithmetic ----------------------------------------------------------- */

static void test_arith(void) {
  u32 h_add = 1, h_mul = 2, h_div = 3, h_cmp = 4, h_unary = 5;
  for (u32 i = 0; i < NVAL; i++) {
    const ld a = value(i);
    h_unary = mix_ld(h_unary, -a);
    h_unary = mix_ld(h_unary, __builtin_fabsl(a));
    h_unary = mix_ld(h_unary, __builtin_sqrtl(a));
    for (u32 j = 0; j < NVAL; j++) {
      const ld b = value(j);
      h_add = mix_ld(h_add, a + b);
      h_add = mix_ld(h_add, a - b);
      h_mul = mix_ld(h_mul, a * b);
      h_div = mix_ld(h_div, a / b);
      h_cmp = mix(h_cmp, (u32)(a < b) | (u32)(a <= b) << 1 | (u32)(a == b) << 2 |
                             (u32)(a != b) << 3 | (u32)(a > b) << 4 | (u32)(a >= b) << 5);
    }
  }
  put_kv("x87.add", h_add);
  put_kv("x87.mul", h_mul);
  put_kv("x87.div", h_div);
  put_kv("x87.cmp", h_cmp);
  put_kv("x87.unary", h_unary);
}

/* ---- conversions ------------------------------------------------------------ */

static const volatile s64 iv[] = {
    0,           1,          -1,          255,         -32768,          32767,
    2147483647,  -2147483647 - 1, 4294967295ll, 9007199254740993ll, -9007199254740993ll,
    9223372036854775807ll, -9223372036854775807ll - 1, 1234567890123456789ll, 1000000000000ll,
};
#define NIV (sizeof iv / sizeof iv[0])

static void test_convert(void) {
  u32 h_from = 1, h_to_fp = 2, h_to_int = 3;
  for (u32 i = 0; i < NIV; i++) {
    const s64 v = iv[i];
    h_from = mix_ld(h_from, (ld)v);
    h_from = mix_ld(h_from, (ld)(u64)v);
    h_from = mix_ld(h_from, (ld)(s32)v);
    h_from = mix_ld(h_from, (ld)(u32)v);
    h_from = mix_ld(h_from, (ld)(s16)v);
    h_from = mix_ld(h_from, (ld)(double)v);
    h_from = mix_ld(h_from, (ld)(float)v);
    /* A 64-bit integer must survive a trip through the x87 untouched. */
    h_from = mix64(h_from, (u64)(s64)(ld)v);
  }
  for (u32 i = 0; i < NVAL; i++) {
    const ld a = value(i);
    h_to_fp = mix64(h_to_fp, dbits((double)a));
    h_to_fp = mix(h_to_fp, fbits((float)a));
    const ld mag = __builtin_fabsl(a);
    if (mag < 9e18L) {
      h_to_int = mix64(h_to_int, (u64)(s64)a);
      h_to_int = mix64(h_to_int, (u64)(s64)(a * 1.5L < 9e18L && a * 1.5L > -9e18L ? a * 1.5L : a));
    }
    if (mag < 2e9L) h_to_int = mix(h_to_int, (u32)(s32)a);
    if (mag < 3e4L) h_to_int = mix(h_to_int, (u32)(s32)(s16)a);
    if (a >= 0.0L && a < 1.8e19L) h_to_int = mix64(h_to_int, (u64)a);
    if (a >= 0.0L && a < 4e9L) h_to_int = mix(h_to_int, (u32)a);
  }
  put_kv("x87.from_int", h_from);
  put_kv("x87.to_fp", h_to_fp);
  put_kv("x87.to_int", h_to_int);
}

/* ---- rounding and precision control ----------------------------------------- */

static NOINLINE void round_int(const ld* x, ld* r) {
  __asm__ volatile("fldt %1\nfrndint\nfstpt %0" : "=m"(*r) : "m"(*x));
}
static NOINLINE void store_int32(const ld* x, s32* r) {
  __asm__ volatile("fldt %1\nfistpl %0" : "=m"(*r) : "m"(*x));
}
static NOINLINE void store_int64(const ld* x, s64* r) {
  __asm__ volatile("fldt %1\nfistpll %0" : "=m"(*r) : "m"(*x));
}
static NOINLINE void store_int16(const ld* x, s16* r) {
  __asm__ volatile("fldt %1\nfistps %0" : "=m"(*r) : "m"(*x));
}
static NOINLINE void store_trunc64(const ld* x, s64* r) {
  __asm__ volatile("fldt %1\nfisttpll %0" : "=m"(*r) : "m"(*x));
}
static NOINLINE void store_double(const ld* x, double* r) {
  __asm__ volatile("fldt %1\nfstpl %0" : "=m"(*r) : "m"(*x));
}
static NOINLINE void store_float(const ld* x, float* r) {
  __asm__ volatile("fldt %1\nfstps %0" : "=m"(*r) : "m"(*x));
}

static const volatile ld rv[] = {
    0.5L,  1.5L,   2.5L,  -0.5L,  -1.5L, -2.5L, 0.49999L, 2.50001L, 1e10L + 0.5L, -7.75L,
    1.0L / 3.0L, 1e-4000L, 123456789012345678.5L, -0.0L, 3.999999999999999999L, 1e-30L,
    16777217.0L, 9007199254740993.0L, 33000.5L, -33000.5L, 5e9L, 1e19L,
};
#define NRV (sizeof rv / sizeof rv[0])

static void test_control(void) {
  const u16 saved = get_cw();
  for (u32 rc = 0; rc < 4; rc++) {
    u32 h = 1;
    set_cw((u16)(0x037F | (rc << 10)));
    for (u32 i = 0; i < NRV; i++) {
      const ld a = rv[i];
      ld r;
      s32 i32;
      s64 i64;
      s16 i16;
      double d;
      float f;
      round_int(&a, &r);
      h = mix_ld(h, r);
      store_int32(&a, &i32);
      store_int64(&a, &i64);
      store_int16(&a, &i16);
      h = mix(h, (u32)i32);
      h = mix(h, (u32)(s32)i16);
      store_int64(&a, &i64);
      h = mix64(h, (u64)i64);
      store_trunc64(&a, &i64);
      h = mix64(h, (u64)i64);
      store_double(&a, &d);
      store_float(&a, &f);
      h = mix64(h, dbits(d));
      h = mix(h, fbits(f));
      for (u32 j = 0; j < NRV; j += 3) {
        const ld b = rv[j];
        h = mix_ld(h, a + b);
        h = mix_ld(h, a * b);
        h = mix_ld(h, a / b);
      }
    }
    set_cw(saved);
    put_str("x87.round");
    put_dec((s32)rc);
    put_kv("", h);
  }
  /* Precision control: 24-bit, then 53-bit significands. */
  for (u32 pc = 0; pc < 3; pc += 2) {
    u32 h = 1;
    for (u32 rc = 0; rc < 4; rc++) {
      set_cw((u16)(0x007F | (pc << 8) | (rc << 10)));
      for (u32 i = 0; i < NRV; i++) {
        for (u32 j = 0; j < NRV; j += 2) {
          const ld a = rv[i], b = rv[j];
          h = mix_ld(h, a + b);
          h = mix_ld(h, a - b);
          h = mix_ld(h, a * b);
          h = mix_ld(h, a / b);
        }
        h = mix_ld(h, __builtin_sqrtl(__builtin_fabsl(rv[i])));
      }
    }
    set_cw(saved);
    put_str("x87.precision");
    put_dec((s32)pc);
    put_kv("", h);
  }
}

/* ---- instructions compilers don't emit ---------------------------------------- */

/* st0 = x, st1 = y on entry; the result is st0 and y is discarded. */
#define X87_BINARY(name, insn)                                                 \
  static NOINLINE void name(const ld* x, const ld* y, ld* r) {                 \
    __asm__ volatile("fldt %2\nfldt %1\n" insn "\nfstpt %0\nfstp %%st(0)"      \
                     : "=m"(*r)                                                \
                     : "m"(*x), "m"(*y));                                      \
  }
/* For instructions that pop: the result replaces y. */
#define X87_BINARY_POP(name, insn)                                             \
  static NOINLINE void name(const ld* x, const ld* y, ld* r) {                 \
    __asm__ volatile("fldt %2\nfldt %1\n" insn "\nfstpt %0" : "=m"(*r) : "m"(*x), "m"(*y)); \
  }
#define X87_UNARY(name, insn)                                                  \
  static NOINLINE void name(const ld* x, ld* r) {                              \
    __asm__ volatile("fldt %1\n" insn "\nfstpt %0" : "=m"(*r) : "m"(*x));      \
  }
/* For instructions that push a second result. */
#define X87_UNARY2(name, insn)                                                 \
  static NOINLINE void name(const ld* x, ld* top, ld* below) {                 \
    __asm__ volatile("fldt %2\n" insn "\nfstpt %0\nfstpt %1"                   \
                     : "=m"(*top), "=m"(*below)                                \
                     : "m"(*x));                                               \
  }

X87_BINARY(do_fprem, "fprem")
X87_BINARY(do_fprem1, "fprem1")
X87_BINARY(do_fscale, "fscale")
X87_BINARY_POP(do_fyl2x, "fyl2x")
X87_BINARY_POP(do_fyl2xp1, "fyl2xp1")
X87_BINARY_POP(do_fpatan, "fpatan")
X87_UNARY(do_fsin, "fsin")
X87_UNARY(do_fcos, "fcos")
X87_UNARY(do_f2xm1, "f2xm1")
X87_UNARY2(do_fxtract, "fxtract")
X87_UNARY2(do_fsincos, "fsincos")
X87_UNARY2(do_fptan, "fptan")

static NOINLINE u16 do_fxam(const ld* x) {
  u16 sw;
  __asm__ volatile("fldt %1\nfxam\nfnstsw %0\nfstp %%st(0)" : "=m"(sw) : "m"(*x));
  return sw & 0x4700;
}

static NOINLINE u16 do_fcom(const ld* x, const ld* y) {
  u16 sw;
  __asm__ volatile("fldt %2\nfldt %1\nfcompp\nfnstsw %0" : "=m"(sw) : "m"(*x), "m"(*y));
  return sw & 0x4500;
}

static NOINLINE u16 do_fprem_status(const ld* x, const ld* y) {
  u16 sw;
  ld sink;
  __asm__ volatile("fldt %3\nfldt %2\nfprem\nfnstsw %0\nfstpt %1\nfstp %%st(0)"
                   : "=m"(sw), "=m"(sink)
                   : "m"(*x), "m"(*y));
  return sw & 0x4700;
}

static NOINLINE void do_bcd(const ld* x, u8* bcd, ld* back) {
  __asm__ volatile("fldt %2\nfbstp %0\nfbld %0\nfstpt %1" : "=m"(*(u8(*)[10])bcd), "=m"(*back) : "m"(*x));
}

static NOINLINE void load_constants(ld out[7]) {
  __asm__ volatile(
      "fld1\nfstpt (%0)\nfldl2t\nfstpt 16(%0)\nfldl2e\nfstpt 32(%0)\nfldpi\nfstpt 48(%0)\n"
      "fldlg2\nfstpt 64(%0)\nfldln2\nfstpt 80(%0)\nfldz\nfstpt 96(%0)"
      :
      : "r"(out)
      : "memory");
}

static void test_special(void) {
  u32 h_rem = 1, h_scale = 2, h_xam = 3, h_xtract = 4, h_bcd = 5;
  for (u32 i = 0; i < NVAL; i++) {
    const ld a = value(i);
    ld top, below;
    h_xam = mix(h_xam, do_fxam(&a));
    do_fxtract(&a, &top, &below);
    h_xtract = mix_ld(mix_ld(h_xtract, top), below);
    for (u32 j = 0; j < NVAL; j++) {
      const ld b = value(j);
      ld r;
      h_xam = mix(h_xam, do_fcom(&a, &b));
      /* When the operands' exponents are 64 or more apart, a real x87 stops
       * part-way and how far it gets is up to the CPU. Skip those pairs. */
      const int ea = raw(a).se & 0x7FFF, eb = raw(b).se & 0x7FFF;
      if (ea != 0x7FFF && eb != 0x7FFF && raw(b).man != 0 && ea - eb >= 60) continue;
      do_fprem(&a, &b, &r);
      h_rem = mix_ld(h_rem, r);
      do_fprem1(&a, &b, &r);
      h_rem = mix_ld(h_rem, r);
    }
  }
  /* Remainders with modest exponent gaps, where the quotient bits matter. */
  static const volatile ld num[] = {17.0L, -17.0L, 1e6L, 123456.789L, 5.5L, 1e18L, 7.0L, 100.0L};
  static const volatile ld den[] = {5.0L, 3.0L, 0.7L, -4.0L, 2.0L, 1.0L / 3.0L, 7.0L, 8.0L};
  for (u32 i = 0; i < 8; i++) {
    for (u32 j = 0; j < 8; j++) {
      const ld a = num[i], b = den[j];
      ld r;
      do_fprem(&a, &b, &r);
      h_rem = mix_ld(h_rem, r);
      do_fprem1(&a, &b, &r);
      h_rem = mix_ld(h_rem, r);
      h_rem = mix(h_rem, do_fprem_status(&a, &b));
    }
  }
  static const volatile ld factor[] = {0.0L, 1.0L, -1.0L, 10.7L, -10.7L, 16000.0L, -16500.0L, 40000.0L, -40000.0L};
  for (u32 i = 0; i < NVAL; i++) {
    for (u32 j = 0; j < 9; j++) {
      const ld a = value(i), b = factor[j];
      ld r;
      do_fscale(&a, &b, &r);
      h_scale = mix_ld(h_scale, r);
    }
  }
  static const volatile ld bcd_in[] = {0.0L, 1.0L, -1.0L, 123456789.0L, -987654321098765432.0L,
                                       999999999999999999.0L, 12.5L, 13.5L, -0.4L};
  for (u32 i = 0; i < 9; i++) {
    const ld a = bcd_in[i];
    u8 bcd[10];
    ld back;
    do_bcd(&a, bcd, &back);
    for (u32 k = 0; k < 10; k++) h_bcd = mix(h_bcd, bcd[k]);
    h_bcd = mix_ld(h_bcd, back);
  }
  put_kv("x87.rem", h_rem);
  put_kv("x87.scale", h_scale);
  put_kv("x87.xam", h_xam);
  put_kv("x87.xtract", h_xtract);
  put_kv("x87.bcd", h_bcd);

  ld constants[7];
  load_constants(constants);
  put_ld("x87.one", constants[0]);
  put_ld("x87.l2t", constants[1]);
  put_ld("x87.l2e", constants[2]);
  put_ld("x87.pi", constants[3]);
  put_ld("x87.lg2", constants[4]);
  put_ld("x87.ln2", constants[5]);
  put_ld("x87.zero", constants[6]);
}

static void test_transcendental(void) {
  static const volatile ld in[] = {0.5L, 1.25L, -0.3L, 2.0L, 0.001L, 3.0L, -1.7L, 0.75L};
  u32 h = 1;
  for (u32 i = 0; i < 8; i++) {
    const ld x = in[i];
    const ld small = x * 0.25L; /* f2xm1 wants -1..1 */
    const ld pos = __builtin_fabsl(x) + 0.5L;
    const ld three = 3.0L;
    ld r, r2;
    do_fsin(&x, &r);
    h = mix(h, fbits((float)r));
    do_fcos(&x, &r);
    h = mix(h, fbits((float)r));
    do_fsincos(&x, &r, &r2);
    h = mix(mix(h, fbits((float)r)), fbits((float)r2));
    do_fptan(&x, &r, &r2);
    h = mix(mix(h, fbits((float)r)), fbits((float)r2));
    do_f2xm1(&small, &r);
    h = mix(h, fbits((float)r));
    do_fyl2x(&pos, &three, &r);
    h = mix(h, fbits((float)r));
    do_fyl2xp1(&small, &three, &r);
    h = mix(h, fbits((float)r));
    do_fpatan(&pos, &x, &r);
    h = mix(h, fbits((float)r));
  }
  put_kv("x87.transcendental", h);
}

int test_main(int argc, char** argv, char** envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  test_arith();
  test_convert();
  test_control();
  test_special();
  test_transcendental();
  put_str("done\n");
  return 0;
}
