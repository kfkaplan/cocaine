import ctypes
import sys

cgl = ctypes.CDLL("/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL")
gl = ctypes.CDLL("/System/Library/Frameworks/OpenGL.framework/Versions/A/Libraries/libGL.dylib")
glu = ctypes.CDLL("/System/Library/Frameworks/OpenGL.framework/Versions/A/Libraries/libGLU.dylib")
uint = ctypes.c_uint
intp = ctypes.POINTER(ctypes.c_int)
uintp = ctypes.POINTER(uint)

cgl.CGLChoosePixelFormat.argtypes = [intp, uintp, intp]
cgl.CGLDescribePixelFormat.argtypes = [uint, ctypes.c_int, uint, intp]
cgl.CGLRetainPixelFormat.argtypes = [uint]
cgl.CGLRetainPixelFormat.restype = uint
cgl.CGLReleasePixelFormat.argtypes = [uint]
cgl.CGLDestroyPixelFormat.argtypes = [uint]
cgl.CGLCreateContext.argtypes = [uint, uint, uintp]
cgl.CGLRetainContext.argtypes = [uint]
cgl.CGLRetainContext.restype = uint
cgl.CGLReleaseContext.argtypes = [uint]
cgl.CGLDestroyContext.argtypes = [uint]
cgl.CGLSetParameter.argtypes = [uint, uint, intp]
cgl.CGLSetFullScreen.argtypes = [uint]
# Leopard 10.5.0's OpenGL doesn't have this one yet; later updates do.
has_on_display = hasattr(cgl, "CGLSetFullScreenOnDisplay")
if has_on_display:
    cgl.CGLSetFullScreenOnDisplay.argtypes = [uint, uint]
cgl.CGLClearDrawable.argtypes = [uint]
cgl.CGLFlushDrawable.argtypes = [uint]
gl.CGLSetCurrentContext.argtypes = [uint]
gl.CGLGetCurrentContext.restype = uint
gl.glMatrixMode.argtypes = [uint]
gl.glGetDoublev.argtypes = [uint, ctypes.POINTER(ctypes.c_double)]
gl.glGetIntegerv.argtypes = [uint, intp]
gl.glPixelStorei.argtypes = [uint, ctypes.c_int]
gl.glPushClientAttrib.argtypes = [uint]
gl.glGenLists.argtypes = [ctypes.c_int]
gl.glGenLists.restype = uint
gl.glNewList.argtypes = [uint, uint]
gl.glClearColor.argtypes = [ctypes.c_float] * 4
gl.glClear.argtypes = [uint]
gl.glCallLists.argtypes = [ctypes.c_int, uint, ctypes.c_void_p]
gl.glListBase.argtypes = [uint]
gl.glReadPixels.argtypes = [ctypes.c_int] * 4 + [uint, uint, ctypes.c_void_p]
gl.glViewport.argtypes = [ctypes.c_int] * 4
gl.glScissor.argtypes = [ctypes.c_int] * 4
gl.glColor4f.argtypes = [ctypes.c_float] * 4
gl.glVertexPointer.argtypes = [ctypes.c_int, uint, ctypes.c_int, ctypes.c_void_p]
gl.glDrawRangeElements.argtypes = [uint, uint, uint, ctypes.c_int, uint, ctypes.c_void_p]
glu.gluPerspective.argtypes = [ctypes.c_double] * 4
glu.gluNewQuadric.restype = uint
glu.gluDeleteQuadric.argtypes = [uint]
glu.gluQuadricNormals.argtypes = [uint, uint]
glu.gluQuadricTexture.argtypes = [uint, ctypes.c_ubyte]
glu.gluSphere.argtypes = [uint, ctypes.c_double, ctypes.c_int, ctypes.c_int]
glu.gluCylinder.argtypes = [uint, ctypes.c_double, ctypes.c_double, ctypes.c_double, ctypes.c_int, ctypes.c_int]
glu.gluBuild2DMipmaps.argtypes = [uint, ctypes.c_int, ctypes.c_int, ctypes.c_int, uint, uint, ctypes.c_void_p]

attrs = (ctypes.c_int * 3)(5, 73, 0)
fmt = uint()
count = ctypes.c_int()
assert cgl.CGLChoosePixelFormat(attrs, ctypes.byref(fmt), ctypes.byref(count)) == 0
assert fmt.value and count.value == 1
value = ctypes.c_int()
assert cgl.CGLDescribePixelFormat(fmt, 0, 70, ctypes.byref(value)) == 0 and value.value
assert cgl.CGLRetainPixelFormat(fmt) == fmt.value
cgl.CGLReleasePixelFormat(fmt)
ctx = uint()
assert cgl.CGLCreateContext(fmt, 0, ctypes.byref(ctx)) == 0 and ctx.value
assert cgl.CGLDestroyPixelFormat(fmt) == 0
assert cgl.CGLDestroyPixelFormat(fmt) == 10002
assert cgl.CGLRetainContext(ctx) == ctx.value
cgl.CGLReleaseContext(ctx)
assert gl.CGLSetCurrentContext(ctx) == 0
assert gl.CGLGetCurrentContext() == ctx.value
assert cgl.CGLSetParameter(ctx, 9999, ctypes.byref(value)) == 10010

gl.glPixelStorei(0x0cf0, 0)
gl.glPushClientAttrib(1)
gl.glPixelStorei(0x0cf0, 1)
gl.glPopClientAttrib()
gl.glGetIntegerv(0x0cf0, ctypes.byref(value))
assert value.value == 0

gl.glMatrixMode(0x1701)
gl.glLoadIdentity()
glu.gluPerspective(90.0, 2.0, 1.0, 11.0)
matrix = (ctypes.c_double * 16)()
gl.glGetDoublev(0x0ba7, matrix)
assert abs(matrix[0] - 0.5) < 0.00001
assert abs(matrix[5] - 1.0) < 0.00001
q = glu.gluNewQuadric()
assert q
glu.gluQuadricNormals(q, 100000)
glu.gluQuadricTexture(q, 1)
glu.gluSphere(q, 1.0, 8, 8)
glu.gluCylinder(q, 1.0, 0.5, 2.0, 8, 8)
assert gl.glGetError() == 0
glu.gluDeleteQuadric(q)

texture = (ctypes.c_ubyte * 16)(*([255, 0, 0, 255, 0, 0, 0, 0] * 2))
assert glu.gluBuild2DMipmaps(0x0de1, 3, 2, 2, 0x1907, 0x1401, texture) == 0
assert glu.gluBuild2DMipmaps(0x0de1, 3, 2, 2, 0x1907, 0x1401, ctypes.c_void_p(1)) == 100901
lists = gl.glGenLists(2)
assert lists
gl.glNewList(lists + 1, 0x1300)
gl.glClearColor(1.0, 0.0, 0.0, 1.0)
gl.glClear(0x4000)
gl.glEndList()
gl.glListBase(lists)
indices = (ctypes.c_ushort * 1)(1)
gl.glCallLists(1, 0x1403, indices)
pixel = (ctypes.c_ubyte * 4)()
gl.glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, pixel)
assert tuple(pixel) == (255, 0, 0, 255), tuple(pixel)

gl.glGetString.argtypes = [uint]
gl.glGetString.restype = ctypes.c_char_p
extensions = gl.glGetString(0x1f03).split()
assert "GL_ARB_texture_compression" in extensions
assert "GL_EXT_texture_compression_s3tc" in extensions
gl.glGetIntegerv(0x86a2, ctypes.byref(value))
assert value.value > 0
formats = (ctypes.c_int * value.value)()
gl.glGetIntegerv(0x86a3, formats)
assert all(format in formats for format in (0x83f0, 0x83f2, 0x83f3)), tuple(formats)
gl.glGenTextures.argtypes = [ctypes.c_int, uintp]
gl.glBindTexture.argtypes = [uint, uint]
gl.glTexParameteri.argtypes = [uint, uint, ctypes.c_int]
gl.glGetTexLevelParameteriv.argtypes = [uint, ctypes.c_int, uint, intp]
gl.glTexCoord2f.argtypes = [ctypes.c_float] * 2
gl.glTexCoord2fv.argtypes = [ctypes.POINTER(ctypes.c_float)]
gl.glVertex2f.argtypes = [ctypes.c_float] * 2
gl.glTexImage2D.argtypes = [uint, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                            ctypes.c_int, uint, uint, ctypes.c_void_p]
for suffix in ("", "ARB"):
    getattr(gl, "glCompressedTexImage2D" + suffix).argtypes = [
        uint, ctypes.c_int, uint] + [ctypes.c_int] * 4 + [ctypes.c_void_p]
    getattr(gl, "glCompressedTexSubImage2D" + suffix).argtypes = [
        uint] + [ctypes.c_int] * 5 + [uint, ctypes.c_int, ctypes.c_void_p]
    getattr(gl, "glGetCompressedTexImage" + suffix).argtypes = [uint, ctypes.c_int, ctypes.c_void_p]
texture_name = uint()
gl.glGenTextures(1, ctypes.byref(texture_name))
gl.glBindTexture(0x0de1, texture_name)
gl.glTexParameteri(0x0de1, 0x2801, 0x2600)
gl.glTexParameteri(0x0de1, 0x2800, 0x2600)
gl.glMatrixMode(0x1701)
gl.glLoadIdentity()
gl.glMatrixMode(0x1700)
gl.glLoadIdentity()
gl.glViewport(0, 0, 1, 1)
gl.glEnable(0x0de1)
gl.glColor4f(1, 1, 1, 1)

def textured_pixel(coordinates=None):
    gl.glBegin(7)
    for x, y, u, v in ((-1, -1, 0, 0), (1, -1, 1, 0), (1, 1, 1, 1), (-1, 1, 0, 1)):
        if coordinates is None:
            gl.glTexCoord2f(u, v)
        else:
            gl.glTexCoord2fv(coordinates)
        gl.glVertex2f(x, y)
    gl.glEnd()
    gl.glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, pixel)
    return tuple(pixel)

red_block = "\x00\xf8\x1f\x00\x00\x00\x00\x00"
for suffix in ("", "ARB"):
    for format, block, alpha in ((0x83f1, red_block, 255),
                                 (0x83f2, "\xff" * 8 + red_block, 255),
                                 (0x83f3, "\x80" + "\x00" * 7 + red_block, 128)):
        getattr(gl, "glCompressedTexImage2D" + suffix)(0x0de1, 0, format, 4, 4, 0, len(block), ctypes.c_char_p(block))
        assert gl.glGetError() == 0
        gl.glGetTexLevelParameteriv(0x0de1, 0, 0x1000, ctypes.byref(value))
        assert value.value == 4, value.value
        gl.glGetTexLevelParameteriv(0x0de1, 0, 0x86a0, ctypes.byref(value))
        assert value.value == len(block), value.value
        copied = ctypes.create_string_buffer(len(block))
        getattr(gl, "glGetCompressedTexImage" + suffix)(0x0de1, 0, copied)
        assert copied.raw == block, repr(copied.raw)
        assert textured_pixel() == (255, 0, 0, alpha), tuple(pixel)
    blue_block = "\xff" + "\x00" * 7 + "\x1f\x00\x00\xf8" + "\x00" * 4
    getattr(gl, "glCompressedTexSubImage2D" + suffix)(0x0de1, 0, 0, 0, 4, 4, 0x83f3, len(blue_block), ctypes.c_char_p(blue_block))
    assert gl.glGetError() == 0
    assert textured_pixel() == (0, 0, 255, 255), tuple(pixel)
texels = (ctypes.c_ubyte * 16)(255, 0, 0, 255, 0, 255, 0, 255,
                               0, 0, 255, 255, 255, 255, 255, 255)
gl.glTexImage2D(0x0de1, 0, 0x1908, 2, 2, 0, 0x1908, 0x1401, texels)
assert textured_pixel((ctypes.c_float * 2)(0.75, 0.25)) == (0, 255, 0, 255), tuple(pixel)
assert textured_pixel((ctypes.c_float * 2)(0.25, 0.75)) == (0, 0, 255, 255), tuple(pixel)
gl.glDisable(0x0de1)
gl.glBindTexture(0x0de1, 0)

cg = ctypes.CDLL("/System/Library/Frameworks/ApplicationServices.framework/Versions/A/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics")
cg.CGMainDisplayID.restype = uint
cg.CGDisplayIDToOpenGLDisplayMask.argtypes = [uint]
cg.CGDisplayIDToOpenGLDisplayMask.restype = uint
cg.CGDisplayCapture.argtypes = [uint]
cg.CGDisplayRelease.argtypes = [uint]
cg.CGDisplayBaseAddress.argtypes = [uint]
cg.CGDisplayBaseAddress.restype = ctypes.c_void_p
cg.CGDisplayBytesPerRow.argtypes = [uint]
cg.CGDisplayPixelsWide.argtypes = [uint]
cg.CGDisplayPixelsHigh.argtypes = [uint]
display = cg.CGMainDisplayID()
assert cg.CGDisplayCapture(display) == 0
try:
    assert cgl.CGLSetFullScreen(0) == 10004
    if has_on_display:
        assert cgl.CGLSetFullScreenOnDisplay(ctx, 0) == 10006
    assert cgl.CGLSetFullScreen(ctx) == 0
    mask = cg.CGDisplayIDToOpenGLDisplayMask(display)
    if has_on_display:
        assert cgl.CGLSetFullScreenOnDisplay(ctx, mask) == 0
    gl.glViewport(0, 0, 16, 16)
    gl.glClearColor(1.0, 0.0, 0.0, 1.0)
    gl.glClear(0x4000)
    gl.glEnable(0x0c11)
    gl.glScissor(0, 0, 1, 1)
    gl.glClearColor(0.0, 1.0, 0.0, 1.0)
    gl.glClear(0x4000)
    gl.glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, pixel)
    assert tuple(pixel) == (0, 255, 0, 255), tuple(pixel)
    gl.glReadPixels(2, 2, 1, 1, 0x1908, 0x1401, pixel)
    assert tuple(pixel) == (255, 0, 0, 255), tuple(pixel)
    gl.glDisable(0x0c11)
    gl.glMatrixMode(0x1701)
    gl.glLoadIdentity()
    gl.glMatrixMode(0x1700)
    gl.glLoadIdentity()
    vertices = (ctypes.c_float * 6)(-1, -1, 3, -1, -1, 3)
    elements = (ctypes.c_ushort * 3)(0, 1, 2)
    gl.glVertexPointer(2, 0x1406, 0, vertices)
    gl.glEnableClientState(0x8074)
    gl.glColor4f(0.0, 0.0, 1.0, 1.0)
    gl.glDrawRangeElements(4, 0, 2, 3, 0x1403, elements)
    gl.glReadPixels(8, 8, 1, 1, 0x1908, 0x1401, pixel)
    assert tuple(pixel) == (0, 0, 255, 255), tuple(pixel)
    padded = (ctypes.c_float * 24)()
    for index, x, y in ((2, -1, -1), (4, 3, -1), (7, -1, 3)):
        padded[3 * index] = x
        padded[3 * index + 1] = y
    sparse = (ctypes.c_ushort * 3)(2, 4, 7)
    gl.glVertexPointer(2, 0x1406, 12, padded)
    gl.glColor4f(0.0, 1.0, 0.0, 1.0)
    gl.glDrawRangeElements(4, 2, 7, 3, 0x1403, sparse)
    gl.glReadPixels(8, 8, 1, 1, 0x1908, 0x1401, pixel)
    assert tuple(pixel) == (0, 255, 0, 255), tuple(pixel)
    assert gl.glGetError() == 0
    assert cgl.CGLFlushDrawable(ctx) == 0
    width = cg.CGDisplayPixelsWide(display)
    height = cg.CGDisplayPixelsHigh(display)
    stride = cg.CGDisplayBytesPerRow(display)
    base = cg.CGDisplayBaseAddress(display)
    shown = ctypes.string_at(base + (height // 2) * stride + (width // 2) * 4, 4)
    rgb = shown[1:4] if sys.byteorder == "big" else shown[2::-1]
    assert rgb == "\xff\x00\x00", repr(shown)
    assert cgl.CGLClearDrawable(ctx) == 0
    assert cgl.CGLFlushDrawable(ctx) == 10005
finally:
    assert cg.CGDisplayRelease(display) == 0
assert gl.CGLSetCurrentContext(0) == 0
assert cgl.CGLDestroyContext(ctx) == 0
assert cgl.CGLDestroyContext(ctx) == 10004
print "CGL compatibility ok"
