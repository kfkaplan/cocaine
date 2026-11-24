import ctypes
import struct

audio = ctypes.CDLL("/System/Library/Frameworks/AudioToolbox.framework/Versions/A/AudioToolbox")
uint = ctypes.c_uint32
int64 = ctypes.c_int64
read_callback = ctypes.CFUNCTYPE(ctypes.c_int32, ctypes.c_void_p, int64, uint,
                                ctypes.c_void_p, ctypes.POINTER(uint))
size_callback = ctypes.CFUNCTYPE(int64, ctypes.c_void_p)
audio.AudioFileOpenWithCallbacks.argtypes = [ctypes.c_void_p, read_callback, ctypes.c_void_p,
                                             size_callback, ctypes.c_void_p, uint,
                                             ctypes.POINTER(ctypes.c_void_p)]
audio.AudioFileReadBytes.argtypes = [ctypes.c_void_p, ctypes.c_ubyte, int64,
                                     ctypes.POINTER(uint), ctypes.c_void_p]
audio.AudioFileClose.argtypes = [ctypes.c_void_p]


def open_data(data):
    def read(client, position, requested, buffer, actual):
        part = data[position:position + requested]
        ctypes.memmove(buffer, part, len(part))
        actual[0] = len(part)
        return 0

    def size(client):
        return len(data)

    callbacks = (read_callback(read), size_callback(size))
    file = ctypes.c_void_p()
    status = audio.AudioFileOpenWithCallbacks(None, callbacks[0], None, callbacks[1], None, 0,
                                               ctypes.byref(file))
    return status, file, callbacks


def read_bytes(file, offset, count):
    data = ctypes.create_string_buffer(count)
    actual = uint(count)
    status = audio.AudioFileReadBytes(file, 0, offset, ctypes.byref(actual), data)
    assert status == 0, status
    return data.raw[:actual.value]


track = "\x00\x90\x3c\x64\x83\x60\x80\x3c\x40\x00\xff\x2f\x00"
midi = "MThd" + struct.pack(">IHHH", 6, 0, 1, 480) + "MTrk" + struct.pack(">I", len(track)) + track
status, file, callbacks = open_data(midi)
assert status == 0 and file.value, status
first = read_bytes(file, 0, 44100 * 4)
assert len(first) == 44100 * 4
assert max(abs(x) for x in struct.unpack("<%dh" % (len(first) / 2), first)) > 100
read_bytes(file, 44100 * 4, 4096)
again = read_bytes(file, 0, 44100)
assert max(abs(x) for x in struct.unpack("<%dh" % (len(again) / 2), again)) > 100
assert audio.AudioFileClose(file) == 0

pcm = "\x01\x00\x02\x00" * 100
wave = ("RIFF" + struct.pack("<I", len(pcm) + 36) + "WAVEfmt " +
        struct.pack("<IHHIIHH", 16, 1, 2, 44100, 176400, 4, 16) +
        "data" + struct.pack("<I", len(pcm)) + pcm)
status, file, callbacks = open_data(wave)
assert status == 0 and file.value, status
assert read_bytes(file, 0, len(pcm)) == pcm
assert audio.AudioFileClose(file) == 0

status, file, callbacks = open_data("MThd\x00\x00")
assert status != 0
print "MIDI audio compatibility ok"
