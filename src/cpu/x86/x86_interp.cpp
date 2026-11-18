// 32-bit Intel user-mode interpreter: decoding and the integer instruction
// set. The x87 lives in x87.cpp and MMX/SSE in sse.cpp.
//
// This is the reference core: simple and exact rather than fast. Flags are
// computed eagerly after every instruction that sets them.

#include "cpu/rounding.h"
#include <atomic>
#include <cfenv>
#include <ctime>

#include "cpu/x86/x86.h"

namespace {

enum : u32 {
  CF = 0x001,
  PF = 0x004,
  AF = 0x010,
  ZF = 0x040,
  SF = 0x080,
  TF = 0x100,
  DF = 0x400,
  OF = 0x800,
  AC = 0x40000,
  ID = 0x200000,
};
constexpr u32 kArithFlags = CF | PF | AF | ZF | SF | OF;

enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };

// One lock for every LOCK-prefixed instruction in the process. It makes them
// atomic with respect to each other, which is what guest code relies on when
// several threads update the same word. (A JIT can use real atomic
// instructions instead.)
std::atomic_flag g_bus_lock = ATOMIC_FLAG_INIT;

}  // namespace

void x86_bus_lock(bool take) {
  if (take) {
    while (g_bus_lock.test_and_set(std::memory_order_acquire)) {
    }
  } else {
    g_bus_lock.clear(std::memory_order_release);
  }
}

namespace {

struct BusLock {
  bool held = false;
  void take() {
    while (g_bus_lock.test_and_set(std::memory_order_acquire)) {
    }
    held = true;
  }
  ~BusLock() {
    if (held) g_bus_lock.clear(std::memory_order_release);
  }
};

inline u32 mask_of(int bits) { return bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1); }


inline s32 sext(u32 v, int bits) {
  const int sh = 32 - bits;
  return (s32)(v << sh) >> sh;
}

// ---- guest memory (little-endian, same as the host) -------------------------

inline u32 ld(const u8* m, u32 a, int sz) {
  if (sz == 4) {
    u32 v;
    memcpy(&v, m + a, 4);
    return v;
  }
  if (sz == 2) {
    u16 v;
    memcpy(&v, m + a, 2);
    return v;
  }
  return m[a];
}

inline void st(u8* m, u32 a, int sz, u32 v) {
  if (sz == 4) {
    memcpy(m + a, &v, 4);
  } else if (sz == 2) {
    const u16 t = (u16)v;
    memcpy(m + a, &t, 2);
  } else {
    m[a] = (u8)v;
  }
}

// A LOCK-prefixed instruction (or XCHG) has to be atomic against every other
// access to its operand, including plain stores from other threads, not only
// against other locked instructions: a spinlock released by a plain store in
// the middle of a locked read-modify-write must not be lost. So the locked
// instruction reads its memory operand once, keeps its write back, and puts
// it in with a real compare-and-swap at the end; if the memory changed in the
// meantime, the instruction runs again.
struct Atomic {
  bool on = false, have = false, wrote = false;
  u32 ea = 0, old = 0, now = 0;
  int sz = 0;
};
inline bool aligned(u32 ea, int sz) { return (ea & (u32)(sz - 1)) == 0; }
inline u32 atomic_ld(const u8* m, u32 ea, int sz) {
  if (!aligned(ea, sz)) return ld(m, ea, sz);
  if (sz == 4) return __atomic_load_n((const u32*)(m + ea), __ATOMIC_SEQ_CST);
  if (sz == 2) return __atomic_load_n((const u16*)(m + ea), __ATOMIC_SEQ_CST);
  return __atomic_load_n(m + ea, __ATOMIC_SEQ_CST);
}
// Writes `now` if the memory still holds `old`.
inline bool atomic_cas(u8* m, u32 ea, int sz, u32 old, u32 now) {
  if (!aligned(ea, sz)) {  // can't be done atomically on this Mac; rare enough
    if (ld(m, ea, sz) != old) return false;
    st(m, ea, sz, now);
    return true;
  }
  if (sz == 4) {
    u32 e = old;
    return __atomic_compare_exchange_n((u32*)(m + ea), &e, now, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  }
  if (sz == 2) {
    u16 e = (u16)old;
    return __atomic_compare_exchange_n((u16*)(m + ea), &e, (u16)now, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  }
  u8 e = (u8)old;
  return __atomic_compare_exchange_n(m + ea, &e, (u8)now, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

// ---- flags ------------------------------------------------------------------

// Sign, zero and parity flags of a result already masked to `bits`.
inline u32 szp(u32 r, int bits) {
  u32 f = 0;
  if (!r) f |= ZF;
  if ((r >> (bits - 1)) & 1) f |= SF;
  if (!(__builtin_popcount(r & 0xFF) & 1)) f |= PF;
  return f;
}

// Evaluates condition code `cc` (the low four bits of Jcc/SETcc/CMOVcc).
inline bool cond(u32 f, int cc) {
  bool r;
  switch (cc >> 1) {
    case 0: r = (f & OF) != 0; break;
    case 1: r = (f & CF) != 0; break;
    case 2: r = (f & ZF) != 0; break;
    case 3: r = (f & (CF | ZF)) != 0; break;
    case 4: r = (f & SF) != 0; break;
    case 5: r = (f & PF) != 0; break;
    case 6: r = ((f & SF) != 0) != ((f & OF) != 0); break;
    default: r = (f & ZF) != 0 || ((f & SF) != 0) != ((f & OF) != 0); break;
  }
  return r != ((cc & 1) != 0);
}

// The eight classic ALU operations, numbered as the opcode map encodes them:
// ADD OR ADC SBB AND SUB XOR CMP. Operands must already be masked to `bits`.
u32 alu(X86State& s, int op, u32 a, u32 b, int bits) {
  const u32 mask = mask_of(bits), sign = 1u << (bits - 1);
  u32 r, f = 0;
  switch (op) {
    case 0:
    case 2: {
      const u32 cin = (op == 2) ? (s.eflags & CF) : 0;
      const u64 wide = (u64)a + b + cin;
      r = (u32)wide & mask;
      if ((wide >> bits) & 1) f |= CF;
      if ((a ^ b ^ r) & 0x10) f |= AF;
      if ((a ^ r) & (b ^ r) & sign) f |= OF;
      break;
    }
    case 3:
    case 5:
    case 7: {
      const u32 bin = (op == 3) ? (s.eflags & CF) : 0;
      const u64 wide = (u64)a - b - bin;
      r = (u32)wide & mask;
      if ((wide >> bits) & 1) f |= CF;
      if ((a ^ b ^ r) & 0x10) f |= AF;
      if ((a ^ b) & (a ^ r) & sign) f |= OF;
      break;
    }
    case 1: r = a | b; break;
    case 4: r = a & b; break;
    default: r = a ^ b; break;
  }
  s.eflags = (s.eflags & ~kArithFlags) | f | szp(r, bits);
  return r;
}

// INC and DEC: like add/subtract by one, except CF is left alone.
u32 incdec(X86State& s, u32 a, bool dec, int bits) {
  const u32 mask = mask_of(bits), sign = 1u << (bits - 1);
  const u32 r = (dec ? a - 1 : a + 1) & mask;
  u32 f = szp(r, bits);
  if ((a ^ r) & 0x10) f |= AF;
  if (dec ? (a == sign) : (r == sign)) f |= OF;
  s.eflags = (s.eflags & ~(kArithFlags & ~CF)) | f;
  return r;
}

// Shift/rotate group, numbered as encoded: ROL ROR RCL RCR SHL SHR SAL SAR.
u32 shift(X86State& s, int sub, u32 a, u32 count, int bits) {
  count &= 31;
  if (!count) return a;
  const u32 mask = mask_of(bits), top = (u32)bits - 1;
  u32 cf = s.eflags & CF;
  u32 r = a, of = 0;
  switch (sub) {
    case 0: {  // rol
      const u32 c = count % (u32)bits;
      if (c) r = ((a << c) | (a >> (bits - c))) & mask;
      cf = r & 1;
      of = ((r >> top) ^ cf) & 1;
      break;
    }
    case 1: {  // ror
      const u32 c = count % (u32)bits;
      if (c) r = ((a >> c) | (a << (bits - c))) & mask;
      cf = (r >> top) & 1;
      of = ((r >> top) ^ (r >> (top - 1))) & 1;
      break;
    }
    case 2: {  // rcl
      const u32 c = count % ((u32)bits + 1);
      for (u32 i = 0; i < c; i++) {
        const u32 out = (r >> top) & 1;
        r = ((r << 1) | cf) & mask;
        cf = out;
      }
      of = ((r >> top) ^ cf) & 1;
      break;
    }
    case 3: {  // rcr
      const u32 c = count % ((u32)bits + 1);
      of = ((a >> top) ^ cf) & 1;
      for (u32 i = 0; i < c; i++) {
        const u32 out = r & 1;
        r = (r >> 1) | (cf << top);
        cf = out;
      }
      break;
    }
    case 4:
    case 6: {  // shl / sal
      const u64 wide = (u64)a << count;
      r = (u32)wide & mask;
      cf = (u32)(wide >> bits) & 1;
      of = ((r >> top) ^ cf) & 1;
      break;
    }
    case 5:  // shr
      cf = (a >> (count - 1)) & 1;
      r = a >> count;
      of = (a >> top) & 1;
      break;
    default: {  // sar
      const s32 sa = sext(a, bits);
      cf = (u32)(sa >> (count - 1)) & 1;
      r = (u32)(sa >> count) & mask;
      of = 0;
      break;
    }
  }
  u32 f = s.eflags & ~(CF | OF);
  if (cf) f |= CF;
  if (of) f |= OF;
  if (sub >= 4) f = (f & ~(PF | AF | ZF | SF)) | szp(r, bits);  // rotates leave these alone
  s.eflags = f;
  return r;
}

// Double-precision shifts: SHLD shifts `a` left, filling from the top of `b`;
// SHRD shifts right, filling from the bottom of `b`.
u32 shift_double(X86State& s, bool right, u32 a, u32 b, u32 count, int bits) {
  count &= 31;
  if (!count || count > (u32)bits) return a;
  const u32 mask = mask_of(bits), top = (u32)bits - 1;
  u32 r, cf;
  if (right) {
    r = (u32)((((u64)b << bits) | a) >> count) & mask;
    cf = (a >> (count - 1)) & 1;
  } else {
    r = (u32)(((((u64)a << bits) | b) << count) >> bits) & mask;
    cf = (a >> (bits - count)) & 1;
  }
  u32 f = (s.eflags & ~kArithFlags) | szp(r, bits);
  if (cf) f |= CF;
  if (((a ^ r) >> top) & 1) f |= OF;
  s.eflags = f;
  return r;
}

}  // namespace

u32 x86_alu(X86State* s, u32 op, u32 a, u32 b, u32 bits) { return alu(*s, (int)op, a, b, (int)bits); }
u32 x86_incdec(X86State* s, u32 a, u32 dec, u32 bits) { return incdec(*s, a, dec != 0, (int)bits); }
u32 x86_shift(X86State* s, u32 kind, u32 a, u32 count, u32 bits) { return shift(*s, (int)kind, a, count, (int)bits); }

X86Stop x86_interp(X86State& s, u64 max_insns) {
  u8* const m = s.mem;
  u32* const R = s.reg;
  u64 left = max_insns;

  // SSE arithmetic runs on host floating point, in the guest's rounding mode.
  sse_apply_rounding(s.mxcsr);

  auto stop = [&](X86StopKind kind, u32 eip, u8 vector = 0, const char* what = nullptr) {
    s.eip = eip;
    s.icount += max_insns - left;
    if (!t_keep_guest_rounding) host_rounding(FE_TONEAREST);  // host code expects the default mode
    return X86Stop{kind, eip, vector, what};
  };

  for (;;) {
    if (left == 0) return stop(X86StopKind::Budget, s.eip);
    const u32 start = s.eip;  // s.eip stays here until the instruction completes
    u32 ip = start;

    // ---- prefixes ----
    bool op16 = false, locked = false;
    int rep = 0;
    u32 seg_base = 0;
    u8 op;
    for (;;) {
      op = m[ip++];
      if (op == 0x66) {
        op16 = true;
      } else if (op == 0xF2 || op == 0xF3) {
        rep = op;
      } else if (op == 0xF0) {
        locked = true;
      } else if (op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E) {
        seg_base = s.seg_base[(op >> 3) & 3];
      } else if (op == 0x64) {
        seg_base = s.seg_base[X86State::FS];
      } else if (op == 0x65) {
        seg_base = s.seg_base[X86State::GS];
      } else {
        break;
      }
    }
    if (__builtin_expect(s.trace, 0)) {
      std::fprintf(stderr, "%08x: %02x %02x %02x %02x", start, m[start], m[start + 1],
                   m[start + 2], m[start + 3]);
      if (s.trace_registers) {
        std::fprintf(stderr, " | eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x fl=%03x",
                     R[0], R[1], R[2], R[3], R[4], R[5], R[6], R[7], s.eflags & 0xFFF);
      }
      std::fputc('\n', stderr);
    }

    const int osz = op16 ? 2 : 4;
    const int obits = osz * 8;
    // XCHG with memory locks the bus even without the prefix.
    BusLock bus;
    Atomic atom;
    u32 saved_regs[8], saved_flags = 0;
    if (locked || op == 0x86 || op == 0x87) {
      bus.take();
      atom.on = true;
      memcpy(saved_regs, s.reg, sizeof saved_regs);
      saved_flags = s.eflags;
    }
    // The end of a locked instruction: its write goes in, or, if another
    // thread changed the operand meanwhile, it is undone to be run again.
    // Every way an instruction finishes passes through here.
    auto commit = [&]() -> bool {
      if (__builtin_expect(!atom.wrote, 1) || atomic_cas(m, atom.ea, atom.sz, atom.old, atom.now)) return true;
      memcpy(s.reg, saved_regs, sizeof saved_regs);
      s.eflags = saved_flags;
      return false;
    };

#define UNIMPL(msg) return stop(X86StopKind::Unimplemented, start, 0, (msg))
#define ILLEGAL(msg) return stop(X86StopKind::Illegal, start, 0, (msg))
#define DIVIDE_ERROR() return stop(X86StopKind::DivideError, start)

    // ---- operand helpers ----
    auto fetch8 = [&]() -> u32 { return m[ip++]; };
    auto fetch16 = [&]() -> u32 {
      const u32 v = ld(m, ip, 2);
      ip += 2;
      return v;
    };
    auto fetch32 = [&]() -> u32 {
      const u32 v = ld(m, ip, 4);
      ip += 4;
      return v;
    };
    auto fetch_imm = [&](int sz) -> u32 { return sz == 1 ? fetch8() : sz == 2 ? fetch16() : fetch32(); };

    auto reg_read = [&](int r, int sz) -> u32 {
      if (sz == 4) return R[r];
      if (sz == 2) return R[r] & 0xFFFF;
      return r < 4 ? (R[r] & 0xFF) : ((R[r - 4] >> 8) & 0xFF);  // AL CL DL BL AH CH DH BH
    };
    auto reg_write = [&](int r, int sz, u32 v) {
      if (sz == 4) {
        R[r] = v;
      } else if (sz == 2) {
        R[r] = (R[r] & 0xFFFF0000u) | (v & 0xFFFF);
      } else if (r < 4) {
        R[r] = (R[r] & 0xFFFFFF00u) | (v & 0xFF);
      } else {
        R[r - 4] = (R[r - 4] & 0xFFFF00FFu) | ((v & 0xFF) << 8);
      }
    };

    struct ModRM {
      u32 ea;
      u8 mod, reg, rm;
    };
    // Decodes a ModRM byte (plus SIB and displacement). `with_seg` is false
    // only for LEA, which wants the bare offset.
    auto decode = [&](bool with_seg = true) -> ModRM {
      const u8 b = m[ip++];
      ModRM r{0, (u8)(b >> 6), (u8)((b >> 3) & 7), (u8)(b & 7)};
      if (r.mod == 3) return r;
      u32 ea;
      if (r.rm == 4) {
        const u8 sib = m[ip++];
        const u32 index = (sib >> 3) & 7, base = sib & 7;
        ea = (index != 4) ? R[index] << (sib >> 6) : 0;
        if (base == 5 && r.mod == 0) {
          ea += fetch32();
        } else {
          ea += R[base];
        }
      } else if (r.rm == 5 && r.mod == 0) {
        ea = fetch32();
      } else {
        ea = R[r.rm];
      }
      if (r.mod == 1) {
        ea += (u32)(s32)(s8)m[ip++];
      } else if (r.mod == 2) {
        ea += fetch32();
      }
      r.ea = with_seg ? ea + seg_base : ea;
      return r;
    };
    auto rm_read = [&](const ModRM& r, int sz) -> u32 {
      if (r.mod == 3) return reg_read(r.rm, sz);
      if (__builtin_expect(atom.on, 0)) {
        if (!atom.have) {
          atom.have = true;
          atom.ea = r.ea;
          atom.sz = sz;
          atom.old = atom.now = atomic_ld(m, r.ea, sz);
        }
        if (r.ea == atom.ea && sz == atom.sz) return atom.now;
      }
      return ld(m, r.ea, sz);
    };
    auto rm_write = [&](const ModRM& r, int sz, u32 v) {
      if (r.mod == 3) {
        reg_write(r.rm, sz, v);
      } else if (__builtin_expect(atom.on && atom.have && r.ea == atom.ea && sz == atom.sz, 0)) {
        atom.now = v;
        atom.wrote = true;
      } else {
        st(m, r.ea, sz, v);
      }
    };

    // The store comes first so a faulting push leaves ESP untouched.
    auto push = [&](u32 v, int sz) {
      st(m, R[ESP] - (u32)sz, sz, v);
      R[ESP] -= (u32)sz;
    };
    auto pop = [&](int sz) -> u32 {
      const u32 v = ld(m, R[ESP], sz);
      R[ESP] += (u32)sz;
      return v;
    };
    auto load_seg = [&](int i, u32 sel) {
      s.seg[i] = (u16)sel;
      s.seg_base[i] = ((u16)sel && (u16)sel == s.tls_selector) ? s.tls_base : 0;
    };
    // BT/BTS/BTR/BTC. With a register bit offset and a memory operand, the
    // offset can reach outside the addressed word.
    auto bit_test = [&](int kind, ModRM r, u32 offset, bool offset_from_reg) {
      if (r.mod != 3 && offset_from_reg) {
        const s32 so = sext(offset, obits);
        r.ea += (u32)((so >> (osz == 2 ? 4 : 5)) * osz);
      }
      const u32 bit = offset & (u32)(obits - 1);
      u32 v = rm_read(r, osz);
      s.eflags = (s.eflags & ~CF) | ((v >> bit) & 1);
      if (kind == 0) return;
      if (kind == 1) {
        v |= 1u << bit;
      } else if (kind == 2) {
        v &= ~(1u << bit);
      } else {
        v ^= 1u << bit;
      }
      rm_write(r, osz, v);
    };

    // ---- the regular ALU block: 00-3F except the odd columns ----
    if (op < 0x40 && (op & 7) < 6) {
      const int aop = (op >> 3) & 7;
      switch (op & 7) {
        case 0:
        case 1: {  // Eb,Gb / Ev,Gv
          const int sz = (op & 1) ? osz : 1;
          const ModRM r = decode();
          const u32 res = alu(s, aop, rm_read(r, sz), reg_read(r.reg, sz), sz * 8);
          if (aop != 7) rm_write(r, sz, res);
          break;
        }
        case 2:
        case 3: {  // Gb,Eb / Gv,Ev
          const int sz = (op & 1) ? osz : 1;
          const ModRM r = decode();
          const u32 res = alu(s, aop, reg_read(r.reg, sz), rm_read(r, sz), sz * 8);
          if (aop != 7) reg_write(r.reg, sz, res);
          break;
        }
        case 4: {  // AL,Ib
          const u32 imm = fetch8();
          const u32 res = alu(s, aop, R[EAX] & 0xFF, imm, 8);
          if (aop != 7) reg_write(EAX, 1, res);
          break;
        }
        default: {  // eAX,Iv
          const u32 imm = fetch_imm(osz);
          const u32 res = alu(s, aop, reg_read(EAX, osz), imm, obits);
          if (aop != 7) reg_write(EAX, osz, res);
          break;
        }
      }
      if (!commit()) continue;
      s.eip = ip;
      left--;
      continue;
    }

    switch (op) {
      case 0x06: case 0x0E: case 0x16: case 0x1E:  // push es/cs/ss/ds
        push(s.seg[(op >> 3) & 3], osz);
        break;
      case 0x07: case 0x17: case 0x1F:  // pop es/ss/ds
        load_seg((op >> 3) & 3, pop(osz));
        break;

      case 0x40 ... 0x47:  // inc r
        reg_write(op & 7, osz, incdec(s, reg_read(op & 7, osz), false, obits));
        break;
      case 0x48 ... 0x4F:  // dec r
        reg_write(op & 7, osz, incdec(s, reg_read(op & 7, osz), true, obits));
        break;
      case 0x50 ... 0x57:  // push r
        push(reg_read(op & 7, osz), osz);
        break;
      case 0x58 ... 0x5F: {  // pop r
        const u32 v = pop(osz);
        reg_write(op & 7, osz, v);
        break;
      }
      case 0x60: {  // pusha
        const u32 sp = R[ESP];
        for (int i = 0; i < 8; i++) push(i == ESP ? sp : R[i], 4);
        break;
      }
      case 0x61:  // popa
        for (int i = 7; i >= 0; i--) {
          const u32 v = pop(4);
          if (i != ESP) R[i] = v;
        }
        break;
      case 0x68:  // push Iv
        push(fetch_imm(osz), osz);
        break;
      case 0x6A:  // push Ib
        push((u32)(s32)(s8)fetch8(), osz);
        break;
      case 0x69: case 0x6B: {  // imul Gv,Ev,Iv / imul Gv,Ev,Ib
        const ModRM r = decode();
        const s32 a = sext(rm_read(r, osz), obits);
        const s32 b = (op == 0x6B) ? (s32)(s8)fetch8() : sext(fetch_imm(osz), obits);
        const s64 wide = (s64)a * b;
        const u32 res = (u32)wide & mask_of(obits);
        const bool ovf = wide != (s64)sext(res, obits);
        reg_write(r.reg, osz, res);
        s.eflags = (s.eflags & ~(CF | OF)) | (ovf ? (CF | OF) : 0);
        break;
      }

      case 0x70 ... 0x7F: {  // jcc rel8
        const s32 d = (s8)fetch8();
        if (cond(s.eflags, op & 15)) ip += (u32)d;
        break;
      }

      case 0x80: case 0x81: case 0x82: case 0x83: {  // group 1: ALU Eb/Ev, imm
        const int sz = (op == 0x81 || op == 0x83) ? osz : 1;
        const ModRM r = decode();
        u32 imm = (op == 0x81) ? fetch_imm(osz) : (u32)(s32)(s8)fetch8();
        imm &= mask_of(sz * 8);
        const u32 res = alu(s, r.reg, rm_read(r, sz), imm, sz * 8);
        if (r.reg != 7) rm_write(r, sz, res);
        break;
      }
      case 0x84: case 0x85: {  // test
        const int sz = (op & 1) ? osz : 1;
        const ModRM r = decode();
        alu(s, 4, rm_read(r, sz), reg_read(r.reg, sz), sz * 8);
        break;
      }
      case 0x86: case 0x87: {  // xchg
        const int sz = (op & 1) ? osz : 1;
        const ModRM r = decode();
        const u32 a = rm_read(r, sz), b = reg_read(r.reg, sz);
        rm_write(r, sz, b);
        reg_write(r.reg, sz, a);
        break;
      }
      case 0x88: case 0x89: {  // mov Eb,Gb / Ev,Gv
        const int sz = (op & 1) ? osz : 1;
        const ModRM r = decode();
        rm_write(r, sz, reg_read(r.reg, sz));
        break;
      }
      case 0x8A: case 0x8B: {  // mov Gb,Eb / Gv,Ev
        const int sz = (op & 1) ? osz : 1;
        const ModRM r = decode();
        reg_write(r.reg, sz, rm_read(r, sz));
        break;
      }
      case 0x8C: {  // mov Ev,Sreg
        const ModRM r = decode();
        if (r.reg > 5) ILLEGAL("bad segment register");
        if (r.mod == 3) {
          reg_write(r.rm, osz, s.seg[r.reg]);
        } else {
          st(m, r.ea, 2, s.seg[r.reg]);
        }
        break;
      }
      case 0x8D: {  // lea
        const ModRM r = decode(false);
        if (r.mod == 3) ILLEGAL("lea with a register operand");
        reg_write(r.reg, osz, r.ea);
        break;
      }
      case 0x8E: {  // mov Sreg,Ew
        const ModRM r = decode();
        if (r.reg > 5 || r.reg == X86State::CS) ILLEGAL("bad segment register");
        load_seg(r.reg, rm_read(r, 2));
        break;
      }
      case 0x8F: {  // pop Ev (the address is formed after ESP moves)
        const u32 v = pop(osz);
        const ModRM r = decode();
        rm_write(r, osz, v);
        break;
      }

      case 0x90:  // nop / pause
        break;
      case 0x91 ... 0x97: {  // xchg eAX,r
        const u32 a = reg_read(EAX, osz), b = reg_read(op & 7, osz);
        reg_write(EAX, osz, b);
        reg_write(op & 7, osz, a);
        break;
      }
      case 0x98:  // cbw / cwde
        if (op16) {
          reg_write(EAX, 2, (u32)(s32)(s8)R[EAX]);
        } else {
          R[EAX] = (u32)(s32)(s16)R[EAX];
        }
        break;
      case 0x99:  // cwd / cdq
        if (op16) {
          reg_write(EDX, 2, (R[EAX] & 0x8000) ? 0xFFFF : 0);
        } else {
          R[EDX] = (s32)R[EAX] < 0 ? 0xFFFFFFFFu : 0;
        }
        break;
      case 0x9B:  // fwait
        break;
      case 0x9C:  // pushf
        push(s.eflags | 2, osz);
        break;
      case 0x9D: {  // popf
        // User code may change AC and ID too. Programs test for cpuid by
        // seeing whether ID can be flipped (SDL and the Intel compiler's
        // start-up code do), and take a CPU where it sticks for one that
        // has no cpuid, nor MMX or SSE.
        const u32 keep = kArithFlags | TF | DF | AC | ID;
        s.eflags = (s.eflags & ~keep) | (pop(osz) & keep);
        break;
      }
      case 0x9E:  // sahf
        s.eflags = (s.eflags & ~0xD5u) | ((R[EAX] >> 8) & 0xD5);
        break;
      case 0x9F:  // lahf
        reg_write(4, 1, (s.eflags & 0xD5) | 2);
        break;

      case 0xA0: reg_write(EAX, 1, ld(m, fetch32() + seg_base, 1)); break;      // mov AL,moffs
      case 0xA1: reg_write(EAX, osz, ld(m, fetch32() + seg_base, osz)); break;  // mov eAX,moffs
      case 0xA2: st(m, fetch32() + seg_base, 1, R[EAX]); break;                 // mov moffs,AL
      case 0xA3: st(m, fetch32() + seg_base, osz, R[EAX]); break;               // mov moffs,eAX

      // ---- string instructions; a REP prefix repeats them ECX times ----
      case 0xA4: case 0xA5: {  // movs
        const int sz = (op & 1) ? osz : 1;
        const u32 step = (s.eflags & DF) ? (u32)-sz : (u32)sz;
        for (u32 n = rep ? R[ECX] : 1; n; n--) {
          st(m, R[EDI], sz, ld(m, R[ESI] + seg_base, sz));
          R[ESI] += step;
          R[EDI] += step;
          if (rep) R[ECX]--;
        }
        break;
      }
      case 0xA6: case 0xA7: {  // cmps
        const int sz = (op & 1) ? osz : 1;
        const u32 step = (s.eflags & DF) ? (u32)-sz : (u32)sz;
        for (u32 n = rep ? R[ECX] : 1; n; n--) {
          alu(s, 7, ld(m, R[ESI] + seg_base, sz), ld(m, R[EDI], sz), sz * 8);
          R[ESI] += step;
          R[EDI] += step;
          if (rep) {
            R[ECX]--;
            // REPE (F3) goes on while equal, REPNE (F2) while different.
            if ((rep == 0xF3) != ((s.eflags & ZF) != 0)) break;
          }
        }
        break;
      }
      case 0xAA: case 0xAB: {  // stos
        const int sz = (op & 1) ? osz : 1;
        const u32 step = (s.eflags & DF) ? (u32)-sz : (u32)sz;
        for (u32 n = rep ? R[ECX] : 1; n; n--) {
          st(m, R[EDI], sz, R[EAX]);
          R[EDI] += step;
          if (rep) R[ECX]--;
        }
        break;
      }
      case 0xAC: case 0xAD: {  // lods
        const int sz = (op & 1) ? osz : 1;
        const u32 step = (s.eflags & DF) ? (u32)-sz : (u32)sz;
        for (u32 n = rep ? R[ECX] : 1; n; n--) {
          reg_write(EAX, sz, ld(m, R[ESI] + seg_base, sz));
          R[ESI] += step;
          if (rep) R[ECX]--;
        }
        break;
      }
      case 0xAE: case 0xAF: {  // scas
        const int sz = (op & 1) ? osz : 1;
        const u32 step = (s.eflags & DF) ? (u32)-sz : (u32)sz;
        for (u32 n = rep ? R[ECX] : 1; n; n--) {
          alu(s, 7, reg_read(EAX, sz), ld(m, R[EDI], sz), sz * 8);
          R[EDI] += step;
          if (rep) {
            R[ECX]--;
            if ((rep == 0xF3) != ((s.eflags & ZF) != 0)) break;
          }
        }
        break;
      }

      case 0xA8:  // test AL,Ib
        alu(s, 4, R[EAX] & 0xFF, fetch8(), 8);
        break;
      case 0xA9:  // test eAX,Iv
        alu(s, 4, reg_read(EAX, osz), fetch_imm(osz), obits);
        break;

      case 0xB0 ... 0xB7:  // mov r8,Ib
        reg_write(op & 7, 1, fetch8());
        break;
      case 0xB8 ... 0xBF:  // mov r,Iv
        reg_write(op & 7, osz, fetch_imm(osz));
        break;

      case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {  // group 2: shifts
        const int sz = (op & 1) ? osz : 1;
        const ModRM r = decode();
        const u32 count = (op < 0xD0) ? fetch8() : (op < 0xD2 ? 1 : (R[ECX] & 0xFF));
        rm_write(r, sz, shift(s, r.reg, rm_read(r, sz), count, sz * 8));
        break;
      }

      case 0xC2: {  // ret Iw
        const u32 n = fetch16();
        ip = pop(4);
        R[ESP] += n;
        break;
      }
      case 0xC3:  // ret
        ip = pop(4);
        break;
      case 0xC6: case 0xC7: {  // mov Eb,Ib / Ev,Iv
        const int sz = (op & 1) ? osz : 1;
        const ModRM r = decode();
        rm_write(r, sz, fetch_imm(sz));
        break;
      }
      case 0xC8: {  // enter
        const u32 size = fetch16();
        if (fetch8() & 31) UNIMPL("enter with nested frames");
        push(R[EBP], 4);
        R[EBP] = R[ESP];
        R[ESP] -= size;
        break;
      }
      case 0xC9:  // leave
        R[ESP] = R[EBP];
        R[EBP] = pop(4);
        break;
      case 0xCC:  // int3
        left--;
        return stop(X86StopKind::Breakpoint, ip, 3);
      case 0xCD: {  // int n
        const u8 vector = (u8)fetch8();
        left--;
        return stop(X86StopKind::Int, ip, vector);
      }

      case 0xD7:  // xlat
        reg_write(EAX, 1, m[R[EBX] + (R[EAX] & 0xFF) + seg_base]);
        break;
      case 0xD8 ... 0xDF: {  // x87
        const u8 modrm = m[ip];
        const ModRM r = decode();
        if (!x87_exec(s, op, modrm, r.ea)) UNIMPL("x87 instruction");
        break;
      }

      case 0xE0: case 0xE1: case 0xE2: {  // loopne / loope / loop
        const s32 d = (s8)fetch8();
        R[ECX]--;
        bool take = R[ECX] != 0;
        if (op == 0xE0) take = take && !(s.eflags & ZF);
        if (op == 0xE1) take = take && (s.eflags & ZF);
        if (take) ip += (u32)d;
        break;
      }
      case 0xE3: {  // jecxz
        const s32 d = (s8)fetch8();
        if (!R[ECX]) ip += (u32)d;
        break;
      }
      case 0xE8: {  // call rel32
        const u32 d = fetch32();
        push(ip, 4);
        ip += d;
        break;
      }
      case 0xE9: {  // jmp rel32
        const u32 d = fetch32();
        ip += d;
        break;
      }
      case 0xEB: {  // jmp rel8
        const s32 d = (s8)fetch8();
        ip += (u32)d;
        break;
      }

      case 0xF4:
        return stop(X86StopKind::Halt, start);
      case 0xF5: s.eflags ^= CF; break;   // cmc
      case 0xF8: s.eflags &= ~CF; break;  // clc
      case 0xF9: s.eflags |= CF; break;   // stc
      case 0xFC: s.eflags &= ~DF; break;  // cld
      case 0xFD: s.eflags |= DF; break;   // std

      case 0xF6: case 0xF7: {  // group 3: test/not/neg/mul/imul/div/idiv
        const int sz = (op & 1) ? osz : 1;
        const int bits = sz * 8;
        const ModRM r = decode();
        const u32 v = rm_read(r, sz);
        switch (r.reg) {
          case 0:
          case 1:
            alu(s, 4, v, fetch_imm(sz), bits);
            break;
          case 2:
            rm_write(r, sz, ~v);
            break;
          case 3:
            rm_write(r, sz, alu(s, 5, 0, v, bits));
            break;
          case 4: {  // mul
            bool high;
            if (sz == 1) {
              const u32 p = (R[EAX] & 0xFF) * v;
              reg_write(EAX, 2, p);
              high = (p >> 8) != 0;
            } else if (sz == 2) {
              const u32 p = (R[EAX] & 0xFFFF) * v;
              reg_write(EAX, 2, p);
              reg_write(EDX, 2, p >> 16);
              high = (p >> 16) != 0;
            } else {
              const u64 p = (u64)R[EAX] * v;
              R[EAX] = (u32)p;
              R[EDX] = (u32)(p >> 32);
              high = (p >> 32) != 0;
            }
            s.eflags = (s.eflags & ~(CF | OF)) | (high ? (CF | OF) : 0);
            break;
          }
          case 5: {  // imul
            bool ovf;
            if (sz == 1) {
              const s32 p = (s32)(s8)R[EAX] * (s32)(s8)v;
              reg_write(EAX, 2, (u32)p);
              ovf = p != (s32)(s8)p;
            } else if (sz == 2) {
              const s32 p = (s32)(s16)R[EAX] * (s32)(s16)v;
              reg_write(EAX, 2, (u32)p);
              reg_write(EDX, 2, (u32)p >> 16);
              ovf = p != (s32)(s16)p;
            } else {
              const s64 p = (s64)(s32)R[EAX] * (s32)v;
              R[EAX] = (u32)p;
              R[EDX] = (u32)((u64)p >> 32);
              ovf = p != (s64)(s32)p;
            }
            s.eflags = (s.eflags & ~(CF | OF)) | (ovf ? (CF | OF) : 0);
            break;
          }
          case 6: {  // div
            if (!v) DIVIDE_ERROR();
            if (sz == 1) {
              const u32 n = R[EAX] & 0xFFFF;
              const u32 q = n / v;
              if (q > 0xFF) DIVIDE_ERROR();
              reg_write(EAX, 2, ((n % v) << 8) | q);
            } else if (sz == 2) {
              const u32 n = ((R[EDX] & 0xFFFF) << 16) | (R[EAX] & 0xFFFF);
              const u32 q = n / v;
              if (q > 0xFFFF) DIVIDE_ERROR();
              reg_write(EAX, 2, q);
              reg_write(EDX, 2, n % v);
            } else {
              const u64 n = ((u64)R[EDX] << 32) | R[EAX];
              const u64 q = n / v;
              if (q > 0xFFFFFFFFull) DIVIDE_ERROR();
              R[EAX] = (u32)q;
              R[EDX] = (u32)(n % v);
            }
            break;
          }
          default: {  // idiv
            if (!v) DIVIDE_ERROR();
            if (sz == 1) {
              const s32 n = (s16)R[EAX], d = (s8)v;
              const s32 q = n / d;
              if (q != (s32)(s8)q) DIVIDE_ERROR();
              reg_write(EAX, 2, (((u32)(n % d) & 0xFF) << 8) | ((u32)q & 0xFF));
            } else if (sz == 2) {
              const s32 n = (s32)(((R[EDX] & 0xFFFF) << 16) | (R[EAX] & 0xFFFF));
              const s32 d = (s16)v;
              if (n == INT32_MIN && d == -1) DIVIDE_ERROR();
              const s32 q = n / d;
              if (q != (s32)(s16)q) DIVIDE_ERROR();
              reg_write(EAX, 2, (u32)q);
              reg_write(EDX, 2, (u32)(n % d));
            } else {
              const s64 n = (s64)(((u64)R[EDX] << 32) | R[EAX]);
              const s64 d = (s32)v;
              if (n == INT64_MIN && d == -1) DIVIDE_ERROR();
              const s64 q = n / d;
              if (q != (s64)(s32)q) DIVIDE_ERROR();
              R[EAX] = (u32)q;
              R[EDX] = (u32)(n % d);
            }
            break;
          }
        }
        break;
      }

      case 0xFE: {  // group 4: inc/dec Eb
        const ModRM r = decode();
        if (r.reg > 1) ILLEGAL("bad group 4 opcode");
        rm_write(r, 1, incdec(s, rm_read(r, 1), r.reg == 1, 8));
        break;
      }
      case 0xFF: {  // group 5
        const ModRM r = decode();
        switch (r.reg) {
          case 0:
          case 1:
            rm_write(r, osz, incdec(s, rm_read(r, osz), r.reg == 1, obits));
            break;
          case 2: {  // call Ev
            const u32 target = rm_read(r, 4);
            push(ip, 4);
            ip = target;
            break;
          }
          case 4:  // jmp Ev
            ip = rm_read(r, 4);
            break;
          case 6:  // push Ev
            push(rm_read(r, osz), osz);
            break;
          default:
            UNIMPL("far call or jump");
        }
        break;
      }

      case 0x0F: {
        const u8 op2 = m[ip++];
        switch (op2) {
          case 0x0B:
            ILLEGAL("ud2");
          case 0x18 ... 0x1F:  // prefetch hints and the long nop
            (void)decode();
            break;
          case 0x31: {  // rdtsc: one tick per nanosecond
            const u64 t = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            R[EAX] = (u32)t;
            R[EDX] = (u32)(t >> 32);
            break;
          }
          case 0x34:  // sysenter
            left--;
            return stop(X86StopKind::Sysenter, ip);
          case 0x40 ... 0x4F: {  // cmovcc
            const ModRM r = decode();
            const u32 v = rm_read(r, osz);
            if (cond(s.eflags, op2 & 15)) reg_write(r.reg, osz, v);
            break;
          }
          case 0x80 ... 0x8F: {  // jcc rel32
            const u32 d = fetch32();
            if (cond(s.eflags, op2 & 15)) ip += d;
            break;
          }
          case 0x90 ... 0x9F: {  // setcc
            const ModRM r = decode();
            rm_write(r, 1, cond(s.eflags, op2 & 15) ? 1 : 0);
            break;
          }
          case 0xA0: push(s.seg[X86State::FS], osz); break;
          case 0xA1: load_seg(X86State::FS, pop(osz)); break;
          case 0xA8: push(s.seg[X86State::GS], osz); break;
          case 0xA9: load_seg(X86State::GS, pop(osz)); break;
          case 0xA2: {  // cpuid: a Core Duo, the first Intel Mac CPU
            const u32 leaf = R[EAX];
            u32 a = 0, b = 0, c = 0, d = 0;
            if (leaf == 0) {
              a = 1;
              b = 0x756E6547;  // "GenuineIntel"
              d = 0x49656E69;
              c = 0x6C65746E;
            } else if (leaf == 1) {
              a = 0x000006E8;  // family 6, model 14
              b = 0x00010800;
              c = 0x00000001;  // SSE3
              d = 0x07888111;  // FPU TSC CX8 CMOV CLFSH MMX FXSR SSE SSE2
            } else if (leaf == 0x80000000u) {
              a = 0x80000000u;
            }
            R[EAX] = a;
            R[EBX] = b;
            R[ECX] = c;
            R[EDX] = d;
            break;
          }
          case 0xA3: case 0xAB: case 0xB3: case 0xBB: {  // bt/bts/btr/btc Ev,Gv
            const ModRM r = decode();
            bit_test((op2 >> 3) & 3, r, reg_read(r.reg, osz), true);
            break;
          }
          case 0xBA: {  // bt/bts/btr/btc Ev,Ib
            const ModRM r = decode();
            if (r.reg < 4) ILLEGAL("bad group 8 opcode");
            const u32 offset = fetch8();
            bit_test(r.reg - 4, r, offset, false);
            break;
          }
          case 0xA4: case 0xA5: case 0xAC: case 0xAD: {  // shld / shrd
            const ModRM r = decode();
            const u32 count = (op2 & 1) ? R[ECX] : fetch8();
            rm_write(r, osz, shift_double(s, op2 >= 0xAC, rm_read(r, osz), reg_read(r.reg, osz),
                                          count, obits));
            break;
          }
          case 0x77:  // emms: hand the registers back to the x87
            s.ftag = 0;
            break;
          case 0xAE: {
            const u8 modrm = m[ip];
            const ModRM r = decode();
            if (r.mod == 3 && r.reg >= 5) break;  // lfence / mfence / sfence
            if (r.mod != 3 && r.reg == 7) break;  // clflush
            if (!sse_exec(s, op2, 0, modrm, r.ea, ip)) UNIMPL("0F AE group");
            break;  // fxsave, fxrstor, ldmxcsr, stmxcsr
          }
          case 0xAF: {  // imul Gv,Ev
            const ModRM r = decode();
            const s64 wide = (s64)sext(reg_read(r.reg, osz), obits) * sext(rm_read(r, osz), obits);
            const u32 res = (u32)wide & mask_of(obits);
            const bool ovf = wide != (s64)sext(res, obits);
            reg_write(r.reg, osz, res);
            s.eflags = (s.eflags & ~(CF | OF)) | (ovf ? (CF | OF) : 0);
            break;
          }
          case 0xB0: case 0xB1: {  // cmpxchg
            const int sz = (op2 & 1) ? osz : 1;
            const ModRM r = decode();
            const u32 dst = rm_read(r, sz), acc = reg_read(EAX, sz);
            alu(s, 7, acc, dst, sz * 8);
            if (acc == dst) {
              rm_write(r, sz, reg_read(r.reg, sz));
            } else {
              reg_write(EAX, sz, dst);
            }
            break;
          }
          case 0xB6: case 0xB7: {  // movzx
            const ModRM r = decode();
            reg_write(r.reg, osz, rm_read(r, op2 == 0xB6 ? 1 : 2));
            break;
          }
          case 0xBE: case 0xBF: {  // movsx
            const ModRM r = decode();
            const int ssz = op2 == 0xBE ? 1 : 2;
            reg_write(r.reg, osz, (u32)sext(rm_read(r, ssz), ssz * 8));
            break;
          }
          case 0xBC: case 0xBD: {  // bsf / bsr
            const ModRM r = decode();
            const u32 v = rm_read(r, osz);
            if (!v) {
              s.eflags |= ZF;
            } else {
              s.eflags &= ~ZF;
              reg_write(r.reg, osz, op2 == 0xBC ? (u32)__builtin_ctz(v) : 31 - (u32)__builtin_clz(v));
            }
            break;
          }
          case 0xC0: case 0xC1: {  // xadd
            const int sz = (op2 & 1) ? osz : 1;
            const ModRM r = decode();
            const u32 dst = rm_read(r, sz), src = reg_read(r.reg, sz);
            const u32 sum = alu(s, 0, dst, src, sz * 8);
            reg_write(r.reg, sz, dst);
            rm_write(r, sz, sum);
            break;
          }
          case 0xC7: {  // cmpxchg8b
            const ModRM r = decode();
            if (r.reg != 1 || r.mod == 3) ILLEGAL("bad group 9 opcode");
            if ((r.ea & 7) == 0) {  // a real 64-bit compare-and-swap (see Atomic above)
              u64 expect = (u64)R[EDX] << 32 | R[EAX];
              const u64 want = (u64)R[ECX] << 32 | R[EBX];
              if (__atomic_compare_exchange_n((u64*)(m + r.ea), &expect, want, false, __ATOMIC_SEQ_CST,
                                              __ATOMIC_SEQ_CST)) {
                s.eflags |= ZF;
              } else {
                R[EAX] = (u32)expect;
                R[EDX] = (u32)(expect >> 32);
                s.eflags &= ~ZF;
              }
              break;
            }
            const u32 lo = ld(m, r.ea, 4), hi = ld(m, r.ea + 4, 4);
            if (lo == R[EAX] && hi == R[EDX]) {
              st(m, r.ea, 4, R[EBX]);
              st(m, r.ea + 4, 4, R[ECX]);
              s.eflags |= ZF;
            } else {
              R[EAX] = lo;
              R[EDX] = hi;
              s.eflags &= ~ZF;
            }
            break;
          }
          case 0xC8 ... 0xCF:  // bswap
            R[op2 & 7] = __builtin_bswap32(R[op2 & 7]);
            break;
          default: {  // MMX and SSE; F2/F3/66 select the variant
            const int prefix = rep ? rep : (op16 ? 0x66 : 0);
            const u8 modrm = m[ip];
            const ModRM r = decode();
            if (!sse_exec(s, op2, prefix, modrm, r.ea, ip)) UNIMPL("two-byte opcode");
            break;
          }
        }
        break;
      }

      default:
        UNIMPL("one-byte opcode");
    }

    if (!commit()) continue;
    s.eip = ip;
    left--;
  }
}
