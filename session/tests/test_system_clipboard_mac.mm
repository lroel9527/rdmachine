// The real macOS clipboard (NSPasteboard) behind SystemClipboard(). Runs on
// any Mac with a logged-in session; keeps and restores whatever text was
// on the clipboard so running it does not lose what you copied.
#import <AppKit/AppKit.h>
#include <gtest/gtest.h>

#include "rdmachine/session/system_clipboard.h"

using namespace rdm;

namespace {

class MacClipboardTest : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_ = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
    clipboard_ = SystemClipboard();
    ASSERT_TRUE(clipboard_);
  }
  void TearDown() override {
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    if (saved_) [pb setString:saved_ forType:NSPasteboardTypeString];
  }

  NSString* saved_ = nil;
  std::shared_ptr<Clipboard> clipboard_;
};

}  // namespace

TEST_F(MacClipboardTest, WriteThenRead) {
  const std::string text = "rdmachine 剪贴板测试 ✓ 😀";
  const std::optional<int64_t> count = clipboard_->WriteText(text);
  ASSERT_TRUE(count);
  EXPECT_EQ(clipboard_->ChangeCount(), *count);  // the count is the write's own
  EXPECT_EQ(clipboard_->ReadText(), text);
  NSString* direct = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
  EXPECT_EQ(std::string(direct.UTF8String), text);
}

TEST_F(MacClipboardTest, ChangeCountMovesWhenOthersCopy) {
  ASSERT_TRUE(clipboard_->WriteText("ours"));
  const int64_t before = clipboard_->ChangeCount();
  NSPasteboard* pb = [NSPasteboard generalPasteboard];
  [pb clearContents];
  [pb setString:@"someone else's" forType:NSPasteboardTypeString];
  EXPECT_NE(clipboard_->ChangeCount(), before);
  EXPECT_EQ(clipboard_->ReadText(), "someone else's");
}

TEST_F(MacClipboardTest, ConcealedAndTransientAreNotRead) {
  for (NSString* marker : @[ @"org.nspasteboard.ConcealedType", @"org.nspasteboard.TransientType" ]) {
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:@"hunter2" forType:NSPasteboardTypeString];
    [pb setData:[NSData data] forType:marker];
    EXPECT_EQ(clipboard_->ReadText(), std::nullopt) << marker.UTF8String;
  }
}

TEST_F(MacClipboardTest, NonTextIsNotRead) {
  NSPasteboard* pb = [NSPasteboard generalPasteboard];
  [pb clearContents];
  [pb setData:[NSData dataWithBytes:"\x89PNG" length:4] forType:NSPasteboardTypePNG];
  EXPECT_EQ(clipboard_->ReadText(), std::nullopt);
}
