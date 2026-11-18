// Signals.
//
// A guest process is a host process, and the two systems number their signals
// the same way, so the host kernel does the hard parts: sending, blocking,
// pending, timers, and waking a blocked call. Each guest thread's signal mask
// is simply its host thread's mask.
//
// What is left is calling the guest's handler. A signal the guest handles has
// a small host handler that only notes it arrived. The guest thread notices
// between instructions slices and system calls, and then does what the real
// kernel does: saves the thread's registers in a frame on the guest stack and
// points the thread at the C library's trampoline, which calls the handler and
// then asks, through sigreturn, to have the registers put back.
//
// Not here yet: handing a guest's own crash (a bad memory access, say) to a
// handler it installed for that.

#include <signal.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>

#include "kernel/process.h"

namespace {

constexpr u32 bit(int sig) { return 1u << (sig - 1); }

// cocaine needs these itself, to report guest crashes.
constexpr u32 kOurs = bit(SIGSEGV) | bit(SIGBUS) | bit(SIGILL) | bit(SIGFPE) | bit(SIGTRAP);

// sigaction flags, the same on Mac OS X 10.5 as on this Mac.
enum : u32 {
  kOnStack = 0x01,
  kRestart = 0x02,
  kResetHand = 0x04,
  kNoChildStop = 0x08,
  kNoDefer = 0x10,
  kNoChildWait = 0x20,
  kSigInfo = 0x40,
};
// How the handler wants to be called, as the trampoline is told.
enum : u32 { kStyleTraditional = 1, kStyleSigInfo = 30, kStyleVectorExtra = 5 };
enum : u32 { kSetAltStack = 0x40000000u, kResetAltStack = 0x80000000u };
enum : u32 { kStackDisabled = 4, kStackInUse = 1 };

// Sizes of what goes on the guest stack.
constexpr u32 kUcontextSize = 32, kSigInfoSize = 64;
constexpr u32 kX86McontextSize = 12 + 64 + 524;          // exception, registers, floating point
constexpr u32 kPPCMcontextSize = 32 + 160 + 264 + 576;   // ... and AltiVec

std::atomic<u32> g_arrived{0};  // one bit per signal waiting for its guest handler
siginfo_t g_info[32];           // the details of the latest arrival of each

void on_guest_signal(int sig, siginfo_t* info, void*) {
  if (sig < 1 || sig > 31) return;
  if (info) g_info[sig] = *info;
  g_arrived.fetch_or(bit(sig), std::memory_order_relaxed);
}

u32 host_mask() {
  sigset_t cur = 0;
  pthread_sigmask(SIG_SETMASK, nullptr, &cur);
  return (u32)cur;
}

void set_host_mask(u32 mask) {
  const sigset_t m = mask & ~kOurs;
  pthread_sigmask(SIG_SETMASK, &m, nullptr);
}

}  // namespace

bool Process::signal_arrived() const { return g_arrived.load(std::memory_order_relaxed) != 0; }

bool Process::signal_deliverable() const {
  const u32 arrived = g_arrived.load(std::memory_order_relaxed);
  return arrived && (arrived & ~host_mask());
}

void Process::forget_pending_signals() { g_arrived = 0; }

SysRet Process::sys_sigaction(int sig, u32 act, u32 old) {
  if (sig < 1 || sig > 31) return SysRet::err(EINVAL);
  if ((act && !mem_.range_ok(act, 16)) || (old && !mem_.range_ok(old, 12))) return SysRet::err(EFAULT);
  std::lock_guard<std::mutex> lk(kmu_);
  GuestSigaction& sa = sigactions_[sig];
  const GuestSigaction before = sa;
  if (act) {
    if (sig == SIGKILL || sig == SIGSTOP) return SysRet::err(EINVAL);
    // What the C library passes in has its trampoline's address between the
    // handler and the mask.
    sa.handler = mem_.r32(act);
    sa.tramp = mem_.r32(act + 4);
    sa.mask = mem_.r32(act + 8);
    sa.flags = mem_.r32(act + 12);
    if (!(bit(sig) & kOurs)) {
      struct sigaction host = {};
      host.sa_flags = (int)(sa.flags & (kNoChildStop | kNoChildWait));
      if (sa.handler == 0) {
        host.sa_handler = SIG_DFL;
      } else if (sa.handler == 1) {
        host.sa_handler = SIG_IGN;
      } else {
        // No SA_RESTART: a blocked host call must come back so the guest's
        // handler can run. Whether the guest's call restarts is decided later.
        host.sa_sigaction = on_guest_signal;
        host.sa_flags |= SA_SIGINFO;
        sigfillset(&host.sa_mask);
      }
      sigaction(sig, &host, nullptr);
    }
  }
  if (old) {
    mem_.w32(old, before.handler);
    mem_.w32(old + 4, before.mask);
    mem_.w32(old + 8, before.flags);
  }
  return SysRet::ok();
}

SysRet Process::sys_sigprocmask(int how, u32 set, u32 old) {
  if ((set && !mem_.range_ok(set, 4)) || (old && !mem_.range_ok(old, 4))) return SysRet::err(EFAULT);
  sigset_t before = 0;
  if (set) {
    const sigset_t m = mem_.r32(set) & ~kOurs;
    if (pthread_sigmask(how, &m, &before) != 0) return SysRet::err(EINVAL);
  } else {
    pthread_sigmask(SIG_SETMASK, nullptr, &before);
  }
  if (old) mem_.w32(old, (u32)before);
  return SysRet::ok();
}

// sigsuspend: swap in a mask, sleep until a signal's handler has to run, swap
// the old mask back. It always "fails" with EINTR.
SysRet Process::sys_sigsuspend(Thread& t, u32 mask) {
  sigset_t wait_mask = mask & ~kOurs, everything, before;
  sigfillset(&everything);
  // With everything blocked nothing can slip in between looking and sleeping.
  pthread_sigmask(SIG_BLOCK, &everything, &before);
  if (!(g_arrived.load() & ~(u32)wait_mask)) sigsuspend(&wait_mask);
  pthread_sigmask(SIG_SETMASK, &before, nullptr);
  // The handler has to run although the old mask, now back, may block it.
  t.suspended = true;
  t.suspend_mask = (u32)wait_mask;
  return SysRet::err(EINTR);
}

SysRet Process::sys_sigaltstack(Thread& t, u32 ss, u32 old) {
  if ((ss && !mem_.range_ok(ss, 12)) || (old && !mem_.range_ok(old, 12))) return SysRet::err(EFAULT);
  const u32 was_sp = t.altstack_sp, was_size = t.altstack_size;
  if (ss) {
    if (t.on_altstack) return SysRet::err(EPERM);
    if (mem_.r32(ss + 8) & kStackDisabled) {
      t.altstack_sp = t.altstack_size = 0;
    } else {
      t.altstack_sp = mem_.r32(ss);
      t.altstack_size = mem_.r32(ss + 4);
    }
  }
  if (old) {
    mem_.w32(old, was_sp);
    mem_.w32(old + 4, was_size);
    mem_.w32(old + 8, (was_size ? 0 : kStackDisabled) | (t.on_altstack ? kStackInUse : 0));
  }
  return SysRet::ok();
}

SysRet Process::sys_sigwait(u32 set, u32 sig_out) {
  if (!mem_.range_ok(set, 4) || (sig_out && !mem_.range_ok(sig_out, 4))) return SysRet::err(EFAULT);
  const sigset_t want = mem_.r32(set) & ~kOurs;
  int sig = 0;
  // One that already arrived for a handler counts too.
  const u32 here = g_arrived.load() & (u32)want;
  if (here) {
    sig = __builtin_ctz(here) + 1;
    g_arrived.fetch_and(~bit(sig));
  } else if (const int rc = sigwait(&want, &sig)) {
    return SysRet::err(rc);
  }
  if (sig_out) mem_.w32(sig_out, (u32)sig);
  return SysRet::ok();
}

// setitimer and getitimer: the host's timers send the host's SIGALRM, which
// is the guest's.
SysRet Process::sys_setitimer(int which, u32 value, u32 old, bool set) {
  if ((value && !mem_.range_ok(value, 16)) || (old && !mem_.range_ok(old, 16))) return SysRet::err(EFAULT);
  struct itimerval nv = {}, ov = {};
  if (value) {
    nv.it_interval.tv_sec = (s32)mem_.r32(value);
    nv.it_interval.tv_usec = (s32)mem_.r32(value + 4);
    nv.it_value.tv_sec = (s32)mem_.r32(value + 8);
    nv.it_value.tv_usec = (s32)mem_.r32(value + 12);
  }
  const int rc = set ? setitimer(which, value ? &nv : nullptr, &ov) : getitimer(which, &ov);
  if (rc != 0) return SysRet::err(errno);
  if (old) {
    mem_.w32(old, (u32)ov.it_interval.tv_sec);
    mem_.w32(old + 4, (u32)ov.it_interval.tv_usec);
    mem_.w32(old + 8, (u32)ov.it_value.tv_sec);
    mem_.w32(old + 12, (u32)ov.it_value.tv_usec);
  }
  return SysRet::ok();
}

bool Process::restart_after_signal(u32 num) const {
  switch (num) {
    case 3: case 396:    // read
    case 4: case 397:    // write
    case 5: case 398:    // open
    case 7: case 400:    // wait4
    case 54:             // ioctl
    case 120: case 411:  // readv
    case 121: case 412:  // writev
    case 27: case 401:   // recvmsg
    case 28: case 402:   // sendmsg
    case 29: case 403:   // recvfrom
    case 30: case 404:   // accept
    case 98: case 409:   // connect
    case 133: case 405:  // sendto
      break;
    default:
      return false;
  }
  // The signal that will be delivered next decides.
  const u32 ready = g_arrived.load() & ~host_mask();
  if (!ready) return false;
  const GuestSigaction& sa = sigactions_[__builtin_ctz(ready) + 1];
  return sa.handler > 1 && (sa.flags & kRestart);
}

void Process::deliver_signals(Thread& t) {
  for (;;) {
    const u32 cur = host_mask();
    const u32 blocked = t.suspended ? t.suspend_mask : cur;
    t.suspended = false;
    const u32 ready = g_arrived.load() & ~blocked;
    if (!ready) return;
    const int sig = __builtin_ctz(ready) + 1;
    if (!(g_arrived.fetch_and(~bit(sig)) & bit(sig))) continue;  // another thread took it

    GuestSigaction sa;
    {
      std::lock_guard<std::mutex> lk(kmu_);
      sa = sigactions_[sig];
      if (sa.handler > 1 && (sa.flags & kResetHand)) {
        sigactions_[sig].handler = 0;
        signal(sig, SIG_DFL);
      }
    }
    if (sa.handler == 1) continue;  // ignored since it arrived
    if (sa.handler == 0) {
      // Back to the default since it arrived: let the host do the default.
      signal(sig, SIG_DFL);
      kill(getpid(), sig);
      continue;
    }
    if (opt_.strace) std::fprintf(stderr, "[signal %d -> handler 0x%08x]\n", sig, sa.handler);
    if (arch_ == Arch::PPC) {
      push_signal_frame_ppc(t, sig, sa.handler, sa.tramp, sa.flags, cur);
    } else {
      push_signal_frame_x86(t, sig, sa.handler, sa.tramp, sa.flags, cur);
    }
    if (exited_) return;
    // While the handler runs, its own signal and the ones it named are blocked.
    set_host_mask(cur | sa.mask | ((sa.flags & kNoDefer) ? 0 : bit(sig)));
  }
}

namespace {

// Where the frame goes: below the interrupted code's stack, or at the top of
// the alternate stack if the handler asked for that and it isn't in use.
u32 frame_stack(Thread& t, u32 sp, u32 flags) {
  if ((flags & kOnStack) && t.altstack_size && !t.on_altstack) {
    t.on_altstack = true;
    return t.altstack_sp + t.altstack_size;
  }
  return sp;
}

void write_siginfo(GuestMemory& mem, u32 at, int sig) {
  const siginfo_t& in = g_info[sig];
  memset(mem.host(at), 0, kSigInfoSize);
  mem.w32(at, (u32)sig);
  mem.w32(at + 4, (u32)in.si_errno);
  mem.w32(at + 8, (u32)in.si_code);
  mem.w32(at + 12, (u32)in.si_pid);
  mem.w32(at + 16, (u32)in.si_uid);
  mem.w32(at + 20, (u32)in.si_status);
  mem.w32(at + 28, (u32)(uintptr_t)in.si_value.sival_ptr);
  mem.w32(at + 32, (u32)in.si_band);
}

void write_ucontext(GuestMemory& mem, u32 at, bool was_on_stack, u32 mask, u32 sp, u32 mcontext,
                    u32 mcontext_size) {
  mem.w32(at, was_on_stack ? 1 : 0);
  mem.w32(at + 4, mask);
  mem.w32(at + 8, sp);
  mem.w32(at + 12, 0);
  mem.w32(at + 16, was_on_stack ? kStackInUse : 0);
  mem.w32(at + 20, 0);
  mem.w32(at + 24, mcontext_size);
  mem.w32(at + 28, mcontext);
}

}  // namespace

// Intel. From the new stack pointer upward: the frame the trampoline sees as
// its arguments (a return address that is never used, the handler, the style,
// the signal number, and pointers to the next two), the saved registers, the
// signal's details, and the context that ties them together.
void Process::push_signal_frame_x86(Thread& t, int sig, u32 handler, u32 tramp, u32 flags,
                                    u32 mask) {
  X86State& s = t.x86;
  const bool was_on_stack = t.on_altstack;
  const u32 top = frame_stack(t, s.reg[X86State::ESP], flags);
  const u32 uctx = top - kUcontextSize;
  const u32 info = uctx - kSigInfoSize;
  const u32 mctx = info - kX86McontextSize;
  const u32 frame = ((mctx - 24) & ~15u) - 4;  // the stack is 16-byte aligned before the "call"
  if (!mem_.range_ok(frame, top - frame)) {
    std::fprintf(stderr, "cocaine: no room on the guest stack to deliver signal %d\n", sig);
    exit_code_ = 128 + SIGSEGV;
    exited_ = true;
    kcv_.notify_all();
    return;
  }

  memset(mem_.host(mctx), 0, kX86McontextSize);
  const u32 regs = mctx + 12;
  static const int kOrder[8] = {X86State::EAX, X86State::EBX, X86State::ECX, X86State::EDX,
                                X86State::EDI, X86State::ESI, X86State::EBP, X86State::ESP};
  for (int i = 0; i < 8; i++) mem_.w32(regs + 4 * (u32)i, s.reg[kOrder[i]]);
  mem_.w32(regs + 32, s.seg[X86State::SS]);
  mem_.w32(regs + 36, s.eflags);
  mem_.w32(regs + 40, s.eip);
  mem_.w32(regs + 44, s.seg[X86State::CS]);
  mem_.w32(regs + 48, s.seg[X86State::DS]);
  mem_.w32(regs + 52, s.seg[X86State::ES]);
  mem_.w32(regs + 56, s.seg[X86State::FS]);
  mem_.w32(regs + 60, s.seg[X86State::GS]);
  x87_fxsave(s, mctx + 12 + 64 + 8);

  write_siginfo(mem_, info, sig);
  write_ucontext(mem_, uctx, was_on_stack, mask, frame, mctx, kX86McontextSize);

  mem_.w32(frame, 0xFFFFFFFFu);
  mem_.w32(frame + 4, handler);
  mem_.w32(frame + 8, (flags & kSigInfo) ? kStyleSigInfo : kStyleTraditional);
  mem_.w32(frame + 12, (u32)sig);
  mem_.w32(frame + 16, info);
  mem_.w32(frame + 20, uctx);

  s.reg[X86State::ESP] = frame;
  s.eip = tramp;
  s.eflags &= ~0x400u;  // string instructions go forward, as C code expects
}

// PowerPC. The handler's arguments go in registers, and below the saved state
// there is an ordinary call frame for the trampoline to use. The stack's red
// zone, which the interrupted code may be using, is left alone.
void Process::push_signal_frame_ppc(Thread& t, int sig, u32 handler, u32 tramp, u32 flags,
                                    u32 mask) {
  PPCState& s = t.ppc;
  const bool was_on_stack = t.on_altstack;
  const u32 top = frame_stack(t, s.gpr[1], flags);
  const u32 mctx = ((top - 224) & ~15u) - kPPCMcontextSize;
  const u32 uctx = mctx - kUcontextSize;
  const u32 info = uctx - kSigInfoSize;
  const u32 sp = ((info - 64) & ~15u) - 48;
  if (!mem_.range_ok(sp, top - sp)) {
    std::fprintf(stderr, "cocaine: no room on the guest stack to deliver signal %d\n", sig);
    exit_code_ = 128 + SIGSEGV;
    exited_ = true;
    kcv_.notify_all();
    return;
  }

  memset(mem_.host(sp), 0, mctx + kPPCMcontextSize - sp);
  const u32 regs = mctx + 32;
  mem_.w32(regs, s.pc);
  mem_.w32(regs + 4, 0x0200D032);  // the machine state register of ordinary user code
  for (u32 i = 0; i < 32; i++) mem_.w32(regs + 8 + 4 * i, s.gpr[i]);
  mem_.w32(regs + 136, s.cr);
  mem_.w32(regs + 140, s.xer);
  mem_.w32(regs + 144, s.lr);
  mem_.w32(regs + 148, s.ctr);
  mem_.w32(regs + 156, s.vrsave);
  const u32 fp = regs + 160;
  for (u32 i = 0; i < 32; i++) mem_.w64(fp + 8 * i, s.fpr[i].u);
  mem_.w32(fp + 260, s.fpscr);
  const u32 vec = fp + 264;
  for (u32 i = 0; i < 32; i++) {
    for (u32 b = 0; b < 16; b++) mem_.w8(vec + 16 * i + b, s.vr[i].b[15 - b]);
  }
  mem_.w32(vec + 512 + 12, s.vscr);
  mem_.w32(vec + 544, 0xFFFFFFFFu);  // every vector register holds something

  write_siginfo(mem_, info, sig);
  write_ucontext(mem_, uctx, was_on_stack, mask, sp, mctx, kPPCMcontextSize);

  s.gpr[1] = sp;
  s.gpr[3] = handler;
  s.gpr[4] = ((flags & kSigInfo) ? kStyleSigInfo : kStyleTraditional) + kStyleVectorExtra;
  s.gpr[5] = (u32)sig;
  s.gpr[6] = info;
  s.gpr[7] = uctx;
  s.pc = tramp;
  s.reserved = false;
}

// sigreturn: put back what the frame saved. The handler may have changed any
// of it, so it is read back from guest memory. longjmp also comes here, with
// no context, just to say which stack it has landed on.
SysRet Process::sys_sigreturn(Thread& t, u32 uctx, u32 style) {
  if (style == kSetAltStack || style == kResetAltStack) {
    t.on_altstack = style == kSetAltStack;
    return SysRet::ok();
  }
  if (!uctx) return SysRet::ok();
  if (!mem_.range_ok(uctx, kUcontextSize)) return SysRet::err(EFAULT);
  const u32 mctx = mem_.r32(uctx + 28), size = mem_.r32(uctx + 24);

  if (arch_ == Arch::I386) {
    if (!mem_.range_ok(mctx, kX86McontextSize)) return SysRet::err(EFAULT);
    X86State& s = t.x86;
    const u32 regs = mctx + 12;
    static const int kOrder[8] = {X86State::EAX, X86State::EBX, X86State::ECX, X86State::EDX,
                                  X86State::EDI, X86State::ESI, X86State::EBP, X86State::ESP};
    for (int i = 0; i < 8; i++) s.reg[kOrder[i]] = mem_.r32(regs + 4 * (u32)i);
    // Only the flags ordinary code can change itself.
    const u32 kUserFlags = 0x00000DD5u | 0x00200000u;
    s.eflags = (s.eflags & ~kUserFlags) | (mem_.r32(regs + 36) & kUserFlags);
    s.eip = mem_.r32(regs + 40);
    x87_fxrstor(s, mctx + 12 + 64 + 8);
    sse_apply_rounding(s.mxcsr);
  } else {
    const bool with_vectors = size >= kPPCMcontextSize || style == 6 || style == 35;
    if (!mem_.range_ok(mctx, with_vectors ? kPPCMcontextSize : kPPCMcontextSize - 576)) {
      return SysRet::err(EFAULT);
    }
    PPCState& s = t.ppc;
    const u32 regs = mctx + 32;
    s.pc = mem_.r32(regs);
    for (u32 i = 0; i < 32; i++) s.gpr[i] = mem_.r32(regs + 8 + 4 * i);
    s.cr = mem_.r32(regs + 136);
    s.xer = mem_.r32(regs + 140);
    s.lr = mem_.r32(regs + 144);
    s.ctr = mem_.r32(regs + 148);
    s.vrsave = mem_.r32(regs + 156);
    const u32 fp = regs + 160;
    for (u32 i = 0; i < 32; i++) s.fpr[i].u = mem_.r64(fp + 8 * i);
    s.fpscr = mem_.r32(fp + 260);
    if (with_vectors) {
      const u32 vec = fp + 264;
      for (u32 i = 0; i < 32; i++) {
        for (u32 b = 0; b < 16; b++) s.vr[i].b[15 - b] = mem_.r8(vec + 16 * i + b);
      }
      s.vscr = mem_.r32(vec + 512 + 12);
    }
    s.reserved = false;
  }
  t.on_altstack = mem_.r32(uctx) & 1;
  set_host_mask(mem_.r32(uctx + 4));
  return SysRet::none();  // every register is already what it should be
}
