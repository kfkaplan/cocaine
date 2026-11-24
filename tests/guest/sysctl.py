import ctypes

lib = ctypes.CDLL("/usr/lib/libSystem.B.dylib")
lib.sysctl.argtypes = [ctypes.POINTER(ctypes.c_int), ctypes.c_uint,
                       ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t),
                       ctypes.c_void_p, ctypes.c_size_t]
lib.sysctlbyname.argtypes = [ctypes.c_char_p, ctypes.c_void_p,
                             ctypes.POINTER(ctypes.c_size_t), ctypes.c_void_p, ctypes.c_size_t]
mib = (ctypes.c_int * 2)(1, 62)

def read(size=64):
    buf = ctypes.create_string_buffer(size)
    length = ctypes.c_size_t(size)
    assert lib.sysctl(mib, 2, buf, ctypes.byref(length), None, 0) == 0
    return buf.value, length.value

original, length = read()
assert original and length == len(original) + 1
new = ctypes.create_string_buffer("ghost-test")
old = ctypes.create_string_buffer(64)
length = ctypes.c_size_t(64)
assert lib.sysctl(mib, 2, old, ctypes.byref(length), new, 10) == 0
assert old.value == original
assert read() == ("ghost-test", 11)
assert read(5) == ("ghos", 5)

buf = ctypes.create_string_buffer(64)
length = ctypes.c_size_t(64)
assert lib.sysctlbyname("kern.procname", buf, ctypes.byref(length), None, 0) == 0
assert buf.value == "ghost-test"
too_long = ctypes.create_string_buffer("x" * 33)
assert lib.sysctl(mib, 2, None, None, too_long, 33) == -1
assert read() == ("ghost-test", 11)
maximum = ctypes.create_string_buffer("x" * 32)
assert lib.sysctl(mib, 2, None, None, maximum, 32) == 0
assert read() == ("x" * 32, 33)
print "process name sysctl ok"
