#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace wsld {

/// 256-bit BLAKE3 digest identifying file content in the cache and on the wire.
struct Digest {
  std::array<std::uint8_t, 32> bytes{};

  friend bool operator==(const Digest&, const Digest&) = default;
  [[nodiscard]] std::string hex() const;
  [[nodiscard]] static std::optional<Digest> from_hex(std::string_view hex) noexcept;
};

struct DigestHash {
  std::size_t operator()(const Digest& d) const noexcept {
    std::size_t h;
    static_assert(sizeof(h) <= sizeof(d.bytes));
    std::memcpy(&h, d.bytes.data(), sizeof(h));  // already uniformly random
    return h;
  }
};

/// One-shot BLAKE3 of a buffer.
[[nodiscard]] Digest blake3_hash(std::span<const std::byte> data) noexcept;
[[nodiscard]] inline Digest blake3_hash(std::string_view s) noexcept {
  return blake3_hash(std::as_bytes(std::span{s.data(), s.size()}));
}

/// Incremental BLAKE3 hasher. Movable, cheap to construct (no heap).
class Blake3Hasher {
 public:
  Blake3Hasher() noexcept;
  void update(std::span<const std::byte> data) noexcept;
  void update(std::string_view s) noexcept { update(std::as_bytes(std::span{s.data(), s.size()})); }
  [[nodiscard]] Digest finalize() const noexcept;
  void reset() noexcept;

 private:
  // Opaque storage for blake3_hasher (sizeof == 1912 on all supported targets);
  // keeps <blake3.h> out of the public header.
  alignas(16) std::array<std::byte, 1912> state_;
};

}  // namespace wsld
