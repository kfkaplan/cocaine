/* Signals: handlers run, the interrupted code carries on undisturbed, blocking
 * works, and a timer's signals arrive while the program is busy.
 *
 * The guest builds have no C library, so they make the system calls
 * themselves and supply the trampoline the kernel calls: the routine that
 * calls the handler and then asks for the interrupted registers back. */
#include "rt.h"

#define SIG_USR1 30
#define SIG_ALRM 14

static volatile u32 ticks, last_sig, info_sig, scratch;

/* Busywork that uses plenty of registers, integer and floating point. If a
 * handler's use of them leaked into the code it interrupted, the hashes
 * printed at the end would come out wrong. */
static NOINLINE u32 churn(u32 x) {
  double d = (double)x;
  float f = 1.5f;
  u64 q = x;
  for (int i = 0; i < 40; i++) {
    x = mix(x, (u32)i);
    d = d * 1.25 + (double)(x & 15);
    f = f * 0.5f + (float)(x & 3);
    q = q * 6364136223846793005ull + x;
  }
  return x ^ (u32)(s32)d ^ (u32)(s32)f ^ (u32)q ^ (u32)(q >> 32);
}

static void on_signal(int sig, void* info, void* context) {
  (void)context;
  last_sig = (u32)sig;
  info_sig = *(u32*)info; /* the signal number is the first field */
  ticks++;
  scratch = churn(ticks);
}

#ifdef RT_HOST

#include <signal.h>
#include <sys/time.h>
#include <unistd.h>

static void host_handler(int sig, siginfo_t* info, void* context) { on_signal(sig, info, context); }

static void install(int sig) {
  struct sigaction sa;
  sa.sa_sigaction = host_handler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(sig, &sa, 0);
}
static void send_self(int sig) { kill(getpid(), sig); }
static void set_blocked(int sig, int blocked) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, sig);
  sigprocmask(blocked ? SIG_BLOCK : SIG_UNBLOCK, &set, 0);
}
static void timer(u32 usec) {
  struct itimerval tv = {{0, (int)usec}, {0, (int)usec}};
  setitimer(ITIMER_REAL, &tv, 0);
}

#else

/* A raw BSD system call; a negative result is an error number. */
s32 sys_call(u32 number, u32 a, u32 b, u32 c);

static void trampoline(void (*handler)(int, void*, void*), int style, int sig, void* info,
                       void* context) {
  handler(sig, info, context);
  sys_call(184, (u32)context, (u32)style, 0); /* sigreturn */
}

static void install(int sig) {
  /* handler, trampoline, signals to block meanwhile, flags (SA_SIGINFO) */
  u32 action[4] = {(u32)on_signal, (u32)trampoline, 0, 0x40};
  sys_call(46, (u32)sig, (u32)action, 0);
}
static void send_self(int sig) { sys_call(37, (u32)sys_call(20, 0, 0, 0), (u32)sig, 1); }
static void set_blocked(int sig, int blocked) {
  u32 set = 1u << (sig - 1);
  sys_call(48, blocked ? 1 : 2, (u32)&set, 0);
}
static void timer(u32 usec) {
  u32 tv[4] = {0, usec, 0, usec};
  sys_call(83, 0, (u32)tv, 0);
}

#endif

int test_main(int argc, char** argv, char** envp) {
  (void)argc, (void)argv, (void)envp;
  install(SIG_USR1);
  install(SIG_ALRM);

  for (int i = 0; i < 3; i++) send_self(SIG_USR1);
  put_kv("handler calls after three signals", ticks);
  put_kv("signal number", last_sig);
  put_kv("signal number in its details", info_sig);

  set_blocked(SIG_USR1, 1);
  send_self(SIG_USR1);
  put_kv("calls while blocked", ticks);
  set_blocked(SIG_USR1, 0);
  put_kv("calls once unblocked", ticks);

  /* A fixed amount of work, interrupted by the timer however many times. */
  ticks = 0;
  timer(1000);
  u32 h = 1;
  double d = 0.5;
  float f = 0.25f;
  u64 q = 7;
  for (u32 i = 0; i < 3000000; i++) {
    h = mix(h, i);
    d = d * 0.999 + (double)(h & 255);
    f = f * 0.75f + (float)(h & 31);
    q = q * 6364136223846793005ull + h;
  }
  while (ticks == 0) {
  } /* a fast machine can finish before the first tick */
  timer(0);
  u64 d_bits;
  u32 f_bits;
  __builtin_memcpy(&d_bits, &d, 8);
  __builtin_memcpy(&f_bits, &f, 4);
  put_kv("work: integer hash", h);
  put_kv64("work: double", d_bits);
  put_kv("work: float", f_bits);
  put_kv64("work: 64-bit", q);
  put_str("the timer fired\n");
  return 0;
}
