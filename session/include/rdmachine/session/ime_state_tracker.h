#pragma once
// What the server tells the client about its input method (docs/ime.md):
// the state built from RDMachine Input's events, and which of them still
// count (6.4.0). Not thread-safe: the media host holds its lock around it.
//
// - macOS activates and deactivates RDMachine Input's controllers (one per
//   text field) in no fixed order; one of them going and coming back is the
//   same field, and keeps what was known about its caret.
// - A report says which client message it already reflects; the state's
//   seq is that of the report its caret came from.
// - "The system ended the composition" counts only for the composition
//   under way, not for one a controller still remembers from before.
#include <cstdint>
#include <optional>
#include <string>

#include "rdmachine/ime_wire.h"

namespace rdm {

class ImeStateTracker {
 public:
  struct Report {
    std::optional<bool> active;  // nullopt: not said (before 6.3.6)
    uint64_t ctl = 0;            // 0: not said
    std::string app;
    std::optional<uint64_t> answers;  // nullopt: not said (before 6.4.0)
    std::optional<ImeRect> caret;     // in the session's pixels; nullopt: not found
    std::optional<ImeRange> marked, selected;
  };

  // A session starts. field_active, app, ctl: the text field that has the
  // focus, as the input method said before the session.
  void Reset(bool field_active, const std::string& app, uint64_t ctl);
  // The input method process connected or went away.
  void OnConnection(bool connected);
  // A controller over there was activated or deactivated.
  void OnField(bool active, const std::string& app, uint64_t ctl);
  // False when the report does not count (from a field without the focus).
  // last_input: the client's last message, for an input method that does
  // not say what its report reflects.
  bool OnReport(const Report& r, uint64_t last_input);
  // A mark went over with this bridge id.
  void OnMark(uint64_t mark_id);
  // A commit or a cancel went over: no composition now.
  void OnCompositionDone();
  // The system ended a composition over there. True when it is the one under
  // way (the client drops its own); false for another one.
  bool OnEnded(std::optional<uint64_t> mark_id);

  ImeState& state() { return state_; }
  const ImeState& state() const { return state_; }

 private:
  void NewField(uint64_t ctl);

  ImeState state_;
  uint64_t ctl_ = 0;
  bool composing_ = false;
  uint64_t mark_id_ = 0;
};

}  // namespace rdm
