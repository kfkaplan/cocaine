// MMX, SSE, SSE2 and SSE3.
//
// Floating point runs on host floating point, which is IEEE like the guest's.
// Two things differ on a real Intel CPU and are reproduced by hand: the NaN a
// failed operation produces (negative, where ARM's is positive) and which
// operand's NaN survives; and the "integer indefinite" 0x80000000 that a
// float-to-integer conversion returns when the value doesn't fit.
//
// Not modelled: the MXCSR exception flags, and its flush-to-zero and
// denormals-are-zero modes.

#include "cpu/rounding.h"
#include <cfenv>
#include <cmath>

#include "cpu/x86/x86.h"

namespace {

typedef X86State::Xmm Xmm;

// Sixteen bytes viewed as lanes of each width. MMX uses the first eight.
union Vec {
  u8 b[16];
  s8 sb[16];
  u16 w[8];
  s16 sw[8];
  u32 d[4];
  s32 sd[4];
  u64 q[2];
};

constexpr u32 kNaN32 = 0xFFC00000u;
constexpr u64 kNaN64 = 0xFFF8000000000000ull;
constexpr u32 kQuiet32 = 0x00400000u;
constexpr u64 kQuiet64 = 0x0008000000000000ull;

inline float f32(u32 bits) {
  float f;
  memcpy(&f, &bits, 4);
  return f;
}
inline u32 bits32(float f) {
  u32 b;
  memcpy(&b, &f, 4);
  return b;
}
inline double f64(u64 bits) {
  double d;
  memcpy(&d, &bits, 8);
  return d;
}
inline u64 bits64(double d) {
  u64 b;
  memcpy(&b, &d, 8);
  return b;
}
inline bool is_nan32(u32 b) { return (b & 0x7FFFFFFFu) > 0x7F800000u; }
inline bool is_nan64(u64 b) { return (b & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull; }

// Arithmetic selected by the opcode byte: 51 sqrt, 58 add, 59 mul, 5C sub,
// 5D min, 5E div, 5F max. `a` is the destination operand, `b` the source.
u32 arith32(u8 op, u32 a, u32 b) {
  const float x = f32(a), y = f32(b);
  float r;
  switch (op) {
    case 0x58: r = x + y; break;
    case 0x59: r = x * y; break;
    case 0x5C: r = x - y; break;
    case 0x5E: r = x / y; break;
    // min and max return the source unless the comparison is strictly true,
    // which also covers NaNs and equal zeros.
    case 0x5D: return x < y ? a : b;
    case 0x5F: return x > y ? a : b;
    default:
      r = std::sqrt(y);
      if (r != r) return is_nan32(b) ? (b | kQuiet32) : kNaN32;
      return bits32(r);
  }
  if (r != r) {
    if (is_nan32(a)) return a | kQuiet32;
    if (is_nan32(b)) return b | kQuiet32;
    return kNaN32;
  }
  return bits32(r);
}

u64 arith64(u8 op, u64 a, u64 b) {
  const double x = f64(a), y = f64(b);
  double r;
  switch (op) {
    case 0x58: r = x + y; break;
    case 0x59: r = x * y; break;
    case 0x5C: r = x - y; break;
    case 0x5E: r = x / y; break;
    case 0x5D: return x < y ? a : b;
    case 0x5F: return x > y ? a : b;
    default:
      r = std::sqrt(y);
      if (r != r) return is_nan64(b) ? (b | kQuiet64) : kNaN64;
      return bits64(r);
  }
  if (r != r) {
    if (is_nan64(a)) return a | kQuiet64;
    if (is_nan64(b)) return b | kQuiet64;
    return kNaN64;
  }
  return bits64(r);
}

// The eight compare predicates of cmpps and friends.
bool compare(u32 pred, double x, double y) {
  const bool unordered = x != x || y != y;
  switch (pred & 7) {
    case 0: return x == y;
    case 1: return x < y;
    case 2: return x <= y;
    case 3: return unordered;
    case 4: return !(x == y);
    case 5: return !(x < y);
    case 6: return !(x <= y);
    default: return !unordered;
  }
}

// Float to 32-bit integer, with Intel's answer when it doesn't fit.
s32 to_s32(double v, bool truncate) {
  if (v != v) return INT32_MIN;
  const double r = truncate ? std::trunc(v) : std::nearbyint(v);
  if (r >= 2147483648.0 || r < -2147483648.0) return INT32_MIN;
  return (s32)r;
}

// Widening and narrowing keep a NaN's payload but quiet it.
u64 widen(u32 a) {
  if (is_nan32(a)) {
    return ((u64)(a & 0x80000000u) << 32) | 0x7FF8000000000000ull | ((u64)(a & 0x003FFFFFu) << 29);
  }
  return bits64((double)f32(a));
}
u32 narrow(u64 a) {
  if (is_nan64(a)) {
    return (u32)((a >> 32) & 0x80000000u) | 0x7FC00000u | (u32)((a >> 29) & 0x003FFFFFu);
  }
  return bits32((float)f64(a));
}

inline u8 sat_u8(s32 v) { return v < 0 ? 0 : (v > 255 ? 255 : (u8)v); }
inline u8 sat_s8(s32 v) { return (u8)(s8)(v < -128 ? -128 : (v > 127 ? 127 : v)); }
inline u16 sat_u16(s32 v) { return v < 0 ? 0 : (v > 65535 ? 65535 : (u16)v); }
inline u16 sat_s16(s32 v) { return (u16)(s16)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v)); }

// Shifts every lane of the first `n` bytes. kind: 0 logical right,
// 1 arithmetic right, 2 left. Counts past the lane width clear the lane (or
// fill it with the sign, for arithmetic shifts).
void shift_lanes(Vec& v, int kind, int lane_bits, u64 count, int n) {
  const bool over = count >= (u64)lane_bits;
  const int c = over ? lane_bits - 1 : (int)count;
  for (int i = 0; i < n * 8 / lane_bits; i++) {
    if (lane_bits == 16) {
      if (kind == 1) {
        v.sw[i] = (s16)(v.sw[i] >> c);
      } else if (over) {
        v.w[i] = 0;
      } else {
        v.w[i] = (u16)(kind == 0 ? v.w[i] >> c : v.w[i] << c);
      }
    } else if (lane_bits == 32) {
      if (kind == 1) {
        v.sd[i] = v.sd[i] >> c;
      } else if (over) {
        v.d[i] = 0;
      } else {
        v.d[i] = kind == 0 ? v.d[i] >> c : v.d[i] << c;
      }
    } else {
      if (over) {
        v.q[i] = 0;
      } else {
        v.q[i] = kind == 0 ? v.q[i] >> c : v.q[i] << c;
      }
    }
  }
}

// The big family of packed-integer instructions that exist for both MMX
// (n = 8) and SSE2 (n = 16). Returns false if `op` isn't one of them.
bool packed_int(u8 op, const Vec& a, const Vec& b, Vec& r, int n) {
  const int nb = n, nw = n / 2, nd = n / 4, nq = n / 8;
  r = a;
  switch (op) {
    // -- interleave --
    case 0x60: for (int i = 0; i < nb / 2; i++) { r.b[2 * i] = a.b[i]; r.b[2 * i + 1] = b.b[i]; } break;
    case 0x61: for (int i = 0; i < nw / 2; i++) { r.w[2 * i] = a.w[i]; r.w[2 * i + 1] = b.w[i]; } break;
    case 0x62: for (int i = 0; i < nd / 2; i++) { r.d[2 * i] = a.d[i]; r.d[2 * i + 1] = b.d[i]; } break;
    case 0x68: for (int i = 0; i < nb / 2; i++) { r.b[2 * i] = a.b[nb / 2 + i]; r.b[2 * i + 1] = b.b[nb / 2 + i]; } break;
    case 0x69: for (int i = 0; i < nw / 2; i++) { r.w[2 * i] = a.w[nw / 2 + i]; r.w[2 * i + 1] = b.w[nw / 2 + i]; } break;
    case 0x6A: for (int i = 0; i < nd / 2; i++) { r.d[2 * i] = a.d[nd / 2 + i]; r.d[2 * i + 1] = b.d[nd / 2 + i]; } break;
    case 0x6C:
      if (n != 16) return false;
      r.q[0] = a.q[0];
      r.q[1] = b.q[0];
      break;
    case 0x6D:
      if (n != 16) return false;
      r.q[0] = a.q[1];
      r.q[1] = b.q[1];
      break;

    // -- narrow with saturation --
    case 0x63: for (int i = 0; i < nw; i++) { r.b[i] = sat_s8(a.sw[i]); r.b[nw + i] = sat_s8(b.sw[i]); } break;
    case 0x67: for (int i = 0; i < nw; i++) { r.b[i] = sat_u8(a.sw[i]); r.b[nw + i] = sat_u8(b.sw[i]); } break;
    case 0x6B: for (int i = 0; i < nd; i++) { r.w[i] = sat_s16(a.sd[i]); r.w[nd + i] = sat_s16(b.sd[i]); } break;

    // -- compare --
    case 0x64: for (int i = 0; i < nb; i++) r.b[i] = a.sb[i] > b.sb[i] ? 0xFF : 0; break;
    case 0x65: for (int i = 0; i < nw; i++) r.w[i] = a.sw[i] > b.sw[i] ? 0xFFFF : 0; break;
    case 0x66: for (int i = 0; i < nd; i++) r.d[i] = a.sd[i] > b.sd[i] ? 0xFFFFFFFFu : 0; break;
    case 0x74: for (int i = 0; i < nb; i++) r.b[i] = a.b[i] == b.b[i] ? 0xFF : 0; break;
    case 0x75: for (int i = 0; i < nw; i++) r.w[i] = a.w[i] == b.w[i] ? 0xFFFF : 0; break;
    case 0x76: for (int i = 0; i < nd; i++) r.d[i] = a.d[i] == b.d[i] ? 0xFFFFFFFFu : 0; break;

    // -- shift by a count held in the low 64 bits of the source --
    case 0xD1: shift_lanes(r, 0, 16, b.q[0], n); break;
    case 0xD2: shift_lanes(r, 0, 32, b.q[0], n); break;
    case 0xD3: shift_lanes(r, 0, 64, b.q[0], n); break;
    case 0xE1: shift_lanes(r, 1, 16, b.q[0], n); break;
    case 0xE2: shift_lanes(r, 1, 32, b.q[0], n); break;
    case 0xF1: shift_lanes(r, 2, 16, b.q[0], n); break;
    case 0xF2: shift_lanes(r, 2, 32, b.q[0], n); break;
    case 0xF3: shift_lanes(r, 2, 64, b.q[0], n); break;

    // -- add and subtract, wrapping --
    case 0xFC: for (int i = 0; i < nb; i++) r.b[i] = (u8)(a.b[i] + b.b[i]); break;
    case 0xFD: for (int i = 0; i < nw; i++) r.w[i] = (u16)(a.w[i] + b.w[i]); break;
    case 0xFE: for (int i = 0; i < nd; i++) r.d[i] = a.d[i] + b.d[i]; break;
    case 0xD4: for (int i = 0; i < nq; i++) r.q[i] = a.q[i] + b.q[i]; break;
    case 0xF8: for (int i = 0; i < nb; i++) r.b[i] = (u8)(a.b[i] - b.b[i]); break;
    case 0xF9: for (int i = 0; i < nw; i++) r.w[i] = (u16)(a.w[i] - b.w[i]); break;
    case 0xFA: for (int i = 0; i < nd; i++) r.d[i] = a.d[i] - b.d[i]; break;
    case 0xFB: for (int i = 0; i < nq; i++) r.q[i] = a.q[i] - b.q[i]; break;

    // -- add and subtract, saturating --
    case 0xDC: for (int i = 0; i < nb; i++) r.b[i] = sat_u8(a.b[i] + b.b[i]); break;
    case 0xDD: for (int i = 0; i < nw; i++) r.w[i] = sat_u16(a.w[i] + b.w[i]); break;
    case 0xD8: for (int i = 0; i < nb; i++) r.b[i] = sat_u8(a.b[i] - b.b[i]); break;
    case 0xD9: for (int i = 0; i < nw; i++) r.w[i] = sat_u16(a.w[i] - b.w[i]); break;
    case 0xEC: for (int i = 0; i < nb; i++) r.b[i] = sat_s8(a.sb[i] + b.sb[i]); break;
    case 0xED: for (int i = 0; i < nw; i++) r.w[i] = sat_s16(a.sw[i] + b.sw[i]); break;
    case 0xE8: for (int i = 0; i < nb; i++) r.b[i] = sat_s8(a.sb[i] - b.sb[i]); break;
    case 0xE9: for (int i = 0; i < nw; i++) r.w[i] = sat_s16(a.sw[i] - b.sw[i]); break;

    // -- multiply --
    case 0xD5: for (int i = 0; i < nw; i++) r.w[i] = (u16)(a.sw[i] * b.sw[i]); break;
    case 0xE5: for (int i = 0; i < nw; i++) r.w[i] = (u16)((a.sw[i] * b.sw[i]) >> 16); break;
    case 0xE4: for (int i = 0; i < nw; i++) r.w[i] = (u16)(((u32)a.w[i] * b.w[i]) >> 16); break;
    case 0xF4: for (int i = 0; i < nq; i++) r.q[i] = (u64)a.d[2 * i] * b.d[2 * i]; break;
    case 0xF5:  // pmaddwd
      for (int i = 0; i < nd; i++) {
        r.d[i] = (u32)(a.sw[2 * i] * b.sw[2 * i]) + (u32)(a.sw[2 * i + 1] * b.sw[2 * i + 1]);
      }
      break;
    case 0xF6:  // psadbw: sum of absolute differences per eight bytes
      for (int i = 0; i < nq; i++) {
        u32 sum = 0;
        for (int k = 0; k < 8; k++) {
          const int d = a.b[8 * i + k] - b.b[8 * i + k];
          sum += (u32)(d < 0 ? -d : d);
        }
        r.q[i] = sum;
      }
      break;

    // -- minimum, maximum, average --
    case 0xDA: for (int i = 0; i < nb; i++) r.b[i] = a.b[i] < b.b[i] ? a.b[i] : b.b[i]; break;
    case 0xDE: for (int i = 0; i < nb; i++) r.b[i] = a.b[i] > b.b[i] ? a.b[i] : b.b[i]; break;
    case 0xEA: for (int i = 0; i < nw; i++) r.sw[i] = a.sw[i] < b.sw[i] ? a.sw[i] : b.sw[i]; break;
    case 0xEE: for (int i = 0; i < nw; i++) r.sw[i] = a.sw[i] > b.sw[i] ? a.sw[i] : b.sw[i]; break;
    case 0xE0: for (int i = 0; i < nb; i++) r.b[i] = (u8)((a.b[i] + b.b[i] + 1) >> 1); break;
    case 0xE3: for (int i = 0; i < nw; i++) r.w[i] = (u16)((a.w[i] + b.w[i] + 1) >> 1); break;

    // -- logic --
    case 0xDB: for (int i = 0; i < nq; i++) r.q[i] = a.q[i] & b.q[i]; break;
    case 0xDF: for (int i = 0; i < nq; i++) r.q[i] = ~a.q[i] & b.q[i]; break;
    case 0xEB: for (int i = 0; i < nq; i++) r.q[i] = a.q[i] | b.q[i]; break;
    case 0xEF: for (int i = 0; i < nq; i++) r.q[i] = a.q[i] ^ b.q[i]; break;

    default:
      return false;
  }
  return true;
}

// True if `op` belongs to the packed-integer family, so the caller can tell
// before touching the operands.
struct PackedIntTable {
  bool valid[256];
  PackedIntTable() {
    const Vec z = {};
    Vec r;
    for (int i = 0; i < 256; i++) valid[i] = packed_int((u8)i, z, z, r, 16);
  }
};

bool is_packed_int(u8 op) {
  static const PackedIntTable table;
  return table.valid[op];
}

}  // namespace

void sse_apply_rounding(u32 mxcsr) {
  static const int kModes[4] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
  host_rounding(kModes[(mxcsr >> 13) & 3]);
}

bool sse_exec(X86State& s, u8 op2, int prefix, u8 modrm, u32 ea, u32& ip) {
  u8* const m = s.mem;
  const int mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
  const bool is_reg = mod == 3;
  Xmm& X = s.xmm[reg];
  const bool none = prefix == 0, p66 = prefix == 0x66, pF3 = prefix == 0xF3, pF2 = prefix == 0xF2;

  // ---- operand helpers ----
  auto load_xmm = [&]() -> Xmm {
    if (is_reg) return s.xmm[rm];
    Xmm v;
    memcpy(v.b, m + ea, 16);
    return v;
  };
  auto load_q = [&]() -> u64 {
    if (is_reg) return s.xmm[rm].q[0];
    u64 v;
    memcpy(&v, m + ea, 8);
    return v;
  };
  auto load_d = [&]() -> u32 {
    if (is_reg) return s.xmm[rm].d[0];
    u32 v;
    memcpy(&v, m + ea, 4);
    return v;
  };
  // A 32-bit operand that is a general register when it isn't memory.
  auto load_gpr = [&]() -> u32 {
    if (is_reg) return s.reg[rm];
    u32 v;
    memcpy(&v, m + ea, 4);
    return v;
  };
  // MMX registers are the significands of the x87 registers. Using one marks
  // the whole x87 stack as in use, until `emms`.
  auto load_mm = [&]() -> u64 {
    if (is_reg) return s.fpr[rm].man;
    u64 v;
    memcpy(&v, m + ea, 8);
    return v;
  };
  auto set_mm = [&](int i, u64 v) {
    s.fpr[i].man = v;
    s.fpr[i].se = 0xFFFF;
    s.fsw &= ~0x3800;
    s.ftag = 0xFF;
  };
  auto imm8 = [&]() -> u32 { return m[ip++]; };

  switch (op2) {
    // ---- moves ----
    case 0x10:  // movups / movupd / movss / movsd, load
      if (none || p66) {
        X = load_xmm();
      } else if (pF3) {
        if (is_reg) {
          X.d[0] = s.xmm[rm].d[0];
        } else {
          X.q[0] = load_d();
          X.q[1] = 0;
        }
      } else {
        if (is_reg) {
          X.q[0] = s.xmm[rm].q[0];
        } else {
          X.q[0] = load_q();
          X.q[1] = 0;
        }
      }
      return true;
    case 0x11:  // ... store
      if (none || p66) {
        if (is_reg) {
          s.xmm[rm] = X;
        } else {
          memcpy(m + ea, X.b, 16);
        }
      } else if (pF3) {
        if (is_reg) {
          s.xmm[rm].d[0] = X.d[0];
        } else {
          memcpy(m + ea, &X.d[0], 4);
        }
      } else {
        if (is_reg) {
          s.xmm[rm].q[0] = X.q[0];
        } else {
          memcpy(m + ea, &X.q[0], 8);
        }
      }
      return true;
    case 0x12:
      if (none) {  // movlps (memory) / movhlps (register)
        X.q[0] = is_reg ? s.xmm[rm].q[1] : load_q();
      } else if (p66) {  // movlpd
        X.q[0] = load_q();
      } else if (pF2) {  // movddup
        const u64 v = load_q();
        X.q[0] = X.q[1] = v;
      } else {  // movsldup
        const Xmm v = load_xmm();
        X.d[0] = X.d[1] = v.d[0];
        X.d[2] = X.d[3] = v.d[2];
      }
      return true;
    case 0x13:  // movlps / movlpd, store
      if (is_reg || !(none || p66)) return false;
      memcpy(m + ea, &X.q[0], 8);
      return true;
    case 0x14: {  // unpcklps / unpcklpd
      const Xmm b = load_xmm(), a = X;
      if (none) {
        X.d[0] = a.d[0];
        X.d[1] = b.d[0];
        X.d[2] = a.d[1];
        X.d[3] = b.d[1];
      } else if (p66) {
        X.q[1] = b.q[0];
      } else {
        return false;
      }
      return true;
    }
    case 0x15: {  // unpckhps / unpckhpd
      const Xmm b = load_xmm(), a = X;
      if (none) {
        X.d[0] = a.d[2];
        X.d[1] = b.d[2];
        X.d[2] = a.d[3];
        X.d[3] = b.d[3];
      } else if (p66) {
        X.q[0] = a.q[1];
        X.q[1] = b.q[1];
      } else {
        return false;
      }
      return true;
    }
    case 0x16:
      if (none) {  // movhps (memory) / movlhps (register)
        X.q[1] = load_q();
      } else if (p66) {  // movhpd
        X.q[1] = load_q();
      } else if (pF3) {  // movshdup
        const Xmm v = load_xmm();
        X.d[0] = X.d[1] = v.d[1];
        X.d[2] = X.d[3] = v.d[3];
      } else {
        return false;
      }
      return true;
    case 0x17:  // movhps / movhpd, store
      if (is_reg || !(none || p66)) return false;
      memcpy(m + ea, &X.q[1], 8);
      return true;
    case 0x28:  // movaps / movapd, load
      X = load_xmm();
      return true;
    case 0x29:  // ... store
      if (is_reg) {
        s.xmm[rm] = X;
      } else {
        memcpy(m + ea, X.b, 16);
      }
      return true;
    case 0x2B:  // movntps / movntpd
      if (is_reg) return false;
      memcpy(m + ea, X.b, 16);
      return true;
    case 0xF0:  // lddqu
      if (!pF2 || is_reg) return false;
      X = load_xmm();
      return true;
    case 0xC3: {  // movnti
      if (is_reg || !none) return false;
      memcpy(m + ea, &s.reg[reg], 4);
      return true;
    }

    // ---- integer <-> float conversions ----
    case 0x2A:
      if (pF3) {  // cvtsi2ss
        X.d[0] = bits32((float)(s32)load_gpr());
      } else if (pF2) {  // cvtsi2sd
        X.q[0] = bits64((double)(s32)load_gpr());
      } else {  // cvtpi2ps / cvtpi2pd, from an MMX register or memory
        const u64 v = load_mm();
        const s32 lo = (s32)v, hi = (s32)(v >> 32);
        if (none) {
          X.d[0] = bits32((float)lo);
          X.d[1] = bits32((float)hi);
        } else {
          X.q[0] = bits64((double)lo);
          X.q[1] = bits64((double)hi);
        }
      }
      return true;
    case 0x2C:
    case 0x2D: {
      const bool truncate = op2 == 0x2C;
      if (pF3) {  // cvt(t)ss2si
        s.reg[reg] = (u32)to_s32(f32(load_d()), truncate);
      } else if (pF2) {  // cvt(t)sd2si
        s.reg[reg] = (u32)to_s32(f64(load_q()), truncate);
      } else if (none) {  // cvt(t)ps2pi
        const u64 v = load_q();
        const u32 lo = (u32)to_s32(f32((u32)v), truncate);
        const u32 hi = (u32)to_s32(f32((u32)(v >> 32)), truncate);
        set_mm(reg, ((u64)hi << 32) | lo);
      } else {  // cvt(t)pd2pi
        const Xmm v = load_xmm();
        const u32 lo = (u32)to_s32(f64(v.q[0]), truncate);
        const u32 hi = (u32)to_s32(f64(v.q[1]), truncate);
        set_mm(reg, ((u64)hi << 32) | lo);
      }
      return true;
    }
    case 0x5A:
      if (none) {  // cvtps2pd
        const u64 v = load_q();
        X.q[0] = widen((u32)v);
        X.q[1] = widen((u32)(v >> 32));
      } else if (p66) {  // cvtpd2ps
        const Xmm v = load_xmm();
        X.d[0] = narrow(v.q[0]);
        X.d[1] = narrow(v.q[1]);
        X.q[1] = 0;
      } else if (pF3) {  // cvtss2sd
        X.q[0] = widen(load_d());
      } else {  // cvtsd2ss
        X.d[0] = narrow(load_q());
      }
      return true;
    case 0x5B: {
      const Xmm v = load_xmm();
      if (none) {  // cvtdq2ps
        for (int i = 0; i < 4; i++) X.d[i] = bits32((float)(s32)v.d[i]);
      } else if (p66 || pF3) {  // cvtps2dq / cvttps2dq
        for (int i = 0; i < 4; i++) X.d[i] = (u32)to_s32(f32(v.d[i]), pF3);
      } else {
        return false;
      }
      return true;
    }
    case 0xE6:
      if (pF3) {  // cvtdq2pd
        const u64 v = load_q();
        X.q[0] = bits64((double)(s32)v);
        X.q[1] = bits64((double)(s32)(v >> 32));
      } else if (p66 || pF2) {  // cvttpd2dq / cvtpd2dq
        const Xmm v = load_xmm();
        X.d[0] = (u32)to_s32(f64(v.q[0]), p66);
        X.d[1] = (u32)to_s32(f64(v.q[1]), p66);
        X.q[1] = 0;
      } else {
        return false;
      }
      return true;

    // ---- compares ----
    case 0x2E:
    case 0x2F: {  // ucomiss / comiss / ucomisd / comisd
      double x, y;
      if (none) {
        x = f32(X.d[0]);
        y = f32(load_d());
      } else if (p66) {
        x = f64(X.q[0]);
        y = f64(load_q());
      } else {
        return false;
      }
      u32 f = s.eflags & ~0x8D5u;  // clears OF SF ZF AF PF CF
      if (x != x || y != y) {
        f |= 0x45;
      } else if (x < y) {
        f |= 0x01;
      } else if (x == y) {
        f |= 0x40;
      }
      s.eflags = f;
      return true;
    }
    case 0xC2: {  // cmpps / cmppd / cmpss / cmpsd
      if (none) {
        const Xmm b = load_xmm();
        const u32 pred = imm8();
        for (int i = 0; i < 4; i++) X.d[i] = compare(pred, f32(X.d[i]), f32(b.d[i])) ? ~0u : 0;
      } else if (p66) {
        const Xmm b = load_xmm();
        const u32 pred = imm8();
        for (int i = 0; i < 2; i++) X.q[i] = compare(pred, f64(X.q[i]), f64(b.q[i])) ? ~0ull : 0;
      } else if (pF3) {
        const u32 b = load_d();
        X.d[0] = compare(imm8(), f32(X.d[0]), f32(b)) ? ~0u : 0;
      } else {
        const u64 b = load_q();
        X.q[0] = compare(imm8(), f64(X.q[0]), f64(b)) ? ~0ull : 0;
      }
      return true;
    }
    case 0x50: {  // movmskps / movmskpd
      if (!is_reg) return false;
      const Xmm& v = s.xmm[rm];
      if (none) {
        s.reg[reg] = (v.d[0] >> 31) | ((v.d[1] >> 31) << 1) | ((v.d[2] >> 31) << 2) |
                     ((v.d[3] >> 31) << 3);
      } else if (p66) {
        s.reg[reg] = (u32)(v.q[0] >> 63) | ((u32)(v.q[1] >> 63) << 1);
      } else {
        return false;
      }
      return true;
    }

    // ---- arithmetic ----
    case 0x51: case 0x58: case 0x59: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
      if (none) {
        const Xmm b = load_xmm();
        for (int i = 0; i < 4; i++) X.d[i] = arith32(op2, X.d[i], b.d[i]);
      } else if (p66) {
        const Xmm b = load_xmm();
        for (int i = 0; i < 2; i++) X.q[i] = arith64(op2, X.q[i], b.q[i]);
      } else if (pF3) {
        X.d[0] = arith32(op2, X.d[0], load_d());
      } else {
        X.q[0] = arith64(op2, X.q[0], load_q());
      }
      return true;
    case 0x52:
    case 0x53: {  // rsqrtps/ss, rcpps/ss: exact here, a 12-bit estimate on real CPUs
      auto estimate = [&](u32 v) -> u32 {
        const float x = f32(v);
        const float r = op2 == 0x52 ? 1.0f / std::sqrt(x) : 1.0f / x;
        if (r != r) return is_nan32(v) ? (v | kQuiet32) : kNaN32;
        return bits32(r);
      };
      if (none) {
        const Xmm b = load_xmm();
        for (int i = 0; i < 4; i++) X.d[i] = estimate(b.d[i]);
      } else if (pF3) {
        X.d[0] = estimate(load_d());
      } else {
        return false;
      }
      return true;
    }
    case 0x54: case 0x55: case 0x56: case 0x57: {  // andps andnps orps xorps (and the pd forms)
      if (!(none || p66)) return false;
      const Xmm b = load_xmm();
      for (int i = 0; i < 2; i++) {
        const u64 a = X.q[i];
        X.q[i] = op2 == 0x54 ? (a & b.q[i])
                 : op2 == 0x55 ? (~a & b.q[i])
                 : op2 == 0x56 ? (a | b.q[i])
                               : (a ^ b.q[i]);
      }
      return true;
    }
    case 0xC6: {  // shufps / shufpd
      const Xmm b = load_xmm(), a = X;
      const u32 sel = imm8();
      if (none) {
        X.d[0] = a.d[sel & 3];
        X.d[1] = a.d[(sel >> 2) & 3];
        X.d[2] = b.d[(sel >> 4) & 3];
        X.d[3] = b.d[(sel >> 6) & 3];
      } else if (p66) {
        X.q[0] = a.q[sel & 1];
        X.q[1] = b.q[(sel >> 1) & 1];
      } else {
        return false;
      }
      return true;
    }
    case 0x7C:
    case 0x7D: {  // haddps/pd, hsubps/pd
      const u8 op = op2 == 0x7C ? 0x58 : 0x5C;
      const Xmm b = load_xmm(), a = X;
      if (pF2) {
        X.d[0] = arith32(op, a.d[0], a.d[1]);
        X.d[1] = arith32(op, a.d[2], a.d[3]);
        X.d[2] = arith32(op, b.d[0], b.d[1]);
        X.d[3] = arith32(op, b.d[2], b.d[3]);
      } else if (p66) {
        X.q[0] = arith64(op, a.q[0], a.q[1]);
        X.q[1] = arith64(op, b.q[0], b.q[1]);
      } else {
        return false;
      }
      return true;
    }
    case 0xD0: {  // addsubps / addsubpd: subtract in even lanes, add in odd ones
      if (!(pF2 || p66)) return false;
      const Xmm b = load_xmm();
      if (pF2) {
        for (int i = 0; i < 4; i++) X.d[i] = arith32((i & 1) ? 0x58 : 0x5C, X.d[i], b.d[i]);
      } else {
        X.q[0] = arith64(0x5C, X.q[0], b.q[0]);
        X.q[1] = arith64(0x58, X.q[1], b.q[1]);
      }
      return true;
    }

    // ---- control and state ----
    case 0xAE:
      if (is_reg) return false;
      switch (reg) {
        case 0: x87_fxsave(s, ea); return true;
        case 1: x87_fxrstor(s, ea); return true;
        case 2:  // ldmxcsr
          memcpy(&s.mxcsr, m + ea, 4);
          s.mxcsr &= 0x0000FFBF;
          sse_apply_rounding(s.mxcsr);
          return true;
        case 3:  // stmxcsr
          memcpy(m + ea, &s.mxcsr, 4);
          return true;
        default: return false;
      }

    // ---- integer moves ----
    case 0x6E:  // movd mm/xmm, r/m32
      if (none) {
        set_mm(reg, load_gpr());
      } else if (p66) {
        X.q[0] = load_gpr();
        X.q[1] = 0;
      } else {
        return false;
      }
      return true;
    case 0x7E:
      if (pF3) {  // movq xmm, xmm/m64
        X.q[0] = load_q();
        X.q[1] = 0;
      } else if (none || p66) {  // movd r/m32, mm/xmm
        const u32 v = none ? (u32)s.fpr[reg].man : X.d[0];
        if (is_reg) {
          s.reg[rm] = v;
        } else {
          memcpy(m + ea, &v, 4);
        }
      } else {
        return false;
      }
      return true;
    case 0x6F:  // movq mm / movdqa / movdqu, load
      if (none) {
        set_mm(reg, load_mm());
      } else if (p66 || pF3) {
        X = load_xmm();
      } else {
        return false;
      }
      return true;
    case 0x7F:  // ... store
      if (none) {
        const u64 v = s.fpr[reg].man;
        if (is_reg) {
          set_mm(rm, v);
        } else {
          memcpy(m + ea, &v, 8);
        }
      } else if (p66 || pF3) {
        if (is_reg) {
          s.xmm[rm] = X;
        } else {
          memcpy(m + ea, X.b, 16);
        }
      } else {
        return false;
      }
      return true;
    case 0xD6:
      if (p66) {  // movq xmm/m64, xmm
        if (is_reg) {
          s.xmm[rm].q[0] = X.q[0];
          s.xmm[rm].q[1] = 0;
        } else {
          memcpy(m + ea, &X.q[0], 8);
        }
      } else if (pF3 && is_reg) {  // movq2dq
        X.q[0] = s.fpr[rm].man;
        X.q[1] = 0;
      } else if (pF2 && is_reg) {  // movdq2q
        set_mm(reg, s.xmm[rm].q[0]);
      } else {
        return false;
      }
      return true;
    case 0xD7: {  // pmovmskb
      if (!is_reg) return false;
      u32 mask = 0;
      if (none) {
        const u64 v = s.fpr[rm].man;
        for (int i = 0; i < 8; i++) mask |= (u32)((v >> (8 * i + 7)) & 1) << i;
      } else if (p66) {
        for (int i = 0; i < 16; i++) mask |= (u32)(s.xmm[rm].b[i] >> 7) << i;
      } else {
        return false;
      }
      s.reg[reg] = mask;
      return true;
    }
    case 0xE7:  // movntq / movntdq
      if (is_reg) return false;
      if (none) {
        memcpy(m + ea, &s.fpr[reg].man, 8);
      } else if (p66) {
        memcpy(m + ea, X.b, 16);
      } else {
        return false;
      }
      return true;
    case 0xF7: {  // maskmovq / maskmovdqu: store selected bytes at [edi]
      if (!is_reg) return false;
      const u32 dst = s.reg[X86State::EDI];
      if (none) {
        const u64 v = s.fpr[reg].man, k = s.fpr[rm].man;
        for (u32 i = 0; i < 8; i++) {
          if ((k >> (8 * i + 7)) & 1) m[dst + i] = (u8)(v >> (8 * i));
        }
      } else if (p66) {
        for (u32 i = 0; i < 16; i++) {
          if (s.xmm[rm].b[i] & 0x80) m[dst + i] = X.b[i];
        }
      } else {
        return false;
      }
      return true;
    }

    // ---- shuffles, shifts by a constant, word insert and extract ----
    case 0x70: {
      if (none) {  // pshufw
        const u64 v = load_mm();
        const u32 sel = imm8();
        u64 r = 0;
        for (int i = 0; i < 4; i++) r |= ((v >> (16 * ((sel >> (2 * i)) & 3))) & 0xFFFF) << (16 * i);
        set_mm(reg, r);
        return true;
      }
      const Xmm v = load_xmm();
      const u32 sel = imm8();
      if (p66) {  // pshufd
        for (int i = 0; i < 4; i++) X.d[i] = v.d[(sel >> (2 * i)) & 3];
      } else if (pF3) {  // pshufhw
        X.q[0] = v.q[0];
        for (int i = 0; i < 4; i++) X.w[4 + i] = v.w[4 + ((sel >> (2 * i)) & 3)];
      } else {  // pshuflw
        X.q[1] = v.q[1];
        for (int i = 0; i < 4; i++) X.w[i] = v.w[(sel >> (2 * i)) & 3];
      }
      return true;
    }
    case 0x71: case 0x72: case 0x73: {
      if (!is_reg || !(none || p66)) return false;
      const u32 count = imm8();
      const int n = p66 ? 16 : 8;
      const int lane = op2 == 0x71 ? 16 : (op2 == 0x72 ? 32 : 64);
      Vec v = {};
      if (p66) {
        memcpy(v.b, s.xmm[rm].b, 16);
      } else {
        v.q[0] = s.fpr[rm].man;
      }
      if (op2 == 0x73 && (reg == 3 || reg == 7)) {  // psrldq / pslldq: shift by whole bytes
        if (!p66) return false;
        Vec r = {};
        const int c = count > 16 ? 16 : (int)count;
        for (int i = 0; i < 16 - c; i++) {
          if (reg == 3) {
            r.b[i] = v.b[i + c];
          } else {
            r.b[i + c] = v.b[i];
          }
        }
        v = r;
      } else if (reg == 2) {
        shift_lanes(v, 0, lane, count, n);
      } else if (reg == 4 && lane != 64) {
        shift_lanes(v, 1, lane, count, n);
      } else if (reg == 6) {
        shift_lanes(v, 2, lane, count, n);
      } else {
        return false;
      }
      if (p66) {
        memcpy(s.xmm[rm].b, v.b, 16);
      } else {
        set_mm(rm, v.q[0]);
      }
      return true;
    }
    case 0xC4: {  // pinsrw
      u32 v;
      if (is_reg) {
        v = s.reg[rm];
      } else {
        u16 t;
        memcpy(&t, m + ea, 2);
        v = t;
      }
      const u32 idx = imm8();
      if (none) {
        const int sh = 16 * (int)(idx & 3);
        set_mm(reg, (s.fpr[reg].man & ~(0xFFFFull << sh)) | ((u64)(v & 0xFFFF) << sh));
      } else if (p66) {
        X.w[idx & 7] = (u16)v;
      } else {
        return false;
      }
      return true;
    }
    case 0xC5: {  // pextrw
      if (!is_reg) return false;
      const u32 idx = imm8();
      if (none) {
        s.reg[reg] = (u32)(s.fpr[rm].man >> (16 * (idx & 3))) & 0xFFFF;
      } else if (p66) {
        s.reg[reg] = s.xmm[rm].w[idx & 7];
      } else {
        return false;
      }
      return true;
    }

    // ---- everything else is the packed-integer family ----
    default: {
      if (!(none || p66) || !is_packed_int(op2)) return false;
      const int n = p66 ? 16 : 8;
      Vec a = {}, b = {}, r;
      if (p66) {
        memcpy(a.b, X.b, 16);
        const Xmm src = load_xmm();
        memcpy(b.b, src.b, 16);
      } else {
        a.q[0] = s.fpr[reg].man;
        b.q[0] = load_mm();
      }
      if (!packed_int(op2, a, b, r, n)) return false;
      if (p66) {
        memcpy(X.b, r.b, 16);
      } else {
        set_mm(reg, r.q[0]);
      }
      return true;
    }
  }
}
