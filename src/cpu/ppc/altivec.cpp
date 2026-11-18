// AltiVec, the PowerPC vector instruction set (G4 and G5).
//
// Registers are kept byte-reversed (see PPCState::Vec), so operations that
// treat every element alike can loop in host order, while anything that cares
// where an element sits goes through the B/H/W accessors, which take the
// guest's big-endian element numbers.
//
// Vector floating point always rounds to nearest. Not modelled: flushing
// denormals to zero in "non-Java" mode, and the exact low bits of the
// estimate instructions (vrefp and friends return correctly rounded results).

#include "cpu/rounding.h"
#include <cfenv>
#include <cmath>

#include "cpu/ppc/ppc.h"

namespace {

typedef PPCState::Vec V;

#define B(v, i) ((v).b[15 - (i)])
#define SB(v, i) ((v).sb[15 - (i)])
#define H(v, i) ((v).h[7 - (i)])
#define SH(v, i) ((v).sh[7 - (i)])
#define W(v, i) ((v).w[3 - (i)])
#define SW(v, i) ((v).sw[3 - (i)])

#define EACH_B for (int i = 0; i < 16; i++)
#define EACH_H for (int i = 0; i < 8; i++)
#define EACH_W for (int i = 0; i < 4; i++)

// Saturating conversions. Each reports saturation through `sat`.
inline u8 sat_u8(s32 v, bool& sat) {
  if (v < 0) { sat = true; return 0; }
  if (v > 255) { sat = true; return 255; }
  return (u8)v;
}
inline s8 sat_s8(s32 v, bool& sat) {
  if (v < -128) { sat = true; return -128; }
  if (v > 127) { sat = true; return 127; }
  return (s8)v;
}
inline u16 sat_u16(s32 v, bool& sat) {
  if (v < 0) { sat = true; return 0; }
  if (v > 65535) { sat = true; return 65535; }
  return (u16)v;
}
inline s16 sat_s16(s32 v, bool& sat) {
  if (v < -32768) { sat = true; return -32768; }
  if (v > 32767) { sat = true; return 32767; }
  return (s16)v;
}
inline u16 sat_u16_from_u32(u32 v, bool& sat) {
  if (v > 65535) { sat = true; return 65535; }
  return (u16)v;
}
inline u32 sat_u32(s64 v, bool& sat) {
  if (v < 0) { sat = true; return 0; }
  if (v > 0xFFFFFFFFll) { sat = true; return 0xFFFFFFFFu; }
  return (u32)v;
}
inline s32 sat_s32(s64 v, bool& sat) {
  if (v < INT32_MIN) { sat = true; return INT32_MIN; }
  if (v > INT32_MAX) { sat = true; return INT32_MAX; }
  return (s32)v;
}

inline u8 rotl8(u8 v, unsigned n) { return n ? (u8)((v << n) | (v >> (8 - n))) : v; }
inline u16 rotl16(u16 v, unsigned n) { return n ? (u16)((v << n) | (v >> (16 - n))) : v; }
inline u32 rotl32(u32 v, unsigned n) { return n ? (v << n) | (v >> (32 - n)) : v; }

inline float fmax_ppc(float a, float b) {
  if (a != a) return a;
  if (b != b) return b;
  if (a > b) return a;
  if (b > a) return b;
  return std::signbit(a) ? b : a;  // +0 beats -0
}
inline float fmin_ppc(float a, float b) {
  if (a != a) return a;
  if (b != b) return b;
  if (a < b) return a;
  if (b < a) return b;
  return std::signbit(a) ? a : b;  // -0 beats +0
}

// Negates a rounded result but leaves a NaN alone. Deliberately out of line:
// inlined next to an fma, the compiler folds the negation into an ARM
// multiply-add that negates before adding, which turns a cancelled sum's -0
// into +0.
__attribute__((noinline)) float negate_result(float r) {
  if (r != r) return r;
  u32 bits;
  memcpy(&bits, &r, 4);
  bits ^= 0x80000000u;
  memcpy(&r, &bits, 4);
  return r;
}

// Vector floating point ignores the FPSCR rounding mode.
struct NearestRounding {
  int saved;
  NearestRounding() : saved(host_rounding_now()) {
    if (saved != FE_TONEAREST) host_rounding(FE_TONEAREST);
  }
  ~NearestRounding() {
    if (saved != FE_TONEAREST) host_rounding(saved);
  }
};

// Compare results go to CR field 6 when the record bit is set: 8 if every
// element matched, 2 if none did.
void record_cr6(PPCState& s, bool all, bool none) {
  s.cr = (s.cr & ~0xF0u) | ((all ? 8u : 0u) << 4) | ((none ? 2u : 0u) << 4);
}

bool compare(PPCState& s, u32 insn, const V& a, const V& b, V& r) {
  bool all = true, none = true;
#define CMP_B(expr) EACH_B { const bool t = (expr); r.b[i] = t ? 0xFF : 0; all &= t; none &= !t; }
#define CMP_H(expr) EACH_H { const bool t = (expr); r.h[i] = t ? 0xFFFF : 0; all &= t; none &= !t; }
#define CMP_W(expr) EACH_W { const bool t = (expr); r.w[i] = t ? 0xFFFFFFFFu : 0; all &= t; none &= !t; }
  switch (insn & 0x3FF) {
    case 6: CMP_B(a.b[i] == b.b[i]) break;      // vcmpequb
    case 70: CMP_H(a.h[i] == b.h[i]) break;     // vcmpequh
    case 134: CMP_W(a.w[i] == b.w[i]) break;    // vcmpequw
    case 518: CMP_B(a.b[i] > b.b[i]) break;     // vcmpgtub
    case 582: CMP_H(a.h[i] > b.h[i]) break;     // vcmpgtuh
    case 646: CMP_W(a.w[i] > b.w[i]) break;     // vcmpgtuw
    case 774: CMP_B(a.sb[i] > b.sb[i]) break;   // vcmpgtsb
    case 838: CMP_H(a.sh[i] > b.sh[i]) break;   // vcmpgtsh
    case 902: CMP_W(a.sw[i] > b.sw[i]) break;   // vcmpgtsw
    case 198: CMP_W(a.f[i] == b.f[i]) break;    // vcmpeqfp
    case 454: CMP_W(a.f[i] >= b.f[i]) break;    // vcmpgefp
    case 710: CMP_W(a.f[i] > b.f[i]) break;     // vcmpgtfp
    case 966: {  // vcmpbfp: bit 31 if a > b, bit 30 if a < -b, both for a NaN
      bool in_bounds = true;
      EACH_W {
        const bool le = a.f[i] <= b.f[i], ge = a.f[i] >= -b.f[i];
        r.w[i] = (le ? 0 : 0x80000000u) | (ge ? 0 : 0x40000000u);
        in_bounds &= le && ge;
      }
      all = false;
      none = in_bounds;
      break;
    }
    default: return false;
  }
#undef CMP_B
#undef CMP_H
#undef CMP_W
  if (insn & 0x400) record_cr6(s, all, none);
  return true;
}

}  // namespace

bool altivec_exec(PPCState& s, u32 insn) {
  const u32 vd = (insn >> 21) & 31, va = (insn >> 16) & 31, vb = (insn >> 11) & 31,
            vc = (insn >> 6) & 31;
  // Copies, so the destination may be one of the sources.
  const V a = s.vr[va], b = s.vr[vb], c = s.vr[vc];
  V r = {};
  bool sat = false;

  if ((insn & 0x3F) >= 32) {
    // ---- four-operand forms ----
    switch (insn & 0x3F) {
      case 32: EACH_H r.sh[i] = sat_s16(((a.sh[i] * b.sh[i]) >> 15) + c.sh[i], sat); break;            // vmhaddshs
      case 33: EACH_H r.sh[i] = sat_s16(((a.sh[i] * b.sh[i] + 0x4000) >> 15) + c.sh[i], sat); break;   // vmhraddshs
      case 34: EACH_H r.h[i] = (u16)((u32)a.h[i] * b.h[i] + c.h[i]); break;                            // vmladduhm
      case 36:  // vmsumubm
        EACH_W {
          u32 sum = W(c, i);
          for (int k = 0; k < 4; k++) sum += (u32)B(a, 4 * i + k) * B(b, 4 * i + k);
          W(r, i) = sum;
        }
        break;
      case 37:  // vmsummbm: signed bytes times unsigned bytes
        EACH_W {
          u32 sum = W(c, i);
          for (int k = 0; k < 4; k++) sum += (u32)(SB(a, 4 * i + k) * (s32)B(b, 4 * i + k));
          W(r, i) = sum;
        }
        break;
      case 38:  // vmsumuhm
        EACH_W W(r, i) = W(c, i) + (u32)H(a, 2 * i) * H(b, 2 * i) + (u32)H(a, 2 * i + 1) * H(b, 2 * i + 1);
        break;
      case 39:  // vmsumuhs
        EACH_W {
          const s64 sum = (s64)W(c, i) + (s64)((u64)H(a, 2 * i) * H(b, 2 * i)) +
                          (s64)((u64)H(a, 2 * i + 1) * H(b, 2 * i + 1));
          W(r, i) = sat_u32(sum, sat);
        }
        break;
      case 40:  // vmsumshm
        EACH_W W(r, i) = W(c, i) + (u32)(SH(a, 2 * i) * SH(b, 2 * i)) +
                         (u32)(SH(a, 2 * i + 1) * SH(b, 2 * i + 1));
        break;
      case 41:  // vmsumshs
        EACH_W {
          const s64 sum = (s64)SW(c, i) + (s64)SH(a, 2 * i) * SH(b, 2 * i) +
                          (s64)SH(a, 2 * i + 1) * SH(b, 2 * i + 1);
          SW(r, i) = sat_s32(sum, sat);
        }
        break;
      case 42: EACH_W r.w[i] = (a.w[i] & ~c.w[i]) | (b.w[i] & c.w[i]); break;  // vsel
      case 43:  // vperm
        EACH_B {
          const u32 idx = B(c, i) & 31;
          B(r, i) = idx < 16 ? B(a, idx) : B(b, idx - 16);
        }
        break;
      case 44: {  // vsldoi
        const int sh = (int)(vc & 15);
        EACH_B B(r, i) = i + sh < 16 ? B(a, i + sh) : B(b, i + sh - 16);
        break;
      }
      case 46: {  // vmaddfp: a*c + b, rounded once
        const NearestRounding nearest;
        EACH_W r.f[i] = fmaf(a.f[i], c.f[i], b.f[i]);
        break;
      }
      case 47: {  // vnmsubfp: -(a*c - b)
        const NearestRounding nearest;
        EACH_W r.f[i] = negate_result(fmaf(a.f[i], c.f[i], -b.f[i]));
        break;
      }
      default:
        return false;
    }
    s.vr[vd] = r;
    if (sat) s.vscr |= 1;
    return true;
  }

  if ((insn & 0x3F) == 6) {
    if (!compare(s, insn, a, b, r)) return false;
    s.vr[vd] = r;
    return true;
  }

  // ---- three-operand forms ----
  const s32 simm = ((s32)(va << 27)) >> 27;  // the 5-bit immediate of the splat instructions
  switch (insn & 0x7FF) {
    // -- add --
    case 0: EACH_B r.b[i] = (u8)(a.b[i] + b.b[i]); break;                          // vaddubm
    case 64: EACH_H r.h[i] = (u16)(a.h[i] + b.h[i]); break;                        // vadduhm
    case 128: EACH_W r.w[i] = a.w[i] + b.w[i]; break;                              // vadduwm
    case 384: EACH_W r.w[i] = (u32)(((u64)a.w[i] + b.w[i]) >> 32); break;          // vaddcuw
    case 512: EACH_B r.b[i] = sat_u8(a.b[i] + b.b[i], sat); break;                 // vaddubs
    case 576: EACH_H r.h[i] = sat_u16(a.h[i] + b.h[i], sat); break;                // vadduhs
    case 640: EACH_W r.w[i] = sat_u32((s64)a.w[i] + b.w[i], sat); break;           // vadduws
    case 768: EACH_B r.sb[i] = sat_s8(a.sb[i] + b.sb[i], sat); break;              // vaddsbs
    case 832: EACH_H r.sh[i] = sat_s16(a.sh[i] + b.sh[i], sat); break;             // vaddshs
    case 896: EACH_W r.sw[i] = sat_s32((s64)a.sw[i] + b.sw[i], sat); break;        // vaddsws
    // -- subtract --
    case 1024: EACH_B r.b[i] = (u8)(a.b[i] - b.b[i]); break;                       // vsububm
    case 1088: EACH_H r.h[i] = (u16)(a.h[i] - b.h[i]); break;                      // vsubuhm
    case 1152: EACH_W r.w[i] = a.w[i] - b.w[i]; break;                             // vsubuwm
    case 1408: EACH_W r.w[i] = a.w[i] >= b.w[i] ? 1 : 0; break;                    // vsubcuw
    case 1536: EACH_B r.b[i] = sat_u8(a.b[i] - b.b[i], sat); break;                // vsububs
    case 1600: EACH_H r.h[i] = sat_u16(a.h[i] - b.h[i], sat); break;               // vsubuhs
    case 1664: EACH_W r.w[i] = sat_u32((s64)a.w[i] - b.w[i], sat); break;          // vsubuws
    case 1792: EACH_B r.sb[i] = sat_s8(a.sb[i] - b.sb[i], sat); break;             // vsubsbs
    case 1856: EACH_H r.sh[i] = sat_s16(a.sh[i] - b.sh[i], sat); break;            // vsubshs
    case 1920: EACH_W r.sw[i] = sat_s32((s64)a.sw[i] - b.sw[i], sat); break;       // vsubsws
    // -- maximum and minimum --
    case 2: EACH_B r.b[i] = a.b[i] > b.b[i] ? a.b[i] : b.b[i]; break;              // vmaxub
    case 66: EACH_H r.h[i] = a.h[i] > b.h[i] ? a.h[i] : b.h[i]; break;             // vmaxuh
    case 130: EACH_W r.w[i] = a.w[i] > b.w[i] ? a.w[i] : b.w[i]; break;            // vmaxuw
    case 258: EACH_B r.sb[i] = a.sb[i] > b.sb[i] ? a.sb[i] : b.sb[i]; break;       // vmaxsb
    case 322: EACH_H r.sh[i] = a.sh[i] > b.sh[i] ? a.sh[i] : b.sh[i]; break;       // vmaxsh
    case 386: EACH_W r.sw[i] = a.sw[i] > b.sw[i] ? a.sw[i] : b.sw[i]; break;       // vmaxsw
    case 514: EACH_B r.b[i] = a.b[i] < b.b[i] ? a.b[i] : b.b[i]; break;            // vminub
    case 578: EACH_H r.h[i] = a.h[i] < b.h[i] ? a.h[i] : b.h[i]; break;            // vminuh
    case 642: EACH_W r.w[i] = a.w[i] < b.w[i] ? a.w[i] : b.w[i]; break;            // vminuw
    case 770: EACH_B r.sb[i] = a.sb[i] < b.sb[i] ? a.sb[i] : b.sb[i]; break;       // vminsb
    case 834: EACH_H r.sh[i] = a.sh[i] < b.sh[i] ? a.sh[i] : b.sh[i]; break;       // vminsh
    case 898: EACH_W r.sw[i] = a.sw[i] < b.sw[i] ? a.sw[i] : b.sw[i]; break;       // vminsw
    // -- average, rounding up --
    case 1026: EACH_B r.b[i] = (u8)((a.b[i] + b.b[i] + 1) >> 1); break;            // vavgub
    case 1090: EACH_H r.h[i] = (u16)((a.h[i] + b.h[i] + 1) >> 1); break;           // vavguh
    case 1154: EACH_W r.w[i] = (u32)(((u64)a.w[i] + b.w[i] + 1) >> 1); break;      // vavguw
    case 1282: EACH_B r.sb[i] = (s8)((a.sb[i] + b.sb[i] + 1) >> 1); break;         // vavgsb
    case 1346: EACH_H r.sh[i] = (s16)((a.sh[i] + b.sh[i] + 1) >> 1); break;        // vavgsh
    case 1410: EACH_W r.sw[i] = (s32)(((s64)a.sw[i] + b.sw[i] + 1) >> 1); break;   // vavgsw
    // -- rotate and shift, element by element --
    case 4: EACH_B r.b[i] = rotl8(a.b[i], b.b[i] & 7); break;                      // vrlb
    case 68: EACH_H r.h[i] = rotl16(a.h[i], b.h[i] & 15); break;                   // vrlh
    case 132: EACH_W r.w[i] = rotl32(a.w[i], b.w[i] & 31); break;                  // vrlw
    case 260: EACH_B r.b[i] = (u8)(a.b[i] << (b.b[i] & 7)); break;                 // vslb
    case 324: EACH_H r.h[i] = (u16)(a.h[i] << (b.h[i] & 15)); break;               // vslh
    case 388: EACH_W r.w[i] = a.w[i] << (b.w[i] & 31); break;                      // vslw
    case 516: EACH_B r.b[i] = (u8)(a.b[i] >> (b.b[i] & 7)); break;                 // vsrb
    case 580: EACH_H r.h[i] = (u16)(a.h[i] >> (b.h[i] & 15)); break;               // vsrh
    case 644: EACH_W r.w[i] = a.w[i] >> (b.w[i] & 31); break;                      // vsrw
    case 772: EACH_B r.sb[i] = (s8)(a.sb[i] >> (b.b[i] & 7)); break;               // vsrab
    case 836: EACH_H r.sh[i] = (s16)(a.sh[i] >> (b.h[i] & 15)); break;             // vsrah
    case 900: EACH_W r.sw[i] = a.sw[i] >> (b.w[i] & 31); break;                    // vsraw
    // -- shift the whole register --
    case 452: {  // vsl: left by 0-7 bits
      const int sh = B(b, 15) & 7;
      EACH_B B(r, i) = (u8)((B(a, i) << sh) | (i < 15 ? B(a, i + 1) >> (8 - sh) : 0));
      break;
    }
    case 708: {  // vsr: right by 0-7 bits
      const int sh = B(b, 15) & 7;
      EACH_B B(r, i) = (u8)((B(a, i) >> sh) | (i > 0 ? B(a, i - 1) << (8 - sh) : 0));
      break;
    }
    case 1036: {  // vslo: left by whole bytes
      const int sh = (B(b, 15) >> 3) & 15;
      EACH_B B(r, i) = i + sh < 16 ? B(a, i + sh) : 0;
      break;
    }
    case 1100: {  // vsro: right by whole bytes
      const int sh = (B(b, 15) >> 3) & 15;
      EACH_B B(r, i) = i >= sh ? B(a, i - sh) : 0;
      break;
    }
    // -- logic --
    case 1028: EACH_W r.w[i] = a.w[i] & b.w[i]; break;     // vand
    case 1092: EACH_W r.w[i] = a.w[i] & ~b.w[i]; break;    // vandc
    case 1156: EACH_W r.w[i] = a.w[i] | b.w[i]; break;     // vor
    case 1284: EACH_W r.w[i] = ~(a.w[i] | b.w[i]); break;  // vnor
    case 1220: EACH_W r.w[i] = a.w[i] ^ b.w[i]; break;     // vxor
    // -- multiply even or odd elements into double-width results --
    case 8: EACH_H H(r, i) = (u16)(B(a, 2 * i + 1) * B(b, 2 * i + 1)); break;       // vmuloub
    case 520: EACH_H H(r, i) = (u16)(B(a, 2 * i) * B(b, 2 * i)); break;             // vmuleub
    case 264: EACH_H SH(r, i) = (s16)(SB(a, 2 * i + 1) * SB(b, 2 * i + 1)); break;  // vmulosb
    case 776: EACH_H SH(r, i) = (s16)(SB(a, 2 * i) * SB(b, 2 * i)); break;          // vmulesb
    case 72: EACH_W W(r, i) = (u32)H(a, 2 * i + 1) * H(b, 2 * i + 1); break;        // vmulouh
    case 584: EACH_W W(r, i) = (u32)H(a, 2 * i) * H(b, 2 * i); break;               // vmuleuh
    case 328: EACH_W SW(r, i) = SH(a, 2 * i + 1) * SH(b, 2 * i + 1); break;         // vmulosh
    case 840: EACH_W SW(r, i) = SH(a, 2 * i) * SH(b, 2 * i); break;                 // vmulesh
    // -- sums across elements --
    case 1544:  // vsum4ubs
      EACH_W {
        s64 sum = W(b, i);
        for (int k = 0; k < 4; k++) sum += B(a, 4 * i + k);
        W(r, i) = sat_u32(sum, sat);
      }
      break;
    case 1800:  // vsum4sbs
      EACH_W {
        s64 sum = SW(b, i);
        for (int k = 0; k < 4; k++) sum += SB(a, 4 * i + k);
        SW(r, i) = sat_s32(sum, sat);
      }
      break;
    case 1608:  // vsum4shs
      EACH_W SW(r, i) = sat_s32((s64)SW(b, i) + SH(a, 2 * i) + SH(a, 2 * i + 1), sat);
      break;
    case 1672:  // vsum2sws
      SW(r, 1) = sat_s32((s64)SW(a, 0) + SW(a, 1) + SW(b, 1), sat);
      SW(r, 3) = sat_s32((s64)SW(a, 2) + SW(a, 3) + SW(b, 3), sat);
      break;
    case 1928:  // vsumsws
      SW(r, 3) = sat_s32((s64)SW(a, 0) + SW(a, 1) + SW(a, 2) + SW(a, 3) + SW(b, 3), sat);
      break;
    // -- merge (interleave) --
    case 12: for (int i = 0; i < 8; i++) { B(r, 2 * i) = B(a, i); B(r, 2 * i + 1) = B(b, i); } break;           // vmrghb
    case 268: for (int i = 0; i < 8; i++) { B(r, 2 * i) = B(a, 8 + i); B(r, 2 * i + 1) = B(b, 8 + i); } break;  // vmrglb
    case 76: for (int i = 0; i < 4; i++) { H(r, 2 * i) = H(a, i); H(r, 2 * i + 1) = H(b, i); } break;           // vmrghh
    case 332: for (int i = 0; i < 4; i++) { H(r, 2 * i) = H(a, 4 + i); H(r, 2 * i + 1) = H(b, 4 + i); } break;  // vmrglh
    case 140: for (int i = 0; i < 2; i++) { W(r, 2 * i) = W(a, i); W(r, 2 * i + 1) = W(b, i); } break;          // vmrghw
    case 396: for (int i = 0; i < 2; i++) { W(r, 2 * i) = W(a, 2 + i); W(r, 2 * i + 1) = W(b, 2 + i); } break;  // vmrglw
    // -- splat --
    case 524: EACH_B r.b[i] = B(b, va & 15); break;   // vspltb
    case 588: EACH_H r.h[i] = H(b, va & 7); break;    // vsplth
    case 652: EACH_W r.w[i] = W(b, va & 3); break;    // vspltw
    case 780: EACH_B r.sb[i] = (s8)simm; break;       // vspltisb
    case 844: EACH_H r.sh[i] = (s16)simm; break;      // vspltish
    case 908: EACH_W r.sw[i] = simm; break;           // vspltisw
    // -- pack: a's elements then b's, each narrowed to half width --
    case 14: for (int i = 0; i < 8; i++) { B(r, i) = (u8)H(a, i); B(r, 8 + i) = (u8)H(b, i); } break;  // vpkuhum
    case 78: for (int i = 0; i < 4; i++) { H(r, i) = (u16)W(a, i); H(r, 4 + i) = (u16)W(b, i); } break;  // vpkuwum
    case 142: for (int i = 0; i < 8; i++) { B(r, i) = sat_u8(H(a, i), sat); B(r, 8 + i) = sat_u8(H(b, i), sat); } break;  // vpkuhus
    case 206: for (int i = 0; i < 4; i++) { H(r, i) = sat_u16_from_u32(W(a, i), sat); H(r, 4 + i) = sat_u16_from_u32(W(b, i), sat); } break;  // vpkuwus
    case 270: for (int i = 0; i < 8; i++) { B(r, i) = sat_u8(SH(a, i), sat); B(r, 8 + i) = sat_u8(SH(b, i), sat); } break;   // vpkshus
    case 334: for (int i = 0; i < 4; i++) { H(r, i) = sat_u16(SW(a, i), sat); H(r, 4 + i) = sat_u16(SW(b, i), sat); } break; // vpkswus
    case 398: for (int i = 0; i < 8; i++) { SB(r, i) = sat_s8(SH(a, i), sat); SB(r, 8 + i) = sat_s8(SH(b, i), sat); } break; // vpkshss
    case 462: for (int i = 0; i < 4; i++) { SH(r, i) = sat_s16(SW(a, i), sat); SH(r, 4 + i) = sat_s16(SW(b, i), sat); } break; // vpkswss
    case 782:  // vpkpx: 8-8-8-8 pixels to 1-5-5-5
      for (int i = 0; i < 8; i++) {
        const u32 p = i < 4 ? W(a, i) : W(b, i - 4);
        H(r, i) = (u16)((((p >> 24) & 1) << 15) | (((p >> 19) & 0x1F) << 10) |
                        (((p >> 11) & 0x1F) << 5) | ((p >> 3) & 0x1F));
      }
      break;
    // -- unpack: widen half of b's elements with sign extension --
    case 526: EACH_H SH(r, i) = SB(b, i); break;        // vupkhsb
    case 654: EACH_H SH(r, i) = SB(b, 8 + i); break;    // vupklsb
    case 590: EACH_W SW(r, i) = SH(b, i); break;        // vupkhsh
    case 718: EACH_W SW(r, i) = SH(b, 4 + i); break;    // vupklsh
    case 846:   // vupkhpx
    case 974:   // vupklpx: 1-5-5-5 pixels to 8-8-8-8
      EACH_W {
        const u32 p = H(b, ((insn & 0x7FF) == 846 ? 0 : 4) + i);
        W(r, i) = ((p & 0x8000) ? 0xFF000000u : 0) | (((p >> 10) & 0x1F) << 16) |
                  (((p >> 5) & 0x1F) << 8) | (p & 0x1F);
      }
      break;
    // -- floating point --
    case 10: { const NearestRounding n; EACH_W r.f[i] = a.f[i] + b.f[i]; break; }   // vaddfp
    case 74: { const NearestRounding n; EACH_W r.f[i] = a.f[i] - b.f[i]; break; }   // vsubfp
    case 1034: EACH_W r.f[i] = fmax_ppc(a.f[i], b.f[i]); break;                     // vmaxfp
    case 1098: EACH_W r.f[i] = fmin_ppc(a.f[i], b.f[i]); break;                     // vminfp
    case 266: { const NearestRounding n; EACH_W r.f[i] = 1.0f / b.f[i]; break; }               // vrefp
    case 330: { const NearestRounding n; EACH_W r.f[i] = 1.0f / std::sqrt(b.f[i]); break; }    // vrsqrtefp
    case 394: { const NearestRounding n; EACH_W r.f[i] = exp2f(b.f[i]); break; }               // vexptefp
    case 458: { const NearestRounding n; EACH_W r.f[i] = log2f(b.f[i]); break; }               // vlogefp
    case 522: { const NearestRounding n; EACH_W r.f[i] = rintf(b.f[i]); break; }    // vrfin
    case 586: EACH_W r.f[i] = truncf(b.f[i]); break;                                // vrfiz
    case 650: EACH_W r.f[i] = ceilf(b.f[i]); break;                                 // vrfip
    case 714: EACH_W r.f[i] = floorf(b.f[i]); break;                                // vrfim
    case 778: { const NearestRounding n; EACH_W r.f[i] = (float)std::ldexp((double)b.w[i], -(int)va); break; }   // vcfux
    case 842: { const NearestRounding n; EACH_W r.f[i] = (float)std::ldexp((double)b.sw[i], -(int)va); break; }  // vcfsx
    case 906:   // vctuxs
    case 970:   // vctsxs: scale by a power of two, then truncate with saturation
      EACH_W {
        const double x = std::ldexp((double)b.f[i], (int)va);
        if (x != x) {
          r.w[i] = 0;  // a NaN converts to zero without saturating
        } else if ((insn & 0x7FF) == 906) {
          r.w[i] = sat_u32(x >= 4294967296.0 ? 0x100000000ll : (x <= -1.0 ? -1 : (s64)x), sat);
        } else {
          r.sw[i] = sat_s32(x >= 2147483648.0 ? 0x80000000ll : (x <= -2147483649.0 ? -0x80000001ll : (s64)x), sat);
        }
      }
      break;
    // -- status and control --
    case 1540: W(r, 3) = s.vscr; break;  // mfvscr
    case 1604:                           // mtvscr
      s.vscr = W(b, 3) & 0x00010001u;
      return true;
    default:
      return false;
  }
  s.vr[vd] = r;
  if (sat) s.vscr |= 1;
  return true;
}

bool altivec_memory(PPCState& s, u32 insn) {
  const u32 vd = (insn >> 21) & 31, ra = (insn >> 16) & 31, rb = (insn >> 11) & 31;
  u32 ea = (ra ? s.gpr[ra] : 0) + s.gpr[rb];
  u8* const m = s.mem;
  V& v = s.vr[vd];
  switch ((insn >> 1) & 0x3FF) {
    case 103:
    case 359: {  // lvx, lvxl: the address is rounded down to 16 bytes
      const u8* p = m + (ea & ~15u);
      EACH_B B(v, i) = p[i];
      return true;
    }
    case 231:
    case 487: {  // stvx, stvxl
      u8* p = m + (ea & ~15u);
      EACH_B p[i] = B(v, i);
      return true;
    }
    // The element loads and stores move the one element the address selects.
    case 7:  // lvebx
      B(v, ea & 15) = m[ea];
      return true;
    case 39:  // lvehx
      ea &= ~1u;
      H(v, (ea & 15) >> 1) = (u16)((m[ea] << 8) | m[ea + 1]);
      return true;
    case 71:  // lvewx
      ea &= ~3u;
      W(v, (ea & 15) >> 2) = ((u32)m[ea] << 24) | ((u32)m[ea + 1] << 16) | ((u32)m[ea + 2] << 8) | m[ea + 3];
      return true;
    case 135:  // stvebx
      m[ea] = B(v, ea & 15);
      return true;
    case 167: {  // stvehx
      ea &= ~1u;
      const u16 x = H(v, (ea & 15) >> 1);
      m[ea] = (u8)(x >> 8);
      m[ea + 1] = (u8)x;
      return true;
    }
    case 199: {  // stvewx
      ea &= ~3u;
      const u32 x = W(v, (ea & 15) >> 2);
      m[ea] = (u8)(x >> 24);
      m[ea + 1] = (u8)(x >> 16);
      m[ea + 2] = (u8)(x >> 8);
      m[ea + 3] = (u8)x;
      return true;
    }
    // Permute-control vectors for unaligned access.
    case 6:  // lvsl
      EACH_B B(v, i) = (u8)((ea & 15) + (u32)i);
      return true;
    case 38:  // lvsr
      EACH_B B(v, i) = (u8)(16 - (ea & 15) + (u32)i);
      return true;
    default:
      return false;
  }
}
