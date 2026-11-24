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
volumes = ctypes.create_string_buffer(272)
assert lib.getfsstat(volumes, len(volumes), 2) == 1
assert root[32:40] == system[32:40] == volumes.raw[32:40]
assert root[48:52] == system[48:52] == volumes.raw[48:52]
assert root[75:].split("\0", 1)[0] == "/"
assert volumes.raw[75:].split("\0", 1)[0] == "/"

device = os.stat("/System").st_dev
assert mount_flags("/.vol/%d/@" % device) == mount_flags("/System")
assert os.stat("/.vol/%d/@/System" % device).st_ino == os.stat("/System").st_ino
if device != 0:
    attrs = AttrList(5, 0, 0, 0x80004000, 0, 0, 0)
    result = ctypes.create_string_buffer(64)
    assert lib.getattrlist("/.vol/0/@", ctypes.byref(attrs), result, len(result), 0) == -1
print "volume metadata ok"
