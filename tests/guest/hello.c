/* The first thing cocaine ever ran: prints a greeting, its arguments, and one
 * environment variable. Checks the loader's initial stack and the write and
 * exit system calls. */
#include "rt.h"

static int starts_with(const char* s, const char* prefix) {
  while (*prefix) {
    if (*s++ != *prefix++) return 0;
  }
  return 1;
}

int test_main(int argc, char** argv, char** envp) {
  put_str("Hello from cocaine!\n");
  put_str("argc = ");
  put_dec(argc);
  put_str("\n");
  for (int i = 1; i < argc; i++) {
    put_str("argv[");
    put_dec(i);
    put_str("] = ");
    put_str(argv[i]);
    put_str("\n");
  }
  for (char** e = envp; *e; e++) {
    if (starts_with(*e, "COCAINE_TEST=")) {
      put_str(*e);
      put_str("\n");
    }
  }
  return argc == 4 ? 42 : 1;
}
