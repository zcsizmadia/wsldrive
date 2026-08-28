#pragma once

#include "core/hash_util.hpp"
#include "core/types.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wsld {

/// Append-only string interner. Every distinct string gets a dense NameId and
/// stable storage, so nodes store 4-byte ids and lookups by name compare ids.
///
/// Interning also gives negative lookups for free: a name that has never been
/// interned cannot be the name of any node, so `find` failing is a definitive
/// "no such entry" without touching the tree.
class StringPool {
 public:
  StringPool();
  StringPool(const StringPool&) = delete;
  StringPool& operator=(const StringPool&) = delete;
  StringPool(StringPool&&) noexcept = default;
  StringPool& operator=(StringPool&&) noexcept = default;

  /// Returns the id for `s`, interning it if necessary.
  NameId intern(std::string_view s);

  /// Returns the id for `s` if it has been interned.
  [[nodiscard]] std::optional<NameId> find(std::string_view s) const noexcept {
    const auto it = index_.find(s);
    if (it == index_.end()) return std::nullopt;
    return it->second;
  }

  [[nodiscard]] std::string_view view(NameId id) const noexcept {
    const Entry& e = entries_[id];
    return {e.data, e.len};
  }

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }

  void clear();

 private:
  struct Entry {
    const char* data;
    std::uint32_t len;
  };

  const char* store(std::string_view s);

  static constexpr std::size_t kChunkSize = std::size_t{1} << 20;

  std::vector<Entry> entries_;
  std::vector<std::unique_ptr<char[]>> chunks_;
  std::vector<std::unique_ptr<char[]>> large_;
  std::size_t chunk_used_ = 0;
  std::size_t chunk_cap_ = 0;
  std::size_t bytes_ = 0;
  std::unordered_map<std::string_view, NameId, StringHash, std::equal_to<>> index_;
};

}  // namespace wsld
