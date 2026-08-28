#include "core/content_cache.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace wsld {

namespace fs = std::filesystem;

namespace {

struct FileCloser {
  void operator()(std::FILE* f) const noexcept {
    if (f != nullptr) std::fclose(f);
  }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

FilePtr open_file(const fs::path& p, const char* mode) noexcept {
#ifdef _WIN32
  std::wstring wmode(mode, mode + std::char_traits<char>::length(mode));
  return FilePtr{_wfopen(p.c_str(), wmode.c_str())};
#else
  return FilePtr{std::fopen(p.c_str(), mode)};
#endif
}

}  // namespace

Result<ContentCache> ContentCache::open(Options opts) {
  std::error_code ec;
  fs::create_directories(opts.root / "objects", ec);
  if (ec) return fail(Errc::IoError);
  fs::create_directories(opts.root / "tmp", ec);
  if (ec) return fail(Errc::IoError);
  ContentCache cache(std::move(opts));
  if (auto r = cache.scan(); !r) return fail(r.error());
  return cache;
}

fs::path ContentCache::object_path(const Digest& d) const {
  const std::string hex = d.hex();
  return opts_.root / "objects" / hex.substr(0, 2) / hex;
}

Result<void> ContentCache::scan() {
  std::error_code ec;
  index_.clear();
  bytes_ = 0;
  for (const fs::directory_entry& bucket : fs::directory_iterator(opts_.root / "objects", ec)) {
    if (ec) return fail(Errc::IoError);
    if (!bucket.is_directory()) continue;
    for (const fs::directory_entry& obj : fs::directory_iterator(bucket.path(), ec)) {
      if (ec) return fail(Errc::IoError);
      const auto digest = Digest::from_hex(obj.path().filename().string());
      if (!digest || !obj.is_regular_file()) continue;
      const std::uint64_t size = obj.file_size(ec);
      if (ec) continue;
      index_.emplace(*digest, Entry{size, tick_++});
      bytes_ += size;
    }
  }
  // Leftover temporaries from an interrupted run are garbage.
  for (const fs::directory_entry& tmp : fs::directory_iterator(opts_.root / "tmp", ec)) fs::remove(tmp.path(), ec);
  return {};
}

Result<Digest> ContentCache::put(std::span<const std::byte> data) {
  const Digest d = blake3_hash(data);
  if (auto it = index_.find(d); it != index_.end()) {
    it->second.last_use = tick_++;
    return d;
  }

  const fs::path final_path = object_path(d);
  const fs::path tmp_path = opts_.root / "tmp" / (d.hex().substr(0, 16) + "." + std::to_string(tmp_counter_++));
  std::error_code ec;
  fs::create_directories(final_path.parent_path(), ec);
  if (ec) return fail(Errc::IoError);

  {
    FilePtr f = open_file(tmp_path, "wb");
    if (!f) return fail(Errc::IoError);
    if (!data.empty() && std::fwrite(data.data(), 1, data.size(), f.get()) != data.size()) {
      f.reset();
      fs::remove(tmp_path, ec);
      return fail(Errc::IoError);
    }
    if (std::fflush(f.get()) != 0) {
      f.reset();
      fs::remove(tmp_path, ec);
      return fail(Errc::IoError);
    }
  }

  fs::rename(tmp_path, final_path, ec);
  if (ec) {
    // Another writer may have raced us; if the object now exists, that is fine.
    fs::remove(tmp_path, ec);
    if (!fs::exists(final_path)) return fail(Errc::IoError);
  }
  index_.emplace(d, Entry{data.size(), tick_++});
  bytes_ += data.size();
  return d;
}

Result<std::vector<std::byte>> ContentCache::get(const Digest& d) {
  auto it = index_.find(d);
  if (it == index_.end()) {
    ++misses_;
    return fail(Errc::NotFound);
  }
  FilePtr f = open_file(object_path(d), "rb");
  if (!f) {
    // The object vanished under us (external cleanup); drop it from the index.
    bytes_ -= it->second.size;
    index_.erase(it);
    ++misses_;
    return fail(Errc::NotFound);
  }
  std::vector<std::byte> buf(static_cast<std::size_t>(it->second.size));
  if (!buf.empty() && std::fread(buf.data(), 1, buf.size(), f.get()) != buf.size()) return fail(Errc::IoError);
  it->second.last_use = tick_++;
  ++hits_;
  return buf;
}

std::optional<fs::path> ContentCache::path_for(const Digest& d) const {
  if (!index_.contains(d)) return std::nullopt;
  return object_path(d);
}

Result<void> ContentCache::erase(const Digest& d) {
  auto it = index_.find(d);
  if (it == index_.end()) return fail(Errc::NotFound);
  std::error_code ec;
  fs::remove(object_path(d), ec);
  bytes_ -= it->second.size;
  index_.erase(it);
  if (ec) return fail(Errc::IoError);
  return {};
}

void ContentCache::evict_to(std::uint64_t target_bytes) {
  if (bytes_ <= target_bytes) return;
  std::vector<std::pair<std::uint64_t, Digest>> by_age;
  by_age.reserve(index_.size());
  for (const auto& [d, e] : index_) by_age.emplace_back(e.last_use, d);
  std::sort(by_age.begin(), by_age.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& [age, d] : by_age) {
    if (bytes_ <= target_bytes) break;
    (void)erase(d);
  }
}

}  // namespace wsld
