import ctypes
import os
import shutil
import struct
import tempfile

lib = ctypes.CDLL("/usr/lib/libSystem.B.dylib")

class AttrList(ctypes.Structure):
    _fields_ = [("count", ctypes.c_ushort), ("reserved", ctypes.c_ushort),
                ("common", ctypes.c_uint), ("volume", ctypes.c_uint),
                ("directory", ctypes.c_uint), ("file", ctypes.c_uint),
                ("fork", ctypes.c_uint)]

class FileStore(ctypes.Structure):
    _fields_ = [("flags", ctypes.c_uint), ("posmode", ctypes.c_int),
                ("offset", ctypes.c_longlong), ("length", ctypes.c_longlong),
                ("bytesalloc", ctypes.c_longlong)]

def attributes(path, common, fileattr):
    attrs = AttrList(5, 0, common, 0, 0, fileattr, 0)
    result = ctypes.create_string_buffer(256)
    rc = lib.getattrlist(path, ctypes.byref(attrs), result, len(result), 0)
    return rc, result.raw

root_rc, root_attrs = attributes("/System", 0x80, 0)
assert root_rc == 0
assert struct.unpack("=II", root_attrs[4:12]) == (2, 0)

directory = tempfile.mkdtemp(prefix="cocaine-metadata-", dir=os.path.expanduser("~/Library"))
path = directory + "/library"
side = directory + "/._library"
finder = "shlbTEST" + "\0" * 24
fork = "resource data"
try:
    open(path, "wb").close()
    fd = os.open(path, os.O_RDWR)
    try:
        store = FileStore(0, 3, 0, 4096, 0)
        assert lib.fcntl(fd, 42, ctypes.byref(store)) == 0
        assert store.bytesalloc >= 4096
        assert os.fstat(fd).st_size == 0
        invalid = FileStore(0, -1, 0, 4096, 0)
        assert lib.fcntl(fd, 42, ctypes.byref(invalid)) == -1
        assert lib.fcntl(-1, 42, ctypes.byref(store)) == -1
    finally:
        os.close(fd)
    header = struct.pack(">II16sH", 0x51607, 0x20000, "\0" * 16, 2)
    entries = struct.pack(">III", 9, 50, 32) + struct.pack(">III", 2, 82, len(fork))
    f = open(side, "wb")
    f.write(header + entries + finder + fork)
    f.close()
    rc, data = attributes(path, 0x4000, 0x1000)
    assert rc == 0 and data[4:36] == finder
    assert struct.unpack("=Q", data[36:44])[0] == len(fork)
    f = open(path + "/..namedfork/rsrc", "rb")
    assert f.read(4) == fork[:4]
    f.seek(0)
    assert f.read() == fork
    f.close()
    fd = os.open(path + "/..namedfork/rsrc", os.O_RDONLY)
    try:
        try:
            os.write(fd, "x")
            raise AssertionError("read-only resource fork was writable")
        except OSError:
            pass
    finally:
        os.close(fd)

    replacement = "APPLTEST" + "\0" * 24
    attrs = AttrList(5, 0, 0x4000, 0, 0, 0, 0)
    value = ctypes.create_string_buffer(replacement, 32)
    assert lib.setattrlist(path, ctypes.byref(attrs), value, 32, 0) == 0
    rc, data = attributes(path, 0x4000, 0x1000)
    assert rc == 0 and data[4:36] == replacement

    fd = os.open(path + "/..namedfork/rsrc", os.O_CREAT | os.O_RDWR, 0600)
    os.write(fd, "native")
    os.close(fd)
    rc, data = attributes(path, 0x4000, 0x1000)
    assert rc == 0 and struct.unpack("=Q", data[36:44])[0] == 6
    f = open(path + "/..namedfork/rsrc", "rb")
    assert f.read() == "native"
    f.close()

    os.unlink(path)
    open(path, "wb").close()
    f = open(side, "wb")
    f.write(header + struct.pack(">III", 2, 50, 1000) + entries[12:] + finder + fork)
    f.close()
    assert attributes(path, 0x4000, 0x1000)[0] == -1
    try:
        open(path + "/..namedfork/rsrc", "rb")
        raise AssertionError("invalid sidecar opened")
    except IOError:
        pass
    f = open(side, "wb")
    f.write(header + struct.pack(">III", 2, 50, 16) +
            struct.pack(">III", 2, 66, 16) + "\0" * 32)
    f.close()
    assert attributes(path, 0x4000, 0x1000)[0] == -1
finally:
    shutil.rmtree(directory)
print "file metadata ok"
