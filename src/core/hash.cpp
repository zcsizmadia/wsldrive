#include "core/hash.hpp"

#include <blake3.h>

#include <cstring>
#include <new>

namespace wsld {

static_assert(sizeof(blake3_hasher) <= 1912, "Blake3Hasher::state_ is too small for blake3_hasher");

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

blake3_hasher* as_hasher(std::array<std::byte, 1912>& s) noexcept {
  return std::launder(reinterpret_cast<blake3_hasher*>(s.data()));
}
const blake3_hasher* as_hasher(const std::array<std::byte, 1912>& s) noexcept {
  return std::launder(reinterpret_cast<const blake3_hasher*>(s.data()));
}

}  // namespace

std::string Digest::hex() const {
  std::string out(64, '\0');
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    out[2 * i] = kHexDigits[bytes[i] >> 4];
    out[2 * i + 1] = kHexDigits[bytes[i] & 0x0F];
  }
  return out;
}

std::optional<Digest> Digest::from_hex(std::string_view hex) noexcept {
  if (hex.size() != 64) return std::nullopt;
  Digest d;
  for (std::size_t i = 0; i < 32; ++i) {
    const int hi = hex_value(hex[2 * i]);
    const int lo = hex_value(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    d.bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  return d;
}

Digest blake3_hash(std::span<const std::byte> data) noexcept {
  blake3_hasher h;
  blake3_hasher_init(&h);
  blake3_hasher_update(&h, data.data(), data.size());
  Digest d;
  blake3_hasher_finalize(&h, d.bytes.data(), d.bytes.size());
  return d;
}

Blake3Hasher::Blake3Hasher() noexcept { reset(); }

void Blake3Hasher::reset() noexcept { blake3_hasher_init(as_hasher(state_)); }

void Blake3Hasher::update(std::span<const std::byte> data) noexcept {
  blake3_hasher_update(as_hasher(state_), data.data(), data.size());
}

Digest Blake3Hasher::finalize() const noexcept {
  Digest d;
  blake3_hasher_finalize(as_hasher(state_), d.bytes.data(), d.bytes.size());
  return d;
}

}  // namespace wsld
