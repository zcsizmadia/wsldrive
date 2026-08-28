#pragma once

#include "core/hash_util.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace wsld {

/// Open-addressing hash map from 64-bit keys to 32-bit values with linear probing.
///
/// Designed for the metadata tree's (parent, name) -> node index lookups: a hot,
/// read-mostly path where std::unordered_map's per-node allocation and pointer
/// chasing dominate. Slots are 16 bytes and stored contiguously.
///
/// Two key values are reserved as sentinels and must never be inserted:
/// `kEmpty` (all bits set) and `kTombstone` (all bits set minus one).
class U64Map {
 public:
  static constexpr std::uint64_t kEmpty = ~0ULL;
  static constexpr std::uint64_t kTombstone = ~0ULL - 1;

  U64Map() = default;
  explicit U64Map(std::size_t expected_size) { reserve(expected_size); }

  [[nodiscard]] const std::uint32_t* find(std::uint64_t key) const noexcept {
    assert(key != kEmpty && key != kTombstone);
    if (slots_.empty()) return nullptr;
    const std::size_t mask = slots_.size() - 1;
    std::size_t i = static_cast<std::size_t>(mix64(key)) & mask;
    for (;;) {
      const Slot& s = slots_[i];
      if (s.key == key) return &s.value;
      if (s.key == kEmpty) return nullptr;
      i = (i + 1) & mask;
    }
  }

  [[nodiscard]] std::uint32_t* find(std::uint64_t key) noexcept {
    return const_cast<std::uint32_t*>(std::as_const(*this).find(key));
  }

  [[nodiscard]] bool contains(std::uint64_t key) const noexcept { return find(key) != nullptr; }

  /// Inserts or overwrites. Returns true if a new key was inserted.
  bool insert_or_assign(std::uint64_t key, std::uint32_t value) { return emplace(key, value, /*assign=*/true); }

  /// Inserts only if absent. Returns true if a new key was inserted.
  bool insert_if_absent(std::uint64_t key, std::uint32_t value) { return emplace(key, value, /*assign=*/false); }

  bool erase(std::uint64_t key) noexcept {
    assert(key != kEmpty && key != kTombstone);
    if (slots_.empty()) return false;
    const std::size_t mask = slots_.size() - 1;
    std::size_t i = static_cast<std::size_t>(mix64(key)) & mask;
    for (;;) {
      Slot& s = slots_[i];
      if (s.key == key) {
        s.key = kTombstone;
        --size_;
        return true;
      }
      if (s.key == kEmpty) return false;
      i = (i + 1) & mask;
    }
  }

  void reserve(std::size_t n) {
    // Keep the load factor at or below 3/4 after inserting `n` keys.
    std::size_t cap = 16;
    while (cap * 3 < n * 4) cap <<= 1;
    if (cap > slots_.size()) rehash(cap);
  }

  void clear() noexcept {
    for (Slot& s : slots_) s = Slot{};
    size_ = 0;
    used_ = 0;
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

  /// Visits every (key, value) pair. Order is unspecified.
  template <class F>
  void for_each(F&& f) const {
    for (const Slot& s : slots_)
      if (s.key != kEmpty && s.key != kTombstone) f(s.key, s.value);
  }

 private:
  struct Slot {
    std::uint64_t key = kEmpty;
    std::uint32_t value = 0;
    std::uint32_t pad_ = 0;
  };

  bool emplace(std::uint64_t key, std::uint32_t value, bool assign) {
    assert(key != kEmpty && key != kTombstone);
    grow_if_needed();
    const std::size_t mask = slots_.size() - 1;
    std::size_t i = static_cast<std::size_t>(mix64(key)) & mask;
    std::size_t tomb = static_cast<std::size_t>(-1);
    for (;;) {
      Slot& s = slots_[i];
      if (s.key == key) {
        if (assign) s.value = value;
        return false;
      }
      if (s.key == kTombstone) {
        if (tomb == static_cast<std::size_t>(-1)) tomb = i;
      } else if (s.key == kEmpty) {
        Slot& dst = tomb != static_cast<std::size_t>(-1) ? slots_[tomb] : s;
        if (tomb == static_cast<std::size_t>(-1)) ++used_;
        dst.key = key;
        dst.value = value;
        ++size_;
        return true;
      }
      i = (i + 1) & mask;
    }
  }

  void grow_if_needed() {
    if (slots_.empty()) {
      rehash(16);
      return;
    }
    // `used_` counts live slots plus tombstones; both consume probe distance.
    if ((used_ + 1) * 4 > slots_.size() * 3) {
      // If most of the pressure is tombstones, rehash in place at the same size.
      const std::size_t new_cap = (size_ + 1) * 2 > slots_.size() ? slots_.size() * 2 : slots_.size();
      rehash(new_cap);
    }
  }

  void rehash(std::size_t new_cap) {
    std::vector<Slot> old = std::move(slots_);
    slots_.assign(new_cap, Slot{});
    size_ = 0;
    used_ = 0;
    const std::size_t mask = new_cap - 1;
    for (const Slot& s : old) {
      if (s.key == kEmpty || s.key == kTombstone) continue;
      std::size_t i = static_cast<std::size_t>(mix64(s.key)) & mask;
      while (slots_[i].key != kEmpty) i = (i + 1) & mask;
      slots_[i] = s;
      ++size_;
      ++used_;
    }
  }

  std::vector<Slot> slots_;
  std::size_t size_ = 0;  // live entries
  std::size_t used_ = 0;  // live entries + tombstones
};

}  // namespace wsld
