#include "rdmachine/session/ime_state_tracker.h"

namespace rdm {

void ImeStateTracker::Reset(bool field_active, const std::string& app, uint64_t ctl) {
  state_ = ImeState{};
  state_.field = 1;
  ctl_ = ctl;
  composing_ = false;
  mark_id_ = 0;
  if (field_active) {
    state_.client = true;
    state_.app = app;
  }
}

void ImeStateTracker::NewField(uint64_t ctl) {
  ctl_ = ctl;
  if (++state_.field == 0) state_.field = 1;
  state_.caret.reset();
  state_.marked.reset();
  state_.selected.reset();
}

void ImeStateTracker::OnConnection(bool connected) {
  if (connected) return;  // once back, it says which field has the focus
  state_.client = false;
  state_.app.clear();
  NewField(0);  // its controllers are gone with it
}

void ImeStateTracker::OnField(bool active, const std::string& app, uint64_t ctl) {
  if (active) {
    // An input method before 6.4.0 does not say which controller: every
    // activation is then taken as another field, as before.
    if (ctl == 0 || ctl != ctl_) NewField(ctl);
    state_.client = true;
    state_.app = app;
  } else {
    // The caret stays: the same field is often activated again within a
    // millisecond, and a field that has just been activated may not answer
    // where its caret is before its first text (Chromium).
    state_.client = false;
    state_.app.clear();
  }
}

bool ImeStateTracker::OnReport(const Report& r, uint64_t last_input) {
  if (r.active && !*r.active) return false;  // a late report of a field that lost the focus
  if (r.active && *r.active) {
    // A field reporting with the focus whose activation was not seen (it
    // came before this session, or got lost): it is the field now.
    if (r.ctl != 0 && r.ctl != ctl_) NewField(r.ctl);
    state_.client = true;
    state_.app = r.app;
  }
  state_.marked = r.marked;
  state_.selected = r.selected;
  if (r.caret) {
    // Not found: the client keeps the one it has, and seq stays that of the
    // report the caret came from.
    state_.caret = r.caret;
    state_.seq = r.answers.value_or(last_input);
  }
  return true;
}

void ImeStateTracker::OnMark(uint64_t mark_id) {
  composing_ = true;
  mark_id_ = mark_id;
}

void ImeStateTracker::OnCompositionDone() {
  composing_ = false;
  state_.marked.reset();
}

bool ImeStateTracker::OnEnded(std::optional<uint64_t> mark_id) {
  // An input method before 6.4.0 does not say which: taken as the one under way.
  if (mark_id && (!composing_ || *mark_id != mark_id_)) return false;
  composing_ = false;
  state_.marked.reset();
  return true;
}

}  // namespace rdm
