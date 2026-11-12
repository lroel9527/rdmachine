#include "rdmachine/tls.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

namespace rdm {
namespace {

std::string MbedError(const char* what, int ret) {
  char buf[160];
  mbedtls_strerror(ret, buf, sizeof buf);
  char out[256];
  std::snprintf(out, sizeof out, "%s: %s (-0x%04x)", what, buf, static_cast<unsigned>(-ret));
  return out;
}

bool EnsurePsa(std::string* error) {
  static const psa_status_t status = psa_crypto_init();
  if (status != PSA_SUCCESS) {
    if (error) *error = "psa_crypto_init failed";
    return false;
  }
  return true;
}

// Entropy source + DRBG. Thread-safe because Mbed TLS is built with
// MBEDTLS_THREADING_C (cmake/mbedtls_user_config.h).
struct Rng {
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  Rng() {
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
  }
  ~Rng() {
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
  }
  int Seed(const char* personalization) {
    return mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                 reinterpret_cast<const unsigned char*>(personalization), std::strlen(personalization));
  }
};

std::optional<std::string> SpkiFingerprint(const mbedtls_pk_context* pk) {
  std::vector<unsigned char> der(1024);
  const int len = mbedtls_pk_write_pubkey_der(pk, der.data(), der.size());
  if (len <= 0) return std::nullopt;
  unsigned char hash[32];
  // mbedtls_pk_write_pubkey_der writes at the end of the buffer.
  if (mbedtls_sha256(der.data() + der.size() - len, static_cast<size_t>(len), hash, 0) != 0) return std::nullopt;
  std::string out = "sha256:";
  static const char kHex[] = "0123456789abcdef";
  for (unsigned char b : hash) {
    out += kHex[b >> 4];
    out += kHex[b & 15];
  }
  return out;
}

// The connection's address on the other end (local = false) or this one.
void FormatAddress(int fd, bool local, std::string* address, std::string* ip) {
  sockaddr_storage ss{};
  socklen_t len = sizeof ss;
  const int rc = local ? getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &len)
                       : getpeername(fd, reinterpret_cast<sockaddr*>(&ss), &len);
  if (rc != 0) return;
  char host[INET6_ADDRSTRLEN] = {};
  uint16_t port = 0;
  if (ss.ss_family == AF_INET) {
    auto* a = reinterpret_cast<sockaddr_in*>(&ss);
    inet_ntop(AF_INET, &a->sin_addr, host, sizeof host);
    port = ntohs(a->sin_port);
  } else if (ss.ss_family == AF_INET6) {
    auto* a = reinterpret_cast<sockaddr_in6*>(&ss);
    inet_ntop(AF_INET6, &a->sin6_addr, host, sizeof host);
    port = ntohs(a->sin6_port);
    // An IPv4 client on a dual-stack socket: report the IPv4 address.
    if (std::strncmp(host, "::ffff:", 7) == 0 && std::strchr(host + 7, '.')) std::memmove(host, host + 7, std::strlen(host + 7) + 1);
  }
  *ip = host;
  *address = (std::strchr(host, ':') ? "[" + *ip + "]" : *ip) + ":" + std::to_string(port);
}

void SetSendTimeout(int fd, int ms) {
  timeval tv{};
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

constexpr int kSendTimeoutMs = 10000;

// A TCP connection to host:port made within timeout_ms in all (every address
// the name has is tried in turn), as a blocking socket; -1 with *error. The
// operating system's own limit for a connection nobody answers is over a
// minute, far too long for a client that tries again every few seconds.
int ConnectTcp(const std::string& host, const std::string& port, int timeout_ms, std::string* error) {
  // As mbedtls_net_connect does: writing to a connection the peer closed is
  // an error to handle, not a signal that ends the process.
  static const bool sigpipe_ignored = [] {
    signal(SIGPIPE, SIG_IGN);
    return true;
  }();
  (void)sigpipe_ignored;
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* list = nullptr;
  const int gai = getaddrinfo(host.c_str(), port.c_str(), &hints, &list);
  if (gai != 0 || !list) {
    if (error) *error = "connect to " + host + ":" + port + ": " + (gai != 0 ? gai_strerror(gai) : "no address");
    return -1;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 1));
  std::string why = "timed out";
  int fd = -1;
  for (addrinfo* a = list; a && fd < 0; a = a->ai_next) {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) {
      why = "timed out";
      break;
    }
    const int s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (s < 0) {
      why = std::strerror(errno);
      continue;
    }
    const int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
    int err = 0;
    if (connect(s, a->ai_addr, a->ai_addrlen) != 0) {
      err = errno;
      if (err == EINPROGRESS || err == EINTR) {
        pollfd pfd{s, POLLOUT, 0};
        int pr;
        do {
          const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                                 std::chrono::steady_clock::now())
                              .count();
          pr = poll(&pfd, 1, static_cast<int>(std::max<long long>(ms, 0)));
        } while (pr < 0 && errno == EINTR);
        if (pr == 0) {
          err = ETIMEDOUT;
        } else if (pr < 0) {
          err = errno;
        } else {
          socklen_t len = sizeof err;
          if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) != 0) err = errno;
        }
      }
    }
    if (err != 0) {
      why = err == ETIMEDOUT ? "timed out" : std::strerror(err);
      close(s);
      continue;
    }
    fcntl(s, F_SETFL, flags & ~O_NONBLOCK);
#if defined(SO_NOSIGPIPE)
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    fd = s;
  }
  freeaddrinfo(list);
  if (fd < 0 && error) *error = "connect to " + host + ":" + port + ": " + why;
  return fd;
}

}  // namespace

// ---------------------------------------------------------------------------
// Identity

std::optional<TlsIdentity> GenerateTlsIdentity(const std::string& common_name, std::string* error) {
  if (!EnsurePsa(error)) return std::nullopt;
  Rng rng;
  int ret = rng.Seed("rdmachine identity");
  if (ret != 0) {
    if (error) *error = MbedError("seed", ret);
    return std::nullopt;
  }
  mbedtls_pk_context key;
  mbedtls_pk_init(&key);
  mbedtls_x509write_cert crt;
  mbedtls_x509write_crt_init(&crt);
  std::optional<TlsIdentity> result;
  do {
    if ((ret = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))) != 0 ||
        (ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), mbedtls_ctr_drbg_random, &rng.drbg)) != 0) {
      if (error) *error = MbedError("key generation", ret);
      break;
    }
    // Commas, equals signs and the like would change the meaning of the DN.
    std::string cn;
    for (char c : common_name) cn += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' || c == ' ') ? c : '-';
    if (cn.empty()) cn = "rdmachine";
    const std::string dn = "CN=" + cn + ",O=rdmachine";
    unsigned char serial[16];
    if ((ret = mbedtls_ctr_drbg_random(&rng.drbg, serial, sizeof serial)) != 0) {
      if (error) *error = MbedError("serial", ret);
      break;
    }
    serial[0] &= 0x7f;  // positive
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);
    if ((ret = mbedtls_x509write_crt_set_subject_name(&crt, dn.c_str())) != 0 ||
        (ret = mbedtls_x509write_crt_set_issuer_name(&crt, dn.c_str())) != 0 ||
        (ret = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof serial)) != 0 ||
        // Clients pin the key, not the dates; a long validity avoids an
        // expiry nobody would act on.
        (ret = mbedtls_x509write_crt_set_validity(&crt, "20240101000000", "20991231235959")) != 0 ||
        (ret = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1)) != 0) {
      if (error) *error = MbedError("certificate fields", ret);
      break;
    }
    std::vector<unsigned char> buf(4096);
    if ((ret = mbedtls_x509write_crt_pem(&crt, buf.data(), buf.size(), mbedtls_ctr_drbg_random, &rng.drbg)) != 0) {
      if (error) *error = MbedError("certificate", ret);
      break;
    }
    TlsIdentity id;
    id.cert_pem = reinterpret_cast<const char*>(buf.data());
    std::fill(buf.begin(), buf.end(), 0);
    if ((ret = mbedtls_pk_write_key_pem(&key, buf.data(), buf.size())) != 0) {
      if (error) *error = MbedError("key export", ret);
      break;
    }
    id.key_pem = reinterpret_cast<const char*>(buf.data());
    mbedtls_platform_zeroize(buf.data(), buf.size());
    result = std::move(id);
  } while (false);
  mbedtls_x509write_crt_free(&crt);
  mbedtls_pk_free(&key);
  return result;
}

std::optional<std::string> CertificateFingerprint(const std::string& cert_pem) {
  mbedtls_x509_crt crt;
  mbedtls_x509_crt_init(&crt);
  std::optional<std::string> fp;
  if (mbedtls_x509_crt_parse(&crt, reinterpret_cast<const unsigned char*>(cert_pem.c_str()), cert_pem.size() + 1) == 0)
    fp = SpkiFingerprint(&crt.pk);
  mbedtls_x509_crt_free(&crt);
  return fp;
}

std::string FormatFingerprint(const std::string& fingerprint) {
  std::string hex = fingerprint.rfind("sha256:", 0) == 0 ? fingerprint.substr(7) : fingerprint;
  std::string out;
  for (size_t i = 0; i < hex.size(); ++i) {
    if (i > 0 && i % 4 == 0) out += ' ';
    out += hex[i];
  }
  return out;
}

// ---------------------------------------------------------------------------
// Connection

struct TlsConnection::Impl {
  mbedtls_net_context net;
  mbedtls_ssl_context ssl;
  int timeout_ms = 0;  // for the next receive
  bool closed = false;

  Impl() {
    mbedtls_net_init(&net);
    mbedtls_ssl_init(&ssl);
  }
  ~Impl() {
    mbedtls_ssl_free(&ssl);
    mbedtls_net_free(&net);
  }

  static int Send(void* ctx, const unsigned char* buf, size_t len) {
    return mbedtls_net_send(&static_cast<Impl*>(ctx)->net, buf, len);
  }
  static int Recv(void* ctx, unsigned char* buf, size_t len) {
    auto* self = static_cast<Impl*>(ctx);
    return mbedtls_net_recv_timeout(&self->net, buf, len, static_cast<uint32_t>(std::max(self->timeout_ms, 1)));
  }

  // Runs the handshake to completion within timeout_ms.
  int Handshake(int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      if (left.count() <= 0) return MBEDTLS_ERR_SSL_TIMEOUT;
      this->timeout_ms = static_cast<int>(left.count());
      const int ret = mbedtls_ssl_handshake(&ssl);
      if (ret == 0) return 0;
      if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) return ret;
    }
  }
};

TlsConnection::TlsConnection(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
  FormatAddress(impl_->net.fd, false, &peer_address_, &peer_ip_);
  std::string local_address;
  FormatAddress(impl_->net.fd, true, &local_address, &local_ip_);
}

TlsConnection::~TlsConnection() { Close(); }

int TlsConnection::Read(uint8_t* buf, size_t n, int timeout_ms) {
  if (impl_->closed) return kClosed;
  impl_->timeout_ms = timeout_ms;
  for (;;) {
    const int ret = mbedtls_ssl_read(&impl_->ssl, buf, n);
    if (ret > 0) return ret;
    if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == MBEDTLS_ERR_NET_CONN_RESET) return kClosed;
    if (ret == MBEDTLS_ERR_SSL_TIMEOUT) return kTimeout;
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
        ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
      continue;
    return kError;
  }
}

bool TlsConnection::WriteAll(const uint8_t* data, size_t n) {
  if (impl_->closed) return false;
  while (n > 0) {
    const int ret = mbedtls_ssl_write(&impl_->ssl, data, n);
    if (ret > 0) {
      data += ret;
      n -= static_cast<size_t>(ret);
      continue;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
    return false;
  }
  return true;
}

void TlsConnection::Close() {
  if (!impl_ || impl_->closed) return;
  impl_->closed = true;
  impl_->timeout_ms = 200;
  mbedtls_ssl_close_notify(&impl_->ssl);
  mbedtls_net_close(&impl_->net);
}

void TlsConnection::Shutdown() {
  // Only a syscall on the descriptor: safe from another thread while Read()
  // is blocked, which then returns kClosed. The write side stays open so the
  // owner can still send a last message (bye) before closing.
  if (impl_ && impl_->net.fd >= 0) ::shutdown(impl_->net.fd, SHUT_RD);
}

bool TlsConnection::ServerHandshake(int timeout_ms, std::string* error) {
  const int ret = impl_->Handshake(timeout_ms);
  if (ret != 0) {
    if (error) *error = MbedError("TLS handshake", ret);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Listener

struct TlsListener::Impl {
  mbedtls_net_context net;
  mbedtls_ssl_config conf;
  mbedtls_x509_crt cert;
  mbedtls_pk_context key;
  Rng rng;

  Impl() {
    mbedtls_net_init(&net);
    mbedtls_ssl_config_init(&conf);
    mbedtls_x509_crt_init(&cert);
    mbedtls_pk_init(&key);
  }
  ~Impl() {
    mbedtls_net_free(&net);
    mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&cert);
    mbedtls_pk_free(&key);
  }
};

TlsListener::TlsListener(std::unique_ptr<Impl> impl, uint16_t port) : impl_(std::move(impl)), port_(port) {}
TlsListener::~TlsListener() { Close(); }

std::unique_ptr<TlsListener> TlsListener::Listen(const std::string& bind_address, uint16_t port,
                                                 const TlsIdentity& identity, std::string* error) {
  if (!EnsurePsa(error)) return nullptr;
  auto impl = std::make_unique<Impl>();
  int ret;
  if ((ret = impl->rng.Seed("rdmachine server")) != 0) {
    if (error) *error = MbedError("seed", ret);
    return nullptr;
  }
  if ((ret = mbedtls_x509_crt_parse(&impl->cert, reinterpret_cast<const unsigned char*>(identity.cert_pem.c_str()),
                                    identity.cert_pem.size() + 1)) != 0) {
    if (error) *error = MbedError("certificate", ret);
    return nullptr;
  }
  if ((ret = mbedtls_pk_parse_key(&impl->key, reinterpret_cast<const unsigned char*>(identity.key_pem.c_str()),
                                  identity.key_pem.size() + 1, nullptr, 0, mbedtls_ctr_drbg_random,
                                  &impl->rng.drbg)) != 0) {
    if (error) *error = MbedError("private key", ret);
    return nullptr;
  }
  mbedtls_ssl_config* conf = &impl->conf;
  if ((ret = mbedtls_ssl_config_defaults(conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
    if (error) *error = MbedError("TLS config", ret);
    return nullptr;
  }
  mbedtls_ssl_conf_min_tls_version(conf, MBEDTLS_SSL_VERSION_TLS1_3);
  mbedtls_ssl_conf_max_tls_version(conf, MBEDTLS_SSL_VERSION_TLS1_3);
  mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_NONE);
  mbedtls_ssl_conf_rng(conf, mbedtls_ctr_drbg_random, &impl->rng.drbg);
  if ((ret = mbedtls_ssl_conf_own_cert(conf, &impl->cert, &impl->key)) != 0) {
    if (error) *error = MbedError("TLS certificate", ret);
    return nullptr;
  }
  const std::string port_str = std::to_string(port);
  if ((ret = mbedtls_net_bind(&impl->net, bind_address.empty() ? nullptr : bind_address.c_str(), port_str.c_str(),
                              MBEDTLS_NET_PROTO_TCP)) != 0) {
    if (error) *error = MbedError(("listen on port " + port_str).c_str(), ret);
    return nullptr;
  }
  sockaddr_storage ss{};
  socklen_t len = sizeof ss;
  uint16_t bound = port;
  if (getsockname(impl->net.fd, reinterpret_cast<sockaddr*>(&ss), &len) == 0) {
    bound = ss.ss_family == AF_INET6 ? ntohs(reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port)
                                     : ntohs(reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
  }
  return std::unique_ptr<TlsListener>(new TlsListener(std::move(impl), bound));
}

std::unique_ptr<TlsConnection> TlsListener::Accept(int timeout_ms, std::string* error) {
  if (error) error->clear();
  if (!impl_ || impl_->net.fd < 0) {
    if (error) *error = "listener closed";
    return nullptr;
  }
  const int ready = mbedtls_net_poll(&impl_->net, MBEDTLS_NET_POLL_READ, static_cast<uint32_t>(timeout_ms));
  if (ready == 0) return nullptr;  // timeout
  if (ready < 0) {
    if (error) *error = MbedError("poll", ready);
    return nullptr;
  }
  auto conn = std::make_unique<TlsConnection::Impl>();
  int ret = mbedtls_net_accept(&impl_->net, &conn->net, nullptr, 0, nullptr);
  if (ret != 0) {
    if (error) *error = MbedError("accept", ret);
    return nullptr;
  }
  SetSendTimeout(conn->net.fd, kSendTimeoutMs);
  if ((ret = mbedtls_ssl_setup(&conn->ssl, &impl_->conf)) != 0) {
    if (error) *error = MbedError("TLS setup", ret);
    return nullptr;
  }
  mbedtls_ssl_set_bio(&conn->ssl, conn.get(), &TlsConnection::Impl::Send, &TlsConnection::Impl::Recv, nullptr);
  return std::unique_ptr<TlsConnection>(new TlsConnection(std::move(conn)));
}

void TlsListener::Close() {
  if (impl_) mbedtls_net_close(&impl_->net);
}

// ---------------------------------------------------------------------------
// Client

namespace {

struct ClientConf {
  mbedtls_ssl_config conf;
  Rng rng;
  ClientConf() { mbedtls_ssl_config_init(&conf); }
  ~ClientConf() { mbedtls_ssl_config_free(&conf); }
};

}  // namespace

std::unique_ptr<TlsConnection> TlsConnect(const std::string& host, uint16_t port, int timeout_ms,
                                          std::string* error) {
  if (!EnsurePsa(error)) return nullptr;
  // One configuration per process, set up on first use.
  static ClientConf* shared = [] {
    auto* c = new ClientConf();
    if (c->rng.Seed("rdmachine client") != 0 ||
        mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
      delete c;
      return static_cast<ClientConf*>(nullptr);
    }
    mbedtls_ssl_conf_min_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_3);
    // No CA chain: the server is self-signed and judged by the caller from
    // its key fingerprint. TLS 1.3 still makes the server prove it holds
    // that key (CertificateVerify), which Mbed TLS checks in this mode.
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->rng.drbg);
    return c;
  }();
  if (!shared) {
    if (error) *error = "TLS client setup failed";
    return nullptr;
  }

  auto impl = std::make_unique<TlsConnection::Impl>();
  const std::string port_str = std::to_string(port);
  impl->net.fd = ConnectTcp(host, port_str, timeout_ms, error);
  if (impl->net.fd < 0) return nullptr;
  SetSendTimeout(impl->net.fd, kSendTimeoutMs);
  int ret;
  if ((ret = mbedtls_ssl_setup(&impl->ssl, &shared->conf)) != 0 ||
      (ret = mbedtls_ssl_set_hostname(&impl->ssl, host.c_str())) != 0) {
    if (error) *error = MbedError("TLS setup", ret);
    return nullptr;
  }
  mbedtls_ssl_set_bio(&impl->ssl, impl.get(), &TlsConnection::Impl::Send, &TlsConnection::Impl::Recv, nullptr);
  if ((ret = impl->Handshake(timeout_ms)) != 0) {
    if (error) *error = MbedError("TLS handshake", ret);
    return nullptr;
  }
  const mbedtls_x509_crt* peer = mbedtls_ssl_get_peer_cert(&impl->ssl);
  std::optional<std::string> fp = peer ? SpkiFingerprint(&peer->pk) : std::nullopt;
  if (!fp) {
    if (error) *error = "server sent no usable certificate";
    return nullptr;
  }
  auto conn = std::unique_ptr<TlsConnection>(new TlsConnection(std::move(impl)));
  conn->peer_fingerprint_ = *fp;
  return conn;
}

}  // namespace rdm
