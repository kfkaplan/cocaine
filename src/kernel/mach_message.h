#pragma once

#include <vector>

#include "common.h"

// Bits and port-right kinds of the Mach message format.
constexpr u32 kComplex = 0x80000000u;  // the message carries ports or out-of-line memory
constexpr u32 kMoveSend = 17;          // how a reply hands a port right to the guest

// Builds a message for the guest to receive, in the guest's byte order.
class Message {
 public:
  Message(bool big_endian, u32 local_port, s32 id, bool complex = false) : be_(big_endian) {
    put(0x1200 | (complex ? kComplex : 0));  // bits: the receiver holds a send-once right
    put(0);                                  // size, filled in by finish()
    put(0);                                  // remote port
    put(local_port);
    put(0);                                  // reserved
    put((u32)id);
  }

  void put(u32 v) {
    if (be_) v = __builtin_bswap32(v);
    const u8* p = (const u8*)&v;
    bytes_.insert(bytes_.end(), p, p + 4);
  }
  void put64(u64 v) {
    if (be_) {
      put((u32)(v >> 32));
      put((u32)v);
    } else {
      put((u32)v);
      put((u32)(v >> 32));
    }
  }
  // The data-representation record that follows the header of simple messages.
  void ndr() {
    const u8 record[8] = {0, 0, 0, 0, (u8)(be_ ? 0 : 1), 0, 0, 0};
    bytes_.insert(bytes_.end(), record, record + 8);
  }
  // A port right carried in a complex message.
  void port(u32 name, u32 disposition) {
    put(name);
    put(0);
    const u8 tail[4] = {0, 0, (u8)disposition, 0};  // type 0: a port descriptor
    bytes_.insert(bytes_.end(), tail, tail + 4);
  }

  // Out-of-line memory carried in a complex message.
  void ool_data(u32 address, u32 size) {
    put(address);
    put(size);
    const u8 tail[4] = {0, 1, 0, 1};  // a virtual copy; type 1: out-of-line data
    bytes_.insert(bytes_.end(), tail, tail + 4);
  }
  // An out-of-line array of port rights carried in a complex message.
  void ool_ports(u32 address, u32 count) {
    put(address);
    put(count);
    const u8 tail[4] = {0, 0, (u8)kMoveSend, 2};  // type 2: out-of-line ports
    bytes_.insert(bytes_.end(), tail, tail + 4);
  }

  std::vector<u8> finish() {
    u32 size = (u32)bytes_.size();
    if (be_) size = __builtin_bswap32(size);
    memcpy(bytes_.data() + 4, &size, 4);
    return bytes_;
  }

 private:
  bool be_;
  std::vector<u8> bytes_;
};
