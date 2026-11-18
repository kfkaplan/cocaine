// Decrypting Apple-protected programs: see protect.h.
//
// The method is the one in the "Dont Steal Mac OS X" kernel extension that
// comes with Mac OS X 10.5 (its page_transform routine, and the routine
// that fetches the keys): each 4096-byte page is two halves, the first
// decrypted with the SMC's key OSK0 and the second with OSK1, both AES with
// 256-bit keys in CBC mode starting from a vector of zeroes. The kernel
// (xnu's mach_loader.c) applies it to a protected segment's pages from the
// fourth page of the file on.

#include "kernel/protect.h"

#include <CommonCrypto/CommonCryptor.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {

constexpr u32 kPage = 4096;
constexpr u32 kPlainPages = 3;  // of the file, from its start

struct Keys {
  bool found = false;
  u8 first[32], second[32];
};

const Keys& keys(const std::string& sysroot) {
  static Keys k;
  static std::once_flag once;
  std::call_once(once, [&] {
    std::string text;
    if (const char* e = getenv("COCAINE_OSK")) {
      text = e;
    } else if (FILE* f = fopen((sysroot + "/.cocaine/osk").c_str(), "r")) {
      char buf[256];
      const size_t n = fread(buf, 1, sizeof buf, f);
      fclose(f);
      text.assign(buf, n);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    if (text.size() != 64) return;
    memcpy(k.first, text.data(), 32);
    memcpy(k.second, text.data() + 32, 32);
    k.found = true;
  });
  return k;
}

bool decrypt_half(u8* half, const u8* key) {
  static const u8 kZeroes[16] = {};
  u8 plain[kPage / 2];
  size_t done = 0;
  if (CCCrypt(kCCDecrypt, kCCAlgorithmAES, 0, key, 32, kZeroes, half, sizeof plain, plain, sizeof plain,
              &done) != kCCSuccess ||
      done != sizeof plain) {
    return false;
  }
  memcpy(half, plain, sizeof plain);
  return true;
}

}  // namespace

bool have_protection_keys(const std::string& sysroot) { return keys(sysroot).found; }

bool unprotect_segment(u8* bytes, u32 fileoff, u32 size, const std::string& sysroot, std::string& err) {
  const Keys& k = keys(sysroot);
  if (!k.found) {
    err = "this is one of the programs Apple stored encrypted, and the keys to it are missing. They are "
          "the two an Intel Mac keeps in its SMC (OSK0 and OSK1); cocaine looks for them, run together, "
          "in COCAINE_OSK or in the file .cocaine/osk in the system root";
    return false;
  }
  // The first three pages of the file are plain, whichever segment they are in.
  const u32 plain_until = kPlainPages * kPage;
  u32 at = fileoff < plain_until ? plain_until - fileoff : 0;
  for (; at + kPage <= size; at += kPage) {
    if (!decrypt_half(bytes + at, k.first) || !decrypt_half(bytes + at + kPage / 2, k.second)) {
      err = "could not decrypt a protected page";
      return false;
    }
  }
  return true;
}
