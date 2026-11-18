// The x87 floating point unit.
//
// Arithmetic is exact 80-bit software floating point (f80.cpp). The
// transcendental instructions (fsin, fyl2x and friends) go through the host's
// double-precision maths library, so they are accurate to about 53 bits
// rather than the 64 a real x87 gives.
//
// Exceptions are always treated as masked, which is how Mac OS X runs. The
// exception flags in the status word are not maintained, apart from "invalid"
// on a failed integer store.

#include <cmath>

#include "cpu/x86/x86.h"

namespace {

using f80::Env;

enum : u16 {
  C0 = 0x0100,
  C1 = 0x0200,
  C2 = 0x0400,
  C3 = 0x4000,
  kCondition = C0 | C1 | C2 | C3,
};

// ---- guest memory (little-endian) -------------------------------------------

inline u16 rd16(const X86State& s, u32 a) {
  u16 v;
  memcpy(&v, s.mem + a, 2);
  return v;
}
inline u32 rd32(const X86State& s, u32 a) {
  u32 v;
  memcpy(&v, s.mem + a, 4);
  return v;
}
inline u64 rd64(const X86State& s, u32 a) {
  u64 v;
  memcpy(&v, s.mem + a, 8);
  return v;
}
inline F80 rd80(const X86State& s, u32 a) {
  F80 v;
  memcpy(&v.man, s.mem + a, 8);
  memcpy(&v.se, s.mem + a + 8, 2);
  return v;
}
inline void wr16(const X86State& s, u32 a, u16 v) { memcpy(s.mem + a, &v, 2); }
inline void wr32(const X86State& s, u32 a, u32 v) { memcpy(s.mem + a, &v, 4); }
inline void wr64(const X86State& s, u32 a, u64 v) { memcpy(s.mem + a, &v, 8); }
inline void wr80(const X86State& s, u32 a, F80 v) {
  memcpy(s.mem + a, &v.man, 8);
  memcpy(s.mem + a + 8, &v.se, 2);
}

// ---- the register stack ------------------------------------------------------

inline int top(const X86State& s) { return (s.fsw >> 11) & 7; }
inline void set_top(X86State& s, int t) { s.fsw = (u16)((s.fsw & ~0x3800) | ((t & 7) << 11)); }
inline F80& ST(X86State& s, int i) { return s.fpr[(top(s) + i) & 7]; }

inline void push(X86State& s, F80 v) {
  const int t = (top(s) - 1) & 7;
  set_top(s, t);
  s.fpr[t] = v;
  s.ftag |= (u8)(1 << t);
}

inline void pop(X86State& s) {
  const int t = top(s);
  s.ftag &= (u8)~(1 << t);
  set_top(s, t + 1);
}

inline Env fp_env(const X86State& s) {
  static const int kPrecision[4] = {24, 64, 53, 64};
  return Env{(s.fcw >> 10) & 3, kPrecision[(s.fcw >> 8) & 3]};
}
inline int rounding(const X86State& s) { return (s.fcw >> 10) & 3; }

// ---- helpers -----------------------------------------------------------------

// The six arithmetic operations, numbered as the ModRM reg field encodes
// them: 0 add, 1 mul, 4 a-b, 5 b-a, 6 a/b, 7 b/a.
F80 arith(int kind, F80 a, F80 b, Env env) {
  switch (kind) {
    case 0: return f80::add(a, b, env);
    case 1: return f80::mul(a, b, env);
    case 4: return f80::sub(a, b, env);
    case 5: return f80::sub(b, a, env);
    case 6: return f80::div(a, b, env);
    default: return f80::div(b, a, env);
  }
}

// Compare result into the status word, as fcom leaves it.
void set_compare(X86State& s, f80::Order o) {
  s.fsw &= ~(C0 | C2 | C3);
  switch (o) {
    case f80::kLess: s.fsw |= C0; break;
    case f80::kEqual: s.fsw |= C3; break;
    case f80::kGreater: break;
    case f80::kUnordered: s.fsw |= C0 | C2 | C3; break;
  }
}

// Compare result into EFLAGS, as fcomi leaves it (ZF, PF, CF).
void set_compare_eflags(X86State& s, f80::Order o) {
  u32 f = s.eflags & ~0x8D5u;  // clears OF SF ZF AF PF CF
  switch (o) {
    case f80::kLess: f |= 0x01; break;
    case f80::kEqual: f |= 0x40; break;
    case f80::kGreater: break;
    case f80::kUnordered: f |= 0x45; break;
  }
  s.eflags = f;
}

void store_int(X86State& s, u32 ea, int bits, int rc) {
  bool invalid;
  const s64 v = f80::to_int(ST(s, 0), rc, bits, invalid);
  if (invalid) s.fsw |= 1;
  if (bits == 16) {
    wr16(s, ea, (u16)v);
  } else if (bits == 32) {
    wr32(s, ea, (u32)v);
  } else {
    wr64(s, ea, (u64)v);
  }
}

// The full tag word of fnstenv: two bits per physical register.
u16 tag_word(const X86State& s) {
  u16 tw = 0;
  for (int i = 0; i < 8; i++) {
    int tag;
    const F80 v = s.fpr[i];
    if (!(s.ftag & (1 << i))) {
      tag = 3;  // empty
    } else if (f80::is_zero(v)) {
      tag = 1;
    } else if (f80::biased_exp(v) == 0x7FFF || f80::biased_exp(v) == 0 || !(v.man >> 63)) {
      tag = 2;  // NaN, infinity, denormal
    } else {
      tag = 0;
    }
    tw |= (u16)(tag << (2 * i));
  }
  return tw;
}

void store_env(const X86State& s, u32 ea) {
  wr32(s, ea, 0xFFFF0000u | s.fcw);
  wr32(s, ea + 4, 0xFFFF0000u | s.fsw);
  wr32(s, ea + 8, 0xFFFF0000u | tag_word(s));
  wr32(s, ea + 12, 0);  // instruction and operand pointers are not tracked
  wr32(s, ea + 16, 0);
  wr32(s, ea + 20, 0);
  wr32(s, ea + 24, 0xFFFF0000u);
}

void load_env(X86State& s, u32 ea) {
  s.fcw = rd16(s, ea);
  s.fsw = rd16(s, ea + 4);
  const u16 tw = rd16(s, ea + 8);
  s.ftag = 0;
  for (int i = 0; i < 8; i++) {
    if (((tw >> (2 * i)) & 3) != 3) s.ftag |= (u8)(1 << i);
  }
}

void reset(X86State& s) {
  s.fcw = 0x037F;
  s.fsw = 0;
  s.ftag = 0;
}

// Transcendentals, through host doubles.
inline F80 via_double(double d) { return f80::from_double(d); }

void load_bcd(X86State& s, u32 ea) {
  u8 bcd[10];
  memcpy(bcd, s.mem + ea, 10);
  u64 v = 0;
  for (int i = 8; i >= 0; i--) v = v * 100 + (u64)(bcd[i] >> 4) * 10 + (bcd[i] & 15);
  const F80 r = f80::from_i64((s64)v);
  push(s, (bcd[9] & 0x80) ? f80::negate(r) : r);
}

void store_bcd(X86State& s, u32 ea) {
  bool invalid;
  const F80 st0 = ST(s, 0);
  const s64 v = f80::to_int(st0, rounding(s), 64, invalid);
  u64 mag = v < 0 ? 0 - (u64)v : (u64)v;
  u8 bcd[10] = {0, 0, 0, 0, 0, 0, 0, 0xC0, 0xFF, 0xFF};  // the "BCD indefinite"
  if (!invalid && mag < 1000000000000000000ull) {
    for (int i = 0; i < 9; i++) {
      bcd[i] = (u8)((mag % 10) | ((mag / 10 % 10) << 4));
      mag /= 100;
    }
    bcd[9] = f80::sign(st0) ? 0x80 : 0;
  } else {
    s.fsw |= 1;
  }
  memcpy(s.mem + ea, bcd, 10);
}

}  // namespace

void x87_fxsave(const X86State& s, u32 ea) {
  u8 area[512] = {};
  memcpy(area + 0, &s.fcw, 2);
  memcpy(area + 2, &s.fsw, 2);
  area[4] = s.ftag;
  memcpy(area + 24, &s.mxcsr, 4);
  const u32 mxcsr_mask = 0x0000FFBF;
  memcpy(area + 28, &mxcsr_mask, 4);
  for (int i = 0; i < 8; i++) {
    const F80 v = s.fpr[(top(s) + i) & 7];
    memcpy(area + 32 + 16 * i, &v.man, 8);
    memcpy(area + 40 + 16 * i, &v.se, 2);
    memcpy(area + 160 + 16 * i, s.xmm[i].b, 16);
  }
  memcpy(s.mem + ea, area, 512);
}

void x87_fxrstor(X86State& s, u32 ea) {
  const u8* area = s.mem + ea;
  memcpy(&s.fcw, area + 0, 2);
  memcpy(&s.fsw, area + 2, 2);
  s.ftag = area[4];
  memcpy(&s.mxcsr, area + 24, 4);
  s.mxcsr &= 0x0000FFBF;
  for (int i = 0; i < 8; i++) {
    F80& v = s.fpr[(top(s) + i) & 7];
    memcpy(&v.man, area + 32 + 16 * i, 8);
    memcpy(&v.se, area + 40 + 16 * i, 2);
    memcpy(s.xmm[i].b, area + 160 + 16 * i, 16);
  }
  sse_apply_rounding(s.mxcsr);
}

bool x87_exec(X86State& s, u8 op, u8 modrm, u32 ea) {
  const int mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
  const Env env = fp_env(s);
  const int rc = rounding(s);

  // ---- memory operand forms ----
  if (mod != 3) {
    switch (op) {
      case 0xD8:  // arithmetic with a 32-bit float
      case 0xDA:  // ... a 32-bit integer
      case 0xDC:  // ... a 64-bit float
      case 0xDE: {  // ... a 16-bit integer
        F80 src;
        if (op == 0xD8) {
          src = f80::from_float_bits(rd32(s, ea));
        } else if (op == 0xDA) {
          src = f80::from_i64((s32)rd32(s, ea));
        } else if (op == 0xDC) {
          src = f80::from_double_bits(rd64(s, ea));
        } else {
          src = f80::from_i64((s16)rd16(s, ea));
        }
        if (reg == 2 || reg == 3) {
          set_compare(s, f80::compare(ST(s, 0), src));
          if (reg == 3) pop(s);
        } else {
          ST(s, 0) = arith(reg, ST(s, 0), src, env);
        }
        return true;
      }

      case 0xD9:
        switch (reg) {
          case 0: push(s, f80::from_float_bits(rd32(s, ea))); return true;       // fld m32
          case 2: wr32(s, ea, f80::to_float_bits(ST(s, 0), rc)); return true;    // fst m32
          case 3: wr32(s, ea, f80::to_float_bits(ST(s, 0), rc)); pop(s); return true;
          case 4: load_env(s, ea); return true;                                  // fldenv
          case 5: s.fcw = rd16(s, ea); return true;                              // fldcw
          case 6: store_env(s, ea); s.fcw |= 0x3F; return true;                  // fnstenv
          case 7: wr16(s, ea, s.fcw); return true;                               // fnstcw
          default: return false;
        }

      case 0xDB:
        switch (reg) {
          case 0: push(s, f80::from_i64((s32)rd32(s, ea))); return true;         // fild m32
          case 1: store_int(s, ea, 32, f80::kTruncate); pop(s); return true;     // fisttp m32
          case 2: store_int(s, ea, 32, rc); return true;                         // fist m32
          case 3: store_int(s, ea, 32, rc); pop(s); return true;                 // fistp m32
          case 5: push(s, rd80(s, ea)); return true;                             // fld m80
          case 7: wr80(s, ea, ST(s, 0)); pop(s); return true;                    // fstp m80
          default: return false;
        }

      case 0xDD:
        switch (reg) {
          case 0: push(s, f80::from_double_bits(rd64(s, ea))); return true;      // fld m64
          case 1: store_int(s, ea, 64, f80::kTruncate); pop(s); return true;     // fisttp m64
          case 2: wr64(s, ea, f80::to_double_bits(ST(s, 0), rc)); return true;   // fst m64
          case 3: wr64(s, ea, f80::to_double_bits(ST(s, 0), rc)); pop(s); return true;
          case 4:  // frstor
            load_env(s, ea);
            for (int i = 0; i < 8; i++) ST(s, i) = rd80(s, ea + 28 + 10 * (u32)i);
            return true;
          case 6:  // fnsave
            store_env(s, ea);
            for (int i = 0; i < 8; i++) wr80(s, ea + 28 + 10 * (u32)i, ST(s, i));
            reset(s);
            return true;
          case 7: wr16(s, ea, s.fsw); return true;                               // fnstsw m16
          default: return false;
        }

      case 0xDF:
        switch (reg) {
          case 0: push(s, f80::from_i64((s16)rd16(s, ea))); return true;         // fild m16
          case 1: store_int(s, ea, 16, f80::kTruncate); pop(s); return true;     // fisttp m16
          case 2: store_int(s, ea, 16, rc); return true;                         // fist m16
          case 3: store_int(s, ea, 16, rc); pop(s); return true;                 // fistp m16
          case 4: load_bcd(s, ea); return true;                                  // fbld
          case 5: push(s, f80::from_i64((s64)rd64(s, ea))); return true;         // fild m64
          case 6: store_bcd(s, ea); pop(s); return true;                         // fbstp
          default: store_int(s, ea, 64, rc); pop(s); return true;                // fistp m64
        }
    }
    return false;
  }

  // ---- register forms ----
  F80& st0 = ST(s, 0);
  F80& sti = ST(s, rm);
  switch (op) {
    case 0xD8:  // op st0, sti
      if (reg == 2 || reg == 3) {
        set_compare(s, f80::compare(st0, sti));
        if (reg == 3) pop(s);
      } else {
        st0 = arith(reg, st0, sti, env);
      }
      return true;

    case 0xDC:  // op sti, st0
    case 0xDE: {  // ... and pop
      if (op == 0xDE && modrm == 0xD9) {  // fcompp
        set_compare(s, f80::compare(st0, ST(s, 1)));
        pop(s);
        pop(s);
        return true;
      }
      if (reg == 2 || reg == 3) return false;
      // With the destination swapped, so are the reversed forms: reg 4 is
      // st0 - sti and reg 5 is sti - st0 (likewise 6 and 7 for divide).
      sti = arith(reg >= 4 ? reg ^ 1 : reg, sti, st0, env);
      if (op == 0xDE) pop(s);
      return true;
    }

    case 0xD9:
      if (reg == 0) {  // fld sti
        const F80 v = sti;
        push(s, v);
        return true;
      }
      if (reg == 1) {  // fxch
        const F80 t = st0;
        st0 = sti;
        sti = t;
        return true;
      }
      switch (modrm) {
        case 0xD0: return true;  // fnop
        case 0xE0: st0 = f80::negate(st0); return true;  // fchs
        case 0xE1: st0 = f80::abs(st0); return true;     // fabs
        case 0xE4: set_compare(s, f80::compare(st0, f80::zero())); return true;  // ftst
        case 0xE5: {  // fxam
          u16 c;
          if (!(s.ftag & (1 << top(s)))) {
            c = C3 | C0;
          } else if (f80::is_nan(st0)) {
            c = C0;
          } else if (f80::is_inf(st0)) {
            c = C2 | C0;
          } else if (f80::is_zero(st0)) {
            c = C3;
          } else if (f80::is_denormal(st0)) {
            c = C3 | C2;
          } else {
            c = C2;
          }
          if (f80::sign(st0)) c |= C1;
          s.fsw = (u16)((s.fsw & ~kCondition) | c);
          return true;
        }
        case 0xE8: push(s, f80::one()); return true;                                  // fld1
        case 0xE9: push(s, f80::make(false, 0x4000, 0xD49A784BCD1B8AFEull)); return true;  // fldl2t
        case 0xEA: push(s, f80::make(false, 0x3FFF, 0xB8AA3B295C17F0BCull)); return true;  // fldl2e
        case 0xEB: push(s, f80::make(false, 0x4000, 0xC90FDAA22168C235ull)); return true;  // fldpi
        case 0xEC: push(s, f80::make(false, 0x3FFD, 0x9A209A84FBCFF799ull)); return true;  // fldlg2
        case 0xED: push(s, f80::make(false, 0x3FFE, 0xB17217F7D1CF79ACull)); return true;  // fldln2
        case 0xEE: push(s, f80::zero()); return true;                                 // fldz
        case 0xF0:  // f2xm1
          st0 = via_double(std::expm1(f80::to_double(st0) * 0.6931471805599453));
          return true;
        case 0xF1:  // fyl2x
          ST(s, 1) = via_double(f80::to_double(ST(s, 1)) * std::log2(f80::to_double(st0)));
          pop(s);
          return true;
        case 0xF2:  // fptan
          st0 = via_double(std::tan(f80::to_double(st0)));
          push(s, f80::one());
          s.fsw &= ~C2;
          return true;
        case 0xF3:  // fpatan
          ST(s, 1) = via_double(std::atan2(f80::to_double(ST(s, 1)), f80::to_double(st0)));
          pop(s);
          return true;
        case 0xF4: {  // fxtract
          const F80 v = st0;
          if (f80::is_nan(v)) {
            push(s, v);
          } else if (f80::is_inf(v)) {
            st0 = f80::infinity(false);
            push(s, v);
          } else if (f80::is_zero(v)) {
            st0 = f80::infinity(true);
            push(s, v);
          } else {
            const int lz = __builtin_clzll(v.man);
            const int e = (f80::biased_exp(v) ? f80::biased_exp(v) : 1) - 16383 - lz;
            st0 = f80::from_i64(e);
            push(s, f80::make(f80::sign(v), 16383, v.man << lz));
          }
          return true;
        }
        case 0xF5:
        case 0xF8: {  // fprem1, fprem
          u32 q;
          st0 = f80::remainder(st0, ST(s, 1), modrm == 0xF5, q);
          u16 c = 0;
          if (q & 4) c |= C0;
          if (q & 2) c |= C3;
          if (q & 1) c |= C1;
          s.fsw = (u16)((s.fsw & ~kCondition) | c);  // C2 clear: reduction complete
          return true;
        }
        case 0xF6: set_top(s, top(s) - 1); return true;  // fdecstp
        case 0xF7: set_top(s, top(s) + 1); return true;  // fincstp
        case 0xF9:  // fyl2xp1
          ST(s, 1) = via_double(f80::to_double(ST(s, 1)) * std::log1p(f80::to_double(st0)) *
                                1.4426950408889634);
          pop(s);
          return true;
        case 0xFA: st0 = f80::sqrt(st0, env); return true;  // fsqrt
        case 0xFB: {  // fsincos
          const double x = f80::to_double(st0);
          st0 = via_double(std::sin(x));
          push(s, via_double(std::cos(x)));
          s.fsw &= ~C2;
          return true;
        }
        case 0xFC: st0 = f80::round_to_int(st0, rc); return true;       // frndint
        case 0xFD: st0 = f80::scale(st0, ST(s, 1), env); return true;   // fscale
        case 0xFE: st0 = via_double(std::sin(f80::to_double(st0))); s.fsw &= ~C2; return true;
        case 0xFF: st0 = via_double(std::cos(f80::to_double(st0))); s.fsw &= ~C2; return true;
        default: return false;
      }

    case 0xDA:
      if (modrm == 0xE9) {  // fucompp
        set_compare(s, f80::compare(st0, ST(s, 1)));
        pop(s);
        pop(s);
        return true;
      }
      if (reg < 4) {  // fcmovb / fcmove / fcmovbe / fcmovu
        static const u32 kFlag[4] = {0x01, 0x40, 0x41, 0x04};
        if (s.eflags & kFlag[reg]) st0 = sti;
        return true;
      }
      return false;

    case 0xDB:
      if (reg < 4) {  // fcmovnb / fcmovne / fcmovnbe / fcmovnu
        static const u32 kFlag[4] = {0x01, 0x40, 0x41, 0x04};
        if (!(s.eflags & kFlag[reg])) st0 = sti;
        return true;
      }
      if (reg == 5 || reg == 6) {  // fucomi, fcomi
        set_compare_eflags(s, f80::compare(st0, sti));
        return true;
      }
      if (modrm == 0xE2) {  // fnclex
        s.fsw &= 0x7F00;
        return true;
      }
      if (modrm == 0xE3) {  // fninit
        reset(s);
        return true;
      }
      return modrm == 0xE0 || modrm == 0xE1 || modrm == 0xE4;  // 8087 leftovers: no-ops

    case 0xDD:
      switch (reg) {
        case 0: s.ftag &= (u8)~(1 << ((top(s) + rm) & 7)); return true;  // ffree
        case 2: sti = st0; return true;                                  // fst sti
        case 3: sti = st0; pop(s); return true;                          // fstp sti
        case 4: set_compare(s, f80::compare(st0, sti)); return true;     // fucom
        case 5: set_compare(s, f80::compare(st0, sti)); pop(s); return true;  // fucomp
        default: return false;
      }

    case 0xDF:
      if (modrm == 0xE0) {  // fnstsw ax
        s.reg[X86State::EAX] = (s.reg[X86State::EAX] & 0xFFFF0000u) | s.fsw;
        return true;
      }
      if (reg == 5 || reg == 6) {  // fucomip, fcomip
        set_compare_eflags(s, f80::compare(st0, sti));
        pop(s);
        return true;
      }
      if (reg == 0) {  // ffreep
        s.ftag &= (u8)~(1 << ((top(s) + rm) & 7));
        pop(s);
        return true;
      }
      return false;
  }
  return false;
}
