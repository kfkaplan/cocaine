// MIDI sound files presented to the guest's AudioFile reader as PCM WAVE.
// The guest still controls playback through NSSound and its usual audio path.

#include "audio/midi.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "kernel/process.h"

namespace {

constexpr u32 kRate = 44100, kBlock = 512, kBytesPerFrame = 4;
constexpr u32 kReadSize = 4096, kMaxMidiBytes = 64 * 1024 * 1024;
constexpr u32 kFileError = 0x7768743f;  // AudioFile's unspecified error, 'wht?'
constexpr u32 kUnsupportedType = 0x7479703f;  // 'typ?'
constexpr u32 kBadAddress = (u32)-50;  // paramErr

struct Event {
  u64 frame = 0;
  u8 status = 0, data1 = 0, data2 = 0;
  std::vector<u8> sysex;
};

struct MidiSource {
  std::mutex mu;
  std::vector<Event> events;
  std::array<u8, 44> header = {};
  std::array<u8, kBlock * kBytesPerFrame> block = {};
  AudioUnit synth = nullptr;
  u64 frames = 0, next_frame = 0, block_start = 0;
  u32 block_frames = 0;
  size_t next_event = 0;

  ~MidiSource() {
    if (synth) {
      AudioUnitUninitialize(synth);
      AudioComponentInstanceDispose(synth);
    }
  }

  bool reset() {
    if (synth) {
      AudioUnitUninitialize(synth);
      AudioComponentInstanceDispose(synth);
      synth = nullptr;
    }
    const AudioComponentDescription desc = {kAudioUnitType_MusicDevice, kAudioUnitSubType_DLSSynth,
                                             kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent component = AudioComponentFindNext(nullptr, &desc);
    OSStatus error = component ? AudioComponentInstanceNew(component, &synth) : -1;
    if (!error) {
      AudioStreamBasicDescription fmt = {};
      fmt.mSampleRate = kRate;
      fmt.mFormatID = kAudioFormatLinearPCM;
      fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
      fmt.mBytesPerPacket = fmt.mBytesPerFrame = sizeof(float);
      fmt.mFramesPerPacket = 1;
      fmt.mChannelsPerFrame = 2;
      fmt.mBitsPerChannel = 32;
      error = AudioUnitSetProperty(synth, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &fmt, sizeof fmt);
    }
    if (!error) {
      const UInt32 offline = 1;
      error = AudioUnitSetProperty(synth, kAudioUnitProperty_OfflineRender, kAudioUnitScope_Global, 0,
                                   &offline, sizeof offline);
    }
    if (!error) error = AudioUnitInitialize(synth);
    if (error) {
      log_warn("MIDI: cannot initialize the Mac's synthesizer (%d)", (int)error);
      if (synth) AudioComponentInstanceDispose(synth);
      synth = nullptr;
      return false;
    }
    next_frame = block_start = 0;
    block_frames = 0;
    next_event = 0;
    return true;
  }

  bool render_block() {
    if (next_frame >= frames) return false;
    const u32 n = (u32)std::min<u64>(kBlock, frames - next_frame);
    while (next_event < events.size() && events[next_event].frame < next_frame + n) {
      const Event& ev = events[next_event++];
      OSStatus error = ev.sysex.empty()
                           ? MusicDeviceMIDIEvent(synth, ev.status, ev.data1, ev.data2,
                                                  (u32)(ev.frame > next_frame ? ev.frame - next_frame : 0))
                           : MusicDeviceSysEx(synth, ev.sysex.data(), (u32)ev.sysex.size());
      if (error) {
        log_warn("MIDI: synthesizer rejected an event (%d)", (int)error);
        return false;
      }
    }
    std::array<float, kBlock> left = {}, right = {};
    struct Buffers {
      UInt32 count;
      AudioBuffer buffers[2];
    } output = {2, {{1, n * (u32)sizeof(float), left.data()}, {1, n * (u32)sizeof(float), right.data()}}};
    AudioTimeStamp timestamp = {};
    timestamp.mFlags = kAudioTimeStampSampleTimeValid;
    timestamp.mSampleTime = (double)next_frame;
    AudioUnitRenderActionFlags flags = 0;
    const OSStatus error = AudioUnitRender(synth, &flags, &timestamp, 0, n, (AudioBufferList*)&output);
    if (error) {
      log_warn("MIDI: synthesizer could not render (%d)", (int)error);
      return false;
    }
    for (u32 i = 0; i < n; i++) {
      const float samples[2] = {left[i], right[i]};
      for (u32 channel = 0; channel < 2; channel++) {
        const float value = std::clamp(samples[channel], -1.0f, 1.0f);
        const s16 sample = (s16)std::lrint(value * (value < 0 ? 32768.0f : 32767.0f));
        const u16 bits = (u16)sample;
        block[i * 4 + channel * 2] = (u8)bits;
        block[i * 4 + channel * 2 + 1] = (u8)(bits >> 8);
      }
    }
    block_start = next_frame;
    block_frames = n;
    next_frame += n;
    return true;
  }

  bool read(u64 offset, u8* out, u32 count) {
    while (count && offset < header.size()) {
      const u32 n = (u32)std::min<u64>(count, header.size() - offset);
      memcpy(out, header.data() + offset, n);
      offset += n;
      out += n;
      count -= n;
    }
    while (count) {
      const u64 pos = offset - header.size();
      const u64 frame = pos / kBytesPerFrame;
      if (block_frames && frame < block_start && !reset()) return false;
      while (!block_frames || frame >= block_start + block_frames) {
        if (!render_block()) return false;
      }
      const u64 in_block = pos - block_start * kBytesPerFrame;
      const u32 n = (u32)std::min<u64>(count, (u64)block_frames * kBytesPerFrame - in_block);
      memcpy(out, block.data() + in_block, n);
      offset += n;
      out += n;
      count -= n;
    }
    return true;
  }
};

std::mutex g_mu;
std::unordered_map<u32, std::shared_ptr<MidiSource>> g_sources;
std::unordered_map<u32, u32> g_files;

std::shared_ptr<MidiSource> source_of(u32 id) {
  std::lock_guard<std::mutex> lock(g_mu);
  const auto it = g_sources.find(id);
  return it == g_sources.end() ? nullptr : it->second;
}

void put16(u8* p, u16 value) { p[0] = (u8)value; p[1] = (u8)(value >> 8); }
void put32(u8* p, u32 value) {
  put16(p, (u16)value);
  put16(p + 2, (u16)(value >> 16));
}

bool seconds_at(MusicSequence sequence, double beat, double& seconds) {
  const OSStatus error = MusicSequenceGetSecondsForBeats(sequence, beat, &seconds);
  if (error || !std::isfinite(seconds) || seconds < 0) {
    log_warn("MIDI: cannot convert beat time %g (%d)", beat, (int)error);
    return false;
  }
  return true;
}

bool load_sequence(const std::vector<u8>& data, MidiSource& result) {
  CFDataRef contents = CFDataCreate(nullptr, data.data(), (CFIndex)data.size());
  if (!contents) {
    log_warn("MIDI: cannot allocate sequence data");
    return false;
  }
  MusicSequence sequence = nullptr;
  OSStatus error = NewMusicSequence(&sequence);
  if (!error) error = MusicSequenceFileLoadData(sequence, contents, kMusicSequenceFile_MIDIType, 0);
  CFRelease(contents);
  if (error) {
    log_warn("MIDI: cannot load sequence (%d)", (int)error);
    if (sequence) DisposeMusicSequence(sequence);
    return false;
  }
  UInt32 tracks = 0;
  error = MusicSequenceGetTrackCount(sequence, &tracks);
  double duration = 0;
  for (UInt32 t = 0; !error && t < tracks; t++) {
    MusicTrack track = nullptr;
    error = MusicSequenceGetIndTrack(sequence, t, &track);
    MusicTimeStamp length = 0;
    UInt32 size = sizeof length;
    if (!error) error = MusicTrackGetProperty(track, kSequenceTrackProperty_TrackLength, &length, &size);
    double seconds = 0;
    if (!error && !seconds_at(sequence, length, seconds)) error = -1;
    if (error) break;
    duration = std::max(duration, seconds);
    MusicEventIterator iterator = nullptr;
    error = NewMusicEventIterator(track, &iterator);
    Boolean has = false;
    if (!error) error = MusicEventIteratorHasCurrentEvent(iterator, &has);
    while (!error && has) {
      MusicTimeStamp beat = 0;
      MusicEventType type = 0;
      const void* info = nullptr;
      UInt32 bytes = 0;
      error = MusicEventIteratorGetEventInfo(iterator, &beat, &type, &info, &bytes);
      if (error) break;
      if (type == kMusicEventType_MIDINoteMessage || type == kMusicEventType_MIDIChannelMessage ||
          type == kMusicEventType_MIDIRawData) {
        if (!seconds_at(sequence, beat, seconds)) { error = -1; break; }
        Event ev;
        ev.frame = (u64)std::llround(seconds * kRate);
        if (type == kMusicEventType_MIDINoteMessage && bytes >= sizeof(MIDINoteMessage)) {
          const auto& note = *(const MIDINoteMessage*)info;
          ev.status = 0x90 | (note.channel & 15);
          ev.data1 = note.note;
          ev.data2 = note.velocity;
          double end = 0;
          if (!seconds_at(sequence, beat + note.duration, end)) { error = -1; break; }
          duration = std::max(duration, end);
          result.events.push_back(ev);
          ev.frame = (u64)std::llround(end * kRate);
          ev.status = 0x80 | (note.channel & 15);
          ev.data2 = note.releaseVelocity;
        } else if (type == kMusicEventType_MIDIChannelMessage && bytes >= sizeof(MIDIChannelMessage)) {
          const auto& msg = *(const MIDIChannelMessage*)info;
          ev.status = msg.status;
          ev.data1 = msg.data1;
          ev.data2 = msg.data2;
        } else if (type == kMusicEventType_MIDIRawData && bytes >= sizeof(UInt32)) {
          const auto& raw = *(const MIDIRawData*)info;
          if (raw.length > bytes - sizeof(UInt32)) { error = -1; break; }
          ev.sysex.assign(raw.data, raw.data + raw.length);
          if (ev.sysex.empty()) { error = -1; break; }
        } else {
          error = -1;
          break;
        }
        result.events.push_back(std::move(ev));
      }
      error = MusicEventIteratorNextEvent(iterator);
      if (!error) error = MusicEventIteratorHasCurrentEvent(iterator, &has);
    }
    if (iterator) DisposeMusicEventIterator(iterator);
  }
  DisposeMusicSequence(sequence);
  if (error || duration > (double)(UINT32_MAX - 36) / (kRate * kBytesPerFrame) - 2) {
    log_warn("MIDI: cannot read sequence events or duration (%d)", (int)error);
    return false;
  }
  std::stable_sort(result.events.begin(), result.events.end(),
                   [](const Event& a, const Event& b) { return a.frame < b.frame; });
  result.frames = (u64)std::ceil((duration + 2) * kRate);
  const u32 pcm_bytes = (u32)(result.frames * kBytesPerFrame);
  u8* h = result.header.data();
  memcpy(h, "RIFF", 4); put32(h + 4, pcm_bytes + 36);
  memcpy(h + 8, "WAVEfmt ", 8); put32(h + 16, 16);
  put16(h + 20, 1); put16(h + 22, 2); put32(h + 24, kRate);
  put32(h + 28, kRate * kBytesPerFrame); put16(h + 32, kBytesPerFrame); put16(h + 34, 16);
  memcpy(h + 36, "data", 4); put32(h + 40, pcm_bytes);
  return result.reset();
}

void wave_read(GuestCall& c) {
  const u32 id = c.i(), first = c.i(), second = c.i(), requested = c.i(), buffer = c.i(), actual = c.i();
  const u64 offset = c.powerpc ? ((u64)first << 32) | second : ((u64)second << 32) | first;
  const auto source = source_of(id);
  if (!source || !actual || !c.mem.range_ok(actual, 4, GuestMemory::kWrite)) return c.ret(kBadAddress);
  c.mem.w32(actual, 0);
  const u64 length = source->header.size() + source->frames * kBytesPerFrame;
  const u32 count = offset < length ? (u32)std::min<u64>(requested, length - offset) : 0;
  if (count && (!buffer || !c.mem.range_ok(buffer, count, GuestMemory::kWrite))) return c.ret(kBadAddress);
  std::lock_guard<std::mutex> lock(source->mu);
  if (count && !source->read(offset, c.mem.host(buffer), count)) return c.ret(kFileError);
  c.mem.w32(actual, count);
  c.ret(0);
}

void wave_size(GuestCall& c) {
  const auto source = source_of(c.i());
  if (!source) {
    log_warn("MIDI: audio-file size requested after close");
    return c.ret(0);
  }
  const u64 length = source->header.size() + source->frames * kBytesPerFrame;
  if (c.powerpc) {
    c.thread.ppc.gpr[3] = (u32)(length >> 32);
    c.thread.ppc.gpr[4] = (u32)length;
  } else {
    c.thread.x86.reg[X86State::EAX] = (u32)length;
    c.thread.x86.reg[X86State::EDX] = (u32)(length >> 32);
  }
}

u32 read_original(GuestCall& c, u32 client, u32 read, u64 offset, u32 n, u32 buffer, u32 actual) {
  const u32 lo = (u32)offset, hi = (u32)(offset >> 32);
  return c.call(read, {client, c.powerpc ? hi : lo, c.powerpc ? lo : hi, n, buffer, actual});
}

}  // namespace

void midi_audio_file_open(GuestCall& c) {
  const u32 client = c.i(), read = c.i(), write = c.i(), size = c.i(), setsize = c.i(), hint = c.i(), out = c.i();
  if (!c.original()) {
    log_warn("MIDI: cannot call the guest's AudioFileOpenWithCallbacks");
    return c.ret(kFileError);
  }
  const u32 original_error = c.call(c.original(), {client, read, write, size, setsize, hint, out});
  if (original_error != kUnsupportedType || !read) return c.ret(original_error);
  const u32 probe = c.scratch(8);
  if (!probe) {
    log_warn("MIDI: cannot allocate a file probe");
    return c.ret(original_error);
  }
  c.mem.w32(probe + 4, 0);
  const u32 first = read_original(c, client, read, 0, 4, probe, probe + 4);
  if (first || c.mem.r32(probe + 4) != 4 || memcmp(c.mem.host(probe), "MThd", 4))
    return c.ret(original_error);

  const u32 scratch = c.scratch(kReadSize + 4);
  if (!scratch) {
    log_warn("MIDI: cannot allocate a sequence read buffer");
    return c.ret(kFileError);
  }
  std::vector<u8> data;
  for (;;) {
    c.mem.w32(scratch + kReadSize, 0);
    const u32 error = read_original(c, client, read, data.size(), kReadSize, scratch, scratch + kReadSize);
    const u32 got = c.mem.r32(scratch + kReadSize);
    if (error || got > kReadSize || data.size() + got > kMaxMidiBytes) {
      log_warn("MIDI: cannot read sequence data (error %u, %u bytes)", error, got);
      return c.ret(error ? error : kFileError);
    }
    data.insert(data.end(), c.mem.host(scratch), c.mem.host(scratch) + got);
    if (got < kReadSize) break;
  }
  auto source = std::make_shared<MidiSource>();
  if (!load_sequence(data, *source)) return c.ret(kFileError);
  const u32 id = c.scratch(4);
  const u32 read_fn = native_function(c.process, "cocaine_midi_wave_read", wave_read);
  const u32 size_fn = native_function(c.process, "cocaine_midi_wave_size", wave_size);
  if (!id || !read_fn || !size_fn || !out || !c.mem.range_ok(out, 4, GuestMemory::kWrite)) {
    log_warn("MIDI: cannot create the guest's audio-file callbacks");
    return c.ret(kFileError);
  }
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_sources[id] = source;
  }
  const u32 error = c.call(c.original(), {id, read_fn, 0, size_fn, 0, 0x57415645, out});
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (error) g_sources.erase(id);
    else g_files[c.mem.r32(out)] = id;
  }
  if (error) log_warn("MIDI: guest cannot open synthesized audio (%u)", error);
  else log_info("MIDI: opened a %.1f second sequence", (double)source->frames / kRate);
  c.ret(error);
}

void midi_audio_file_close(GuestCall& c) {
  const u32 file = c.i();
  if (!c.original()) {
    log_warn("MIDI: cannot call the guest's AudioFileClose");
    return c.ret(kFileError);
  }
  const u32 error = c.call(c.original(), {file});
  if (!error) {
    std::lock_guard<std::mutex> lock(g_mu);
    const auto it = g_files.find(file);
    if (it != g_files.end()) {
      g_sources.erase(it->second);
      g_files.erase(it);
    }
  }
  c.ret(error);
}
