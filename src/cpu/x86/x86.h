#pragma once

#include "common.h"
#include "cpu/x86/f80.h"

// User-mode state of one 32-bit Intel thread: a Core Duo, the first Intel Mac
// CPU (integer, x87, MMX, SSE, SSE2, SSE3).
struct X86State {
  enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };
  enum { ES, CS, SS, DS, FS, GS };

  u32 reg[8] = {};
  u32 eip = 0;
  u32 eflags = 0x202;
  // Flags the translator has put off working out: an operation and its
  // operands, from which it sets eflags once somebody wants them. Zero
  // (nothing waiting) whenever anything but translated code is looking.
  u32 lazy_kind = 0, lazy_a = 0, lazy_b = 0;
  u16 seg[6] = {};
  u32 seg_base[6] = {};  // all zero except the thread-local segment

  // The one selector with a non-zero base: Mac OS X points %gs at the
  // thread's data. The kernel layer sets these.
  u16 tls_selector = 0;
  u32 tls_base = 0;

  // x87: eight 80-bit registers used as a stack, whose top index lives in
  // bits 11-13 of the status word. The MMX registers are the significands of
  // these same registers.
  F80 fpr[8];
  u16 fcw = 0x037F;  // control word: all exceptions masked, 64-bit precision
  u16 fsw = 0;       // status word
  u8 ftag = 0;       // bit i set: physical register i holds a value

  // SSE.
  union Xmm {
    u8 b[16];
    u16 w[8];
    u32 d[4];
    u64 q[2];
  } xmm[8] = {};
  u32 mxcsr = 0x1F80;  // all exceptions masked, round to nearest

  bool trace = false;
  bool trace_registers = false;  // with trace: also print the registers
  u8* mem = nullptr;  // host address of guest address 0
  u64 icount = 0;     // instructions executed so far

  // For translated code (x86_jit.cpp): the countdown to going back to the
  // kernel layer, where its blocks are, and a word passed out along with eip.
  s64 jit_budget = 0;
  u64* jit_table = nullptr;
  u8* jit_base = nullptr;
  u32 jit_arg = 0;
};

// A host call: `int 0xFE` followed by a 16-bit identifier, which hands control
// to the kernel layer. cocaine plants these in code it supplies itself, such
// as the comm page, so that those routines run as native host code. It
// arrives as an ordinary Int stop; the kernel layer reads and skips the
// identifier.
constexpr u8 kX86HostCallVector = 0xFE;

enum class X86StopKind {
  Int,            // `int n`; eip is already past it, vector says which
  Sysenter,       // `sysenter`
  Breakpoint,     // `int3`; eip is past it
  DivideError,    // divide by zero or quotient overflow; eip is at the instruction
  Halt,           // `hlt`, privileged in user mode; eip is at the instruction
  Illegal,        // invalid opcode; eip is at the instruction
  Unimplemented,  // valid but not emulated yet; eip is at the instruction
  Budget,         // ran max_insns instructions
};

struct X86Stop {
  X86StopKind kind;
  u32 eip;
  u8 vector;
  const char* what;  // short reason for Illegal/Unimplemented
};

// Runs guest code until something needs the kernel layer's attention: through
// the translator (x86_jit.cpp) where it can, else the interpreter.
X86Stop x86_run(X86State& s, u64 max_insns);

// The interpreter (x86_interp.cpp): the definition of what every instruction
// does, and what the translator falls back on.
X86Stop x86_interp(X86State& s, u64 max_insns);
// The interpreter's arithmetic, for the translator to call when the flags an
// instruction leaves are wanted: each returns the result and updates eflags.
u32 x86_alu(X86State* s, u32 op, u32 a, u32 b, u32 bits);     // ADD OR ADC SBB AND SUB XOR CMP
u32 x86_incdec(X86State* s, u32 a, u32 dec, u32 bits);
u32 x86_shift(X86State* s, u32 kind, u32 a, u32 count, u32 bits);

// The translator. Guest memory from `addr` on has changed hands: forget
// translations of it. For crash reports: bring `s` up to date if the fault
// in `context` was in translated code. And the lock to hold across fork.
void x86_jit_invalidate(u32 addr, u64 size);
bool x86_jit_fault(const void* context, X86State& s);
void x86_jit_fork_lock(bool take);
void x86_jit_report();  // with COCAINE_JIT_STATS: what was left to the interpreter
// Says whether a guest address is in memory the guest can write: code there
// may change under the translator, which then checks it before each use.
extern bool (*x86_jit_writable)(u32 addr);
extern bool (*x86_jit_readable)(u32 addr);  // and whether it can read there, before looking ahead through a jump

// ---- pieces of the core that live in their own files ------------------------

// Executes one x87 instruction. `op` is its first byte (D8-DF), `modrm` the
// byte after it, and `ea` the address of its memory operand if it has one.
// Returns false for an instruction that isn't implemented.
bool x87_exec(X86State& s, u8 op, u8 modrm, u32 ea);
void x87_fxsave(const X86State& s, u32 ea);
void x87_fxrstor(X86State& s, u32 ea);

// Executes one MMX/SSE instruction from the 0F xx opcode space. `prefix` is
// its mandatory prefix (0, 0x66, 0xF2 or 0xF3) and `ip` points past the ModRM
// bytes, where an immediate operand would be. Returns false for an
// instruction that isn't implemented.
bool sse_exec(X86State& s, u8 op2, int prefix, u8 modrm, u32 ea, u32& ip);
// Applies the MXCSR rounding mode to host floating point.
void sse_apply_rounding(u32 mxcsr);

// Takes or releases the lock behind the `lock` prefix. Held across a fork so
// the child doesn't inherit it taken by a thread that isn't there.
void x86_bus_lock(bool take);
