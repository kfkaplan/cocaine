#include "kernel/display_pixels.h"

#include <cstring>

int main() {
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
