// Draws the two app icons (client and server) as .icns files, so they can be
// remade from code:
//
//   swiftc -O packaging/macos/icons/make_icons.swift -o /tmp/make_icons
//   /tmp/make_icons packaging/macos/icons
//
// writes RDMachine.icns (client: blue, a display outline) and
// RDMachineServer.icns (server: graphite, the same display with a green dot),
// on Apple's icon grid (an 824-point rounded square in a 1024 canvas).
import AppKit

struct Style {
  let top: NSColor, bottom: NSColor
  let accent: NSColor
  let server: Bool
}

func rgb(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat) -> NSColor {
  NSColor(srgbRed: r / 255, green: g / 255, blue: b / 255, alpha: 1)
}

func drawIcon(_ s: Style, size: CGFloat) -> NSBitmapImageRep {
  let px = Int(size)
  let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: px, pixelsHigh: px, bitsPerSample: 8,
                             samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
                             bytesPerRow: 0, bitsPerPixel: 0)!
  NSGraphicsContext.saveGraphicsState()
  NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
  let k = size / 1024  // everything below is on the 1024 grid
  let ctx = NSGraphicsContext.current!.cgContext
  ctx.scaleBy(x: k, y: k)

  // Body: the rounded square, a soft vertical gradient, a faint shadow.
  let body = NSRect(x: 100, y: 100, width: 824, height: 824)
  let bodyPath = NSBezierPath(roundedRect: body, xRadius: 185, yRadius: 185)
  NSGraphicsContext.saveGraphicsState()
  let shadow = NSShadow()
  shadow.shadowColor = NSColor.black.withAlphaComponent(0.28)
  shadow.shadowOffset = NSSize(width: 0, height: -10)
  shadow.shadowBlurRadius = 24
  shadow.set()
  s.bottom.setFill()
  bodyPath.fill()
  NSGraphicsContext.restoreGraphicsState()
  NSGradient(starting: s.top, ending: s.bottom)!.draw(in: bodyPath, angle: -90)
  // The mark: a display outlined in white, and its base. The server adds
  // one dot, its "on" light.
  let white = NSColor.white
  let screen = NSBezierPath(roundedRect: NSRect(x: 262, y: 400, width: 500, height: 330), xRadius: 54, yRadius: 54)
  screen.lineWidth = 52
  white.setStroke()
  screen.stroke()
  let base = NSBezierPath(roundedRect: NSRect(x: 392, y: 286, width: 240, height: 48), xRadius: 24, yRadius: 24)
  white.setFill()
  base.fill()
  if s.server {
    s.accent.setFill()
    NSBezierPath(ovalIn: NSRect(x: 512 - 44, y: 565 - 44, width: 88, height: 88)).fill()
  }
  NSGraphicsContext.restoreGraphicsState()
  return rep
}

func writeIcns(_ s: Style, name: String, dir: String) throws {
  let iconset = (NSTemporaryDirectory() as NSString).appendingPathComponent("\(name).iconset")
  try? FileManager.default.removeItem(atPath: iconset)
  try FileManager.default.createDirectory(atPath: iconset, withIntermediateDirectories: true)
  for base in [16, 32, 128, 256, 512] {
    for scale in [1, 2] {
      let rep = drawIcon(s, size: CGFloat(base * scale))
      let file = scale == 1 ? "icon_\(base)x\(base).png" : "icon_\(base)x\(base)@2x.png"
      try rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: iconset).appendingPathComponent(file))
    }
  }
  let out = (dir as NSString).appendingPathComponent("\(name).icns")
  let task = Process()
  task.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
  task.arguments = ["-c", "icns", iconset, "-o", out]
  try task.run()
  task.waitUntilExit()
  // A preview next to it, for looking at in a browser or a review.
  try drawIcon(s, size: 1024).representation(using: .png, properties: [:])!
    .write(to: URL(fileURLWithPath: (dir as NSString).appendingPathComponent("\(name)-1024.png")))
  print(out)
}

let dir = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "."
let client = Style(top: rgb(78, 148, 255), bottom: rgb(28, 82, 214), accent: rgb(92, 200, 255), server: false)
let server = Style(top: rgb(74, 86, 104), bottom: rgb(26, 32, 42), accent: rgb(64, 214, 170), server: true)
try writeIcns(client, name: "RDMachine", dir: dir)
try writeIcns(server, name: "RDMachineServer", dir: dir)
