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
  std::optional<std::string> ReadText() override {
    std::lock_guard lock(mu_);
    ++reads_;
    if (concealed_) return std::nullopt;
    return text_;
  }
  std::optional<int64_t> WriteText(const std::string& text) override {
    std::lock_guard lock(mu_);
    ++writes_;
    text_ = text;
    concealed_ = false;
    return ++count_;
  }

  // What a local application (the user) does.
  void Copy(const std::string& text, bool concealed = false) {
    std::lock_guard lock(mu_);
    text_ = text;
    concealed_ = concealed;
    ++count_;
  }
  void CopyNonText() {
    std::lock_guard lock(mu_);
    text_.reset();
    concealed_ = false;
    ++count_;
  }
  std::optional<std::string> text() {
    std::lock_guard lock(mu_);
    return text_;
  }
  int writes() {
    std::lock_guard lock(mu_);
    return writes_;
  }

 private:
  std::mutex mu_;
  std::optional<std::string> text_;
  bool concealed_ = false;
  int64_t count_ = 0;
  int reads_ = 0;
  int writes_ = 0;
};

}  // namespace rdm
