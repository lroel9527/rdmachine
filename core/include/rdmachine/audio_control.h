#pragma once
// The sound during a session (docs/protocol.md, "声音"; protocol revision
// 3): the client tells the server to pause it (its user turned sound off in
// the menu, or its output is muted or at volume 0: nobody hears it, so
// nothing need be sent) and to go on. The server tells rdaccel
// (rda_host_set_audio_paused); this computer stays muted meanwhile.
//
// Platform independent: the message only.
#include <cstdint>
#include <optional>
#include <string>

namespace rdm {

// rdaccel application message type (RDA_MSG_USER_FIRST + 0x13), client to
// server.
constexpr uint32_t kAudioControlMessageType = 0x10013;

struct AudioControl {
  bool paused = false;

  bool operator==(const AudioControl&) const = default;
};

// The client overlay's line for a session without sound (docs/audio.md,
// 10): why there is none. `revision`: what both sides speak (sound needs 3);
// `requested`: whether this session asked for sound; `wanted_now`: whether
// the user wants it now (View > Play Sound).
std::string AudioOffLine(bool session_has_audio, const std::string& audio_reason, int revision, bool requested,
                         bool wanted_now);

// JSON: {"paused":false}
std::string EncodeAudioControl(const AudioControl& m);
// nullopt for anything that is not a JSON object; a missing or mistyped
// field keeps its default, unknown fields are ignored.
std::optional<AudioControl> DecodeAudioControl(const std::string& payload);

}  // namespace rdm
