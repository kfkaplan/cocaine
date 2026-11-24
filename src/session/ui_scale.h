#pragma once

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

inline bool configure_legacy_ui_scale(const std::vector<std::string>& libraries, std::string& error) {
  if (getenv("COCAINE_UI_SCALE")) return true;
  for (const std::string& library : libraries) {
    for (const char* suffix : {"libSDL-1.2.0.dylib", "SDL.framework/Versions/A/SDL",
                                "SDL.framework/Versions/Current/SDL", "SDL.framework/SDL"}) {
      const size_t n = strlen(suffix);
      if (library.size() < n || library.compare(library.size() - n, n, suffix) != 0 ||
          (library.size() > n && library[library.size() - n - 1] != '/')) continue;
      // SDL 1.2 mixes a window's pixel frame with its view's point frame
      // when locating software pixels. They must use the same units.
      if (setenv("COCAINE_UI_SCALE", "1", 0) != 0) {
        error = std::string("cannot set SDL 1.2 UI scaling: ") + strerror(errno);
        return false;
      }
      return true;
    }
  }
  return true;
}
