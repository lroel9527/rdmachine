#include "rdmachine/messages.h"

#include <algorithm>
#include <limits>

#include <nlohmann/json.hpp>

#include "rdmachine/base64.h"

namespace rdm {

const char* VersionString() { return RDM_VERSION_STRING; }

namespace {

using nlohmann::json;

const char* AuthMethodName(AuthMethod m) {
  switch (m) {
    case AuthMethod::kPassword: return "password";
    case AuthMethod::kPairingCode: return "pairing_code";
  }
  return "";
}

std::optional<AuthMethod> AuthMethodFrom(std::string_view s) {
  if (s == "password") return AuthMethod::kPassword;
  if (s == "pairing_code") return AuthMethod::kPairingCode;
  return std::nullopt;
}

const char* TransportName(MediaTransport t) {
  return t == MediaTransport::kTcp ? "tcp" : "udp";
}

std::optional<MediaTransport> TransportFrom(std::string_view s) {
  if (s == "udp") return MediaTransport::kUdp;
  if (s == "tcp") return MediaTransport::kTcp;
  return std::nullopt;
}

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
  return {{"protocol", m.protocol}, {"client_name", m.client_name}, {"client_version", m.client_version}};
}
json ToJson(const ServerHello& m) {
  json methods = json::array();
  for (AuthMethod a : m.auth_methods) methods.push_back(AuthMethodName(a));
  return {{"protocol", m.protocol},
          {"server_name", m.server_name},
          {"server_version", m.server_version},
          {"auth_methods", methods}};
}
json ToJson(const Auth& m) {
  json j = {{"method", AuthMethodName(m.method)}};
  if (m.method == AuthMethod::kPassword) {
    j["username"] = m.username;
    j["password"] = m.password;
  } else {
    j["code"] = m.code;
  }
  return j;
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

json ToJson(const SessionRequest& m) {
  json j = {{"display_index", m.display_index}, {"transport", TransportName(m.transport)}, {"view_only", m.view_only}};
  if (m.desktop) j["desktop"] = ToJson(*m.desktop);
  if (m.text_mode) j["text_mode"] = true;
  return j;
}
json ToJson(const SessionReady& m) {
  json displays = json::array();
  for (const DisplayInfo& d : m.displays) {
    displays.push_back(
        {{"index", d.index}, {"name", d.name}, {"width", d.width}, {"height", d.height}, {"scale", d.scale}});
  }
  json j = {{"transport", TransportName(m.transport)},
            {"port", m.port},
            {"key", Base64Encode(m.key)},
            {"auth_token", Base64Encode(m.auth_token)},
            {"displays", displays}};
  if (m.desktop) j["desktop"] = ToJson(*m.desktop);
  if (m.login_window) j["login_window"] = true;
  return j;
}
json ToJson(const Ping& m) { return {{"id", m.id}}; }
json ToJson(const Pong& m) { return {{"id", m.id}}; }
json ToJson(const Bye& m) { return {{"reason", m.reason}}; }
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
  return Ok(std::move(m));
}

ParseResult ParseServerHello(const json& j, Reader& r) {
  ServerHello m;
  int64_t protocol = 0;
  if (!r.Int("protocol", &protocol, true, 0, std::numeric_limits<int>::max()) ||
      !r.Str("server_name", &m.server_name) || !r.Str("server_version", &m.server_version))
    return Bad(r.error);
  m.protocol = static_cast<int>(protocol);
  auto it = j.find("auth_methods");
  if (it == j.end() || !it->is_array()) return Bad("auth_methods: missing or not an array");
  for (const json& v : *it) {
    // Methods this client does not know are skipped: a newer server may offer more.
    if (!v.is_string()) return Bad("auth_methods: not a string");
    if (auto a = AuthMethodFrom(v.get<std::string>())) m.auth_methods.push_back(*a);
  }
  return Ok(std::move(m));
}

ParseResult ParseAuth(Reader& r) {
  Auth m;
  std::string method;
  if (!r.Str("method", &method)) return Bad(r.error);
  auto a = AuthMethodFrom(method);
  if (!a) return Bad("method: unknown");
  m.method = *a;
  if (m.method == AuthMethod::kPassword) {
    if (!r.Str("username", &m.username) || !r.Str("password", &m.password)) return Bad(r.error);
  } else {
    if (!r.Str("code", &m.code)) return Bad(r.error);
  }
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
      !r.Bool("text_mode", &m.text_mode, false))
    return Bad(r.error);
  auto t = TransportFrom(transport);
  if (!t) return Bad("transport: unknown");
  m.display_index = static_cast<int32_t>(display);
  m.transport = *t;
  std::string error;
  if (!ReadDesktop(j, &m.desktop, &error)) return Bad(error);
  return Ok(std::move(m));
}

ParseResult ParseSessionReady(const json& j, Reader& r) {
  SessionReady m;
  std::string transport;
  int64_t port = 0;
  if (!r.Str("transport", &transport) || !r.Int("port", &port, true, 1, 65535) || !r.Bytes("key", &m.key) ||
      !r.Bytes("auth_token", &m.auth_token) || !r.Bool("login_window", &m.login_window, false))
    return Bad(r.error);
  auto t = TransportFrom(transport);
  if (!t) return Bad("transport: unknown");
  m.transport = *t;
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
  if (type == "server_hello") return ParseServerHello(j, r);
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
  if (type == "error") {
    Error m;
    if (!r.Str("code", &m.code) || !r.Str("message", &m.message, false)) return Bad(r.error);
    return Ok(std::move(m));
  }
  return Bad("type: unknown \"" + type + "\"");
}

}  // namespace rdm
