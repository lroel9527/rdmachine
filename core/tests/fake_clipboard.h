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
    if (concealed_ || promise_) return std::nullopt;  // never our own promise
    return content_;
  }
  std::optional<int64_t> Write(const ClipboardContent& content) override {
    std::lock_guard lock(mu_);
    ++writes_;
    content_ = content;
    concealed_ = false;
    promise_.reset();
    return ++count_;
  }
  std::optional<int64_t> WritePromise(std::shared_ptr<ClipboardPromise> promise) override {
    std::lock_guard lock(mu_);
    if (!can_promise_) return std::nullopt;
    ++writes_;
    content_ = {};
    concealed_ = false;
    promise_ = std::move(promise);
    return ++count_;
  }

  // What a local application (the user) does.
  void Copy(const std::string& text, bool concealed = false) {
    std::lock_guard lock(mu_);
    promise_.reset();
    content_ = ClipboardContent::Text(text);
    concealed_ = concealed;
    ++count_;
  }
  void CopyContent(const ClipboardContent& c) {
    std::lock_guard lock(mu_);
    promise_.reset();
    content_ = c;
    concealed_ = false;
    ++count_;
  }
  void CopyNothingShared() {
    std::lock_guard lock(mu_);
    promise_.reset();
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
  // The peer's copy as promised here (null if the clipboard holds something else).
  std::shared_ptr<ClipboardPromise> promise() {
    std::lock_guard lock(mu_);
    return promise_;
  }
  void set_can_promise(bool b) {
    std::lock_guard lock(mu_);
    can_promise_ = b;
  }
  int writes() {
    std::lock_guard lock(mu_);
    return writes_;
  }

 private:
  std::mutex mu_;
  ClipboardContent content_;
  bool concealed_ = false;
  std::shared_ptr<ClipboardPromise> promise_;
  bool can_promise_ = true;
  int64_t count_ = 0;
  int reads_ = 0;
  int writes_ = 0;
};

}  // namespace rdm
