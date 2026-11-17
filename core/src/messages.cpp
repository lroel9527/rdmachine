#include "rdmachine/messages.h"

#include <algorithm>
#include <limits>

#include <nlohmann/json.hpp>

#include "rdmachine/base64.h"

namespace rdm {

const char* VersionString() { return RDM_VERSION_STRING; }

namespace {

using nlohmann::json;

// Field readers. Each returns false and sets *err when the field is missing
// (unless optional) or has the wrong type.
class Reader {
 public:
  explicit Reader(const json& j) : j_(j) {}

  bool Str(const char* key, std::string* out, bool required = true) {
    auto it = j_.find(key);
    if (it == j_.end()) return !required || Fail(key, "missing");
    if (!it->is_string()) return Fail(key, "not a string");
    *out = it->get<std::string>();
    return true;
  }

  bool Int(const char* key, int64_t* out, bool required = true, int64_t lo = std::numeric_limits<int64_t>::min(),
           int64_t hi = std::numeric_limits<int64_t>::max()) {
    auto it = j_.find(key);
    if (it == j_.end()) return !required || Fail(key, "missing");
    if (!it->is_number_integer()) return Fail(key, "not an integer");
    const int64_t v = it->get<int64_t>();
    if (v < lo || v > hi) return Fail(key, "out of range");
    *out = v;
    return true;
  }

  bool Bool(const char* key, bool* out, bool required = true) {
    auto it = j_.find(key);
    if (it == j_.end()) return !required || Fail(key, "missing");
    if (!it->is_boolean()) return Fail(key, "not a boolean");
    *out = it->get<bool>();
    return true;
  }

  bool Num(const char* key, double* out, bool required = true) {
    auto it = j_.find(key);
    if (it == j_.end()) return !required || Fail(key, "missing");
    if (!it->is_number()) return Fail(key, "not a number");
    *out = it->get<double>();
    return true;
  }

  template <size_t N>
  bool Bytes(const char* key, std::array<uint8_t, N>* out) {
    std::string s;
    if (!Str(key, &s)) return false;
    auto bytes = Base64Decode(s);
    if (!bytes) return Fail(key, "not base64");
    if (bytes->size() != N) return Fail(key, "wrong length");
    std::copy(bytes->begin(), bytes->end(), out->begin());
    return true;
  }

  bool Fail(const char* key, const char* what) {
    error = std::string(key) + ": " + what;
    return false;
  }

  std::string error;

 private:
  const json& j_;
};

json ToJson(const ClientHello& m) {
  return {{"protocol", m.protocol},
          {"revision", m.revision},
          {"client_name", m.client_name},
          {"client_version", m.client_version}};
}
json ToJson(const ServerHello& m) {
  // auth_methods: clients before 6.0 need the list (they also knew
  // pairing codes); a password is the only way in.
  return {{"protocol", m.protocol},
          {"revision", m.revision},
          {"server_name", m.server_name},
          {"server_version", m.server_version},
          {"auth_methods", json::array({"password"})}};
}
json ToJson(const Auth& m) {
  return {{"method", "password"}, {"username", m.username}, {"password", m.password}};
}
json ToJson(const AuthResult& m) {
  json j = {{"ok", m.ok}};
  if (!m.ok) j["error"] = m.error;
  if (m.retry_after_ms) j["retry_after_ms"] = *m.retry_after_ms;
  return j;
}
json ToJson(const DesktopSpec& d) { return {{"width", d.width}, {"height", d.height}, {"scale", d.scale}}; }

// An optional "desktop" object. False (with *error) only when it is there
// and malformed.
bool ReadDesktop(const json& j, std::optional<DesktopSpec>* out, std::string* error) {
  auto it = j.find("desktop");
  if (it == j.end() || it->is_null()) return true;
  if (!it->is_object()) {
    *error = "desktop: not an object";
    return false;
  }
  Reader r(*it);
  DesktopSpec d;
  int64_t width = 0, height = 0;
  if (!r.Int("width", &width, true, 1, 1 << 20) || !r.Int("height", &height, true, 1, 1 << 20) ||
      !r.Num("scale", &d.scale, false)) {
    *error = "desktop: " + r.error;
    return false;
  }
  if (!(d.scale > 0) || d.scale > 16) {
    *error = "desktop: scale: out of range";
    return false;
  }
  d.width = static_cast<int32_t>(width);
  d.height = static_cast<int32_t>(height);
  *out = d;
  return true;
}

// An extra desktop or display: {"desktop":{...},"x":..,"y":..} and, for a
// display (port != null), "port".
bool ReadExtra(const json& e, DesktopSpec* desktop, int32_t* x, int32_t* y, uint16_t* port, std::string* error) {
  if (!e.is_object()) {
    *error = "not an object";
    return false;
  }
  std::optional<DesktopSpec> d;
  if (!ReadDesktop(e, &d, error)) return false;
  if (!d) {
    *error = "desktop: missing";
    return false;
  }
  Reader r(e);
  int64_t ex = 0, ey = 0, p = 0;
  if (!r.Int("x", &ex, true, -100000, 100000) || !r.Int("y", &ey, true, -100000, 100000) ||
      (port && !r.Int("port", &p, true, 1, 65535))) {
    *error = r.error;
    return false;
  }
  *desktop = *d;
  *x = static_cast<int32_t>(ex);
  *y = static_cast<int32_t>(ey);
  if (port) *port = static_cast<uint16_t>(p);
  return true;
}

json ToJson(const SessionRequest& m) {
  json j = {{"display_index", m.display_index}, {"view_only", m.view_only}};
  if (m.desktop) j["desktop"] = ToJson(*m.desktop);
  if (m.text_mode) j["text_mode"] = true;
  if (m.audio) j["audio"] = true;
  if (!m.extra_desktops.empty()) {
    json extras = json::array();
    for (const ExtraDesktop& e : m.extra_desktops) extras.push_back({{"desktop", ToJson(e.desktop)}, {"x", e.x}, {"y", e.y}});
    j["extra_desktops"] = std::move(extras);
  }
  return j;
}
json ToJson(const SessionReady& m) {
  json displays = json::array();
  for (const DisplayInfo& d : m.displays) {
    displays.push_back(
        {{"index", d.index}, {"name", d.name}, {"width", d.width}, {"height", d.height}, {"scale", d.scale}});
  }
  // transport: clients before 6.0 need it (they could also ask for TCP).
  json j = {{"transport", "udp"},
            {"port", m.port},
            {"key", Base64Encode(m.key)},
            {"auth_token", Base64Encode(m.auth_token)},
            {"displays", displays}};
  if (m.desktop) j["desktop"] = ToJson(*m.desktop);
  if (m.login_window) j["login_window"] = true;
  if (m.audio) j["audio"] = true;
  if (!m.audio && !m.audio_reason.empty()) j["audio_reason"] = m.audio_reason;
  if (!m.extra_displays.empty()) {
    json extras = json::array();
    for (const ExtraDisplay& e : m.extra_displays) {
      extras.push_back({{"port", e.port}, {"desktop", ToJson(e.desktop)}, {"x", e.x}, {"y", e.y}});
    }
    j["extra_displays"] = std::move(extras);
  }
  return j;
}
json ToJson(const Ping& m) { return {{"id", m.id}}; }
json ToJson(const Pong& m) { return {{"id", m.id}}; }
json ToJson(const Bye& m) { return {{"reason", m.reason}}; }
json ToJson(const SessionMoving& m) { return {{"reason", m.reason}}; }
json ToJson(const Error& m) { return {{"code", m.code}, {"message", m.message}}; }

template <typename T>
ParseResult Ok(T m) {
  return ParseResult{Message(std::move(m)), {}};
}
ParseResult Bad(std::string error) { return ParseResult{std::nullopt, std::move(error)}; }

ParseResult ParseClientHello(Reader& r) {
  ClientHello m;
  int64_t protocol = 0;
  if (!r.Int("protocol", &protocol, true, 0, std::numeric_limits<int>::max()) ||
      !r.Str("client_name", &m.client_name) || !r.Str("client_version", &m.client_version))
    return Bad(r.error);
  m.protocol = static_cast<int>(protocol);
  int64_t revision = 0;  // an older client says none
  if (!r.Int("revision", &revision, false, 0, std::numeric_limits<int>::max())) return Bad(r.error);
  m.revision = static_cast<int>(revision);
  return Ok(std::move(m));
}

ParseResult ParseServerHello(Reader& r) {
  ServerHello m;
  int64_t protocol = 0;
  if (!r.Int("protocol", &protocol, true, 0, std::numeric_limits<int>::max()) ||
      !r.Str("server_name", &m.server_name) || !r.Str("server_version", &m.server_version))
    return Bad(r.error);
  m.protocol = static_cast<int>(protocol);
  int64_t revision = 0;  // an older server says none
  if (!r.Int("revision", &revision, false, 0, std::numeric_limits<int>::max())) return Bad(r.error);
  m.revision = static_cast<int>(revision);
  return Ok(std::move(m));
}

ParseResult ParseAuth(Reader& r) {
  Auth m;
  std::string method;
  if (!r.Str("method", &method)) return Bad(r.error);
  // A client before 6.0 may try a pairing code: there are none any more.
  if (method != "password") return Bad("method: unknown");
  if (!r.Str("username", &m.username) || !r.Str("password", &m.password)) return Bad(r.error);
  return Ok(std::move(m));
}

ParseResult ParseAuthResult(Reader& r) {
  AuthResult m;
  if (!r.Bool("ok", &m.ok)) return Bad(r.error);
  if (!m.ok && !r.Str("error", &m.error)) return Bad(r.error);
  int64_t retry = -1;
  if (!r.Int("retry_after_ms", &retry, false, 0)) return Bad(r.error);
  if (retry >= 0) m.retry_after_ms = retry;
  return Ok(std::move(m));
}

ParseResult ParseSessionRequest(const json& j, Reader& r) {
  SessionRequest m;
  int64_t display = 0;
  std::string transport = "udp";
  if (!r.Int("display_index", &display, false, 0, std::numeric_limits<int32_t>::max()) ||
      !r.Str("transport", &transport, false) || !r.Bool("view_only", &m.view_only, false) ||
      !r.Bool("text_mode", &m.text_mode, false) || !r.Bool("audio", &m.audio, false))
    return Bad(r.error);
  // A client before 6.0 asks for "tcp" when UDP did not get through.
  if (transport != "udp") return Bad("transport: unknown");
  m.display_index = static_cast<int32_t>(display);
  std::string error;
  if (!ReadDesktop(j, &m.desktop, &error)) return Bad(error);
  if (auto it = j.find("extra_desktops"); it != j.end()) {
    if (!it->is_array() || it->size() > kMaxExtraDesktops) return Bad("extra_desktops: not an array of at most 3");
    for (const json& e : *it) {
      ExtraDesktop x;
      if (!ReadExtra(e, &x.desktop, &x.x, &x.y, nullptr, &error)) return Bad("extra_desktops: " + error);
      m.extra_desktops.push_back(x);
    }
  }
  return Ok(std::move(m));
}

ParseResult ParseSessionReady(const json& j, Reader& r) {
  SessionReady m;
  int64_t port = 0;
  if (!r.Int("port", &port, true, 1, 65535) || !r.Bytes("key", &m.key) ||
      !r.Bytes("auth_token", &m.auth_token) || !r.Bool("login_window", &m.login_window, false) ||
      !r.Bool("audio", &m.audio, false) || !r.Str("audio_reason", &m.audio_reason, false))
    return Bad(r.error);
  m.port = static_cast<uint16_t>(port);
  auto it = j.find("displays");
  if (it == j.end() || !it->is_array()) return Bad("displays: missing or not an array");
  for (const json& d : *it) {
    if (!d.is_object()) return Bad("displays: not an object");
    Reader dr(d);
    DisplayInfo info;
    int64_t index = 0, width = 0, height = 0;
    if (!dr.Int("index", &index, true, 0, std::numeric_limits<int32_t>::max()) || !dr.Str("name", &info.name) ||
        !dr.Int("width", &width, true, 1, std::numeric_limits<int32_t>::max()) ||
        !dr.Int("height", &height, true, 1, std::numeric_limits<int32_t>::max()) ||
        !dr.Num("scale", &info.scale, false))
      return Bad("displays: " + dr.error);
    info.index = static_cast<int32_t>(index);
    info.width = static_cast<int32_t>(width);
    info.height = static_cast<int32_t>(height);
    m.displays.push_back(std::move(info));
  }
  std::string error;
  if (!ReadDesktop(j, &m.desktop, &error)) return Bad(error);
  if (auto it = j.find("extra_displays"); it != j.end()) {
    if (!it->is_array() || it->size() > kMaxExtraDesktops) return Bad("extra_displays: not an array of at most 3");
    for (const json& e : *it) {
      ExtraDisplay x;
      if (!ReadExtra(e, &x.desktop, &x.x, &x.y, &x.port, &error)) return Bad("extra_displays: " + error);
      m.extra_displays.push_back(x);
    }
  }
  return Ok(std::move(m));
}

}  // namespace

const char* MessageType(const Message& m) {
  struct V {
    const char* operator()(const ClientHello&) { return "hello"; }
    const char* operator()(const ServerHello&) { return "server_hello"; }
    const char* operator()(const Auth&) { return "auth"; }
    const char* operator()(const AuthResult&) { return "auth_result"; }
    const char* operator()(const SessionRequest&) { return "session_request"; }
    const char* operator()(const SessionReady&) { return "session_ready"; }
    const char* operator()(const Ping&) { return "ping"; }
    const char* operator()(const Pong&) { return "pong"; }
    const char* operator()(const Bye&) { return "bye"; }
    const char* operator()(const Error&) { return "error"; }
    const char* operator()(const SessionMoving&) { return "session_moving"; }
  };
  return std::visit(V{}, m);
}

std::string SerializeMessage(const Message& m) {
  json j = std::visit([](const auto& v) { return ToJson(v); }, m);
  j["type"] = MessageType(m);
  return j.dump();
}

ParseResult ParseMessage(std::string_view text) {
  json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded()) return Bad("not valid JSON");
  if (!j.is_object()) return Bad("not a JSON object");
  Reader r(j);
  std::string type;
  if (!r.Str("type", &type)) return Bad(r.error);

  if (type == "hello") return ParseClientHello(r);
  if (type == "server_hello") return ParseServerHello(r);
  if (type == "auth") return ParseAuth(r);
  if (type == "auth_result") return ParseAuthResult(r);
  if (type == "session_request") return ParseSessionRequest(j, r);
  if (type == "session_ready") return ParseSessionReady(j, r);
  if (type == "ping" || type == "pong") {
    int64_t id = 0;
    if (!r.Int("id", &id)) return Bad(r.error);
    if (type == "ping") return Ok(Ping{id});
    return Ok(Pong{id});
  }
  if (type == "bye") {
    Bye m;
    if (!r.Str("reason", &m.reason)) return Bad(r.error);
    return Ok(std::move(m));
  }
  if (type == "session_moving") {
    SessionMoving m;
    if (!r.Str("reason", &m.reason, false)) return Bad(r.error);
    return Ok(std::move(m));
  }
  if (type == "error") {
    Error m;
    if (!r.Str("code", &m.code) || !r.Str("message", &m.message, false)) return Bad(r.error);
    return Ok(std::move(m));
  }
  return Bad("type: unknown \"" + type + "\"");
}

}  // namespace rdm
