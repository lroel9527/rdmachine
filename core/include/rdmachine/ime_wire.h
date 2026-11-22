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
#include <vector>

namespace rdm {

// rdaccel application message types (RDA_MSG_USER_FIRST + 0x14 / + 0x15).
// kImeMessageType goes client to server with rda_viewer_send_input_message,
// in order with the keys; kImeStateMessageType server to client on the
// message channel.
constexpr uint32_t kImeMessageType = 0x10014;
constexpr uint32_t kImeStateMessageType = 0x10015;

// The server's input method, as Text Input Sources knows it.
inline constexpr const char* kImeInputSourceId = "org.rdmachine.inputmethod.Remote";

// One key event as rdaccel carries it (rda_input_event): kind
// RDA_INPUT_KEY_DOWN or RDA_INPUT_KEY_UP, a USB HID usage, RDA_MOD_* bits.
struct ImeKeyEvent {
  uint32_t kind = 0;
  uint32_t code = 0;
  uint32_t modifiers = 0;

  bool operator==(const ImeKeyEvent&) const = default;
};

// Client to server. UTF-16 indices, as NSTextInputClient counts.
struct ImeInput {
  enum class Kind { kMark, kCommit, kCancel, kMode, kKey, kSource };
  Kind kind = Kind::kMark;
  uint64_t seq = 0;
  std::string text;          // kMark: the composition; kCommit: what goes in
  uint32_t sel_location = 0;  // kMark: the selection inside the composition
  uint32_t sel_length = 0;
  bool mode_on = false;  // kMode: the client starts (true) or stops typing with its input method
  // The latest key number of the server's back and typed (ImeState) the
  // client had heard of when it sent this (6.6.0; 0 = none).
  uint64_t ack = 0;

  // kKey (6.6.0, to a server that says keys): one key the client handled
  // while it types with its input method, and how it handled it. The server
  // judges it as it arrives, by whether a text field takes input methods
  // there then, as a Mac decides when its app handles the key (docs/ime.md).
  uint64_t key = 0;  // the client's number for it, rising through the session
  bool im = false;   // its input method took it; otherwise it goes in as `events`
  // A key-down that went by whether a text field over there takes text: what
  // the client took that to be. Absent for keys that go the same way either
  // way (shortcuts, key-ups, modifiers, ABC here).
  std::optional<bool> field{};
  // im: the key-down as a key (typed if no text field takes it there);
  // otherwise the keys that go in.
  std::vector<ImeKeyEvent> events{};
  // What the client's input method did while it handled the key (marks,
  // commits, cancels), in order: they go in before `events`.
  std::vector<ImeInput> then{};

  // kSource (7.1.0, to a server that says badge): the input source the
  // client types with now, for the server's menu bar icon. Not one of the
  // numbered messages: seq is the last one sent, unchanged.
  std::string source_id{};     // its Text Input Sources id (for the log)
  std::string icon{};          // ImeIconFingerprint of its icon; empty: it has none
  std::vector<uint8_t> png{};  // the icon itself, once the server asked for it (ImeState need_icon)

  bool operator==(const ImeInput&) const = default;
};

// {"seq":1,"mark":"ni'hao","sel":[6,0]}, {"seq":2,"commit":"你好"},
// {"seq":3,"cancel":true}, {"seq":4,"mode":"on"|"off"},
// {"seq":4,"key":{"id":7,"im":true,"field":true,"ev":[[5,17,0]],"then":[{"seq":5,"mark":"n"}]}},
// {"seq":5,"source":{"id":"com.apple.inputmethod.SCIM.ITABC","icon":"<32 hex>","png":"<base64>"}};
// any of them with "ack":N.
std::string EncodeImeInput(const ImeInput& m);
// nullopt for anything that is not exactly one of those (a key's "then"
// holds only marks, commits and cancels).
std::optional<ImeInput> DecodeImeInput(const std::string& payload);

// The fingerprint both sides know an input source icon by (7.1.0): 32
// lowercase hex digits, the first 16 bytes of SHA-256 of its PNG. The server
// keeps the icons it was sent by it, so each one crosses the network once.
std::string ImeIconFingerprint(const std::vector<uint8_t>& png);
bool IsImeIconFingerprint(const std::string& s);
// The largest icon image the server takes: base64 and the JSON around it
// still fit rdaccel's 64 KiB input message (an icon is about 1 KB).
inline constexpr size_t kImeIconMaxBytes = 32 * 1024;

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
  // 6.6.0: this server judges the client's keys as they arrive (ImeInput
  // kKey); the client numbers them.
  bool keys = false;
  // The client's key from which the keys it sent before it heard this were
  // not used: a text field took input methods here when they came, so its
  // input method gets them. It handles them again, in order (0 = none).
  uint64_t back = 0;
  // The client's key from which what its input method took was typed here
  // as keys, no text field taking input methods when they came: its
  // composition went nowhere (0 = none).
  uint64_t typed = 0;
  // The client's last key the server had judged when it sent this (0 =
  // none): later ones were still on their way. Not a change of its own (the
  // state is not sent for it alone).
  uint64_t judged = 0;
  // 7.1.0: this server shows the input source the client types with on its
  // menu bar icon (while the client types with its own input method this
  // Mac is on RDMachine Input, and in full screen the client's own menu bar
  // is hidden): the client says which one (ImeInput kSource).
  bool badge = false;
  // The icon (fingerprint) the server does not have: the client sends it
  // once, with its image. Empty: none wanted.
  std::string need_icon;

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
