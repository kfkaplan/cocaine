#pragma once

#include "mem/guest_memory.h"

// Writes the fields of a guest structure one after another, in the guest's
// byte order. The 32-bit Mac OS X ABIs lay structures out with at most 4-byte
// alignment, so fields simply follow each other.
class GuestWriter {
 public:
  GuestWriter(GuestMemory& mem, u32 at) : mem_(mem), at_(at) {}

  void u8_(u8 v) {
    mem_.w8(at_, v);
    at_ += 1;
  }
  void u16_(u16 v) {
    mem_.w16(at_, v);
    at_ += 2;
  }
  void u32_(u32 v) {
    mem_.w32(at_, v);
    at_ += 4;
  }
  void u64_(u64 v) {
    mem_.w64(at_, v);
    at_ += 8;
  }
  void zero(u32 n) {
    memset(mem_.host(at_), 0, n);
    at_ += n;
  }
  u32 at() const { return at_; }

 private:
  GuestMemory& mem_;
  u32 at_;
};
