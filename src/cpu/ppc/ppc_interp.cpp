// PowerPC 32-bit user-mode interpreter.
//
// This is the reference core: simple and exact rather than fast. A JIT can
// later share PPCState and be checked against it.
//
// Bit numbering follows the PowerPC books: bit 0 is the most significant.

#include "cpu/rounding.h"
#include <cfenv>
#include <cstdlib>
#include <unistd.h>
#include <cmath>
#include <ctime>

#include "cpu/ppc/ppc.h"
#include "cpu/ppc/ppc_jit.h"

// For working on cocaine: COCAINE_WATCH=address (in hexadecimal) logs each
// store of a word to that guest address, and where in the program it was made.
u32 g_ppc_watch = [] {
  const char* e = getenv("COCAINE_WATCH");
  return e ? (u32)strtoul(e, nullptr, 16) : 0xFFFFFFFFu;
}();

// For working on cocaine: COCAINE_CALLS=address,address (in hexadecimal)
// logs each direct call or jump to one of those places, with its arguments.
static u32 g_call_watch[16];
// COCAINE_NAN=1, for working on cocaine: report the first floating point
// instructions that make a NaN out of numbers, with their inputs.
static const bool g_nan_watch = getenv("COCAINE_NAN") != nullptr;
static int g_nan_reports = 0;
static void note_nan(u32 pc, u32 insn, double a, double b, double c, double r, bool uses_c) {
  if (!std::isnan(r) || std::isnan(a) || std::isnan(b) || (uses_c && std::isnan(c)) || g_nan_reports >= 24) return;
  static u32 seen[24];
  for (int k = 0; k < g_nan_reports; k++) {
    if (seen[k] == pc) return;  // each place once
  }
  seen[g_nan_reports++] = pc;
  log_warn("[%d] NaN made at 0x%08x (%08x) from %g %g %g", getpid(), pc, insn, a, b, c);
}
static int g_call_watch_count = [] {
  int n = 0;
  if (const char* e = getenv("COCAINE_CALLS")) {
    for (const char* p = e; *p && n < 16;) {
      char* end = nullptr;
      g_call_watch[n++] = (u32)strtoul(p, &end, 16);
      if (*end != ',') break;
      p = end + 1;
    }
  }
  return n;
}();

namespace {

constexpr u32 XER_SO = 0x80000000u;
constexpr u32 XER_OV = 0x40000000u;
constexpr u32 XER_CA = 0x20000000u;

// ---- guest memory (big-endian) ---------------------------------------------

inline u32 ld8(const u8* m, u32 a) { return m[a]; }
inline u32 ld16(const u8* m, u32 a) {
  u16 v;
  memcpy(&v, m + a, 2);
  return __builtin_bswap16(v);
}
inline u32 ld32(const u8* m, u32 a) {
  u32 v;
  memcpy(&v, m + a, 4);
  return __builtin_bswap32(v);
}
inline u64 ld64(const u8* m, u32 a) {
  u64 v;
  memcpy(&v, m + a, 8);
  return __builtin_bswap64(v);
}

// COCAINE_CALLS: a call to one of the watched addresses, by bl or bctrl.
static void note_call(PPCState& s, u8* m, u32 pc, u32 npc, u32 insn) {
  for (int k = 0; k < g_call_watch_count; k++) {
    if (g_call_watch[k] == npc) {
      log_warn("[%d] call of 0x%x from 0x%x (to return to 0x%x): %x %x %x %x %x %x, floats %g %g %g",
               getpid(), npc, pc, s.lr, s.gpr[3], s.gpr[4], s.gpr[5], s.gpr[6], s.gpr[7], s.gpr[8],
               s.fpr[1].d, s.fpr[2].d, s.fpr[3].d);
      if (insn & 1) s.watch_return = s.lr, s.watch_first = s.gpr[3];
      for (int r = 3; r <= 4; r++) {  // what the first two arguments point at, if they point
        const u32 a = s.gpr[r];
        if (a >= 0x1000 && a < 0xC0000000u && !(a & 3)) {
          log_warn("[%d]   r%d -> %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", getpid(), r,
                   ld32(m, a), ld32(m, a + 4), ld32(m, a + 8), ld32(m, a + 12), ld32(m, a + 16), ld32(m, a + 20),
                   ld32(m, a + 24), ld32(m, a + 28), ld32(m, a + 32), ld32(m, a + 36), ld32(m, a + 40),
                   ld32(m, a + 44));
        }
      }
    }
  }
}

inline void st8(u8* m, u32 a, u32 v) { m[a] = (u8)v; }
inline void st16(u8* m, u32 a, u32 v) {
  const u16 t = __builtin_bswap16((u16)v);
  memcpy(m + a, &t, 2);
}
inline void st32(u8* m, u32 a, u32 v) {
  v = __builtin_bswap32(v);
  memcpy(m + a, &v, 4);
}
inline void st64(u8* m, u32 a, u64 v) {
  v = __builtin_bswap64(v);
  memcpy(m + a, &v, 8);
}

// ---- integer helpers --------------------------------------------------------

inline u32 rotl32(u32 x, unsigned n) { return n ? (x << n) | (x >> (32 - n)) : x; }

// Mask with ones from bit `mb` through bit `me`, wrapping around if mb > me.
inline u32 mask32(unsigned mb, unsigned me) {
  const u32 a = 0xFFFFFFFFu >> mb, b = 0xFFFFFFFFu << (31 - me);
  return mb <= me ? (a & b) : (a | b);
}

inline void set_crf(PPCState& s, unsigned field, u32 v) {
  const unsigned sh = 28 - 4 * field;
  s.cr = (s.cr & ~(0xFu << sh)) | (v << sh);
}

// CR0 after a recording ("dot") instruction: LT/GT/EQ of the result, plus SO.
inline void set_cr0(PPCState& s, u32 r) {
  const u32 c = ((s32)r < 0) ? 8u : (r ? 4u : 2u);
  set_crf(s, 0, c | (s.xer >> 31));
}

inline u32 crbit(const PPCState& s, unsigned n) { return (s.cr >> (31 - n)) & 1; }

inline void set_crbit(PPCState& s, unsigned n, u32 v) {
  const u32 bit = 1u << (31 - n);
  s.cr = (s.cr & ~bit) | (v ? bit : 0);
}

inline void set_ov(PPCState& s, bool ov) {
  if (ov) {
    s.xer |= XER_OV | XER_SO;
  } else {
    s.xer &= ~XER_OV;
  }
}

// a + b + carry_in, the one adder behind every add/subtract form
// (subtracts pass ~a and a carry-in of 1).
inline u32 add3(PPCState& s, u32 a, u32 b, u32 cin, bool set_ca, bool oe) {
  const u64 wide = (u64)a + b + cin;
  const u32 r = (u32)wide;
  if (set_ca) s.xer = (s.xer & ~XER_CA) | ((wide >> 32) ? XER_CA : 0);
  if (oe) set_ov(s, (((a ^ r) & (b ^ r)) >> 31) != 0);
  return r;
}

inline u32 carry_in(const PPCState& s) { return (s.xer & XER_CA) ? 1 : 0; }

inline bool trap_cond(u32 to, u32 a, u32 b) {
  return ((to & 16) && (s32)a < (s32)b) || ((to & 8) && (s32)a > (s32)b) ||
         ((to & 4) && a == b) || ((to & 2) && a < b) || ((to & 1) && a > b);
}

// The time base ticks once per nanosecond; the kernel layer reports the
// matching frequency to the guest.
inline u64 timebase() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

// ---- floating point helpers -------------------------------------------------

inline void apply_rounding(u32 fpscr) {
  static const int kModes[4] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};
  host_rounding(kModes[fpscr & 3]);
}

// lfs: widen single bits to double bits without quieting signalling NaNs.
inline u64 single_to_double_bits(u32 w) {
  if ((w & 0x7F800000u) == 0x7F800000u) {
    return ((u64)(w & 0x80000000u) << 32) | 0x7FF0000000000000ull | ((u64)(w & 0x007FFFFFu) << 29);
  }
  float f;
  memcpy(&f, &w, 4);
  const double d = (double)f;  // exact for every finite single
  u64 u;
  memcpy(&u, &d, 8);
  return u;
}

// stfs: the PowerPC "SINGLE" conversion. It moves bits rather than rounding,
// which is exact whenever the register already holds a single-precision value.
inline u32 double_to_single_bits(u64 d) {
  const u32 exp = (u32)(d >> 52) & 0x7FF;
  if (exp > 896 || (d & 0x7FFFFFFFFFFFFFFFull) == 0) {
    return (u32)((d >> 32) & 0xC0000000u) | (u32)((d >> 29) & 0x3FFFFFFFu);
  }
  if (exp >= 874) {  // result is a single-precision denormal
    const u64 frac = (1ull << 52) | (d & 0x000FFFFFFFFFFFFFull);
    const int shift = -126 - ((int)exp - 1023);
    return (u32)((d >> 32) & 0x80000000u) | (u32)(((frac >> shift) >> 29) & 0x007FFFFFu);
  }
  return (u32)((d >> 32) & 0x80000000u);  // too small: undefined on real hardware
}

inline double round_single(double x) { return (double)(float)x; }

// Single-precision fused multiply-add, rounded once when the inputs are
// themselves singles (the normal case).
inline double fma_single(double a, double c, double b) {
  const float fa = (float)a, fc = (float)c, fb = (float)b;
  if ((double)fa == a && (double)fc == c && (double)fb == b) return (double)fmaf(fa, fc, fb);
  return round_single(std::fma(a, c, b));
}

// The final step of fnmadd/fnmsub: negate the rounded sum, but leave a NaN
// alone. Deliberately out of line: inlined next to an fma, the compiler folds
// the negation into an ARM multiply-add that negates before adding, so a sum
// that cancels comes out as +0 where PowerPC gives -0.
__attribute__((noinline)) double negate_result(double r) {
  if (r != r) return r;
  u64 bits;
  memcpy(&bits, &r, 8);
  bits ^= 0x8000000000000000ull;
  memcpy(&r, &bits, 8);
  return r;
}

// Sets LT/GT/EQ/UN for a floating compare.
inline u32 fp_compare(double a, double b) {
  if (a < b) return 8;
  if (a > b) return 4;
  if (a == b) return 2;
  return 1;
}

inline u64 to_int32_bits(double x, bool truncate) {
  s32 r;
  if (std::isnan(x)) {
    r = INT32_MIN;
  } else {
    const double y = truncate ? std::trunc(x) : std::nearbyint(x);
    if (y >= 2147483648.0) {
      r = INT32_MAX;
    } else if (y < -2147483648.0) {
      r = INT32_MIN;
    } else {
      r = (s32)y;
    }
  }
  return 0xFFF8000000000000ull | (u32)r;
}

inline u64 to_int64_bits(double x, bool truncate) {
  if (std::isnan(x)) return (u64)INT64_MIN;
  const double y = truncate ? std::trunc(x) : std::nearbyint(x);
  if (y >= 9223372036854775808.0) return (u64)INT64_MAX;
  if (y < -9223372036854775808.0) return (u64)INT64_MIN;
  return (u64)(s64)y;
}

}  // namespace

// ---- instruction fields -----------------------------------------------------

#define RD ((insn >> 21) & 31)
#define RS RD
#define RA ((insn >> 16) & 31)
#define RB ((insn >> 11) & 31)
#define RC ((insn >> 6) & 31)
#define SH RB
#define MB RC
#define ME ((insn >> 1) & 31)
#define SIMM ((u32)(s32)(s16)insn)
#define UIMM (insn & 0xFFFFu)
#define RCBIT (insn & 1)
#define OE ((insn >> 10) & 1)
#define CRFD ((insn >> 23) & 7)
#define CRFS ((insn >> 18) & 7)

#define G(n) s.gpr[n]
#define FD s.fpr[RD]
#define FA s.fpr[RA].d
#define FB s.fpr[RB].d
#define FC s.fpr[RC].d

// Effective addresses: displacement and indexed, plain and "with update".
#define EA_D ((RA ? G(RA) : 0) + SIMM)
#define EA_DU (G(RA) + SIMM)
#define EA_X ((RA ? G(RA) : 0) + G(RB))
#define EA_XU (G(RA) + G(RB))

#define REC(r) \
  do { \
    if (RCBIT) set_cr0(s, (r)); \
  } while (0)
#define FP_REC() \
  do { \
    if (RCBIT) set_crf(s, 1, s.fpscr >> 28); \
  } while (0)

bool ppc_debugging(const PPCState& s) {
  return s.trace || g_call_watch_count != 0 || g_ppc_watch != 0xFFFFFFFFu || g_nan_watch;
}

void ppc_apply_rounding(u32 fpscr) { apply_rounding(fpscr); }

PPCStop ppc_interp(PPCState& s, u64 max_insns) {
  u8* const m = s.mem;
  u64 left = max_insns;
  u32 pc = s.pc;
  apply_rounding(s.fpscr);

  auto stop = [&](PPCStopKind kind, u32 at, u32 insn) {
    s.pc = at;
    s.icount += max_insns - left;
    if (!t_keep_guest_rounding) host_rounding(FE_TONEAREST);  // host code expects the default mode
    return PPCStop{kind, at, insn};
  };

  for (;;) {
    if (left == 0) return stop(PPCStopKind::Budget, pc, 0);
    s.pc = pc;  // kept current so a memory fault can report where it happened
    const u32 insn = ld32(m, pc);
    u32 npc = pc + 4;
    if (__builtin_expect(s.trace, 0)) {
      std::fprintf(stderr, "%08x: %08x", pc, insn);
      if (s.trace_registers) {
        std::fprintf(stderr, " |");
        for (int i = 0; i < 13; i++) std::fprintf(stderr, " r%d=%08x", i, s.gpr[i]);
        std::fprintf(stderr, " lr=%08x ctr=%08x cr=%08x", s.lr, s.ctr, s.cr);
      }
      std::fputc('\n', stderr);
    }

    switch (insn >> 26) {
      case 1:  // host call
        left--;
        return stop(PPCStopKind::HostCall, pc + 4, insn);
      case 3:  // twi
        if (trap_cond(RD, G(RA), SIMM)) return stop(PPCStopKind::Trap, pc, insn);
        break;
      case 7:  // mulli
        G(RD) = (u32)((s64)(s32)G(RA) * (s32)SIMM);
        break;
      case 8:  // subfic
        G(RD) = add3(s, ~G(RA), SIMM, 1, true, false);
        break;
      case 10: {  // cmpli
        const u32 a = G(RA), b = UIMM;
        set_crf(s, CRFD, (a < b ? 8u : a > b ? 4u : 2u) | (s.xer >> 31));
        break;
      }
      case 11: {  // cmpi
        const s32 a = (s32)G(RA), b = (s32)SIMM;
        set_crf(s, CRFD, (a < b ? 8u : a > b ? 4u : 2u) | (s.xer >> 31));
        break;
      }
      case 12:  // addic
        G(RD) = add3(s, G(RA), SIMM, 0, true, false);
        break;
      case 13: {  // addic.
        const u32 r = add3(s, G(RA), SIMM, 0, true, false);
        G(RD) = r;
        set_cr0(s, r);
        break;
      }
      case 14:  // addi
        G(RD) = (RA ? G(RA) : 0) + SIMM;
        break;
      case 15:  // addis
        G(RD) = (RA ? G(RA) : 0) + (UIMM << 16);
        break;

      case 16: {  // bc
        const u32 bo = RD, bi = RA;
        if (!(bo & 4)) s.ctr--;
        const bool ctr_ok = (bo & 4) || ((s.ctr != 0) != (((bo >> 1) & 1) != 0));
        const bool cond_ok = (bo & 16) || (crbit(s, bi) == ((bo >> 3) & 1));
        if (insn & 1) s.lr = pc + 4;
        if (ctr_ok && cond_ok) {
          const u32 bd = (u32)(s32)(s16)(insn & 0xFFFC);
          npc = (insn & 2) ? bd : pc + bd;
        }
        break;
      }
      case 17:  // sc
        left--;
        return stop(PPCStopKind::Syscall, pc + 4, insn);
      case 18: {  // b
        const u32 li = (u32)(((s32)(insn << 6)) >> 6) & ~3u;
        if (insn & 1) s.lr = pc + 4;
        npc = (insn & 2) ? li : pc + li;
        if (__builtin_expect(g_call_watch_count != 0, 0)) note_call(s, m, pc, npc, insn);
        break;
      }

      case 19: {
        const u32 xo = (insn >> 1) & 0x3FF;
        switch (xo) {
          case 0:  // mcrf
            set_crf(s, CRFD, (s.cr >> (28 - 4 * CRFS)) & 0xF);
            break;
          case 16: {  // bclr
            const u32 bo = RD, bi = RA;
            if (!(bo & 4)) s.ctr--;
            const bool ctr_ok = (bo & 4) || ((s.ctr != 0) != (((bo >> 1) & 1) != 0));
            const bool cond_ok = (bo & 16) || (crbit(s, bi) == ((bo >> 3) & 1));
            const u32 target = s.lr & ~3u;
            if (insn & 1) s.lr = pc + 4;
            if (ctr_ok && cond_ok) npc = target;
            if (__builtin_expect(g_call_watch_count != 0, 0) && npc == s.watch_return) {
              log_warn("[%d]   returned %x (float %g)", getpid(), s.gpr[3], s.fpr[1].d);
              if (const u32 a = s.watch_first; a >= 0x1000 && a < 0xC0000000u && !(a & 3)) {
                log_warn("[%d]   its first argument -> %08x %08x %08x %08x %08x %08x", getpid(), ld32(m, a),
                         ld32(m, a + 4), ld32(m, a + 8), ld32(m, a + 12), ld32(m, a + 16), ld32(m, a + 20));
              }
              s.watch_return = 0;
            }
            break;
          }
          case 528: {  // bcctr
            const u32 bo = RD, bi = RA;
            const bool cond_ok = (bo & 16) || (crbit(s, bi) == ((bo >> 3) & 1));
            if (insn & 1) s.lr = pc + 4;
            if (cond_ok) npc = s.ctr & ~3u;
            if (__builtin_expect(g_call_watch_count != 0, 0) && cond_ok) note_call(s, m, pc, npc, insn);
            break;
          }
          case 33: set_crbit(s, RD, !(crbit(s, RA) | crbit(s, RB))); break;   // crnor
          case 129: set_crbit(s, RD, crbit(s, RA) & !crbit(s, RB)); break;    // crandc
          case 193: set_crbit(s, RD, crbit(s, RA) ^ crbit(s, RB)); break;     // crxor
          case 225: set_crbit(s, RD, !(crbit(s, RA) & crbit(s, RB))); break;  // crnand
          case 257: set_crbit(s, RD, crbit(s, RA) & crbit(s, RB)); break;     // crand
          case 289: set_crbit(s, RD, !(crbit(s, RA) ^ crbit(s, RB))); break;  // creqv
          case 417: set_crbit(s, RD, crbit(s, RA) | !crbit(s, RB)); break;    // crorc
          case 449: set_crbit(s, RD, crbit(s, RA) | crbit(s, RB)); break;     // cror
          case 150: break;                                                    // isync
          default: return stop(PPCStopKind::Illegal, pc, insn);
        }
        break;
      }

      case 20: {  // rlwimi
        const u32 mask = mask32(MB, ME);
        const u32 r = (rotl32(G(RS), SH) & mask) | (G(RA) & ~mask);
        G(RA) = r;
        REC(r);
        break;
      }
      case 21: {  // rlwinm
        const u32 r = rotl32(G(RS), SH) & mask32(MB, ME);
        G(RA) = r;
        REC(r);
        break;
      }
      case 23: {  // rlwnm
        const u32 r = rotl32(G(RS), G(RB) & 31) & mask32(MB, ME);
        G(RA) = r;
        REC(r);
        break;
      }
      case 24: G(RA) = G(RS) | UIMM; break;          // ori
      case 25: G(RA) = G(RS) | (UIMM << 16); break;  // oris
      case 26: G(RA) = G(RS) ^ UIMM; break;          // xori
      case 27: G(RA) = G(RS) ^ (UIMM << 16); break;  // xoris
      case 28: {                                     // andi.
        const u32 r = G(RS) & UIMM;
        G(RA) = r;
        set_cr0(s, r);
        break;
      }
      case 29: {  // andis.
        const u32 r = G(RS) & (UIMM << 16);
        G(RA) = r;
        set_cr0(s, r);
        break;
      }

      case 31: {
        const u32 xo = (insn >> 1) & 0x3FF;
        switch (xo) {
          case 0: {  // cmp
            const s32 a = (s32)G(RA), b = (s32)G(RB);
            set_crf(s, CRFD, (a < b ? 8u : a > b ? 4u : 2u) | (s.xer >> 31));
            break;
          }
          case 32: {  // cmpl
            const u32 a = G(RA), b = G(RB);
            set_crf(s, CRFD, (a < b ? 8u : a > b ? 4u : 2u) | (s.xer >> 31));
            break;
          }
          case 4:  // tw
            if (trap_cond(RD, G(RA), G(RB))) return stop(PPCStopKind::Trap, pc, insn);
            break;

          // -- add / subtract family; the +512 cases are the overflow-recording forms --
          case 266: case 778: { const u32 r = add3(s, G(RA), G(RB), 0, false, OE); G(RD) = r; REC(r); break; }               // add
          case 10: case 522: { const u32 r = add3(s, G(RA), G(RB), 0, true, OE); G(RD) = r; REC(r); break; }                 // addc
          case 138: case 650: { const u32 r = add3(s, G(RA), G(RB), carry_in(s), true, OE); G(RD) = r; REC(r); break; }      // adde
          case 202: case 714: { const u32 r = add3(s, G(RA), 0, carry_in(s), true, OE); G(RD) = r; REC(r); break; }          // addze
          case 234: case 746: { const u32 r = add3(s, G(RA), 0xFFFFFFFFu, carry_in(s), true, OE); G(RD) = r; REC(r); break; } // addme
          case 40: case 552: { const u32 r = add3(s, ~G(RA), G(RB), 1, false, OE); G(RD) = r; REC(r); break; }               // subf
          case 8: case 520: { const u32 r = add3(s, ~G(RA), G(RB), 1, true, OE); G(RD) = r; REC(r); break; }                 // subfc
          case 136: case 648: { const u32 r = add3(s, ~G(RA), G(RB), carry_in(s), true, OE); G(RD) = r; REC(r); break; }     // subfe
          case 200: case 712: { const u32 r = add3(s, ~G(RA), 0, carry_in(s), true, OE); G(RD) = r; REC(r); break; }         // subfze
          case 232: case 744: { const u32 r = add3(s, ~G(RA), 0xFFFFFFFFu, carry_in(s), true, OE); G(RD) = r; REC(r); break; } // subfme
          case 104: case 616: { const u32 r = add3(s, ~G(RA), 0, 1, false, OE); G(RD) = r; REC(r); break; }                  // neg

          // -- multiply / divide --
          case 235: case 747: {  // mullw
            const s64 wide = (s64)(s32)G(RA) * (s32)G(RB);
            const u32 r = (u32)wide;
            if (OE) set_ov(s, wide != (s64)(s32)r);
            G(RD) = r;
            REC(r);
            break;
          }
          case 75: {  // mulhw
            const u32 r = (u32)(((s64)(s32)G(RA) * (s32)G(RB)) >> 32);
            G(RD) = r;
            REC(r);
            break;
          }
          case 11: {  // mulhwu
            const u32 r = (u32)(((u64)G(RA) * G(RB)) >> 32);
            G(RD) = r;
            REC(r);
            break;
          }
          case 491: case 1003: {  // divw
            const s32 a = (s32)G(RA), b = (s32)G(RB);
            const bool bad = b == 0 || (a == INT32_MIN && b == -1);
            // The result of a bad divide is undefined; this matches what G4s return.
            const u32 r = bad ? (a < 0 ? 0xFFFFFFFFu : 0) : (u32)(a / b);
            if (OE) set_ov(s, bad);
            G(RD) = r;
            REC(r);
            break;
          }
          case 459: case 971: {  // divwu
            const u32 a = G(RA), b = G(RB);
            const u32 r = b ? a / b : 0;
            if (OE) set_ov(s, b == 0);
            G(RD) = r;
            REC(r);
            break;
          }

          // -- logic --
          case 28: { const u32 r = G(RS) & G(RB); G(RA) = r; REC(r); break; }     // and
          case 60: { const u32 r = G(RS) & ~G(RB); G(RA) = r; REC(r); break; }    // andc
          case 444: { const u32 r = G(RS) | G(RB); G(RA) = r; REC(r); break; }    // or
          case 412: { const u32 r = G(RS) | ~G(RB); G(RA) = r; REC(r); break; }   // orc
          case 316: { const u32 r = G(RS) ^ G(RB); G(RA) = r; REC(r); break; }    // xor
          case 476: { const u32 r = ~(G(RS) & G(RB)); G(RA) = r; REC(r); break; } // nand
          case 124: { const u32 r = ~(G(RS) | G(RB)); G(RA) = r; REC(r); break; } // nor
          case 284: { const u32 r = ~(G(RS) ^ G(RB)); G(RA) = r; REC(r); break; } // eqv
          case 954: { const u32 r = (u32)(s32)(s8)G(RS); G(RA) = r; REC(r); break; }  // extsb
          case 922: { const u32 r = (u32)(s32)(s16)G(RS); G(RA) = r; REC(r); break; } // extsh
          case 26: {  // cntlzw
            const u32 v = G(RS);
            const u32 r = v ? (u32)__builtin_clz(v) : 32;
            G(RA) = r;
            REC(r);
            break;
          }

          // -- shifts --
          case 24: {  // slw
            const u32 n = G(RB) & 63;
            const u32 r = n < 32 ? G(RS) << n : 0;
            G(RA) = r;
            REC(r);
            break;
          }
          case 536: {  // srw
            const u32 n = G(RB) & 63;
            const u32 r = n < 32 ? G(RS) >> n : 0;
            G(RA) = r;
            REC(r);
            break;
          }
          case 792: case 824: {  // sraw, srawi
            const u32 n = (xo == 824) ? SH : (G(RB) & 63);
            const u32 v = G(RS);
            u32 r;
            bool ca;
            if (n == 0) {
              r = v;
              ca = false;
            } else if (n < 32) {
              r = (u32)((s32)v >> n);
              ca = (s32)v < 0 && (v & ((1u << n) - 1)) != 0;
            } else {
              r = (s32)v < 0 ? 0xFFFFFFFFu : 0;
              ca = (s32)v < 0;
            }
            s.xer = (s.xer & ~XER_CA) | (ca ? XER_CA : 0);
            G(RA) = r;
            REC(r);
            break;
          }

          // -- condition and special registers --
          case 19: G(RD) = s.cr; break;  // mfcr
          case 144: {                    // mtcrf
            const u32 crm = (insn >> 12) & 0xFF;
            u32 mask = 0;
            for (int i = 0; i < 8; i++) {
              if (crm & (0x80u >> i)) mask |= 0xF0000000u >> (4 * i);
            }
            s.cr = (s.cr & ~mask) | (G(RS) & mask);
            break;
          }
          case 512:  // mcrxr
            set_crf(s, CRFD, s.xer >> 28);
            s.xer &= 0x0FFFFFFFu;
            break;
          case 339: {  // mfspr
            const u32 spr = ((insn >> 16) & 0x1F) | ((insn >> 6) & 0x3E0);
            switch (spr) {
              case 1: G(RD) = s.xer; break;
              case 8: G(RD) = s.lr; break;
              case 9: G(RD) = s.ctr; break;
              case 256: G(RD) = s.vrsave; break;
              case 259: G(RD) = s.sprg3; break;
              case 268: G(RD) = (u32)timebase(); break;
              case 269: G(RD) = (u32)(timebase() >> 32); break;
              default: return stop(PPCStopKind::Illegal, pc, insn);
            }
            break;
          }
          case 371: {  // mftb
            const u32 tbr = ((insn >> 16) & 0x1F) | ((insn >> 6) & 0x3E0);
            if (tbr == 268) {
              G(RD) = (u32)timebase();
            } else if (tbr == 269) {
              G(RD) = (u32)(timebase() >> 32);
            } else {
              return stop(PPCStopKind::Illegal, pc, insn);
            }
            break;
          }
          case 467: {  // mtspr
            const u32 spr = ((insn >> 16) & 0x1F) | ((insn >> 6) & 0x3E0);
            switch (spr) {
              case 1: s.xer = G(RS) & 0xE000007Fu; break;
              case 8: s.lr = G(RS); break;
              case 9: s.ctr = G(RS); break;
              case 256: s.vrsave = G(RS); break;
              default: return stop(PPCStopKind::Illegal, pc, insn);
            }
            break;
          }

          // -- integer loads --
          case 23: G(RD) = ld32(m, EA_X); break;  // lwzx
          case 55: { const u32 ea = EA_XU; G(RD) = ld32(m, ea); G(RA) = ea; break; }  // lwzux
          case 87: G(RD) = ld8(m, EA_X); break;   // lbzx
          case 119: { const u32 ea = EA_XU; G(RD) = ld8(m, ea); G(RA) = ea; break; }  // lbzux
          case 279: G(RD) = ld16(m, EA_X); break; // lhzx
          case 311: { const u32 ea = EA_XU; G(RD) = ld16(m, ea); G(RA) = ea; break; } // lhzux
          case 343: G(RD) = (u32)(s32)(s16)ld16(m, EA_X); break;  // lhax
          case 375: { const u32 ea = EA_XU; G(RD) = (u32)(s32)(s16)ld16(m, ea); G(RA) = ea; break; }  // lhaux
          case 534: G(RD) = __builtin_bswap32(ld32(m, EA_X)); break;          // lwbrx
          case 790: G(RD) = __builtin_bswap16((u16)ld16(m, EA_X)); break;     // lhbrx

          // -- integer stores --
          case 151: st32(m, EA_X, G(RS)); break;  // stwx
          case 183: { const u32 ea = EA_XU; st32(m, ea, G(RS)); G(RA) = ea; break; }  // stwux
          case 215: st8(m, EA_X, G(RS)); break;   // stbx
          case 247: { const u32 ea = EA_XU; st8(m, ea, G(RS)); G(RA) = ea; break; }   // stbux
          case 407: st16(m, EA_X, G(RS)); break;  // sthx
          case 439: { const u32 ea = EA_XU; st16(m, ea, G(RS)); G(RA) = ea; break; }  // sthux
          case 662: st32(m, EA_X, __builtin_bswap32(G(RS))); break;           // stwbrx
          case 918: st16(m, EA_X, __builtin_bswap16((u16)G(RS))); break;      // sthbrx

          // -- atomic pair --
          case 20: {  // lwarx
            const u32 ea = EA_X;
            const u32 v = ld32(m, ea);
            s.reserve_addr = ea;
            s.reserve_val = v;
            s.reserved = true;
            G(RD) = v;
            break;
          }
          case 150: {  // stwcx.
            const u32 ea = EA_X;
            bool ok = false;
            if (s.reserved && s.reserve_addr == ea) {
              u32 expect = __builtin_bswap32(s.reserve_val);
              const u32 desired = __builtin_bswap32(G(RS));
              ok = __atomic_compare_exchange_n((u32*)(m + ea), &expect, desired, false,
                                               __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            }
            s.reserved = false;
            set_crf(s, 0, (ok ? 2u : 0u) | (s.xer >> 31));
            break;
          }

          // -- string (byte-count) loads and stores --
          case 597: case 533: {  // lswi, lswx
            u32 ea, n;
            if (xo == 597) {
              ea = RA ? G(RA) : 0;
              n = RB ? RB : 32;
            } else {
              ea = EA_X;
              n = s.xer & 0x7F;
            }
            u32 r = RD;
            int shift = 24;
            for (u32 i = 0; i < n; i++) {
              if (shift == 24) G(r) = 0;
              G(r) |= ld8(m, ea + i) << shift;
              if (shift == 0) {
                shift = 24;
                r = (r + 1) & 31;
              } else {
                shift -= 8;
              }
            }
            break;
          }
          case 725: case 661: {  // stswi, stswx
            u32 ea, n;
            if (xo == 725) {
              ea = RA ? G(RA) : 0;
              n = RB ? RB : 32;
            } else {
              ea = EA_X;
              n = s.xer & 0x7F;
            }
            u32 r = RS;
            int shift = 24;
            for (u32 i = 0; i < n; i++) {
              st8(m, ea + i, G(r) >> shift);
              if (shift == 0) {
                shift = 24;
                r = (r + 1) & 31;
              } else {
                shift -= 8;
              }
            }
            break;
          }

          // -- cache and ordering hints: nothing to do in an interpreter --
          case 54:    // dcbst
          case 86:    // dcbf
          case 246:   // dcbtst
          case 278:   // dcbt
          case 598:   // sync
          case 854:   // eieio
          case 342:   // dst
          case 374:   // dstst
          case 822:   // dss
            break;
          case 982:  // icbi: nothing to do here, but translations of that code must go
            ppc_jit_invalidate(EA_X & ~31u, 32);
            break;
          case 1014:  // dcbz: G3/G4 cache lines are 32 bytes
            memset(m + (EA_X & ~31u), 0, 32);
            break;

          // -- floating point loads and stores, indexed --
          case 535: FD.u = single_to_double_bits(ld32(m, EA_X)); break;  // lfsx
          case 567: { const u32 ea = EA_XU; FD.u = single_to_double_bits(ld32(m, ea)); G(RA) = ea; break; }  // lfsux
          case 599: FD.u = ld64(m, EA_X); break;  // lfdx
          case 631: { const u32 ea = EA_XU; FD.u = ld64(m, ea); G(RA) = ea; break; }  // lfdux
          case 663: st32(m, EA_X, double_to_single_bits(FD.u)); break;  // stfsx
          case 695: { const u32 ea = EA_XU; st32(m, ea, double_to_single_bits(FD.u)); G(RA) = ea; break; }  // stfsux
          case 727: st64(m, EA_X, FD.u); break;  // stfdx
          case 759: { const u32 ea = EA_XU; st64(m, ea, FD.u); G(RA) = ea; break; }  // stfdux
          case 983: st32(m, EA_X, (u32)FD.u); break;  // stfiwx

          default:  // what's left are the AltiVec loads and stores
            if (!altivec_memory(s, insn)) return stop(PPCStopKind::Illegal, pc, insn);
            break;
        }
        break;
      }

      // -- integer loads and stores, displacement forms --
      case 32: G(RD) = ld32(m, EA_D); break;  // lwz
      case 33: { const u32 ea = EA_DU; G(RD) = ld32(m, ea); G(RA) = ea; break; }  // lwzu
      case 34: G(RD) = ld8(m, EA_D); break;   // lbz
      case 35: { const u32 ea = EA_DU; G(RD) = ld8(m, ea); G(RA) = ea; break; }   // lbzu
      case 36: {  // stw
        const u32 ea = EA_D;
        if (__builtin_expect(ea == g_ppc_watch, 0)) {
          log_warn("watched word 0x%x: 0x%x -> 0x%x by the code at 0x%x (called from 0x%x)", ea,
                   ld32(m, ea), G(RS), s.pc, s.lr);
        }
        st32(m, ea, G(RS));
        break;
      }
      case 37: { const u32 ea = EA_DU; st32(m, ea, G(RS)); G(RA) = ea; break; }   // stwu
      case 38: st8(m, EA_D, G(RS)); break;    // stb
      case 39: { const u32 ea = EA_DU; st8(m, ea, G(RS)); G(RA) = ea; break; }    // stbu
      case 40: G(RD) = ld16(m, EA_D); break;  // lhz
      case 41: { const u32 ea = EA_DU; G(RD) = ld16(m, ea); G(RA) = ea; break; }  // lhzu
      case 42: G(RD) = (u32)(s32)(s16)ld16(m, EA_D); break;  // lha
      case 43: { const u32 ea = EA_DU; G(RD) = (u32)(s32)(s16)ld16(m, ea); G(RA) = ea; break; }  // lhau
      case 44: st16(m, EA_D, G(RS)); break;   // sth
      case 45: { const u32 ea = EA_DU; st16(m, ea, G(RS)); G(RA) = ea; break; }   // sthu
      case 46: {  // lmw
        u32 ea = EA_D;
        for (u32 r = RD; r < 32; r++, ea += 4) G(r) = ld32(m, ea);
        break;
      }
      case 47: {  // stmw
        u32 ea = EA_D;
        for (u32 r = RS; r < 32; r++, ea += 4) st32(m, ea, G(r));
        break;
      }

      // -- floating point loads and stores, displacement forms --
      case 48: FD.u = single_to_double_bits(ld32(m, EA_D)); break;  // lfs
      case 49: { const u32 ea = EA_DU; FD.u = single_to_double_bits(ld32(m, ea)); G(RA) = ea; break; }  // lfsu
      case 50: FD.u = ld64(m, EA_D); break;  // lfd
      case 51: { const u32 ea = EA_DU; FD.u = ld64(m, ea); G(RA) = ea; break; }  // lfdu
      case 52: st32(m, EA_D, double_to_single_bits(FD.u)); break;  // stfs
      case 53: { const u32 ea = EA_DU; st32(m, ea, double_to_single_bits(FD.u)); G(RA) = ea; break; }  // stfsu
      case 54: st64(m, EA_D, FD.u); break;  // stfd
      case 55: { const u32 ea = EA_DU; st64(m, ea, FD.u); G(RA) = ea; break; }  // stfdu

      case 59: {  // single-precision arithmetic
        const double nan_a = FA, nan_b = FB, nan_c = FC;
        switch ((insn >> 1) & 31) {
          case 18: FD.d = round_single(FA / FB); break;           // fdivs
          case 20: FD.d = round_single(FA - FB); break;           // fsubs
          case 21: FD.d = round_single(FA + FB); break;           // fadds
          case 22: FD.d = round_single(std::sqrt(FB)); break;     // fsqrts
          case 24: FD.d = round_single(1.0 / FB); break;          // fres
          case 25: FD.d = round_single(FA * FC); break;           // fmuls
          case 28: FD.d = fma_single(FA, FC, -FB); break;         // fmsubs
          case 29: FD.d = fma_single(FA, FC, FB); break;          // fmadds
          case 30: FD.d = negate_result(fma_single(FA, FC, -FB)); break;  // fnmsubs
          case 31: FD.d = negate_result(fma_single(FA, FC, FB)); break;   // fnmadds
          default: return stop(PPCStopKind::Illegal, pc, insn);
        }
        if (__builtin_expect(g_nan_watch, 0)) {
          const u32 x = (insn >> 1) & 31;
          note_nan(pc, insn, x == 22 || x == 24 ? 0 : nan_a, nan_b, nan_c, FD.d, x == 25 || x >= 28);
        }
        FP_REC();
        break;
      }

      case 63: {  // double-precision arithmetic and FPSCR
        bool done = true;
        const double nan_a = FA, nan_b = FB, nan_c = FC;
        switch ((insn >> 1) & 31) {
          case 18: FD.d = FA / FB; break;                    // fdiv
          case 20: FD.d = FA - FB; break;                    // fsub
          case 21: FD.d = FA + FB; break;                    // fadd
          case 22: FD.d = std::sqrt(FB); break;              // fsqrt
          case 23: FD.d = (FA >= 0.0) ? FC : FB; break;      // fsel
          case 25: FD.d = FA * FC; break;                    // fmul
          case 26: FD.d = 1.0 / std::sqrt(FB); break;        // frsqrte
          case 28: FD.d = std::fma(FA, FC, -FB); break;      // fmsub
          case 29: FD.d = std::fma(FA, FC, FB); break;       // fmadd
          case 30: FD.d = negate_result(std::fma(FA, FC, -FB)); break;  // fnmsub
          case 31: FD.d = negate_result(std::fma(FA, FC, FB)); break;   // fnmadd
          default: done = false; break;
        }
        if (done) {
          if (__builtin_expect(g_nan_watch, 0)) {
            const u32 x = (insn >> 1) & 31;
            if (x != 23) note_nan(pc, insn, x == 22 || x == 26 ? 0 : nan_a, nan_b, nan_c, FD.d, x == 25 || x >= 28);
          }
          FP_REC();
          break;
        }
        switch ((insn >> 1) & 0x3FF) {
          case 0: case 32: {  // fcmpu, fcmpo
            const u32 c = fp_compare(FA, FB);
            set_crf(s, CRFD, c);
            s.fpscr = (s.fpscr & ~0x0000F000u) | (c << 12);
            break;
          }
          case 12: FD.d = round_single(FB); FP_REC(); break;                    // frsp
          case 14: FD.u = to_int32_bits(FB, false); FP_REC(); break;            // fctiw
          case 15: FD.u = to_int32_bits(FB, true); FP_REC(); break;             // fctiwz
          case 814: FD.u = to_int64_bits(FB, false); FP_REC(); break;           // fctid
          case 815: FD.u = to_int64_bits(FB, true); FP_REC(); break;            // fctidz
          case 846: FD.d = (double)(s64)s.fpr[RB].u; FP_REC(); break;           // fcfid
          case 40: FD.u = s.fpr[RB].u ^ 0x8000000000000000ull; FP_REC(); break; // fneg
          case 72: FD.u = s.fpr[RB].u; FP_REC(); break;                         // fmr
          case 136: FD.u = s.fpr[RB].u | 0x8000000000000000ull; FP_REC(); break;  // fnabs
          case 264: FD.u = s.fpr[RB].u & 0x7FFFFFFFFFFFFFFFull; FP_REC(); break;  // fabs
          case 583: FD.u = 0xFFF8000000000000ull | s.fpscr; FP_REC(); break;    // mffs
          case 64: {  // mcrfs
            static const u32 kClear[8] = {0x90000000u, 0x0F000000u, 0x00F00000u, 0x00080000u,
                                          0,           0x00000700u, 0,           0};
            set_crf(s, CRFD, (s.fpscr >> (28 - 4 * CRFS)) & 0xF);
            s.fpscr &= ~kClear[CRFS];
            break;
          }
          case 38:  // mtfsb1
            s.fpscr |= 1u << (31 - RD);
            apply_rounding(s.fpscr);
            FP_REC();
            break;
          case 70:  // mtfsb0
            s.fpscr &= ~(1u << (31 - RD));
            apply_rounding(s.fpscr);
            FP_REC();
            break;
          case 134: {  // mtfsfi
            const unsigned sh = 28 - 4 * CRFD;
            s.fpscr = (s.fpscr & ~(0xFu << sh)) | (((insn >> 12) & 0xF) << sh);
            apply_rounding(s.fpscr);
            FP_REC();
            break;
          }
          case 711: {  // mtfsf
            const u32 fm = (insn >> 17) & 0xFF;
            u32 mask = 0;
            for (int i = 0; i < 8; i++) {
              if (fm & (0x80u >> i)) mask |= 0xF0000000u >> (4 * i);
            }
            s.fpscr = (s.fpscr & ~mask) | ((u32)s.fpr[RB].u & mask);
            apply_rounding(s.fpscr);
            FP_REC();
            break;
          }
          default: return stop(PPCStopKind::Illegal, pc, insn);
        }
        break;
      }

      case 4:  // AltiVec
        if (!altivec_exec(s, insn)) return stop(PPCStopKind::Illegal, pc, insn);
        break;

      default:
        return stop(PPCStopKind::Illegal, pc, insn);
    }

    pc = npc;
    left--;
  }
}
