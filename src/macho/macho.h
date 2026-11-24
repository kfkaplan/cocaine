#pragma once

#include <optional>
#include <vector>

#include "common.h"

// A parsed 32-bit Mach-O file: one architecture slice of a PowerPC or Intel
// executable, dynamic linker, or library.
struct MachSegment {
  std::string name;
  u32 vmaddr = 0, vmsize = 0;
  u32 fileoff = 0, filesize = 0;
  u32 maxprot = 0, initprot = 0;
  u32 flags = 0;
};

// A segment flag: its contents in the file are encrypted ("protected"), all
// but the first three pages of the file. See kernel/protect.h.
constexpr u32 kSegmentProtected = 0x8;

struct MachImage {
  std::string path;
  Arch arch = Arch::PPC;
  bool big_endian = false;
  u32 cpusubtype = 0;
  u32 filetype = 0;
  u32 flags = 0;
  std::vector<u8> data;  // the bytes of the chosen slice
  std::vector<MachSegment> segments;

  bool has_entry = false;  // LC_UNIXTHREAD gives the start address directly
  u32 entry = 0;
  bool has_main = false;  // LC_MAIN (10.8+): entry is found by dyld
  std::string dylinker;   // empty for static executables
  std::vector<std::string> dylibs;

  // Address the Mach-O header itself is mapped at, or 0 if no segment maps it.
  u32 header_addr() const;
};

enum : u32 {
  kMachExecute = 2,
  kMachDylib = 6,
  kMachDylinker = 7,
  kMachBundle = 8,
};

bool is_powerpc_pef(const u8* data, size_t size);

// Reads `path` and picks one slice. `want` forces an architecture; without it
// a universal file gives its PowerPC slice if it has one, else Intel.
bool load_macho(const std::string& path, std::optional<Arch> want, MachImage& out,
                std::string& err);
