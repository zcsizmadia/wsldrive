#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace wsld {

/// Fast 64-bit finaliser (splitmix-style). Good enough for hash tables, not for security.
[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t x) noexcept {
  x ^= x >> 32;
  x *= 0xd6e8feb86659fd93ULL;
  x ^= x >> 32;
  x *= 0xd6e8feb86659fd93ULL;
  x ^= x >> 32;
  return x;
}

/// Non-cryptographic hash of an arbitrary byte range, 8 bytes per step.
[[nodiscard]] inline std::uint64_t hash_bytes(const void* data, std::size_t len) noexcept {
  const auto* p = static_cast<const unsigned char*>(data);
  std::uint64_t h = 0x9E3779B97F4A7C15ULL ^ (static_cast<std::uint64_t>(len) * 0xff51afd7ed558ccdULL);
  while (len >= 8) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    h = (h ^ v) * 0x9FB21C651E98DF25ULL;
    h ^= h >> 29;
    p += 8;
    len -= 8;
  }
  if (len != 0) {
    std::uint64_t v = 0;
    std::memcpy(&v, p, len);
    h = (h ^ v) * 0x9FB21C651E98DF25ULL;
    h ^= h >> 29;
  }
  return mix64(h);
}

[[nodiscard]] inline std::uint64_t hash_string(std::string_view s) noexcept {
  return hash_bytes(s.data(), s.size());
}

/// Transparent hasher usable with std::string and std::string_view keys alike.
struct StringHash {
  using is_transparent = void;
  std::size_t operator()(std::string_view s) const noexcept { return static_cast<std::size_t>(hash_string(s)); }
};

}  // namespace wsld
