#pragma once

#include "common.h"

// User-mode state of one 32-bit PowerPC thread: a G4, with AltiVec.
struct PPCState {
  u32 gpr[32] = {};
  union FPR {
    double d;
    u64 u;
  } fpr[32] = {};

  // AltiVec registers. Each is stored byte-reversed: the guest numbers
  // elements from the most significant end, so guest element i is host
  // element (count - 1 - i), and each element is then in host byte order.
  union Vec {
    u8 b[16];
    s8 sb[16];
    u16 h[8];
    s16 sh[8];
    u32 w[4];
    s32 sw[4];
    float f[4];
  } vr[32] = {};
  u32 vscr = 0x00010000;  // "non-Java" mode, as Mac OS X sets it; bit 0 is SAT
  u32 pc = 0;
  u32 lr = 0, ctr = 0;
  u32 cr = 0;     // CR0 is the top four bits
  u32 xer = 0;    // SO/OV/CA in the top three bits, string byte count at the bottom
  u32 fpscr = 0;  // only the rounding mode and compare bits are kept accurate
  u32 vrsave = 0;
  u32 sprg3 = 0;  // per-thread pointer that user code reads as SPR 259

  // lwarx/stwcx. reservation. stwcx. succeeds only if the word still holds
  // the value lwarx saw, which makes the pair a compare-and-swap.
  u32 reserve_addr = 0, reserve_val = 0;
  bool reserved = false;

  bool trace = false;
  bool trace_registers = false;  // with trace: also print the registers
  u8* mem = nullptr;  // host address of guest address 0
  u64 icount = 0;     // instructions executed so far
  u32 watch_return = 0;  // for COCAINE_CALLS: where the watched call in progress returns to
  u32 watch_first = 0;   // ... and its first argument, often where its answer is to go
  u32 jit_arg = 0;       // a word passed out of translated code along with pc
};

// A host call: an instruction word no real program contains (primary opcode
// 1), which hands control to the kernel layer with the low 16 bits as an
// identifier. cocaine plants these in code it supplies itself, such as the
// comm page, so that those routines run as native host code.
constexpr u32 kPPCHostCall = 1u << 26;

enum class PPCStopKind {
  Syscall,  // `sc`; pc is already past it
  HostCall, // a host call word; pc is already past it
  Trap,     // `tw`/`twi` whose condition held; pc is at the instruction
  Illegal,  // unknown or unimplemented instruction; pc is at the instruction
  Budget,   // ran max_insns instructions
};

struct PPCStop {
  PPCStopKind kind;
  u32 pc;
  u32 insn;
};

// Runs guest code until something needs the kernel layer's attention: through
// the translator (ppc_jit.cpp) where it can, else the interpreter.
PPCStop ppc_run(PPCState& s, u64 max_insns);

// The interpreter (ppc_interp.cpp), one instruction at a time. It is the
// definition of what every instruction does: the translator's output is
// tested against it, and falls back on it for anything rare.
PPCStop ppc_interp(PPCState& s, u64 max_insns);
// Whether a debugging aid that only the interpreter honours is switched on.
bool ppc_debugging(const PPCState& s);
// Sets the host's floating point rounding to what the guest has asked for.
void ppc_apply_rounding(u32 fpscr);

// For working on cocaine: a guest address whose word-sized stores are logged
// with the place in the program that made them (COCAINE_WATCH). None by default.
extern u32 g_ppc_watch;

// AltiVec lives in altivec.cpp. Both return false for an instruction they
// don't implement.
bool altivec_exec(PPCState& s, u32 insn);    // primary opcode 4
bool altivec_memory(PPCState& s, u32 insn);  // the vector loads and stores under opcode 31
