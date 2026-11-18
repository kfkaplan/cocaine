#pragma once
// The host's floating point rounding mode, set only when it changes.
//
// Setting it writes a control register, which on Apple Silicon is slow, and
// the CPU cores switch between the guest's mode and the host's default on
// every trip in and out of the interpreter. Each thread remembers what it
// last set, so that a switch to the mode already in force costs nothing.
// Everything in cocaine that changes the rounding mode goes through here.

#include <cfenv>

inline thread_local int t_host_rounding = -1;

inline void host_rounding(int mode) {
  if (t_host_rounding == -1) t_host_rounding = fegetround();
  if (t_host_rounding == mode) return;
  fesetround(mode);
  t_host_rounding = mode;
}
// Set by the translators around a single interpreted instruction: the
// interpreter then leaves the guest's mode in force when it returns, since
// translated code wants that mode straight back.
inline thread_local bool t_keep_guest_rounding = false;

inline int host_rounding_now() {
  if (t_host_rounding == -1) t_host_rounding = fegetround();
  return t_host_rounding;
}
