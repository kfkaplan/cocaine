// A maths workout for the Intel translator: doubles, singles, conversions,
// arrays the compiler turns into packed SSE, and a little x87.
#include "rt.h"

#define N 4096
static float fa[N], fb[N], fc[N];
static double da[N], db[N];
static volatile int rounds = 3000;

static double mandel(void) {
  double total = 0;
  for (int again = 0; again < rounds / 50; again++)
  for (int y = 0; y < 120; y++) {
    for (int x = 0; x < 160; x++) {
      const double cr = -2.0 + x * (3.0 / 160), ci = -1.2 + y * (2.4 / 120);
      double zr = 0, zi = 0;
      int k = 0;
      while (k < 200 && zr * zr + zi * zi < 4.0) {
        const double t = zr * zr - zi * zi + cr;
        zi = 2 * zr * zi + ci;
        zr = t;
        k++;
      }
      total += k;
    }
  }
  return total;
}

static float singles(void) {
  float acc = 0;
  for (int r = 0; r < rounds; r++) {
    for (int i = 0; i < N; i++) fc[i] = fa[i] * fb[i] + fc[i] * 0.5f;
    for (int i = 0; i < N; i++) {
      float v = fc[i];
      if (v > 100.0f) v = 100.0f;
      if (v < -100.0f) v = -100.0f;
      acc += v;
    }
  }
  return acc;
}

static double doubles(void) {
  double acc = 0;
  for (int r = 0; r < rounds; r++) {
    for (int i = 0; i < N; i++) da[i] = da[i] * 0.999 + db[i] / (1.0 + i);
    for (int i = 0; i < N; i++) acc += __builtin_sqrt(da[i] * da[i] + 1.0);
  }
  return acc;
}

static u32 convert(void) {
  u32 h = 0;
  for (int r = 0; r < rounds; r++) {
    for (int i = 0; i < N; i++) {
      const int a = (int)(fa[i] * 37.5f);
      const double d = (double)fb[i] * a;
      const float f = (float)(d * 0.25);
      h = h * 31 + (u32)(int)f + (u32)a;
    }
  }
  return h;
}

static u32 extended(void) {
  volatile long double x = 1.0L;
  long double acc = 0;
  for (int i = 1; i < rounds * 1000; i++) {
    acc += x / i;
    acc = acc * 0.999L + 1;
  }
  return (u32)(long long)(acc * 1000);
}

int test_main(int argc, char** argv, char** envp) {
  (void)envp;
  for (int i = 0; i < N; i++) {
    fa[i] = (float)(i % 97) * 0.25f - 3.0f;
    fb[i] = (float)(i % 31) * 0.5f + 0.125f;
    fc[i] = 1.0f;
    da[i] = i * 0.001;
    db[i] = (i % 113) * 0.75;
  }
  const char which = argc > 1 ? argv[1][0] : 'a';
  if (which == 'a' || which == 'm') put_kv("mandel", (u32)mandel());
  if (which == 'a' || which == 's') put_kv("singles", (u32)(int)singles());
  if (which == 'a' || which == 'd') put_kv("doubles", (u32)(long long)doubles());
  if (which == 'a' || which == 'c') put_kv("convert", convert());
  if (which == 'a' || which == 'e') put_kv("extended", extended());
  return 0;
}
