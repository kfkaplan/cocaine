#include "mem/guest_memory.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>

namespace {
constexpr u64 kPageMask = GuestMemory::kPageSize - 1;

inline u64 page_floor(u64 a) { return a & ~kPageMask; }
inline u64 page_ceil(u64 a) { return (a + kPageMask) & ~kPageMask; }
}  // namespace

GuestMemory::~GuestMemory() {
  if (region_) munmap(region_, skew_ + kSpan + kGuard);
}

bool GuestMemory::init(bool big_endian, std::string& err) {
  big_endian_ = big_endian;
  host_page_ = (u64)sysconf(_SC_PAGESIZE);
  // The guard after the window makes an access that runs past the last guest
  // byte fault instead of touching unrelated host memory.
  // Guest address 0 is placed in the last 4 KB of a host page, so that the
  // guest's zero page has a host page to itself. Programs start at 0x1000,
  // and with 16 KB host pages they would otherwise share a host page with
  // the zero page, making null-pointer accesses quietly succeed.
  skew_ = host_page_ > kPageSize ? host_page_ - kPageSize : 0;
  void* p = mmap(nullptr, skew_ + kSpan + kGuard, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (p == MAP_FAILED) {
    err = std::string("cannot reserve the 4 GB guest address space: ") + strerror(errno);
    return false;
  }
  region_ = (u8*)p;
  base_ = region_ + skew_;
  pages_.assign(kPages, 0);
  return true;
}

bool GuestMemory::contains_host(const void* p) const {
  const u8* q = (const u8*)p;
  return region_ && q >= region_ && q < region_ + skew_ + kSpan + kGuard;
}

bool GuestMemory::map(u32 addr, u64 size, u8 prot) {
  if (!size) return true;
  const u64 start = page_floor(addr), end = page_ceil((u64)addr + size);
  if (end > kSpan) return false;
  if (on_change) on_change((u32)start, end - start);

  // A mapping with no access rights (how libSystem reserves page zero) only
  // marks the pages as taken. The host pages stay closed, so touching them
  // faults; they are opened if the protection is ever raised.
  const bool none = (prot & 7) == 0;
  std::lock_guard<std::mutex> lk(mu_);
  // Shared memory being replaced goes back first, a whole host page at a
  // time, or the new mapping's zeroes would land in the shared file.
  for (u64 h = host_ceil(start); h + host_page_ <= host_floor(end); h += host_page_) {
    if (pages_[(h - skew_) / kPageSize] & kShared) release_host(h, h + host_page_);
  }
  if (!none) {
    const u64 hs = host_floor(start), he = host_ceil(end);
    if (mprotect(region_ + hs, he - hs, PROT_READ | PROT_WRITE) != 0) return false;
  }

  for (u64 a = start; a < end; a += kPageSize) {
    u8& f = pages_[a / kPageSize];
    const bool stale = (f & (kMapped | kDirty)) != 0;
    // Fresh host pages are already zero. Only reused ones need clearing,
    // which keeps big first-time mappings (stacks, heaps) lazy.
    if (stale && !none) memset(base_ + a, 0, kPageSize);
    f = kMapped | (prot & 7) | (stale && none ? kDirty : 0);
  }
  return true;
}

u32 GuestMemory::map_shared(u64 size, u8 prot, u32 lo, u64 hi,
                            const std::function<bool(void*, u64)>& place, u64 lead) {
  if (!size) return 0;
  const u64 len = (lead + size + host_page_ - 1) & ~(host_page_ - 1);
  // A free stretch with room to slide up to where a host page begins.
  const u32 found = find_free(len + host_page_, lo, hi);
  if (!found) return 0;
  const u64 at = host_ceil(found) - skew_;
  std::lock_guard<std::mutex> lk(mu_);
  // Only part of what find_free set aside is used: the slide up to a host
  // page, and whatever is left past the end, go back to being free.
  const u64 reserved_end = (u64)found + page_ceil(len + host_page_);
  if (!place(base_ + at, len)) {
    unreserve(found, reserved_end);
    return 0;
  }
  const u64 from = at + lead, to = from + page_ceil(size);
  for (u64 a = at; a < at + len; a += kPageSize) {
    pages_[a / kPageSize] = a >= from && a < to ? (u8)(kMapped | kShared | (prot & 7)) : (u8)kShared;
  }
  unreserve(found, reserved_end);
  return (u32)from;
}

int GuestMemory::map_shared_at(u32 addr, u64 size, u8 prot,
                               const std::function<bool(void*, u64)>& place, u64 lead) {
  if (!size || (addr & kPageMask) || host_page_offset(addr) != lead) return 1;
  const u64 at = (u64)addr - lead;
  const u64 len = (lead + size + host_page_ - 1) & ~(host_page_ - 1);
  if (at + len > kSpan) return 2;
  std::lock_guard<std::mutex> lk(mu_);
  for (u64 a = at; a < at + len; a += kPageSize) {
    if (pages_[a / kPageSize] & (kMapped | kShared | kReserved)) return 2;
  }
  if (!place(base_ + at, len)) return 2;
  const u64 to = (u64)addr + page_ceil(size);
  for (u64 a = at; a < at + len; a += kPageSize) {
    pages_[a / kPageSize] = a >= addr && a < to ? (u8)(kMapped | kShared | (prot & 7)) : (u8)kShared;
  }
  return 0;
}

u32 GuestMemory::map_shared_file(u64 size, u8 prot, int fd, u64 offset, u32 lo, u64 hi) {
  if (offset & (host_page_ - 1)) return 0;
  const int host_prot = (prot & kWrite) ? PROT_READ | PROT_WRITE : PROT_READ;
  return map_shared(size, prot, lo, hi, [&](void* at, u64 len) {
    return mmap(at, len, host_prot, MAP_SHARED | MAP_FIXED, fd, (off_t)offset) != MAP_FAILED;
  });
}

u32 GuestMemory::map_alone(u64 size, u8 prot, u32 lo, u64 hi) {
  if (!size) return 0;
  const u64 want = page_ceil(size), len = (size + host_page_ - 1) & ~(host_page_ - 1);
  const u32 found = find_free(len + host_page_, lo, hi);
  if (!found) return 0;
  const u32 at = (u32)(host_ceil(found) - skew_);
  const bool ok = map(at, want, prot);
  // The rest of the last host page is kept out of use.
  if (ok && len > want) map(at + (u32)want, len - want, 0);
  // What find_free set aside around that goes back to being free.
  std::lock_guard<std::mutex> lk(mu_);
  unreserve(found, (u64)found + page_ceil(len + host_page_));
  return ok ? at : 0;
}

// Pages in [from, to) that were set aside by find_free and never mapped are
// free again. Call with mu_ held.
void GuestMemory::unreserve(u64 from, u64 to) {
  if (to > kSpan) to = kSpan;
  for (u64 a = page_floor(from); a < to; a += kPageSize) pages_[a / kPageSize] &= (u8)~kReserved;
}

// Offsets into the reservation of the host pages covering a guest range.
u64 GuestMemory::host_floor(u64 guest) const { return (guest + skew_) & ~(host_page_ - 1); }
u64 GuestMemory::host_ceil(u64 guest) const {
  return (guest + skew_ + host_page_ - 1) & ~(host_page_ - 1);
}

// Hands back host pages [from, to), given as offsets into the reservation.
void GuestMemory::release_host(u64 from, u64 to) {
  // Replacing the range drops its memory and hands back zero pages later.
  mmap(region_ + from, to - from, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
  const u64 g0 = from > skew_ ? from - skew_ : 0;
  const u64 g1 = to - skew_ < kSpan ? to - skew_ : kSpan;
  // A page another thread has set aside stays set aside: only its owner
  // takes that back (unmap never releases such a host page anyway).
  for (u64 a = g0; a < g1; a += kPageSize) pages_[a / kPageSize] &= (u8)kReserved;
}

void GuestMemory::unmap(u32 addr, u64 size) {
  if (!size) return;
  const u64 start = page_floor(addr);
  u64 end = page_ceil((u64)addr + size);
  if (end > kSpan) end = kSpan;
  if (on_change && end > start) on_change((u32)start, end - start);

  std::lock_guard<std::mutex> lk(mu_);
  for (u64 a = start; a < end; a += kPageSize) {
    u8& f = pages_[a / kPageSize];
    if (f & kMapped) f = ((f & kShared) ? kShared : kDirty) | (f & kReserved);
  }

  // Give back host pages that no longer hold any mapped guest page. One that
  // holds a page set aside by find_free is kept: another thread is about to
  // map there, and may already have placed shared memory in it.
  const u64 hs = host_floor(start), he = host_ceil(end);
  u64 run = 0;
  bool in_run = false;
  for (u64 h = hs; h < he; h += host_page_) {
    const u64 g0 = h > skew_ ? h - skew_ : 0;
    const u64 g1 = h + host_page_ - skew_ < kSpan ? h + host_page_ - skew_ : kSpan;
    bool used = false;
    for (u64 a = g0; a < g1; a += kPageSize) {
      if (pages_[a / kPageSize] & (kMapped | kReserved)) {
        used = true;
        break;
      }
    }
    if (!used) {
      if (!in_run) {
        run = h;
        in_run = true;
      }
    } else if (in_run) {
      release_host(run, h);
      in_run = false;
    }
  }
  if (in_run) release_host(run, he);
}

bool GuestMemory::protect(u32 addr, u64 size, u8 prot) {
  const u64 start = page_floor(addr), end = page_ceil((u64)addr + size);
  if (end > kSpan) return false;
  if (on_change && end > start) on_change((u32)start, end - start);
  std::lock_guard<std::mutex> lk(mu_);
  // The host pages need opening only where a page was mapped with no access
  // (its host page may still be closed), or where shared memory that was
  // read-only becomes writable. Anything else is already readable and
  // writable underneath, and shared memory mapped from a read-only file
  // can't be opened for writing at all, so it must not be asked to be.
  bool needs_opening = false;
  for (u64 a = start; a < end; a += kPageSize) {
    const u8 f = pages_[a / kPageSize];
    if (!(f & kMapped)) return false;
    if (!(f & 7) || ((f & kShared) && (prot & kWrite) && !(f & kWrite))) needs_opening = true;
  }
  if ((prot & 7) && needs_opening) {
    const u64 hs = host_floor(start), he = host_ceil(end);
    if (mprotect(region_ + hs, he - hs, PROT_READ | PROT_WRITE) != 0) return false;
  }
  for (u64 a = start; a < end; a += kPageSize) {
    u8& f = pages_[a / kPageSize];
    if ((prot & 7) && (f & kDirty)) {
      memset(base_ + a, 0, kPageSize);
      f &= ~kDirty;
    }
    // Shared stays shared: unmapped later, such a page must not be handed
    // out again and zeroed while its host page still belongs to others.
    f = (f & (kMapped | kDirty | kShared)) | (prot & 7);
  }
  return true;
}

u32 GuestMemory::find_free(u64 size, u32 lo, u64 hi, u64 align) const {
  u64 need = page_ceil(size) / kPageSize;
  if (!need) need = 1;
  if (hi > kSpan) hi = kSpan;
  const u64 step = align > kPageSize ? align / kPageSize : 1;  // in pages
  std::lock_guard<std::mutex> lk(mu_);
  const u64 end = hi / kPageSize;
  u64 start = page_ceil(lo) / kPageSize;
  start = (start + step - 1) / step * step;
  while (start + need <= end) {
    // Find the last mapped page in the candidate range, if any, and skip past it.
    u64 blocked = 0;
    bool free = true;
    for (u64 i = start + need; i-- > start;) {
      if (pages_[i] & (kMapped | kShared | kReserved)) {
        blocked = i;
        free = false;
        break;
      }
    }
    if (free) {
      // Finding and mapping are two steps, and two threads allocating at
      // once must not both be given this range in between.
      for (u64 i = start; i < start + need; i++) pages_[i] |= kReserved;
      return (u32)(start * kPageSize);
    }
    start = (blocked + 1 + step - 1) / step * step;
  }
  return 0;
}

bool GuestMemory::range_ok(u32 addr, u64 size, u8 need) const {
  if (!size) return true;
  const u64 start = page_floor(addr), end = page_ceil((u64)addr + size);
  if (end > kSpan) return false;
  const u8 want = kMapped | need;
  std::lock_guard<std::mutex> lk(mu_);
  for (u64 a = start; a < end; a += kPageSize) {
    const u8 f = pages_[a / kPageSize];
    // A page reserved with no access at all has no memory behind it.
    if ((f & want) != want || !(f & (kRead | kWrite | kExec))) return false;
  }
  return true;
}

bool GuestMemory::any_mapped(u32 addr, u64 size) const {
  if (!size) return false;
  const u64 start = page_floor(addr);
  u64 end = page_ceil((u64)addr + size);
  if (end > kSpan) end = kSpan;
  std::lock_guard<std::mutex> lk(mu_);
  // Pages find_free has handed out count as taken: another thread is about
  // to map them, and a fixed allocation must not land there meanwhile.
  for (u64 a = start; a < end; a += kPageSize) {
    if (pages_[a / kPageSize] & (kMapped | kShared | kReserved)) return true;
  }
  return false;
}

bool GuestMemory::region_at(u32 addr, u32& start, u32& length, u8& prot) const {
  std::lock_guard<std::mutex> lk(mu_);
  u64 i = addr / kPageSize;
  while (i < kPages && !(pages_[i] & kMapped)) i++;
  if (i >= kPages) return false;
  const u8 flags = pages_[i] & (kMapped | 7);
  u64 j = i;
  while (j < kPages && (pages_[j] & (kMapped | 7)) == flags) j++;
  start = (u32)(i * kPageSize);
  length = (u32)((j - i) * kPageSize);
  prot = flags & 7;
  return true;
}

u16 GuestMemory::r16(u32 a) const {
  u16 v;
  memcpy(&v, base_ + a, 2);
  return big_endian_ ? __builtin_bswap16(v) : v;
}

u32 GuestMemory::r32(u32 a) const {
  u32 v;
  memcpy(&v, base_ + a, 4);
  return big_endian_ ? __builtin_bswap32(v) : v;
}

u64 GuestMemory::r64(u32 a) const {
  u64 v;
  memcpy(&v, base_ + a, 8);
  return big_endian_ ? __builtin_bswap64(v) : v;
}

void GuestMemory::w16(u32 a, u16 v) {
  if (big_endian_) v = __builtin_bswap16(v);
  memcpy(base_ + a, &v, 2);
}

void GuestMemory::w32(u32 a, u32 v) {
  if (big_endian_) v = __builtin_bswap32(v);
  memcpy(base_ + a, &v, 4);
}

void GuestMemory::w64(u32 a, u64 v) {
  if (big_endian_) v = __builtin_bswap64(v);
  memcpy(base_ + a, &v, 8);
}

bool GuestMemory::copy_in(void* dst, u32 addr, u64 n) const {
  if (!range_ok(addr, n)) return false;
  memcpy(dst, base_ + addr, n);
  return true;
}

bool GuestMemory::copy_out(u32 addr, const void* src, u64 n) {
  if (!range_ok(addr, n)) return false;
  memcpy(base_ + addr, src, n);
  return true;
}

bool GuestMemory::read_cstr(u32 addr, std::string& out, size_t max) const {
  out.clear();
  u64 a = addr;
  while (out.size() < max) {
    // Check one page at a time, then scan it for the terminator.
    const u64 page_end = page_floor(a) + kPageSize;
    if (a >= kSpan || !range_ok((u32)a, 1)) return false;
    const u8* p = base_ + a;
    const size_t n = (size_t)(page_end - a);
    const void* nul = memchr(p, 0, n);
    if (nul) {
      out.append((const char*)p, (const u8*)nul - p);
      return true;
    }
    out.append((const char*)p, n);
    a = page_end;
  }
  return false;
}
