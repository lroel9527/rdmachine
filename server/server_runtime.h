#pragma once
// The running server: the media host and control server built from the
// settings, restarted when they change. Shared by the command line (Linux,
// --headless) and the macOS menu bar app. Thread-safe.
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "rdmachine/control_server.h"
#include "rdmachine/session/media_host.h"
#include "rdmachine/session/server_settings.h"
#include "rdmachine/tls.h"

namespace rdm {

// Command-line options override the saved settings for one run.
struct SettingsOverrides {
  std::optional<uint16_t> port;
  std::optional<bool> offer_pairing_codes;
  std::optional<bool> view_only;
  std::optional<ClipboardMode> clipboard_mode;
  std::optional<int32_t> fps;
  std::optional<int32_t> bitrate_kbps;

  ServerSettings Apply(ServerSettings s) const;
  std::string Describe() const;  // "--port, --fps"; empty if none
};

class ServerRuntime {
 public:
  using Clock = std::chrono::steady_clock;

  struct Options {
    std::string config_dir;
    std::string server_name;
    std::string bind_address;  // empty = all interfaces
    std::string pam_service;   // empty = the platform default
    SettingsOverrides overrides;
    std::function<void(const std::string&)> log;
  };

  ServerRuntime(Options options, TlsIdentity identity);
  ~ServerRuntime();  // Stop()

  // Starts with the saved settings (plus overrides). A bad settings file or a
  // port in use fails with *error.
  bool Start(std::string* error);
  // Saves `saved` (what the settings window shows, without overrides) and
  // restarts with it; a running session ends.
  bool ApplySettings(const ServerSettings& saved, std::string* error);
  void Stop();
  // Shows or hides the statistics panel: saved, no restart.
  bool SetShowStatsOverlay(bool show, std::string* error);

  // The settings as saved in the file, and as in effect (with overrides).
  ServerSettings saved_settings() const;
  ServerSettings effective_settings() const;
  const SettingsOverrides& overrides() const { return options_.overrides; }
  bool running() const;
  uint16_t port() const;

  // A new pairing code (empty if pairing is off or the server is stopped).
  std::string IssuePairingCode();
  struct PairingCode {
    std::string code;  // empty: none issued, or used, voided or expired
    std::chrono::seconds remaining{0};
  };
  PairingCode CurrentPairingCode() const;

  // Ends the running session (the client's connection drops).
  void Disconnect();
  MediaHostStatus Status() const;

 private:
  bool StartLocked(const ServerSettings& saved, std::string* error);
  void StopLocked();
  void Log(const std::string& line) const;

  const Options options_;
  const TlsIdentity identity_;
  mutable std::mutex mu_;
  ServerSettings saved_;
  std::unique_ptr<MediaHost> media_;
  std::unique_ptr<ControlServer> server_;
  std::string code_;  // last issued; valid while the server says time remains
};

}  // namespace rdm
