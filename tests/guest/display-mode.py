import ctypes
import sys
import time

cg = ctypes.CDLL("/System/Library/Frameworks/ApplicationServices.framework/Frameworks/CoreGraphics.framework/CoreGraphics")
cf = ctypes.CDLL("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
uint = ctypes.c_uint
ptr = ctypes.c_void_p
cg.CGMainDisplayID.restype = uint
cg.CGDisplayCurrentMode.argtypes = [uint]
cg.CGDisplayCurrentMode.restype = ptr
cg.CGDisplayBestModeForParameters.argtypes = [uint, uint, uint, uint, ctypes.POINTER(uint)]
cg.CGDisplayBestModeForParameters.restype = ptr
cg.CGDisplaySwitchToMode.argtypes = [uint, ptr]
cg.CGDisplayPixelsWide.argtypes = [uint]
cg.CGDisplayPixelsHigh.argtypes = [uint]
cf.CFRetain.argtypes = [ptr]
cf.CFRetain.restype = ptr
cf.CFRelease.argtypes = [ptr]

display = cg.CGMainDisplayID()
width, height, bits, original_width, original_height = map(int, sys.argv[1:])
original = cg.CGDisplayCurrentMode(display)
assert original
cf.CFRetain(original)
exact = uint()
mode = cg.CGDisplayBestModeForParameters(display, bits, width, height, ctypes.byref(exact))
assert mode and exact.value
assert cg.CGDisplaySwitchToMode(display, mode) == 0
assert cg.CGDisplayPixelsWide(display) == width and cg.CGDisplayPixelsHigh(display) == height
time.sleep(30)
assert cg.CGDisplaySwitchToMode(display, original) == 0
assert cg.CGDisplayPixelsWide(display) == original_width and cg.CGDisplayPixelsHigh(display) == original_height
time.sleep(25)
cf.CFRelease(original)
print "display mode switch ok"
