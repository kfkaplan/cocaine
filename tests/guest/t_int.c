/* Integer torture test for the CPU cores.
 *
 * Every group computes a pile of results from values the compiler can't see
 * through (volatile tables, a seeded generator) and prints one hash per
 * operation family. The emulated output must equal the native output. */
#include "rt.h"

static volatile u32 g_seed = 0x2545F491u;
static u32 rng;

static u32 rnd(void) {
  u32 x = rng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rng = x;
  return x;
}

static const volatile u32 edge32[] = {
    0,          1,          2,          3,          0x7F,       0x80,       0xFF,
    0x100,      0x7FFF,     0x8000,     0xFFFF,     0x10000,    0x7FFFFFFF, 0x80000000u,
    0x80000001u, 0xFFFFFFFEu, 0xFFFFFFFFu, 0x12345678, 0xDEADBEEFu, 0x00FF00FF, 1000000007u,
};
#define N32 (sizeof edge32 / sizeof edge32[0])

static const volatile u64 edge64[] = {
    0,
    1,
    2,
    0xFF,
    0xFFFF,
    0xFFFFFFFFull,
    0x100000000ull,
    0x1FFFFFFFFull,
    0x7FFFFFFFFFFFFFFFull,
    0x8000000000000000ull,
    0x8000000000000001ull,
    0xFFFFFFFF00000000ull,
    0xFFFFFFFFFFFFFFFFull,
    0x0123456789ABCDEFull,
    0xFEDCBA9876543210ull,
    1000000007ull,
    0x00000000FFFFFFFEull,
};
#define N64 (sizeof edge64 / sizeof edge64[0])

/* ---- 32-bit arithmetic, logic and comparisons ------------------------------ */

static void test_arith32(void) {
  u32 h_add = 1, h_mul = 2, h_div = 3, h_logic = 4, h_cmp = 5;
  for (u32 i = 0; i < N32; i++) {
    for (u32 j = 0; j < N32; j++) {
      const u32 a = edge32[i], b = edge32[j];
      const s32 sa = (s32)a, sb = (s32)b;
      h_add = mix(h_add, a + b);
      h_add = mix(h_add, a - b);
      h_add = mix(h_add, 0u - a);
      h_add = mix(h_add, a + b + 1);
      h_mul = mix(h_mul, a * b);
      h_mul = mix(h_mul, (u32)(((u64)a * b) >> 32));
      h_mul = mix(h_mul, (u32)(((s64)sa * sb) >> 32));
      h_mul = mix(h_mul, a * 10 + b * 100003u);
      if (b) {
        h_div = mix(h_div, a / b);
        h_div = mix(h_div, a % b);
      }
      if (b && !(sa == INT32_MIN && sb == -1)) {
        h_div = mix(h_div, (u32)(sa / sb));
        h_div = mix(h_div, (u32)(sa % sb));
      }
      h_div = mix(h_div, a / 10);
      h_div = mix(h_div, (u32)(sa / 7));
      h_div = mix(h_div, (u32)(sa % 1000));
      h_logic = mix(h_logic, a & b);
      h_logic = mix(h_logic, a | b);
      h_logic = mix(h_logic, a ^ b);
      h_logic = mix(h_logic, ~a);
      h_logic = mix(h_logic, a & ~b);
      h_logic = mix(h_logic, ~(a | b));
      h_logic = mix(h_logic, ~(a ^ b));
      h_cmp = mix(h_cmp, (u32)(a < b) | (u32)(a <= b) << 1 | (u32)(a == b) << 2 |
                             (u32)(sa < sb) << 3 | (u32)(sa <= sb) << 4 | (u32)(a > b) << 5 |
                             (u32)(sa > sb) << 6 | (u32)(a != b) << 7 | (u32)(sa >= sb) << 8);
      h_cmp = mix(h_cmp, sa < sb ? a : b);
      h_cmp = mix(h_cmp, a > b ? a - b : b - a);
    }
  }
  put_kv("arith32.add", h_add);
  put_kv("arith32.mul", h_mul);
  put_kv("arith32.div", h_div);
  put_kv("arith32.logic", h_logic);
  put_kv("arith32.cmp", h_cmp);
}

/* ---- shifts and rotates ----------------------------------------------------- */

static void test_shifts(void) {
  u32 h_shl = 1, h_shr = 2, h_sar = 3, h_rot = 4, h_const = 5;
  for (u32 i = 0; i < N32; i++) {
    const u32 a = edge32[i];
    for (volatile u32 vn = 0; vn < 32; vn++) {
      const u32 n = vn;
      h_shl = mix(h_shl, a << n);
      h_shr = mix(h_shr, a >> n);
      h_sar = mix(h_sar, (u32)((s32)a >> n));
      h_rot = mix(h_rot, (a << n) | (a >> ((32 - n) & 31)));
      h_rot = mix(h_rot, (a >> n) | (a << ((32 - n) & 31)));
    }
    h_const = mix(h_const, (a << 3) | (a >> 29));
    h_const = mix(h_const, (a >> 8) & 0xFF);
    h_const = mix(h_const, (u32)((s32)a >> 31));
    h_const = mix(h_const, (u32)((s32)a >> 5) + (a << 17));
    h_const = mix(h_const, (u32)(u16)(a >> 4) | (u32)(u8)a << 24);
    h_const = mix(h_const, (u32)(s32)(s8)a + (u32)(s32)(s16)a);
  }
  put_kv("shift.shl", h_shl);
  put_kv("shift.shr", h_shr);
  put_kv("shift.sar", h_sar);
  put_kv("shift.rot", h_rot);
  put_kv("shift.const", h_const);
}

/* ---- 64-bit arithmetic on a 32-bit CPU -------------------------------------- */

static void test_arith64(void) {
  u32 h_add = 1, h_mul = 2, h_div = 3, h_shift = 4, h_cmp = 5;
  for (u32 i = 0; i < N64; i++) {
    for (u32 j = 0; j < N64; j++) {
      const u64 a = edge64[i], b = edge64[j];
      const s64 sa = (s64)a, sb = (s64)b;
      const u32 n = (u32)b & 63;
      h_add = mix64(h_add, a + b);
      h_add = mix64(h_add, a - b);
      h_add = mix64(h_add, 0 - a);
      h_add = mix64(h_add, a + 1);
      h_add = mix64(h_add, a - 1);
      h_mul = mix64(h_mul, a * b);
      h_mul = mix64(h_mul, a * 1000003ull);
      if (b) {
        h_div = mix64(h_div, a / b);
        h_div = mix64(h_div, a % b);
      }
      if (b && !(sa == INT64_MIN && sb == -1)) {
        h_div = mix64(h_div, (u64)(sa / sb));
        h_div = mix64(h_div, (u64)(sa % sb));
      }
      h_shift = mix64(h_shift, a << n);
      h_shift = mix64(h_shift, a >> n);
      h_shift = mix64(h_shift, (u64)(sa >> n));
      h_shift = mix64(h_shift, a << 1);
      h_shift = mix64(h_shift, a >> 33);
      h_cmp = mix(h_cmp, (u32)(a < b) | (u32)(a <= b) << 1 | (u32)(a == b) << 2 |
                             (u32)(sa < sb) << 3 | (u32)(sa <= sb) << 4 | (u32)(a > b) << 5 |
                             (u32)(sa > sb) << 6);
      h_cmp = mix64(h_cmp, sa < sb ? a : b);
      h_cmp = mix64(h_cmp, (a & b) | (a ^ ~b));
    }
  }
  put_kv("arith64.add", h_add);
  put_kv("arith64.mul", h_mul);
  put_kv("arith64.div", h_div);
  put_kv("arith64.shift", h_shift);
  put_kv("arith64.cmp", h_cmp);
}

/* ---- bit counting and byte swapping ----------------------------------------- */

static u32 popcount_slow(u32 v) {
  u32 n = 0;
  while (v) {
    n += v & 1;
    v >>= 1;
  }
  return n;
}

static void test_bits(void) {
  u32 h_count = 1, h_swap = 2;
  rng = g_seed;
  for (int k = 0; k < 400; k++) {
    /* One rnd() per statement: C leaves the order within an expression open,
     * and the builds must agree. */
    const u32 base = rnd();
    const u32 down = rnd() & 31;
    const u32 v = (base >> down) | (1u << (rnd() & 31));
    h_count = mix(h_count, (u32)__builtin_clz(v));
    h_count = mix(h_count, (u32)__builtin_ctz(v));
    h_count = mix(h_count, popcount_slow(v));
    h_swap = mix(h_swap, __builtin_bswap32(v));
    h_swap = mix(h_swap, __builtin_bswap16((u16)v));
    h_swap = mix64(h_swap, __builtin_bswap64(((u64)v << 32) | rnd()));
  }
  put_kv("bits.count", h_count);
  put_kv("bits.swap", h_swap);
}

/* ---- memory: widths, sign extension, structs, block moves ------------------- */

struct record {
  u32 id;
  u16 kind;
  u8 flags;
  s8 delta;
  u64 stamp;
  char name[20];
};

static u8 bytes[1024];
static s16 halves[256];
static struct record records[32];

static NOINLINE struct record make_record(u32 i) {
  struct record r;
  r.id = i * 2654435761u;
  r.kind = (u16)(i * 7);
  r.flags = (u8)(i ^ 0x5A);
  r.delta = (s8)(i * 13);
  r.stamp = (u64)i * 0x100000001B3ull;
  for (u32 k = 0; k < sizeof r.name; k++) r.name[k] = (char)('a' + (i + k) % 26);
  return r;
}

static void test_memory(void) {
  rng = g_seed ^ 0xA5A5A5A5u;
  for (u32 i = 0; i < sizeof bytes; i++) bytes[i] = (u8)rnd();
  for (u32 i = 0; i < 256; i++) halves[i] = (s16)rnd();

  u32 h_load = 1;
  for (u32 i = 0; i < sizeof bytes; i++) {
    h_load = mix(h_load, bytes[i]);
    h_load = mix(h_load, (u32)(s32)(s8)bytes[i]);
  }
  for (u32 i = 0; i < 256; i++) {
    h_load = mix(h_load, (u32)(s32)halves[i]);
    h_load = mix(h_load, (u16)halves[i]);
  }
  /* Unaligned 16-, 32- and 64-bit accesses. */
  for (u32 i = 0; i + 8 <= 200; i++) {
    u16 a;
    u32 b;
    u64 c;
    __builtin_memcpy(&a, bytes + i, 2);
    __builtin_memcpy(&b, bytes + i, 4);
    __builtin_memcpy(&c, bytes + i, 8);
    /* Fold in a byte-order-independent way so all builds agree. */
    h_load = mix(h_load, (u32)(a & 0xFF) + (u32)(a >> 8));
    h_load = mix(h_load, (b & 0xFF) + ((b >> 8) & 0xFF) + ((b >> 16) & 0xFF) + (b >> 24));
    h_load = mix(h_load, (u32)(c ^ (c >> 32)) == 0 ? 1 : 2);
    b ^= 0x5A5A5A5Au;  /* the same bytes whichever end comes first */
    __builtin_memcpy(bytes + 512 + i, &b, 4);
  }
  put_kv("mem.load", h_load);

  /* Byte order is the one thing the builds may disagree on, so check it
   * against what each compiler believes instead of printing it. */
  volatile u32 word = 0x11223344u;
  const u8 first = *(volatile u8*)&word;
#if defined(__BIG_ENDIAN__) || defined(__PPC__) || defined(__powerpc__)
  put_str(first == 0x11 ? "mem.endian ok\n" : "mem.endian WRONG\n");
#else
  put_str(first == 0x44 ? "mem.endian ok\n" : "mem.endian WRONG\n");
#endif

  u32 h_struct = 1;
  for (u32 i = 0; i < 32; i++) records[i] = make_record(i);
  for (u32 i = 0; i < 16; i++) {
    const struct record tmp = records[i];
    records[i] = records[31 - i];
    records[31 - i] = tmp;
  }
  for (u32 i = 0; i < 32; i++) {
    const struct record* r = &records[i];
    h_struct = mix(h_struct, r->id);
    h_struct = mix(h_struct, r->kind);
    h_struct = mix(h_struct, r->flags);
    h_struct = mix(h_struct, (u32)(s32)r->delta);
    h_struct = mix64(h_struct, r->stamp);
    for (u32 k = 0; k < sizeof r->name; k++) h_struct = mix(h_struct, (u8)r->name[k]);
  }
  put_kv("mem.struct", h_struct);

  u32 h_block = 1;
  __builtin_memmove(bytes + 10, bytes, 300);  /* overlapping, moving up */
  __builtin_memmove(bytes + 400, bytes + 417, 300);  /* overlapping, moving down */
  __builtin_memset(bytes + 900, 0xC3, 77);
  __builtin_memcpy(bytes + 700, bytes + 100, 129);
  for (u32 i = 0; i < sizeof bytes; i++) h_block = mix(h_block, bytes[i]);
  h_block = mix(h_block, (u32)__builtin_memcmp(bytes + 700, bytes + 100, 129));
  h_block = mix(h_block, __builtin_memcmp(bytes, bytes + 1, 64) < 0 ? 1u : 2u);
  put_kv("mem.block", h_block);
}

/* ---- classic algorithms ------------------------------------------------------ */

static u32 crc_table[256];

static void test_crc(void) {
  for (u32 i = 0; i < 256; i++) {
    u32 c = i;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crc_table[i] = c;
  }
  rng = g_seed + 99;
  u32 crc = 0xFFFFFFFFu;
  u32 a = 1, b = 0;
  for (u32 i = 0; i < 20000; i++) {
    const u8 byte = (u8)(rnd() >> 11);
    crc = crc_table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  put_kv("algo.crc32", ~crc);
  put_kv("algo.adler32", (b << 16) | a);
}

static u32 sort_data[1500];

static void quicksort(u32* v, s32 lo, s32 hi) {
  while (lo < hi) {
    const u32 pivot = v[(lo + hi) / 2];
    s32 i = lo, j = hi;
    while (i <= j) {
      while (v[i] < pivot) i++;
      while (v[j] > pivot) j--;
      if (i <= j) {
        const u32 t = v[i];
        v[i] = v[j];
        v[j] = t;
        i++;
        j--;
      }
    }
    if (j - lo < hi - i) {
      quicksort(v, lo, j);
      lo = i;
    } else {
      quicksort(v, i, hi);
      hi = j;
    }
  }
}

static void test_sort(void) {
  rng = g_seed * 3;
  for (u32 i = 0; i < 1500; i++) sort_data[i] = rnd() % 100000;
  quicksort(sort_data, 0, 1499);
  u32 h = 1, sorted = 1;
  for (u32 i = 0; i < 1500; i++) {
    h = mix(h, sort_data[i]);
    if (i && sort_data[i - 1] > sort_data[i]) sorted = 0;
  }
  put_kv("algo.sort", h);
  put_str(sorted ? "algo.sorted ok\n" : "algo.sorted WRONG\n");

  /* Insertion sort on signed 16-bit values. */
  for (u32 i = 0; i < 200; i++) halves[i] = (s16)rnd();
  for (s32 i = 1; i < 200; i++) {
    const s16 key = halves[i];
    s32 j = i - 1;
    while (j >= 0 && halves[j] > key) {
      halves[j + 1] = halves[j];
      j--;
    }
    halves[j + 1] = key;
  }
  h = 1;
  for (u32 i = 0; i < 200; i++) h = mix(h, (u32)(s32)halves[i]);
  put_kv("algo.sort16", h);
}

/* ---- calls: recursion, pointers, jump tables, varargs, wide arguments ------- */

static NOINLINE u32 fib(u32 n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

static u32 op_add(u32 a, u32 b) { return a + b; }
static u32 op_sub(u32 a, u32 b) { return a - b; }
static u32 op_mul(u32 a, u32 b) { return a * b; }
static u32 op_xor(u32 a, u32 b) { return a ^ b; }
static u32 op_rot(u32 a, u32 b) { return (a << (b & 31)) | (a >> ((32 - b) & 31)); }
static u32 op_min(u32 a, u32 b) { return a < b ? a : b; }
static u32 op_and(u32 a, u32 b) { return a & ~b; }
static u32 op_mad(u32 a, u32 b) { return a * 31 + b; }

static u32 (*const volatile ops[8])(u32, u32) = {op_add, op_sub, op_mul, op_xor,
                                                 op_rot, op_min, op_and, op_mad};

static NOINLINE u32 dense_switch(u32 k, u32 v) {
  switch (k) {
    case 0: return v + 1;
    case 1: return v * 3;
    case 2: return v ^ 0x55555555u;
    case 3: return v >> 3;
    case 4: return v << 5;
    case 5: return ~v;
    case 6: return v - 77;
    case 7: return v | 0x80000001u;
    case 8: return v & 0x0FFFFFF0u;
    case 9: return (v << 16) | (v >> 16);
    case 10: return v * v;
    case 11: return v / 3;
    default: return 0xDEAD;
  }
}

static NOINLINE u32 sparse_switch(u32 k) {
  switch (k) {
    case 3: return 11;
    case 70: return 22;
    case 500: return 33;
    case 9000: return 44;
    case 0x10000: return 55;
    case 0x7FFFFFFF: return 66;
    default: return 77;
  }
}

static NOINLINE u32 sum_ints(int n, ...) {
  va_list ap;
  va_start(ap, n);
  u32 total = 0;
  for (int i = 0; i < n; i++) total = total * 3 + va_arg(ap, u32);
  va_end(ap);
  return total;
}

static NOINLINE u64 sum_mixed(int n, ...) {
  va_list ap;
  va_start(ap, n);
  u64 total = 0;
  for (int i = 0; i < n; i++) {
    total += va_arg(ap, u32);
    total ^= va_arg(ap, u64) << (i & 7);
  }
  va_end(ap);
  return total;
}

static NOINLINE u32 many_args(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g, u32 h, u32 i,
                              u32 j, u32 k, u32 l) {
  return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i + 10 * j + 11 * k +
         12 * l;
}

static NOINLINE u64 wide_args(u32 a, u64 b, u32 c, u64 d, s64 e) {
  return (b + a) * (d - c) + (u64)(e >> 3);
}

struct pair {
  u32 lo, hi;
};

static NOINLINE struct pair split(u64 v) {
  struct pair p = {(u32)v, (u32)(v >> 32)};
  return p;
}

static void test_calls(void) {
  put_kv("call.fib", fib(21));

  rng = g_seed ^ 0x1234;
  u32 h = 1;
  for (int i = 0; i < 500; i++) {
    const u32 a = rnd(), b = rnd();
    h = mix(h, ops[a & 7](a, b));
    h = mix(h, dense_switch(b % 13, a));
    h = mix(h, sparse_switch(i % 5 == 0 ? 70 : (i % 7 == 0 ? 0x10000 : a & 0x1FF)));
  }
  h = mix(h, sparse_switch(3) + sparse_switch(500) + sparse_switch(9000) +
                 sparse_switch(0x7FFFFFFF));
  put_kv("call.dispatch", h);

  u32 r[5];
  for (int i = 0; i < 5; i++) r[i] = rnd();
  put_kv("call.varargs", sum_ints(7, r[0], 2u, 3u, r[1], 5u, 6u, r[2]));
  put_kv64("call.varargs64",
           sum_mixed(3, 1u, 0x1122334455667788ull, r[3], 0xFFFFFFFF00000001ull, 3u,
                     (u64)r[4] << 20));
  put_kv("call.many", many_args(r[0], 2, r[1], 4, 5, r[2], 7, 8, r[3], 10, 11, r[4]));
  put_kv64("call.wide",
           wide_args(r[1], 0x123456789ull * r[2], 17, (u64)r[3] << 31, -123456789012345ll));
  const struct pair p = split(0xCAFEF00DDEADBEEFull + rnd());
  put_kv("call.struct", p.lo ^ (p.hi * 3));
}

/* ---- multi-word arithmetic: carries rippling through 32-bit limbs ----------- */

static void test_carry(void) {
  u32 x[8], y[8], sum[8], diff[8];
  rng = g_seed ^ 0xBEEF;
  u32 h = 1;
  for (int round = 0; round < 50; round++) {
    for (int i = 0; i < 8; i++) {
      x[i] = (round & 1) ? rnd() : 0xFFFFFFFFu - (rnd() & 3);
      y[i] = (round & 2) ? rnd() : (rnd() & 7);
    }
    u32 carry = 0, borrow = 0;
    for (int i = 0; i < 8; i++) {
      const u64 s = (u64)x[i] + y[i] + carry;
      sum[i] = (u32)s;
      carry = (u32)(s >> 32);
      const u64 d = (u64)x[i] - y[i] - borrow;
      diff[i] = (u32)d;
      borrow = (u32)(d >> 63);
    }
    for (int i = 0; i < 8; i++) h = mix(mix(h, sum[i]), diff[i]);
    h = mix(h, carry * 2 + borrow);
  }
  put_kv("carry.limbs", h);
}

int test_main(int argc, char** argv, char** envp) {
  (void)argc;
  (void)argv;
  (void)envp;
  test_arith32();
  test_shifts();
  test_arith64();
  test_bits();
  test_memory();
  test_crc();
  test_sort();
  test_calls();
  test_carry();
  put_str("done\n");
  return 0;
}
