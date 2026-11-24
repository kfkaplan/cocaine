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
  const char* display = "COCAINE_DISPLAY";
  const char* viewer = "COCAINE_VIEWER_DOUBLE";
  if (unsetenv(name) != 0 || unsetenv(display) != 0 || unsetenv(viewer) != 0) return 1;
  auto postal = [&] {
    if (configure_legacy_ui_scale({"@executable_path/libSDL-1.2.0.dylib"}, error,
                                  "com.runningwithscissors.postal2")) return true;
    std::fprintf(stderr, "%s\n", error.c_str());
    return false;
  };
  if (!configure({"@executable_path/libSDL-1.2.0.dylib"}, "1") ||
      getenv(display) || getenv(viewer)) return 1;
  if (!postal() || strcmp(getenv(display), "1024x768") != 0 ||
      strcmp(getenv(viewer), "1") != 0 || strcmp(getenv(name), "1") != 0) return 1;
  if (setenv(display, "1280x1024", 1) != 0 || setenv(viewer, "0", 1) != 0 ||
      setenv(name, "2", 1) != 0) return 1;
  if (!postal() || strcmp(getenv(display), "1280x1024") != 0 ||
      strcmp(getenv(viewer), "0") != 0 || strcmp(getenv(name), "2") != 0) return 1;
  if (unsetenv(display) != 0 || unsetenv(viewer) != 0) return 1;
  if (!configure_legacy_ui_scale({"@executable_path/libSDL2-2.0.0.dylib"}, error,
                                 "com.runningwithscissors.postal2") ||
      getenv(display) || getenv(viewer)) return 1;
  if (unsetenv(name) != 0) return 1;
  auto snes = [&] {
    if (configure_legacy_ui_scale({}, error, "com.snes9x.macos.snes9xcustom")) return true;
    std::fprintf(stderr, "%s\n", error.c_str());
    return false;
  };
  if (!snes() || !getenv(name) || strcmp(getenv(name), "1") != 0 ||
      !getenv(display) || strcmp(getenv(display), "1280x800") != 0 ||
      !getenv(viewer) || strcmp(getenv(viewer), "1") != 0) return 1;
  if (setenv(name, "2", 1) != 0 || setenv(display, "1280x1024", 1) != 0 ||
      setenv(viewer, "0", 1) != 0) return 1;
  if (!snes() || strcmp(getenv(name), "2") != 0 ||
      strcmp(getenv(display), "1280x1024") != 0 || strcmp(getenv(viewer), "0") != 0) return 1;
  return 0;
}
