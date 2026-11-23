// Training and test pictures for the client's 1x -> 2x upscaler (docs/upscaler.md).
// Each picture is drawn twice by AppKit and Core Text, at 1x and at 2x, the way
// macOS draws a window's backing store on a standard and on a Retina display:
// antialiasing, font smoothing, subpixel positioning, icons from their own
// representations, images resampled by Core Graphics. 8-bit sRGB, as the
// client's surfaces hold the picture.
//
//   make_data OUTDIR CORPUS.txt
//     OUTDIR/train/NAME_1x.png, NAME_2x.png   random text pages, icon pages, wallpapers
//     OUTDIR/test/NAME_1x.png, NAME_2x.png    a fixed page, an icon page, two held-out wallpapers
import AppKit
import CoreText
import ImageIO
import UniformTypeIdentifiers

struct RNG {
  var s: UInt64
  mutating func next() -> UInt64 { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s }
  mutating func uniform() -> Double { Double(next() % 1_000_000) / 1_000_000 }
  mutating func pick<T>(_ a: [T]) -> T { a[Int(next() % UInt64(a.count))] }
}

func makeContext(_ w: Int, _ h: Int) -> CGContext {
  let info = CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue
  let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0,
                      space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: info)!
  ctx.setAllowsAntialiasing(true); ctx.setShouldAntialias(true)
  ctx.setAllowsFontSmoothing(true); ctx.setShouldSmoothFonts(true)
  ctx.setAllowsFontSubpixelPositioning(true); ctx.setShouldSubpixelPositionFonts(true)
  ctx.setAllowsFontSubpixelQuantization(true); ctx.setShouldSubpixelQuantizeFonts(true)
  ctx.interpolationQuality = .high
  return ctx
}

func save(_ img: CGImage, _ path: String) {
  let dst = CGImageDestinationCreateWithURL(URL(fileURLWithPath: path) as CFURL, UTType.png.identifier as CFString, 1, nil)!
  CGImageDestinationAddImage(dst, img, nil)
  CGImageDestinationFinalize(dst)
}

// Draws `draw` (top-left point coordinates, a flipped AppKit context) at 1x and 2x.
func renderPair(_ w: Int, _ h: Int, _ dir: String, _ name: String, _ draw: () -> Void) {
  for scale in [1, 2] {
    let ctx = makeContext(w * scale, h * scale)
    ctx.translateBy(x: 0, y: CGFloat(h * scale))
    ctx.scaleBy(x: CGFloat(scale), y: -CGFloat(scale))
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(cgContext: ctx, flipped: true)
    draw()
    NSGraphicsContext.restoreGraphicsState()
    save(ctx.makeImage()!, "\(dir)/\(name)_\(scale)x.png")
  }
}

func rgb(_ hex: UInt32) -> NSColor {
  NSColor(srgbRed: CGFloat((hex >> 16) & 255) / 255, green: CGFloat((hex >> 8) & 255) / 255,
          blue: CGFloat(hex & 255) / 255, alpha: 1)
}
func sys(_ size: CGFloat, _ w: NSFont.Weight = .regular) -> NSFont { NSFont.systemFont(ofSize: size, weight: w) }
func mono(_ size: CGFloat, _ w: NSFont.Weight = .regular) -> NSFont { NSFont.monospacedSystemFont(ofSize: size, weight: w) }
func fill(_ r: NSRect, _ c: NSColor) { c.setFill(); r.fill() }

// One line of text with its baseline at `base` (points from the top), as Core
// Text lays it out in a text field.
func text(_ s: String, _ f: NSFont, _ c: NSColor, _ x: CGFloat, _ base: CGFloat) {
  let ctx = NSGraphicsContext.current!.cgContext
  let line = CTLineCreateWithAttributedString(NSAttributedString(string: s, attributes: [.font: f, .foregroundColor: c]))
  ctx.saveGState()
  ctx.textMatrix = CGAffineTransform(scaleX: 1, y: -1)  // the context is flipped
  ctx.textPosition = CGPoint(x: x, y: base)
  CTLineDraw(line, ctx)
  ctx.restoreGState()
}

let pairs: [(UInt32, UInt32)] = [(0x1d1d1f, 0xffffff), (0x6e6e73, 0xffffff), (0x000000, 0xf2f2f7), (0xe5e5ea, 0x1e1e1e),
                                 (0x98989d, 0x1e1e1e), (0xffffff, 0x0a84ff), (0x0a84ff, 0xffffff), (0xff3b30, 0xffffff),
                                 (0xdfdfe0, 0x1f1f24), (0xfc5fa3, 0x1f1f24), (0x30d158, 0x000000), (0x1d1d1f, 0xfff8dc),
                                 (0xffffff, 0x2c2c2e), (0x333333, 0xe8e8e8), (0x67b7a4, 0x1f1f24), (0x000000, 0xffffff)]
let weights: [NSFont.Weight] = [.regular, .regular, .regular, .medium, .semibold, .bold, .light]

// Random lines of the corpus in random sizes (10-20 pt), weights and colours,
// at fractional x positions; baselines on whole points, as AppKit lays out.
func textPage(_ lines: [String], _ rng: inout RNG) -> () -> Void {
  var bands: [(NSRect, NSColor, String, NSFont, NSColor, CGFloat, CGFloat)] = []
  var y: CGFloat = 0
  while y < 450 {
    let (fg, bg) = rng.pick(pairs)
    let size = CGFloat(10 + Int(rng.next() % 21)) * 0.5 + 5
    let f = rng.next() % 4 == 0 ? mono(size, rng.pick(weights)) : sys(size, rng.pick(weights))
    let band = ceil(size * 1.9)
    bands.append((NSRect(x: 0, y: y, width: 640, height: band), rgb(bg), rng.pick(lines), f, rgb(fg),
                  4 + CGFloat(rng.uniform() * 12), (y + size * 1.35).rounded()))
    y += band
  }
  return {
    for b in bands {
      fill(b.0, b.1)
      text(b.2, b.3, b.4, b.5, b.6)
    }
  }
}

// App icons as Finder, the Dock and toolbars show them, in the usual sizes on
// light and dark backgrounds; each drawn from the representation macOS picks
// for that scale.
func iconPage(_ icons: [NSImage], _ rng: inout RNG) -> () -> Void {
  var items: [(NSImage, NSRect)] = []
  let sizes: [CGFloat] = [16, 16, 18, 20, 24, 32, 32, 48, 64]
  var y: CGFloat = 6
  while y < 440 {
    let size = rng.pick(sizes)
    var x: CGFloat = 6 + CGFloat(Int(rng.next() % 8))
    while x < 620 - size {
      items.append((rng.pick(icons), NSRect(x: x, y: y, width: size, height: size)))
      x += size + CGFloat(4 + Int(rng.next() % 12))
    }
    y += size + 6
  }
  let bg = rng.pick([0xffffff, 0xf2f2f7, 0x1e1e1e, 0x2c2c2e, 0xe8e8e8, 0x3a3a3c] as [UInt32])
  return {
    fill(NSRect(x: 0, y: 0, width: 640, height: 480), rgb(bg))
    for (icon, r) in items { icon.draw(in: r, from: .zero, operation: .sourceOver, fraction: 1, respectFlipped: true, hints: nil) }
  }
}

// A wallpaper drawn into the page, as a window shows a picture: resampled by
// Core Graphics at each scale.
func photoPage(_ path: String) -> (() -> Void)? {
  guard let src = CGImageSourceCreateWithURL(URL(fileURLWithPath: path) as CFURL, nil),
        let img = CGImageSourceCreateImageAtIndex(src, 0, nil) else { return nil }
  return {
    let ctx = NSGraphicsContext.current!.cgContext
    ctx.saveGState()
    ctx.translateBy(x: 0, y: 400)
    ctx.scaleBy(x: 1, y: -1)
    // The middle of the picture, 3:2.
    let side = CGFloat(min(img.width, img.height))
    let crop = img.cropping(to: CGRect(x: (CGFloat(img.width) - side) / 2, y: (CGFloat(img.height) - side * 2 / 3) / 2,
                                       width: side, height: side * 2 / 3))!
    ctx.draw(crop, in: CGRect(x: 0, y: 0, width: 600, height: 400))
    ctx.restoreGState()
  }
}

// The fixed test page: what a Mac desktop shows most, in the sizes apps use.
func testPage() {
  let black = rgb(0x1d1d1f), gray = rgb(0x6e6e73)
  fill(NSRect(x: 0, y: 0, width: 1000, height: 720), .white)
  text("省流量模式的文字清晰度", sys(20, .semibold), black, 24, 40)
  text("远程桌面用一倍分辨率渲染时，客户端把每个像素放大成两行两列。", sys(13), black, 24, 68)
  text("窗口、菜单、按钮和输入框里的文字都应该清楚，标点“，。；：”也一样。", sys(13), black, 24, 88)
  text("文件已拷贝到剪贴板，在访达里按 ⌘V 粘贴；断线后会接着传。", sys(13), black, 24, 108)
  text("The quick brown fox jumps over the lazy dog. 0123456789 (Retina)", sys(13), black, 24, 132)
  text("Data saver keeps text sharp while wallpapers and icons get cheaper.", sys(13), black, 24, 152)
  text("次要说明文字 12 点：系统设置 → 隐私与安全性 → 屏幕录制", sys(12), gray, 24, 176)
  text("更小的文字 11 点：上次连接 2026年11月22日 09:41，延迟 3.2 毫秒", sys(11), gray, 24, 196)
  text("笔画多的字：藏 龍 蠻 鬱 齉 灩 爨 罐 籍 黴 镶 噩 壤 蘸 馨 戴 赢", sys(13), black, 24, 222)
  text("Menu: RDMachine  文件  编辑  显示  窗口  帮助", sys(13, .medium), black, 24, 248)
  text("粗体标题 15 点 Semibold Heading", sys(15, .semibold), black, 24, 274)
  fill(NSRect(x: 560, y: 16, width: 420, height: 270), rgb(0xf2f2f7))
  for (i, s) in ["访达", "文稿", "下载", "桌面", "应用程序", "Downloads", "AirDrop", "iCloud 云盘"].enumerated() {
    text(s, sys(13), black, 584, CGFloat(46 + i * 28))
  }
  text("10 点数字：12.4 ms  0.0%  1.23 MB  60 fps", sys(10), gray, 700, 46)
  fill(NSRect(x: 24, y: 296, width: 120, height: 28), rgb(0x0a84ff))
  text("连接", sys(13, .medium), .white, 70, 315)
  text("忘记密码", sys(13), rgb(0x0a84ff), 164, 315)
  text("连不上：UDP 4100 被拦住了", sys(13), rgb(0xff3b30), 248, 315)
  text("Link to docs/protocol.md", sys(13), rgb(0x0a84ff), 440, 315)
  for (i, s) in ["const double ratio = _streamW > 0 ? w * backing / _streamW : 1;",
                 "if (![_layer.magnificationFilter isEqualToString:filter]) {",
                 "  // 每个像素原样放大成 2 × 2，不做平滑"].enumerated() {
    text(s, mono(12), black, 24, CGFloat(352 + i * 18))
  }
  fill(NSRect(x: 0, y: 420, width: 1000, height: 300), rgb(0x1e1e1e))
  let light = rgb(0xe5e5ea), dim = rgb(0x98989d)
  text("深色模式：浅色文字在深色背景上，笔画容易显得发虚。", sys(13), light, 24, 452)
  text("Dark mode text: settings, sidebar labels and the terminal.", sys(13), light, 24, 474)
  text("12 点次要文字：最多再等 42 秒 · 正在重连", sys(12), dim, 24, 496)
  let kw = rgb(0xfc5fa3), str = rgb(0xfc6a5d), fn = rgb(0x67b7a4), plain = rgb(0xdfdfe0), cm = rgb(0x6c7986)
  text("func", mono(12), kw, 24, 530); text("viewDesktop() -> DesktopSpec {", mono(12), plain, 64, 530)
  text("let", mono(12), kw, 44, 548); text("scale = DesktopScale(window.backingScaleFactor)", mono(12), fn, 72, 548)
  text("print(\"picture \\(w)x\\(h) at \\(scale)x\")", mono(12), str, 44, 566)
  text("// 一倍桌面：像素只有四分之一", mono(12), cm, 44, 584)
  text("}", mono(12), plain, 24, 602)
  text("user@mac ~ % ./dist/rdmachine-connect mac-mini --text-mode", mono(12), rgb(0x30d158), 24, 636)
  text("终端里的中文：已连接，延迟 3 ms，流量 52 kbps", mono(12), plain, 24, 656)
  fill(NSRect(x: 600, y: 440, width: 380, height: 260), rgb(0x2c2c2e))
  text("设置", sys(13, .semibold), light, 624, 470)
  for (i, s) in ["通用", "外观", "显示器", "键盘", "隐私与安全性", "Accessibility"].enumerated() {
    text(s, sys(13), i == 2 ? .white : light, 624, CGFloat(500 + i * 28))
  }
}

let a = CommandLine.arguments
guard a.count == 3 else { print("usage: make_data OUTDIR CORPUS.txt"); exit(2) }
let out = a[1]
for d in ["train", "test"] { try! FileManager.default.createDirectory(atPath: "\(out)/\(d)", withIntermediateDirectories: true) }
let lines = try! String(contentsOfFile: a[2], encoding: .utf8).split(separator: "\n").map(String.init).filter { $0.count >= 8 }

let appDirs = ["/System/Applications", "/System/Applications/Utilities"]
var apps: [String] = []
for d in appDirs {
  for f in (try? FileManager.default.contentsOfDirectory(atPath: d)) ?? [] where f.hasSuffix(".app") { apps.append("\(d)/\(f)") }
}
apps.sort()
// Even-numbered apps for training, odd for the test page.
let icons = apps.map { NSWorkspace.shared.icon(forFile: $0) }
let trainIcons = icons.enumerated().filter { $0.offset % 2 == 0 }.map(\.element)
let testIcons = icons.enumerated().filter { $0.offset % 2 == 1 }.map(\.element)

var rng = RNG(s: 12345)
for i in 0..<32 { renderPair(640, 480, "\(out)/train", String(format: "text%02d", i), textPage(lines, &rng)) }
for i in 0..<8 { renderPair(640, 480, "\(out)/train", String(format: "icons%02d", i), iconPage(trainIcons, &rng)) }
let pictures = "/System/Library/Desktop Pictures"
for name in ["Mac Purple", "Radial Sky Blue", "iMac Blue", "iMac Orange", "Mac Yellow", "iMac Pink"] {
  if let draw = photoPage("\(pictures)/\(name).heic") { renderPair(600, 400, "\(out)/train", "photo-" + name.replacingOccurrences(of: " ", with: "_"), draw) }
}
renderPair(1000, 720, "\(out)/test", "page", testPage)
var trng = RNG(s: 777)
renderPair(640, 480, "\(out)/test", "icons", iconPage(testIcons, &trng))
for name in ["Sonoma", "iMac Green"] {
  if let draw = photoPage("\(pictures)/\(name).heic") { renderPair(600, 400, "\(out)/test", "photo-" + name.replacingOccurrences(of: " ", with: "_"), draw) }
}
print("wrote \(out)/train and \(out)/test (\(icons.count) app icons)")
