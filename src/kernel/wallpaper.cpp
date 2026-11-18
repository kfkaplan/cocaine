// The guest's desktop picture.
//
// On a real Leopard the Dock draws the desktop picture, in a window of its
// own below every other. The Dock can't be started here yet (guests can't
// launch other programs through Launch Services), so without this the desktop
// is the window server's plain grey. Instead, once loginwindow has the session
// ready, cocaine makes that window itself, from inside loginwindow and with
// Leopard's own CoreGraphics: a window the size of the screen at the desktop's
// level, the picture loaded from its file and drawn to fill it.
//
// COCAINE_WALLPAPER names the picture (a JPEG or PNG on this Mac), or "none".
// Without it: images/linen.jpeg beside cocaine's build folder, or else
// Leopard's own /System/Library/CoreServices/DefaultDesktop.jpg.

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cmath>
#include <string>

#include "kernel/process.h"

namespace {

u32 float_bits(float v) {
  u32 b;
  memcpy(&b, &v, 4);
  return b;
}
float bits_float(u32 b) {
  float v;
  memcpy(&v, &b, 4);
  return v;
}

bool ends_with_ci(const std::string& s, const char* end) {
  const size_t n = strlen(end);
  return s.size() >= n && strcasecmp(s.c_str() + s.size() - n, end) == 0;
}

}  // namespace

void Process::draw_wallpaper(Thread& t) {
  std::string path;
  if (const char* e = getenv("COCAINE_WALLPAPER")) {
    if (!strcmp(e, "none") || !*e) return;
    char full[PATH_MAX];
    path = realpath(e, full) ? full : e;
  } else {
    std::string dir = opt_.self_path;  // .../build/cocaine
    for (int up = 0; up < 2 && dir.rfind('/') != std::string::npos; up++) dir.erase(dir.rfind('/'));
    const std::string ours = dir + "/images/linen.jpeg";
    path = access(ours.c_str(), R_OK) == 0 ? ours : "/System/Library/CoreServices/DefaultDesktop.jpg";
  }

  auto fn = [&](const char* name) { return guest_symbol(name); };
  auto call = [&](const char* name, std::initializer_list<u32> args) { return call_guest(t, fn(name), args); };
  for (const char* name : {"CGSMainConnectionID", "CGMainDisplayID", "CGDisplayBounds", "CGSNewRegionWithRect",
                           "CGSNewWindow", "CGWindowLevelForKey", "CGSSetWindowLevel", "CGSOrderWindow",
                           "CGWindowContextCreate", "CGDataProviderCreateWithFilename", "CGImageGetWidth",
                           "CGImageGetHeight", "CGContextDrawImage", "CGContextFlush"}) {
    if (!fn(name)) {
      log_warn("no wallpaper: this CoreGraphics has no %s", name);
      return;
    }
  }

  // Guest memory for what's passed by address: the screen's rectangle, the
  // new region and window, and the picture's file name.
  const u32 rect = guest_scratch(16), region = guest_scratch(4), window = guest_scratch(4);
  const u32 name = guest_scratch((u32)path.size() + 1);
  if (!rect || !region || !window || !name) return;
  for (size_t i = 0; i <= path.size(); i++) mem_.w8(name + (u32)i, i < path.size() ? (u8)path[i] : 0);

  const u32 cid = call("CGSMainConnectionID", {});
  // CGDisplayBounds returns its rectangle through a hidden first argument.
  call("CGDisplayBounds", {rect, call("CGMainDisplayID", {})});
  const float x = bits_float(mem_.r32(rect)), y = bits_float(mem_.r32(rect + 4));
  const float w = bits_float(mem_.r32(rect + 8)), h = bits_float(mem_.r32(rect + 12));
  if (!(w > 0 && h > 0)) {
    log_warn("no wallpaper: the screen's size came back as %g x %g", w, h);
    return;
  }

  mem_.w32(region, 0);
  mem_.w32(window, 0);
  call("CGSNewRegionWithRect", {rect, region});
  // CGSNewWindow(connection, backing (2: buffered), left, top, region, &window)
  call_guest_args(t, fn("CGSNewWindow"),
                  {{cid}, {2}, {float_bits(x), true}, {float_bits(y), true}, {mem_.r32(region)}, {window}});
  const u32 wid = mem_.r32(window);
  if (!wid) {
    log_warn("no wallpaper: the window server made no window for it");
    return;
  }
  // Clicks go through it, as they did through the grey: owned by
  // loginwindow, it would otherwise make loginwindow the active program, and
  // the app in front would stop getting keys. (Tag bit 9 is what Cocoa's
  // setIgnoresMouseEvents: sets.)
  const u32 tags = guest_scratch(8);
  if (tags && fn("CGSSetWindowTags")) {
    mem_.w32(tags, 1u << 9);
    mem_.w32(tags + 4, 0);
    call("CGSSetWindowTags", {cid, wid, tags, 32});
  }
  const u32 level = call("CGWindowLevelForKey", {2});  // kCGDesktopWindowLevelKey
  call("CGSSetWindowLevel", {cid, wid, level});
  call("CGSOrderWindow", {cid, wid, 1, 0});  // in front of whatever else is at its level

  const u32 ctx = call("CGWindowContextCreate", {cid, wid, 0});
  const u32 provider = call("CGDataProviderCreateWithFilename", {name});
  const bool png = ends_with_ci(path, ".png");
  const u32 image = provider ? call(png ? "CGImageCreateWithPNGDataProvider" : "CGImageCreateWithJPEGDataProvider",
                                    {provider, 0, 1, 0})
                             : 0;
  if (!ctx || !image) {
    log_warn("no wallpaper: couldn't %s %s", ctx ? "load the picture" : "draw in the window", path.c_str());
    return;
  }
  // Scaled to fill the screen, the same shape, centred: the sides (or the
  // top and bottom) trimmed if its shape isn't the screen's.
  const float iw = (float)call("CGImageGetWidth", {image}), ih = (float)call("CGImageGetHeight", {image});
  const float scale = std::fmax(w / iw, h / ih);
  const float dw = iw * scale, dh = ih * scale;
  // A rectangle passed by value goes as four words on both CPUs.
  call("CGContextDrawImage", {ctx, float_bits((w - dw) / 2), float_bits((h - dh) / 2), float_bits(dw), float_bits(dh),
                              image});
  call("CGContextFlush", {ctx});
  log_info("the wallpaper is %s, drawn %g x %g on a %g x %g screen", path.c_str(), dw, dh, w, h);
}
