// Software 80-bit extended-precision arithmetic for the x87 core.
//
// Every operation works the same way: unpack to a sign, an exponent and a
// 128-bit significand whose top bit is set, compute exactly (keeping a
// "sticky" low bit for anything shifted out), then round once in round_pack.

#include "cpu/x86/f80.h"

#include <cmath>

namespace f80 {
namespace {

typedef unsigned __int128 u128;

constexpr u64 kIntBit = 0x8000000000000000ull;

inline int clz64(u64 v) { return __builtin_clzll(v); }
inline int clz128(u128 v) {
  const u64 hi = (u64)(v >> 64);
  return hi ? clz64(hi) : 64 + clz64((u64)v);
}

// Shifts right, folding every bit shifted out into bit 0.
inline u128 shr_jam(u128 v, s32 n) {
  if (n <= 0) return v;
  if (n >= 128) return v != 0;
  return (v >> n) | ((v & (((u128)1 << n) - 1)) != 0);
}

// A finite, non-zero value: (man / 2^63) * 2^exp with man's top bit set.
struct Unpacked {
  bool sign;
  s32 exp;
  u64 man;
};

inline Unpacked unpack(F80 a) {
  // Denormals share the scale of biased exponent 1.
  Unpacked u{sign(a), (biased_exp(a) ? biased_exp(a) : 1) - 16383, a.man};
  const int lz = clz64(u.man);
  u.man <<= lz;
  u.exp -= lz;
  return u;
}

inline F80 quiet(F80 a) {
  a.man |= 0xC000000000000000ull;
  return a;
}

// With two NaNs the x87 keeps the one with the larger significand.
F80 propagate_nan(F80 a, F80 b) {
  const bool an = is_nan(a), bn = is_nan(b);
  if (an && bn) return quiet((a.man << 1) >= (b.man << 1) ? a : b);
  return quiet(an ? a : b);
}

inline bool round_up(int rc, bool sgn, bool odd, u128 rest, u128 half) {
  switch (rc) {
    case kNearest: return rest > half || (rest == half && odd);
    case kDown: return sgn && rest != 0;
    case kUp: return !sgn && rest != 0;
    default: return false;
  }
}

// Rounds and packs sgn * (sig / 2^127) * 2^exp. `sig` must have bit 127 set.
F80 round_pack(bool sgn, s32 exp, u128 sig, Env env) {
  s32 biased = exp + 16383;
  bool denormal = false;
  if (biased <= 0) {
    sig = shr_jam(sig, 1 - biased);
    biased = 0;
    denormal = true;
  }
  const int drop = 128 - env.prec;
  const u128 half = (u128)1 << (drop - 1);
  const u128 rest = sig & (((u128)1 << drop) - 1);
  u128 kept = sig >> drop;  // the significand as a `prec`-bit integer
  if (round_up(env.rc, sgn, (kept & 1) != 0, rest, half)) kept++;

  u64 man;
  if (kept >> env.prec) {  // rounding carried out of the top
    man = kIntBit;
    biased++;
  } else {
    man = (u64)(kept << (64 - env.prec));
  }
  // A denormal that rounded up to a full significand is the smallest normal.
  if (denormal) biased = (man & kIntBit) ? 1 : 0;

  if (biased >= 0x7FFF) {
    const bool to_inf =
        env.rc == kNearest || (env.rc == kUp && !sgn) || (env.rc == kDown && sgn);
    if (to_inf) return infinity(sgn);
    return make(sgn, 0x7FFE, ~0ull << (64 - env.prec));
  }
  return make(sgn, biased, man);
}

// Packs into a smaller IEEE format (`prec` significand bits including the
// implicit one). Returns the exponent and fraction fields, without the sign.
u64 pack_small(bool sgn, s32 exp, u128 sig, int prec, int bias, int rc) {
  s32 biased = exp + bias;
  bool denormal = false;
  if (biased <= 0) {
    sig = shr_jam(sig, 1 - biased);
    biased = 0;
    denormal = true;
  }
  const int drop = 128 - prec;
  const u128 half = (u128)1 << (drop - 1);
  const u128 rest = sig & (((u128)1 << drop) - 1);
  u64 kept = (u64)(sig >> drop);
  if (round_up(rc, sgn, (kept & 1) != 0, rest, half)) kept++;
  if (kept >> prec) {
    kept >>= 1;
    biased++;
  }
  if (denormal) biased = (kept >> (prec - 1)) ? 1 : 0;

  const s32 emax = 2 * bias + 1;
  const u64 frac_mask = ((u64)1 << (prec - 1)) - 1;
  if (biased >= emax) {
    const bool to_inf = rc == kNearest || (rc == kUp && !sgn) || (rc == kDown && sgn);
    if (to_inf) return (u64)emax << (prec - 1);
    return ((u64)(emax - 1) << (prec - 1)) | frac_mask;
  }
  return ((u64)biased << (prec - 1)) | (kept & frac_mask);
}

F80 add_impl(F80 a, F80 b, bool subtract, Env env) {
  if (is_nan(a) || is_nan(b)) return propagate_nan(a, b);
  const bool sa = sign(a), sb = sign(b) != subtract;
  if (is_inf(a)) {
    if (is_inf(b) && sa != sb) return indefinite();
    return infinity(sa);
  }
  if (is_inf(b)) return infinity(sb);
  if (is_zero(a) && is_zero(b)) return zero(sa == sb ? sa : env.rc == kDown);
  if (is_zero(b)) {
    const Unpacked x = unpack(a);
    return round_pack(sa, x.exp, (u128)x.man << 64, env);
  }
  if (is_zero(a)) {
    const Unpacked y = unpack(b);
    return round_pack(sb, y.exp, (u128)y.man << 64, env);
  }

  Unpacked x = unpack(a), y = unpack(b);
  x.sign = sa;
  y.sign = sb;
  if (x.exp < y.exp || (x.exp == y.exp && x.man < y.man)) {
    const Unpacked t = x;
    x = y;
    y = t;
  }
  // One spare bit on top for the carry, 63 below for alignment.
  const u128 xs = (u128)x.man << 63;
  const u128 ys = shr_jam((u128)y.man << 63, x.exp - y.exp);
  u128 sig;
  if (x.sign == y.sign) {
    sig = xs + ys;
  } else {
    sig = xs - ys;
    if (sig == 0) return zero(env.rc == kDown);
  }
  const int lz = clz128(sig);
  return round_pack(x.sign, x.exp + 1 - lz, sig << lz, env);
}

// Integer square root of a value in [2^126, 2^128).
u64 isqrt(u128 r, u128& rem) {
  const double est = std::sqrt((double)r);
  u64 x = est >= 18446744073709549568.0 ? ~0ull : (u64)est;
  for (int i = 0; i < 2; i++) {
    const u128 avg = ((u128)x + r / x) >> 1;
    x = avg > ~0ull ? ~0ull : (u64)avg;
  }
  while ((u128)x * x > r) x--;
  while (x != ~0ull && (u128)(x + 1) * (x + 1) <= r) x++;
  rem = r - (u128)x * x;
  return x;
}

// Splits a finite value into an integer part and a 64-bit fraction:
// |a| * 2^64, with everything below folded into bit 0.
u128 fixed_point(const Unpacked& u) { return shr_jam((u128)u.man << 64, 63 - u.exp); }

}  // namespace

F80 add(F80 a, F80 b, Env env) { return add_impl(a, b, false, env); }
F80 sub(F80 a, F80 b, Env env) { return add_impl(a, b, true, env); }

F80 mul(F80 a, F80 b, Env env) {
  if (is_nan(a) || is_nan(b)) return propagate_nan(a, b);
  const bool sgn = sign(a) != sign(b);
  if (is_inf(a) || is_inf(b)) {
    if (is_zero(a) || is_zero(b)) return indefinite();
    return infinity(sgn);
  }
  if (is_zero(a) || is_zero(b)) return zero(sgn);
  const Unpacked x = unpack(a), y = unpack(b);
  const u128 sig = (u128)x.man * y.man;  // in [2^126, 2^128)
  const int lz = clz128(sig);
  return round_pack(sgn, x.exp + y.exp + 1 - lz, sig << lz, env);
}

F80 div(F80 a, F80 b, Env env) {
  if (is_nan(a) || is_nan(b)) return propagate_nan(a, b);
  const bool sgn = sign(a) != sign(b);
  if (is_inf(a)) return is_inf(b) ? indefinite() : infinity(sgn);
  if (is_inf(b)) return zero(sgn);
  if (is_zero(b)) return is_zero(a) ? indefinite() : infinity(sgn);
  if (is_zero(a)) return zero(sgn);

  const Unpacked x = unpack(a), y = unpack(b);
  s32 exp = x.exp - y.exp;
  u128 num;
  if (x.man >= y.man) {
    num = (u128)x.man << 63;
  } else {
    num = (u128)x.man << 64;
    exp--;
  }
  // Two rounds of long division give 128 quotient bits; the top one is set.
  const u64 q1 = (u64)(num / y.man);
  const u128 n2 = (u128)(u64)(num % y.man) << 64;
  const u64 q2 = (u64)(n2 / y.man);
  const bool inexact = (n2 % y.man) != 0;
  return round_pack(sgn, exp, ((u128)q1 << 64) | q2 | (inexact ? 1 : 0), env);
}

F80 sqrt(F80 a, Env env) {
  if (is_nan(a)) return quiet(a);
  if (is_zero(a)) return a;
  if (sign(a)) return indefinite();
  if (is_inf(a)) return a;
  const Unpacked x = unpack(a);
  // Make the exponent even so it can be halved.
  const bool odd = (x.exp & 1) != 0;
  const u128 radicand = (u128)x.man << (odd ? 64 : 63);
  u128 rem;
  const u64 root = isqrt(radicand, rem);
  // The true root is past the halfway point exactly when rem > root, and it
  // can never sit on the halfway point, so one guard bit plus sticky is exact.
  const u128 sig = ((u128)root << 64) | ((u128)(rem > root ? 1 : 0) << 63) | (rem != 0 ? 1 : 0);
  return round_pack(false, (x.exp - (odd ? 1 : 0)) / 2, sig, env);
}

F80 round_to_int(F80 a, int rc) {
  if (is_nan(a)) return quiet(a);
  if (is_inf(a) || is_zero(a)) return a;
  const Unpacked u = unpack(a);
  if (u.exp >= 63) return a;  // already integral
  const u128 fp = fixed_point(u);
  u64 ipart = (u64)(fp >> 64);
  const u64 frac = (u64)fp;
  if (round_up(rc, u.sign, (ipart & 1) != 0, frac, (u128)kIntBit)) ipart++;
  if (!ipart) return zero(u.sign);
  const int lz = clz64(ipart);
  return make(u.sign, 16383 + 63 - lz, ipart << lz);
}

F80 scale(F80 a, F80 b, Env env) {
  if (is_nan(a) || is_nan(b)) return propagate_nan(a, b);
  if (is_inf(b)) {
    if (sign(b)) return is_inf(a) ? indefinite() : zero(sign(a));
    return is_zero(a) ? indefinite() : infinity(sign(a));
  }
  if (is_inf(a) || is_zero(a) || is_zero(b)) return a;
  bool invalid;
  s64 n = to_int(b, kTruncate, 32, invalid);
  if (invalid) n = sign(b) ? -100000 : 100000;
  if (n > 100000) n = 100000;
  if (n < -100000) n = -100000;
  const Unpacked x = unpack(a);
  return round_pack(x.sign, x.exp + (s32)n, (u128)x.man << 64, env);
}

F80 remainder(F80 a, F80 b, bool nearest, u32& quotient) {
  quotient = 0;
  if (is_nan(a) || is_nan(b)) return propagate_nan(a, b);
  if (is_inf(a) || is_zero(b)) return indefinite();
  if (is_inf(b) || is_zero(a)) return a;

  const Unpacked x = unpack(a), y = unpack(b);
  const s32 d = x.exp - y.exp;
  const Env exact{kNearest, 64};
  if (d < 0) {
    // |a| < |b|, so the truncated quotient is zero. Rounding to nearest
    // still picks 1 when |a| is more than half of |b|.
    if (nearest && d == -1 && x.man > y.man) {
      quotient = 1;
      const u128 diff = ((u128)y.man << 1) - x.man;  // (|b| - |a|) at |a|'s scale
      const int lz = clz128(diff);
      return round_pack(!x.sign, x.exp + 64 - lz, diff << lz, exact);
    }
    return a;
  }

  // Long division of x.man * 2^d by y.man, at most 63 bits at a time.
  u64 q = x.man / y.man;
  u64 r = x.man % y.man;
  for (s32 left = d; left > 0;) {
    const int k = left > 63 ? 63 : left;
    const u128 t = (u128)r << k;
    q = (q << k) | (u64)(t / y.man);
    r = (u64)(t % y.man);
    left -= k;
  }
  bool sgn = x.sign;
  if (nearest) {
    const u128 twice = (u128)r << 1;
    if (twice > y.man || (twice == y.man && (q & 1))) {
      r = y.man - r;
      sgn = !sgn;
      q++;
    }
  }
  quotient = (u32)q;
  if (!r) return zero(x.sign);
  const int lz = clz64(r);
  return round_pack(sgn, y.exp - lz, (u128)(r << lz) << 64, exact);
}

Order compare(F80 a, F80 b) {
  if (is_nan(a) || is_nan(b)) return kUnordered;
  if (is_zero(a) && is_zero(b)) return kEqual;
  const bool sa = sign(a), sb = sign(b);
  if (sa != sb) return sa ? kLess : kGreater;
  const int ea = biased_exp(a) ? biased_exp(a) : 1, eb = biased_exp(b) ? biased_exp(b) : 1;
  int c = 0;
  if (ea != eb) {
    c = ea < eb ? -1 : 1;
  } else if (a.man != b.man) {
    c = a.man < b.man ? -1 : 1;
  }
  if (c == 0) return kEqual;
  return ((c < 0) != sa) ? kLess : kGreater;
}

F80 from_double_bits(u64 bits) {
  const bool s = (bits >> 63) != 0;
  const int e = (int)(bits >> 52) & 0x7FF;
  const u64 f = bits & 0x000FFFFFFFFFFFFFull;
  if (e == 0x7FF) return make(s, 0x7FFF, kIntBit | (f << 11) | (f ? 0x4000000000000000ull : 0));
  if (e == 0) {
    if (!f) return zero(s);
    const int lz = clz64(f);
    return make(s, 16383 - 1011 - lz, f << lz);
  }
  return make(s, e - 1023 + 16383, kIntBit | (f << 11));
}

F80 from_float_bits(u32 bits) {
  const bool s = (bits >> 31) != 0;
  const int e = (int)(bits >> 23) & 0xFF;
  const u64 f = bits & 0x007FFFFFu;
  if (e == 0xFF) return make(s, 0x7FFF, kIntBit | (f << 40) | (f ? 0x4000000000000000ull : 0));
  if (e == 0) {
    if (!f) return zero(s);
    const int lz = clz64(f);
    // f / 2^23 * 2^-126 is (f << lz) / 2^63 * 2^(-86 - lz).
    return make(s, 16383 - 86 - lz, f << lz);
  }
  return make(s, e - 127 + 16383, kIntBit | (f << 40));
}

F80 from_double(double d) {
  u64 bits;
  memcpy(&bits, &d, 8);
  return from_double_bits(bits);
}

F80 from_i64(s64 v) {
  if (!v) return zero();
  const u64 mag = v < 0 ? 0 - (u64)v : (u64)v;
  const int lz = clz64(mag);
  return make(v < 0, 16383 + 63 - lz, mag << lz);
}

u64 to_double_bits(F80 a, int rc) {
  const u64 s = (u64)sign(a) << 63;
  if (is_nan(a)) return s | 0x7FF8000000000000ull | ((a.man >> 11) & 0x000FFFFFFFFFFFFFull);
  if (is_inf(a)) return s | 0x7FF0000000000000ull;
  if (is_zero(a)) return s;
  const Unpacked u = unpack(a);
  return s | pack_small(u.sign, u.exp, (u128)u.man << 64, 53, 1023, rc);
}

u32 to_float_bits(F80 a, int rc) {
  const u32 s = (u32)sign(a) << 31;
  if (is_nan(a)) return s | 0x7FC00000u | (u32)((a.man >> 40) & 0x007FFFFFu);
  if (is_inf(a)) return s | 0x7F800000u;
  if (is_zero(a)) return s;
  const Unpacked u = unpack(a);
  return s | (u32)pack_small(u.sign, u.exp, (u128)u.man << 64, 24, 127, rc);
}

double to_double(F80 a) {
  const u64 bits = to_double_bits(a, kNearest);
  double d;
  memcpy(&d, &bits, 8);
  return d;
}

s64 to_int(F80 a, int rc, int bits, bool& invalid) {
  const s64 indefinite_int = bits == 64 ? INT64_MIN : (bits == 32 ? (s64)INT32_MIN : (s64)INT16_MIN);
  invalid = false;
  if (is_zero(a)) return 0;
  if (is_nan(a) || is_inf(a)) {
    invalid = true;
    return indefinite_int;
  }
  const Unpacked u = unpack(a);
  if (u.exp > 63) {
    invalid = true;
    return indefinite_int;
  }
  const u128 fp = fixed_point(u);
  u128 ipart = fp >> 64;
  const u64 frac = (u64)fp;
  if (round_up(rc, u.sign, (ipart & 1) != 0, frac, (u128)kIntBit)) ipart++;
  const u128 limit = (u128)1 << (bits - 1);
  if (u.sign ? ipart > limit : ipart >= limit) {
    invalid = true;
    return indefinite_int;
  }
  return u.sign ? (s64)(0 - (u64)ipart) : (s64)(u64)ipart;
}

}  // namespace f80
