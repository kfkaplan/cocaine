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
    assert cgl.CGLSetFullScreenOnDisplay(ctx, 0) == 10006
    assert cgl.CGLSetFullScreen(ctx) == 0
    mask = cg.CGDisplayIDToOpenGLDisplayMask(display)
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
