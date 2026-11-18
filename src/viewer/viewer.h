#pragma once

#include <cstdint>

// The start of the file a guest's display is copied into for the viewer.
constexpr uint32_t kDisplayFileMagic = 0x636F6344;  // "cocD"
struct DisplayFileHeader {
  uint32_t magic;
  uint32_t width, height;
  uint32_t big_endian;  // the pixels are in a PowerPC guest's byte order
  uint32_t writer;      // the process keeping the picture up to date
  uint32_t frame;       // goes up each time the picture in the file changes
  uint32_t reserved[2];
};

// Shows the display in that file in a window, until the window is closed or
// the writer goes away.
int run_viewer(const char* path);
