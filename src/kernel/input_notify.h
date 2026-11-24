#pragma once

#include <mach/mach.h>

inline kern_return_t notify_input(mach_port_t port) {
  mach_msg_header_t message = {};
  message.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
  message.msgh_size = sizeof message;
  message.msgh_remote_port = port;
  return mach_msg(&message, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof message, 0,
                  MACH_PORT_NULL, 0, MACH_PORT_NULL);
}
