#pragma once
// TLS 1.3 over TCP for the control connection (docs/protocol.md, "TLS and
// server identity"). The server has a long-lived self-signed certificate; the
// client does not check a chain but pins the SHA-256 of the server's public
// key (SubjectPublicKeyInfo), trusting it on first use.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace rdm {

struct TlsIdentity {
  std::string cert_pem;
  std::string key_pem;  // secret: store with owner-only permissions
};

// A new ECDSA P-256 key and a self-signed certificate for it.
std::optional<TlsIdentity> GenerateTlsIdentity(const std::string& common_name, std::string* error);

// "sha256:" + 64 lowercase hex digits of SHA-256(SubjectPublicKeyInfo).
std::optional<std::string> CertificateFingerprint(const std::string& cert_pem);

// For people comparing by eye: the hex in groups of four, e.g. "3f2a 9c1e ...".
std::string FormatFingerprint(const std::string& fingerprint);

class TlsConnection {
 public:
  ~TlsConnection();
  TlsConnection(const TlsConnection&) = delete;
  TlsConnection& operator=(const TlsConnection&) = delete;

  enum : int { kClosed = 0, kTimeout = -1, kError = -2 };
  // Reads up to n bytes. Returns the count (> 0), kClosed when the peer closed
  // the connection, kTimeout after timeout_ms without data, or kError.
  int Read(uint8_t* buf, size_t n, int timeout_ms);
  bool WriteAll(const uint8_t* data, size_t n);
  // Sends close_notify (best effort) and closes the socket. Idempotent.
  // Not thread-safe with Read/WriteAll; use Shutdown() from another thread.
  void Close();
  // Shuts the socket's receiving side down without touching TLS state, so
  // that a Read() blocked on another thread returns kClosed; writing still
  // works. Safe to call from any thread.
  void Shutdown();
  // Server side: completes the TLS handshake of an accepted connection.
  bool ServerHandshake(int timeout_ms, std::string* error);

  // "ip:port" of the other end, for logs and rate limiting (ip only).
  const std::string& peer_address() const { return peer_address_; }
  const std::string& peer_ip() const { return peer_ip_; }
  // This end's address of the connection (IPv4 as such, not mapped into
  // IPv6); empty if unknown. On a computer with several ways out (VPN,
  // Tailscale, Wi-Fi and Ethernet) it is the address the other end chose.
  const std::string& local_ip() const { return local_ip_; }
  // Client side: fingerprint of the server's certificate.
  const std::string& peer_fingerprint() const { return peer_fingerprint_; }

  struct Impl;

 private:
  friend class TlsListener;
  friend std::unique_ptr<TlsConnection> TlsConnect(const std::string&, uint16_t, int, std::string*);
  explicit TlsConnection(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
  std::string peer_address_, peer_ip_, local_ip_, peer_fingerprint_;
};

class TlsListener {
 public:
  ~TlsListener();
  TlsListener(const TlsListener&) = delete;
  TlsListener& operator=(const TlsListener&) = delete;

  // Binds to `bind_address` (empty = all interfaces) and `port` (0 = any free
  // port; see port()).
  static std::unique_ptr<TlsListener> Listen(const std::string& bind_address, uint16_t port,
                                             const TlsIdentity& identity, std::string* error);

  uint16_t port() const { return port_; }

  // Waits up to timeout_ms for a client and accepts its TCP connection. The
  // caller runs ServerHandshake() (typically on the connection's own thread,
  // so a slow client cannot hold up others). Returns nullptr with an empty
  // *error on timeout.
  std::unique_ptr<TlsConnection> Accept(int timeout_ms, std::string* error);

  // Unblocks nothing by itself: callers loop on Accept with a short timeout.
  void Close();

  struct Impl;

 private:
  explicit TlsListener(std::unique_ptr<Impl> impl, uint16_t port);
  std::unique_ptr<Impl> impl_;
  uint16_t port_ = 0;
};

// Connects and completes the handshake within timeout_ms. The certificate is
// NOT judged here: check peer_fingerprint() against the pinned value before
// sending anything.
std::unique_ptr<TlsConnection> TlsConnect(const std::string& host, uint16_t port, int timeout_ms,
                                          std::string* error);

}  // namespace rdm
