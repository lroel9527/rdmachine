#pragma once
// Keyboard input source sync ("同步键盘语言", docs/protocol.md): the client
// tells the server which input source it uses (Pinyin, ABC, ...) and the
// server selects the matching one, so switching on the client (Caps Lock,
// ⌃Space, the menu) switches the server too. A switch cannot travel as a
// key: macOS only switches on Caps Lock from a physical keyboard.
//
// Platform independent: reading and selecting input sources is rdaccel's
// (rda_current_input_source, rda_injector_select_input_source); this is the
// message and the choice of a match.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rdm {

// rdaccel application message type (RDA_MSG_USER_FIRST + 1), client to server.
constexpr uint32_t kInputSourceMessageType = 0x10001;

struct InputSourceState {
  std::string id;        // "com.apple.inputmethod.SCIM.ITABC", "com.apple.keylayout.ABC"
  std::string name;      // localized, for display only
  std::string language;  // BCP 47, "zh-Hans"; may be empty
  bool input_method = false;
  bool ascii_capable = false;

  bool operator==(const InputSourceState&) const = default;
};

// JSON: {"id":..,"name":..,"language":..,"input_method":..,"ascii_capable":..}
std::string EncodeInputSource(const InputSourceState& s);
// nullopt for anything that is not such an object with a non-empty id.
std::optional<InputSourceState> DecodeInputSource(const std::string& payload);

// The server's input source for what the client uses, from the server's
// enabled sources: the same id if enabled there; else an input method for
// the same language (exact, then the same primary language: "zh-Hans" is
// not "zh-Hant"); else, for a plain layout, an ASCII-capable layout (the
// same language first). nullopt when nothing fits.
//
// layouts_only (the login window): never an input method. Its user list and
// password field take keys only from a keyboard layout; with Pinyin selected
// everything typed vanishes into the input method. A client on an input
// method gets an ASCII layout here (ABC, then U.S., then any).
std::optional<std::string> ChooseInputSource(const InputSourceState& wanted,
                                             const std::vector<InputSourceState>& available,
                                             bool layouts_only = false);

}  // namespace rdm
