#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

// The guest CPU architectures cocaine can run.
enum class Arch { PPC, I386 };

inline const char* arch_name(Arch a) { return a == Arch::PPC ? "ppc" : "i386"; }

// Logging goes to stderr with a "cocaine:" prefix so it never mixes with the
// guest program's own output on stdout.
extern int g_verbosity;  // 0 = warnings only, 1 = info, 2 = debug

void log_warn(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log_info(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log_debug(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
