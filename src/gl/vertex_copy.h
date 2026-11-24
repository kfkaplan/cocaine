#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

inline void copy_vertex_array(const uint8_t* src, uint8_t* dst, size_t count,
                              size_t components, size_t component_size, size_t stride, bool swap) {
  const size_t element_size = components * component_size;
  if (!swap || component_size == 1) {
    if (stride == element_size) {
      std::memcpy(dst, src, count * element_size);
    } else {
      for (size_t i = 0; i < count; ++i)
        std::memcpy(dst + i * element_size, src + i * stride, element_size);
    }
    return;
  }

  if (stride == element_size) {
    size_t i = 0, bytes = count * element_size;
#if defined(__ARM_NEON)
    for (; bytes - i >= 16; i += 16) {
      const uint8x16_t input = vld1q_u8(src + i);
      uint8x16_t output;
      if (component_size == 2) output = vrev16q_u8(input);
      else if (component_size == 4) output = vrev32q_u8(input);
      else output = vrev64q_u8(input);
      vst1q_u8(dst + i, output);
    }
#endif
    for (; i < bytes; i += component_size)
      for (size_t b = 0; b < component_size; ++b) dst[i + b] = src[i + component_size - 1 - b];
    return;
  }

  for (size_t i = 0; i < count; ++i) {
    const uint8_t* in = src + i * stride;
    uint8_t* out = dst + i * element_size;
    for (size_t k = 0; k < components; ++k, in += component_size, out += component_size) {
      if (component_size == 2) {
        uint16_t value;
        std::memcpy(&value, in, sizeof value);
        value = __builtin_bswap16(value);
        std::memcpy(out, &value, sizeof value);
      } else if (component_size == 4) {
        uint32_t value;
        std::memcpy(&value, in, sizeof value);
        value = __builtin_bswap32(value);
        std::memcpy(out, &value, sizeof value);
      } else {
        uint64_t value;
        std::memcpy(&value, in, sizeof value);
        value = __builtin_bswap64(value);
        std::memcpy(out, &value, sizeof value);
      }
    }
  }
}
