#pragma once

#include "common.h"

// How a guest's Mach message crosses between cocaine processes.
//
// Every port a guest can name is a real port of the host process running it,
// under the same name, so this Mac's kernel does the hard parts of Mach for
// the guests: queueing, moving port rights between processes, noticing dead
// ports. What it can't do is understand a Mac OS X 10.5 message: those are
// laid out for a 32-bit machine, in PowerPC byte order half the time.
//
// So a guest message travels inside a host message:
//
//   host header             destination, reply port and message number
//   host descriptors        one per port right or block of out-of-line
//                           memory in the guest message, in the same order,
//                           so the host kernel transfers them
//   Meta                    below
//   the guest message       exactly as the sender wrote it
//
// The receiving side copies the guest message out and rewrites the fields the
// kernel would have rewritten: the header, and each descriptor with the port
// name or memory address the receiver was given.
namespace tunnel {

constexpr u32 kMagic = 0x636F6341;  // "cocA"

struct Meta {
  u32 magic;
  u32 flags;  // bit 0: the guest message is big-endian (its sender is PowerPC)
  u32 size;   // of the guest message that follows
  u32 reserved;
};

constexpr u32 kBigEndian = 1;
// The sender is a guest that was told it is root (a system daemon). The host
// kernel stamps every message with its sender's real user, which is whoever
// runs cocaine; the receiving side reports root instead. Old programs check:
// Leopard's user-lookup code ignores answers that don't come from root.
constexpr u32 kFromRoot = 2;

// Message numbers cocaine's own processes use between themselves; these are
// plain host messages, not guest ones.
//
// A new session server asks the session's first process for the ports it is
// to take over:
constexpr s32 kHandoffRequest = 0x636F0001;
constexpr s32 kHandoffReply = 0x636F0002;
// A new process asks its session's server for the bootstrap port. It finds
// the server through the session's name, which the server holds with this
// Mac's own launchd, and which every process of the session is told.
constexpr s32 kJoin = 0x636F0003;
constexpr s32 kJoinReply = 0x636F0004;
// Sent to a task port that belongs to another process, to learn whose it is
// (the pid_for_task call); the answer carries the process number.
constexpr s32 kWhoAreYou = 0x636F0005;
constexpr s32 kIAm = 0x636F0006;
// Memory shared by port (a "memory entry") can only be whole host pages,
// which are four guest pages each. When a guest shares a range that starts
// part-way into a host page, the entry covers the whole pages around it, and
// the session server is told how far in the range begins, so that whoever
// maps the entry can ask and line its own view up the same way.
constexpr s32 kEntryNote = 0x636F0007;    // an entry port, then the offset
constexpr s32 kEntryAsk = 0x636F0008;     // an entry port
constexpr s32 kEntryAnswer = 0x636F0009;  // the offset (0 if never told)
// The login window tells the session server it has finished setting the
// window server's session up, which is when applications may come in. See
// where it is sent for how that moment is recognized.
constexpr s32 kSessionReady = 0x636F000A;

}  // namespace tunnel
