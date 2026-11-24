#include <cstdio>
#include <vector>

#include "gl/vertex_copy.h"

int main() {
  for (size_t component_size : {1u, 2u, 4u, 8u}) {
    for (size_t components : {1u, 2u, 3u, 4u}) {
      const size_t element_size = component_size * components;
      for (size_t stride : {element_size, element_size + 5}) {
        for (size_t count : {0u, 1u, 3u, 4u, 5u, 37u}) {
          for (bool swap : {false, true}) {
            std::vector<uint8_t> input(2 + count * stride + element_size);
            for (size_t i = 0; i < input.size(); ++i) input[i] = (uint8_t)(i * 73 + 17);
            std::vector<uint8_t> expected(2 + count * element_size, 0xa5);
            std::vector<uint8_t> actual = expected;
            for (size_t i = 0; i < count; ++i) {
              for (size_t c = 0; c < components; ++c) {
                for (size_t b = 0; b < component_size; ++b) {
                  const size_t source = swap ? component_size - 1 - b : b;
                  expected[1 + i * element_size + c * component_size + b] =
                      input[1 + i * stride + c * component_size + source];
                }
              }
            }
            copy_vertex_array(input.data() + 1, actual.data() + 1, count, components,
                              component_size, stride, swap);
            if (actual != expected) {
              std::fprintf(stderr, "vertex copy failed: size=%zu components=%zu stride=%zu count=%zu swap=%d\n",
                           component_size, components, stride, count, swap);
              return 1;
            }
          }
        }
      }
    }
  }
  return 0;
}
