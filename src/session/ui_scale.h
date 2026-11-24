#pragma once

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

inline bool configure_legacy_ui_scale(const std::vector<std::string>& libraries, std::string& error,
                                      const std::string& bundle_id = {}) {
  auto set_default = [&](const char* name, const char* value) {
    if (getenv(name)) return true;
    if (setenv(name, value, 0) == 0) return true;
    error = std::string("cannot set ") + name + ": " + strerror(errno);
    return false;
  };
  for (const std::string& library : libraries) {
    for (const char* suffix : {"libSDL-1.2.0.dylib", "SDL.framework/Versions/A/SDL",
                                "SDL.framework/Versions/Current/SDL", "SDL.framework/SDL"}) {
      const size_t n = strlen(suffix);
      if (library.size() < n || library.compare(library.size() - n, n, suffix) != 0 ||
          (library.size() > n && library[library.size() - n - 1] != '/')) continue;
      // SDL 1.2 mixes a window's pixel frame with its view's point frame
      // when locating software pixels. They must use the same units.
      if (!set_default("COCAINE_UI_SCALE", "1")) return false;
      if (bundle_id == "com.runningwithscissors.postal2" &&
          (!set_default("COCAINE_DISPLAY", "1024x768") ||
           !set_default("COCAINE_VIEWER_DOUBLE", "1"))) return false;
      return true;
    }
  }
  return true;
}
