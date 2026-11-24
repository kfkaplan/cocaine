#include "session/ui_scale.h"

#include <cstdio>

int main() {
  const char* name = "COCAINE_UI_SCALE";
  std::string error;
  auto configure = [&](std::vector<std::string> libraries, const char* expected) {
    if (!configure_legacy_ui_scale(libraries, error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return false;
    }
    const char* actual = getenv(name);
    if ((!actual != !expected) || (actual && strcmp(actual, expected) != 0)) {
      std::fprintf(stderr, "UI scale: got %s, expected %s\n",
                   actual ? actual : "unset", expected ? expected : "unset");
      return false;
    }
    return true;
  };
  if (unsetenv(name) != 0) return 1;
  if (!configure({"/System/Library/Frameworks/Cocoa.framework/Versions/A/Cocoa",
                  "@executable_path/libSDL2-2.0.0.dylib",
                  "@executable_path/notlibSDL-1.2.0.dylib"}, nullptr)) return 1;
  if (!configure({"@executable_path/libSDL-1.2.0.dylib"}, "1")) return 1;
  if (unsetenv(name) != 0) return 1;
  if (!configure({"/Library/Frameworks/SDL.framework/Versions/A/SDL"}, "1")) return 1;
  if (setenv(name, "2", 1) != 0) return 1;
  if (!configure({"@executable_path/libSDL-1.2.0.dylib"}, "2")) return 1;
  return 0;
}
