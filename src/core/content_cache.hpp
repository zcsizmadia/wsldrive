#pragma once

#include "core/error.hpp"
#include "core/hash.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace wsld {

/// Content-addressed on-disk cache of file contents keyed by BLAKE3 digest.
///
/// Layout: `<root>/objects/<hh>/<hex digest>` plus `<root>/tmp` for in-flight
/// writes. Objects are written to a temporary file and renamed into place so a
/// crash never leaves a partial object visible. The index (digest -> size,
/// last-use) lives in memory and is rebuilt by scanning on open.
///
/// Not thread-safe; callers serialise access.
class ContentCache {
 public:
  struct Options {
    std::filesystem::path root;
    std::uint64_t max_bytes = std::uint64_t{4} << 30;  // soft cap enforced by `evict`
  };

  struct Stats {
    std::size_t objects;
    std::uint64_t bytes;
    std::uint64_t hits;
    std::uint64_t misses;
  };

  [[nodiscard]] static Result<ContentCache> open(Options opts);

  ContentCache(ContentCache&&) noexcept = default;
  ContentCache& operator=(ContentCache&&) noexcept = default;

  /// Stores `data`, returning its digest. Storing existing content is a cheap no-op.
  [[nodiscard]] Result<Digest> put(std::span<const std::byte> data);
  [[nodiscard]] bool contains(const Digest& d) const noexcept { return index_.contains(d); }
  /// Reads an object fully into memory and marks it recently used.
  [[nodiscard]] Result<std::vector<std::byte>> get(const Digest& d);
  /// Path of a stored object, for callers that want to map or stream it.
  [[nodiscard]] std::optional<std::filesystem::path> path_for(const Digest& d) const;
  [[nodiscard]] Result<void> erase(const Digest& d);

  /// Removes least-recently-used objects until total bytes <= target.
  void evict_to(std::uint64_t target_bytes);
  /// Enforces `Options::max_bytes`.
  void evict() { evict_to(opts_.max_bytes); }

  [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::size_t count() const noexcept { return index_.size(); }
  [[nodiscard]] Stats stats() const noexcept { return Stats{index_.size(), bytes_, hits_, misses_}; }
  [[nodiscard]] const std::filesystem::path& root() const noexcept { return opts_.root; }

 private:
  struct Entry {
    std::uint64_t size;
    std::uint64_t last_use;
  };

  explicit ContentCache(Options opts) : opts_(std::move(opts)) {}
  [[nodiscard]] std::filesystem::path object_path(const Digest& d) const;
  Result<void> scan();

  Options opts_;
  std::unordered_map<Digest, Entry, DigestHash> index_;
  std::uint64_t bytes_ = 0;
  std::uint64_t tick_ = 0;
  std::uint64_t hits_ = 0;
  std::uint64_t misses_ = 0;
  std::uint64_t tmp_counter_ = 0;
};

}  // namespace wsld
