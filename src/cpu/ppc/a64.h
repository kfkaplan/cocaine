#pragma once

#include "common.h"

// Encodings of the ARM64 instructions the PowerPC translator emits.
//
// Each function returns one instruction word. Registers are numbers 0 to 30;
// 31 is the zero register or the stack pointer, whichever the instruction
// takes there. The "w" forms work on 32 bits, the "x" forms on 64.
// tests/a64-check.sh assembles the same instructions with the system's
// assembler and compares, so a wrong bit here is caught there.
namespace a64 {

enum Cond : u32 { EQ, NE, HS, LO, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE, AL };
constexpr Cond invert(Cond c) { return (Cond)(c ^ 1); }
constexpr int ZR = 31, SP = 31;

// -- arithmetic and logic on registers (optionally with the second shifted left) --
constexpr u32 rrr(u32 op, int rd, int rn, int rm, u32 lsl = 0) {
  return op | (u32)rm << 16 | lsl << 10 | (u32)rn << 5 | (u32)rd;
}
constexpr u32 addw(int rd, int rn, int rm, u32 lsl = 0) { return rrr(0x0B000000, rd, rn, rm, lsl); }
constexpr u32 addsw(int rd, int rn, int rm, u32 lsl = 0) { return rrr(0x2B000000, rd, rn, rm, lsl); }
constexpr u32 subw(int rd, int rn, int rm) { return rrr(0x4B000000, rd, rn, rm); }
constexpr u32 subsw(int rd, int rn, int rm, u32 lsl = 0) { return rrr(0x6B000000, rd, rn, rm, lsl); }
constexpr u32 addx(int rd, int rn, int rm) { return rrr(0x8B000000, rd, rn, rm); }
// a 64-bit register plus a 32-bit one, zero-extended: a host address from a guest one
constexpr u32 addx_uxtw(int rd, int rn, int rm) { return 0x8B204000 | (u32)rm << 16 | (u32)rn << 5 | (u32)rd; }
constexpr u32 andw(int rd, int rn, int rm) { return rrr(0x0A000000, rd, rn, rm); }
constexpr u32 bicw(int rd, int rn, int rm) { return rrr(0x0A200000, rd, rn, rm); }
constexpr u32 orrw(int rd, int rn, int rm, u32 lsl = 0) { return rrr(0x2A000000, rd, rn, rm, lsl); }
constexpr u32 ornw(int rd, int rn, int rm) { return rrr(0x2A200000, rd, rn, rm); }
constexpr u32 eorw(int rd, int rn, int rm) { return rrr(0x4A000000, rd, rn, rm); }
constexpr u32 eonw(int rd, int rn, int rm) { return rrr(0x4A200000, rd, rn, rm); }
constexpr u32 andsw(int rd, int rn, int rm, u32 lsl = 0) { return rrr(0x6A000000, rd, rn, rm, lsl); }
constexpr u32 orrx(int rd, int rn, int rm) { return rrr(0xAA000000, rd, rn, rm); }
// second operand shifted right (logical)
constexpr u32 orrw_lsr(int rd, int rn, int rm, u32 lsr) { return rrr(0x2A400000, rd, rn, rm, lsr); }
constexpr u32 eorw_lsr(int rd, int rn, int rm, u32 lsr) { return rrr(0x4A400000, rd, rn, rm, lsr); }
constexpr u32 cmpx(int rn, int rm) { return rrr(0xEB000000, 31, rn, rm); }
constexpr u32 andw_lsr(int rd, int rn, int rm, u32 lsr) { return rrr(0x0A400000, rd, rn, rm, lsr); }
constexpr u32 movw(int rd, int rm) { return orrw(rd, ZR, rm); }
constexpr u32 movx(int rd, int rm) { return orrx(rd, ZR, rm); }
constexpr u32 mvnw(int rd, int rm) { return ornw(rd, ZR, rm); }
constexpr u32 negw(int rd, int rm) { return subw(rd, ZR, rm); }
constexpr u32 cmpw(int rn, int rm) { return subsw(ZR, rn, rm); }
constexpr u32 tstw(int rn, int rm) { return andsw(ZR, rn, rm); }
constexpr u32 adcsw(int rd, int rn, int rm) { return rrr(0x3A000000, rd, rn, rm); }
constexpr u32 sbcsw(int rd, int rn, int rm) { return rrr(0x7A000000, rd, rn, rm); }

// -- with a 12-bit immediate (0..4095), optionally shifted left twelve --
constexpr u32 imm12(u32 op, int rd, int rn, u32 imm, bool shifted) {
  return op | (shifted ? 1u << 22 : 0) | (imm & 0xFFF) << 10 | (u32)rn << 5 | (u32)rd;
}
constexpr u32 addw_i(int rd, int rn, u32 imm, bool sh = false) { return imm12(0x11000000, rd, rn, imm, sh); }
constexpr u32 addsw_i(int rd, int rn, u32 imm, bool sh = false) { return imm12(0x31000000, rd, rn, imm, sh); }
constexpr u32 subw_i(int rd, int rn, u32 imm, bool sh = false) { return imm12(0x51000000, rd, rn, imm, sh); }
constexpr u32 subsw_i(int rd, int rn, u32 imm, bool sh = false) { return imm12(0x71000000, rd, rn, imm, sh); }
constexpr u32 subsx_i(int rd, int rn, u32 imm) { return imm12(0xF1000000, rd, rn, imm, false); }
constexpr u32 cmpw_i(int rn, u32 imm) { return subsw_i(ZR, rn, imm); }

// -- moving constants in, sixteen bits at a time (piece 0 is the lowest) --
constexpr u32 movzw(int rd, u32 imm16, u32 piece = 0) { return 0x52800000 | piece << 21 | (imm16 & 0xFFFF) << 5 | (u32)rd; }
constexpr u32 movnw(int rd, u32 imm16, u32 piece = 0) { return 0x12800000 | piece << 21 | (imm16 & 0xFFFF) << 5 | (u32)rd; }
constexpr u32 movkw(int rd, u32 imm16, u32 piece = 0) { return 0x72800000 | piece << 21 | (imm16 & 0xFFFF) << 5 | (u32)rd; }
constexpr u32 movzx(int rd, u32 imm16, u32 piece = 0) { return 0xD2800000 | piece << 21 | (imm16 & 0xFFFF) << 5 | (u32)rd; }
constexpr u32 movkx(int rd, u32 imm16, u32 piece = 0) { return 0xF2800000 | piece << 21 | (imm16 & 0xFFFF) << 5 | (u32)rd; }

// -- bit fields and constant shifts (32-bit unless named x) --
constexpr u32 bfm(u32 op, int rd, int rn, u32 immr, u32 imms) {
  return op | (immr & 63) << 16 | (imms & 63) << 10 | (u32)rn << 5 | (u32)rd;
}
constexpr u32 lslw_i(int rd, int rn, u32 n) { return bfm(0x53000000, rd, rn, (32 - n) & 31, 31 - n); }
constexpr u32 lsrw_i(int rd, int rn, u32 n) { return bfm(0x53000000, rd, rn, n, 31); }
constexpr u32 asrw_i(int rd, int rn, u32 n) { return bfm(0x13000000, rd, rn, n, 31); }
constexpr u32 ubfxw(int rd, int rn, u32 lsb, u32 width) { return bfm(0x53000000, rd, rn, lsb, lsb + width - 1); }
constexpr u32 bfiw(int rd, int rn, u32 lsb, u32 width) { return bfm(0x33000000, rd, rn, (32 - lsb) & 31, width - 1); }
constexpr u32 sxtbw(int rd, int rn) { return bfm(0x13000000, rd, rn, 0, 7); }
constexpr u32 sxthw(int rd, int rn) { return bfm(0x13000000, rd, rn, 0, 15); }
constexpr u32 lslx_i(int rd, int rn, u32 n) { return bfm(0xD3400000, rd, rn, (64 - n) & 63, 63 - n); }
constexpr u32 lsrx_i(int rd, int rn, u32 n) { return bfm(0xD3400000, rd, rn, n, 63); }
constexpr u32 ubfxx(int rd, int rn, u32 lsb, u32 width) { return bfm(0xD3400000, rd, rn, lsb, lsb + width - 1); }
constexpr u32 sxtw(int rd, int rn) { return bfm(0x93400000, rd, rn, 0, 31); }  // 32 bits to 64, signed
constexpr u32 rorw_i(int rd, int rn, u32 n) { return 0x13800000 | (u32)rn << 16 | (n & 31) << 10 | (u32)rn << 5 | (u32)rd; }

// -- and/or/eor with a repeating-pattern immediate; valid() says whether a
//    value can be written that way (most masks of adjacent ones can) --
struct Mask {
  bool ok = false;
  u32 immr = 0, imms = 0;
};
constexpr Mask mask32(u32 v) {
  if (v == 0 || v == 0xFFFFFFFFu) return {};
  for (u32 size = 2; size <= 32; size *= 2) {
    const u32 field = size == 32 ? 0xFFFFFFFFu : (1u << size) - 1;
    const u32 pattern = v & field;
    bool repeats = true;
    for (u32 at = size; at < 32; at += size) repeats &= ((v >> at) & field) == pattern;
    if (!repeats) continue;
    u32 ones = 0;
    for (u32 b = 0; b < size; b++) ones += (pattern >> b) & 1;
    if (ones == 0 || ones == size) continue;
    const u32 low = (1u << ones) - 1;
    for (u32 r = 0; r < size; r++) {
      const u32 rotated = r ? ((low >> r) | (low << (size - r))) & field : low;
      if (rotated == pattern) return {true, r, ((~(size * 2 - 1)) & 0x3F) | (ones - 1)};
    }
  }
  return {};
}
constexpr u32 logic_i(u32 op, int rd, int rn, Mask m) { return op | m.immr << 16 | m.imms << 10 | (u32)rn << 5 | (u32)rd; }
constexpr u32 andw_i(int rd, int rn, Mask m) { return logic_i(0x12000000, rd, rn, m); }
constexpr u32 orrw_i(int rd, int rn, Mask m) { return logic_i(0x32000000, rd, rn, m); }
constexpr u32 eorw_i(int rd, int rn, Mask m) { return logic_i(0x52000000, rd, rn, m); }
constexpr u32 tstw_i(int rn, Mask m) { return logic_i(0x72000000, ZR, rn, m); }

// -- shifts by a register, multiply, divide, counting, byte order --
constexpr u32 lslvw(int rd, int rn, int rm) { return rrr(0x1AC02000, rd, rn, rm); }
constexpr u32 lsrvw(int rd, int rn, int rm) { return rrr(0x1AC02400, rd, rn, rm); }
constexpr u32 asrvw(int rd, int rn, int rm) { return rrr(0x1AC02800, rd, rn, rm); }
constexpr u32 rorvw(int rd, int rn, int rm) { return rrr(0x1AC02C00, rd, rn, rm); }
constexpr u32 lslvx(int rd, int rn, int rm) { return rrr(0x9AC02000, rd, rn, rm); }
constexpr u32 asrvx(int rd, int rn, int rm) { return rrr(0x9AC02800, rd, rn, rm); }
constexpr u32 mulw(int rd, int rn, int rm) { return rrr(0x1B007C00, rd, rn, rm); }
constexpr u32 smull(int rd, int rn, int rm) { return rrr(0x9B207C00, rd, rn, rm); }  // x = w * w, signed
constexpr u32 umull(int rd, int rn, int rm) { return rrr(0x9BA07C00, rd, rn, rm); }
constexpr u32 udivw(int rd, int rn, int rm) { return rrr(0x1AC00800, rd, rn, rm); }
constexpr u32 sdivw(int rd, int rn, int rm) { return rrr(0x1AC00C00, rd, rn, rm); }
constexpr u32 clzw(int rd, int rn) { return 0x5AC01000 | (u32)rn << 5 | (u32)rd; }
constexpr u32 revw(int rd, int rn) { return 0x5AC00800 | (u32)rn << 5 | (u32)rd; }
constexpr u32 rev16w(int rd, int rn) { return 0x5AC00400 | (u32)rn << 5 | (u32)rd; }
constexpr u32 revx(int rd, int rn) { return 0xDAC00C00 | (u32)rn << 5 | (u32)rd; }

// -- choosing by condition --
constexpr u32 cselw(int rd, int rn, int rm, Cond c) { return 0x1A800000 | (u32)rm << 16 | c << 12 | (u32)rn << 5 | (u32)rd; }
constexpr u32 csetw(int rd, Cond c) { return 0x1A9F07E0 | invert(c) << 12 | (u32)rd; }

// -- memory. `at` forms: an offset from a register, which must be a multiple
//    of the size and fit twelve bits once divided by it. `idx` forms: a
//    64-bit base plus a 32-bit index (zero-extended), which is how guest
//    addresses are reached. --
constexpr u32 mem_at(u32 op, int rt, int rn, u32 offset, u32 size) { return op | (offset / size) << 10 | (u32)rn << 5 | (u32)rt; }
constexpr u32 ldrw_at(int rt, int rn, u32 off) { return mem_at(0xB9400000, rt, rn, off, 4); }
constexpr u32 strw_at(int rt, int rn, u32 off) { return mem_at(0xB9000000, rt, rn, off, 4); }
constexpr u32 ldrx_at(int rt, int rn, u32 off) { return mem_at(0xF9400000, rt, rn, off, 8); }
constexpr u32 strx_at(int rt, int rn, u32 off) { return mem_at(0xF9000000, rt, rn, off, 8); }
constexpr u32 ldrb_at(int rt, int rn, u32 off) { return mem_at(0x39400000, rt, rn, off, 1); }
constexpr u32 strb_at(int rt, int rn, u32 off) { return mem_at(0x39000000, rt, rn, off, 1); }
constexpr u32 ldrd_at(int rt, int rn, u32 off) { return mem_at(0xFD400000, rt, rn, off, 8); }
constexpr u32 strd_at(int rt, int rn, u32 off) { return mem_at(0xFD000000, rt, rn, off, 8); }
constexpr u32 mem_idx(u32 op, int rt, int rn, int rm) { return op | (u32)rm << 16 | (u32)rn << 5 | (u32)rt; }
constexpr u32 ldrw_idx(int rt, int rn, int rm) { return mem_idx(0xB8604800, rt, rn, rm); }
constexpr u32 strw_idx(int rt, int rn, int rm) { return mem_idx(0xB8204800, rt, rn, rm); }
constexpr u32 ldrx_idx(int rt, int rn, int rm) { return mem_idx(0xF8604800, rt, rn, rm); }
constexpr u32 strx_idx(int rt, int rn, int rm) { return mem_idx(0xF8204800, rt, rn, rm); }
constexpr u32 ldrb_idx(int rt, int rn, int rm) { return mem_idx(0x38604800, rt, rn, rm); }
constexpr u32 strb_idx(int rt, int rn, int rm) { return mem_idx(0x38204800, rt, rn, rm); }
constexpr u32 ldrh_idx(int rt, int rn, int rm) { return mem_idx(0x78604800, rt, rn, rm); }
constexpr u32 strh_idx(int rt, int rn, int rm) { return mem_idx(0x78204800, rt, rn, rm); }
// a 64-bit base plus a 64-bit index times eight
constexpr u32 ldrx_scaled(int rt, int rn, int rm) { return 0xF8607800 | (u32)rm << 16 | (u32)rn << 5 | (u32)rt; }
// pairs, for the entry code: push with pre-decrement, pop with post-increment, and plain offsets
constexpr u32 stp_pre(int r1, int r2, int rn, int off) { return 0xA9800000 | ((u32)(off / 8) & 0x7F) << 15 | (u32)r2 << 10 | (u32)rn << 5 | (u32)r1; }
constexpr u32 ldp_post(int r1, int r2, int rn, int off) { return 0xA8C00000 | ((u32)(off / 8) & 0x7F) << 15 | (u32)r2 << 10 | (u32)rn << 5 | (u32)r1; }
constexpr u32 stp_at(int r1, int r2, int rn, int off) { return 0xA9000000 | ((u32)(off / 8) & 0x7F) << 15 | (u32)r2 << 10 | (u32)rn << 5 | (u32)r1; }
constexpr u32 ldp_at(int r1, int r2, int rn, int off) { return 0xA9400000 | ((u32)(off / 8) & 0x7F) << 15 | (u32)r2 << 10 | (u32)rn << 5 | (u32)r1; }

// -- floating point, double precision unless named s --
constexpr u32 fmov_dx(int fd, int xn) { return 0x9E670000 | (u32)xn << 5 | (u32)fd; }
constexpr u32 fmov_xd(int xd, int fn) { return 0x9E660000 | (u32)fn << 5 | (u32)xd; }
constexpr u32 fmov_sw(int fd, int wn) { return 0x1E270000 | (u32)wn << 5 | (u32)fd; }
constexpr u32 fmov_dd(int fd, int fn) { return 0x1E604000 | (u32)fn << 5 | (u32)fd; }
constexpr u32 faddd(int fd, int fn, int fm) { return rrr(0x1E602800, fd, fn, fm); }
constexpr u32 fsubd(int fd, int fn, int fm) { return rrr(0x1E603800, fd, fn, fm); }
constexpr u32 fmuld(int fd, int fn, int fm) { return rrr(0x1E600800, fd, fn, fm); }
constexpr u32 fdivd(int fd, int fn, int fm) { return rrr(0x1E601800, fd, fn, fm); }
// fa + fn * fm, and its relatives
constexpr u32 fmaddd(int fd, int fn, int fm, int fa) { return 0x1F400000 | (u32)fm << 16 | (u32)fa << 10 | (u32)fn << 5 | (u32)fd; }
constexpr u32 fnmsubd(int fd, int fn, int fm, int fa) { return 0x1F608000 | (u32)fm << 16 | (u32)fa << 10 | (u32)fn << 5 | (u32)fd; }  // fn * fm - fa
constexpr u32 fmadds(int fd, int fn, int fm, int fa) { return 0x1F000000 | (u32)fm << 16 | (u32)fa << 10 | (u32)fn << 5 | (u32)fd; }
constexpr u32 fnmsubs(int fd, int fn, int fm, int fa) { return 0x1F208000 | (u32)fm << 16 | (u32)fa << 10 | (u32)fn << 5 | (u32)fd; }  // fn * fm - fa
constexpr u32 fnegd(int fd, int fn) { return 0x1E614000 | (u32)fn << 5 | (u32)fd; }
constexpr u32 fabsd(int fd, int fn) { return 0x1E60C000 | (u32)fn << 5 | (u32)fd; }
constexpr u32 fsqrtd(int fd, int fn) { return 0x1E61C000 | (u32)fn << 5 | (u32)fd; }
constexpr u32 fcvt_sd(int fd, int fn) { return 0x1E624000 | (u32)fn << 5 | (u32)fd; }  // double to single
constexpr u32 fcvt_ds(int fd, int fn) { return 0x1E22C000 | (u32)fn << 5 | (u32)fd; }  // single to double
constexpr u32 fcmpd(int fn, int fm) { return 0x1E602000 | (u32)fm << 16 | (u32)fn << 5; }
constexpr u32 fcmpd_zero(int fn) { return 0x1E602008 | (u32)fn << 5; }
constexpr u32 fcselD(int fd, int fn, int fm, Cond c) { return 0x1E600C00 | (u32)fm << 16 | c << 12 | (u32)fn << 5 | (u32)fd; }
constexpr u32 fcvtzs_wd(int wd, int fn) { return 0x1E780000 | (u32)fn << 5 | (u32)wd; }  // to a 32-bit integer, towards zero

// single precision, 128-bit moves and logic, and integers to floating point:
// what the Intel translator needs for SSE
constexpr u32 ldrs_at(int rt, int rn, u32 off) { return mem_at(0xBD400000, rt, rn, off, 4); }
constexpr u32 strs_at(int rt, int rn, u32 off) { return mem_at(0xBD000000, rt, rn, off, 4); }
constexpr u32 ldrq_at(int rt, int rn, u32 off) { return mem_at(0x3DC00000, rt, rn, off, 16); }
constexpr u32 strq_at(int rt, int rn, u32 off) { return mem_at(0x3D800000, rt, rn, off, 16); }
constexpr u32 ldrd_idx(int rt, int rn, int rm) { return mem_idx(0xFC604800, rt, rn, rm); }
constexpr u32 strd_idx(int rt, int rn, int rm) { return mem_idx(0xFC204800, rt, rn, rm); }
constexpr u32 ldrs_idx(int rt, int rn, int rm) { return mem_idx(0xBC604800, rt, rn, rm); }
constexpr u32 strs_idx(int rt, int rn, int rm) { return mem_idx(0xBC204800, rt, rn, rm); }
constexpr u32 ldrq_idx(int rt, int rn, int rm) { return mem_idx(0x3CE04800, rt, rn, rm); }
constexpr u32 strq_idx(int rt, int rn, int rm) { return mem_idx(0x3CA04800, rt, rn, rm); }
constexpr u32 addx_i(int rd, int rn, u32 imm) { return imm12(0x91000000, rd, rn, imm, false); }
constexpr u32 fadds(int fd, int fn, int fm) { return rrr(0x1E202800, fd, fn, fm); }
constexpr u32 fsubs(int fd, int fn, int fm) { return rrr(0x1E203800, fd, fn, fm); }
constexpr u32 fmuls(int fd, int fn, int fm) { return rrr(0x1E200800, fd, fn, fm); }
constexpr u32 fdivs(int fd, int fn, int fm) { return rrr(0x1E201800, fd, fn, fm); }
constexpr u32 fsqrts(int fd, int fn) { return 0x1E21C000 | (u32)fn << 5 | (u32)fd; }
constexpr u32 fcmps(int fn, int fm) { return 0x1E202000 | (u32)fm << 16 | (u32)fn << 5; }
constexpr u32 scvtf_dw(int fd, int wn) { return 0x1E620000 | (u32)wn << 5 | (u32)fd; }
constexpr u32 scvtf_sw(int fd, int wn) { return 0x1E220000 | (u32)wn << 5 | (u32)fd; }
constexpr u32 eorv(int vd, int vn, int vm) { return rrr(0x6E201C00, vd, vn, vm); }
constexpr u32 andv(int vd, int vn, int vm) { return rrr(0x4E201C00, vd, vn, vm); }
constexpr u32 orrv(int vd, int vn, int vm) { return rrr(0x4EA01C00, vd, vn, vm); }

// -- more of the same, for the Intel translator's SSE --
constexpr u32 fmov_ws(int wd, int fn) { return 0x1E260000 | (u32)fn << 5 | (u32)wd; }
constexpr u32 fcvtzs_xd(int xd, int fn) { return 0x9E780000 | (u32)fn << 5 | (u32)xd; }  // to a 64-bit integer, towards zero
constexpr u32 fcvtzs_xs(int xd, int fn) { return 0x9E380000 | (u32)fn << 5 | (u32)xd; }
constexpr u32 frintid(int fd, int fn) { return 0x1E67C000 | (u32)fn << 5 | (u32)fd; }  // to a whole number, the current rounding
constexpr u32 frintis(int fd, int fn) { return 0x1E27C000 | (u32)fn << 5 | (u32)fd; }
constexpr u32 cmpx_sxtw(int xn, int wm) { return 0xEB20C01F | (u32)wm << 16 | (u32)xn << 5; }
constexpr u32 fcselS(int fd, int fn, int fm, Cond c) { return 0x1E200C00 | (u32)fm << 16 | c << 12 | (u32)fn << 5 | (u32)fd; }
constexpr u32 bicv(int vd, int vn, int vm) { return rrr(0x4E601C00, vd, vn, vm); }  // vn where vm has no bit
constexpr u32 bslv(int vd, int vn, int vm) { return rrr(0x6E601C00, vd, vn, vm); }  // vn where vd has a bit, else vm
// Four singles at once, or two doubles when `d`.
constexpr u32 faddv(bool d, int vd, int vn, int vm) { return rrr(0x4E20D400 | (u32)d << 22, vd, vn, vm); }
constexpr u32 fsubv(bool d, int vd, int vn, int vm) { return rrr(0x4EA0D400 | (u32)d << 22, vd, vn, vm); }
constexpr u32 fmulv(bool d, int vd, int vn, int vm) { return rrr(0x6E20DC00 | (u32)d << 22, vd, vn, vm); }
constexpr u32 fdivv(bool d, int vd, int vn, int vm) { return rrr(0x6E20FC00 | (u32)d << 22, vd, vn, vm); }
constexpr u32 fsqrtv(bool d, int vd, int vn) { return 0x6EA1F800 | (u32)d << 22 | (u32)vn << 5 | (u32)vd; }
constexpr u32 fcmeqv(bool d, int vd, int vn, int vm) { return rrr(0x4E20E400 | (u32)d << 22, vd, vn, vm); }
constexpr u32 fcmgtv(bool d, int vd, int vn, int vm) { return rrr(0x6EA0E400 | (u32)d << 22, vd, vn, vm); }
// Integers at once: `size` 0 for sixteen bytes, 1 eight halves, 2 four words, 3 two doublewords.
constexpr u32 addv(u32 size, int vd, int vn, int vm) { return rrr(0x4E208400 | size << 22, vd, vn, vm); }
constexpr u32 subv(u32 size, int vd, int vn, int vm) { return rrr(0x6E208400 | size << 22, vd, vn, vm); }
constexpr u32 cmeqv(u32 size, int vd, int vn, int vm) { return rrr(0x6E208C00 | size << 22, vd, vn, vm); }
constexpr u32 cmgtv(u32 size, int vd, int vn, int vm) { return rrr(0x4E203400 | size << 22, vd, vn, vm); }
constexpr u32 umov_xd1(int xd, int vn) { return 0x4E183C00 | (u32)vn << 5 | (u32)xd; }  // the high half
constexpr u32 andx(int rd, int rn, int rm) { return rrr(0x8A000000, rd, rn, rm); }
constexpr u32 andx_tops(int rd, int rn) { return 0x9201C000 | (u32)rn << 5 | (u32)rd; }  // and 0x8080808080808080
constexpr u32 cmnx_i(int rn, u32 imm) { return 0xB100001F | imm << 10 | (u32)rn << 5; }
constexpr u32 mulx(int rd, int rn, int rm) { return 0x9B007C00 | (u32)rm << 16 | (u32)rn << 5 | (u32)rd; }
constexpr u32 rbitw(int rd, int rn) { return 0x5AC00000 | (u32)rn << 5 | (u32)rd; }
constexpr u32 scvtfv(bool d, int vd, int vn) { return 0x4E21D800 | (u32)d << 22 | (u32)vn << 5 | (u32)vd; }   // integers to floating point
constexpr u32 fcvtzsv(bool d, int vd, int vn) { return 0x4EA1B800 | (u32)d << 22 | (u32)vn << 5 | (u32)vd; }  // and back, towards zero
constexpr u32 frintiv(bool d, int vd, int vn) { return 0x6EA19800 | (u32)d << 22 | (u32)vn << 5 | (u32)vd; }
constexpr u32 mvni_max32(int vd) { return 0x6F046400 | (u32)vd; }  // 0x7FFFFFFF in each of four words
constexpr u32 sxtl_2d(int vd, int vn) { return 0x0F20A400 | (u32)vn << 5 | (u32)vd; }   // two words to two doublewords
constexpr u32 fcvtl_2d(int vd, int vn) { return 0x0E617800 | (u32)vn << 5 | (u32)vd; }  // two singles to two doubles
constexpr u32 fcvtn_2s(int vd, int vn) { return 0x0E616800 | (u32)vn << 5 | (u32)vd; }  // and back; the high half cleared
constexpr u32 fcmgev(bool d, int vd, int vn, int vm) { return rrr(0x6E20E400 | (u32)d << 22, vd, vn, vm); }
constexpr u32 mvnv(int vd, int vn) { return 0x6E205800 | (u32)vn << 5 | (u32)vd; }
constexpr u32 zip1v(u32 size, int vd, int vn, int vm) { return rrr(0x4E003800 | size << 22, vd, vn, vm); }  // low halves interleaved
constexpr u32 zip2v(u32 size, int vd, int vn, int vm) { return rrr(0x4E007800 | size << 22, vd, vn, vm); }  // high halves
constexpr u32 mulv(u32 size, int vd, int vn, int vm) { return rrr(0x4E209C00 | size << 22, vd, vn, vm); }
// Shifts of each element by a constant: right by 1 to its width, left by 0 to one less.
constexpr u32 ushrv(u32 size, int vd, int vn, u32 n) { return 0x6F000400 | ((16u << size) - n) << 16 | (u32)vn << 5 | (u32)vd; }
constexpr u32 sshrv(u32 size, int vd, int vn, u32 n) { return 0x4F000400 | ((16u << size) - n) << 16 | (u32)vn << 5 | (u32)vd; }

// -- for the PowerPC translator's AltiVec (four singles, or integers of `size`) --
constexpr u32 fmlav(int vd, int vn, int vm) { return rrr(0x4E20CC00, vd, vn, vm); }  // vd += vn * vm, rounded once
constexpr u32 fnegv(int vd, int vn) { return 0x6EA0F800 | (u32)vn << 5 | (u32)vd; }
constexpr u32 fmaxv(int vd, int vn, int vm) { return rrr(0x4E20F400, vd, vn, vm); }
constexpr u32 fminv(int vd, int vn, int vm) { return rrr(0x4EA0F400, vd, vn, vm); }
constexpr u32 frintnv(int vd, int vn) { return 0x4E218800 | (u32)vn << 5 | (u32)vd; }  // to nearest, ties to even
constexpr u32 frintmv(int vd, int vn) { return 0x4E219800 | (u32)vn << 5 | (u32)vd; }  // down
constexpr u32 frintpv(int vd, int vn) { return 0x4EA18800 | (u32)vn << 5 | (u32)vd; }  // up
constexpr u32 frintzv(int vd, int vn) { return 0x4EA19800 | (u32)vn << 5 | (u32)vd; }  // towards zero
constexpr u32 fmov_one4s(int vd) { return 0x4F03F600 | (u32)vd; }                     // 1.0 in each of four
// Words to singles, divided by 2^fbits (0 to 31).
constexpr u32 ucvtfv_fixed(int vd, int vn, u32 fbits) {
  return fbits ? 0x6F20E400 | (64 - fbits) << 16 | (u32)vn << 5 | (u32)vd : 0x6E21D800 | (u32)vn << 5 | (u32)vd;
}
constexpr u32 scvtfv_fixed(int vd, int vn, u32 fbits) {
  return fbits ? 0x4F20E400 | (64 - fbits) << 16 | (u32)vn << 5 | (u32)vd : 0x4E21D800 | (u32)vn << 5 | (u32)vd;
}
// Singles to words, multiplied by 2^fbits (0 to 31), towards zero, saturating.
constexpr u32 fcvtzuv_fixed(int vd, int vn, u32 fbits) {
  return fbits ? 0x6F20FC00 | (64 - fbits) << 16 | (u32)vn << 5 | (u32)vd : 0x6EA1B800 | (u32)vn << 5 | (u32)vd;
}
constexpr u32 fcvtzsv_fixed(int vd, int vn, u32 fbits) {
  return fbits ? 0x4F20FC00 | (64 - fbits) << 16 | (u32)vn << 5 | (u32)vd : 0x4EA1B800 | (u32)vn << 5 | (u32)vd;
}
constexpr u32 tbl2(int vd, int vn, int vm) { return rrr(0x4E002000, vd, vn, vm); }    // bytes of vn, vn+1 picked by vm
constexpr u32 rev64v(int vd, int vn) { return 0x4E200800 | (u32)vn << 5 | (u32)vd; }    // bytes reversed in each half
constexpr u32 extv(int vd, int vn, int vm, u32 at) { return rrr(0x6E000000, vd, vn, vm) | at << 11; }  // vn:vm from byte `at`
// One element of vn in every element: `size` as above, `index` counted in host order.
constexpr u32 dup_elem(u32 size, int vd, int vn, u32 index) {
  return 0x4E000400 | (((index << 1 | 1) << size) & 0x1F) << 16 | (u32)vn << 5 | (u32)vd;
}
constexpr u32 dup_w(u32 size, int vd, int wn) { return 0x4E000C00 | (1u << size) << 16 | (u32)wn << 5 | (u32)vd; }  // a register's low element
constexpr u32 ushlv(u32 size, int vd, int vn, int vm) { return rrr(0x6E204400 | size << 22, vd, vn, vm); }  // by each of vm, right if negative
constexpr u32 negv(u32 size, int vd, int vn) { return 0x6E20B800 | size << 22 | (u32)vn << 5 | (u32)vd; }
// Each element halved in width, into the low half (the high half cleared), or with `hi` into the high half (the low kept).
constexpr u32 xtnv(u32 size, bool hi, int vd, int vn) { return 0x0E212800 | (u32)hi << 30 | size << 22 | (u32)vn << 5 | (u32)vd; }
constexpr u32 shlv(u32 size, int vd, int vn, u32 n) { return 0x4F005400 | ((8u << size) + n) << 16 | (u32)vn << 5 | (u32)vd; }

// -- branches. Offsets are in bytes from the instruction itself. --
constexpr u32 b(s64 off) { return 0x14000000 | ((u32)(off / 4) & 0x03FFFFFF); }
constexpr u32 bl(s64 off) { return 0x94000000 | ((u32)(off / 4) & 0x03FFFFFF); }
constexpr u32 bcond(Cond c, s64 off) { return 0x54000000 | ((u32)(off / 4) & 0x7FFFF) << 5 | c; }
constexpr u32 cbzw(int rt, s64 off) { return 0x34000000 | ((u32)(off / 4) & 0x7FFFF) << 5 | (u32)rt; }
constexpr u32 cbnzw(int rt, s64 off) { return 0x35000000 | ((u32)(off / 4) & 0x7FFFF) << 5 | (u32)rt; }
constexpr u32 cbnzx(int rt, s64 off) { return 0xB5000000 | ((u32)(off / 4) & 0x7FFFF) << 5 | (u32)rt; }
constexpr u32 tbzw(int rt, u32 bit, s64 off) { return 0x36000000 | bit << 19 | ((u32)(off / 4) & 0x3FFF) << 5 | (u32)rt; }
constexpr u32 tbnzw(int rt, u32 bit, s64 off) { return 0x37000000 | bit << 19 | ((u32)(off / 4) & 0x3FFF) << 5 | (u32)rt; }
constexpr u32 br(int rn) { return 0xD61F0000 | (u32)rn << 5; }
constexpr u32 blr(int rn) { return 0xD63F0000 | (u32)rn << 5; }
constexpr u32 ret() { return 0xD65F03C0; }
constexpr u32 nop() { return 0xD503201F; }

}  // namespace a64
