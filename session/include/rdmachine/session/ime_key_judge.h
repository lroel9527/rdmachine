#pragma once
// The server's judgement of the client's keys (docs/ime.md, 6.6.0).
//
// A Mac decides whether a key goes to the input method when its app handles
// the key: if a text field that takes input methods has the focus then, the
// input method gets it; otherwise the app gets it as a key. The client
// decides as it sends, by what the server last told it, which is a network
// trip old. So the server judges each key again as it arrives, by what holds
// here then, and corrects the client where it was wrong:
//
// - The client sent a key-down as a key, taking it that no text field takes
//   input methods here, and one does: the server cannot compose, so the key
//   goes back (ImeState back). The client's input method gets it, and the
//   client handles again, in order, every key it sent before it heard so.
// - The client's input method took a key-down and no text field takes input
//   methods here: the key goes in as a key now, and what the input method
//   said for it is dropped. The client's composition went nowhere (ImeState
//   typed). The keys after it are judged the same way as they come; once a
//   text field takes input methods again, the next one the client's input
//   method took goes back (its composition was built on keys typed here).
//
// What the client sent before it heard of a back is not used at all (it
// sends it again). Of what it sent before it heard of a typed, the keys are
// judged as above, and what its input method said is dropped.
//
// Not thread-safe: the media host holds its lock around it.
#include <cstdint>
#include <optional>

namespace rdm {

class ImeKeyJudge {
 public:
  // One key as the client sent it (ImeInput kKey).
  struct Key {
    uint64_t id = 0;
    uint64_t ack = 0;
    bool im = false;            // its input method took it (always a key-down)
    std::optional<bool> field;  // a key-down that went by the field: whether the client took it that one takes text
  };
  struct Verdict {
    enum class Keys {
      kNone,    // nothing goes in
      kAsSent,  // the keys it sent
      kTyped,   // the key-down its input method took, typed as a key (pressed and let go)
    };
    bool messages = false;  // what its input method said goes in (before the keys)
    Keys keys = Keys::kNone;
    bool told = false;  // back or typed changed: tell the client
  };

  // A session starts: the client numbers its keys from 1 again.
  void Reset() { back_ = typed_ = judged_ = 0; }
  // field_now: a text field that takes input methods has the focus here,
  // and no password field (what the client would be told now).
  Verdict OnKey(const Key& k, bool field_now);
  // A mark, commit or cancel the client sent on its own (not with a key).
  bool TakesMessage(uint64_t ack) const { return ack >= back_ && ack >= typed_; }

  uint64_t back() const { return back_; }
  uint64_t typed() const { return typed_; }
  // The last key judged (ImeState judged): the client holds its next keys
  // while one it sent before it heard of a text field may still come back.
  uint64_t judged() const { return judged_; }

 private:
  uint64_t back_ = 0, typed_ = 0, judged_ = 0;
};

}  // namespace rdm
