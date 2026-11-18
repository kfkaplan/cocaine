// OpenAL for PowerPC guests, done by the Mac's own OpenAL.
//
// Games of the time shipped OpenAL inside their bundles, built on Core Audio
// drivers cocaine doesn't have. So, as with OpenGL (src/gl/gl.cpp), cocaine
// takes over the guest's OpenAL and makes each call on the host's. Sources
// and buffers are numbers and pass straight through; devices and contexts
// are pointers, so the guest gets stand-in handles. What needs care is byte
// order: 16-bit samples, attribute lists and the arrays some calls take or
// fill are big-endian in guest memory.

#define OPENAL_DEPRECATED
#include <OpenAL/al.h>
#include <OpenAL/alc.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "kernel/hooks.h"
#include "kernel/process.h"

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace {

// Whether the guest is big-endian (PowerPC); Intel guests need no swapping.
bool g_big = true;
inline u8* gp(GuestCall& c, u32 addr) {
  g_big = c.powerpc;
  return addr ? c.mem.host(addr) : nullptr;
}
inline u32 rd32(const u8* p) {
  return g_big ? (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]
               : (u32)p[3] << 24 | (u32)p[2] << 16 | (u32)p[1] << 8 | p[0];
}
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
inline float rdf(const u8* p) {
  const u32 v = rd32(p);
  float f;
  memcpy(&f, &v, 4);
  return f;
}
inline void wrf(u8* p, float f) {
  u32 v;
  memcpy(&v, &f, 4);
  wr32(p, v);
}

// Stand-in handles for the host's devices and contexts.
std::mutex g_mu;
std::unordered_map<u32, void*> g_handles;
u32 g_next = 0x00A1D000;
u32 handle_for(void* p) {
  if (!p) return 0;
  std::lock_guard<std::mutex> lk(g_mu);
  for (const auto& [h, q] : g_handles) {
    if (q == p) return h;
  }
  g_next += 0x10;
  g_handles[g_next] = p;
  return g_next;
}
void* host_of(u32 h) {
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_handles.find(h);
  return it == g_handles.end() ? nullptr : it->second;
}
void forget(u32 h) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_handles.erase(h);
}

// What has gone to the speakers, for -v.
std::atomic<u32> g_plays{0}, g_queued{0};
std::atomic<u64> g_bytes{0};

// How many floats a vector parameter has.
int count_of(ALenum p) {
  switch (p) {
    case AL_ORIENTATION: return 6;
    case AL_POSITION: case AL_VELOCITY: case AL_DIRECTION: return 3;
    default: return 1;
  }
}

// ---- devices and contexts -------------------------------------------------------

void alcOpenDevice_(GuestCall& c) {
  const u32 name = c.i();
  (void)name;  // the guest names its own era's devices; the Mac's default will do
  ALCdevice* d = alcOpenDevice(nullptr);
  if (!d) log_warn("OpenAL: the Mac has no sound device to open");
  c.ret(handle_for(d));
}
void alcCloseDevice_(GuestCall& c) {
  const u32 h = c.i();
  ALCdevice* d = (ALCdevice*)host_of(h);
  if (d) alcCloseDevice(d);
  forget(h);
  c.ret(1);
}
void alcCreateContext_(GuestCall& c) {
  ALCdevice* d = (ALCdevice*)host_of(c.i());
  const u8* in = gp(c, c.i());
  std::vector<ALCint> attrs;
  for (int k = 0; in && k < 64; k += 2) {
    const ALCint key = (ALCint)rd32(in + 4 * k);
    attrs.push_back(key);
    if (!key) break;
    attrs.push_back((ALCint)rd32(in + 4 * k + 4));
  }
  if (!attrs.empty() && attrs.back() != 0) attrs.push_back(0);
  ALCcontext* x = d ? alcCreateContext(d, attrs.empty() ? nullptr : attrs.data()) : nullptr;
  c.ret(handle_for(x));
}
void alcMakeContextCurrent_(GuestCall& c) {
  const u32 h = c.i();
  c.ret(alcMakeContextCurrent(h ? (ALCcontext*)host_of(h) : nullptr));
}
void alcGetCurrentContext_(GuestCall& c) { c.ret(handle_for(alcGetCurrentContext())); }
void alcDestroyContext_(GuestCall& c) {
  const u32 h = c.i();
  if (ALCcontext* x = (ALCcontext*)host_of(h)) alcDestroyContext(x);
  forget(h);
}

// ---- state ----------------------------------------------------------------------

// Reading OpenAL's error clears it. An error taken for the -v tally (see
// tally) is kept here for the guest's own next alGetError, since OpenAL only
// keeps the first error until it is read.
std::atomic<ALenum> g_pending_error{AL_NO_ERROR};
ALenum take_error() {
  const ALenum pending = g_pending_error.exchange(AL_NO_ERROR);
  const ALenum now = alGetError();
  return pending != AL_NO_ERROR ? pending : now;
}

void alGetError_(GuestCall& c) { c.ret((u32)take_error()); }
void alEnable_(GuestCall& c) { alEnable((ALenum)c.i()); }
void alDistanceModel_(GuestCall& c) { alDistanceModel((ALenum)c.i()); }
void alGetString_(GuestCall& c) {
  const ALenum p = (ALenum)c.i();
  static std::mutex mu;
  static std::unordered_map<ALenum, u32> made;
  std::lock_guard<std::mutex> lk(mu);
  if (auto it = made.find(p); it != made.end()) {
    c.ret(it->second);
    return;
  }
  const char* s = (const char*)alGetString(p);
  if (!s) {
    c.ret(0);
    return;
  }
  const u32 at = c.scratch((u32)strlen(s) + 1);
  if (at) memcpy(c.mem.host(at), s, strlen(s) + 1);
  made[p] = at;
  c.ret(at);
}

// ---- buffers and sources --------------------------------------------------------

void names_out(GuestCall& c, void (*gen)(ALsizei, ALuint*)) {
  const ALsizei n = (ALsizei)c.i();
  u8* out = gp(c, c.i());
  if (n <= 0 || !out) return;
  std::vector<ALuint> v((size_t)n);
  gen(n, v.data());
  for (ALsizei k = 0; k < n; k++) wr32(out + 4 * k, v[(size_t)k]);
}
void names_in(GuestCall& c, void (*del)(ALsizei, const ALuint*)) {
  const ALsizei n = (ALsizei)c.i();
  const u8* in = gp(c, c.i());
  if (n <= 0 || !in) return;
  std::vector<ALuint> v((size_t)n);
  for (ALsizei k = 0; k < n; k++) v[(size_t)k] = rd32(in + 4 * k);
  del(n, v.data());
}
void alGenBuffers_(GuestCall& c) { names_out(c, alGenBuffers); }
void alDeleteBuffers_(GuestCall& c) { names_in(c, alDeleteBuffers); }
void alGenSources_(GuestCall& c) { names_out(c, alGenSources); }
void alDeleteSources_(GuestCall& c) { names_in(c, alDeleteSources); }
void alIsBuffer_(GuestCall& c) { c.ret(alIsBuffer(c.i())); }

void alBufferData_(GuestCall& c) {
  const ALuint b = c.i();
  const ALenum format = (ALenum)c.i();
  const u8* data = gp(c, c.i());
  const ALsizei size = (ALsizei)c.i();
  const ALsizei freq = (ALsizei)c.i();
  g_bytes += size > 0 ? (u64)size : 0;
  if (!data || size <= 0) {
    alBufferData(b, format, nullptr, 0, freq);
    return;
  }
  if (g_big && (format == AL_FORMAT_MONO16 || format == AL_FORMAT_STEREO16)) {  // big-endian samples
    static thread_local std::vector<u8> swapped;
    swapped.resize((size_t)size);
    for (ALsizei k = 0; k + 1 < size; k += 2) {
      swapped[(size_t)k] = data[k + 1];
      swapped[(size_t)k + 1] = data[k];
    }
    alBufferData(b, format, swapped.data(), size, freq);
  } else {
    alBufferData(b, format, data, size, freq);
  }
}
void alGetBufferi_(GuestCall& c) {
  const ALuint b = c.i();
  const ALenum p = (ALenum)c.i();
  u8* out = gp(c, c.i());
  ALint v = 0;
  alGetBufferi(b, p, &v);
  if (out) wr32(out, (u32)v);
}

void tally() {  // with -v, now and then: what the game has sent to the speakers
  static std::atomic<u32> last{0};
  const u32 n = g_plays + g_queued;
  if (n / 50 != last / 50) {
    last = n;
    const ALenum error = take_error();
    if (error != AL_NO_ERROR) g_pending_error = error;  // still the guest's to read
    log_info("OpenAL: %u sounds started, %u buffers queued, %llu bytes of sound, error %d", (u32)g_plays,
             (u32)g_queued, (unsigned long long)g_bytes, (int)error);
  }
}
void alSourcePlay_(GuestCall& c) {
  alSourcePlay(c.i());
  g_plays++;
  tally();
}
void alSourceStop_(GuestCall& c) { alSourceStop(c.i()); }
void alSourcef_(GuestCall& c) {
  const ALuint s = c.i();
  const ALenum p = (ALenum)c.i();
  alSourcef(s, p, c.f());
}
void alSource3f_(GuestCall& c) {
  const ALuint s = c.i();
  const ALenum p = (ALenum)c.i();
  const float x = c.f(), y = c.f(), z = c.f();
  alSource3f(s, p, x, y, z);
}
void alSourcefv_(GuestCall& c) {
  const ALuint s = c.i();
  const ALenum p = (ALenum)c.i();
  const u8* in = gp(c, c.i());
  ALfloat v[6] = {};
  for (int k = 0; in && k < count_of(p); k++) v[k] = rdf(in + 4 * k);
  alSourcefv(s, p, v);
}
void alSourcei_(GuestCall& c) {
  const ALuint s = c.i();
  const ALenum p = (ALenum)c.i();
  alSourcei(s, p, (ALint)c.i());
}
void alGetSourcef_(GuestCall& c) {
  const ALuint s = c.i();
  const ALenum p = (ALenum)c.i();
  u8* out = gp(c, c.i());
  ALfloat v = 0;
  alGetSourcef(s, p, &v);
  if (out) wrf(out, v);
}
void alGetSourcefv_(GuestCall& c) {
  const ALuint s = c.i();
  const ALenum p = (ALenum)c.i();
  u8* out = gp(c, c.i());
  ALfloat v[6] = {};
  alGetSourcefv(s, p, v);
  for (int k = 0; out && k < count_of(p); k++) wrf(out + 4 * k, v[k]);
}
void alGetSourcei_(GuestCall& c) {
  const ALuint s = c.i();
  const ALenum p = (ALenum)c.i();
  u8* out = gp(c, c.i());
  ALint v = 0;
  alGetSourcei(s, p, &v);
  if (out) wr32(out, (u32)v);
}
void alSourceQueueBuffers_(GuestCall& c) {
  const ALuint s = c.i();
  const ALsizei n = (ALsizei)c.i();
  const u8* in = gp(c, c.i());
  if (n <= 0 || !in) return;
  std::vector<ALuint> v((size_t)n);
  for (ALsizei k = 0; k < n; k++) v[(size_t)k] = rd32(in + 4 * k);
  alSourceQueueBuffers(s, n, v.data());
  g_queued += (u32)n;
  tally();
}
void alSourceUnqueueBuffers_(GuestCall& c) {
  const ALuint s = c.i();
  const ALsizei n = (ALsizei)c.i();
  u8* out = gp(c, c.i());
  if (n <= 0 || !out) return;
  std::vector<ALuint> v((size_t)n);
  alSourceUnqueueBuffers(s, n, v.data());
  for (ALsizei k = 0; k < n; k++) wr32(out + 4 * k, v[(size_t)k]);
}

// ---- the listener ---------------------------------------------------------------

void alListenerf_(GuestCall& c) {
  const ALenum p = (ALenum)c.i();
  alListenerf(p, c.f());
}
void alListener3f_(GuestCall& c) {
  const ALenum p = (ALenum)c.i();
  const float x = c.f(), y = c.f(), z = c.f();
  alListener3f(p, x, y, z);
}
void alListenerfv_(GuestCall& c) {
  const ALenum p = (ALenum)c.i();
  const u8* in = gp(c, c.i());
  ALfloat v[6] = {};
  for (int k = 0; in && k < count_of(p); k++) v[k] = rdf(in + 4 * k);
  alListenerfv(p, v);
}
void alGetListenerf_(GuestCall& c) {
  const ALenum p = (ALenum)c.i();
  u8* out = gp(c, c.i());
  ALfloat v = 0;
  alGetListenerf(p, &v);
  if (out) wrf(out, v);
}
void alGetListener3f_(GuestCall& c) {
  const ALenum p = (ALenum)c.i();
  u8* x = gp(c, c.i());
  u8* y = gp(c, c.i());
  u8* z = gp(c, c.i());
  ALfloat a = 0, b = 0, d = 0;
  alGetListener3f(p, &a, &b, &d);
  if (x) wrf(x, a);
  if (y) wrf(y, b);
  if (z) wrf(z, d);
}
void alGetListeneri_(GuestCall& c) {
  const ALenum p = (ALenum)c.i();
  u8* out = gp(c, c.i());
  ALint v = 0;
  alGetListeneri(p, &v);
  if (out) wr32(out, (u32)v);
}

#define H(name) {#name, name##_}
const HookSpec kAL[] = {
    H(alcOpenDevice), H(alcCloseDevice), H(alcCreateContext), H(alcMakeContextCurrent), H(alcGetCurrentContext),
    H(alcDestroyContext), H(alGetError), H(alEnable), H(alDistanceModel), H(alGetString), H(alGenBuffers),
    H(alDeleteBuffers), H(alGenSources), H(alDeleteSources), H(alIsBuffer), H(alBufferData), H(alGetBufferi),
    H(alSourcePlay), H(alSourceStop), H(alSourcef), H(alSource3f), H(alSourcefv), H(alSourcei), H(alGetSourcef),
    H(alGetSourcefv), H(alGetSourcei), H(alSourceQueueBuffers), H(alSourceUnqueueBuffers), H(alListenerf),
    H(alListener3f), H(alListenerfv), H(alGetListenerf), H(alGetListener3f), H(alGetListeneri),
};
#undef H

const HookLibrary kLibraries[] = {
    {"/OpenAL.framework/Versions/A/OpenAL", "al", kAL, sizeof kAL / sizeof kAL[0], true},
};

}  // namespace

const HookLibrary* al_hook_libraries(size_t& n) {
  n = sizeof kLibraries / sizeof kLibraries[0];
  return kLibraries;
}
