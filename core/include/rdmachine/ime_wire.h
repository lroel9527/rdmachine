#pragma once
// Typing with the client's input method (docs/ime.md): the client composes
// with its own input method and sends the composition and the committed text;
// the server puts them into the focused app through an input method of its
// own (RDMachine Input) and tells the client where the caret is, so the
// client's candidate window sits next to it.
//
// Platform independent: the two messages, and the server-side link between
// the agent and the input method process.
#include <cstdint>
#include <optional>
#include <string>

namespace rdm {

// rdaccel application message types (RDA_MSG_USER_FIRST + 0x14 / + 0x15).
// kImeMessageType goes client to server with rda_viewer_send_input_message,
// in order with the keys; kImeStateMessageType server to client on the
// message channel.
constexpr uint32_t kImeMessageType = 0x10014;
constexpr uint32_t kImeStateMessageType = 0x10015;

// The server's input method, as Text Input Sources knows it.
inline constexpr const char* kImeInputSourceId = "org.rdmachine.inputmethod.Remote";

// Client to server. UTF-16 indices, as NSTextInputClient counts.
struct ImeInput {
  enum class Kind { kMark, kCommit, kCancel, kMode };
  Kind kind = Kind::kMark;
  uint64_t seq = 0;
  std::string text;          // kMark: the composition; kCommit: what goes in
  uint32_t sel_location = 0;  // kMark: the selection inside the composition
  uint32_t sel_length = 0;
  bool mode_on = false;  // kMode: the client starts (true) or stops typing with its input method

  bool operator==(const ImeInput&) const = default;
};

// {"seq":1,"mark":"ni'hao","sel":[6,0]}, {"seq":2,"commit":"你好"},
// {"seq":3,"cancel":true}, {"seq":4,"mode":"on"|"off"}.
std::string EncodeImeInput(const ImeInput& m);
// nullopt for anything that is not exactly one of those.
std::optional<ImeInput> DecodeImeInput(const std::string& payload);

struct ImeRange {
  uint32_t location = 0, length = 0;
  bool operator==(const ImeRange&) const = default;
};

// A rectangle in the pixels of one of the session's displays (0 = the main
// picture, 1.. = the extra displays in order), origin top left.
struct ImeRect {
  int32_t display = 0;
  int32_t x = 0, y = 0, width = 0, height = 0;
  bool operator==(const ImeRect&) const = default;
};

// Server to client, sent when something changed.
struct ImeState {
  bool available = false;  // the server can put the client's text in through its input method
  bool client = false;     // a text field that takes input methods has the focus there
  bool secure = false;     // secure input (a password field): the client sends plain keys
  std::string app;         // bundle id of that field's app
  // The last ImeInput the caret below already reflects: all of them before
  // the first one the server's input method had not put in when it found
  // that caret (6.4.0; before, merely the last message received).
  uint64_t seq = 0;
  // Changes when another text field takes the focus over there (not when the
  // same one is let go and taken again): what was known about the last
  // field's caret is no use for this one. 0 = not said (before 6.4.0).
  uint32_t field = 0;
  // Where the composition starts (the caret when there is none); absent when
  // not known.
  std::optional<ImeRect> caret;
  std::optional<ImeRange> marked, selected;
  // The composition was ended over there (a click elsewhere, another app):
  // the client drops its own.
  bool ended = false;

  bool operator==(const ImeState&) const = default;
};

std::string EncodeImeState(const ImeState& s);
// nullopt for anything that is not a JSON object; missing or mistyped
// fields keep their defaults.
std::optional<ImeState> DecodeImeState(const std::string& payload);

// Where the agent listens for the input method process of the same user:
// ~/Library/Application Support/RDMachine/ime.sock (home from the password
// database, not $HOME). Empty when there is no home.
std::string ImeSocketPath();

}  // namespace rdm
