// File-system calls that need more than a one-line translation.

#include <sys/attr.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

#include "kernel/guest_struct.h"
#include "kernel/process.h"

namespace {

// How one attribute is laid out in a getattrlist buffer. The host and the
// 32-bit guest agree on everything except times, which are 16 bytes on the
// host and 8 for the guest, and (for PowerPC) byte order.
enum AttrType {
  kU32,      // one 32-bit value
  kU64,      // one 64-bit value
  kTime,     // a timespec
  kRef,      // offset and length of variable-length data placed after the fixed part
  kPair,     // two 32-bit values (a file-system or object identifier)
  kBytes16,  // opaque bytes, copied as they are
  kBytes32,
  kWords8,   // eight 32-bit values (volume capabilities)
  kWords10,  // ten 32-bit values (supported-attribute sets)
};

struct AttrDef {
  u32 bit;
  AttrType type;
};

const AttrDef kCommon[] = {
    {0x00000001, kRef},   {0x00000002, kU32},  {0x00000004, kPair},    {0x00000008, kU32},
    {0x00000010, kU32},   {0x00000020, kPair}, {0x00000040, kPair},    {0x00000080, kPair},
    {0x00000100, kU32},   {0x00000200, kTime}, {0x00000400, kTime},    {0x00000800, kTime},
    {0x00001000, kTime},  {0x00002000, kTime}, {0x00004000, kBytes32}, {0x00008000, kU32},
    {0x00010000, kU32},   {0x00020000, kU32},  {0x00040000, kU32},     {0x00200000, kU32},
    {0x00400000, kRef},   {0x00800000, kBytes16}, {0x01000000, kBytes16}, {0x02000000, kU64},
    {0x04000000, kU64},
};
const AttrDef kVolume[] = {
    {0x00000001, kU32}, {0x00000002, kU32}, {0x00000004, kU64},    {0x00000008, kU64},
    {0x00000010, kU64}, {0x00000020, kU64}, {0x00000040, kU64},    {0x00000080, kU32},
    {0x00000100, kU32}, {0x00000200, kU32}, {0x00000400, kU32},    {0x00000800, kU32},
    {0x00001000, kRef}, {0x00002000, kRef}, {0x00004000, kU32},    {0x00008000, kRef},
    {0x00010000, kU64}, {0x00020000, kWords8}, {0x40000000, kWords10},
};
const AttrDef kDir[] = {{0x1, kU32}, {0x2, kU32}, {0x4, kU32}};
const AttrDef kFile[] = {
    {0x0001, kU32}, {0x0002, kU64}, {0x0004, kU64}, {0x0008, kU32}, {0x0010, kU32},
    {0x0020, kU32}, {0x0040, kU32}, {0x0080, kU32}, {0x0200, kU64}, {0x0400, kU64},
    {0x1000, kU64}, {0x2000, kU64},
};

u32 host_size(AttrType t) {
  switch (t) {
    case kU32: return 4;
    case kU64: return 8;
    case kTime: return 16;
    case kRef: return 8;
    case kPair: return 8;
    case kBytes16: return 16;
    case kBytes32: return 32;
    case kWords8: return 32;
    default: return 40;
  }
}

template <size_t N>
bool known(u32 wanted, const AttrDef (&defs)[N]) {
  for (const AttrDef& d : defs) wanted &= ~d.bit;
  return wanted == 0;
}

}  // namespace

// getattrlist: asks for an arbitrary set of a file's attributes in one call.
// Carbon's File Manager is built on it. The host has the same call, so the
// request goes straight through and the answer is re-packed for the guest.
SysRet Process::sys_getattrlist(const std::string& path, u32 list, u32 buf, u32 size, u32 options) {
  if (!mem_.range_ok(list, 24) || !mem_.range_ok(buf, size)) return SysRet::err(EFAULT);
  struct attrlist al = {};
  al.bitmapcount = mem_.r16(list);
  al.commonattr = mem_.r32(list + 4);
  al.volattr = mem_.r32(list + 8);
  al.dirattr = mem_.r32(list + 12);
  al.fileattr = mem_.r32(list + 16);
  al.forkattr = mem_.r32(list + 20);
  if (al.bitmapcount != 5) return SysRet::err(EINVAL);
  if (al.forkattr || !known(al.commonattr, kCommon) || !known(al.volattr & ~0x80000000u, kVolume) ||
      !known(al.dirattr, kDir) || !known(al.fileattr, kFile)) {
    log_warn("getattrlist asked for attributes that aren't translated yet "
             "(common %x volume %x dir %x file %x fork %x)",
             al.commonattr, al.volattr, al.dirattr, al.fileattr, al.forkattr);
    return SysRet::err(EINVAL);
  }

  const bool follow = !(options & 1);  // FSOPT_NOFOLLOW
  const std::string host = host_path(path, follow);
  std::vector<u8> in(16384);
  if (getattrlist(host.c_str(), &al, in.data(), in.size(), options) != 0) return SysRet::err(errno);
  struct stat st;
  const bool is_dir = (follow ? stat(host.c_str(), &st) : lstat(host.c_str(), &st)) == 0 && S_ISDIR(st.st_mode);

  // The attributes present, in the order both sides lay them out. Directory
  // attributes come back only for directories, file attributes only for files.
  std::vector<AttrType> fields;
  for (const AttrDef& d : kCommon) if (al.commonattr & d.bit) fields.push_back(d.type);
  for (const AttrDef& d : kVolume) if (al.volattr & d.bit) fields.push_back(d.type);
  if (is_dir) {
    for (const AttrDef& d : kDir) if (al.dirattr & d.bit) fields.push_back(d.type);
  } else {
    for (const AttrDef& d : kFile) if (al.fileattr & d.bit) fields.push_back(d.type);
  }

  const bool be = mem_.big_endian();
  std::vector<u8> out(4);  // the leading length word, filled in last
  auto put32 = [&](std::vector<u8>& v, size_t at, u32 x) {
    if (be) x = __builtin_bswap32(x);
    if (v.size() < at + 4) v.resize(at + 4);
    memcpy(v.data() + at, &x, 4);
  };
  auto get32 = [&](size_t at) -> u32 {
    u32 x = 0;
    if (at + 4 <= in.size()) memcpy(&x, in.data() + at, 4);
    return x;
  };

  struct Pending {
    size_t guest_ref;  // where the guest's offset/length pair sits
    size_t host_data;  // where the host put the data
    u32 length;
  };
  std::vector<Pending> pending;
  size_t hp = 4;
  for (AttrType t : fields) {
    const size_t gp = out.size();
    switch (t) {
      case kU32:
        put32(out, gp, get32(hp));
        break;
      case kU64:
      case kPair: {
        // A 64-bit value keeps its halves in the guest's order; a pair is
        // just two separate words.
        const u32 a = get32(hp), b = get32(hp + 4);
        const bool swap_halves = t == kU64 && be;
        put32(out, gp, swap_halves ? b : a);
        put32(out, gp + 4, swap_halves ? a : b);
        break;
      }
      case kTime: {
        s64 sec, nsec;
        memcpy(&sec, in.data() + hp, 8);
        memcpy(&nsec, in.data() + hp + 8, 8);
        put32(out, gp, (u32)sec);
        put32(out, gp + 4, (u32)nsec);
        break;
      }
      case kRef: {
        const s32 offset = (s32)get32(hp);
        pending.push_back({gp, (size_t)((s64)hp + offset), get32(hp + 4)});
        put32(out, gp, 0);
        put32(out, gp + 4, 0);
        break;
      }
      case kBytes16:
      case kBytes32:
        out.insert(out.end(), in.begin() + (long)hp, in.begin() + (long)hp + host_size(t));
        break;
      case kWords8:
      case kWords10:
        for (u32 i = 0; i < host_size(t); i += 4) put32(out, gp + i, get32(hp + i));
        break;
    }
    hp += host_size(t);
  }
  // Variable-length data follows the fixed part, each piece padded to 4 bytes.
  for (const Pending& p : pending) {
    const size_t at = out.size();
    if (p.host_data + p.length <= in.size()) {
      out.insert(out.end(), in.begin() + (long)p.host_data, in.begin() + (long)(p.host_data + p.length));
    }
    while (out.size() % 4) out.push_back(0);
    put32(out, p.guest_ref, (u32)(at - p.guest_ref));
    put32(out, p.guest_ref + 4, p.length);
  }
  put32(out, 0, (u32)out.size());
  memcpy(mem_.host(buf), out.data(), out.size() < size ? out.size() : size);
  return SysRet::ok();
}
