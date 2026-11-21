#include "rdmachine/session/ime_key_judge.h"

namespace rdm {

ImeKeyJudge::Verdict ImeKeyJudge::OnKey(const Key& k, bool field_now) {
  using Keys = Verdict::Keys;
  Verdict v;
  if (k.id > judged_) judged_ = k.id;
  if (k.ack < back_) return v;  // sent before it heard of the back: it sends it again
  // Its composition went nowhere, and it did not know yet when it sent this.
  const bool gone = k.ack < typed_;
  if (k.im) {
    if (!field_now) {
      // Nothing here takes input methods: a key, as on that Mac.
      if (!gone) {
        typed_ = k.id;
        v.told = true;
      }
      v.keys = Keys::kTyped;
      return v;
    }
    if (gone) {
      // A text field takes it again, but the client's composition holds keys
      // that were typed here as keys: its input method composes afresh.
      back_ = k.id;
      v.told = true;
      return v;
    }
    v.messages = true;
    return v;
  }
  if (k.field && !*k.field && field_now) {
    // A text field takes input methods here: the client's input method gets it.
    back_ = k.id;
    v.told = true;
    return v;
  }
  v.messages = !gone;
  v.keys = Keys::kAsSent;
  return v;
}

}  // namespace rdm
