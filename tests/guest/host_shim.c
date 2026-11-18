/* Lets a guest test program build as a normal host program, whose output is
 * the reference the emulated builds are checked against. */
#include <unistd.h>

#include "rt.h"

void rt_start(int argc, char** argv);

s32 sys_write(s32 fd, const void* buf, u32 n) { return (s32)write(fd, buf, n); }

void sys_exit(s32 code) { _exit(code); }

int main(int argc, char** argv) { rt_start(argc, argv); }
