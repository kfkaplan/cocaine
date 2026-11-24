"""Exercise the Intel commpage pattern fill with guarded, unaligned buffers."""
import ctypes
import sys

libc = ctypes.CDLL(None)
libc.mmap.restype = ctypes.c_void_p
libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                      ctypes.c_int, ctypes.c_int, ctypes.c_longlong]
code = libc.mmap(None, 4096, 7, 0x1002, -1, 0)
if code == ctypes.c_void_p(-1).value:
    raise RuntimeError("mmap failed")

# Save edi; load dst, length, and pattern; call the commpage; restore edi.
instructions = ("\x57\x8b\x7c\x24\x08\x8b\x54\x24\x0c\x8b\x44\x24\x10"
                "\xf3\x0f\x6f\x00\xb8\x00\x10\xff\xff\xff\xd0\x5f\xc3")
ctypes.memmove(code, instructions, len(instructions))
fill = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p)(code)
pattern = ctypes.create_string_buffer("".join(chr(i * 17) for i in range(16)))

if len(sys.argv) > 1 and sys.argv[1] == "bench":
    buffer = ctypes.create_string_buffer(8 * 1024 * 1024)
    for i in range(256):
        fill(ctypes.addressof(buffer) + 1, len(buffer) - 2, pattern)
    print ord(buffer[-2])
else:
    buffer = ctypes.create_string_buffer(256)
    for offset in range(16):
        for length in range(201):
            ctypes.memset(buffer, 0x5a, len(buffer))
            fill(ctypes.addressof(buffer) + offset, length, pattern)
            expected = ("\x5a" * offset +
                        (pattern.raw[:16] * 13)[:length] +
                        "\x5a" * (len(buffer) - offset - length))
            if buffer.raw != expected:
                raise AssertionError("offset %d length %d" % (offset, length))
    buffer = ctypes.create_string_buffer(8256)
    for offset in (0, 1, 15):
        for length in (4095, 4096, 4097, 8191, 8192, 8193):
            ctypes.memset(buffer, 0x5a, len(buffer))
            fill(ctypes.addressof(buffer) + offset, length, pattern)
            expected = ("\x5a" * offset +
                        (pattern.raw[:16] * ((length + 15) // 16))[:length] +
                        "\x5a" * (len(buffer) - offset - length))
            if buffer.raw != expected:
                raise AssertionError("offset %d length %d" % (offset, length))
    print "pattern fill ok"
