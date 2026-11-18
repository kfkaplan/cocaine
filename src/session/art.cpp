// cocaine's own menu bar logo: a pile of powder where the Apple logo was.
//
// Leopard's menu bar draws the Apple menu's logo from small PNGs kept in
// CoreUI's art file, SArtFile.bin: a black one, the white one shown while
// the menu is open, and grey and blue ones for the other appearances. The
// file is a header (a version, a count of images, where their data starts),
// a list of where each image's 16-byte entry is, the entries themselves
// (kind, width, height, length, and where the data is from the start of the
// data) and then the data. A copy of the file is made with each logo's entry
// pointing at a new picture added at the end, drawn here in the logo's own
// colour and size; the guest is shown the copy (see Process::host_path).
// Apple's file is left as it is.

#include "session/art.h"

#include <ApplicationServices/ApplicationServices.h>
#include <ImageIO/ImageIO.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "common.h"

namespace {

const char kArtFile[] = "/System/Library/PrivateFrameworks/CoreUI.framework/Versions/A/Resources/SArtFile.bin";

u16 le16(const std::vector<u8>& d, size_t at) { return (u16)(d[at] | d[at + 1] << 8); }
u32 le32(const std::vector<u8>& d, size_t at) {
  return (u32)d[at] | (u32)d[at + 1] << 8 | (u32)d[at + 2] << 16 | (u32)d[at + 3] << 24;
}
void put32(std::vector<u8>& d, size_t at, u32 v) {
  for (int i = 0; i < 4; i++) d[at + i] = (u8)(v >> (8 * i));
}

// What an image in the file is: its pixels, unpremultiplied RGBA.
std::vector<u8> pixels_of(const u8* png, size_t len, int w, int h) {
  std::vector<u8> out((size_t)w * h * 4);
  CFDataRef data = CFDataCreate(nullptr, png, (CFIndex)len);
  CGImageSourceRef src = CGImageSourceCreateWithData(data, nullptr);
  CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, nullptr) : nullptr;
  if (img) {
    CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
    CGContextRef c = CGBitmapContextCreate(out.data(), w, h, 8, (size_t)w * 4, rgb, kCGImageAlphaPremultipliedLast);
    CGContextDrawImage(c, CGRectMake(0, 0, w, h), img);
    CGContextRelease(c);
    CGColorSpaceRelease(rgb);
    CGImageRelease(img);
  }
  if (src) CFRelease(src);
  CFRelease(data);
  return out;
}

// The pile, drawn into the box the logo filled: a low, rounded heap of
// powder, with loose grains on the ground either side. At menu bar size the
// grains are what make it read as powder rather than a hill.
void draw_pile(CGContextRef c, CGRect box) {
  const CGFloat x = box.origin.x, y = box.origin.y, w = box.size.width, h = box.size.height;
  auto at = [&](CGFloat u, CGFloat v) { return CGPointMake(x + u * w, y + v * h); };
  CGMutablePathRef p = CGPathCreateMutable();
  const CGPoint start = at(0.12, 0.0);
  CGPathMoveToPoint(p, nullptr, start.x, start.y);
  auto curve = [&](CGFloat u1, CGFloat v1, CGFloat u2, CGFloat v2, CGFloat u, CGFloat v) {
    const CGPoint a = at(u1, v1), b = at(u2, v2), e = at(u, v);
    CGPathAddCurveToPoint(p, nullptr, a.x, a.y, b.x, b.y, e.x, e.y);
  };
  curve(0.18, 0.28, 0.30, 0.62, 0.48, 0.70);  // up the left side, steepening, to the top
  curve(0.56, 0.73, 0.62, 0.70, 0.66, 0.62);  // over the rounded top
  curve(0.74, 0.46, 0.82, 0.16, 0.88, 0.0);   // down the right side to the ground
  CGPathCloseSubpath(p);
  CGContextAddPath(c, p);
  CGContextFillPath(c);
  CGPathRelease(p);
  const struct {
    CGFloat u, v, r;
  } grains[] = {{0.03, 0.06, 0.05}, {0.97, 0.05, 0.045}, {0.95, 0.24, 0.035}, {0.06, 0.26, 0.035}};
  for (const auto& g : grains) {
    const CGPoint m = at(g.u, g.v);
    const CGFloat r = g.r * w;
    CGContextFillEllipseInRect(c, CGRectMake(m.x - r, m.y - r, 2 * r, 2 * r));
  }
}

// A new picture for a logo: the same size, the pile in the logo's colour,
// filling the part of the picture the logo filled.
std::vector<u8> pile_like(const u8* png, size_t len, int w, int h) {
  const std::vector<u8> old = pixels_of(png, len, w, h);
  double r = 0, g = 0, b = 0, weight = 0;
  int left = w, right = -1, top = -1, bottom = h;  // rows count up from the bottom, as CoreGraphics draws
  for (int row = 0; row < h; row++) {
    for (int col = 0; col < w; col++) {
      const u8* px = &old[((size_t)row * w + col) * 4];
      if (px[3] < 32) continue;
      // The colour from the solid pixels only, not the soft edge.
      if (px[3] >= 240) r += px[0], g += px[1], b += px[2], weight += px[3];
      const int up = h - 1 - row;
      left = std::min(left, col), right = std::max(right, col);
      bottom = std::min(bottom, up), top = std::max(top, up);
    }
  }
  if (right < 0 || weight <= 0) return {};
  std::vector<u8> fresh((size_t)w * h * 4);
  CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
  CGContextRef c = CGBitmapContextCreate(fresh.data(), w, h, 8, (size_t)w * 4, rgb, kCGImageAlphaPremultipliedLast);
  CGContextSetRGBFillColor(c, r / weight, g / weight, b / weight, 1);
  // Standing where the logo stood, centred on it, wider and lower: a pile
  // is wider than it is tall.
  const CGFloat bw = right - left + 1, bh = top - bottom + 1;
  const CGFloat pw = std::min<CGFloat>(w, bw + 4);
  draw_pile(c, CGRectMake(std::max<CGFloat>(0, left + (bw - pw) / 2), bottom, pw, bh * 0.68));
  CGImageRef img = CGBitmapContextCreateImage(c);
  CFMutableDataRef out = CFDataCreateMutable(nullptr, 0);
  CGImageDestinationRef dest = CGImageDestinationCreateWithData(out, CFSTR("public.png"), 1, nullptr);
  CGImageDestinationAddImage(dest, img, nullptr);
  const bool ok = CGImageDestinationFinalize(dest);
  std::vector<u8> bytes;
  if (ok) bytes.assign(CFDataGetBytePtr(out), CFDataGetBytePtr(out) + CFDataGetLength(out));
  CFRelease(dest);
  CFRelease(out);
  CGImageRelease(img);
  CGContextRelease(c);
  CGColorSpaceRelease(rgb);
  return bytes;
}

// Whether the picture is an Apple logo. The only pictures in the file of
// the logos' sizes are the logos and the round clear buttons of search
// fields; the logos' topmost pixels are the tip of the leaf, well right of
// the middle (by 1.5 to 2.5 pixels), where a round button's are centred.
bool is_logo(const u8* png, size_t len, int w, int h) {
  if (h != 22 || (w != 19 && w != 22)) return false;
  const std::vector<u8> px = pixels_of(png, len, w, h);  // top row first
  for (int row = 0; row < h; row++) {
    double sum = 0;
    int n = 0;
    for (int col = 0; col < w; col++) {
      if (px[((size_t)row * w + col) * 4 + 3] >= 96) sum += col, n++;
    }
    if (n) return sum / n > (w - 1) / 2.0 + 1;  // a pixel or more right of the middle
  }
  return false;
}

}  // namespace

std::string menu_logo_file(const std::string& sysroot) { return sysroot + "/.cocaine/art/SArtFile.bin"; }

void prepare_menu_logo(const std::string& sysroot) {
  const std::string made = menu_logo_file(sysroot);
  if (const char* e = getenv("COCAINE_MENU_LOGO"); e && !strcmp(e, "apple")) {
    unlink(made.c_str());  // Apple's own, then
    return;
  }
  std::ifstream in(sysroot + kArtFile, std::ios::binary);
  std::vector<u8> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (d.size() < 8 || le16(d, 0) != 1) return;
  const u32 count = le16(d, 2), data = le32(d, 4);
  if (8 + 4 * (size_t)count > d.size() || data > d.size()) return;
  int swapped = 0;
  for (u32 i = 0; i < count; i++) {
    const u32 entry = le32(d, 8 + 4 * i);
    if ((size_t)entry + 16 > d.size()) return;
    const int w = le16(d, entry + 4), h = le16(d, entry + 6);
    const u32 len = le32(d, entry + 8), off = data + le32(d, entry + 12);
    if ((size_t)off + len > d.size() || len < 8 || memcmp(&d[off], "\x89PNG", 4) != 0) continue;
    if (!is_logo(&d[off], len, w, h)) continue;
    const std::vector<u8> fresh = pile_like(&d[off], len, w, h);
    if (fresh.empty()) continue;
    while (d.size() % 4) d.push_back(0);
    put32(d, entry + 8, (u32)fresh.size());
    put32(d, entry + 12, (u32)(d.size() - data));
    d.insert(d.end(), fresh.begin(), fresh.end());
    log_debug("menu logo: art file image %u (%dx%d) swapped", i, w, h);
    swapped++;
  }
  if (!swapped) return;
  mkdir((sysroot + "/.cocaine").c_str(), 0755);
  mkdir((sysroot + "/.cocaine/art").c_str(), 0755);
  // Written aside and moved into place: another session may be reading it.
  const std::string temp = made + "." + std::to_string(getpid());
  {
    std::ofstream out(temp, std::ios::binary);
    out.write((const char*)d.data(), (std::streamsize)d.size());
    if (!out) {
      unlink(temp.c_str());
      return;
    }
  }
  rename(temp.c_str(), made.c_str());
}
