// Draws the app's icon, cocaine's pile of powder (the same shape as the
// guest's menu bar logo, src/session/art.cpp), into an .iconset folder for
// iconutil. Usage: make-icon <folder>.iconset

import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

func drawIcon(size: Int) -> CGImage {
  let s = CGFloat(size)
  let ctx = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8, bytesPerRow: 0,
                      space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
  // The tile: Apple's grid puts the artwork in 824 of 1024, with rounded corners.
  let inset = s * 100 / 1024
  let tile = CGRect(x: inset, y: inset, width: s - 2 * inset, height: s - 2 * inset)
  let radius = tile.width * 0.225
  let path = CGPath(roundedRect: tile, cornerWidth: radius, cornerHeight: radius, transform: nil)
  ctx.saveGState()
  ctx.setShadow(offset: CGSize(width: 0, height: -s * 0.01), blur: s * 0.03,
                color: CGColor(gray: 0, alpha: 0.35))
  ctx.addPath(path)
  ctx.setFillColor(CGColor(gray: 0.18, alpha: 1))
  ctx.fillPath()
  ctx.restoreGState()
  ctx.saveGState()
  ctx.addPath(path)
  ctx.clip()
  let gradient = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(),
                            colors: [CGColor(gray: 0.30, alpha: 1), CGColor(gray: 0.12, alpha: 1)] as CFArray,
                            locations: [0, 1])!
  ctx.drawLinearGradient(gradient, start: CGPoint(x: 0, y: tile.maxY), end: CGPoint(x: 0, y: tile.minY), options: [])
  ctx.restoreGState()

  // The pile, in the lower middle of the tile.
  let box = CGRect(x: tile.minX + tile.width * 0.14, y: tile.minY + tile.height * 0.22,
                   width: tile.width * 0.72, height: tile.height * 0.46)
  func at(_ u: CGFloat, _ v: CGFloat) -> CGPoint { CGPoint(x: box.minX + u * box.width, y: box.minY + v * box.height) }
  let pile = CGMutablePath()
  pile.move(to: at(0.12, 0))
  pile.addCurve(to: at(0.48, 0.70), control1: at(0.18, 0.28), control2: at(0.30, 0.62))
  pile.addCurve(to: at(0.66, 0.62), control1: at(0.56, 0.73), control2: at(0.62, 0.70))
  pile.addCurve(to: at(0.88, 0.0), control1: at(0.74, 0.46), control2: at(0.82, 0.16))
  pile.closeSubpath()
  ctx.addPath(pile)
  ctx.setFillColor(CGColor(gray: 0.97, alpha: 1))
  ctx.fillPath()
  for (u, v, r) in [(0.03, 0.06, 0.05), (0.97, 0.05, 0.045), (0.95, 0.24, 0.035), (0.06, 0.26, 0.035)] {
    let m = at(CGFloat(u), CGFloat(v))
    let rr = CGFloat(r) * box.width
    ctx.fillEllipse(in: CGRect(x: m.x - rr, y: m.y - rr, width: 2 * rr, height: 2 * rr))
  }
  return ctx.makeImage()!
}

let folder = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "AppIcon.iconset"
try? FileManager.default.createDirectory(atPath: folder, withIntermediateDirectories: true)
for base in [16, 32, 128, 256, 512] {
  for scale in [1, 2] {
    let name = scale == 1 ? "icon_\(base)x\(base).png" : "icon_\(base)x\(base)@2x.png"
    let url = URL(fileURLWithPath: folder).appendingPathComponent(name) as CFURL
    let dest = CGImageDestinationCreateWithURL(url, UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(dest, drawIcon(size: base * scale), nil)
    CGImageDestinationFinalize(dest)
  }
}
