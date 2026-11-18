// OpenGL for PowerPC guests, done by the Mac's own OpenGL.
//
// Leopard's OpenGL drives graphics cards no Apple Silicon Mac has, so cocaine
// takes over the guest's libGL and AGL (see kernel/hooks.h) and does each
// call with the host's OpenGL instead. Three things differ from simply
// forwarding the calls:
//
//  - Byte order. Guest memory is big-endian. Single values arrive in
//    registers and need nothing, but arrays (matrices, vertex arrays,
//    indices, glGet results) are byte-swapped on the way in or out. Pixel
//    data needs no copy: the host's GL_UNPACK_SWAP_BYTES/GL_PACK_SWAP_BYTES
//    are kept the opposite of what the guest asked for, which reads and
//    writes multi-byte pixels the guest's way and leaves bytes alone.
//
//  - Vertex arrays live in guest memory, where the host can't read them as
//    they are. The pointers are noted, and at each draw the elements it uses
//    are converted into host buffers.
//
//  - Where the picture goes. Each guest context renders into an offscreen
//    framebuffer on the host. aglSwapBuffers reads the frame back and copies
//    it into the guest window's own pixels (the QuickDraw port given to
//    aglSetDrawable), then asks the guest's window server to show it, the
//    way a game that drew in software would. The window server, the viewer
//    and window moves then all just work.

#define GL_SILENCE_DEPRECATION 1

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>

#include <cmath>
#include <ctime>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "kernel/hooks.h"
#include "kernel/process.h"

namespace {

// ---- guest memory -------------------------------------------------------------

// Whether the guest is big-endian (PowerPC). Intel guests share this Mac's
// byte order, and nothing of theirs needs swapping. One CPU per process.
bool g_big = true;

inline u8* gp(GuestCall& c, u32 addr) {
  g_big = c.powerpc;
  return addr ? c.mem.host(addr) : nullptr;
}

inline u16 rd16(const u8* p) { return g_big ? (u16)(p[0] << 8 | p[1]) : (u16)(p[1] << 8 | p[0]); }
inline u32 rd32(const u8* p) {
  return g_big ? (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]
               : (u32)p[3] << 24 | (u32)p[2] << 16 | (u32)p[1] << 8 | p[0];
}
inline u64 rd64(const u8* p) { return g_big ? (u64)rd32(p) << 32 | rd32(p + 4) : (u64)rd32(p + 4) << 32 | rd32(p); }
inline void wr32(u8* p, u32 v) {
  if (!g_big) {
    memcpy(p, &v, 4);
    return;
  }
  p[0] = (u8)(v >> 24);
  p[1] = (u8)(v >> 16);
  p[2] = (u8)(v >> 8);
  p[3] = (u8)v;
}
inline void wr64(u8* p, u64 v) {
  if (!g_big) {
    memcpy(p, &v, 8);
    return;
  }
  wr32(p, (u32)(v >> 32));
  wr32(p + 4, (u32)v);
}
inline float rdf(const u8* p) {
  const u32 v = rd32(p);
  float f;
  memcpy(&f, &v, 4);
  return f;
}
inline double rdd(const u8* p) {
  const u64 v = rd64(p);
  double d;
  memcpy(&d, &v, 8);
  return d;
}

// `n` floats (or doubles, or ints) from the guest.
template <int N>
struct Floats {
  GLfloat v[N] = {};
  Floats(GuestCall& c, u32 addr, int n = N) {
    const u8* p = gp(c, addr);
    for (int k = 0; p && k < n && k < N; k++) v[k] = rdf(p + 4 * k);
  }
};
template <int N>
struct Doubles {
  GLdouble v[N] = {};
  Doubles(GuestCall& c, u32 addr, int n = N) {
    const u8* p = gp(c, addr);
    for (int k = 0; p && k < n && k < N; k++) v[k] = rdd(p + 8 * k);
  }
};

// ---- contexts -----------------------------------------------------------------

struct Array {
  bool on = false;
  GLint size = 4;
  GLenum type = GL_FLOAT;
  GLsizei stride = 0;
  u32 ptr = 0;
  std::vector<u8> host;
};

struct Context {
  u32 handle = 0;
  CGLContextObj cgl = nullptr;
  GLuint fbo = 0, color = 0, depth = 0;
  int width = 0, height = 0;
  u32 port = 0;
  bool unpack_swap = false, pack_swap = false;  // what the guest asked for
  Array vertex, normal, color_array;
  Array tex[8];
  int client_unit = 0;
  std::vector<u8> frame;
  u32 frames = 0;
};

std::mutex g_mu;
std::unordered_map<u32, Context*> g_contexts;
u32 g_next_handle = 0x00C0C000;
thread_local Context* t_current = nullptr;

std::vector<std::string> g_said;
void say_once(const std::string& what) {
  std::lock_guard<std::mutex> lk(g_mu);
  for (const auto& s : g_said) {
    if (s == what) return;
  }
  g_said.push_back(what);
  log_info("%s", what.c_str());
}

Context* context_of(u32 handle) {
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_contexts.find(handle);
  return it == g_contexts.end() ? nullptr : it->second;
}

// The framebuffer the guest draws into, sized to its window.
void size_framebuffer(Context* x, int w, int h) {
  if (w <= 0 || h <= 0) w = h = 1;
  if (x->fbo && w == x->width && h == x->height) return;
  if (!x->fbo) {
    glGenFramebuffersEXT(1, &x->fbo);
    glGenRenderbuffersEXT(1, &x->color);
    glGenRenderbuffersEXT(1, &x->depth);
  }
  glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, x->fbo);
  glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, x->color);
  glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, w, h);
  glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, x->color);
  glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, x->depth);
  glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT, w, h);
  glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, x->depth);
  glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, x->depth);
  const GLenum st = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
  if (st != GL_FRAMEBUFFER_COMPLETE_EXT) log_warn("OpenGL: the offscreen framebuffer is incomplete (0x%x)", st);
  glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
  glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
  x->width = w;
  x->height = h;
  x->frame.resize((size_t)w * h * 4);
}

// The guest window's content size, from its port.
bool port_size(GuestCall& c, u32 port, int& w, int& h) {
  static u32 rect = 0;
  if (!rect) rect = c.scratch(16);
  const u32 fn = c.symbol("GetPortBounds");
  if (!port || !fn || !rect) return false;
  c.call(fn, {port, rect});
  const u8* r = gp(c, rect);
  const int top = (s16)rd16(r), left = (s16)rd16(r + 2), bottom = (s16)rd16(r + 4), right = (s16)rd16(r + 6);
  w = right - left;
  h = bottom - top;
  return w > 0 && h > 0;
}

// The frame, into the guest window's pixels, and onto the screen.
void present(GuestCall& c, Context* x) {
  if (!x->port || !x->fbo) return;
  glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glPixelStorei(GL_PACK_ROW_LENGTH, 0);
  glPixelStorei(GL_PACK_SKIP_ROWS, 0);
  glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
  glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
  GLint read = 0;
  glGetIntegerv(GL_READ_BUFFER, &read);
  glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
  // Packed so that each pixel lands in memory in the guest's own order: A R G B
  // on PowerPC, B G R A on Intel.
  glReadPixels(0, 0, x->width, x->height, GL_BGRA, g_big ? GL_UNSIGNED_INT_8_8_8_8 : GL_UNSIGNED_INT_8_8_8_8_REV,
               x->frame.data());
  glReadBuffer((GLenum)read);
  glPopClientAttrib();

  static u32 lock, unlock, pixmap, base, rowbytes, pixbounds, flush, dirty, rect, whole;
  if (!lock) {
    lock = c.symbol("LockPortBits");
    unlock = c.symbol("UnlockPortBits");
    pixmap = c.symbol("GetPortPixMap");
    base = c.symbol("GetPixBaseAddr");
    rowbytes = c.symbol("GetPixRowBytes");
    pixbounds = c.symbol("GetPixBounds");
    flush = c.symbol("QDFlushPortBuffer");
    dirty = c.symbol("QDAddRectToDirtyRegion");
    rect = c.scratch(16);
    whole = c.scratch(16);
  }
  if (!lock || !pixmap || !base || !rowbytes || !flush) {
    say_once("OpenGL: can't find QuickDraw's port functions, so nothing is shown");
    return;
  }
  c.call(lock, {x->port});
  const u32 pm = c.call(pixmap, {x->port});
  const u32 pixels = c.call(base, {pm});
  const u32 stride = c.call(rowbytes, {pm}) & 0x3FFF;
  int left = 0, top = 0, pw = 0, ph = 0;
  if (pixbounds && rect) {
    c.call(pixbounds, {pm, rect});
    const u8* r = gp(c, rect);
    top = (s16)rd16(r);
    left = (s16)rd16(r + 2);
    ph = (s16)rd16(r + 4) - top;
    pw = (s16)rd16(r + 6) - left;
  }
  // The pixels start at the pixmap's corner, which for a window takes in
  // its title bar; the port's own bounds are where the content is.
  int ctop = 0, cleft = 0;
  if (const u32 portbounds = c.symbol("GetPortBounds"); portbounds && rect) {
    c.call(portbounds, {x->port, rect});
    const u8* r = gp(c, rect);
    ctop = (s16)rd16(r);
    cleft = (s16)rd16(r + 2);
  }
  const int dy = ctop - top, dx = cleft - left;
  if (pixels && stride >= 4 && dx >= 0 && dy >= 0) {
    const int w = pw > 0 ? std::min(pw - dx, x->width) : x->width;
    const int h = ph > 0 ? std::min(ph - dy, x->height) : x->height;
    for (int y = 0; y < h; y++) {
      const u8* src = x->frame.data() + (size_t)(x->height - 1 - y) * x->width * 4;
      const u32 at = pixels + (u32)(y + dy) * stride + (u32)dx * 4;
      memcpy(c.mem.host(at), src, (size_t)std::max(0, std::min<int>(w * 4, (int)stride - dx * 4)));
    }
  }
  top = ctop;
  left = cleft;
  if (unlock) c.call(unlock, {x->port});
  // QuickDraw shows only what it knows changed, and it didn't see these
  // pixels change: say the whole window did.
  if (dirty && whole) {
    u8* r = gp(c, whole);
    const s16 box[4] = {(s16)top, (s16)left, (s16)(top + x->height), (s16)(left + x->width)};
    for (int k = 0; k < 4; k++) {
      r[2 * k] = (u8)((u16)box[k] >> 8);
      r[2 * k + 1] = (u8)box[k];
    }
    c.call(dirty, {x->port, whole});
  }
  c.call(flush, {x->port, 0});
  if (++x->frames == 1) say_once("OpenGL: first frame shown");
  if (x->frames % 300 == 0) log_info("OpenGL: %u frames shown", x->frames);
  // COCAINE_GL_FRAME=n:file, for working on cocaine: frame n as cocaine read it back, raw A R G B.
  if (const char* e = getenv("COCAINE_GL_FRAME")) {
    if (strtoul(e, nullptr, 10) == x->frames) {
      if (const char* colon = strchr(e, ':')) {
        if (FILE* f = fopen(colon + 1, "wb")) {
          fwrite(x->frame.data(), 1, x->frame.size(), f);
          fclose(f);
        }
      }
    }
  }
}

// ---- AGL ------------------------------------------------------------------------

void aglChoosePixelFormat(GuestCall& c) {
  g_big = c.powerpc;
  c.i();
  c.i();
  const u32 attribs = c.i();
  std::string list;
  for (const u8* p = gp(c, attribs); p && rd32(p) != 0 && list.size() < 200; p += 4) list += " " + std::to_string(rd32(p));
  log_debug("aglChoosePixelFormat:%s", list.c_str());
  c.ret(0x00C0F000);  // any pixel format the guest asks for, the host's will do
}

void aglCreateContext(GuestCall& c) {
  g_big = c.powerpc;
  c.i();
  const u32 share = c.i();
  const CGLPixelFormatAttribute attrs[] = {
      kCGLPFAAccelerated, kCGLPFAColorSize, (CGLPixelFormatAttribute)24, kCGLPFAAlphaSize,
      (CGLPixelFormatAttribute)8, kCGLPFADepthSize, (CGLPixelFormatAttribute)24, kCGLPFAStencilSize,
      (CGLPixelFormatAttribute)8, kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_Legacy,
      (CGLPixelFormatAttribute)0};
  CGLPixelFormatObj pf = nullptr;
  GLint n = 0;
  if (CGLChoosePixelFormat(attrs, &pf, &n) != kCGLNoError || !pf) {
    log_warn("OpenGL: the Mac offered no pixel format");
    c.ret(0);
    return;
  }
  Context* shared = share ? context_of(share) : nullptr;
  auto* x = new Context;
  const CGLError e = CGLCreateContext(pf, shared ? shared->cgl : nullptr, &x->cgl);
  CGLReleasePixelFormat(pf);
  if (e != kCGLNoError) {
    log_warn("OpenGL: the Mac would not make a context (%d)", (int)e);
    delete x;
    c.ret(0);
    return;
  }
  std::lock_guard<std::mutex> lk(g_mu);
  x->handle = g_next_handle += 0x10;
  g_contexts[x->handle] = x;
  c.ret(x->handle);
}

void make_current(Context* x) {
  t_current = x;
  CGLSetCurrentContext(x ? x->cgl : nullptr);
  if (!x) return;
  if (!x->fbo) {
    size_framebuffer(x, x->width, x->height);
    glPixelStorei(GL_UNPACK_SWAP_BYTES, g_big);  // big-endian pixels read the guest's way
    glPixelStorei(GL_PACK_SWAP_BYTES, g_big);
  }
  glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, x->fbo);
}

void aglSetCurrentContext(GuestCall& c) {
  const u32 h = c.i();
  Context* x = h ? context_of(h) : nullptr;
  make_current(x);
  c.ret(h == 0 || x != nullptr);
}

void aglGetCurrentContext(GuestCall& c) { c.ret(t_current ? t_current->handle : 0); }

void aglDestroyContext(GuestCall& c) {
  const u32 h = c.i();
  Context* x = context_of(h);
  if (x) {
    if (t_current == x) make_current(nullptr);
    // Renderbuffers belong to every context sharing with this one, so going
    // with the context isn't enough to free them: they are freed by name.
    if (x->fbo) {
      const CGLContextObj before = CGLGetCurrentContext();
      CGLSetCurrentContext(x->cgl);
      glDeleteFramebuffersEXT(1, &x->fbo);
      glDeleteRenderbuffersEXT(1, &x->color);
      glDeleteRenderbuffersEXT(1, &x->depth);
      CGLSetCurrentContext(before);
    }
    CGLDestroyContext(x->cgl);
    std::lock_guard<std::mutex> lk(g_mu);
    g_contexts.erase(h);
    delete x;
  }
  c.ret(1);
}

void resize_to_port(GuestCall& c, Context* x) {
  int w = 0, h = 0;
  if (!port_size(c, x->port, w, h)) return;
  CGLContextObj before = CGLGetCurrentContext();
  CGLSetCurrentContext(x->cgl);
  size_framebuffer(x, w, h);
  // The byte swapping is this context's own state, and it is the current one
  // just now, whichever was before: make_current won't set it once the
  // framebuffer exists.
  glPixelStorei(GL_UNPACK_SWAP_BYTES, g_big != x->unpack_swap);
  glPixelStorei(GL_PACK_SWAP_BYTES, g_big != x->pack_swap);
  CGLSetCurrentContext(before);
  if (t_current) glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, t_current->fbo);
}

void aglSetDrawable(GuestCall& c) {
  Context* x = context_of(c.i());
  const u32 port = c.i();
  if (x) {
    x->port = port;
    if (port) resize_to_port(c, x);
    log_debug("aglSetDrawable: port 0x%x, %d x %d", port, x->width, x->height);
  }
  c.ret(x != nullptr);
}

void aglUpdateContext(GuestCall& c) {
  Context* x = context_of(c.i());
  if (x && x->port) resize_to_port(c, x);
  c.ret(x != nullptr);
}

// Frames are shown at most this often (COCAINE_GL_FPS, 60 unless set): a
// Mac of the time held a game to the display's refresh, and frames past what
// the screen shows only cost power, twice over, since the guest's window
// server composites each one.
void pace() {
  static const double interval = [] {
    double fps = 60;
    if (const char* e = getenv("COCAINE_GL_FPS")) fps = atof(e);
    return fps >= 1 ? 1e9 / fps : 0.0;
  }();
  if (interval <= 0) return;
  static thread_local u64 next = 0;
  const u64 now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  if (next > now && next - now < 1000000000ull) {
    struct timespec ts = {0, (long)(next - now)};
    nanosleep(&ts, nullptr);
    next += (u64)interval;
  } else {
    next = now + (u64)interval;  // late, or the first frame: start the beat from here
  }
}

void aglSwapBuffers(GuestCall& c) {
  Context* x = context_of(c.i());
  if (x && x == t_current) {
    pace();
    present(c, x);
  }
  c.ret(0);
}

void aglSetFullScreen(GuestCall& c) {
  say_once("OpenGL: full screen is not done yet; set the game to play in a window");
  c.ret(0);
}

void agl_true(GuestCall& c) { c.ret(1); }
void agl_zero(GuestCall& c) { c.ret(0); }

void aglErrorString(GuestCall& c) {
  static u32 s = 0;
  if (!s) {
    s = c.scratch(16);
    if (s) memcpy(c.mem.host(s), "no error", 9);
  }
  c.ret(s);
}

// ---- GL: state ----------------------------------------------------------------

void glEnable_(GuestCall& c) { glEnable(c.i()); }
void glDisable_(GuestCall& c) { glDisable(c.i()); }
void glIsEnabled_(GuestCall& c) { c.ret(glIsEnabled(c.i())); }
void glClear_(GuestCall& c) { glClear(c.i()); }
void glClearColor_(GuestCall& c) {
  const float r = c.f(), g = c.f(), b = c.f(), a = c.f();
  glClearColor(r, g, b, a);
}
void glViewport_(GuestCall& c) {
  const s32 x = (s32)c.i(), y = (s32)c.i(), w = (s32)c.i(), h = (s32)c.i();
  glViewport(x, y, w, h);
}
void glBlendFunc_(GuestCall& c) {
  const GLenum s = c.i(), d = c.i();
  glBlendFunc(s, d);
}
void glAlphaFunc_(GuestCall& c) {
  const GLenum f = c.i();
  glAlphaFunc(f, c.f());
}
void glCullFace_(GuestCall& c) { glCullFace(c.i()); }
void glFrontFace_(GuestCall& c) { glFrontFace(c.i()); }
void glDepthFunc_(GuestCall& c) { glDepthFunc(c.i()); }
void glDepthMask_(GuestCall& c) { glDepthMask((GLboolean)c.i()); }
void glColorMask_(GuestCall& c) {
  const GLboolean r = (GLboolean)c.i(), g = (GLboolean)c.i(), b = (GLboolean)c.i(), a = (GLboolean)c.i();
  glColorMask(r, g, b, a);
}
void glStencilFunc_(GuestCall& c) {
  const GLenum f = c.i();
  const GLint ref = (GLint)c.i();
  glStencilFunc(f, ref, c.i());
}
void glStencilMask_(GuestCall& c) { glStencilMask(c.i()); }
void glStencilOp_(GuestCall& c) {
  const GLenum a = c.i(), b = c.i(), d = c.i();
  glStencilOp(a, b, d);
}
void glPolygonMode_(GuestCall& c) {
  const GLenum f = c.i();
  glPolygonMode(f, c.i());
}
void glPolygonOffset_(GuestCall& c) {
  const float f = c.f(), u = c.f();
  glPolygonOffset(f, u);
}
void glLineWidth_(GuestCall& c) { glLineWidth(c.f()); }
void glHint_(GuestCall& c) {
  const GLenum t = c.i();
  glHint(t, c.i());
}
void glPushAttrib_(GuestCall& c) { glPushAttrib(c.i()); }
void glPopAttrib_(GuestCall& c) { glPopAttrib(); }
void glFinish_(GuestCall& c) { glFinish(); }
void glGetError_(GuestCall& c) { c.ret(glGetError()); }

// The window's front and back buffers are one offscreen framebuffer here.
GLenum buffer(GLenum b) {
  return (b == GL_BACK || b == GL_FRONT || b == GL_FRONT_AND_BACK || b == GL_BACK_LEFT || b == GL_FRONT_LEFT)
             ? GL_COLOR_ATTACHMENT0_EXT
             : b;
}
void glDrawBuffer_(GuestCall& c) { glDrawBuffer(buffer(c.i())); }
void glReadBuffer_(GuestCall& c) { glReadBuffer(buffer(c.i())); }

void glPixelStorei_(GuestCall& c) {
  const GLenum p = c.i();
  const GLint v = (GLint)c.i();
  if (p == GL_UNPACK_SWAP_BYTES || p == GL_PACK_SWAP_BYTES) {
    if (t_current) (p == GL_UNPACK_SWAP_BYTES ? t_current->unpack_swap : t_current->pack_swap) = v != 0;
    glPixelStorei(p, g_big != (v != 0));
    return;
  }
  glPixelStorei(p, v);
}

// glGet: the host's answer, back to the guest in its byte order. However
// many values the name has, the host writes them; the ones it didn't touch
// still hold the marker.
template <typename T>
int written(const T* v, T marker) {
  int n = 16;
  while (n > 0 && memcmp(&v[n - 1], &marker, sizeof(T)) == 0) n--;
  return n > 0 ? n : 1;
}
GLenum guest_buffer(GLenum b) { return b == GL_COLOR_ATTACHMENT0_EXT ? GL_BACK : b; }
void glGetIntegerv_(GuestCall& c) {
  const GLenum p = c.i();
  u8* out = gp(c, c.i());
  GLint v[16];
  const GLint marker = 0x7A5A7A5A;
  for (GLint& x : v) x = marker;
  glGetIntegerv(p, v);
  if (p == GL_DRAW_BUFFER || p == GL_READ_BUFFER) v[0] = (GLint)guest_buffer((GLenum)v[0]);
  if (p == GL_UNPACK_SWAP_BYTES) v[0] = t_current && t_current->unpack_swap;
  if (p == GL_PACK_SWAP_BYTES) v[0] = t_current && t_current->pack_swap;
  const int n = written(v, marker);
  for (int k = 0; out && k < n; k++) wr32(out + 4 * k, (u32)v[k]);
}
void glGetFloatv_(GuestCall& c) {
  const GLenum p = c.i();
  u8* out = gp(c, c.i());
  GLfloat v[16];
  u32 bits = 0x7FA5A5A5;
  GLfloat marker;
  memcpy(&marker, &bits, 4);
  for (GLfloat& x : v) x = marker;
  glGetFloatv(p, v);
  const int n = written(v, marker);
  for (int k = 0; out && k < n; k++) {
    u32 w;
    memcpy(&w, &v[k], 4);
    wr32(out + 4 * k, w);
  }
}
void glGetDoublev_(GuestCall& c) {
  const GLenum p = c.i();
  u8* out = gp(c, c.i());
  GLdouble v[16];
  u64 bits = 0x7FF5A5A5A5A5A5A5ull;
  GLdouble marker;
  memcpy(&marker, &bits, 8);
  for (GLdouble& x : v) x = marker;
  glGetDoublev(p, v);
  const int n = written(v, marker);
  for (int k = 0; out && k < n; k++) {
    u64 w;
    memcpy(&w, &v[k], 8);
    wr64(out + 8 * k, w);
  }
}

// The extensions offered: the host's, less any whose functions cocaine
// doesn't pass through.
const char* const kExtensions[] = {
    "GL_ARB_multitexture", "GL_EXT_compiled_vertex_array", "GL_ARB_texture_env_combine",
    "GL_EXT_texture_env_combine", "GL_ARB_texture_env_add", "GL_EXT_texture_env_add", "GL_ARB_texture_env_dot3",
    "GL_EXT_texture_filter_anisotropic", "GL_SGIS_generate_mipmap", "GL_EXT_abgr", "GL_EXT_bgra",
    "GL_ARB_texture_border_clamp", "GL_EXT_texture_edge_clamp", "GL_SGIS_texture_edge_clamp", "GL_EXT_stencil_wrap",
    "GL_ARB_texture_cube_map", "GL_EXT_separate_specular_color", "GL_EXT_packed_pixels", "GL_APPLE_packed_pixels",
    "GL_EXT_texture_lod_bias", "GL_EXT_rescale_normal", "GL_ARB_texture_non_power_of_two",
};
void glGetString_(GuestCall& c) {
  const GLenum name = c.i();
  static std::mutex mu;
  static std::unordered_map<GLenum, u32> made;
  std::lock_guard<std::mutex> lk(mu);
  auto it = made.find(name);
  if (it != made.end()) {
    c.ret(it->second);
    return;
  }
  const char* host = (const char*)glGetString(name);
  if (!host) {  // no context yet: nothing to say, and nothing to remember
    c.ret(0);
    return;
  }
  std::string s = host;
  if (name == GL_EXTENSIONS) {
    std::string kept;
    for (const char* e : kExtensions) {
      const std::string padded = " " + s + " ";
      if (padded.find(std::string(" ") + e + " ") != std::string::npos) kept += std::string(e) + " ";
    }
    s = kept;
  } else if (name == GL_RENDERER) {
    s += " (through cocaine)";
  } else if (name == GL_VERSION) {
    s = "1.5 " + s;  // what the game can use; the rest it can't reach anyway
  }
  const u32 at = c.scratch((u32)s.size() + 1);
  if (at) memcpy(c.mem.host(at), s.c_str(), s.size() + 1);
  made[name] = at;
  c.ret(at);
}

// ---- GL: matrices -------------------------------------------------------------

void glMatrixMode_(GuestCall& c) { glMatrixMode(c.i()); }
void glLoadIdentity_(GuestCall& c) { glLoadIdentity(); }
void glPushMatrix_(GuestCall& c) { glPushMatrix(); }
void glPopMatrix_(GuestCall& c) { glPopMatrix(); }
void glLoadMatrixf_(GuestCall& c) { glLoadMatrixf(Floats<16>(c, c.i()).v); }
void glMultMatrixf_(GuestCall& c) { glMultMatrixf(Floats<16>(c, c.i()).v); }
void glTranslatef_(GuestCall& c) {
  const float x = c.f(), y = c.f(), z = c.f();
  glTranslatef(x, y, z);
}
void glScalef_(GuestCall& c) {
  const float x = c.f(), y = c.f(), z = c.f();
  glScalef(x, y, z);
}
void glRotatef_(GuestCall& c) {
  const float a = c.f(), x = c.f(), y = c.f(), z = c.f();
  glRotatef(a, x, y, z);
}
void glOrtho_(GuestCall& c) {
  const double l = c.d(), r = c.d(), b = c.d(), t = c.d(), n = c.d(), f = c.d();
  glOrtho(l, r, b, t, n, f);
}
void glFrustum_(GuestCall& c) {
  const double l = c.d(), r = c.d(), b = c.d(), t = c.d(), n = c.d(), f = c.d();
  glFrustum(l, r, b, t, n, f);
}
void glClipPlane_(GuestCall& c) {
  const GLenum p = c.i();
  glClipPlane(p, Doubles<4>(c, c.i()).v);
}

// ---- GL: immediate mode -------------------------------------------------------

void glBegin_(GuestCall& c) { glBegin(c.i()); }
void glEnd_(GuestCall& c) { glEnd(); }
void glVertex2f_(GuestCall& c) {
  const float x = c.f(), y = c.f();
  glVertex2f(x, y);
}
void glVertex2i_(GuestCall& c) {
  const s32 x = (s32)c.i(), y = (s32)c.i();
  glVertex2i(x, y);
}
void glVertex3f_(GuestCall& c) {
  const float x = c.f(), y = c.f(), z = c.f();
  glVertex3f(x, y, z);
}
void glVertex2fv_(GuestCall& c) { glVertex2fv(Floats<2>(c, c.i()).v); }
void glVertex3fv_(GuestCall& c) { glVertex3fv(Floats<3>(c, c.i()).v); }
void glTexCoord2f_(GuestCall& c) {
  const float s = c.f(), t = c.f();
  glTexCoord2f(s, t);
}
void glColor3f_(GuestCall& c) {
  const float r = c.f(), g = c.f(), b = c.f();
  glColor3f(r, g, b);
}
void glColor4f_(GuestCall& c) {
  const float r = c.f(), g = c.f(), b = c.f(), a = c.f();
  glColor4f(r, g, b, a);
}
void glColor4fv_(GuestCall& c) { glColor4fv(Floats<4>(c, c.i()).v); }
void glColor3i_(GuestCall& c) {
  const s32 r = (s32)c.i(), g = (s32)c.i(), b = (s32)c.i();
  glColor3i(r, g, b);
}
void glColor3ub_(GuestCall& c) {
  const GLubyte r = (GLubyte)c.i(), g = (GLubyte)c.i(), b = (GLubyte)c.i();
  glColor3ub(r, g, b);
}
void glColor4ub_(GuestCall& c) {
  const GLubyte r = (GLubyte)c.i(), g = (GLubyte)c.i(), b = (GLubyte)c.i(), a = (GLubyte)c.i();
  glColor4ub(r, g, b, a);
}
void glColor3ubv_(GuestCall& c) {
  const u8* p = gp(c, c.i());
  if (p) glColor3ubv(p);
}
void glColor4ubv_(GuestCall& c) {
  const u8* p = gp(c, c.i());
  if (p) glColor4ubv(p);
}
void glRecti_(GuestCall& c) {
  const s32 a = (s32)c.i(), b = (s32)c.i(), d = (s32)c.i(), e = (s32)c.i();
  glRecti(a, b, d, e);
}

// ---- GL: lighting, fog, texture environment -----------------------------------

int params_of(GLenum pname) {
  switch (pname) {
    case GL_AMBIENT: case GL_DIFFUSE: case GL_SPECULAR: case GL_POSITION: case GL_EMISSION:
    case GL_AMBIENT_AND_DIFFUSE: case GL_FOG_COLOR: case GL_TEXTURE_ENV_COLOR: case GL_OBJECT_PLANE:
    case GL_EYE_PLANE: case GL_LIGHT_MODEL_AMBIENT: case GL_TEXTURE_BORDER_COLOR:
      return 4;
    case GL_SPOT_DIRECTION: case GL_COLOR_INDEXES:
      return 3;
    default:
      return 1;
  }
}
void glFogf_(GuestCall& c) {
  const GLenum p = c.i();
  glFogf(p, c.f());
}
void glFogi_(GuestCall& c) {
  const GLenum p = c.i();
  glFogi(p, (GLint)c.i());
}
void glFogfv_(GuestCall& c) {
  const GLenum p = c.i();
  glFogfv(p, Floats<4>(c, c.i(), params_of(p)).v);
}
void glLightf_(GuestCall& c) {
  const GLenum l = c.i(), p = c.i();
  glLightf(l, p, c.f());
}
void glLightfv_(GuestCall& c) {
  const GLenum l = c.i(), p = c.i();
  glLightfv(l, p, Floats<4>(c, c.i(), params_of(p)).v);
}
void glLightModelfv_(GuestCall& c) {
  const GLenum p = c.i();
  glLightModelfv(p, Floats<4>(c, c.i(), params_of(p)).v);
}
void glMaterialfv_(GuestCall& c) {
  const GLenum f = c.i(), p = c.i();
  glMaterialfv(f, p, Floats<4>(c, c.i(), params_of(p)).v);
}
void glTexEnvf_(GuestCall& c) {
  const GLenum t = c.i(), p = c.i();
  glTexEnvf(t, p, c.f());
}
void glTexEnvi_(GuestCall& c) {
  const GLenum t = c.i(), p = c.i();
  glTexEnvi(t, p, (GLint)c.i());
}
void glTexEnvfv_(GuestCall& c) {
  const GLenum t = c.i(), p = c.i();
  glTexEnvfv(t, p, Floats<4>(c, c.i(), params_of(p)).v);
}
void glGetTexEnviv_(GuestCall& c) {
  const GLenum t = c.i(), p = c.i();
  u8* out = gp(c, c.i());
  GLint v[4] = {};
  glGetTexEnviv(t, p, v);
  for (int k = 0; out && k < params_of(p); k++) wr32(out + 4 * k, (u32)v[k]);
}
void glTexGeni_(GuestCall& c) {
  const GLenum t = c.i(), p = c.i();
  glTexGeni(t, p, (GLint)c.i());
}
void glTexGenfv_(GuestCall& c) {
  const GLenum t = c.i(), p = c.i();
  glTexGenfv(t, p, Floats<4>(c, c.i(), params_of(p)).v);
}

// ---- GL: textures ---------------------------------------------------------------

void glGenTextures_(GuestCall& c) {
  const s32 n = (s32)c.i();
  u8* out = gp(c, c.i());
  if (n <= 0 || !out) return;
  std::vector<GLuint> v((size_t)n);
  glGenTextures(n, v.data());
  for (s32 k = 0; k < n; k++) wr32(out + 4 * k, v[(size_t)k]);
}
void glDeleteTextures_(GuestCall& c) {
  const s32 n = (s32)c.i();
  const u8* in = gp(c, c.i());
  if (n <= 0 || !in) return;
  std::vector<GLuint> v((size_t)n);
  for (s32 k = 0; k < n; k++) v[(size_t)k] = rd32(in + 4 * k);
  glDeleteTextures(n, v.data());
}
void glBindTexture_(GuestCall& c) {
  const GLenum t = c.i();
  glBindTexture(t, c.i());
}
void glTexParameteri_(GuestCall& c) {
  const GLenum t = c.i(), p = c.i();
  glTexParameteri(t, p, (GLint)c.i());
}
void glTexParameterf_(GuestCall& c) {
  const GLenum t = c.i(), p = c.i();
  glTexParameterf(t, p, c.f());
}
void glTexImage2D_(GuestCall& c) {
  const GLenum target = c.i();
  const GLint level = (GLint)c.i(), internal = (GLint)c.i();
  const GLsizei w = (GLsizei)c.i(), h = (GLsizei)c.i();
  const GLint border = (GLint)c.i();
  const GLenum format = c.i(), type = c.i();
  const u32 pixels = c.i();
  glTexImage2D(target, level, internal, w, h, border, format, type, gp(c, pixels));
}
void glTexSubImage2D_(GuestCall& c) {
  const GLenum target = c.i();
  const GLint level = (GLint)c.i(), x = (GLint)c.i(), y = (GLint)c.i();
  const GLsizei w = (GLsizei)c.i(), h = (GLsizei)c.i();
  const GLenum format = c.i(), type = c.i();
  const u32 pixels = c.i();
  glTexSubImage2D(target, level, x, y, w, h, format, type, gp(c, pixels));
}
void glReadPixels_(GuestCall& c) {
  const GLint x = (GLint)c.i(), y = (GLint)c.i();
  const GLsizei w = (GLsizei)c.i(), h = (GLsizei)c.i();
  const GLenum format = c.i(), type = c.i();
  glReadPixels(x, y, w, h, format, type, gp(c, c.i()));
}
void glActiveTextureARB_(GuestCall& c) { glActiveTextureARB(c.i()); }

// ---- GL: vertex arrays ------------------------------------------------------------

int type_size(GLenum t) {
  switch (t) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    case GL_DOUBLE: return 8;
    default: return 4;
  }
}
void set_pointer(GuestCall& c, Array& a, bool has_size, GLint fixed_size) {
  a.size = has_size ? (GLint)c.i() : fixed_size;
  a.type = c.i();
  a.stride = (GLsizei)c.i();
  a.ptr = c.i();
}
void glVertexPointer_(GuestCall& c) {
  if (t_current) set_pointer(c, t_current->vertex, true, 0);
}
void glNormalPointer_(GuestCall& c) {
  if (t_current) set_pointer(c, t_current->normal, false, 3);
}
void glColorPointer_(GuestCall& c) {
  if (t_current) set_pointer(c, t_current->color_array, true, 0);
}
void glTexCoordPointer_(GuestCall& c) {
  if (t_current) set_pointer(c, t_current->tex[t_current->client_unit & 7], true, 0);
}
Array* array_for(GLenum cap) {
  if (!t_current) return nullptr;
  switch (cap) {
    case GL_VERTEX_ARRAY: return &t_current->vertex;
    case GL_NORMAL_ARRAY: return &t_current->normal;
    case GL_COLOR_ARRAY: return &t_current->color_array;
    case GL_TEXTURE_COORD_ARRAY: return &t_current->tex[t_current->client_unit & 7];
    default: return nullptr;
  }
}
void glEnableClientState_(GuestCall& c) {
  const GLenum cap = c.i();
  if (Array* a = array_for(cap)) a->on = true;
  glEnableClientState(cap);
}
void glDisableClientState_(GuestCall& c) {
  const GLenum cap = c.i();
  if (Array* a = array_for(cap)) a->on = false;
  glDisableClientState(cap);
}
void glClientActiveTextureARB_(GuestCall& c) {
  const GLenum unit = c.i();
  if (t_current) t_current->client_unit = (int)(unit - GL_TEXTURE0);
  glClientActiveTextureARB(unit);
}

// Elements lo..hi of an array, converted for the host, at the same indices.
const void* convert(GuestCall& c, Array& a, u32 lo, u32 hi) {
  const int ts = type_size(a.type), es = a.size * ts;
  const u32 stride = a.stride ? (u32)a.stride : (u32)es;
  a.host.resize((size_t)(hi + 1) * (size_t)es);
  for (u32 i = lo; i <= hi; i++) {
    const u8* src = gp(c, a.ptr + i * stride);
    u8* dst = a.host.data() + (size_t)i * (size_t)es;
    if (!g_big) {  // the host's order already
      memcpy(dst, src, (size_t)es);
      continue;
    }
    for (int k = 0; k < a.size; k++, src += ts, dst += ts) {
      switch (ts) {
        case 1: dst[0] = src[0]; break;
        case 2: dst[0] = src[1]; dst[1] = src[0]; break;
        case 4: dst[0] = src[3]; dst[1] = src[2]; dst[2] = src[1]; dst[3] = src[0]; break;
        default: for (int b = 0; b < 8; b++) dst[b] = src[7 - b]; break;
      }
    }
  }
  return a.host.data();
}
void prepare_arrays(GuestCall& c, u32 lo, u32 hi) {
  Context* x = t_current;
  if (!x) return;
  if (x->vertex.on && x->vertex.ptr) glVertexPointer(x->vertex.size, x->vertex.type, 0, convert(c, x->vertex, lo, hi));
  if (x->normal.on && x->normal.ptr) glNormalPointer(x->normal.type, 0, convert(c, x->normal, lo, hi));
  if (x->color_array.on && x->color_array.ptr)
    glColorPointer(x->color_array.size, x->color_array.type, 0, convert(c, x->color_array, lo, hi));
  bool switched = false;
  for (int u = 0; u < 8; u++) {
    Array& a = x->tex[u];
    if (!a.on || !a.ptr) continue;
    glClientActiveTextureARB(GL_TEXTURE0 + (GLenum)u);
    switched = true;
    glTexCoordPointer(a.size, a.type, 0, convert(c, a, lo, hi));
  }
  if (switched) glClientActiveTextureARB(GL_TEXTURE0 + (GLenum)x->client_unit);
}
void glDrawArrays_(GuestCall& c) {
  const GLenum mode = c.i();
  const GLint first = (GLint)c.i();
  const GLsizei count = (GLsizei)c.i();
  if (count <= 0 || first < 0) return;
  prepare_arrays(c, (u32)first, (u32)(first + count - 1));
  glDrawArrays(mode, first, count);
}
void glDrawElements_(GuestCall& c) {
  const GLenum mode = c.i();
  const GLsizei count = (GLsizei)c.i();
  const GLenum type = c.i();
  const u8* in = gp(c, c.i());
  if (count <= 0 || !in) return;
  static thread_local std::vector<GLuint> idx;
  idx.resize((size_t)count);
  u32 lo = 0xFFFFFFFFu, hi = 0;
  for (GLsizei k = 0; k < count; k++) {
    const u32 v = type == GL_UNSIGNED_BYTE ? in[k] : type == GL_UNSIGNED_SHORT ? rd16(in + 2 * k) : rd32(in + 4 * k);
    idx[(size_t)k] = v;
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  prepare_arrays(c, lo, hi);
  glDrawElements(mode, count, GL_UNSIGNED_INT, idx.data());
}
// One element of each enabled array, as immediate mode would give it.
void glArrayElement_(GuestCall& c) {
  Context* x = t_current;
  const u32 i = c.i();
  if (!x) return;
  // Integers are scaled to [0, 1] or [-1, 1] only where GL scales them:
  // colours of every integer type, and normals (whose types are all
  // signed). Vertices and texture coordinates keep their integer values.
  auto get = [&](const Array& a, bool scaled, GLfloat out[4]) {
    const int ts = type_size(a.type);
    const u32 stride = a.stride ? (u32)a.stride : (u32)(a.size * ts);
    const u8* p = gp(c, a.ptr + i * stride);
    for (int k = 0; k < 4; k++) out[k] = k == 3 ? 1.0f : 0.0f;
    for (int k = 0; k < a.size && k < 4; k++, p += ts) {
      switch (a.type) {
        case GL_UNSIGNED_BYTE: out[k] = scaled ? p[0] / 255.0f : p[0]; break;
        case GL_BYTE: out[k] = scaled ? (s8)p[0] / 127.0f : (s8)p[0]; break;
        case GL_UNSIGNED_SHORT: out[k] = scaled ? rd16(p) / 65535.0f : rd16(p); break;
        case GL_SHORT: out[k] = scaled ? (s16)rd16(p) / 32767.0f : (s16)rd16(p); break;
        case GL_UNSIGNED_INT: out[k] = (float)(scaled ? rd32(p) / 4294967295.0 : (double)rd32(p)); break;
        case GL_INT: out[k] = (float)(scaled ? (s32)rd32(p) / 2147483647.0 : (double)(s32)rd32(p)); break;
        case GL_DOUBLE: out[k] = (float)rdd(p); break;
        default: out[k] = rdf(p); break;
      }
    }
  };
  GLfloat v[4];
  for (int u = 0; u < 8; u++) {
    if (!x->tex[u].on || !x->tex[u].ptr) continue;
    get(x->tex[u], false, v);
    glMultiTexCoord4fv(GL_TEXTURE0 + (GLenum)u, v);
  }
  if (x->color_array.on && x->color_array.ptr) {
    get(x->color_array, true, v);
    glColor4fv(v);
  }
  if (x->normal.on && x->normal.ptr) {
    get(x->normal, true, v);
    glNormal3fv(v);
  }
  if (x->vertex.on && x->vertex.ptr) {
    get(x->vertex, false, v);
    if (x->vertex.size == 2) v[2] = 0;
    glVertex4fv(v);
  }
}
void glLockArraysEXT_(GuestCall& c) {}  // the arrays are copied at each draw anyway
void glUnlockArraysEXT_(GuestCall& c) {}

// ---- the table --------------------------------------------------------------------

#define H(name) {#name, name##_}
const HookSpec kGL[] = {
    H(glEnable), H(glDisable), H(glIsEnabled), H(glClear), H(glClearColor), H(glViewport), H(glBlendFunc),
    H(glAlphaFunc), H(glCullFace), H(glFrontFace), H(glDepthFunc), H(glDepthMask), H(glColorMask),
    H(glStencilFunc), H(glStencilMask), H(glStencilOp), H(glPolygonMode), H(glPolygonOffset), H(glLineWidth),
    H(glHint), H(glPushAttrib), H(glPopAttrib), H(glFinish), H(glGetError), H(glDrawBuffer), H(glReadBuffer),
    H(glPixelStorei), H(glGetIntegerv), H(glGetFloatv), H(glGetDoublev), H(glGetString), H(glMatrixMode),
    H(glLoadIdentity), H(glPushMatrix), H(glPopMatrix), H(glLoadMatrixf), H(glMultMatrixf), H(glTranslatef),
    H(glScalef), H(glRotatef), H(glOrtho), H(glFrustum), H(glClipPlane), H(glBegin), H(glEnd), H(glVertex2f),
    H(glVertex2i), H(glVertex3f), H(glVertex2fv), H(glVertex3fv), H(glTexCoord2f), H(glColor3f), H(glColor4f),
    H(glColor4fv), H(glColor3i), H(glColor3ub), H(glColor4ub), H(glColor3ubv), H(glColor4ubv), H(glRecti),
    H(glFogf), H(glFogi), H(glFogfv), H(glLightf), H(glLightfv), H(glLightModelfv), H(glMaterialfv),
    H(glTexEnvf), H(glTexEnvi), H(glTexEnvfv), H(glGetTexEnviv), H(glTexGeni), H(glTexGenfv), H(glGenTextures),
    H(glDeleteTextures), H(glBindTexture), H(glTexParameteri), H(glTexParameterf), H(glTexImage2D),
    H(glTexSubImage2D), H(glReadPixels), H(glActiveTextureARB), H(glVertexPointer), H(glNormalPointer),
    H(glColorPointer), H(glTexCoordPointer), H(glEnableClientState), H(glDisableClientState),
    H(glClientActiveTextureARB), H(glDrawArrays), H(glDrawElements), H(glArrayElement), H(glLockArraysEXT),
    H(glUnlockArraysEXT),
};
#undef H

const HookSpec kAGL[] = {
    {"aglChoosePixelFormat", aglChoosePixelFormat},
    {"aglDestroyPixelFormat", agl_true},
    {"aglCreateContext", aglCreateContext},
    {"aglDestroyContext", aglDestroyContext},
    {"aglSetCurrentContext", aglSetCurrentContext},
    {"aglGetCurrentContext", aglGetCurrentContext},
    {"aglSetDrawable", aglSetDrawable},
    {"aglUpdateContext", aglUpdateContext},
    {"aglSwapBuffers", aglSwapBuffers},
    {"aglSetFullScreen", aglSetFullScreen},
    {"aglSetInteger", agl_true},
    {"aglConfigure", agl_true},
    {"aglGetError", agl_zero},
    {"aglErrorString", aglErrorString},
};

const HookLibrary kLibraries[] = {
    {"/OpenGL.framework/Versions/A/Libraries/libGL.dylib", "gl", kGL, sizeof kGL / sizeof kGL[0], true},
    {"/AGL.framework/Versions/A/AGL", "agl", kAGL, sizeof kAGL / sizeof kAGL[0], true},
};

}  // namespace

const HookLibrary* gl_hook_libraries(size_t& n) {
  n = sizeof kLibraries / sizeof kLibraries[0];
  return kLibraries;
}
