#include "../src/viewer/viewer.mm"

#include <cstdio>
#include <vector>

int main() {
  @autoreleasepool {
    [NSApplication sharedApplication];
    NSScreen* screen = NSScreen.mainScreen;
    if (!screen) {
      fprintf(stderr, "viewer resize test skipped: no screen\n");
      return 77;
    }
    const NSRect room = screen.visibleFrame;
    NSWindow* window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(NSMidX(room) - 200, NSMidY(room) - 150, 400, 300)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    CocaineDisplayView* view = [[CocaineDisplayView alloc] initWithFrame:window.contentView.bounds];
    view.wantsLayer = YES;
    window.contentView = view;
    DisplayFileHeader header = {kDisplayFileMagic, 800, 600, 1, (uint32_t)getpid(), 0, 0, 0};
    std::vector<uint8_t> pixels(1280 * 768 * 4, 0x7f);
    view->pixels = pixels.data();
    view->header = &header;
    view->pixelCapacity = pixels.size();
    view->width = 800;
    view->height = 600;
    view->pixelScale = 2;
    view->bigEndian = true;
    view->frame = &header.frame;
    window.contentAspectRatio = NSMakeSize(800, 600);

    const uint32_t modes[][2] = {{1024, 768}, {640, 480}, {1280, 720}, {800, 600}};
    for (CGFloat pixelScale : {2.0, 1.0}) {
      view->pixelScale = pixelScale;
      for (const auto& mode : modes) {
        header.width = mode[0];
        header.height = mode[1];
        header.frame += 2;
        [view show];
        NSScreen* current = window.screen ?: NSScreen.mainScreen;
        const NSRect visible = current.visibleFrame;
        const CGFloat scale = current.backingScaleFactor;
        const CGFloat w = mode[0] * pixelScale / scale, h = mode[1] * pixelScale / scale;
        const CGFloat fit = std::min({1.0, std::max(1.0, visible.size.width - 40) / w,
                                     std::max(1.0, visible.size.height - 60) / h});
        const NSSize actual = [window contentRectForFrameRect:window.frame].size;
        if (view->width != (int)mode[0] || view->height != (int)mode[1] ||
            std::abs(actual.width - std::max(1.0, std::floor(w * fit))) > 1 ||
            std::abs(actual.height - std::max(1.0, std::floor(h * fit))) > 1 ||
            std::abs(view.bounds.size.width - actual.width) > 1 ||
            std::abs(view.bounds.size.height - actual.height) > 1 ||
            NSMinX(window.frame) < NSMinX(visible) || NSMaxX(window.frame) > NSMaxX(visible) ||
            NSMinY(window.frame) < NSMinY(visible) || NSMaxY(window.frame) > NSMaxY(visible)) {
          fprintf(stderr, "wrong window size for %ux%u at %.0fx: %.1fx%.1f\n",
                  mode[0], mode[1], pixelScale, actual.width, actual.height);
          return 1;
        }
      }
    }
  }
  return 0;
}
