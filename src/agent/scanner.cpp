#include "agent/scanner.hpp"

#include <chrono>
#include <deque>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include "platform/win/wide.hpp"

#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace wsld::agent {

namespace fs = std::filesystem;

namespace {

std::int64_t to_unix_ns(fs::file_time_type t) noexcept {
  using namespace std::chrono;
  // Two spellings of the same conversion: MSVC only has clock_cast; libstdc++
  // has file_clock::to_sys since GCC 11 but clock_cast only since GCC 13, and
  // the release binaries are built with GCC 12 (for Ubuntu 22.04).
#ifdef _MSC_VER
  const auto sys = clock_cast<system_clock>(t);
#else
  const auto sys = file_clock::to_sys(t);
#endif
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

std::optional<std::uint64_t> device_id(const fs::path& p) noexcept {
#ifdef _WIN32
  // The volume serial identifies the filesystem; a directory handle needs
  // FILE_FLAG_BACKUP_SEMANTICS. Reparse points are followed on purpose: a
  // junction to another volume reports that volume, which is what we compare.
  const HANDLE h = ::CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE) return std::nullopt;
  BY_HANDLE_FILE_INFORMATION info{};
  const BOOL ok = ::GetFileInformationByHandle(h, &info);
  ::CloseHandle(h);
  if (!ok) return std::nullopt;
  return static_cast<std::uint64_t>(info.dwVolumeSerialNumber);
#else
  struct ::stat st {};
  if (::stat(p.c_str(), &st) != 0) return std::nullopt;
  return static_cast<std::uint64_t>(st.st_dev);
#endif
}

Result<ScanStats> scan_tree(const fs::path& root, const std::function<void(const SnapshotEntry&)>& on_entry,
                            const SkipPredicate& skip, bool one_file_system) {
  std::error_code ec;
  if (!fs::is_directory(root, ec)) return fail(Errc::NotADirectory);

  // Filesystem the root lives on; directories on any other one are reported but
  // not descended into. Unknown (nullopt) disables the check rather than
  // silently pruning everything.
  const std::optional<std::uint64_t> root_dev = one_file_system ? device_id(root) : std::nullopt;

  ScanStats stats;
  struct Pending {
    fs::path path;
    std::string rel;  // '/'-separated path relative to root ("" for the root)
    std::uint32_t index;
  };
  std::deque<Pending> queue;
  queue.push_back(Pending{root, std::string{}, 0});
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
      std::string rel = dir.rel.empty() ? name : dir.rel + "/" + name;
      if (skip && skip(rel, a.kind == NodeKind::Directory)) {
        ++stats.skipped;
        continue;
      }
      on_entry(SnapshotEntry{dir.index, name, a});
      switch (a.kind) {
        case NodeKind::Directory: {
          ++stats.directories;
          // A directory on another filesystem (a mount point) is reported as an
          // empty directory rather than traversed — see one_file_system.
          bool descend = true;
          if (root_dev) {
            const auto dev = device_id(e.path());
            if (dev && *dev != *root_dev) {
              descend = false;
              ++stats.skipped;
            }
          }
          if (descend) queue.push_back(Pending{e.path(), std::move(rel), next_index});
          break;
        }
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
