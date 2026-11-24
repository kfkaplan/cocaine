// The viewer: a real macOS window showing the guest system's display.
//
// The guest's window server draws into memory of its own. The cocaine process
// running it copies what changes into a small shared file (see
// Process::share_display) and counts the changes there, and this program,
// started alongside, maps the file and shows each new picture: it is handed
// to the window's layer, so this Mac's graphics hardware does the scaling.
// The mouse and the keyboard go back the other way down a pipe.

#include <algorithm>
#include <cmath>
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "viewer/viewer.h"

static NSSize viewer_content_size(uint32_t width, uint32_t height, CGFloat pixelScale, NSScreen* screen) {
  const CGFloat scale = screen ? screen.backingScaleFactor : 1;
  const NSRect room = screen ? screen.visibleFrame : NSMakeRect(0, 0, 1280, 800);
  const CGFloat w = width * pixelScale / scale, h = height * pixelScale / scale;
  const CGFloat fit = std::min({1.0, std::max(1.0, room.size.width - 40) / w,
                               std::max(1.0, room.size.height - 60) / h});
  return NSMakeSize(std::max(1.0, std::floor(w * fit)), std::max(1.0, std::floor(h * fit)));
}

@interface CocaineDisplayView : NSView {
 @public
  const uint8_t* pixels;
  const DisplayFileHeader* header;
  size_t pixelCapacity;
  int width, height;
  CGFloat pixelScale;
  bool bigEndian;
  pid_t writer;
  const volatile uint32_t* frame;  // the file's count of pictures
  uint32_t shown;                  // the count when the layer was last given one
  double scrollX, scrollY;         // scrolling not yet amounting to a whole line
  const volatile uint32_t* held;   // when the guest last held the pointer (DisplayFileHeader)
  bool holding;                    // the Mac's pointer is hidden and kept still for it
  bool manualCapture, suppressAutoHold, swallowCaptureKeyUp;
  bool skipMove;                   // the next movement includes putting the pointer back
  double moveX, moveY;             // movement not yet amounting to a whole point
}
- (void)show;
- (void)hold:(bool)hold;
@end

@implementation CocaineDisplayView
// Hands the layer the picture as it is now. The guest's pixels are 32 bits
// each with the unused byte first, in the guest CPU's byte order. They are
// copied first: the file goes on changing underneath. The frame number is
// odd while the writer is changing the picture, so a copy is kept only if
// the number was even before and the same after; otherwise the next tick
// tries again.
- (void)show {
  const uint32_t before = __atomic_load_n(frame, __ATOMIC_ACQUIRE);
  if (before & 1) return;
  const uint32_t w = __atomic_load_n(&header->width, __ATOMIC_RELAXED);
  const uint32_t h = __atomic_load_n(&header->height, __ATOMIC_RELAXED);
  const uint64_t bytes64 = (uint64_t)w * h * 4;
  if (!w || !h || bytes64 > pixelCapacity) return;
  const size_t bytes = (size_t)bytes64;
  CFDataRef copy = CFDataCreate(NULL, pixels, (CFIndex)bytes);
  // The copy's reads must be done before the number is read again. An
  // acquire load alone only holds back what comes after it, and this Mac's
  // CPU may otherwise finish the copy's reads later.
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  if (__atomic_load_n(frame, __ATOMIC_RELAXED) != before) {
    CFRelease(copy);
    return;
  }
  shown = before;
  if (width != (int)w || height != (int)h) {
    width = (int)w;
    height = (int)h;
    NSWindow* window = self.window;
    NSScreen* screen = window.screen ?: NSScreen.mainScreen;
    const NSSize size = viewer_content_size(w, h, pixelScale, screen);
    const NSRect old = window.frame;
    NSRect next = [window frameRectForContentRect:NSMakeRect(0, 0, size.width, size.height)];
    next.origin = NSMakePoint(NSMidX(old) - next.size.width / 2, NSMidY(old) - next.size.height / 2);
    if (screen) {
      const NSRect room = screen.visibleFrame;
      next.origin.x = std::clamp(next.origin.x, NSMinX(room), std::max(NSMinX(room), NSMaxX(room) - next.size.width));
      next.origin.y = std::clamp(next.origin.y, NSMinY(room), std::max(NSMinY(room), NSMaxY(room) - next.size.height));
    }
    window.contentAspectRatio = NSMakeSize(width, height);
    [window setFrame:next display:YES];
  }
  CGDataProviderRef data = CGDataProviderCreateWithCFData(copy);
  CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
  CGImageRef image = CGImageCreate(width, height, 8, 32, (size_t)width * 4, space,
                                   (CGBitmapInfo)kCGImageAlphaNoneSkipFirst |
                                       (bigEndian ? kCGBitmapByteOrder32Big : kCGBitmapByteOrder32Little),
                                   data, NULL, false, kCGRenderingIntentDefault);
  [CATransaction begin];
  [CATransaction setDisableActions:YES];  // no cross-fade between pictures
  self.layer.contents = (__bridge id)image;
  [CATransaction commit];
  CGImageRelease(image);
  CGColorSpaceRelease(space);
  CGDataProviderRelease(data);
  CFRelease(copy);
}
// The mouse and keys go back to the guest down the pipe this program was
// started with as its output, six numbers an event: its kind (as Mac OS X
// numbers events: 1 and 2 the left button down and up, 10 a key down, and so
// on), where on the guest's display, then three that depend on the kind.
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { return YES; }
- (void)send:(uint32_t)kind event:(NSEvent*)event {
  const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
  const NSSize size = self.bounds.size;
  if (size.width <= 0 || size.height <= 0) return;
  int32_t key = 0, extra = 0, more = 0;
  if (kind >= 10 && kind <= 12) {
    // The key code, which Macs have kept the same since, then the modifier
    // keys held (also unchanged) with the character typed below them, and
    // whether the key is repeating.
    key = event.keyCode;
    extra = (int32_t)(event.modifierFlags & 0xFFFF0000u);
    if (kind != 12) {
      if (event.charactersIgnoringModifiers.length) {
        extra |= [event.charactersIgnoringModifiers characterAtIndex:0] & 0xFFFF;
      }
      more = event.isARepeat;
    }
  } else if (kind >= 1 && kind <= 4) {
    more = (int32_t)event.clickCount;  // 2 for the second click of a double click
  } else if (kind >= 5 && kind <= 7) {
    // Scale movement with the displayed picture unless a game has captured
    // the pointer for camera controls; keep fractions between events.
    if (skipMove) {
      skipMove = false;
    } else {
      moveX += event.deltaX * (holding ? 1 : width / size.width);
      moveY += event.deltaY * (holding ? 1 : height / size.height);
    }
    key = (int32_t)moveX;
    extra = (int32_t)moveY;
    moveX -= key;
    moveY -= extra;
    more = 1;
  } else if (kind == 22) {
    // Whole lines, down then across; a trackpad's fine movement is saved up.
    const BOOL fine = event.hasPreciseScrollingDeltas;
    scrollY += fine ? event.scrollingDeltaY / 10 : event.scrollingDeltaY;
    scrollX += fine ? event.scrollingDeltaX / 10 : event.scrollingDeltaX;
    key = (int32_t)scrollY;
    more = (int32_t)scrollX;
    if (!key && !more) return;
    scrollY -= key;
    scrollX -= more;
  }
  const int32_t record[6] = {(int32_t)kind, (int32_t)(p.x * width / size.width),
                             (int32_t)((size.height - p.y) * height / size.height), key, extra, more};
  (void)write(STDOUT_FILENO, record, sizeof record);
}
- (void)keyDown:(NSEvent*)event {
  const NSEventModifierFlags modifiers = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
  if (event.keyCode == 46 && modifiers == (NSEventModifierFlagCommand | NSEventModifierFlagOption)) {
    if (!event.isARepeat) {
      manualCapture = !holding;
      suppressAutoHold = holding;
      [self hold:manualCapture];
    }
    swallowCaptureKeyUp = true;
    return;
  }
  [self send:10 event:event];
}
- (void)keyUp:(NSEvent*)event {
  if (event.keyCode == 46 && swallowCaptureKeyUp) {
    swallowCaptureKeyUp = false;
    return;
  }
  [self send:11 event:event];
}
- (void)flagsChanged:(NSEvent*)event { [self send:12 event:event]; }
- (void)mouseDown:(NSEvent*)event { [self send:1 event:event]; }
- (void)mouseUp:(NSEvent*)event { [self send:2 event:event]; }
- (void)rightMouseDown:(NSEvent*)event { [self send:3 event:event]; }
- (void)rightMouseUp:(NSEvent*)event { [self send:4 event:event]; }
- (void)mouseMoved:(NSEvent*)event { [self send:5 event:event]; }
- (void)mouseDragged:(NSEvent*)event { [self send:6 event:event]; }
- (void)rightMouseDragged:(NSEvent*)event { [self send:7 event:event]; }
- (void)scrollWheel:(NSEvent*)event { [self send:22 event:event]; }
// While a program in the guest holds the pointer, as a game looking around
// with the mouse does, the Mac's pointer is hidden and kept still in the
// middle of the window, so it can't wander off and click another app; the
// mouse's movement still comes through. It comes back once the guest has
// let go for a second and a half, or this app isn't the one in front.
- (void)hold:(bool)hold {
  if (hold == holding) return;
  holding = hold;
  moveX = moveY = 0;
  skipMove = false;
  const int32_t record[6] = {(int32_t)kViewerCaptureChanged, hold ? 1 : 0, 0, 0, 0, 0};
  (void)write(STDOUT_FILENO, record, sizeof record);
  self.window.title = hold ? @"cocaine: Mac OS X 10.5 - mouse captured (Cmd-Option-M to release)"
                           : @"cocaine: Mac OS X 10.5";
  if (hold) {
    const NSRect r = [self.window convertRectToScreen:[self convertRect:self.bounds toView:nil]];
    const CGFloat top = NSScreen.screens.count ? NSMaxY(NSScreen.screens[0].frame) : 0;
    CGWarpMouseCursorPosition(CGPointMake(NSMidX(r), top - NSMidY(r)));
    CGAssociateMouseAndMouseCursorPosition(false);
    [NSCursor hide];
    skipMove = true;
  } else {
    CGAssociateMouseAndMouseCursorPosition(true);
    [NSCursor unhide];
  }
}
- (void)tick:(NSTimer*)timer {
  if (__atomic_load_n(frame, __ATOMIC_ACQUIRE) != shown) [self show];
  const uint32_t at = __atomic_load_n(held, __ATOMIC_ACQUIRE);
  const uint32_t now = (uint32_t)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000);
  const bool active = NSApp.isActive && self.window.isKeyWindow;
  if (!active) {
    manualCapture = false;
    suppressAutoHold = false;
  }
  [self hold:active && (manualCapture || (!suppressAutoHold && at != 0 && now - at < 1500))];
  static int ticks;
  if (++ticks % 60 == 0 && kill(writer, 0) != 0) {
    [self hold:false];
    [NSApp terminate:nil];
  }
}
@end

int run_viewer(const char* path) {
  const int fd = open(path, O_RDONLY);
  struct stat st;
  if (fd < 0 || fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(DisplayFileHeader)) return 1;
  const uint8_t* map = (const uint8_t*)mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) return 1;
  const DisplayFileHeader* head = (const DisplayFileHeader*)map;
  if (head->magic != kDisplayFileMagic || !head->width || !head->height ||
      head->width > 4096 || head->height > 4096 ||
      (off_t)(sizeof *head + (size_t)head->width * head->height * 4) > st.st_size) {
    fprintf(stderr, "invalid viewer display file\n");
    return 1;
  }
  unlink(path);  // it lives on for as long as the two programs have it mapped

  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    // Two screen pixels to each guest pixel where that fits (a Retina
    // screen has two of its pixels to a point).
    NSScreen* screen = NSScreen.mainScreen;
    const NSRect room = screen ? screen.visibleFrame : NSMakeRect(0, 0, 1280, 800);
    const char* doublePixels = getenv("COCAINE_VIEWER_DOUBLE");
    if (doublePixels && strcmp(doublePixels, "0") != 0 && strcmp(doublePixels, "1") != 0) {
      fprintf(stderr, "COCAINE_VIEWER_DOUBLE must be 0 or 1\n");
      return 1;
    }
    const CGFloat pixelScale = doublePixels && strcmp(doublePixels, "0") == 0 ? 1 : 2;
    const NSSize size = viewer_content_size(head->width, head->height, pixelScale, screen);
    const CGFloat w = size.width, h = size.height;
    const NSRect frame = NSMakeRect(room.origin.x + (room.size.width - w) / 2,
                                    room.origin.y + (room.size.height - h) / 2, w, h);
    NSWindow* window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    window.title = @"cocaine: Mac OS X 10.5";
    window.contentAspectRatio = NSMakeSize(head->width, head->height);
    CocaineDisplayView* view = [[CocaineDisplayView alloc] initWithFrame:frame];
    view->pixels = map + sizeof *head;
    view->header = head;
    view->pixelCapacity = (size_t)st.st_size - sizeof *head;
    view->width = (int)head->width;
    view->height = (int)head->height;
    view->pixelScale = pixelScale;
    view->bigEndian = head->big_endian != 0;
    view->writer = (pid_t)head->writer;
    view->frame = &head->frame;
    view->held = &head->pointer_held;
    view.layer = [CALayer layer];  // a layer of the view's own, which AppKit then leaves alone
    view.wantsLayer = YES;
    view.layer.contentsGravity = kCAGravityResize;
    view.layer.magnificationFilter = pixelScale == 2 ? kCAFilterNearest : kCAFilterLinear;
    view.layer.minificationFilter = kCAFilterTrilinear;
    view.layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    window.contentView = view;
    [view show];
    window.acceptsMouseMovedEvents = YES;
    [window makeFirstResponder:view];
    signal(SIGPIPE, SIG_IGN);
    [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification
                                                      object:window
                                                       queue:nil
                                                  usingBlock:^(NSNotification* note) {
                                                    // Closing the window ends the whole run:
                                                    // say so (kind 0) before going.
                                                    [view hold:false];
                                                    const int32_t closed[6] = {0, 0, 0, 0, 0, 0};
                                                    (void)write(STDOUT_FILENO, closed, sizeof closed);
                                                    [NSApp terminate:nil];
                                                  }];
    // Twice a screen's usual refresh rate; a tick with nothing new costs nothing.
    NSTimer* timer = [NSTimer timerWithTimeInterval:1.0 / 120 target:view selector:@selector(tick:) userInfo:nil repeats:YES];
    // In the common modes, so that the picture keeps moving while a mouse
    // button is held down in the window.
    [[NSRunLoop currentRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
    // For checking the viewer without looking at it: COCAINE_VIEWER_SNAPSHOT
    // names a file to write what the window shows to, twelve seconds in.
    if (const char* snapshot = getenv("COCAINE_VIEWER_SNAPSHOT")) {
      NSString* to = [NSString stringWithUTF8String:snapshot];
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 12 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        const NSSize size = view.bounds.size;
        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
                                                                        pixelsWide:(NSInteger)size.width
                                                                        pixelsHigh:(NSInteger)size.height
                                                                     bitsPerSample:8
                                                                   samplesPerPixel:4
                                                                          hasAlpha:YES
                                                                          isPlanar:NO
                                                                    colorSpaceName:NSDeviceRGBColorSpace
                                                                       bytesPerRow:0
                                                                      bitsPerPixel:0];
        NSGraphicsContext* context = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
        [view.layer renderInContext:context.CGContext];
        [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:to atomically:YES];
      });
    }
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [NSApp run];
  }
  return 0;
}
