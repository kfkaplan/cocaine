import ctypes
import os
import struct

lib = ctypes.CDLL("/usr/lib/libSystem.B.dylib")

class AttrList(ctypes.Structure):
    _fields_ = [("count", ctypes.c_ushort), ("reserved", ctypes.c_ushort),
                ("common", ctypes.c_uint), ("volume", ctypes.c_uint),
                ("directory", ctypes.c_uint), ("file", ctypes.c_uint),
                ("fork", ctypes.c_uint)]

def mount_flags(path):
    attrs = AttrList(5, 0, 0, 0x80004000, 0, 0, 0)
    result = ctypes.create_string_buffer(64)
    assert lib.getattrlist(path, ctypes.byref(attrs), result, len(result), 0) == 0
    return struct.unpack("=I", result.raw[4:8])[0]

def statfs(path):
    result = ctypes.create_string_buffer(272)
    assert lib.statfs(path, result) == 0
    return result.raw

root = statfs("/")
system = statfs("/System")
count = lib.getfsstat(None, 0, 2)
assert count >= 1
volumes = ctypes.create_string_buffer(272 * (count + 16))
n = lib.getfsstat(volumes, len(volumes), 2)
assert n >= 1
assert root[32:40] == system[32:40] == volumes.raw[32:40]
assert root[48:52] == system[48:52] == volumes.raw[48:52]
assert root[75:].split("\0", 1)[0] == "/"
assert volumes.raw[75:].split("\0", 1)[0] == "/"
assert lib.getfsstat(volumes, 271, 2) == 0
assert lib.getfsstat(volumes, 272, 2) == 1
wide = ctypes.create_string_buffer(2168 * (count + 16))
nw = lib.getfsstat64(wide, len(wide), 2)
assert nw >= 1
assert wide.raw[88:].split("\0", 1)[0] == "/"
mnt = ctypes.c_void_p()
nm = lib.getmntinfo(ctypes.byref(mnt), 2)
assert nm >= 1 and mnt.value
mnt_entries = ctypes.string_at(mnt, 272 * nm)

io = ctypes.CDLL("/System/Library/Frameworks/IOKit.framework/IOKit")
io.IOBSDNameMatching.restype = ctypes.c_void_p

def matching_media(bsd):
    matching = io.IOBSDNameMatching(0, 0, bsd)
    assert matching
    iterator = ctypes.c_uint()
    assert io.IOServiceGetMatchingServices(0, matching, ctypes.byref(iterator)) == 0
    media = io.IOIteratorNext(iterator.value)
    io.IOObjectRelease(iterator.value)
    return media

assert not matching_media("disk999999s1")

volume = os.environ.get("COCAINE_TEST_VOLUME")
if volume:
    entries = [volumes.raw[i * 272:(i + 1) * 272] for i in range(n)]
    entry = [e for e in entries if e[75:165].split("\0", 1)[0] == volume]
    assert len(entry) == 1, (volume, [e[75:165].split("\0", 1)[0] for e in entries])
    assert volume in [mnt_entries[i * 272 + 75:i * 272 + 165].split("\0", 1)[0]
                      for i in range(nm)]
    assert entry[0][32:40] == statfs(volume)[32:40]
    wentries = [wide.raw[i * 2168:(i + 1) * 2168] for i in range(nw)]
    assert volume in [e[88:1112].split("\0", 1)[0] for e in wentries]
    vol_device = os.stat(volume).st_dev
    assert os.stat("/.vol/%d/@" % vol_device).st_ino == os.stat(volume).st_ino
    assert os.stat("/.vol/%d/2" % vol_device).st_ino == os.stat(volume).st_ino
    marker = os.environ.get("COCAINE_TEST_VOLUME_MARKER")
    if marker:
        assert os.path.isfile(os.path.join(volume, marker))
        assert os.path.isfile("/.vol/%d/2/%s" % (vol_device, marker))

    cf = ctypes.CDLL("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
    io.IORegistryEntryCreateCFProperty.restype = ctypes.c_void_p
    cf.CFStringCreateWithCString.restype = ctypes.c_void_p

    device_name = entry[0][165:255].split("\0", 1)[0]
    assert device_name.startswith("/dev/")
    media = matching_media(device_name[5:])
    assert media and io.IOObjectConformsTo(media, "IOMedia")
    parents = ctypes.c_uint()
    assert io.IORegistryEntryCreateIterator(media, "IOService", 3, ctypes.byref(parents)) == 0
    parent = io.IOIteratorNext(parents.value)
    assert parent and io.IOObjectConformsTo(parent, "IOMedia")
    assert io.IOObjectConformsTo(parent, "IOCDMedia")
    key = cf.CFStringCreateWithCString(0, "Whole", 0x08000100)
    whole = io.IORegistryEntryCreateCFProperty(parent, key, 0, 0)
    assert whole and cf.CFBooleanGetValue(whole)
    cf.CFRelease(whole)
    cf.CFRelease(key)
    io.IOObjectRelease(parent)
    assert not io.IOIteratorNext(parents.value)
    io.IOIteratorReset(parents.value)
    parent = io.IOIteratorNext(parents.value)
    assert parent
    io.IOObjectRelease(parent)
    io.IOObjectRelease(parents.value)
    io.IOObjectRelease(media)

device = os.stat("/System").st_dev
assert mount_flags("/.vol/%d/@" % device) == mount_flags("/System")
assert os.stat("/.vol/%d/@/System" % device).st_ino == os.stat("/System").st_ino
if device != 0:
    attrs = AttrList(5, 0, 0, 0x80004000, 0, 0, 0)
    result = ctypes.create_string_buffer(64)
    assert lib.getattrlist("/.vol/0/@", ctypes.byref(attrs), result, len(result), 0) == -1
print "volume metadata ok"
