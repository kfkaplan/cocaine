#include <chrono>
#include <cstdio>
#include <mach/mach.h>

#include "kernel/input_notify.h"

int main() {
  const mach_port_t task = mach_task_self();
  mach_port_t port = MACH_PORT_NULL;
  if (mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &port) != KERN_SUCCESS ||
      mach_port_insert_right(task, port, port, MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS) {
    std::fprintf(stderr, "cannot create input notification port\n");
    return 1;
  }
  mach_port_limits_t limits = {};
  limits.mpl_qlimit = 1;
  if (mach_port_set_attributes(task, port, MACH_PORT_LIMITS_INFO,
                               (mach_port_info_t)&limits, MACH_PORT_LIMITS_INFO_COUNT) != KERN_SUCCESS ||
      notify_input(port) != KERN_SUCCESS) {
    std::fprintf(stderr, "cannot fill input notification port\n");
    return 1;
  }

  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 32; ++i) {
    if (notify_input(port) != MACH_SEND_TIMED_OUT) {
      std::fprintf(stderr, "full input port did not report a pending notification\n");
      return 1;
    }
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  if (elapsed >= std::chrono::milliseconds(800)) {
    std::fprintf(stderr, "input notifications blocked on a full port\n");
    return 1;
  }

  struct {
    mach_msg_header_t header;
    mach_msg_trailer_t trailer;
  } message = {};
  if (mach_msg(&message.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof message, port, 0,
               MACH_PORT_NULL) != KERN_SUCCESS ||
      notify_input(port) != KERN_SUCCESS) {
    std::fprintf(stderr, "input notification was lost or did not resume after draining\n");
    return 1;
  }
  if (mach_port_deallocate(task, port) != KERN_SUCCESS ||
      mach_port_mod_refs(task, port, MACH_PORT_RIGHT_RECEIVE, -1) != KERN_SUCCESS) {
    std::fprintf(stderr, "cannot release input notification port\n");
    return 1;
  }
  return 0;
}
