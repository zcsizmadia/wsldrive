#include "core/string_pool.hpp"

#include <algorithm>
#include <cstring>

namespace wsld {

StringPool::StringPool() {
  entries_.reserve(1024);
  index_.reserve(1024);
}

NameId StringPool::intern(std::string_view s) {
  if (const auto it = index_.find(s); it != index_.end()) return it->second;
  const char* data = store(s);
  const auto id = static_cast<NameId>(entries_.size());
  entries_.push_back(Entry{data, static_cast<std::uint32_t>(s.size())});
  index_.emplace(std::string_view{data, s.size()}, id);
  bytes_ += s.size();
  return id;
}

const char* StringPool::store(std::string_view s) {
  if (s.empty()) {
    // Zero-length names (the root) need a valid, unique pointer but no storage.
    static constexpr char kEmpty = '\0';
    return &kEmpty;
  }
  if (s.size() > kChunkSize / 4) {
    // Oversized names get a dedicated allocation; the current chunk stays in use.
    auto chunk = std::make_unique<char[]>(s.size());
    std::memcpy(chunk.get(), s.data(), s.size());
    return large_.emplace_back(std::move(chunk)).get();
  }
  if (chunk_used_ + s.size() > chunk_cap_) {
    chunks_.push_back(std::make_unique<char[]>(kChunkSize));
    chunk_used_ = 0;
    chunk_cap_ = kChunkSize;
  }
  char* dst = chunks_.back().get() + chunk_used_;
  std::memcpy(dst, s.data(), s.size());
  chunk_used_ += s.size();
  return dst;
}

void StringPool::clear() {
  index_.clear();
  entries_.clear();
  chunks_.clear();
  large_.clear();
  chunk_used_ = 0;
  chunk_cap_ = 0;
  bytes_ = 0;
}

}  // namespace wsld
