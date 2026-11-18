#include "common.h"

int g_verbosity = 0;

static void emit(const char* fmt, va_list ap) {
  std::fputs("cocaine: ", stderr);
  std::vfprintf(stderr, fmt, ap);
  std::fputc('\n', stderr);
}

void log_warn(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  emit(fmt, ap);
  va_end(ap);
}

void log_info(const char* fmt, ...) {
  if (g_verbosity < 1) return;
  va_list ap;
  va_start(ap, fmt);
  emit(fmt, ap);
  va_end(ap);
}

void log_debug(const char* fmt, ...) {
  if (g_verbosity < 2) return;
  va_list ap;
  va_start(ap, fmt);
  emit(fmt, ap);
  va_end(ap);
}
