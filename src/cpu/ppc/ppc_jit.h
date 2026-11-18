#pragma once

#include "cpu/ppc/ppc.h"

// The PowerPC translator (ppc_jit.cpp). ppc_run, declared in ppc.h, goes
// through it unless COCAINE_JIT=0 or a debugging aid needs the interpreter.

// Guest memory from `addr` on has been unmapped, mapped over or changed
// under the guest's own instruction-cache flush: forget translations of it.
void ppc_jit_invalidate(u32 addr, u64 size);

// For crash reports. If the fault described by `context` (what a signal
// handler is given) happened in translated code, brings `s` up to date: its
// pc, which translated code doesn't keep, and the registers that were being
// held in host registers. Safe to call from a fault handler.
bool ppc_jit_fault(const void* context, PPCState& s);

// Held across fork by the forking thread, like the kernel layer's own locks.
void ppc_jit_fork_lock(bool take);

// For working on the translator: with COCAINE_JIT_STATS set, prints what it did.
void ppc_jit_report();
