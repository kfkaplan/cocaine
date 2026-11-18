/* MMX, SSE, SSE2 and SSE3 test.
 *
 * Intel only. The reference is this same source built for 64-bit Intel and
 * run under Rosetta. Results print as hashes of raw register contents, NaNs
 * included, so everything must match bit for bit.
 *
 * Not compared: rcpps and rsqrtps, which are estimates whose low bits differ
 * from one CPU to the next. */
#include <emmintrin.h>
#include <pmmintrin.h>

#include "rt.h"

static volatile u32 g_seed = 0x9E3779B9u;
static u32 rng;

static u32 rnd(void) {
  u32 x = rng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rng = x;
  return x;
}

typedef union {
  __m128i i;
  __m128 f;
  __m128d d;
  u8 b[16];
  u32 w[4];
  u64 q[2];
} v128;

static u32 mix_v(u32 h, v128 v) { return mix64(mix64(h, v.q[0]), v.q[1]); }
static u32 mix_i(u32 h, __m128i x) {
  v128 v;
  v.i = x;
  return mix_v(h, v);
}
static u32 mix_f(u32 h, __m128 x) {
  v128 v;
  v.f = x;
  return mix_v(h, v);
}
static u32 mix_d(u32 h, __m128d x) {
  v128 v;
  v.d = x;
  return mix_v(h, v);
}

/* Random bytes, with stretches of the values saturating arithmetic cares about. */
static __m128i rnd_i(u32 round) {
  v128 v;
  for (int k = 0; k < 4; k++) v.w[k] = rnd();
  switch (round % 7) {
    case 1: v.w[0] = 0; v.w[3] = 0xFFFFFFFFu; break;
    case 2: v.w[1] = 0x80000000u; v.w[2] = 0x7FFFFFFFu; break;
    case 3: v.w[0] = 0x80008000u; v.w[1] = 0x7FFF7FFFu; break;
    case 4: v.w[2] = 0x80808080u; v.w[3] = 0x7F7F7F7Fu; break;
    default: break;
  }
  return v.i;
}

/* ---- SSE2 integer --------------------------------------------------------- */

static void test_integer(void) {
  u32 h_add = 1, h_mul = 2, h_cmp = 3, h_pack = 4, h_shift = 5, h_move = 6;
  rng = g_seed;
  for (u32 round = 0; round < 150; round++) {
    const __m128i a = rnd_i(round), b = rnd_i(round + 3);
    h_add = mix_i(h_add, _mm_add_epi8(a, b));
    h_add = mix_i(h_add, _mm_add_epi16(a, b));
    h_add = mix_i(h_add, _mm_add_epi32(a, b));
    h_add = mix_i(h_add, _mm_add_epi64(a, b));
    h_add = mix_i(h_add, _mm_sub_epi8(a, b));
    h_add = mix_i(h_add, _mm_sub_epi16(a, b));
    h_add = mix_i(h_add, _mm_sub_epi32(a, b));
    h_add = mix_i(h_add, _mm_sub_epi64(a, b));
    h_add = mix_i(h_add, _mm_adds_epi8(a, b));
    h_add = mix_i(h_add, _mm_adds_epi16(a, b));
    h_add = mix_i(h_add, _mm_adds_epu8(a, b));
    h_add = mix_i(h_add, _mm_adds_epu16(a, b));
    h_add = mix_i(h_add, _mm_subs_epi8(a, b));
    h_add = mix_i(h_add, _mm_subs_epi16(a, b));
    h_add = mix_i(h_add, _mm_subs_epu8(a, b));
    h_add = mix_i(h_add, _mm_subs_epu16(a, b));
    h_add = mix_i(h_add, _mm_avg_epu8(a, b));
    h_add = mix_i(h_add, _mm_avg_epu16(a, b));

    h_mul = mix_i(h_mul, _mm_mullo_epi16(a, b));
    h_mul = mix_i(h_mul, _mm_mulhi_epi16(a, b));
    h_mul = mix_i(h_mul, _mm_mulhi_epu16(a, b));
    h_mul = mix_i(h_mul, _mm_mul_epu32(a, b));
    h_mul = mix_i(h_mul, _mm_madd_epi16(a, b));
    h_mul = mix_i(h_mul, _mm_sad_epu8(a, b));

    h_cmp = mix_i(h_cmp, _mm_cmpeq_epi8(a, b));
    h_cmp = mix_i(h_cmp, _mm_cmpeq_epi16(a, b));
    h_cmp = mix_i(h_cmp, _mm_cmpeq_epi32(a, _mm_shuffle_epi32(a, 0xE1)));
    h_cmp = mix_i(h_cmp, _mm_cmpgt_epi8(a, b));
    h_cmp = mix_i(h_cmp, _mm_cmpgt_epi16(a, b));
    h_cmp = mix_i(h_cmp, _mm_cmpgt_epi32(a, b));
    h_cmp = mix_i(h_cmp, _mm_min_epu8(a, b));
    h_cmp = mix_i(h_cmp, _mm_max_epu8(a, b));
    h_cmp = mix_i(h_cmp, _mm_min_epi16(a, b));
    h_cmp = mix_i(h_cmp, _mm_max_epi16(a, b));
    h_cmp = mix_i(h_cmp, _mm_and_si128(a, b));
    h_cmp = mix_i(h_cmp, _mm_andnot_si128(a, b));
    h_cmp = mix_i(h_cmp, _mm_or_si128(a, b));
    h_cmp = mix_i(h_cmp, _mm_xor_si128(a, b));
    h_cmp = mix(h_cmp, (u32)_mm_movemask_epi8(a));

    h_pack = mix_i(h_pack, _mm_unpacklo_epi8(a, b));
    h_pack = mix_i(h_pack, _mm_unpacklo_epi16(a, b));
    h_pack = mix_i(h_pack, _mm_unpacklo_epi32(a, b));
    h_pack = mix_i(h_pack, _mm_unpacklo_epi64(a, b));
    h_pack = mix_i(h_pack, _mm_unpackhi_epi8(a, b));
    h_pack = mix_i(h_pack, _mm_unpackhi_epi16(a, b));
    h_pack = mix_i(h_pack, _mm_unpackhi_epi32(a, b));
    h_pack = mix_i(h_pack, _mm_unpackhi_epi64(a, b));
    h_pack = mix_i(h_pack, _mm_packs_epi16(a, b));
    h_pack = mix_i(h_pack, _mm_packus_epi16(a, b));
    h_pack = mix_i(h_pack, _mm_packs_epi32(a, b));
    h_pack = mix_i(h_pack, _mm_shuffle_epi32(a, 0x1B));
    h_pack = mix_i(h_pack, _mm_shuffle_epi32(a, 0x93));
    h_pack = mix_i(h_pack, _mm_shufflehi_epi16(a, 0x4E));
    h_pack = mix_i(h_pack, _mm_shufflelo_epi16(a, 0xB1));
    h_pack = mix(h_pack, (u32)_mm_extract_epi16(a, 5));
    h_pack = mix_i(h_pack, _mm_insert_epi16(a, (int)(round * 977), 2));

    /* Shifts by a count taken from a register, including counts that are too big. */
    const __m128i count = _mm_cvtsi32_si128((int)(rnd() % 70));
    h_shift = mix_i(h_shift, _mm_sll_epi16(a, count));
    h_shift = mix_i(h_shift, _mm_sll_epi32(a, count));
    h_shift = mix_i(h_shift, _mm_sll_epi64(a, count));
    h_shift = mix_i(h_shift, _mm_srl_epi16(a, count));
    h_shift = mix_i(h_shift, _mm_srl_epi32(a, count));
    h_shift = mix_i(h_shift, _mm_srl_epi64(a, count));
    h_shift = mix_i(h_shift, _mm_sra_epi16(a, count));
    h_shift = mix_i(h_shift, _mm_sra_epi32(a, count));
    /* Shifts by a constant. */
    h_shift = mix_i(h_shift, _mm_slli_epi16(a, 3));
    h_shift = mix_i(h_shift, _mm_slli_epi32(a, 17));
    h_shift = mix_i(h_shift, _mm_slli_epi64(a, 40));
    h_shift = mix_i(h_shift, _mm_srli_epi16(a, 9));
    h_shift = mix_i(h_shift, _mm_srli_epi32(a, 1));
    h_shift = mix_i(h_shift, _mm_srli_epi64(a, 63));
    h_shift = mix_i(h_shift, _mm_srai_epi16(a, 15));
    h_shift = mix_i(h_shift, _mm_srai_epi32(a, 7));
    h_shift = mix_i(h_shift, _mm_slli_si128(a, 5));
    h_shift = mix_i(h_shift, _mm_srli_si128(a, 11));

    /* Moves through memory and the general registers. */
    v128 mem, out;
    mem.i = a;
    out.i = _mm_setzero_si128();
    h_move = mix_i(h_move, _mm_loadu_si128((const __m128i*)(mem.b)));
    h_move = mix_i(h_move, _mm_loadl_epi64(&mem.i));
    h_move = mix_i(h_move, _mm_move_epi64(a));
    h_move = mix_i(h_move, _mm_lddqu_si128(&mem.i));
    _mm_storel_epi64(&out.i, b);
    h_move = mix_v(h_move, out);
    _mm_storeu_si128(&out.i, b);
    h_move = mix_v(h_move, out);
    _mm_stream_si128(&out.i, a);
    h_move = mix_v(h_move, out);
    _mm_stream_si32((int*)&out.w[1], (int)round);
    h_move = mix_v(h_move, out);
    _mm_maskmoveu_si128(b, a, (char*)out.b);
    h_move = mix_v(h_move, out);
    h_move = mix(h_move, (u32)_mm_cvtsi128_si32(a));
    h_move = mix_i(h_move, _mm_cvtsi32_si128((int)rnd()));
  }
  put_kv("sse.int.add", h_add);
  put_kv("sse.int.mul", h_mul);
  put_kv("sse.int.cmp", h_cmp);
  put_kv("sse.int.pack", h_pack);
  put_kv("sse.int.shift", h_shift);
  put_kv("sse.int.move", h_move);
}

/* ---- SSE floating point ------------------------------------------------------ */

static const volatile u32 fv[] = {
    0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x3F000000, 0x40490FDB, 0xC02DF854,
    0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7F800001, 0x7FA12345, 0xFFD54321,
    0x00000001, 0x007FFFFF, 0x00800000, 0x7F7FFFFF, 0x4B800001, 0x3DCCCCCD, 0xCF000000,
    0x4F000000, 0x4EFFFFFF, 0x3FC00000, 0x40200000, 0xBF000000,
};
#define NFV (sizeof fv / sizeof fv[0])

static const volatile u64 dfv[] = {
    0x0000000000000000ull, 0x8000000000000000ull, 0x3FF0000000000000ull, 0xBFF0000000000000ull,
    0x3FE0000000000000ull, 0x400921FB54442D18ull, 0x7FF0000000000000ull, 0xFFF0000000000000ull,
    0x7FF8000000000000ull, 0xFFF8000000000000ull, 0x7FF0000000000001ull, 0x7FF4123456789ABCull,
    0x0000000000000001ull, 0x0010000000000000ull, 0x7FEFFFFFFFFFFFFFull, 0x41E0000000000000ull,
    0xC1E0000000000000ull, 0x41DFFFFFFFC00000ull, 0x3FF8000000000000ull, 0x4004000000000000ull,
    0x3FB999999999999Aull, 0x47EFFFFFE0000000ull, 0x380FFFFFC0000000ull, 0xC1E0000000200000ull,
};
#define NDFV (sizeof dfv / sizeof dfv[0])

static __m128 f4(u32 i) {
  v128 v;
  for (u32 k = 0; k < 4; k++) v.w[k] = fv[(i + k * 5) % NFV];
  return v.f;
}

static __m128d d2(u32 i) {
  v128 v;
  v.q[0] = dfv[i % NDFV];
  v.q[1] = dfv[(i * 7 + 3) % NDFV];
  return v.d;
}

static void test_single(void) {
  u32 h_arith = 1, h_cmp = 2, h_shuffle = 3, h_scalar = 4;
  for (u32 i = 0; i < NFV; i++) {
    for (u32 j = 0; j < NFV; j++) {
      const __m128 a = f4(i), b = f4(j);
      h_arith = mix_f(h_arith, _mm_add_ps(a, b));
      h_arith = mix_f(h_arith, _mm_sub_ps(a, b));
      h_arith = mix_f(h_arith, _mm_mul_ps(a, b));
      h_arith = mix_f(h_arith, _mm_div_ps(a, b));
      h_arith = mix_f(h_arith, _mm_min_ps(a, b));
      h_arith = mix_f(h_arith, _mm_max_ps(a, b));
      h_arith = mix_f(h_arith, _mm_sqrt_ps(a));
      h_arith = mix_f(h_arith, _mm_hadd_ps(a, b));
      h_arith = mix_f(h_arith, _mm_hsub_ps(a, b));
      h_arith = mix_f(h_arith, _mm_addsub_ps(a, b));

      h_cmp = mix_f(h_cmp, _mm_cmpeq_ps(a, b));
      h_cmp = mix_f(h_cmp, _mm_cmplt_ps(a, b));
      h_cmp = mix_f(h_cmp, _mm_cmple_ps(a, b));
      h_cmp = mix_f(h_cmp, _mm_cmpunord_ps(a, b));
      h_cmp = mix_f(h_cmp, _mm_cmpneq_ps(a, b));
      h_cmp = mix_f(h_cmp, _mm_cmpnlt_ps(a, b));
      h_cmp = mix_f(h_cmp, _mm_cmpnle_ps(a, b));
      h_cmp = mix_f(h_cmp, _mm_cmpord_ps(a, b));
      h_cmp = mix_f(h_cmp, _mm_cmpgt_ps(a, b));
      h_cmp = mix_f(h_cmp, _mm_cmpge_ss(a, b));
      h_cmp = mix(h_cmp, (u32)_mm_comieq_ss(a, b) | (u32)_mm_comilt_ss(a, b) << 1 |
                             (u32)_mm_comile_ss(a, b) << 2 | (u32)_mm_comigt_ss(a, b) << 3 |
                             (u32)_mm_comige_ss(a, b) << 4 | (u32)_mm_comineq_ss(a, b) << 5 |
                             (u32)_mm_ucomieq_ss(a, b) << 6 | (u32)_mm_ucomilt_ss(a, b) << 7 |
                             (u32)_mm_ucomigt_ss(a, b) << 8 | (u32)_mm_ucomineq_ss(a, b) << 9);
      h_cmp = mix(h_cmp, (u32)_mm_movemask_ps(a));
      h_cmp = mix_f(h_cmp, _mm_and_ps(a, b));
      h_cmp = mix_f(h_cmp, _mm_andnot_ps(a, b));
      h_cmp = mix_f(h_cmp, _mm_or_ps(a, b));
      h_cmp = mix_f(h_cmp, _mm_xor_ps(a, b));

      h_shuffle = mix_f(h_shuffle, _mm_shuffle_ps(a, b, 0x1B));
      h_shuffle = mix_f(h_shuffle, _mm_shuffle_ps(a, b, 0xD8));
      h_shuffle = mix_f(h_shuffle, _mm_unpacklo_ps(a, b));
      h_shuffle = mix_f(h_shuffle, _mm_unpackhi_ps(a, b));
      h_shuffle = mix_f(h_shuffle, _mm_movehl_ps(a, b));
      h_shuffle = mix_f(h_shuffle, _mm_movelh_ps(a, b));
      h_shuffle = mix_f(h_shuffle, _mm_move_ss(a, b));
      h_shuffle = mix_f(h_shuffle, _mm_movehdup_ps(a));
      h_shuffle = mix_f(h_shuffle, _mm_moveldup_ps(a));

      h_scalar = mix_f(h_scalar, _mm_add_ss(a, b));
      h_scalar = mix_f(h_scalar, _mm_sub_ss(a, b));
      h_scalar = mix_f(h_scalar, _mm_mul_ss(a, b));
      h_scalar = mix_f(h_scalar, _mm_div_ss(a, b));
      h_scalar = mix_f(h_scalar, _mm_min_ss(a, b));
      h_scalar = mix_f(h_scalar, _mm_max_ss(a, b));
      h_scalar = mix_f(h_scalar, _mm_sqrt_ss(b));
    }
  }
  put_kv("sse.single.arith", h_arith);
  put_kv("sse.single.cmp", h_cmp);
  put_kv("sse.single.shuffle", h_shuffle);
  put_kv("sse.single.scalar", h_scalar);
}

static void test_double(void) {
  u32 h_arith = 1, h_cmp = 2, h_shuffle = 3, h_scalar = 4;
  for (u32 i = 0; i < NDFV; i++) {
    for (u32 j = 0; j < NDFV; j++) {
      const __m128d a = d2(i), b = d2(j);
      h_arith = mix_d(h_arith, _mm_add_pd(a, b));
      h_arith = mix_d(h_arith, _mm_sub_pd(a, b));
      h_arith = mix_d(h_arith, _mm_mul_pd(a, b));
      h_arith = mix_d(h_arith, _mm_div_pd(a, b));
      h_arith = mix_d(h_arith, _mm_min_pd(a, b));
      h_arith = mix_d(h_arith, _mm_max_pd(a, b));
      h_arith = mix_d(h_arith, _mm_sqrt_pd(a));
      h_arith = mix_d(h_arith, _mm_hadd_pd(a, b));
      h_arith = mix_d(h_arith, _mm_hsub_pd(a, b));
      h_arith = mix_d(h_arith, _mm_addsub_pd(a, b));

      h_cmp = mix_d(h_cmp, _mm_cmpeq_pd(a, b));
      h_cmp = mix_d(h_cmp, _mm_cmplt_pd(a, b));
      h_cmp = mix_d(h_cmp, _mm_cmple_pd(a, b));
      h_cmp = mix_d(h_cmp, _mm_cmpunord_pd(a, b));
      h_cmp = mix_d(h_cmp, _mm_cmpneq_pd(a, b));
      h_cmp = mix_d(h_cmp, _mm_cmpnlt_pd(a, b));
      h_cmp = mix_d(h_cmp, _mm_cmpnle_pd(a, b));
      h_cmp = mix_d(h_cmp, _mm_cmpord_pd(a, b));
      h_cmp = mix_d(h_cmp, _mm_cmplt_sd(a, b));
      h_cmp = mix(h_cmp, (u32)_mm_comieq_sd(a, b) | (u32)_mm_comilt_sd(a, b) << 1 |
                             (u32)_mm_comile_sd(a, b) << 2 | (u32)_mm_comigt_sd(a, b) << 3 |
                             (u32)_mm_ucomige_sd(a, b) << 4 | (u32)_mm_ucomineq_sd(a, b) << 5);
      h_cmp = mix(h_cmp, (u32)_mm_movemask_pd(a));
      h_cmp = mix_d(h_cmp, _mm_and_pd(a, b));
      h_cmp = mix_d(h_cmp, _mm_andnot_pd(a, b));
      h_cmp = mix_d(h_cmp, _mm_or_pd(a, b));
      h_cmp = mix_d(h_cmp, _mm_xor_pd(a, b));

      h_shuffle = mix_d(h_shuffle, _mm_shuffle_pd(a, b, 1));
      h_shuffle = mix_d(h_shuffle, _mm_shuffle_pd(a, b, 2));
      h_shuffle = mix_d(h_shuffle, _mm_unpacklo_pd(a, b));
      h_shuffle = mix_d(h_shuffle, _mm_unpackhi_pd(a, b));
      h_shuffle = mix_d(h_shuffle, _mm_move_sd(a, b));
      h_shuffle = mix_d(h_shuffle, _mm_movedup_pd(a));

      h_scalar = mix_d(h_scalar, _mm_add_sd(a, b));
      h_scalar = mix_d(h_scalar, _mm_sub_sd(a, b));
      h_scalar = mix_d(h_scalar, _mm_mul_sd(a, b));
      h_scalar = mix_d(h_scalar, _mm_div_sd(a, b));
      h_scalar = mix_d(h_scalar, _mm_min_sd(a, b));
      h_scalar = mix_d(h_scalar, _mm_max_sd(a, b));
      h_scalar = mix_d(h_scalar, _mm_sqrt_sd(a, b));
    }
  }
  put_kv("sse.double.arith", h_arith);
  put_kv("sse.double.cmp", h_cmp);
  put_kv("sse.double.shuffle", h_shuffle);
  put_kv("sse.double.scalar", h_scalar);
}

/* ---- conversions, in each of the four rounding modes ------------------------- */

static void test_convert(void) {
  const u32 saved = _mm_getcsr();
  for (u32 rc = 0; rc < 4; rc++) {
    u32 h = 1;
    _mm_setcsr((saved & ~0x6000u) | (rc << 13));
    for (u32 i = 0; i < NFV; i++) {
      const __m128 a = f4(i);
      const __m128d d = d2(i);
      v128 ints;
      for (u32 k = 0; k < 4; k++) ints.w[k] = fv[(i * 3 + k) % NFV] ^ (0x01234567u * (k + 1));
      h = mix_i(h, _mm_cvtps_epi32(a));
      h = mix_i(h, _mm_cvttps_epi32(a));
      h = mix_i(h, _mm_cvtpd_epi32(d));
      h = mix_i(h, _mm_cvttpd_epi32(d));
      h = mix_f(h, _mm_cvtepi32_ps(ints.i));
      h = mix_d(h, _mm_cvtepi32_pd(ints.i));
      h = mix_d(h, _mm_cvtps_pd(a));
      h = mix_f(h, _mm_cvtpd_ps(d));
      h = mix_d(h, _mm_cvtss_sd(d, a));
      h = mix_f(h, _mm_cvtsd_ss(a, d));
      h = mix(h, (u32)_mm_cvtss_si32(a));
      h = mix(h, (u32)_mm_cvttss_si32(a));
      h = mix(h, (u32)_mm_cvtsd_si32(d));
      h = mix(h, (u32)_mm_cvttsd_si32(d));
      h = mix_f(h, _mm_cvtsi32_ss(a, (int)ints.w[0]));
      h = mix_d(h, _mm_cvtsi32_sd(d, (int)ints.w[1]));
      /* Arithmetic follows the rounding mode too. */
      h = mix_f(h, _mm_div_ps(a, f4(i + 7)));
      h = mix_d(h, _mm_add_pd(d, d2(i + 5)));
      h = mix_d(h, _mm_sqrt_pd(_mm_and_pd(d, d2(18))));
    }
    _mm_setcsr(saved);
    put_str("sse.convert");
    put_dec((s32)rc);
    put_kv("", h);
  }
  put_kv("sse.mxcsr", _mm_getcsr() & 0xFFC0u);
}

/* ---- MMX, by hand: current compilers turn the MMX intrinsics into SSE2 -------- */

#define MMX_BINARY(name, insn)                                                  \
  static NOINLINE u64 name(u64 a, u64 b) {                                      \
    u64 r;                                                                      \
    __asm__ volatile("movq %1, %%mm0\n" insn " %2, %%mm0\nmovq %%mm0, %0\nemms" \
                     : "=m"(r)                                                  \
                     : "m"(a), "m"(b)                                           \
                     : "mm0");                                                  \
    return r;                                                                   \
  }

MMX_BINARY(m_paddb, "paddb") MMX_BINARY(m_paddw, "paddw") MMX_BINARY(m_paddd, "paddd")
MMX_BINARY(m_paddq, "paddq") MMX_BINARY(m_psubb, "psubb") MMX_BINARY(m_psubw, "psubw")
MMX_BINARY(m_psubd, "psubd") MMX_BINARY(m_psubq, "psubq") MMX_BINARY(m_paddsb, "paddsb")
MMX_BINARY(m_paddsw, "paddsw") MMX_BINARY(m_paddusb, "paddusb") MMX_BINARY(m_paddusw, "paddusw")
MMX_BINARY(m_psubsb, "psubsb") MMX_BINARY(m_psubsw, "psubsw") MMX_BINARY(m_psubusb, "psubusb")
MMX_BINARY(m_psubusw, "psubusw") MMX_BINARY(m_pmullw, "pmullw") MMX_BINARY(m_pmulhw, "pmulhw")
MMX_BINARY(m_pmulhuw, "pmulhuw") MMX_BINARY(m_pmuludq, "pmuludq") MMX_BINARY(m_pmaddwd, "pmaddwd")
MMX_BINARY(m_psadbw, "psadbw") MMX_BINARY(m_pavgb, "pavgb") MMX_BINARY(m_pavgw, "pavgw")
MMX_BINARY(m_pminub, "pminub") MMX_BINARY(m_pmaxub, "pmaxub") MMX_BINARY(m_pminsw, "pminsw")
MMX_BINARY(m_pmaxsw, "pmaxsw") MMX_BINARY(m_pand, "pand") MMX_BINARY(m_pandn, "pandn")
MMX_BINARY(m_por, "por") MMX_BINARY(m_pxor, "pxor") MMX_BINARY(m_pcmpeqb, "pcmpeqb")
MMX_BINARY(m_pcmpeqw, "pcmpeqw") MMX_BINARY(m_pcmpeqd, "pcmpeqd") MMX_BINARY(m_pcmpgtb, "pcmpgtb")
MMX_BINARY(m_pcmpgtw, "pcmpgtw") MMX_BINARY(m_pcmpgtd, "pcmpgtd")
MMX_BINARY(m_punpcklbw, "punpcklbw") MMX_BINARY(m_punpcklwd, "punpcklwd")
MMX_BINARY(m_punpckldq, "punpckldq") MMX_BINARY(m_punpckhbw, "punpckhbw")
MMX_BINARY(m_punpckhwd, "punpckhwd") MMX_BINARY(m_punpckhdq, "punpckhdq")
MMX_BINARY(m_packsswb, "packsswb") MMX_BINARY(m_packuswb, "packuswb")
MMX_BINARY(m_packssdw, "packssdw") MMX_BINARY(m_psllw, "psllw") MMX_BINARY(m_pslld, "pslld")
MMX_BINARY(m_psllq, "psllq") MMX_BINARY(m_psrlw, "psrlw") MMX_BINARY(m_psrld, "psrld")
MMX_BINARY(m_psrlq, "psrlq") MMX_BINARY(m_psraw, "psraw") MMX_BINARY(m_psrad, "psrad")

static u64 (*const volatile mmx_ops[])(u64, u64) = {
    m_paddb,    m_paddw,    m_paddd,    m_paddq,     m_psubb,     m_psubw,     m_psubd,
    m_psubq,    m_paddsb,   m_paddsw,   m_paddusb,   m_paddusw,   m_psubsb,    m_psubsw,
    m_psubusb,  m_psubusw,  m_pmullw,   m_pmulhw,    m_pmulhuw,   m_pmuludq,   m_pmaddwd,
    m_psadbw,   m_pavgb,    m_pavgw,    m_pminub,    m_pmaxub,    m_pminsw,    m_pmaxsw,
    m_pand,     m_pandn,    m_por,      m_pxor,      m_pcmpeqb,   m_pcmpeqw,   m_pcmpeqd,
    m_pcmpgtb,  m_pcmpgtw,  m_pcmpgtd,  m_punpcklbw, m_punpcklwd, m_punpckldq, m_punpckhbw,
    m_punpckhwd, m_punpckhdq, m_packsswb, m_packuswb, m_packssdw,
};
#define NMMX (sizeof mmx_ops / sizeof mmx_ops[0])

static u64 (*const volatile mmx_shifts[])(u64, u64) = {m_psllw, m_pslld, m_psllq, m_psrlw,
                                                       m_psrld, m_psrlq, m_psraw, m_psrad};

static NOINLINE void mmx_misc(u64 a, u64 b, u32 word, u64 out[6], u32 scalars[3]) {
  __asm__ volatile(
      "movq %0, %%mm0\n"
      "movq %1, %%mm1\n"
      "pshufw $0x1B, %%mm0, %%mm2\n"
      "movq %%mm2, (%3)\n"
      "movq %%mm0, %%mm3\n"
      "pinsrw $2, %2, %%mm3\n"
      "movq %%mm3, 8(%3)\n"
      "movq %%mm0, %%mm4\n"
      "psllq $13, %%mm4\n"
      "psrlw $3, %%mm4\n"
      "psrad $9, %%mm4\n"
      "movq %%mm4, 16(%3)\n"
      "movd %2, %%mm5\n"
      "movq %%mm5, 24(%3)\n"
      "movntq %%mm1, 32(%3)\n"
      "cvtpi2ps %%mm1, %%xmm0\n"
      "cvtps2pi %%xmm0, %%mm6\n"
      "movq %%mm6, 40(%3)\n"
      "pmovmskb %%mm0, %%eax\n"
      "movl %%eax, (%4)\n"
      "pextrw $3, %%mm0, %%eax\n"
      "movl %%eax, 4(%4)\n"
      "movd %%mm1, %%eax\n"
      "movl %%eax, 8(%4)\n"
      "emms"
      :
      : "m"(a), "m"(b), "r"(word), "r"(out), "r"(scalars)
      : "eax", "mm0", "mm1", "mm2", "mm3", "mm4", "mm5", "mm6", "xmm0", "memory");
}

static void test_mmx(void) {
  u32 h = 1, h_shift = 2, h_misc = 3;
  rng = g_seed ^ 0x5555;
  for (u32 round = 0; round < 60; round++) {
    v128 x, y;
    x.i = rnd_i(round);
    y.i = rnd_i(round + 2);
    for (u32 k = 0; k < NMMX; k++) h = mix64(h, mmx_ops[k](x.q[0], y.q[0]));
    const u64 count = rnd() % 70;
    for (u32 k = 0; k < 8; k++) h_shift = mix64(h_shift, mmx_shifts[k](x.q[1], count));
    u64 out[6];
    u32 scalars[3];
    mmx_misc(x.q[0], y.q[1], rnd(), out, scalars);
    for (u32 k = 0; k < 6; k++) h_misc = mix64(h_misc, out[k]);
    for (u32 k = 0; k < 3; k++) h_misc = mix(h_misc, scalars[k]);
  }
  put_kv("mmx.ops", h);
  put_kv("mmx.shift", h_shift);
  put_kv("mmx.misc", h_misc);
  /* The x87 must work again after MMX has borrowed its registers. */
  volatile long double third = 1.0L;
  third /= 3.0L;
  put_kv64("mmx.then_x87", (u64)(third * 3000000000.0L));
}

int test_main(int argc, char** argv, char** envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  test_integer();
  test_single();
  test_double();
  test_convert();
  test_mmx();
  put_str("done\n");
  return 0;
}
