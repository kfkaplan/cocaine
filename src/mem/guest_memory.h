#pragma once

#include <functional>
#include <mutex>
#include <vector>

#include "common.h"

// The guest's whole 32-bit address space, as one 4 GB window of host memory.
//
// A native arm64 process cannot map anything below 4 GB, so guest address A
// lives at host address base() + A. The window is reserved up front with no
// access; guest mappings then open pieces of it.
//
// Guests think in 4 KB pages while Apple Silicon uses 16 KB pages, so the host
// can't enforce guest page protections exactly. Protections are tracked per
// 4 KB guest page here, and a host page is readable and writable whenever any
// guest page inside it is mapped. The one exception that matters is arranged
// for: the guest's zero page gets a host page of its own, so null-pointer
// accesses fault.
class GuestMemory {
 public:
  enum : u8 {
    kRead = 1,
    kWrite = 2,
    kExec = 4,
    kMapped = 0x10,
    kDirty = 0x20,  // was mapped before, so it may hold stale bytes
    // Backed by memory shared with other processes rather than this one's
    // own. Set on its own (not mapped) for a page the guest has unmapped
    // whose host page is still shared: it can't be reused until that goes.
    kShared = 0x40,
    // Handed out by find_free and not mapped yet: no other thread gets it meanwhile.
    kReserved = 0x80,
  };
  static constexpr u64 kSpan = 1ull << 32;
  static constexpr u32 kPageSize = 4096;

  GuestMemory() = default;
  ~GuestMemory();
  GuestMemory(const GuestMemory&) = delete;
  GuestMemory& operator=(const GuestMemory&) = delete;

  bool init(bool big_endian, std::string& err);

  u8* base() const { return base_; }
  u8* host(u32 addr) const { return base_ + addr; }
  bool big_endian() const { return big_endian_; }
  // True if `p` points inside the guest window (used to classify host faults).
  bool contains_host(const void* p) const;

  // Maps [addr, addr+size) zero-filled, replacing whatever was there.
  bool map(u32 addr, u64 size, u8 prot);
  void unmap(u32 addr, u64 size);
  bool protect(u32 addr, u64 size, u8 prot);
  // Maps `size` bytes of a file, from `offset`, shared with every other
  // process mapping it, somewhere in [lo, hi). Returns 0 if that can't be
  // done: the host can only share whole pages of its own, larger, size, so
  // the offset must be a multiple of that.
  u32 map_shared_file(u64 size, u8 prot, int fd, u64 offset, u32 lo, u64 hi);
  // The general form: finds room for `size` bytes (rounded up to whole host
  // pages) that starts on a host page, and has `place` put shared memory of
  // some kind there. Returns the guest address, or 0.
  // `lead` bytes at the start of the shared memory are not the guest's to
  // see: the address returned is that far in, and the pages before it (and
  // any after the end) are only kept out of use.
  u32 map_shared(u64 size, u8 prot, u32 lo, u64 hi, const std::function<bool(void*, u64)>& place,
                 u64 lead = 0);
  // The same at an address the guest chose. That only works if the address
  // is as far into its host page as `lead` says the shared range is, and
  // nothing else is using the host pages involved. Returns 0 for success,
  // 1 if the address can't line up, 2 if the space is taken.
  int map_shared_at(u32 addr, u64 size, u8 prot, const std::function<bool(void*, u64)>& place,
                    u64 lead);
  // Ordinary zero-filled memory that starts on a host page and has the rest
  // of its last host page to itself, so that it can be shared exactly later.
  u32 map_alone(u64 size, u8 prot, u32 lo, u64 hi);
  u64 host_page() const { return host_page_; }
  // How far into its host page a guest address is.
  u32 host_page_offset(u32 addr) const { return (u32)((addr + skew_) & (host_page_ - 1)); }
  bool on_host_page(u32 addr) const { return host_page_offset(addr) == 0; }
  // Lowest unmapped run of `size` bytes inside [lo, hi) that starts on a
  // multiple of `align`, or 0 if none.
  u32 find_free(u64 size, u32 lo, u64 hi, u64 align = kPageSize) const;
  // True if every page of the range is mapped with some access, so the
  // kernel layer can touch it. `need` adds required protection bits
  // (kRead/kWrite).
  bool range_ok(u32 addr, u64 size, u8 need = 0) const;
  // Takes or releases the lock on the page table, to hold it across a fork.
  void fork_lock(bool take) const { take ? mu_.lock() : mu_.unlock(); }

  // Called (if set) when a range of guest memory is about to be unmapped or
  // mapped afresh, so that translations of code that was there are dropped.
  static inline void (*on_change)(u32 addr, u64 size) = nullptr;
  // True if any page of the range is in use: mapped, shared, or set aside by
  // find_free for a mapping still to come.
  bool any_mapped(u32 addr, u64 size) const;
  // The mapped region containing `addr`, or the next one above it: its start,
  // length and protection. Pages mapped alike and touching count as one.
  bool region_at(u32 addr, u32& start, u32& length, u8& prot) const;

  // Guest-endian accessors for the kernel layer. They don't check mappings;
  // call range_ok() first when the address comes from the guest.
  u8 r8(u32 a) const { return base_[a]; }
  u16 r16(u32 a) const;
  u32 r32(u32 a) const;
  u64 r64(u32 a) const;
  void w8(u32 a, u8 v) { base_[a] = v; }
  void w16(u32 a, u16 v);
  void w32(u32 a, u32 v);
  void w64(u32 a, u64 v);

  // Checked bulk helpers. They return false instead of faulting.
  bool copy_in(void* dst, u32 addr, u64 n) const;
  bool copy_out(u32 addr, const void* src, u64 n);
  bool read_cstr(u32 addr, std::string& out, size_t max = 1 << 20) const;

 private:
  void release_host(u64 from, u64 to);
  void unreserve(u64 from, u64 to);
  u64 host_floor(u64 guest) const;
  u64 host_ceil(u64 guest) const;

  static constexpr u64 kGuard = 1 << 16;
  static constexpr u64 kPages = kSpan / kPageSize;

  u8* region_ = nullptr;  // the whole reservation
  u8* base_ = nullptr;    // host address of guest address 0, `skew_` bytes in
  u64 skew_ = 0;
  bool big_endian_ = false;
  u64 host_page_ = 0;
  mutable std::vector<u8> pages_;  // mutable: find_free reserves what it finds
  mutable std::mutex mu_;
};
