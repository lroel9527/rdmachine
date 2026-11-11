// The real macOS clipboard code (NSPasteboard) behind SystemClipboard(), on
// a private pasteboard: the general one would reach a running session's
// peer (an installed server shares it) and lose what the user copied.
#import <AppKit/AppKit.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "fake_clipboard.h"
#include "rdmachine/session/system_clipboard.h"

using namespace rdm;
namespace fs = std::filesystem;

namespace {

class MacClipboardTest : public ::testing::Test {
 protected:
  void SetUp() override {
    pb_ = [NSPasteboard pasteboardWithUniqueName];
    clipboard_ = SystemClipboard(pb_.name.UTF8String);
    ASSERT_TRUE(clipboard_);
    std::string tmpl = (fs::temp_directory_path() / "rdm-pb-XXXXXX").string();
    dir_ = mkdtemp(tmpl.data());
  }
  void TearDown() override {
    [pb_ releaseGlobally];
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  NSPasteboard* pb_ = nil;
  std::shared_ptr<Clipboard> clipboard_;
  fs::path dir_;
};

std::string Text(const ClipboardContent& c) {
  const std::string* t = c.Find(kMimeText);
  return t ? *t : std::string();
}

}  // namespace

TEST_F(MacClipboardTest, TextWriteThenRead) {
  const std::string text = "rdmachine 剪贴板测试 ✓ 😀";
  const std::optional<int64_t> count = clipboard_->Write(ClipboardContent::Text(text));
  ASSERT_TRUE(count);
  EXPECT_EQ(clipboard_->ChangeCount(), *count);  // the count is the write's own
  auto c = clipboard_->Read();
  ASSERT_TRUE(c);
  EXPECT_EQ(*c, ClipboardContent::Text(text));
  EXPECT_EQ(std::string([pb_ stringForType:NSPasteboardTypeString].UTF8String), text);
}

TEST_F(MacClipboardTest, ChangeCountMovesWhenOthersCopy) {
  ASSERT_TRUE(clipboard_->Write(ClipboardContent::Text("ours")));
  const int64_t before = clipboard_->ChangeCount();
  [pb_ clearContents];
  [pb_ setString:@"someone else's" forType:NSPasteboardTypeString];
  EXPECT_NE(clipboard_->ChangeCount(), before);
  EXPECT_EQ(Text(*clipboard_->Read()), "someone else's");
}

TEST_F(MacClipboardTest, ConcealedAndTransientAreNotRead) {
  for (NSString* marker : @[ @"org.nspasteboard.ConcealedType", @"org.nspasteboard.TransientType" ]) {
    [pb_ clearContents];
    [pb_ setString:@"hunter2" forType:NSPasteboardTypeString];
    [pb_ setData:[NSData data] forType:marker];
    EXPECT_EQ(clipboard_->Read(), std::nullopt) << marker.UTF8String;
  }
}

TEST_F(MacClipboardTest, RichTextAndImagesBothWays) {
  NSImage* img = [[NSImage alloc] initWithSize:NSMakeSize(4, 3)];
  [img lockFocus];
  [NSColor.redColor setFill];
  NSRectFill(NSMakeRect(0, 0, 4, 3));
  [img unlockFocus];
  NSData* tiff = img.TIFFRepresentation;
  [pb_ clearContents];
  [pb_ setString:@"bold" forType:NSPasteboardTypeString];
  [pb_ setData:[@"{\\rtf1 \\b bold}" dataUsingEncoding:NSUTF8StringEncoding] forType:NSPasteboardTypeRTF];
  [pb_ setString:@"<b>bold</b>" forType:NSPasteboardTypeHTML];
  [pb_ setData:tiff forType:NSPasteboardTypeTIFF];
  auto c = clipboard_->Read();
  ASSERT_TRUE(c);
  EXPECT_EQ(Text(*c), "bold");
  ASSERT_TRUE(c->Find(kMimeRtf));
  EXPECT_EQ(*c->Find(kMimeRtf), "{\\rtf1 \\b bold}");
  ASSERT_TRUE(c->Find(kMimeHtml));
  EXPECT_EQ(*c->Find(kMimeHtml), "<b>bold</b>");
  // TIFF comes out as PNG.
  ASSERT_TRUE(c->Find(kMimePng));
  EXPECT_EQ(c->Find(kMimePng)->substr(0, 4), "\x89PNG");

  ASSERT_TRUE(clipboard_->Write(*c));
  EXPECT_EQ(clipboard_->Read(), c);
  NSImage* back = [[NSImage alloc] initWithPasteboard:pb_];
  ASSERT_TRUE(back);
  EXPECT_EQ(back.size.width, 4);
}

TEST_F(MacClipboardTest, CopiedFilesAreReadAsTheirNamesOnly) {
  std::ofstream(dir_ / "a.txt") << "a";
  fs::create_directories(dir_ / "folder");
  [pb_ clearContents];
  // As Finder copies: one item per file, each with its file URL, its name
  // and an icon.
  NSMutableArray* items = [NSMutableArray array];
  for (NSString* name : @[ @"a.txt", @"folder" ]) {
    NSPasteboardItem* item = [[NSPasteboardItem alloc] init];
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:(dir_ / name.UTF8String).c_str()]];
    [item setString:url.absoluteString forType:NSPasteboardTypeFileURL];
    [item setString:name forType:NSPasteboardTypeString];
    [item setData:[NSData dataWithBytes:"II*\0" length:4] forType:NSPasteboardTypeTIFF];
    [items addObject:item];
  }
  ASSERT_TRUE([pb_ writeObjects:items]);
  auto c = clipboard_->Read();
  ASSERT_TRUE(c);
  EXPECT_EQ(*c, ClipboardContent::Text("a.txt\nfolder"));  // every item's name; no icon
}

// ---- the peer's copy, promised on the pasteboard and fetched when read ----

namespace {

// The copying side is an in-memory clipboard; the pasting side this Mac's
// (private) pasteboard. Both sync on their own threads, wired directly.
class MacPromiseTest : public MacClipboardTest {
 protected:
  void SetUp() override {
    MacClipboardTest::SetUp();
    ClipboardSync::Options o;
    o.poll_ms = 10;
    ClipboardSync::Options ob = o;
    copier_ = std::make_unique<ClipboardSync>(fake_, [this](uint32_t t, std::string_view p) {
      paster_->OnMessage(t, reinterpret_cast<const uint8_t*>(p.data()), p.size());
      return true;
    }, o);
    paster_ = std::make_unique<ClipboardSync>(clipboard_, [this](uint32_t t, std::string_view p) {
      copier_->OnMessage(t, reinterpret_cast<const uint8_t*>(p.data()), p.size());
      return true;
    }, ob);
    copier_->Start();
    paster_->Start();
  }
  void TearDown() override {
    paster_->Stop();
    copier_->Stop();
    MacClipboardTest::TearDown();
  }
  // Until the pasteboard changes (the promise arrives).
  bool WaitForChange(NSInteger before) {
    for (int i = 0; i < 300; ++i) {
      if (pb_.changeCount != before) return true;
      usleep(10000);
    }
    return false;
  }

  std::shared_ptr<FakeClipboard> fake_ = std::make_shared<FakeClipboard>();
  std::unique_ptr<ClipboardSync> copier_, paster_;
};

}  // namespace

TEST_F(MacPromiseTest, TextIsFetchedOnlyWhenPasted) {
  const NSInteger before = pb_.changeCount;
  fake_->Copy("pasted on demand ✓");
  ASSERT_TRUE(WaitForChange(before));
  const NSInteger promised = pb_.changeCount;
  EXPECT_TRUE([pb_.types containsObject:NSPasteboardTypeString]);
  EXPECT_EQ(clipboard_->Read(), std::nullopt);  // never reads its own promise
  // An application pastes.
  NSString* s = [pb_ stringForType:NSPasteboardTypeString];
  ASSERT_TRUE(s);
  EXPECT_EQ(std::string(s.UTF8String), "pasted on demand ✓");
  // Filling it in is not a new copy: nothing is announced back.
  EXPECT_EQ(pb_.changeCount, promised);
  usleep(300000);
  EXPECT_FALSE(fake_->promise());
}

TEST_F(MacPromiseTest, ImagesPasteAsPngAndTiff) {
  NSImage* img = [[NSImage alloc] initWithSize:NSMakeSize(5, 2)];
  [img lockFocus];
  [NSColor.blueColor setFill];
  NSRectFill(NSMakeRect(0, 0, 5, 2));
  [img unlockFocus];
  NSData* png = [[NSBitmapImageRep imageRepWithData:img.TIFFRepresentation] representationUsingType:NSBitmapImageFileTypePNG
                                                                                      properties:@{}];
  ClipboardContent c;
  c.formats = {{std::string(kMimePng), std::string(static_cast<const char*>(png.bytes), png.length)}};
  const NSInteger before = pb_.changeCount;
  fake_->CopyContent(c);
  ASSERT_TRUE(WaitForChange(before));
  NSData* tiff = [pb_ dataForType:NSPasteboardTypeTIFF];
  ASSERT_TRUE(tiff);
  NSImage* back = [[NSImage alloc] initWithData:tiff];
  EXPECT_EQ(back.size.width, 5);
  EXPECT_EQ([pb_ dataForType:NSPasteboardTypePNG].length, png.length);
}

TEST_F(MacPromiseTest, APromiseFromAStoppedSessionGivesNothing) {
  const NSInteger before = pb_.changeCount;
  fake_->Copy("too late");
  ASSERT_TRUE(WaitForChange(before));
  paster_->Stop();
  EXPECT_EQ([pb_ stringForType:NSPasteboardTypeString], nil);
}
