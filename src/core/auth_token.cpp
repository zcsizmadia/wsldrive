#include "core/auth_token.hpp"

#include "core/hash.hpp"

#include <array>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>

#include <bcrypt.h>
#else
#include <sys/random.h>
#endif

namespace wsld {

std::string generate_auth_token() {
  std::array<unsigned char, 16> bytes{};
#ifdef _WIN32
  if (::BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
    return {};
#else
  std::size_t got = 0;
  while (got < bytes.size()) {
    const ssize_t n = ::getrandom(bytes.data() + got, bytes.size() - got, 0);
    if (n <= 0) return {};
    got += static_cast<std::size_t>(n);
  }
#endif
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const unsigned char b : bytes) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0F]);
  }
  return out;
}

std::string auth_token_from_env() {
#ifdef _WIN32
  char* buf = nullptr;
  std::size_t len = 0;
  if (::_dupenv_s(&buf, &len, kAuthTokenEnv) != 0 || buf == nullptr) return {};
  std::string out(buf);
  std::free(buf);
  return out;
#else
  const char* v = std::getenv(kAuthTokenEnv);
  return v != nullptr ? std::string(v) : std::string();
#endif
}



namespace {

// Length-prefix every field so that concatenating them cannot be ambiguous:
// without it, ("ab", "c") and ("a", "bc") would hash identically.
void feed(Blake3Hasher& h, std::string_view s) {
  h.update(std::to_string(s.size()));
  h.update(":");
  h.update(s);
}

}  // namespace

std::string generate_auth_nonce() { return generate_auth_token(); }

std::string auth_proof(std::string_view token, std::string_view client_nonce, std::string_view server_nonce,
                       bool from_server) {
  Blake3Hasher h;
  feed(h, "wsldrive-auth-v1");                     // domain separation
  feed(h, from_server ? "server" : "client");      // direction, so proofs are not interchangeable
  feed(h, token);
  feed(h, client_nonce);
  feed(h, server_nonce);
  return h.finalize().hex();
}

bool constant_time_equal(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i)
    diff = static_cast<unsigned char>(diff | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i])));
  return diff == 0;
}

}  // namespace wsld
