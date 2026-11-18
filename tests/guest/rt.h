/* Tiny runtime shared by cocaine's guest test programs.
 *
 * The same test source is built three ways: as a PowerPC guest, as a 32-bit
 * Intel guest, and natively for the host. The native build's output is the
 * reference the guest builds must match, so tests stick to fixed-width types
 * and print results rather than judging them. */
#ifndef COCAINE_TEST_RT_H
#define COCAINE_TEST_RT_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#define NOINLINE __attribute__((noinline))

/* Raw system calls, one implementation per platform. sys_write returns a
 * negative number on failure. */
s32 sys_write(s32 fd, const void* buf, u32 n);
void sys_exit(s32 code) __attribute__((noreturn));

/* Buffered output to stdout. */
void put_str(const char* s);
void put_hex32(u32 v);
void put_hex64(u64 v);
void put_dec(s32 v);
void put_kv(const char* key, u32 v);   /* prints "key = 0x0000002a" */
void put_kv64(const char* key, u64 v);
void rt_flush(void);

/* Folds a value into a running hash, so a whole table of results prints as
 * one line. */
u32 mix(u32 h, u32 v);
u32 mix64(u32 h, u64 v);

/* Each test program defines this instead of main. */
int test_main(int argc, char** argv, char** envp);

#endif
