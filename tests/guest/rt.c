#include "rt.h"

static char out_buf[4096];
static u32 out_len;

void rt_flush(void) {
  u32 off = 0;
  while (off < out_len) {
    const s32 n = sys_write(1, out_buf + off, out_len - off);
    if (n <= 0) break;
    off += (u32)n;
  }
  out_len = 0;
}

static void put_ch(char c) {
  if (out_len == sizeof out_buf) rt_flush();
  out_buf[out_len++] = c;
}

void put_str(const char* s) {
  while (*s) put_ch(*s++);
}

void put_hex32(u32 v) {
  static const char digits[] = "0123456789abcdef";
  for (int i = 28; i >= 0; i -= 4) put_ch(digits[(v >> i) & 15]);
}

void put_hex64(u64 v) {
  put_hex32((u32)(v >> 32));
  put_hex32((u32)v);
}

void put_dec(s32 v) {
  char tmp[12];
  int n = 0;
  u32 u = v < 0 ? 0u - (u32)v : (u32)v;
  if (v < 0) put_ch('-');
  do {
    tmp[n++] = (char)('0' + u % 10);
    u /= 10;
  } while (u);
  while (n) put_ch(tmp[--n]);
}

void put_kv(const char* key, u32 v) {
  put_str(key);
  put_str(" = 0x");
  put_hex32(v);
  put_ch('\n');
}

void put_kv64(const char* key, u64 v) {
  put_str(key);
  put_str(" = 0x");
  put_hex64(v);
  put_ch('\n');
}

NOINLINE u32 mix(u32 h, u32 v) {
  h ^= v;
  h *= 0x01000193u;
  return (h << 5) | (h >> 27);
}

NOINLINE u32 mix64(u32 h, u64 v) { return mix(mix(h, (u32)v), (u32)(v >> 32)); }

/* Called by each platform's startup code. The environment follows argv. */
void rt_start(int argc, char** argv) {
  const int rc = test_main(argc, argv, argv + argc + 1);
  rt_flush();
  sys_exit(rc);
}

#ifndef RT_HOST

/* Compilers call these even in freestanding code (struct copies, array
 * initialisers), and the guests have no C library to supply them. */
void* memcpy(void* dst, const void* src, size_t n) {
  u8* d = dst;
  const u8* s = src;
  while (n--) *d++ = *s++;
  return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
  u8* d = dst;
  const u8* s = src;
  if (d < s) {
    while (n--) *d++ = *s++;
  } else {
    while (n--) d[n] = s[n];
  }
  return dst;
}

void* memset(void* dst, int c, size_t n) {
  u8* d = dst;
  while (n--) *d++ = (u8)c;
  return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
  const u8* x = a;
  const u8* y = b;
  for (size_t i = 0; i < n; i++) {
    if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
  }
  return 0;
}

/* 64-bit division. 32-bit compilers call out to these instead of open-coding
 * it, and here there is no compiler support library to link. */
u64 __udivmoddi4(u64 n, u64 d, u64* rem) {
  u64 q = 0, r = 0;
  for (int i = 63; i >= 0; i--) {
    r = (r << 1) | ((n >> i) & 1);
    if (r >= d) {
      r -= d;
      q |= (u64)1 << i;
    }
  }
  if (rem) *rem = r;
  return q;
}

u64 __udivdi3(u64 n, u64 d) { return __udivmoddi4(n, d, 0); }

u64 __umoddi3(u64 n, u64 d) {
  u64 r;
  __udivmoddi4(n, d, &r);
  return r;
}

s64 __divdi3(s64 n, s64 d) {
  const u64 un = n < 0 ? 0 - (u64)n : (u64)n;
  const u64 ud = d < 0 ? 0 - (u64)d : (u64)d;
  const u64 q = __udivmoddi4(un, ud, 0);
  return (s64)(((n < 0) != (d < 0)) ? 0 - q : q);
}

s64 __moddi3(s64 n, s64 d) {
  const u64 un = n < 0 ? 0 - (u64)n : (u64)n;
  const u64 ud = d < 0 ? 0 - (u64)d : (u64)d;
  u64 r;
  __udivmoddi4(un, ud, &r);
  return (s64)(n < 0 ? 0 - r : r);
}

#endif /* !RT_HOST */
