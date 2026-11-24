#include "mem/guest_memory.h"

#include <sys/mman.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

bool check_allocations() {
  GuestMemory mem;
  std::string err;
  if (!mem.init(false, err)) {
    std::fprintf(stderr, "guest memory init: %s\n", err.c_str());
    return false;
  }
  constexpr u32 base = 0x10000000, page = GuestMemory::kPageSize, count = 384;
  std::vector<bool> used(count, false);
  u32 random = 0x7183b2adu;
  auto next = [&] {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    return random;
  };
  for (u32 i = 0; i < count; i++) {
    if (i >= 96 && next() % 3 == 0) continue;
    const u32 at = base + i * page;
    if (i % 3 == 0) {
      if (mem.find_free(page, at, (u64)at + page) != at) return false;
    } else if (!mem.map(at, page, i % 3 == 1 ? GuestMemory::kRead : 0)) {
      return false;
    }
    used[i] = true;
  }
  for (u32 trial = 0; trial < 1200; trial++) {
    const u32 lo = trial < 32 ? 0 : next() % count;
    const u32 hi = trial < 32 ? count : lo + next() % (count - lo + 1);
    const u32 need = 1 + next() % 8;
    const u32 step = trial < 32 ? 1 : 1u << (next() % 4);
    const u32 offset = trial < 32 ? 0 : next() % 2;
    const u32 first = lo + offset;
    u32 expected = 0;
    for (u32 i = (first + step - 1) / step * step; i + need <= hi; i += step) {
      bool free = true;
      for (u32 j = i; j < i + need; j++) free = free && !used[j];
      if (free) {
        expected = base + i * page;
        for (u32 j = i; j < i + need; j++) used[j] = true;
        break;
      }
    }
    const u32 actual = mem.find_free(need * page - (trial % 2), base + lo * page + offset,
                                     (u64)base + hi * page, step * page);
    if (actual != expected) {
      std::fprintf(stderr, "allocation %u: got %x, expected %x\n", trial, actual, expected);
      return false;
    }
  }
  return true;
}

bool check_shared_pages() {
  GuestMemory mem;
  std::string err;
  if (!mem.init(false, err)) {
    std::fprintf(stderr, "guest memory init: %s\n", err.c_str());
    return false;
  }
  const u64 host_page = mem.host_page();
  if (host_page <= GuestMemory::kPageSize) return true;
  const u32 at = 0x30000000 + GuestMemory::kPageSize;
  const u64 size = 3 * host_page;
  if (mem.map_shared_at(at, size, GuestMemory::kRead | GuestMemory::kWrite,
                        [](void* p, u64 n) {
                          return mmap(p, n, PROT_READ | PROT_WRITE,
                                      MAP_SHARED | MAP_ANON | MAP_FIXED, -1, 0) != MAP_FAILED;
                        }, 0) != 0) {
    std::fprintf(stderr, "shared mapping failed\n");
    return false;
  }
  for (u64 offset = 0; offset < size; offset += host_page)
    mem.unmap(at + (u32)offset, host_page - GuestMemory::kPageSize);
  const u32 actual = mem.find_free(GuestMemory::kPageSize, at, (u64)at + size + GuestMemory::kPageSize);
  if (actual != at + size) {
    std::fprintf(stderr, "shared pages: got %x, expected %llx\n", actual, (unsigned long long)(at + size));
    return false;
  }
  return true;
}

}  // namespace

int main() { return check_allocations() && check_shared_pages() ? 0 : 1; }
