#include "rdmachine/random.h"

#include <mutex>

#include <psa/crypto.h>

namespace rdm {

bool RandomBytes(std::span<uint8_t> out) {
  static std::once_flag once;
  static psa_status_t init = PSA_ERROR_BAD_STATE;
  std::call_once(once, [] { init = psa_crypto_init(); });
  if (init != PSA_SUCCESS) return false;
  return psa_generate_random(out.data(), out.size()) == PSA_SUCCESS;
}

}  // namespace rdm
