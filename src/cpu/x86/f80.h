#pragma once

#include "common.h"

// An x87 extended-precision number: a 64-bit significand with an explicit
// integer bit, a 15-bit exponent biased by 16383, and a sign.
//
// Apple Silicon has no 80-bit floating point, so the x87 core does this
// arithmetic in software. Doing it with doubles instead would silently lose
// bits, most visibly when programs move 64-bit integers through the x87
// stack, which they do.
struct F80 {
  u64 man = 0;
  u16 se = 0;  // sign in bit 15, biased exponent below
};

namespace f80 {

// Rounding modes, numbered as the x87 control word encodes them.
enum Rounding { kNearest = 0, kDown = 1, kUp = 2, kTruncate = 3 };

struct Env {
  int rc = kNearest;
  int prec = 64;  // significand bits a result is rounded to: 24, 53 or 64
};

enum Order { kLess, kEqual, kGreater, kUnordered };

inline bool sign(F80 a) { return (a.se >> 15) != 0; }
inline int biased_exp(F80 a) { return a.se & 0x7FFF; }
inline bool is_nan(F80 a) { return biased_exp(a) == 0x7FFF && (a.man << 1) != 0; }
inline bool is_inf(F80 a) { return biased_exp(a) == 0x7FFF && (a.man << 1) == 0; }
inline bool is_zero(F80 a) { return biased_exp(a) == 0 && a.man == 0; }
inline bool is_denormal(F80 a) { return biased_exp(a) == 0 && a.man != 0; }

inline F80 make(bool s, int e, u64 m) { return F80{m, (u16)((s ? 0x8000 : 0) | e)}; }
inline F80 zero(bool s = false) { return make(s, 0, 0); }
inline F80 one() { return make(false, 16383, 0x8000000000000000ull); }
inline F80 infinity(bool s) { return make(s, 0x7FFF, 0x8000000000000000ull); }
// The NaN the x87 produces for an invalid operation.
inline F80 indefinite() { return make(true, 0x7FFF, 0xC000000000000000ull); }
inline F80 negate(F80 a) {
  a.se ^= 0x8000;
  return a;
}
inline F80 abs(F80 a) {
  a.se &= 0x7FFF;
  return a;
}

F80 add(F80 a, F80 b, Env env);
F80 sub(F80 a, F80 b, Env env);
F80 mul(F80 a, F80 b, Env env);
F80 div(F80 a, F80 b, Env env);
F80 sqrt(F80 a, Env env);
// Rounds to an integral value (frndint).
F80 round_to_int(F80 a, int rc);
// a * 2^trunc(b) (fscale).
F80 scale(F80 a, F80 b, Env env);
// a - q*b, with q = a/b truncated (fprem) or rounded to nearest (fprem1).
// `quotient` receives q's low bits.
F80 remainder(F80 a, F80 b, bool nearest, u32& quotient);
Order compare(F80 a, F80 b);

F80 from_double_bits(u64 bits);
F80 from_float_bits(u32 bits);
F80 from_double(double d);
F80 from_i64(s64 v);
u64 to_double_bits(F80 a, int rc);
u32 to_float_bits(F80 a, int rc);
double to_double(F80 a);  // round to nearest
// Converts to a signed integer of `bits` bits (16, 32 or 64). On NaN or
// overflow it sets `invalid` and returns the "integer indefinite" value.
s64 to_int(F80 a, int rc, int bits, bool& invalid);

}  // namespace f80
