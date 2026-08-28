#include "mount/fuse_mount.hpp"

#include "core/metadata_tree.hpp"
#include "core/path.hpp"

// One implementation over the FUSE3 high-level API: WinFsp-FUSE on Windows
// (Direction A) and libfuse3 on Linux (Direction B). The API is source-
// compatible apart from a few types (fuse_stat vs stat) aliased below and the
// Windows-only DLL load and filename escaping.
#ifdef _WIN32
#include <fuse3/fuse.h>  // FUSE_USE_VERSION comes from the winfsp::fuse3 target
#include <windows.h>
#include "core/win_names.hpp"
using StatT = struct fuse_stat;
using StatvfsT = struct fuse_statvfs;
using OffT = fuse_off_t;
using ModeT = fuse_mode_t;
#ifndef S_IFLNK
#define S_IFLNK 0120000
#endif
#else
#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
using StatT = struct stat;
using StatvfsT = struct statvfs;
using OffT = off_t;
using ModeT = mode_t;
#endif

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>

namespace wsld::mount {

namespace {

struct Context {
  agent::RemoteRoot* root;
};

Context* ctx() { return static_cast<Context*>(fuse_get_context()->private_data); }

#ifdef _WIN32
// winfsp-x64.dll is delay-loaded; load it (default search path, then the
// registry InstallDir) so the binary runs without WinFsp's bin dir on PATH.
bool load_winfsp_dll() {
  if (::LoadLibraryW(L"winfsp-x64.dll") != nullptr) return true;
  HKEY key{};
  if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\WinFsp", 0, KEY_READ | KEY_WOW64_32KEY, &key) !=
      ERROR_SUCCESS)
    return false;
  wchar_t dir[MAX_PATH]{};
  DWORD size = sizeof(dir);
  const LSTATUS rc = ::RegQueryValueExW(key, L"InstallDir", nullptr, nullptr, reinterpret_cast<LPBYTE>(dir), &size);
  ::RegCloseKey(key);
  if (rc != ERROR_SUCCESS) return false;
  std::wstring path = dir;
  if (!path.empty() && path.back() != L'\\') path.push_back(L'\\');
  path += L"bin\\winfsp-x64.dll";
  return ::LoadLibraryW(path.c_str()) != nullptr;
}
#endif

// Name shown in a directory listing: on Windows, escape characters illegal in
// NTFS names; on Linux the raw name is already valid.
std::string present_name(std::string_view raw) {
#ifdef _WIN32
  return escape_for_windows(raw);
#else
  return std::string(raw);
#endif
}

// FUSE paths are absolute ("/", "/a/b"); the tree uses relative '/'-separated
// keys. On Windows, decode the escaped form back to the raw name.
std::string to_rel(const char* path) {
  if (path == nullptr) return {};
  while (*path == '/') ++path;
#ifdef _WIN32
  return unescape_from_windows(path);
#else
  return std::string(path);
#endif
}

void fill_stat(const MetadataTree::Node& n, StatT* st) {
  std::memset(st, 0, sizeof(*st));
  if (n.attr.kind == NodeKind::Directory) {
    st->st_mode = static_cast<ModeT>(S_IFDIR | 0755);
    st->st_nlink = 2;
  } else if (n.attr.kind == NodeKind::Symlink) {
    st->st_mode = static_cast<ModeT>(S_IFLNK | 0777);
    st->st_nlink = 1;
    st->st_size = static_cast<OffT>(n.attr.size);
  } else {
    st->st_mode = static_cast<ModeT>(S_IFREG | 0644);
    st->st_nlink = 1;
    st->st_size = static_cast<OffT>(n.attr.size);
  }
  st->st_mtim.tv_sec = static_cast<decltype(st->st_mtim.tv_sec)>(n.attr.mtime_ns / 1'000'000'000);
  st->st_mtim.tv_nsec = static_cast<decltype(st->st_mtim.tv_nsec)>(n.attr.mtime_ns % 1'000'000'000);
  st->st_atim = st->st_mtim;
  st->st_ctim = st->st_mtim;
}

int op_getattr(const char* path, StatT* st, struct fuse_file_info*) {
  const std::string rel = to_rel(path);
  return ctx()->root->with_tree([&](const MetadataTree& t) -> int {
    const auto id = t.lookup(rel, LookupMode::CaseInsensitive);
    if (!id) return -ENOENT;
    fill_stat(t.node(*id), st);
    return 0;
  });
}

int op_readdir(const char* path, void* buf, fuse_fill_dir_t filler, OffT, struct fuse_file_info*,
               enum fuse_readdir_flags) {
  const std::string rel = to_rel(path);
  return ctx()->root->with_tree([&](const MetadataTree& t) -> int {
    const auto id = t.lookup(rel, LookupMode::CaseInsensitive);
    if (!id) return -ENOENT;
    if (!t.node(*id).is_dir()) return -ENOTDIR;
    filler(buf, ".", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
    filler(buf, "..", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
    int rc = 0;
    t.for_each_child(*id, [&](NodeId c) {
      if (rc != 0) return;
      const std::string name = present_name(t.name(c));
      StatT st;
      fill_stat(t.node(c), &st);
      rc = filler(buf, name.c_str(), &st, 0, static_cast<fuse_fill_dir_flags>(0));
    });
    return 0;
  });
}

int op_open(const char* path, struct fuse_file_info*) {
  const std::string rel = to_rel(path);
  return ctx()->root->with_tree([&](const MetadataTree& t) -> int {
    const auto id = t.lookup(rel, LookupMode::CaseInsensitive);
    if (!id) return -ENOENT;
    if (t.node(*id).is_dir()) return -EISDIR;
    return 0;
  });
}

int err_to_errno(Errc e) {
  switch (e) {
    case Errc::NotFound: return -ENOENT;
    case Errc::AlreadyExists: return -EEXIST;
    case Errc::NotADirectory: return -ENOTDIR;
    case Errc::IsADirectory: return -EISDIR;
    case Errc::InvalidPath:
    case Errc::InvalidArgument: return -EINVAL;
    case Errc::Timeout: return -ETIMEDOUT;
    case Errc::ConnectionClosed: return -EIO;
    default: return -EIO;
  }
}

int op_create(const char* path, ModeT mode, struct fuse_file_info*) {
  auto r = ctx()->root->create_file(to_rel(path), static_cast<std::uint32_t>(mode) & 0777u);
  return r ? 0 : err_to_errno(r.error());
}

int op_write(const char* path, const char* buf, size_t size, OffT offset, struct fuse_file_info*) {
  auto r = ctx()->root->write(to_rel(path), static_cast<std::uint64_t>(offset),
                              std::as_bytes(std::span<const char>(buf, size)));
  if (!r) return err_to_errno(r.error());
  return static_cast<int>(*r);
}

int op_truncate(const char* path, OffT size, struct fuse_file_info*) {
  auto r = ctx()->root->truncate(to_rel(path), static_cast<std::uint64_t>(size));
  return r ? 0 : err_to_errno(r.error());
}

int op_mkdir(const char* path, ModeT mode) {
  auto r = ctx()->root->mkdir(to_rel(path), static_cast<std::uint32_t>(mode) & 0777u);
  return r ? 0 : err_to_errno(r.error());
}

int op_unlink(const char* path) {
  auto r = ctx()->root->unlink(to_rel(path));
  return r ? 0 : err_to_errno(r.error());
}

int op_rmdir(const char* path) {
  auto r = ctx()->root->rmdir(to_rel(path));
  return r ? 0 : err_to_errno(r.error());
}

int op_rename(const char* from, const char* to, unsigned int) {
  auto r = ctx()->root->rename(to_rel(from), to_rel(to));
  return r ? 0 : err_to_errno(r.error());
}

int op_read(const char* path, char* buf, size_t size, OffT offset, struct fuse_file_info*) {
  const std::string rel = to_rel(path);
  const std::uint32_t want = static_cast<std::uint32_t>(std::min<size_t>(size, 16u << 20));
  auto data = ctx()->root->read(rel, static_cast<std::uint64_t>(offset), want);
  if (!data) return err_to_errno(data.error());
  if (!data->empty()) std::memcpy(buf, data->data(), data->size());
  return static_cast<int>(data->size());
}

int op_statfs(const char*, StatvfsT* st) {
  // WinFsp refuses writes ("device error") without free space reported; advertise
  // a large backing store (the real limit is the served volume).
  std::memset(st, 0, sizeof(*st));
  st->f_bsize = 4096;
  st->f_frsize = 4096;
  st->f_blocks = std::uint64_t{1} << 32;  // ~16 TiB
  st->f_bfree = std::uint64_t{1} << 31;
  st->f_bavail = std::uint64_t{1} << 31;
  st->f_files = std::uint64_t{1} << 20;
  st->f_ffree = std::uint64_t{1} << 19;
  st->f_namemax = 255;
  return 0;
}

int op_fsync(const char*, int, struct fuse_file_info*) { return 0; }

void* op_init(struct fuse_conn_info*, struct fuse_config* cfg) {
  cfg->kernel_cache = 0;
  cfg->entry_timeout = 1.0;
  cfg->attr_timeout = 1.0;
  cfg->negative_timeout = 1.0;
  return fuse_get_context()->private_data;
}

fuse_operations make_ops() {
  fuse_operations ops{};
  ops.init = op_init;
  ops.getattr = op_getattr;
  ops.readdir = op_readdir;
  ops.open = op_open;
  ops.read = op_read;
  ops.create = op_create;
  ops.write = op_write;
  ops.truncate = op_truncate;
  ops.mkdir = op_mkdir;
  ops.unlink = op_unlink;
  ops.rmdir = op_rmdir;
  ops.rename = op_rename;
  ops.statfs = op_statfs;
  ops.fsync = op_fsync;
  return ops;
}

}  // namespace

FuseMount::~FuseMount() { unmount(); }

Result<void> FuseMount::mount(const std::string& mountpoint) {
#ifdef _WIN32
  if (!load_winfsp_dll()) return fail(Errc::Unsupported);
#endif

  static Context context;  // FUSE keeps a single mount per process here
  context.root = &root_;
  static fuse_operations ops = make_ops();

  struct fuse_args args = FUSE_ARGS_INIT(0, nullptr);
  fuse_opt_add_arg(&args, "wsldrive");
#ifdef _WIN32
  // uid/gid = -1 maps ownership to the caller; rellinks keeps symlinks relative.
  fuse_opt_add_arg(&args, "-ouid=-1,gid=-1,rellinks,FileSystemName=wsldrive,volname=wsldrive");
#endif
  if (std::getenv("WSLDRIVE_FUSE_DEBUG") != nullptr) fuse_opt_add_arg(&args, "-d");

  struct fuse* f = fuse_new(&args, &ops, sizeof(ops), &context);
  fuse_opt_free_args(&args);
  if (f == nullptr) return fail(Errc::IoError);

  if (fuse_mount(f, mountpoint.c_str()) != 0) {
    fuse_destroy(f);
    return fail(Errc::IoError);
  }
  fuse_ = f;
  mountpoint_ = mountpoint;
  mounted_.store(true);
  loop_ = std::thread([this] {
    fuse_loop(static_cast<struct fuse*>(fuse_));
    mounted_.store(false);
  });
  return {};
}

void FuseMount::unmount() {
  if (fuse_ == nullptr) return;
  auto* f = static_cast<struct fuse*>(fuse_);
  fuse_exit(f);
  fuse_unmount(f);
  if (loop_.joinable()) loop_.join();
  fuse_destroy(f);
  fuse_ = nullptr;
  mounted_.store(false);
}

}  // namespace wsld::mount
