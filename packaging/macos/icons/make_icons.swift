// Draws the two app icons (client and server) as .icns files, so they can be
// remade from code:
//
//   swiftc -O packaging/macos/icons/make_icons.swift -o /tmp/make_icons
//   /tmp/make_icons packaging/macos/icons
//
// writes RDMachine.icns (client: blue, a display with a pointer) and
// RDMachineServer.icns (server: graphite, a display sending out a signal),
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
  // A light rim along the top edge.
  NSGraphicsContext.saveGraphicsState()
  bodyPath.addClip()
  NSGradient(starting: NSColor.white.withAlphaComponent(0.18), ending: NSColor.white.withAlphaComponent(0))!
    .draw(in: NSRect(x: 100, y: 600, width: 824, height: 324), angle: -90)
  NSGraphicsContext.restoreGraphicsState()

  // The display: a white bezel with a dark screen, on a stand.
  let white = NSColor.white
  let screenOuter = NSRect(x: 232, y: 372, width: 560, height: 380)
  let bezel = NSBezierPath(roundedRect: screenOuter, xRadius: 46, yRadius: 46)
  white.setFill()
  bezel.fill()
  let screen = screenOuter.insetBy(dx: 28, dy: 28)
  let screenPath = NSBezierPath(roundedRect: screen, xRadius: 22, yRadius: 22)
  NSGradient(starting: s.top.blended(withFraction: 0.55, of: .black)!,
             ending: s.bottom.blended(withFraction: 0.7, of: .black)!)!.draw(in: screenPath, angle: -90)
  let neck = NSBezierPath(roundedRect: NSRect(x: 466, y: 300, width: 92, height: 80), xRadius: 10, yRadius: 10)
  white.withAlphaComponent(0.92).setFill()
  neck.fill()
  let foot = NSBezierPath(roundedRect: NSRect(x: 372, y: 270, width: 280, height: 42), xRadius: 21, yRadius: 21)
  white.setFill()
  foot.fill()

  if s.server {
    // A signal going out: a dot and three arcs, in the accent colour.
    let c = NSPoint(x: 512, y: 500)
    s.accent.setFill()
    NSBezierPath(ovalIn: NSRect(x: c.x - 26, y: c.y - 26, width: 52, height: 52)).fill()
    for (i, r) in [70.0, 118.0, 166.0].enumerated() {
      let arc = NSBezierPath()
      arc.appendArc(withCenter: c, radius: CGFloat(r), startAngle: 38, endAngle: 142)
      arc.lineWidth = 26
      arc.lineCapStyle = .round
      s.accent.withAlphaComponent(1 - CGFloat(i) * 0.22).setStroke()
      arc.stroke()
    }
  } else {
    // A window on the remote desktop, and the pointer that reaches it.
    let win = NSRect(x: 318, y: 470, width: 250, height: 170)
    let winPath = NSBezierPath(roundedRect: win, xRadius: 16, yRadius: 16)
    s.accent.withAlphaComponent(0.95).setFill()
    winPath.fill()
    NSColor.white.withAlphaComponent(0.85).setFill()
    NSBezierPath(roundedRect: NSRect(x: win.minX, y: win.maxY - 34, width: win.width, height: 34),
                 xRadius: 16, yRadius: 16).fill()
    let pointer = NSBezierPath()
    let p = NSPoint(x: 560, y: 610)  // tip
    pointer.move(to: p)
    pointer.line(to: NSPoint(x: p.x, y: p.y - 190))
    pointer.line(to: NSPoint(x: p.x + 46, y: p.y - 146))
    pointer.line(to: NSPoint(x: p.x + 82, y: p.y - 222))
    pointer.line(to: NSPoint(x: p.x + 116, y: p.y - 206))
    pointer.line(to: NSPoint(x: p.x + 82, y: p.y - 132))
    pointer.line(to: NSPoint(x: p.x + 142, y: p.y - 132))
    pointer.close()
    pointer.lineJoinStyle = .round
    pointer.lineWidth = 18
    NSColor.black.withAlphaComponent(0.85).setStroke()
    pointer.stroke()
    white.setFill()
    pointer.fill()
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
