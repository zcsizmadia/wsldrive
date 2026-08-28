#include "agent/scanner.hpp"

#include <chrono>
#include <deque>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include "platform/win/wide.hpp"
#endif

namespace wsld::agent {

namespace fs = std::filesystem;

namespace {

std::int64_t to_unix_ns(fs::file_time_type t) noexcept {
  using namespace std::chrono;
  const auto sys = clock_cast<system_clock>(t);
  return duration_cast<nanoseconds>(sys.time_since_epoch()).count();
}

Attributes attributes_from(const fs::directory_entry& e, std::error_code& ec) noexcept {
  Attributes a;
  const fs::file_status st = e.symlink_status(ec);
  if (ec) return a;
  switch (st.type()) {
    case fs::file_type::directory: a.kind = NodeKind::Directory; break;
    case fs::file_type::regular: a.kind = NodeKind::File; break;
    case fs::file_type::symlink: a.kind = NodeKind::Symlink; break;
    default: a.kind = NodeKind::Other; break;
  }
  if (a.kind == NodeKind::File) {
    a.size = e.file_size(ec);
    if (ec) {
      a.size = 0;
      ec.clear();
    }
  }
  const auto mtime = e.last_write_time(ec);
  if (!ec) a.mtime_ns = to_unix_ns(mtime);
  ec.clear();

#ifdef _WIN32
  // NTFS has no mode bits: synthesise POSIX-ish ones from the read-only attribute.
  const bool readonly = (st.permissions() & fs::perms::owner_write) == fs::perms::none;
  a.mode = a.kind == NodeKind::Directory ? (readonly ? 0555u : 0755u) : (readonly ? 0444u : 0644u);
#else
  a.mode = static_cast<std::uint32_t>(st.permissions()) & 07777u;
#endif
  return a;
}

}  // namespace

std::string filename_utf8(const fs::path& p) {
#ifdef _WIN32
  return platform::win::to_utf8(p.filename().native());
#else
  return p.filename().native();
#endif
}

fs::path join_relative(const fs::path& root, std::string_view rel) {
#ifdef _WIN32
  std::wstring w = platform::win::to_wide(rel);
  for (wchar_t& c : w)
    if (c == L'/') c = L'\\';
  return root / w;
#else
  return root / fs::path(std::string(rel));
#endif
}

Result<Attributes> read_attributes(const fs::path& p) noexcept {
  std::error_code ec;
  const fs::directory_entry e(p, ec);
  if (ec) return fail(Errc::NotFound);
  Attributes a = attributes_from(e, ec);
  if (ec) return fail(Errc::NotFound);
  return a;
}

Result<ScanStats> scan_tree(const fs::path& root, const std::function<void(const SnapshotEntry&)>& on_entry) {
  std::error_code ec;
  if (!fs::is_directory(root, ec)) return fail(Errc::NotADirectory);

  ScanStats stats;
  struct Pending {
    fs::path path;
    std::uint32_t index;
  };
  std::deque<Pending> queue;
  queue.push_back(Pending{root, 0});
  std::uint32_t next_index = 1;
  std::string name;

  while (!queue.empty()) {
    Pending dir = std::move(queue.front());
    queue.pop_front();
    fs::directory_iterator it(dir.path, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
      ++stats.skipped;
      ec.clear();
      continue;
    }
    for (const fs::directory_entry& e : it) {
      Attributes a = attributes_from(e, ec);
      if (ec) {
        ++stats.skipped;
        ec.clear();
        continue;
      }
      if (a.kind == NodeKind::Other) {
        ++stats.skipped;
        continue;
      }
      name = filename_utf8(e.path());
      on_entry(SnapshotEntry{dir.index, name, a});
      switch (a.kind) {
        case NodeKind::Directory:
          ++stats.directories;
          queue.push_back(Pending{e.path(), next_index});
          break;
        case NodeKind::File: ++stats.files; break;
        case NodeKind::Symlink: ++stats.symlinks; break;
        case NodeKind::Other: break;
      }
      ++next_index;
    }
  }
  return stats;
}

}  // namespace wsld::agent
