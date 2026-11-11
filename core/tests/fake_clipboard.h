#pragma once
// An in-memory system clipboard for ClipboardSync tests.
#include <mutex>

#include "rdmachine/clipboard_sync.h"

namespace rdm {

class FakeClipboard : public Clipboard {
 public:
  int64_t ChangeCount() override {
    std::lock_guard lock(mu_);
    return count_;
  }
  std::optional<ClipboardContent> Read() override {
    std::lock_guard lock(mu_);
    ++reads_;
    if (concealed_) return std::nullopt;
    return content_;
  }
  std::optional<int64_t> Write(const ClipboardContent& content) override {
    std::lock_guard lock(mu_);
    ++writes_;
    content_ = content;
    concealed_ = false;
    return ++count_;
  }

  // What a local application (the user) does.
  void Copy(const std::string& text, bool concealed = false) {
    std::lock_guard lock(mu_);
    content_ = ClipboardContent::Text(text);
    concealed_ = concealed;
    ++count_;
  }
  void CopyContent(const ClipboardContent& c) {
    std::lock_guard lock(mu_);
    content_ = c;
    concealed_ = false;
    ++count_;
  }
  void CopyNothingShared() {
    std::lock_guard lock(mu_);
    content_ = ClipboardContent{};
    concealed_ = false;
    ++count_;
  }
  std::optional<std::string> text() {
    std::lock_guard lock(mu_);
    if (const std::string* t = content_.Find(kMimeText)) return *t;
    return std::nullopt;
  }
  ClipboardContent content() {
    std::lock_guard lock(mu_);
    return content_;
  }
  int writes() {
    std::lock_guard lock(mu_);
    return writes_;
  }

 private:
  std::mutex mu_;
  ClipboardContent content_;
  bool concealed_ = false;
  int64_t count_ = 0;
  int reads_ = 0;
  int writes_ = 0;
};

}  // namespace rdm
