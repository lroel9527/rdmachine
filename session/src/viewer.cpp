#include "rdmachine/session/viewer.h"

namespace rdm {

rda_status CreateSessionViewer(const std::string& host, const SessionReady& ready, rda_viewer_config config,
                               rda_viewer** out, std::string* error) {
  // rdaccel wants a bare address, without the brackets of an IPv6 literal.
  std::string addr = host;
  if (addr.size() > 2 && addr.front() == '[' && addr.back() == ']') addr = addr.substr(1, addr.size() - 2);
  rda_transport* transport = nullptr;
  rda_status s = ready.transport == MediaTransport::kTcp ? rda_tcp_connect(addr.c_str(), ready.port, &transport)
                                                         : rda_udp_connect(addr.c_str(), ready.port, &transport);
  if (s != RDA_OK) {
    if (error) *error = std::string("media connect: ") + rda_last_error();
    return s;
  }
  config.struct_size = sizeof(rda_viewer_config);
  config.encryption_key = ready.key.data();  // copied
  config.allow_plaintext = 0;
  config.auth_token = ready.auth_token.data();  // copied
  config.auth_token_len = ready.auth_token.size();
  s = rda_viewer_create(transport, &config, out);  // takes the transport
  if (s != RDA_OK && error) *error = std::string("viewer: ") + rda_last_error();
  return s;
}

}  // namespace rdm
