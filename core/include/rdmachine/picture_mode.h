#pragma once
// How the picture is sent ("文字模式", docs/protocol.md). Text mode is for
// links far too slow for the whole picture (tens of kbps to a few hundred):
// the server spends what there is on text that is sharp, and photos and
// video update a few times a second at most. The client asks, as it is the
// one that knows its link; the server tells rdaccel (rda_host_set_text_mode),
// which does the work.
//
// Platform independent: the message only.
#include <cstdint>
#include <optional>
#include <string>

namespace rdm {

// rdaccel application message type (RDA_MSG_USER_FIRST + 4), client to
// server: change the picture mode of the running session.
constexpr uint32_t kPictureModeMessageType = 0x10004;

struct PictureMode {
  bool text_mode = false;

  bool operator==(const PictureMode&) const = default;
};

// JSON: {"text_mode":true}
std::string EncodePictureMode(const PictureMode& m);
// nullopt for anything that is not a JSON object; a missing or mistyped
// field keeps its default (so later fields can be added).
std::optional<PictureMode> DecodePictureMode(const std::string& payload);

}  // namespace rdm
