#pragma once

#include "common.h"

// Apple-protected programs.
//
// A handful of Mac OS X's own Intel programs (the Finder, the Dock, the
// login window, the font server and a few more) are stored with most of
// their code encrypted: a segment flagged "protected" is plain for the first
// three pages of the file and AES-encrypted after that. The real kernel
// decrypts each page as it is brought into memory, with two keys it does not
// have itself: the "Dont Steal Mac OS X" kernel extension reads them from
// the SMC, a chip on the logic board of every Intel Mac. That was Apple's
// way of tying the Intel system to its own hardware.
//
// cocaine does what the kernel and that extension do (the method is read
// off the extension in the system root: see protect.cpp), so that those
// programs run from a copy of Mac OS X the person owns, on a Mac. The keys
// are not in this source, for the same reason they are not on the install
// disc: they belong to the hardware. They are looked for in the environment
// variable COCAINE_OSK and then in the file .cocaine/osk in the system
// root: the SMC's two keys OSK0 and OSK1 run together, 64 characters.

// Whether the keys are to hand.
bool have_protection_keys(const std::string& sysroot);

// Decrypts, in place, a protected segment that has been read into `bytes`:
// `fileoff` is where in the program's file it starts and `size` how much of
// it there is. Returns false, with a reason, if the keys are missing.
bool unprotect_segment(u8* bytes, u32 fileoff, u32 size, const std::string& sysroot, std::string& err);
