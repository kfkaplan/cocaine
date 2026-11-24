#include "kernel/display_pixels.h"

#include <cstring>
#include <vector>

static bool test_rgb555() {
  constexpr u32 count = 65536;
  for (bool big_endian : {false, true}) {
    std::vector<u8> src(count * 2 + 1), expected(count * 4);
    for (u32 pixel = 0; pixel < count; pixel++) {
      src[1 + pixel * 2 + (big_endian ? 0 : 1)] = (u8)(pixel >> 8);
      src[1 + pixel * 2 + (big_endian ? 1 : 0)] = (u8)pixel;
      const u8 r = (u8)(((pixel >> 10) & 31) * 255 / 31);
      const u8 g = (u8)(((pixel >> 5) & 31) * 255 / 31);
      const u8 b = (u8)((pixel & 31) * 255 / 31);
      expected[pixel * 4 + (big_endian ? 1 : 2)] = r;
      expected[pixel * 4 + (big_endian ? 2 : 1)] = g;
      expected[pixel * 4 + (big_endian ? 3 : 0)] = b;
    }
    for (u32 width : {0u, 1u, 7u, 15u, 16u, 17u, 31u, 32u, 33u, 127u, count}) {
      std::vector<u8> dst(width * 4 + 2, 0x5a);
      rgb555_to_display(src.data() + 1, dst.data() + 1, width, big_endian);
      if (dst.front() != 0x5a || dst.back() != 0x5a ||
          memcmp(dst.data() + 1, expected.data(), width * 4)) return false;
    }
    std::vector<u8> display(count * 4 + 1), packed(count * 2);
    for (u32 i = 0; i < count; i++) {
      const u8 r = (u8)i, g = (u8)(i >> 8), b = (u8)(i * 73 + (i >> 8));
      display[1 + i * 4 + (big_endian ? 0 : 3)] = (u8)(i * 17);
      display[1 + i * 4 + (big_endian ? 1 : 2)] = r;
      display[1 + i * 4 + (big_endian ? 2 : 1)] = g;
      display[1 + i * 4 + (big_endian ? 3 : 0)] = b;
      const u16 pixel = (u16)((r >> 3) << 10 | (g >> 3) << 5 | (b >> 3));
      packed[i * 2 + (big_endian ? 0 : 1)] = (u8)(pixel >> 8);
      packed[i * 2 + (big_endian ? 1 : 0)] = (u8)pixel;
    }
    for (u32 width : {0u, 1u, 7u, 15u, 16u, 17u, 31u, 32u, 33u, 127u, count}) {
      std::vector<u8> dst(width * 2 + 2, 0x5a);
      display_to_rgb555(display.data() + 1, dst.data() + 1, width, big_endian);
      if (dst.front() != 0x5a || dst.back() != 0x5a ||
          memcmp(dst.data() + 1, packed.data(), width * 2)) return false;
    }
  }
  return true;
}

int main() {
  if (!test_rgb555()) return 1;
  const u8 ppc[] = {0x7c, 0x00, 0x03, 0xe0, 0xaa, 0xaa,
                    0x00, 0x1f, 0x42, 0x10, 0xaa, 0xaa};
  const u8 intel[] = {0x00, 0x7c, 0xe0, 0x03, 0xaa, 0xaa,
                      0x1f, 0x00, 0x10, 0x42, 0xaa, 0xaa};
  const u8 expected_ppc[] = {0, 255, 0, 0, 0, 0, 255, 0,
                             0, 0, 0, 255, 0, 131, 131, 131};
  const u8 expected_intel[] = {0, 0, 255, 0, 0, 255, 0, 0,
                               255, 0, 0, 0, 131, 131, 131, 0};
  u8 out[16] = {};
  for (u32 y = 0; y < 2; y++) rgb555_to_display(ppc + y * 6, out + y * 8, 2, true);
  if (memcmp(out, expected_ppc, sizeof out)) return 1;
  for (u32 y = 0; y < 2; y++) rgb555_to_display(intel + y * 6, out + y * 8, 2, false);
  if (memcmp(out, expected_intel, sizeof out)) return 1;
  u8 converted[] = {0, 0, 0, 0, 0xaa, 0xaa, 0, 0, 0, 0, 0xaa, 0xaa};
  for (u32 y = 0; y < 2; y++) display_to_rgb555(expected_ppc + y * 8, converted + y * 6, 2, true);
  if (memcmp(converted, ppc, sizeof converted)) return 1;
  for (u32 y = 0; y < 2; y++) display_to_rgb555(expected_intel + y * 8, converted + y * 6, 2, false);
  return memcmp(converted, intel, sizeof converted) ? 1 : 0;
}
