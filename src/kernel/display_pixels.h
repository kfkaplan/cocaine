#pragma once

#include "common.h"

inline u8 expand_rgb5(u16 value) {
  // For 0..31, this is exactly floor(value * 255 / 31), with a 16-bit product.
  return (u16)(value * 2106) >> 8;
}

template <bool big_endian>
inline void rgb555_to_display_ordered(const u8* src, u8* dst, u32 width) {
  for (u32 x = 0; x < width; x++) {
    const u16 pixel = big_endian ? (u16)((u16)src[2*x] << 8 | src[2*x+1])
                                 : (u16)((u16)src[2*x+1] << 8 | src[2*x]);
    const u8 r = expand_rgb5((pixel >> 10) & 31);
    const u8 g = expand_rgb5((pixel >> 5) & 31);
    const u8 b = expand_rgb5(pixel & 31);
    if (big_endian) { dst[4*x] = 0; dst[4*x+1] = r; dst[4*x+2] = g; dst[4*x+3] = b; }
    else { dst[4*x] = b; dst[4*x+1] = g; dst[4*x+2] = r; dst[4*x+3] = 0; }
  }
}

inline void rgb555_to_display(const u8* src, u8* dst, u32 width, bool big_endian) {
  if (big_endian) rgb555_to_display_ordered<true>(src, dst, width);
  else rgb555_to_display_ordered<false>(src, dst, width);
}

template <bool big_endian>
inline void display_to_rgb555_ordered(const u8* src, u8* dst, u32 width) {
  for (u32 x = 0; x < width; x++) {
    const u8 r = src[4*x + (big_endian ? 1 : 2)];
    const u8 g = src[4*x + (big_endian ? 2 : 1)];
    const u8 b = src[4*x + (big_endian ? 3 : 0)];
    const u16 pixel = (u16)((r >> 3) << 10 | (g >> 3) << 5 | (b >> 3));
    dst[2*x + (big_endian ? 0 : 1)] = (u8)(pixel >> 8);
    dst[2*x + (big_endian ? 1 : 0)] = (u8)pixel;
  }
}

inline void display_to_rgb555(const u8* src, u8* dst, u32 width, bool big_endian) {
  if (big_endian) display_to_rgb555_ordered<true>(src, dst, width);
  else display_to_rgb555_ordered<false>(src, dst, width);
}
