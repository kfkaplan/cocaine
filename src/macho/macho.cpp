#include "macho/macho.h"

#include <cerrno>
#include <cstdio>

namespace {

enum : u32 {
  kMagic32 = 0xFEEDFACEu,
  kMagic32Swapped = 0xCEFAEDFEu,
  kMagic64 = 0xFEEDFACFu,
  kMagic64Swapped = 0xCFFAEDFEu,
  kFatMagic = 0xCAFEBABEu,  // as read big-endian

  kCpuI386 = 7,
  kCpuPowerPC = 18,
  kCpuAbi64 = 0x01000000u,

  kLcSegment = 0x1,
  kLcUnixThread = 0x5,
  kLcLoadDylib = 0xC,
  kLcLoadDylinker = 0xE,
  kLcLoadWeakDylib = 0x80000018u,
  kLcMain = 0x80000028u,
};

u32 read_be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

bool read_file(const std::string& path, std::vector<u8>& out, std::string& err) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    err = path + ": " + strerror(errno);
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size < 0) {
    std::fclose(f);
    err = path + ": cannot read";
    return false;
  }
  out.resize((size_t)size);
  const size_t got = size ? std::fread(out.data(), 1, (size_t)size, f) : 0;
  std::fclose(f);
  if (got != (size_t)size) {
    err = path + ": short read";
    return false;
  }
  return true;
}

const char* cpu_name(u32 cputype) {
  switch (cputype) {
    case kCpuI386: return "i386";
    case kCpuPowerPC: return "ppc";
    case kCpuI386 | kCpuAbi64: return "x86_64";
    case kCpuPowerPC | kCpuAbi64: return "ppc64";
    case 12: return "arm";
    case 12 | kCpuAbi64: return "arm64";
    default: return "unknown";
  }
}

// Bounds-checked view of one thin Mach-O slice.
struct Slice {
  const u8* p;
  size_t n;
  bool swap;

  bool has(size_t off, size_t len) const { return off <= n && len <= n - off; }
  u32 w(size_t off) const {
    u32 v;
    memcpy(&v, p + off, 4);
    return swap ? __builtin_bswap32(v) : v;
  }
  std::string str(size_t off, size_t max) const {
    std::string s;
    while (max-- && off < n && p[off]) s.push_back((char)p[off++]);
    return s;
  }
};

bool parse_thin(MachImage& img, std::string& err) {
  const std::string& path = img.path;
  if (img.data.size() < 28) {
    err = path + ": too small to be a Mach-O file";
    return false;
  }
  u32 magic;
  memcpy(&magic, img.data.data(), 4);
  Slice s{img.data.data(), img.data.size(), false};
  if (magic == kMagic32) {
    s.swap = false;
  } else if (magic == kMagic32Swapped) {
    s.swap = true;
  } else if (magic == kMagic64 || magic == kMagic64Swapped) {
    err = path + ": this is a 64-bit program; cocaine only handles 32-bit PowerPC and Intel ones";
    return false;
  } else {
    err = path + ": not a Mach-O file";
    return false;
  }
  // The host is little-endian, so a swapped header means a big-endian file.
  img.big_endian = s.swap;

  const u32 cputype = s.w(4);
  if (cputype == kCpuPowerPC) {
    img.arch = Arch::PPC;
  } else if (cputype == kCpuI386) {
    img.arch = Arch::I386;
  } else {
    err = path + ": unsupported CPU type (" + cpu_name(cputype) + ")";
    return false;
  }
  img.cpusubtype = s.w(8);
  img.filetype = s.w(12);
  const u32 ncmds = s.w(16);
  const u32 sizeofcmds = s.w(20);
  img.flags = s.w(24);
  if (!s.has(28, sizeofcmds)) {
    err = path + ": load commands run past the end of the file";
    return false;
  }

  size_t off = 28;
  const size_t end = 28 + (size_t)sizeofcmds;
  for (u32 i = 0; i < ncmds; i++) {
    if (off + 8 > end) {
      err = path + ": truncated load command";
      return false;
    }
    const u32 cmd = s.w(off), cmdsize = s.w(off + 4);
    if (cmdsize < 8 || cmdsize > end - off) {
      err = path + ": bad load command size";
      return false;
    }
    switch (cmd) {
      case kLcSegment: {
        if (cmdsize < 56) {
          err = path + ": bad segment command";
          return false;
        }
        MachSegment seg;
        seg.name = s.str(off + 8, 16);
        seg.vmaddr = s.w(off + 24);
        seg.vmsize = s.w(off + 28);
        seg.fileoff = s.w(off + 32);
        seg.filesize = s.w(off + 36);
        seg.maxprot = s.w(off + 40);
        seg.initprot = s.w(off + 44);
        seg.flags = s.w(off + 52);
        if (seg.filesize > seg.vmsize || !s.has(seg.fileoff, seg.filesize)) {
          err = path + ": segment " + seg.name + " does not fit in the file";
          return false;
        }
        if ((u64)seg.vmaddr + seg.vmsize > (1ull << 32)) {
          err = path + ": segment " + seg.name + " runs past 4 GB";
          return false;
        }
        img.segments.push_back(seg);
        break;
      }
      case kLcUnixThread: {
        // cmd, cmdsize, flavor, count, then the register state. Flavor 1 is
        // the plain integer state on both architectures.
        if (cmdsize < 16) break;
        const u32 flavor = s.w(off + 8), count = s.w(off + 12);
        const size_t st = off + 16;
        if (flavor != 1 || cmdsize < 16 + (u64)count * 4) break;
        if (img.arch == Arch::PPC && count >= 1) {
          img.entry = s.w(st);  // srr0
          img.has_entry = true;
        } else if (img.arch == Arch::I386 && count >= 11) {
          img.entry = s.w(st + 10 * 4);  // eip
          img.has_entry = true;
        }
        break;
      }
      case kLcMain:
        img.has_main = true;
        break;
      case kLcLoadDylinker:
        if (cmdsize >= 12) {
          const u32 name_off = s.w(off + 8);
          if (name_off < cmdsize) img.dylinker = s.str(off + name_off, cmdsize - name_off);
        }
        break;
      case kLcLoadDylib:
      case kLcLoadWeakDylib:
        if (cmdsize >= 12) {
          const u32 name_off = s.w(off + 8);
          if (name_off < cmdsize) img.dylibs.push_back(s.str(off + name_off, cmdsize - name_off));
        }
        break;
      default:
        break;
    }
    off += cmdsize;
  }
  return true;
}

}  // namespace

u32 MachImage::header_addr() const {
  for (const MachSegment& seg : segments) {
    if (seg.fileoff == 0 && seg.filesize != 0) return seg.vmaddr;
  }
  return 0;
}

bool is_powerpc_pef(const u8* data, size_t size) {
  return size >= 40 && !memcmp(data, "Joy!peffpwpc", 12) &&
         read_be32(data + 12) == 1 && (data[32] || data[33]);
}

bool load_macho(const std::string& path, std::optional<Arch> want, MachImage& out,
                std::string& err) {
  std::vector<u8> file;
  if (!read_file(path, file, err)) return false;
  if (file.size() < 8) {
    err = path + ": not a Mach-O file";
    return false;
  }
  if (is_powerpc_pef(file.data(), file.size())) {
    err = path + (want && *want == Arch::I386
                      ? ": is a PowerPC PEF/CFM file, not i386"
                      : ": PowerPC PEF/CFM files must be launched through LaunchCFMApp");
    return false;
  }

  out = MachImage();
  out.path = path;

  if (read_be32(file.data()) == kFatMagic) {
    // Universal file: a big-endian table of slices.
    const u32 n = read_be32(file.data() + 4);
    if (n > 64 || file.size() < 8 + (size_t)n * 20) {
      err = path + ": bad universal header";
      return false;
    }
    // PowerPC first: it is emulated about three times as fast as Intel
    // (whose instructions take much more work to pick apart), and it can
    // open windows without the keys the Intel system programs need (see
    // kernel/protect.h). --arch i386 asks for the other half.
    const u32 order[2] = {kCpuPowerPC, kCpuI386};
    size_t pick_off = 0, pick_size = 0;
    bool found = false;
    std::string have;
    for (int pass = 0; pass < 2 && !found; pass++) {
      const u32 cpu = want ? (*want == Arch::PPC ? kCpuPowerPC : kCpuI386) : order[pass];
      for (u32 i = 0; i < n; i++) {
        const u8* e = file.data() + 8 + (size_t)i * 20;
        const u32 cputype = read_be32(e);
        if (pass == 0) {
          if (!have.empty()) have += ", ";
          have += cpu_name(cputype);
        }
        if (cputype == cpu && !found) {
          pick_off = read_be32(e + 8);
          pick_size = read_be32(e + 12);
          found = true;
        }
      }
      if (want) break;
    }
    if (!found) {
      err = path + ": no " + (want ? arch_name(*want) : "ppc or i386") +
            " slice in this universal file (it has: " + have + ")";
      return false;
    }
    if (pick_off > file.size() || pick_size > file.size() - pick_off) {
      err = path + ": universal slice runs past the end of the file";
      return false;
    }
    out.data.assign(file.begin() + (long)pick_off, file.begin() + (long)(pick_off + pick_size));
  } else {
    out.data = std::move(file);
  }

  if (!parse_thin(out, err)) return false;
  if (want && out.arch != *want) {
    err = path + ": is a " + arch_name(out.arch) + " file, not " + arch_name(*want);
    return false;
  }
  return true;
}
