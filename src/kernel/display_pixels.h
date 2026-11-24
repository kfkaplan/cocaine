#pragma once

#include "common.h"

inline void rgb555_to_display(const u8* src, u8* dst, u32 width, bool big_endian) {
  for (u32 x = 0; x < width; x++) {
    const u16 pixel = big_endian ? (u16)((u16)src[2*x] << 8 | src[2*x+1])
                                 : (u16)((u16)src[2*x+1] << 8 | src[2*x]);
    const u8 r = (u8)(((pixel >> 10) & 31) * 255 / 31);
    const u8 g = (u8)(((pixel >> 5) & 31) * 255 / 31);
    const u8 b = (u8)((pixel & 31) * 255 / 31);
    if (big_endian) { dst[4*x] = 0; dst[4*x+1] = r; dst[4*x+2] = g; dst[4*x+3] = b; }
    else { dst[4*x] = b; dst[4*x+1] = g; dst[4*x+2] = r; dst[4*x+3] = 0; }
  }
}
