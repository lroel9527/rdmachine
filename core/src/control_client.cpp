#include "rdmachine/control_client.h"

#include <type_traits>

namespace rdm {

void ClientConnection::Close(const std::string& reason) {
  if (closed_) return;
  closed_ = true;
  channel_.Send(Bye{reason});
  conn_->Close();
}

namespace {

// Waits for the next message, answering pings on the way.
bool Next(MessageChannel& channel, int timeout_ms, ConnectResult* result, Message* out) {
  for (;;) {
    auto r = channel.Receive(timeout_ms);
    switch (r.status) {
      case MessageChannel::Received::kMessage:
        if (auto* ping = std::get_if<Ping>(&*r.message)) {
          if (!channel.Send(Pong{ping->id})) {
            result->error = "connection_closed";
            return false;
          }
          continue;
        }
        if (auto* err = std::get_if<Error>(&*r.message)) {
          result->error = err->code.empty() ? "internal" : err->code;
          result->detail = err->message;
          return false;
        }
        if (auto* bye = std::get_if<Bye>(&*r.message)) {
          result->error = "connection_closed";
          result->detail = "server said bye: " + bye->reason;
          return false;
        }
        *out = std::move(*r.message);
        return true;
      case MessageChannel::Received::kTimeout:
        result->error = "timeout";
        return false;
      case MessageChannel::Received::kClosed:
        result->error = "connection_closed";
        return false;
      case MessageChannel::Received::kProtocolError:
        result->error = "bad_message";
        result->detail = r.error;
        return false;
    }
  }
}

template <class T>
const T* Expect(const Message& m, ConnectResult* result) {
  const T* p = std::get_if<T>(&m);
  if (!p) {
    result->error = "bad_message";
    result->detail = std::string("unexpected ") + MessageType(m);
  }
  return p;
}

}  // namespace

ConnectResult ConnectAndStartSession(const ClientOptions& options) {
  ConnectResult result;
  std::string error;
  std::unique_ptr<TlsConnection> conn = TlsConnect(options.host, options.port, options.timeout_ms, &error);
  if (!conn) {
    result.error = "connect_failed";
    result.detail = error;
    return result;
  }
  result.fingerprint = conn->peer_fingerprint();
  if (!options.trust_server || !options.trust_server(result.fingerprint)) {
    result.error = "untrusted_server";
    conn->Close();
    return result;
  }
  auto client = std::make_unique<ClientConnection>(std::move(conn));
  MessageChannel& ch = client->channel();

  ClientHello hello;
  hello.client_name = options.client_name;
  hello.client_version = VersionString();
  if (!ch.Send(hello)) {
    result.error = "connection_closed";
    return result;
  }
  Message m;
  if (!Next(ch, options.timeout_ms, &result, &m)) return result;
  const auto* server = Expect<ServerHello>(m, &result);
  if (!server) return result;
  result.server = *server;
  if (server->protocol != kProtocolVersion) {
    result.error = "incompatible";
    return result;
  }

  if (!ch.Send(options.auth)) {
    result.error = "connection_closed";
    return result;
  }
  if (!Next(ch, options.timeout_ms, &result, &m)) return result;
  const auto* auth = Expect<AuthResult>(m, &result);
  if (!auth) return result;
  if (!auth->ok) {
    result.error = auth->error.empty() ? "bad_credentials" : auth->error;
    result.retry_after_ms = auth->retry_after_ms;
    return result;
  }

  if (!ch.Send(options.request)) {
    result.error = "connection_closed";
    return result;
  }
  if (!Next(ch, options.timeout_ms, &result, &m)) return result;
  const auto* ready = Expect<SessionReady>(m, &result);
  if (!ready) return result;
  result.session = *ready;
  result.connection = std::move(client);
  return result;
}

}  // namespace rdm
