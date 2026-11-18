/* AltiVec test: one check per vector instruction.
 *
 * No current Mac has AltiVec, so there is nothing to run for a reference.
 * Instead every operation is written twice: once with the real AltiVec
 * intrinsic (the PowerPC build) and once as a plain C model of what the
 * instruction is documented to do (the native build). Both print a hash per
 * operation over the same inputs, so the usual output comparison applies.
 *
 * A vector is sixteen bytes in memory order, which on PowerPC is also element
 * order, so the model addresses elements through big-endian accessors. */
#include "rt.h"

#ifdef __ALTIVEC__
#include <altivec.h>
#endif

typedef struct {
  u8 b[16];
} __attribute__((aligned(16))) vec;

static const vec zero_vec;

/* ---- the PowerPC side ------------------------------------------------------- */
#ifdef __ALTIVEC__

#define UC(v) ((vector unsigned char)(v))
#define SC(v) ((vector signed char)(v))
#define US(v) ((vector unsigned short)(v))
#define SS(v) ((vector signed short)(v))
#define UI(v) ((vector unsigned int)(v))
#define SI(v) ((vector signed int)(v))
#define FL(v) ((vector float)(v))

static inline vec mk(vector unsigned char v) {
  vec r;
  vec_st(v, 0, r.b);
  return r;
}

#define OP1(name, vexpr, model)                         \
  static NOINLINE vec name(vec x, vec y) {              \
    const vector unsigned char a = vec_ld(0, x.b);      \
    (void)y;                                            \
    return mk(UC(vexpr));                               \
  }
#define OP2(name, vexpr, model)                         \
  static NOINLINE vec name(vec x, vec y) {              \
    const vector unsigned char a = vec_ld(0, x.b);      \
    const vector unsigned char b = vec_ld(0, y.b);      \
    return mk(UC(vexpr));                               \
  }
#define OP3(name, vexpr, model)                         \
  static NOINLINE vec name(vec x, vec y, vec z) {       \
    const vector unsigned char a = vec_ld(0, x.b);      \
    const vector unsigned char b = vec_ld(0, y.b);      \
    const vector unsigned char c = vec_ld(0, z.b);      \
    return mk(UC(vexpr));                               \
  }
#define PRED(name, vexpr, model)                        \
  static NOINLINE u32 name(vec x, vec y) {              \
    const vector unsigned char a = vec_ld(0, x.b);      \
    const vector unsigned char b = vec_ld(0, y.b);      \
    return (u32)(vexpr);                                \
  }

static NOINLINE void clear_sat(void) { vec_mtvscr(vec_splat_u8(0)); }
static NOINLINE u32 read_sat(void) {
  const vec r = mk(UC(vec_mfvscr()));
  return r.b[15] & 1;
}

/* ---- the model side ---------------------------------------------------------- */
#else

static int model_sat;
static void clear_sat(void) { model_sat = 0; }
static u32 read_sat(void) { return (u32)model_sat; }

static u32 gb(const vec* v, int i) { return v->b[i]; }
static s32 gsb(const vec* v, int i) { return (s8)v->b[i]; }
static u32 gh(const vec* v, int i) { return (u32)v->b[2 * i] << 8 | v->b[2 * i + 1]; }
static s32 gsh(const vec* v, int i) { return (s16)gh(v, i); }
static u32 gw(const vec* v, int i) {
  return (u32)v->b[4 * i] << 24 | (u32)v->b[4 * i + 1] << 16 | (u32)v->b[4 * i + 2] << 8 |
         v->b[4 * i + 3];
}
static s32 gsw(const vec* v, int i) { return (s32)gw(v, i); }
static float gf(const vec* v, int i) {
  const u32 w = gw(v, i);
  float f;
  __builtin_memcpy(&f, &w, 4);
  return f;
}
static void pb(vec* v, int i, u32 x) { v->b[i] = (u8)x; }
static void ph(vec* v, int i, u32 x) {
  v->b[2 * i] = (u8)(x >> 8);
  v->b[2 * i + 1] = (u8)x;
}
static void pw(vec* v, int i, u32 x) {
  v->b[4 * i] = (u8)(x >> 24);
  v->b[4 * i + 1] = (u8)(x >> 16);
  v->b[4 * i + 2] = (u8)(x >> 8);
  v->b[4 * i + 3] = (u8)x;
}
static void pf(vec* v, int i, float f) {
  u32 w;
  __builtin_memcpy(&w, &f, 4);
  pw(v, i, w);
}

static u32 su8(s64 v) { return v < 0 ? (model_sat = 1, 0) : v > 255 ? (model_sat = 1, 255) : (u32)v; }
static u32 ss8(s64 v) { return v < -128 ? (model_sat = 1, (u32)-128) : v > 127 ? (model_sat = 1, 127) : (u32)v; }
static u32 su16(s64 v) { return v < 0 ? (model_sat = 1, 0) : v > 65535 ? (model_sat = 1, 65535) : (u32)v; }
static u32 ss16(s64 v) { return v < -32768 ? (model_sat = 1, (u32)-32768) : v > 32767 ? (model_sat = 1, 32767) : (u32)v; }
static u32 su32(s64 v) { return v < 0 ? (model_sat = 1, 0) : v > 0xFFFFFFFFll ? (model_sat = 1, 0xFFFFFFFFu) : (u32)v; }
static u32 ss32(s64 v) {
  return v < -2147483648ll ? (model_sat = 1, 0x80000000u) : v > 2147483647ll ? (model_sat = 1, 0x7FFFFFFFu) : (u32)v;
}
static u32 rol(u32 v, u32 n, u32 bits) {
  const u32 mask = bits == 32 ? 0xFFFFFFFFu : (1u << bits) - 1;
  return n ? ((v << n) | (v >> (bits - n))) & mask : v;
}
static float fmax_m(float a, float b) {
  if (a != a) return a;
  if (b != b) return b;
  if (a > b) return a;
  if (b > a) return b;
  return __builtin_signbit(a) ? b : a;
}
static float fmin_m(float a, float b) {
  if (a != a) return a;
  if (b != b) return b;
  if (a < b) return a;
  if (b < a) return b;
  return __builtin_signbit(a) ? a : b;
}
/* Negates everything but a NaN. Kept out of line and done on the raw bits:
 * written as "-fma(...)", the ARM compiler folds the negation into a
 * multiply-add instruction that returns +0 where the right answer is -0. */
static NOINLINE float neg_m(float r) {
  u32 w;
  __builtin_memcpy(&w, &r, 4);
  if ((w & 0x7F800000u) == 0x7F800000u && (w & 0x007FFFFFu)) return r;
  w ^= 0x80000000u;
  __builtin_memcpy(&r, &w, 4);
  return r;
}
static u32 cts_m(float f, int scale) {
  const double x = (double)f * (double)(1u << scale);
  if (x != x) return 0; /* a NaN converts to zero without saturating */
  if (x >= 2147483648.0) return (model_sat = 1, 0x7FFFFFFFu);
  if (x <= -2147483649.0) return (model_sat = 1, 0x80000000u);
  return (u32)(s32)x;
}
static u32 ctu_m(float f, int scale) {
  const double x = (double)f * (double)(1u << scale);
  if (x != x) return 0;
  if (x >= 4294967296.0) return (model_sat = 1, 0xFFFFFFFFu);
  if (x <= -1.0) return (model_sat = 1, 0);
  return (u32)x;
}

#define FOR(n) for (int i = 0; i < (n); i++)
#define X (&x)
#define Y (&y)
#define Z (&z)
#define R (&r)

#define OP1(name, vexpr, model)            \
  static NOINLINE vec name(vec x, vec y) { \
    vec r = zero_vec;                      \
    (void)y;                               \
    model;                                 \
    return r;                              \
  }
#define OP2(name, vexpr, model)            \
  static NOINLINE vec name(vec x, vec y) { \
    vec r = zero_vec;                      \
    model;                                 \
    return r;                              \
  }
#define OP3(name, vexpr, model)                   \
  static NOINLINE vec name(vec x, vec y, vec z) { \
    vec r = zero_vec;                             \
    model;                                        \
    return r;                                     \
  }
#define PRED(name, vexpr, model)           \
  static NOINLINE u32 name(vec x, vec y) { \
    u32 all = 1, any = 0;                  \
    (void)all;                             \
    (void)any;                             \
    model;                                 \
    return 0;                              \
  }
#define TALLY(cond) \
  do {              \
    if (cond) {     \
      any = 1;      \
    } else {        \
      all = 0;      \
    }               \
  } while (0)

#endif

/* ---- the operations ---------------------------------------------------------- */

OP2(addubm, vec_add(UC(a), UC(b)), FOR(16) pb(R, i, gb(X, i) + gb(Y, i)))
OP2(adduhm, vec_add(US(a), US(b)), FOR(8) ph(R, i, gh(X, i) + gh(Y, i)))
OP2(adduwm, vec_add(UI(a), UI(b)), FOR(4) pw(R, i, gw(X, i) + gw(Y, i)))
OP2(addcuw, vec_addc(UI(a), UI(b)), FOR(4) pw(R, i, (u32)(((u64)gw(X, i) + gw(Y, i)) >> 32)))
OP2(addubs, vec_adds(UC(a), UC(b)), FOR(16) pb(R, i, su8((s64)gb(X, i) + gb(Y, i))))
OP2(adduhs, vec_adds(US(a), US(b)), FOR(8) ph(R, i, su16((s64)gh(X, i) + gh(Y, i))))
OP2(adduws, vec_adds(UI(a), UI(b)), FOR(4) pw(R, i, su32((s64)gw(X, i) + gw(Y, i))))
OP2(addsbs, vec_adds(SC(a), SC(b)), FOR(16) pb(R, i, ss8((s64)gsb(X, i) + gsb(Y, i))))
OP2(addshs, vec_adds(SS(a), SS(b)), FOR(8) ph(R, i, ss16((s64)gsh(X, i) + gsh(Y, i))))
OP2(addsws, vec_adds(SI(a), SI(b)), FOR(4) pw(R, i, ss32((s64)gsw(X, i) + gsw(Y, i))))

OP2(sububm, vec_sub(UC(a), UC(b)), FOR(16) pb(R, i, gb(X, i) - gb(Y, i)))
OP2(subuhm, vec_sub(US(a), US(b)), FOR(8) ph(R, i, gh(X, i) - gh(Y, i)))
OP2(subuwm, vec_sub(UI(a), UI(b)), FOR(4) pw(R, i, gw(X, i) - gw(Y, i)))
OP2(subcuw, vec_subc(UI(a), UI(b)), FOR(4) pw(R, i, gw(X, i) >= gw(Y, i)))
OP2(sububs, vec_subs(UC(a), UC(b)), FOR(16) pb(R, i, su8((s64)gb(X, i) - gb(Y, i))))
OP2(subuhs, vec_subs(US(a), US(b)), FOR(8) ph(R, i, su16((s64)gh(X, i) - gh(Y, i))))
OP2(subuws, vec_subs(UI(a), UI(b)), FOR(4) pw(R, i, su32((s64)gw(X, i) - gw(Y, i))))
OP2(subsbs, vec_subs(SC(a), SC(b)), FOR(16) pb(R, i, ss8((s64)gsb(X, i) - gsb(Y, i))))
OP2(subshs, vec_subs(SS(a), SS(b)), FOR(8) ph(R, i, ss16((s64)gsh(X, i) - gsh(Y, i))))
OP2(subsws, vec_subs(SI(a), SI(b)), FOR(4) pw(R, i, ss32((s64)gsw(X, i) - gsw(Y, i))))

OP2(maxub, vec_max(UC(a), UC(b)), FOR(16) pb(R, i, gb(X, i) > gb(Y, i) ? gb(X, i) : gb(Y, i)))
OP2(maxuh, vec_max(US(a), US(b)), FOR(8) ph(R, i, gh(X, i) > gh(Y, i) ? gh(X, i) : gh(Y, i)))
OP2(maxuw, vec_max(UI(a), UI(b)), FOR(4) pw(R, i, gw(X, i) > gw(Y, i) ? gw(X, i) : gw(Y, i)))
OP2(maxsb, vec_max(SC(a), SC(b)), FOR(16) pb(R, i, (u32)(gsb(X, i) > gsb(Y, i) ? gsb(X, i) : gsb(Y, i))))
OP2(maxsh, vec_max(SS(a), SS(b)), FOR(8) ph(R, i, (u32)(gsh(X, i) > gsh(Y, i) ? gsh(X, i) : gsh(Y, i))))
OP2(maxsw, vec_max(SI(a), SI(b)), FOR(4) pw(R, i, (u32)(gsw(X, i) > gsw(Y, i) ? gsw(X, i) : gsw(Y, i))))
OP2(minub, vec_min(UC(a), UC(b)), FOR(16) pb(R, i, gb(X, i) < gb(Y, i) ? gb(X, i) : gb(Y, i)))
OP2(minuh, vec_min(US(a), US(b)), FOR(8) ph(R, i, gh(X, i) < gh(Y, i) ? gh(X, i) : gh(Y, i)))
OP2(minuw, vec_min(UI(a), UI(b)), FOR(4) pw(R, i, gw(X, i) < gw(Y, i) ? gw(X, i) : gw(Y, i)))
OP2(minsb, vec_min(SC(a), SC(b)), FOR(16) pb(R, i, (u32)(gsb(X, i) < gsb(Y, i) ? gsb(X, i) : gsb(Y, i))))
OP2(minsh, vec_min(SS(a), SS(b)), FOR(8) ph(R, i, (u32)(gsh(X, i) < gsh(Y, i) ? gsh(X, i) : gsh(Y, i))))
OP2(minsw, vec_min(SI(a), SI(b)), FOR(4) pw(R, i, (u32)(gsw(X, i) < gsw(Y, i) ? gsw(X, i) : gsw(Y, i))))

OP2(avgub, vec_avg(UC(a), UC(b)), FOR(16) pb(R, i, (gb(X, i) + gb(Y, i) + 1) >> 1))
OP2(avguh, vec_avg(US(a), US(b)), FOR(8) ph(R, i, (gh(X, i) + gh(Y, i) + 1) >> 1))
OP2(avguw, vec_avg(UI(a), UI(b)), FOR(4) pw(R, i, (u32)(((u64)gw(X, i) + gw(Y, i) + 1) >> 1)))
OP2(avgsb, vec_avg(SC(a), SC(b)), FOR(16) pb(R, i, (u32)((gsb(X, i) + gsb(Y, i) + 1) >> 1)))
OP2(avgsh, vec_avg(SS(a), SS(b)), FOR(8) ph(R, i, (u32)((gsh(X, i) + gsh(Y, i) + 1) >> 1)))
OP2(avgsw, vec_avg(SI(a), SI(b)), FOR(4) pw(R, i, (u32)(((s64)gsw(X, i) + gsw(Y, i) + 1) >> 1)))

OP2(rlb, vec_rl(UC(a), UC(b)), FOR(16) pb(R, i, rol(gb(X, i), gb(Y, i) & 7, 8)))
OP2(rlh, vec_rl(US(a), US(b)), FOR(8) ph(R, i, rol(gh(X, i), gh(Y, i) & 15, 16)))
OP2(rlw, vec_rl(UI(a), UI(b)), FOR(4) pw(R, i, rol(gw(X, i), gw(Y, i) & 31, 32)))
OP2(slb, vec_sl(UC(a), UC(b)), FOR(16) pb(R, i, gb(X, i) << (gb(Y, i) & 7)))
OP2(slh, vec_sl(US(a), US(b)), FOR(8) ph(R, i, gh(X, i) << (gh(Y, i) & 15)))
OP2(slw, vec_sl(UI(a), UI(b)), FOR(4) pw(R, i, gw(X, i) << (gw(Y, i) & 31)))
OP2(srb, vec_sr(UC(a), UC(b)), FOR(16) pb(R, i, gb(X, i) >> (gb(Y, i) & 7)))
OP2(srh, vec_sr(US(a), US(b)), FOR(8) ph(R, i, gh(X, i) >> (gh(Y, i) & 15)))
OP2(srw, vec_sr(UI(a), UI(b)), FOR(4) pw(R, i, gw(X, i) >> (gw(Y, i) & 31)))
OP2(srab, vec_sra(SC(a), UC(b)), FOR(16) pb(R, i, (u32)(gsb(X, i) >> (gb(Y, i) & 7))))
OP2(srah, vec_sra(SS(a), US(b)), FOR(8) ph(R, i, (u32)(gsh(X, i) >> (gh(Y, i) & 15))))
OP2(sraw, vec_sra(SI(a), UI(b)), FOR(4) pw(R, i, (u32)(gsw(X, i) >> (gw(Y, i) & 31))))

/* Whole-register shifts. vsl and vsr want the same count in every byte. */
OP2(sll, vec_sll(UC(a), vec_splat(UC(b), 15)), {
  const u32 sh = gb(Y, 15) & 7;
  FOR(16) pb(R, i, (gb(X, i) << sh) | (i < 15 ? gb(X, i + 1) >> (8 - sh) : 0));
})
OP2(srl, vec_srl(UC(a), vec_splat(UC(b), 15)), {
  const u32 sh = gb(Y, 15) & 7;
  FOR(16) pb(R, i, (gb(X, i) >> sh) | (i > 0 ? gb(X, i - 1) << (8 - sh) : 0));
})
OP2(slo, vec_slo(UC(a), UC(b)), {
  const int sh = (int)((gb(Y, 15) >> 3) & 15);
  FOR(16) pb(R, i, i + sh < 16 ? gb(X, i + sh) : 0);
})
OP2(sro, vec_sro(UC(a), UC(b)), {
  const int sh = (int)((gb(Y, 15) >> 3) & 15);
  FOR(16) pb(R, i, i >= sh ? gb(X, i - sh) : 0);
})
OP2(sld3, vec_sld(UC(a), UC(b), 3), FOR(16) pb(R, i, i + 3 < 16 ? gb(X, i + 3) : gb(Y, i + 3 - 16)))
OP2(sld11, vec_sld(UC(a), UC(b), 11), FOR(16) pb(R, i, i + 11 < 16 ? gb(X, i + 11) : gb(Y, i + 11 - 16)))

OP2(and_, vec_and(UC(a), UC(b)), FOR(16) pb(R, i, gb(X, i) & gb(Y, i)))
OP2(andc, vec_andc(UC(a), UC(b)), FOR(16) pb(R, i, gb(X, i) & ~gb(Y, i)))
OP2(or_, vec_or(UC(a), UC(b)), FOR(16) pb(R, i, gb(X, i) | gb(Y, i)))
OP2(nor, vec_nor(UC(a), UC(b)), FOR(16) pb(R, i, ~(gb(X, i) | gb(Y, i))))
OP2(xor_, vec_xor(UC(a), UC(b)), FOR(16) pb(R, i, gb(X, i) ^ gb(Y, i)))

OP2(muleub, vec_mule(UC(a), UC(b)), FOR(8) ph(R, i, gb(X, 2 * i) * gb(Y, 2 * i)))
OP2(muloub, vec_mulo(UC(a), UC(b)), FOR(8) ph(R, i, gb(X, 2 * i + 1) * gb(Y, 2 * i + 1)))
OP2(mulesb, vec_mule(SC(a), SC(b)), FOR(8) ph(R, i, (u32)(gsb(X, 2 * i) * gsb(Y, 2 * i))))
OP2(mulosb, vec_mulo(SC(a), SC(b)), FOR(8) ph(R, i, (u32)(gsb(X, 2 * i + 1) * gsb(Y, 2 * i + 1))))
OP2(muleuh, vec_mule(US(a), US(b)), FOR(4) pw(R, i, gh(X, 2 * i) * gh(Y, 2 * i)))
OP2(mulouh, vec_mulo(US(a), US(b)), FOR(4) pw(R, i, gh(X, 2 * i + 1) * gh(Y, 2 * i + 1)))
OP2(mulesh, vec_mule(SS(a), SS(b)), FOR(4) pw(R, i, (u32)(gsh(X, 2 * i) * gsh(Y, 2 * i))))
OP2(mulosh, vec_mulo(SS(a), SS(b)), FOR(4) pw(R, i, (u32)(gsh(X, 2 * i + 1) * gsh(Y, 2 * i + 1))))

OP2(sum4ubs, vec_sum4s(UC(a), UI(b)), FOR(4) {
  s64 sum = gw(Y, i);
  for (int k = 0; k < 4; k++) sum += gb(X, 4 * i + k);
  pw(R, i, su32(sum));
})
OP2(sum4sbs, vec_sum4s(SC(a), SI(b)), FOR(4) {
  s64 sum = gsw(Y, i);
  for (int k = 0; k < 4; k++) sum += gsb(X, 4 * i + k);
  pw(R, i, ss32(sum));
})
OP2(sum4shs, vec_sum4s(SS(a), SI(b)),
    FOR(4) pw(R, i, ss32((s64)gsw(Y, i) + gsh(X, 2 * i) + gsh(X, 2 * i + 1))))
OP2(sum2sws, vec_sum2s(SI(a), SI(b)), {
  pw(R, 1, ss32((s64)gsw(X, 0) + gsw(X, 1) + gsw(Y, 1)));
  pw(R, 3, ss32((s64)gsw(X, 2) + gsw(X, 3) + gsw(Y, 3)));
})
OP2(sumsws, vec_sums(SI(a), SI(b)),
    pw(R, 3, ss32((s64)gsw(X, 0) + gsw(X, 1) + gsw(X, 2) + gsw(X, 3) + gsw(Y, 3))))

OP2(mrghb, vec_mergeh(UC(a), UC(b)), FOR(8) { pb(R, 2 * i, gb(X, i)); pb(R, 2 * i + 1, gb(Y, i)); })
OP2(mrglb, vec_mergel(UC(a), UC(b)), FOR(8) { pb(R, 2 * i, gb(X, 8 + i)); pb(R, 2 * i + 1, gb(Y, 8 + i)); })
OP2(mrghh, vec_mergeh(US(a), US(b)), FOR(4) { ph(R, 2 * i, gh(X, i)); ph(R, 2 * i + 1, gh(Y, i)); })
OP2(mrglh, vec_mergel(US(a), US(b)), FOR(4) { ph(R, 2 * i, gh(X, 4 + i)); ph(R, 2 * i + 1, gh(Y, 4 + i)); })
OP2(mrghw, vec_mergeh(UI(a), UI(b)), FOR(2) { pw(R, 2 * i, gw(X, i)); pw(R, 2 * i + 1, gw(Y, i)); })
OP2(mrglw, vec_mergel(UI(a), UI(b)), FOR(2) { pw(R, 2 * i, gw(X, 2 + i)); pw(R, 2 * i + 1, gw(Y, 2 + i)); })

OP2(pkuhum, vec_pack(US(a), US(b)), FOR(8) { pb(R, i, gh(X, i)); pb(R, 8 + i, gh(Y, i)); })
OP2(pkuwum, vec_pack(UI(a), UI(b)), FOR(4) { ph(R, i, gw(X, i)); ph(R, 4 + i, gw(Y, i)); })
OP2(pkuhus, vec_packs(US(a), US(b)), FOR(8) { pb(R, i, su8(gh(X, i))); pb(R, 8 + i, su8(gh(Y, i))); })
OP2(pkuwus, vec_packs(UI(a), UI(b)), FOR(4) { ph(R, i, su16(gw(X, i))); ph(R, 4 + i, su16(gw(Y, i))); })
OP2(pkshss, vec_packs(SS(a), SS(b)), FOR(8) { pb(R, i, ss8(gsh(X, i))); pb(R, 8 + i, ss8(gsh(Y, i))); })
OP2(pkswss, vec_packs(SI(a), SI(b)), FOR(4) { ph(R, i, ss16(gsw(X, i))); ph(R, 4 + i, ss16(gsw(Y, i))); })
OP2(pkshus, vec_packsu(SS(a), SS(b)), FOR(8) { pb(R, i, su8(gsh(X, i))); pb(R, 8 + i, su8(gsh(Y, i))); })
OP2(pkswus, vec_packsu(SI(a), SI(b)), FOR(4) { ph(R, i, su16(gsw(X, i))); ph(R, 4 + i, su16(gsw(Y, i))); })
OP2(pkpx, vec_packpx(UI(a), UI(b)), FOR(8) {
  const u32 p = i < 4 ? gw(X, i) : gw(Y, i - 4);
  ph(R, i, (((p >> 24) & 1) << 15) | (((p >> 19) & 0x1F) << 10) | (((p >> 11) & 0x1F) << 5) |
               ((p >> 3) & 0x1F));
})

OP1(upkhsb, vec_unpackh(SC(a)), FOR(8) ph(R, i, (u32)gsb(X, i)))
OP1(upklsb, vec_unpackl(SC(a)), FOR(8) ph(R, i, (u32)gsb(X, 8 + i)))
OP1(upkhsh, vec_unpackh(SS(a)), FOR(4) pw(R, i, (u32)gsh(X, i)))
OP1(upklsh, vec_unpackl(SS(a)), FOR(4) pw(R, i, (u32)gsh(X, 4 + i)))
OP1(upkhpx, vec_unpackh((vector pixel)(a)), FOR(4) {
  const u32 p = gh(X, i);
  pw(R, i, ((p & 0x8000) ? 0xFF000000u : 0) | (((p >> 10) & 0x1F) << 16) | (((p >> 5) & 0x1F) << 8) | (p & 0x1F));
})
OP1(upklpx, vec_unpackl((vector pixel)(a)), FOR(4) {
  const u32 p = gh(X, 4 + i);
  pw(R, i, ((p & 0x8000) ? 0xFF000000u : 0) | (((p >> 10) & 0x1F) << 16) | (((p >> 5) & 0x1F) << 8) | (p & 0x1F));
})

OP1(spltb5, vec_splat(UC(a), 5), FOR(16) pb(R, i, gb(X, 5)))
OP1(splth6, vec_splat(US(a), 6), FOR(8) ph(R, i, gh(X, 6)))
OP1(spltw1, vec_splat(UI(a), 1), FOR(4) pw(R, i, gw(X, 1)))
OP1(spltisb, vec_add(SC(a), vec_splat_s8(-7)), FOR(16) pb(R, i, gb(X, i) - 7))
OP1(spltish, vec_add(SS(a), vec_splat_s16(11)), FOR(8) ph(R, i, gh(X, i) + 11))
OP1(spltisw, vec_add(SI(a), vec_splat_s32(-16)), FOR(4) pw(R, i, gw(X, i) - 16))

OP2(cmpequb, vec_cmpeq(UC(a), UC(b)), FOR(16) pb(R, i, gb(X, i) == gb(Y, i) ? 0xFF : 0))
OP2(cmpequh, vec_cmpeq(US(a), US(b)), FOR(8) ph(R, i, gh(X, i) == gh(Y, i) ? 0xFFFF : 0))
OP2(cmpequw, vec_cmpeq(UI(a), UI(b)), FOR(4) pw(R, i, gw(X, i) == gw(Y, i) ? 0xFFFFFFFFu : 0))
OP2(cmpgtub, vec_cmpgt(UC(a), UC(b)), FOR(16) pb(R, i, gb(X, i) > gb(Y, i) ? 0xFF : 0))
OP2(cmpgtuh, vec_cmpgt(US(a), US(b)), FOR(8) ph(R, i, gh(X, i) > gh(Y, i) ? 0xFFFF : 0))
OP2(cmpgtuw, vec_cmpgt(UI(a), UI(b)), FOR(4) pw(R, i, gw(X, i) > gw(Y, i) ? 0xFFFFFFFFu : 0))
OP2(cmpgtsb, vec_cmpgt(SC(a), SC(b)), FOR(16) pb(R, i, gsb(X, i) > gsb(Y, i) ? 0xFF : 0))
OP2(cmpgtsh, vec_cmpgt(SS(a), SS(b)), FOR(8) ph(R, i, gsh(X, i) > gsh(Y, i) ? 0xFFFF : 0))
OP2(cmpgtsw, vec_cmpgt(SI(a), SI(b)), FOR(4) pw(R, i, gsw(X, i) > gsw(Y, i) ? 0xFFFFFFFFu : 0))

/* Floating point. */
OP2(addfp, vec_add(FL(a), FL(b)), FOR(4) pf(R, i, gf(X, i) + gf(Y, i)))
OP2(subfp, vec_sub(FL(a), FL(b)), FOR(4) pf(R, i, gf(X, i) - gf(Y, i)))
OP2(maxfp, vec_max(FL(a), FL(b)), FOR(4) pf(R, i, fmax_m(gf(X, i), gf(Y, i))))
OP2(minfp, vec_min(FL(a), FL(b)), FOR(4) pf(R, i, fmin_m(gf(X, i), gf(Y, i))))
OP2(cmpeqfp, vec_cmpeq(FL(a), FL(b)), FOR(4) pw(R, i, gf(X, i) == gf(Y, i) ? 0xFFFFFFFFu : 0))
OP2(cmpgefp, vec_cmpge(FL(a), FL(b)), FOR(4) pw(R, i, gf(X, i) >= gf(Y, i) ? 0xFFFFFFFFu : 0))
OP2(cmpgtfp, vec_cmpgt(FL(a), FL(b)), FOR(4) pw(R, i, gf(X, i) > gf(Y, i) ? 0xFFFFFFFFu : 0))
OP2(cmpbfp, vec_cmpb(FL(a), FL(b)), FOR(4) pw(R, i, (gf(X, i) <= gf(Y, i) ? 0 : 0x80000000u) |
                                                         (gf(X, i) >= -gf(Y, i) ? 0 : 0x40000000u)))
OP1(rfin, vec_round(FL(a)), FOR(4) pf(R, i, __builtin_nearbyintf(gf(X, i))))
OP1(rfiz, vec_trunc(FL(a)), FOR(4) pf(R, i, __builtin_truncf(gf(X, i))))
OP1(rfip, vec_ceil(FL(a)), FOR(4) pf(R, i, __builtin_ceilf(gf(X, i))))
OP1(rfim, vec_floor(FL(a)), FOR(4) pf(R, i, __builtin_floorf(gf(X, i))))
OP1(cfsx3, vec_ctf(SI(a), 3), FOR(4) pf(R, i, (float)((double)gsw(X, i) / 8.0)))
OP1(cfux7, vec_ctf(UI(a), 7), FOR(4) pf(R, i, (float)((double)gw(X, i) / 128.0)))
OP1(ctsxs2, vec_cts(FL(a), 2), FOR(4) pw(R, i, cts_m(gf(X, i), 2)))
OP1(ctuxs5, vec_ctu(FL(a), 5), FOR(4) pw(R, i, ctu_m(gf(X, i), 5)))

/* Four-operand forms. */
OP3(sel, vec_sel(UC(a), UC(b), UC(c)), FOR(16) pb(R, i, (gb(X, i) & ~gb(Z, i)) | (gb(Y, i) & gb(Z, i))))
OP3(perm, vec_perm(UC(a), UC(b), UC(c)), FOR(16) {
  const u32 k = gb(Z, i) & 31;
  pb(R, i, k < 16 ? gb(X, (int)k) : gb(Y, (int)k - 16));
})
OP3(maddfp, vec_madd(FL(a), FL(b), FL(c)), FOR(4) pf(R, i, __builtin_fmaf(gf(X, i), gf(Y, i), gf(Z, i))))
OP3(nmsubfp, vec_nmsub(FL(a), FL(b), FL(c)),
    FOR(4) pf(R, i, neg_m(__builtin_fmaf(gf(X, i), gf(Y, i), -gf(Z, i)))))
OP3(mhaddshs, vec_madds(SS(a), SS(b), SS(c)),
    FOR(8) ph(R, i, ss16(((gsh(X, i) * gsh(Y, i)) >> 15) + gsh(Z, i))))
OP3(mhraddshs, vec_mradds(SS(a), SS(b), SS(c)),
    FOR(8) ph(R, i, ss16(((gsh(X, i) * gsh(Y, i) + 0x4000) >> 15) + gsh(Z, i))))
OP3(mladduhm, vec_mladd(US(a), US(b), US(c)), FOR(8) ph(R, i, gh(X, i) * gh(Y, i) + gh(Z, i)))
OP3(msumubm, vec_msum(UC(a), UC(b), UI(c)), FOR(4) {
  u32 sum = gw(Z, i);
  for (int k = 0; k < 4; k++) sum += gb(X, 4 * i + k) * gb(Y, 4 * i + k);
  pw(R, i, sum);
})
OP3(msummbm, vec_msum(SC(a), UC(b), SI(c)), FOR(4) {
  u32 sum = gw(Z, i);
  for (int k = 0; k < 4; k++) sum += (u32)(gsb(X, 4 * i + k) * (s32)gb(Y, 4 * i + k));
  pw(R, i, sum);
})
OP3(msumuhm, vec_msum(US(a), US(b), UI(c)),
    FOR(4) pw(R, i, gw(Z, i) + gh(X, 2 * i) * gh(Y, 2 * i) + gh(X, 2 * i + 1) * gh(Y, 2 * i + 1)))
OP3(msumuhs, vec_msums(US(a), US(b), UI(c)),
    FOR(4) pw(R, i, su32((s64)gw(Z, i) + (s64)((u64)gh(X, 2 * i) * gh(Y, 2 * i)) +
                             (s64)((u64)gh(X, 2 * i + 1) * gh(Y, 2 * i + 1)))))
OP3(msumshm, vec_msum(SS(a), SS(b), SI(c)),
    FOR(4) pw(R, i, gw(Z, i) + (u32)(gsh(X, 2 * i) * gsh(Y, 2 * i)) +
                        (u32)(gsh(X, 2 * i + 1) * gsh(Y, 2 * i + 1))))
OP3(msumshs, vec_msums(SS(a), SS(b), SI(c)),
    FOR(4) pw(R, i, ss32((s64)gsw(Z, i) + (s64)gsh(X, 2 * i) * gsh(Y, 2 * i) +
                             (s64)gsh(X, 2 * i + 1) * gsh(Y, 2 * i + 1))))

/* Predicates: the compare instructions' record form, which sets CR6. */
PRED(all_eq_b, vec_all_eq(UC(a), UC(b)), { FOR(16) TALLY(gb(X, i) == gb(Y, i)); return all; })
PRED(any_eq_h, vec_any_eq(US(a), US(b)), { FOR(8) TALLY(gh(X, i) == gh(Y, i)); return any; })
PRED(any_ne_w, vec_any_ne(UI(a), UI(b)), { FOR(4) TALLY(gw(X, i) == gw(Y, i)); return !all; })
PRED(all_gt_sh, vec_all_gt(SS(a), SS(b)), { FOR(8) TALLY(gsh(X, i) > gsh(Y, i)); return all; })
PRED(any_gt_uw, vec_any_gt(UI(a), UI(b)), { FOR(4) TALLY(gw(X, i) > gw(Y, i)); return any; })
PRED(all_le_ub, vec_all_le(UC(a), UC(b)), { FOR(16) TALLY(gb(X, i) > gb(Y, i)); return !any; })
PRED(any_gt_sb, vec_any_gt(SC(a), SC(b)), { FOR(16) TALLY(gsb(X, i) > gsb(Y, i)); return any; })
PRED(all_eq_fp, vec_all_eq(FL(a), FL(b)), { FOR(4) TALLY(gf(X, i) == gf(Y, i)); return all; })
PRED(any_ge_fp, vec_any_ge(FL(a), FL(b)), { FOR(4) TALLY(gf(X, i) >= gf(Y, i)); return any; })
PRED(all_gt_fp, vec_all_gt(FL(a), FL(b)), { FOR(4) TALLY(gf(X, i) > gf(Y, i)); return all; })
PRED(all_in_fp, vec_all_in(FL(a), FL(b)),
     { FOR(4) TALLY(gf(X, i) <= gf(Y, i) && gf(X, i) >= -gf(Y, i)); return all; })

/* ---- memory forms ------------------------------------------------------------- */

static u8 pool[64] __attribute__((aligned(16)));

/* lvsl/lvsr and the classic unaligned load built on them. */
static NOINLINE vec unaligned_load(int offset) {
#ifdef __ALTIVEC__
  const u8* p = pool + offset;
  return mk(vec_perm(vec_ld(0, p), vec_ld(15, p), vec_lvsl(0, p)));
#else
  vec r;
  for (int i = 0; i < 16; i++) r.b[i] = pool[offset + i];
  return r;
#endif
}

static NOINLINE vec shift_masks(int offset) {
#ifdef __ALTIVEC__
  return mk(vec_xor(vec_lvsl(offset, pool), vec_lvsr(offset, pool)));
#else
  vec r;
  for (int i = 0; i < 16; i++) r.b[i] = (u8)((offset + i) ^ (16 - offset + i));
  return r;
#endif
}

/* Element stores write the one element the address selects. */
static NOINLINE void element_stores(vec x, u8* out) {
#ifdef __ALTIVEC__
  const vector unsigned char a = vec_ld(0, x.b);
  vec_ste(UC(a), 5, out);
  vec_ste(US(a), 10, (unsigned short*)out);
  vec_ste(UI(a), 20, (unsigned int*)out);
#else
  out[5] = x.b[5];
  out[10] = x.b[10];
  out[11] = x.b[11];
  for (int i = 0; i < 4; i++) out[20 + i] = x.b[4 + i];
#endif
}

/* Element loads fill only the selected element, so only that one is read back. */
static NOINLINE u32 element_loads(void) {
#ifdef __ALTIVEC__
  const vec b = mk(UC(vec_lde(7, pool)));
  const vec h = mk(UC(vec_lde(18, (unsigned short*)pool)));
  const vec w = mk(UC(vec_lde(40, (unsigned int*)pool)));
  return (u32)b.b[7] | (u32)h.b[2] << 8 | (u32)h.b[3] << 16 | (u32)w.b[8] << 24 |
         ((u32)w.b[9] ^ w.b[10] ^ w.b[11]);
#else
  return (u32)pool[7] | (u32)pool[18] << 8 | (u32)pool[19] << 16 | (u32)pool[40] << 24 |
         ((u32)pool[41] ^ pool[42] ^ pool[43]);
#endif
}

/* ---- driver -------------------------------------------------------------------- */

static volatile u32 g_seed = 0xC0CA1AEu;
static u32 rng;

static u32 rnd(void) {
  u32 x = rng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rng = x;
  return x;
}

static vec rnd_vec(u32 round) {
  vec v;
  for (int i = 0; i < 16; i += 4) {
    u32 w = rnd();
    switch ((round + (u32)i) % 9) {
      case 1: w = 0; break;
      case 2: w = 0xFFFFFFFFu; break;
      case 3: w = 0x80000000u; break;
      case 4: w = 0x7FFF8000u; break;
      case 5: w = 0x807F80FFu; break;
      default: break;
    }
    v.b[i] = (u8)(w >> 24);
    v.b[i + 1] = (u8)(w >> 16);
    v.b[i + 2] = (u8)(w >> 8);
    v.b[i + 3] = (u8)w;
  }
  return v;
}

static const volatile u32 float_bits[] = {
    0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x3F000000, 0x40490FDB, 0xC02DF854,
    0x7F800000, 0xFF800000, 0x7FC00000, 0x4B800001, 0x3DCCCCCD, 0xCF000000, 0x4F000000,
    0x4EFFFFFF, 0x3FC00000, 0x40200000, 0xBF000000, 0x42F6E979, 0xC479C000, 0x501502F9,
    0x3E99999A, 0xBFC00000, 0x40600000, 0x4F800000, 0xCF800000, 0x3F7FFFFF, 0x41000000,
};
#define NFLOAT (sizeof float_bits / sizeof float_bits[0])

static vec float_vec(u32 n) {
  vec v;
  for (int i = 0; i < 4; i++) {
    const u32 w = float_bits[(n + (u32)i * 5) % NFLOAT];
    v.b[4 * i] = (u8)(w >> 24);
    v.b[4 * i + 1] = (u8)(w >> 16);
    v.b[4 * i + 2] = (u8)(w >> 8);
    v.b[4 * i + 3] = (u8)w;
  }
  return v;
}

/* Hashes a result. NaNs in float results are folded to one pattern. */
static u32 mix_vec(u32 h, vec v, int is_float) {
  for (int i = 0; i < 4; i++) {
    u32 w = (u32)v.b[4 * i] << 24 | (u32)v.b[4 * i + 1] << 16 | (u32)v.b[4 * i + 2] << 8 |
            v.b[4 * i + 3];
    if (is_float && (w & 0x7F800000u) == 0x7F800000u && (w & 0x007FFFFFu)) w = 0x7FC00000u;
    h = mix(h, w);
  }
  return h;
}

struct op2 {
  const char* name;
  vec (*fn)(vec, vec);
  int floats; /* 1: float inputs, 2: float inputs and results */
};
struct op3 {
  const char* name;
  vec (*fn)(vec, vec, vec);
  int floats;
};
struct pred {
  const char* name;
  u32 (*fn)(vec, vec);
  int floats;
};

#define I(n) {#n, n, 0}
#define F(n) {#n, n, 2}
#define FI(n) {#n, n, 1}

static const struct op2 ops2[] = {
    I(addubm), I(adduhm), I(adduwm), I(addcuw), I(addubs), I(adduhs), I(adduws), I(addsbs),
    I(addshs), I(addsws), I(sububm), I(subuhm), I(subuwm), I(subcuw), I(sububs), I(subuhs),
    I(subuws), I(subsbs), I(subshs), I(subsws), I(maxub), I(maxuh), I(maxuw), I(maxsb),
    I(maxsh), I(maxsw), I(minub), I(minuh), I(minuw), I(minsb), I(minsh), I(minsw),
    I(avgub), I(avguh), I(avguw), I(avgsb), I(avgsh), I(avgsw), I(rlb), I(rlh),
    I(rlw), I(slb), I(slh), I(slw), I(srb), I(srh), I(srw), I(srab),
    I(srah), I(sraw), I(sll), I(srl), I(slo), I(sro), I(sld3), I(sld11),
    I(and_), I(andc), I(or_), I(nor), I(xor_), I(muleub), I(muloub), I(mulesb),
    I(mulosb), I(muleuh), I(mulouh), I(mulesh), I(mulosh), I(sum4ubs), I(sum4sbs), I(sum4shs),
    I(sum2sws), I(sumsws), I(mrghb), I(mrglb), I(mrghh), I(mrglh), I(mrghw), I(mrglw),
    I(pkuhum), I(pkuwum), I(pkuhus), I(pkuwus), I(pkshss), I(pkswss), I(pkshus), I(pkswus),
    I(pkpx), I(upkhsb), I(upklsb), I(upkhsh), I(upklsh), I(upkhpx), I(upklpx), I(spltb5),
    I(splth6), I(spltw1), I(spltisb), I(spltish), I(spltisw), I(cmpequb), I(cmpequh), I(cmpequw),
    I(cmpgtub), I(cmpgtuh), I(cmpgtuw), I(cmpgtsb), I(cmpgtsh), I(cmpgtsw), I(cfsx3), I(cfux7),
    F(addfp), F(subfp), F(maxfp), F(minfp), FI(cmpeqfp), FI(cmpgefp), FI(cmpgtfp), FI(cmpbfp),
    F(rfin), F(rfiz), F(rfip), F(rfim), FI(ctsxs2), FI(ctuxs5),
};
static const struct op3 ops3[] = {
    I(sel), I(perm), F(maddfp), F(nmsubfp), I(mhaddshs), I(mhraddshs), I(mladduhm),
    I(msumubm), I(msummbm), I(msumuhm), I(msumuhs), I(msumshm), I(msumshs),
};
static const struct pred preds[] = {
    I(all_eq_b), I(any_eq_h), I(any_ne_w), I(all_gt_sh), I(any_gt_uw), I(all_le_ub),
    I(any_gt_sb), FI(all_eq_fp), FI(any_ge_fp), FI(all_gt_fp), FI(all_in_fp),
};

#define ROUNDS 64

int test_main(int argc, char** argv, char** envp) {
  (void)argc;
  (void)argv;
  (void)envp;

  for (u32 k = 0; k < sizeof ops2 / sizeof ops2[0]; k++) {
    u32 h = 1;
    rng = g_seed;
    clear_sat();
    for (u32 round = 0; round < ROUNDS; round++) {
      const vec a = ops2[k].floats ? float_vec(round) : rnd_vec(round);
      const vec b = ops2[k].floats ? float_vec(round * 3 + 1) : rnd_vec(round + 4);
      h = mix_vec(h, ops2[k].fn(a, b), ops2[k].floats == 2);
      /* The same value on both sides: equal elements and exact cancellation. */
      h = mix_vec(h, ops2[k].fn(a, a), ops2[k].floats == 2);
    }
    h = mix(h, read_sat());
    put_kv(ops2[k].name, h);
  }

  for (u32 k = 0; k < sizeof ops3 / sizeof ops3[0]; k++) {
    u32 h = 1;
    rng = g_seed + 7;
    clear_sat();
    for (u32 round = 0; round < ROUNDS; round++) {
      const int fl = ops3[k].floats;
      const vec a = fl ? float_vec(round) : rnd_vec(round);
      const vec b = fl ? float_vec(round * 3 + 1) : rnd_vec(round + 4);
      const vec c = fl ? float_vec(round * 7 + 2) : rnd_vec(round + 8);
      h = mix_vec(h, ops3[k].fn(a, b, c), fl == 2);
    }
    h = mix(h, read_sat());
    put_kv(ops3[k].name, h);
  }

  for (u32 k = 0; k < sizeof preds / sizeof preds[0]; k++) {
    u32 h = 1;
    rng = g_seed + 99;
    for (u32 round = 0; round < ROUNDS; round++) {
      const int fl = preds[k].floats;
      const vec a = fl ? float_vec(round) : rnd_vec(round);
      const vec b = fl ? float_vec(round * 3 + 1) : rnd_vec(round + 4);
      h = mix(h, preds[k].fn(a, b) ? 1 : 0);
      h = mix(h, preds[k].fn(a, a) ? 1 : 0);
      h = mix(h, preds[k].fn(b, a) ? 1 : 0);
    }
    put_kv(preds[k].name, h);
  }

  /* The saturation flag sticks until cleared. */
  clear_sat();
  const vec big = rnd_vec(2), small = zero_vec;
  u32 flags = read_sat();
  /* The results are hashed so the compiler can't drop the calls: it doesn't
   * know they also touch the saturation flag. */
  u32 used = mix_vec(1, addubs(small, small), 0);
  flags = flags * 2 + read_sat();
  used = mix_vec(used, addubs(big, big), 0);
  flags = flags * 2 + read_sat();
  used = mix_vec(used, addubm(small, small), 0);
  flags = flags * 2 + read_sat();
  clear_sat();
  flags = flags * 2 + read_sat();
  put_kv("sat_flag", flags);
  put_kv("sat_results", used);

  rng = g_seed ^ 0x77;
  for (u32 i = 0; i < sizeof pool; i++) pool[i] = (u8)rnd();
  u32 h = 1;
  for (int offset = 0; offset < 33; offset++) {
    h = mix_vec(h, unaligned_load(offset), 0);
    h = mix_vec(h, shift_masks(offset & 15), 0);
  }
  put_kv("unaligned", h);
  u8 out[32] __attribute__((aligned(16)));
  for (u32 i = 0; i < sizeof out; i++) out[i] = (u8)i;
  element_stores(rnd_vec(0), out);
  h = 1;
  for (u32 i = 0; i < sizeof out; i++) h = mix(h, out[i]);
  put_kv("element_store", h);
  put_kv("element_load", element_loads());
  put_str("done\n");
  return 0;
}
