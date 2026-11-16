#pragma once
// Which video codec the server's picture uses (rdaccel 1.11, docs/decisions.md
// "视频编码"). Automatic: HEVC whenever this Mac encodes it in hardware fast
// enough for the picture's size, H.264 only when it does not and the client
// can decode H.264. Forcing is for testing and for Macs where the automatic
// choice is wrong; a client that cannot decode H.264 gets HEVC regardless.
#include <optional>
#include <string>

namespace rdm {

enum class VideoCodecChoice { kAuto, kHevc, kH264 };

inline const char* VideoCodecChoiceName(VideoCodecChoice c) {
  switch (c) {
    case VideoCodecChoice::kHevc: return "hevc";
    case VideoCodecChoice::kH264: return "h264";
    case VideoCodecChoice::kAuto: break;
  }
  return "auto";
}

inline std::optional<VideoCodecChoice> ParseVideoCodecChoice(const std::string& s) {
  if (s == "auto") return VideoCodecChoice::kAuto;
  if (s == "hevc") return VideoCodecChoice::kHevc;
  if (s == "h264") return VideoCodecChoice::kH264;
  return std::nullopt;
}

}  // namespace rdm
