#include "agent/client.hpp"

#include "core/path.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace wsld::agent {

RemoteRoot::RemoteRoot(std::unique_ptr<net::FrameChannel> ch) : ch_(std::move(ch)) {}

RemoteRoot::~RemoteRoot() { close(); }

void RemoteRoot::close() {
  // `closed_` may already be set by the reader thread when the peer disconnects,
  // so cleanup is guarded by its own once_flag rather than by `closed_` — the
  // destructor must always join the reader, never skip it.
  closed_.store(true);
  std::call_once(cleanup_, [this] {
    {
      std::lock_guard lock(pf_mu_);
      pf_stop_.store(true);
    }
    pf_cv_.notify_all();
    {
      std::lock_guard lock(rs_mu_);
      rs_stop_ = true;
    }
    rs_cv_.notify_all();
    ch_->shutdown();
    ch_->close();
    if (reader_.joinable()) reader_.join();
    fail_all_pending();  // releases any in-flight prefetch read_many or rescan snapshot request
    if (prefetch_.joinable()) prefetch_.join();
    if (rescan_.joinable()) rescan_.join();
  });
}

void RemoteRoot::set_invalidation_hook(InvalidationHook hook) {
  std::lock_guard lock(stats_mu_);
  hook_ = std::move(hook);
}

RemoteRoot::Stats RemoteRoot::stats() const {
  std::lock_guard lock(stats_mu_);
  return stats_;
}

Result<proto::Hello> RemoteRoot::connect(std::chrono::milliseconds timeout) {
  if (!reader_.joinable()) reader_ = std::thread([this] { reader_loop(); });
  if (!prefetch_.joinable()) prefetch_ = std::thread([this] { prefetch_loop(); });
  if (!rescan_.joinable()) rescan_ = std::thread([this] { rescan_loop(); });
  std::vector<std::byte> payload;
  proto::Writer w(payload);
  proto::write_hello(
      w, proto::Hello{
             .protocol_version = proto::kVersion, .capabilities = 0, .agent = "wsldrive/0.1", .token = token_});
  auto p = request(proto::MsgType::Hello, payload, timeout);
  if (!p) return fail(p.error());
  // An Error reply (e.g. a rejected token) is already decoded by request().
  if ((*p)->type != proto::MsgType::HelloAck) return fail(Errc::ProtocolError);
  proto::Reader r((*p)->payload);
  auto ack = proto::read_hello(r);
  if (!ack) return fail(ack.error());
  proto::Hello out = *ack;
  out.agent = {};  // the view would dangle once `p` dies; callers only need version/caps
  return out;
}

Result<void> RemoteRoot::fetch_snapshot(std::chrono::milliseconds timeout) {
  const auto t0 = std::chrono::steady_clock::now();
  {
    // From here until the tree is replaced, apply_invalidation() also records
    // every batch so the ones the snapshot does not include can be replayed.
    std::unique_lock lock(tree_mu_);
    snapshot_in_flight_ = true;
    snapshot_replay_.clear();
  }
  // Whatever happens below, the recording stops when this call ends.
  struct EndRecording {
    RemoteRoot& self;
    ~EndRecording() {
      std::unique_lock lock(self.tree_mu_);
      self.snapshot_in_flight_ = false;
      self.snapshot_replay_.clear();
    }
  } end_recording{*this};

  auto p = request(proto::MsgType::SnapshotRequest, {}, timeout, /*streaming=*/true);
  if (!p) return fail(p.error());
  if ((*p)->type != proto::MsgType::Snapshot) return fail(Errc::ProtocolError);
  if (!(*p)->stream_ok) return fail(Errc::Corrupt);

  // Names were copied per frame; point each entry at its owned string now that
  // the accumulation vector will not grow again.
  auto& names = (*p)->snap_names;
  auto& entries = (*p)->snap_entries;
  for (std::size_t i = 0; i < entries.size(); ++i) entries[i].name = names[i];
  const std::uint64_t snap_gen = (*p)->snap_generation;
  {
    std::unique_lock lock(tree_mu_);
    if (auto res = tree_.load_snapshot(entries); !res) return res;
    // The agent stamped the snapshot with its generation before scanning, so a
    // batch with a higher generation describes a change the scan may have
    // missed; one at or below it is already in the snapshot.
    for (const proto::InvalidationBatch& b : snapshot_replay_) {
      if (b.generation <= snap_gen) continue;
      for (const proto::InvalidationOp& op : b.ops) {
        if (op.kind == InvalidationKind::Upsert) (void)tree_.upsert_path(op.path, op.attr);
        else if (op.kind == InvalidationKind::Remove) (void)tree_.remove_path(op.path);
      }
    }
    snapshot_replay_.clear();
    snapshot_in_flight_ = false;
  }
  std::lock_guard lock(stats_mu_);
  stats_.generation = (*p)->snap_generation;
  stats_.snapshot_bytes = (*p)->snap_bytes;
  stats_.last_snapshot_time = std::chrono::steady_clock::now() - t0;
  return {};
}

Result<std::vector<std::byte>> RemoteRoot::read_remote(std::string_view path, std::uint64_t offset,
                                                       std::uint32_t length, std::chrono::milliseconds timeout) {
  std::vector<std::byte> payload;
  proto::Writer w(payload);
  proto::write_read_request(w, proto::ReadRequest{.path = path, .offset = offset, .length = length});
  auto p = request(proto::MsgType::ReadRequest, payload, timeout);
  if (!p) return fail(p.error());
  if ((*p)->type != proto::MsgType::ReadResponse) return fail(Errc::ProtocolError);
  proto::Reader r((*p)->payload);
  auto resp = proto::read_read_response(r);
  if (!resp) return fail(resp.error());
  return std::vector<std::byte>(resp->data.begin(), resp->data.end());
}

namespace {
std::vector<std::byte> slice(const std::vector<std::byte>& data, std::uint64_t offset, std::uint32_t length) {
  if (offset >= data.size()) return {};
  const auto end = std::min<std::uint64_t>(offset + length, data.size());
  return std::vector<std::byte>(data.begin() + static_cast<std::ptrdiff_t>(offset),
                                data.begin() + static_cast<std::ptrdiff_t>(end));
}
}  // namespace

Result<std::size_t> RemoteRoot::read_into(std::string_view path, std::uint64_t offset, std::span<std::byte> out,
                                          std::chrono::milliseconds timeout) {
  // Fast path: a cached file is copied straight into the caller's buffer. A
  // mount reads a file in chunks, so avoiding the intermediate vector saves one
  // allocation and one copy per chunk.
  std::int64_t mtime = 0;
  std::uint64_t size = 0;
  bool is_file = false;
  {
    std::shared_lock lock(tree_mu_);
    if (const auto id = tree_.lookup(path, LookupMode::Exact)) {
      const auto& n = tree_.node(*id);
      is_file = n.attr.kind == NodeKind::File;
      mtime = n.attr.mtime_ns;
      size = n.attr.size;
    }
  }
  if (is_file && size <= kMaxCacheableFile) {
    std::shared_ptr<const std::vector<std::byte>> hit;
    {
      std::lock_guard lock(rcache_mu_);
      if (const auto it = rcache_.find(path);
          it != rcache_.end() && it->second.mtime_ns == mtime && it->second.size == size) {
        lru_.splice(lru_.begin(), lru_, it->second.lru);
        hit = it->second.data;
      }
    }
    if (hit) {
      const std::size_t n = offset >= hit->size()
                                ? 0
                                : std::min<std::size_t>(out.size(), hit->size() - static_cast<std::size_t>(offset));
      if (n > 0) std::memcpy(out.data(), hit->data() + offset, n);
      std::lock_guard s(stats_mu_);
      ++stats_.read_cache_hits;
      return n;
    }
  }
  // Miss (or an uncacheable file): reuse the read path, then hand the bytes over.
  auto data = read(path, offset, static_cast<std::uint32_t>(std::min<std::size_t>(out.size(), 0xFFFFFFFFu)), timeout);
  if (!data) return fail(data.error());
  const std::size_t n = std::min(out.size(), data->size());
  if (n > 0) std::memcpy(out.data(), data->data(), n);
  return n;
}

Result<std::vector<std::byte>> RemoteRoot::read(std::string_view path, std::uint64_t offset, std::uint32_t length,
                                                std::chrono::milliseconds timeout) {
  // Version and cacheability come from the in-RAM mirror.
  std::int64_t mtime = 0;
  std::uint64_t size = 0;
  bool is_file = false;
  {
    std::shared_lock lock(tree_mu_);
    if (const auto id = tree_.lookup(path, LookupMode::Exact)) {
      const auto& n = tree_.node(*id);
      is_file = n.attr.kind == NodeKind::File;
      mtime = n.attr.mtime_ns;
      size = n.attr.size;
    }
  }
  if (!is_file || size > kMaxCacheableFile) return read_remote(path, offset, length, timeout);

  {
    // Take a reference to the buffer under the lock, then copy the requested
    // range after releasing it: the copy is the long part, and holding the cache
    // mutex across it serialised every concurrent reader of cached files.
    std::shared_ptr<const std::vector<std::byte>> hit;
    {
      std::lock_guard lock(rcache_mu_);
      if (const auto it = rcache_.find(path);
          it != rcache_.end() && it->second.mtime_ns == mtime && it->second.size == size) {
        lru_.splice(lru_.begin(), lru_, it->second.lru);  // most-recently-used
        hit = it->second.data;
      }
    }
    if (hit) {
      auto out = slice(*hit, offset, length);
      std::lock_guard s(stats_mu_);
      ++stats_.read_cache_hits;
      return out;
    }
  }

  // Miss: synchronous read-ahead. Fetch the target plus its uncached small
  // siblings in one bulk round-trip so a sequential reader over a directory pays
  // one request per directory, not one per file. The async prefetcher then picks
  // up any siblings beyond this batch.
  const std::string target(path);
  const std::string dir(split_parent(path).first);
  std::vector<std::string> batch{target};
  std::vector<std::pair<std::int64_t, std::uint64_t>> meta{{mtime, size}};
  std::uint64_t bytes = size;
  {
    std::shared_lock lock(tree_mu_);
    if (const auto did = tree_.lookup(dir, LookupMode::Exact); did && tree_.node(*did).is_dir()) {
      tree_.for_each_child(*did, [&](NodeId c) {
        if (batch.size() >= 256 || bytes >= (2u << 20)) return;
        const auto& n = tree_.node(c);
        if (n.attr.kind != NodeKind::File || n.attr.size == 0 || n.attr.size > kMaxCacheableFile) return;
        std::string p = dir.empty() ? std::string(tree_.name(c)) : dir + "/" + std::string(tree_.name(c));
        if (p == target) return;
        batch.push_back(std::move(p));
        meta.push_back({n.attr.mtime_ns, n.attr.size});
        bytes += n.attr.size;
      });
    }
  }

  std::vector<std::byte> target_bytes;
  bool have_target = false;
  const auto fetch_t0 = std::chrono::steady_clock::now();  // measure the boundary fetch cost
  if (auto res = read_many(batch, timeout); res && (*res)[0].has_value()) {
    for (std::size_t i = 0; i < batch.size(); ++i)
      if ((*res)[i]) {
        if (i == 0) {
          target_bytes = *(*res)[0];
          have_target = true;
        }
        cache_put(batch[i], meta[i].first, meta[i].second, std::move(*(*res)[i]));
      }
  }
  if (!have_target) {  // bulk path unavailable; fetch the target directly
    auto whole = read_remote(path, 0, static_cast<std::uint32_t>(size), timeout);
    if (!whole) return whole;
    target_bytes = *whole;
    cache_put(target, mtime, size, std::move(*whole));
  }
  const auto fetch_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - fetch_t0)
                            .count();
  {
    std::lock_guard s(stats_mu_);
    ++stats_.read_cache_misses;
    stats_.read_miss_fetch_ns += static_cast<std::uint64_t>(fetch_ns);
  }
  if (batch.size() >= 256) enqueue_prefetch(dir);  // large dir: async-fetch the remainder
  return slice(target_bytes, offset, length);
}

void RemoteRoot::evict_locked() {
  // Evict least-recently-used first. The LRU list makes each victim O(1); a scan
  // for the minimum would make a full cache O(n) per insert (O(n^2) while
  // thrashing the cap).
  while (rcache_bytes_ > rcache_cap_ && rcache_.size() > 1) {
    const auto victim = rcache_.find(lru_.back());
    if (victim == rcache_.end()) {  // should not happen; keep the list consistent
      lru_.pop_back();
      continue;
    }
    cache_erase(victim);
  }
}

void RemoteRoot::set_read_cache_limit(std::uint64_t bytes) {
  std::lock_guard lock(rcache_mu_);
  rcache_cap_ = bytes;
  evict_locked();
  std::lock_guard s(stats_mu_);
  stats_.read_cache_bytes = rcache_bytes_;
}

void RemoteRoot::cache_erase(std::unordered_map<std::string, CacheEntry, StringHash, std::equal_to<>>::iterator it) {
  rcache_bytes_ -= it->second.data ? it->second.data->size() : 0;
  lru_.erase(it->second.lru);
  rcache_.erase(it);
}

void RemoteRoot::cache_put(std::string path, std::int64_t mtime_ns, std::uint64_t size, std::vector<std::byte> data) {
  auto buf = std::make_shared<const std::vector<std::byte>>(std::move(data));
  std::lock_guard lock(rcache_mu_);
  if (const auto it = rcache_.find(path); it != rcache_.end()) cache_erase(it);
  rcache_bytes_ += buf->size();
  lru_.push_front(path);
  rcache_.insert_or_assign(std::move(path), CacheEntry{mtime_ns, size, lru_.begin(), std::move(buf)});
  evict_locked();
  std::lock_guard s(stats_mu_);
  stats_.read_cache_bytes = rcache_bytes_;
}

Result<std::vector<std::optional<std::vector<std::byte>>>> RemoteRoot::read_many(const std::vector<std::string>& paths,
                                                                                 std::chrono::milliseconds timeout) {
  std::vector<std::byte> payload;
  proto::Writer w(payload);
  proto::ReadManyRequest req;
  req.paths.reserve(paths.size());
  for (const auto& p : paths) req.paths.emplace_back(p);
  proto::write_read_many_request(w, req);
  auto p = request(proto::MsgType::ReadManyRequest, payload, timeout);
  if (!p) return fail(p.error());
  if ((*p)->type != proto::MsgType::ReadManyResponse) return fail(Errc::ProtocolError);
  proto::Reader r((*p)->payload);
  auto resp = proto::read_read_many_response(r);
  if (!resp) return fail(resp.error());
  std::vector<std::optional<std::vector<std::byte>>> out(paths.size());
  for (std::size_t i = 0; i < resp->items.size() && i < out.size(); ++i)
    if (resp->items[i].ok) out[i] = std::vector<std::byte>(resp->items[i].data.begin(), resp->items[i].data.end());
  return out;
}

void RemoteRoot::enqueue_prefetch(std::string dir) {
  std::lock_guard lock(pf_mu_);
  if (pf_stop_.load()) return;
  if (!pf_seen_.insert(dir).second) return;  // already queued or done
  pf_pending_.fetch_add(1, std::memory_order_relaxed);
  pf_queue_.push_back(std::move(dir));
  pf_cv_.notify_one();
}

void RemoteRoot::prefetch_loop() {
  struct FileMeta {
    std::string path;
    std::int64_t mtime;
    std::uint64_t size;
  };
  for (;;) {
    std::string dir;
    {
      std::unique_lock lock(pf_mu_);
      pf_cv_.wait(lock, [&] { return pf_stop_.load() || !pf_queue_.empty(); });
      if (pf_stop_.load()) return;
      dir = std::move(pf_queue_.front());
      pf_queue_.pop_front();
    }

    // Enumerate this directory's small regular files from the mirror.
    std::vector<FileMeta> files;
    {
      std::shared_lock lock(tree_mu_);
      const auto id = tree_.lookup(dir, LookupMode::Exact);
      if (id && tree_.node(*id).is_dir()) {
        tree_.for_each_child(*id, [&](NodeId c) {
          const auto& n = tree_.node(c);
          if (n.attr.kind == NodeKind::File && n.attr.size > 0 && n.attr.size <= kMaxCacheableFile) {
            std::string p = dir.empty() ? std::string(tree_.name(c)) : dir + "/" + std::string(tree_.name(c));
            files.push_back(FileMeta{std::move(p), n.attr.mtime_ns, n.attr.size});
          }
        });
      }
    }

    std::vector<std::string> batch;
    std::vector<const FileMeta*> meta;
    std::uint64_t bytes = 0;
    std::uint64_t got_files = 0, got_bytes = 0;  // fetched this directory (for stats)
    auto flush = [&] {
      if (batch.empty() || pf_stop_.load()) {
        batch.clear();
        meta.clear();
        bytes = 0;
        return;
      }
      if (auto res = read_many(batch); res) {
        for (std::size_t i = 0; i < batch.size(); ++i)
          if ((*res)[i]) {
            got_files += 1;
            got_bytes += (*res)[i]->size();
            cache_put(batch[i], meta[i]->mtime, meta[i]->size, std::move(*(*res)[i]));
          }
      }
      batch.clear();
      meta.clear();
      bytes = 0;
    };

    for (const FileMeta& fm : files) {
      if (pf_stop_.load()) break;
      {  // skip files already cached at the current version
        std::lock_guard lock(rcache_mu_);
        const auto it = rcache_.find(fm.path);
        if (it != rcache_.end() && it->second.mtime_ns == fm.mtime && it->second.size == fm.size) continue;
      }
      batch.push_back(fm.path);
      meta.push_back(&fm);
      bytes += fm.size;
      if (batch.size() >= kPrefetchBatchCount || bytes >= kPrefetchBatchBytes) flush();
    }
    flush();

    if (got_files) {
      std::lock_guard s(stats_mu_);
      stats_.prefetch_files += got_files;
      stats_.prefetch_bytes += got_bytes;
    }
    // This directory is done; wake anyone waiting for the prefetcher to drain.
    if (pf_pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      std::lock_guard lock(pf_mu_);
      pf_done_cv_.notify_all();
    }
  }
}

std::size_t RemoteRoot::warm_cache() {
  std::vector<std::string> dirs;
  {
    std::shared_lock lock(tree_mu_);
    const std::uint64_t cap = rcache_cap_;
    std::uint64_t budget = 0;
    std::vector<NodeId> stack{tree_.root()};
    while (!stack.empty()) {
      const NodeId id = stack.back();
      stack.pop_back();
      if (!tree_.node(id).is_dir()) continue;
      std::uint64_t dir_bytes = 0;
      tree_.for_each_child(id, [&](NodeId c) {
        const auto& n = tree_.node(c);
        if (n.attr.kind == NodeKind::File && n.attr.size > 0 && n.attr.size <= kMaxCacheableFile)
          dir_bytes += n.attr.size;
        if (n.is_dir()) stack.push_back(c);  // descend regardless of this dir's budget
      });
      if (dir_bytes == 0) continue;                 // nothing cacheable here
      if (budget + dir_bytes > cap) continue;       // over budget: leave to lazy read-ahead
      budget += dir_bytes;
      dirs.push_back(tree_.path_of(id));
    }
  }
  // Enqueue without the tree lock held (prefetch_loop takes pf_mu_ then tree_mu_).
  for (auto& d : dirs) enqueue_prefetch(std::move(d));
  return dirs.size();
}

void RemoteRoot::rescan_loop() {
  for (;;) {
    {
      std::unique_lock lock(rs_mu_);
      rs_cv_.wait(lock, [&] { return rs_stop_ || rs_requested_; });
      if (rs_stop_) return;
      rs_requested_ = false;  // any Rescan arriving from here on schedules another pass
    }
    if (closed_) return;
    // fetch_snapshot() replaces the mirror and replays whatever changed while
    // the request was in flight, so the tree is current again — not merely as
    // of the moment the agent started scanning.
    if (!fetch_snapshot()) continue;  // connection gone or a bad reply; nothing more to do here
    {
      // Directories may have changed without per-path events, so let the
      // read-ahead visit them again on the next miss.
      std::lock_guard lock(pf_mu_);
      pf_seen_.clear();
    }
    std::lock_guard s(stats_mu_);
    ++stats_.rescans;
  }
}

void RemoteRoot::wait_prefetch_idle(std::chrono::milliseconds timeout) {
  std::unique_lock lock(pf_mu_);
  pf_done_cv_.wait_for(lock, timeout, [&] {
    return pf_stop_.load() || (pf_pending_.load(std::memory_order_acquire) == 0 && pf_queue_.empty());
  });
}

Result<std::chrono::nanoseconds> RemoteRoot::ping(std::chrono::milliseconds timeout) {
  const auto t0 = std::chrono::steady_clock::now();
  auto p = request(proto::MsgType::Ping, {}, timeout);
  if (!p) return fail(p.error());
  if ((*p)->type != proto::MsgType::Pong) return fail(Errc::ProtocolError);
  return std::chrono::steady_clock::now() - t0;
}

void RemoteRoot::drop_cached(std::string_view path) {
  std::lock_guard lock(rcache_mu_);
  if (const auto it = rcache_.find(path); it != rcache_.end()) cache_erase(it);
}

Result<void> RemoteRoot::create_file(std::string_view path, std::uint32_t mode, std::chrono::milliseconds timeout) {
  std::vector<std::byte> pl;
  proto::Writer w(pl);
  proto::write_create_request(w, proto::CreateRequest{.path = path, .mode = mode});
  auto p = request(proto::MsgType::CreateRequest, pl, timeout);
  if (!p) return fail(p.error());
  if ((*p)->type != proto::MsgType::CreateResponse) return fail(Errc::ProtocolError);
  proto::Reader r((*p)->payload);
  auto attr = proto::read_attributes(r);
  if (!attr) return fail(attr.error());
  std::unique_lock lock(tree_mu_);
  (void)tree_.upsert_path(path, *attr);
  return {};
}

Result<std::uint64_t> RemoteRoot::write(std::string_view path, std::uint64_t offset, std::span<const std::byte> data,
                                        std::chrono::milliseconds timeout) {
  std::vector<std::byte> pl;
  proto::Writer w(pl);
  proto::write_write_request(w, proto::WriteRequest{.path = path, .offset = offset, .data = data});
  auto p = request(proto::MsgType::WriteRequest, pl, timeout);
  if (!p) return fail(p.error());
  if ((*p)->type != proto::MsgType::WriteResponse) return fail(Errc::ProtocolError);
  proto::Reader r((*p)->payload);
  auto resp = proto::read_write_response(r);
  if (!resp) return fail(resp.error());
  const std::uint64_t end = offset + resp->written;
  drop_cached(path);
  std::unique_lock lock(tree_mu_);
  if (const auto id = tree_.lookup(path, LookupMode::Exact)) {
    Attributes a = tree_.node(*id).attr;
    a.size = std::max(a.size, end);
    (void)tree_.set_attributes(*id, a);
  } else {
    (void)tree_.upsert_path(path, Attributes{.size = end, .mtime_ns = 0, .mode = 0644, .kind = NodeKind::File});
  }
  return resp->written;
}

Result<void> RemoteRoot::truncate(std::string_view path, std::uint64_t size, std::chrono::milliseconds timeout) {
  std::vector<std::byte> pl;
  proto::Writer w(pl);
  proto::write_truncate_request(w, proto::TruncateRequest{.path = path, .size = size});
  auto p = request(proto::MsgType::TruncateRequest, pl, timeout);
  if (!p) return fail(p.error());
  if ((*p)->type != proto::MsgType::Ok) return fail(Errc::ProtocolError);
  drop_cached(path);
  std::unique_lock lock(tree_mu_);
  if (const auto id = tree_.lookup(path, LookupMode::Exact)) {
    Attributes a = tree_.node(*id).attr;
    a.size = size;
    (void)tree_.set_attributes(*id, a);
  }
  return {};
}

Result<void> RemoteRoot::mkdir(std::string_view path, std::uint32_t mode, std::chrono::milliseconds timeout) {
  std::vector<std::byte> pl;
  proto::Writer w(pl);
  proto::write_mkdir_request(w, proto::MkdirRequest{.path = path, .mode = mode});
  auto p = request(proto::MsgType::MkdirRequest, pl, timeout);
  if (!p) return fail(p.error());
  if ((*p)->type != proto::MsgType::Ok) return fail(Errc::ProtocolError);
  std::unique_lock lock(tree_mu_);
  (void)tree_.upsert_path(path, Attributes{.size = 0, .mtime_ns = 0, .mode = mode, .kind = NodeKind::Directory});
  return {};
}

Result<void> RemoteRoot::unlink(std::string_view path, std::chrono::milliseconds timeout) {
  std::vector<std::byte> pl;
  proto::Writer w(pl);
  proto::write_path_request(w, proto::PathRequest{.path = path});
  auto p = request(proto::MsgType::UnlinkRequest, pl, timeout);
  if (!p) return fail(p.error());
  if ((*p)->type != proto::MsgType::Ok) return fail(Errc::ProtocolError);
  std::unique_lock lock(tree_mu_);
  (void)tree_.remove_path(path);
  return {};
}

Result<void> RemoteRoot::rmdir(std::string_view path, std::chrono::milliseconds timeout) {
  std::vector<std::byte> pl;
  proto::Writer w(pl);
  proto::write_path_request(w, proto::PathRequest{.path = path});
  auto p = request(proto::MsgType::RmdirRequest, pl, timeout);
  if (!p) return fail(p.error());
  if ((*p)->type != proto::MsgType::Ok) return fail(Errc::ProtocolError);
  std::unique_lock lock(tree_mu_);
  (void)tree_.remove_path(path);
  return {};
}

Result<void> RemoteRoot::rename(std::string_view from, std::string_view to, std::chrono::milliseconds timeout) {
  std::vector<std::byte> pl;
  proto::Writer w(pl);
  proto::write_rename_request(w, proto::RenameRequest{.from = from, .to = to});
  auto p = request(proto::MsgType::RenameRequest, pl, timeout);
  if (!p) return fail(p.error());
  if ((*p)->type != proto::MsgType::Ok) return fail(Errc::ProtocolError);
  drop_cached(from);
  drop_cached(to);
  std::unique_lock lock(tree_mu_);
  Attributes moved{.size = 0, .mtime_ns = 0, .mode = 0644, .kind = NodeKind::File};
  bool have = false;
  if (const auto id = tree_.lookup(from, LookupMode::Exact)) {
    moved = tree_.node(*id).attr;
    have = true;
  }
  (void)tree_.remove_path(from);
  if (have) (void)tree_.upsert_path(to, moved);
  return {};
}

Result<std::shared_ptr<RemoteRoot::Pending>> RemoteRoot::request(proto::MsgType type, std::span<const std::byte> payload,
                                                                std::chrono::milliseconds timeout, bool streaming) {
  if (closed_) return fail(Errc::ConnectionClosed);
  const std::uint64_t id = next_request_.fetch_add(1);
  auto pending = std::make_shared<Pending>();
  pending->streaming = streaming;
  {
    std::lock_guard lock(pending_mu_);
    pending_.emplace(id, pending);
  }
  if (auto r = ch_->send(type, id, payload); !r) {
    std::lock_guard lock(pending_mu_);
    pending_.erase(id);
    return fail(r.error());
  }
  std::unique_lock lock(pending->mu);
  if (!pending->cv.wait_for(lock, timeout, [&] { return pending->done; })) {
    std::lock_guard plock(pending_mu_);
    pending_.erase(id);
    return fail(Errc::Timeout);
  }
  if (pending->type == proto::MsgType::Error) {
    proto::Reader r(pending->payload);
    auto e = proto::read_error(r);
    if (e && e->code != 0 && e->code <= static_cast<std::uint32_t>(kLastErrc))
      return fail(static_cast<Errc>(e->code));
    return fail(Errc::ProtocolError);
  }
  if (pending->payload.empty() && pending->type == proto::MsgType::Ping) return fail(Errc::ConnectionClosed);
  return pending;
}

void RemoteRoot::reader_loop() {
  while (!closed_) {
    auto f = ch_->receive();
    if (!f) break;
    if (f->header.type == proto::MsgType::Invalidation) {
      apply_invalidation(f->payload);
      continue;
    }
    const bool streamed_chunk = f->header.type == proto::MsgType::Snapshot;
    std::shared_ptr<Pending> pending;
    {
      std::lock_guard lock(pending_mu_);
      const auto it = pending_.find(f->header.request_id);
      if (it == pending_.end()) continue;  // late reply to a timed-out request
      pending = it->second;
      // A streaming snapshot stays registered until it completes (handled below);
      // every other reply completes on this single frame.
      if (!(pending->streaming && streamed_chunk)) pending_.erase(it);
    }

    if (pending->streaming && streamed_chunk) {
      bool complete = false;
      {
        std::lock_guard lock(pending->mu);
        pending->snap_bytes += f->payload.size();
        proto::Reader r(f->payload);
        if (auto hdr = proto::read_snapshot_header(r)) {
          pending->snap_generation = hdr->generation;
          for (std::uint32_t i = 0; i < hdr->count; ++i) {
            auto e = proto::read_snapshot_entry(r);
            if (!e) {
              pending->stream_ok = false;
              break;
            }
            pending->snap_names.emplace_back(e->name);
            pending->snap_entries.push_back(SnapshotEntry{e->parent, {}, e->attr});
          }
        } else {
          pending->stream_ok = false;
        }
        const bool more = (f->header.flags & proto::kFlagMore) != 0;
        if (!more || !pending->stream_ok) {
          pending->type = proto::MsgType::Snapshot;
          pending->done = true;
          complete = true;
        }
      }
      if (complete) {
        {
          std::lock_guard lock(pending_mu_);
          pending_.erase(f->header.request_id);
        }
        pending->cv.notify_one();
      }
      continue;
    }

    {
      std::lock_guard lock(pending->mu);
      pending->type = f->header.type;
      pending->payload.assign(f->payload.begin(), f->payload.end());
      pending->done = true;
    }
    pending->cv.notify_one();
  }
  closed_ = true;
  fail_all_pending();
}

void RemoteRoot::fail_all_pending() {
  std::unordered_map<std::uint64_t, std::shared_ptr<Pending>> pending;
  {
    std::lock_guard lock(pending_mu_);
    pending.swap(pending_);
  }
  for (auto& [id, p] : pending) {
    std::lock_guard lock(p->mu);
    p->type = proto::MsgType::Ping;  // marker: no payload + Ping type == connection lost
    p->payload.clear();
    p->done = true;
    p->cv.notify_all();
  }
}

void RemoteRoot::apply_invalidation(std::span<const std::byte> payload) {
  proto::Reader r(payload);
  auto batch = proto::read_invalidation(r);
  if (!batch) return;
  bool rescan = false;
  {
    std::unique_lock lock(tree_mu_);
    for (const proto::InvalidationOp& op : batch->ops) {
      switch (op.kind) {
        case InvalidationKind::Upsert: (void)tree_.upsert_path(op.path, op.attr); break;
        case InvalidationKind::Remove: (void)tree_.remove_path(op.path); break;
        case InvalidationKind::Rescan: rescan = true; break;  // handled below, off this thread
      }
    }
    // A snapshot being fetched right now will replace this tree; keep the batch
    // so fetch_snapshot() can replay it if the snapshot pre-dates it.
    if (snapshot_in_flight_) snapshot_replay_.push_back(*batch);
  }
  if (rescan) {
    // The agent's watcher overflowed: per-path events were lost, so the mirror
    // can no longer be trusted. Re-fetch the snapshot on the rescan thread —
    // this is the reader thread, and the request would wait on itself.
    std::lock_guard lock(rs_mu_);
    rs_requested_ = true;
    rs_cv_.notify_one();
  }
  // Drop cached contents for any changed path so the next read refetches.
  {
    std::lock_guard lock(rcache_mu_);
    for (const proto::InvalidationOp& op : batch->ops) {
      if (const auto it = rcache_.find(op.path); it != rcache_.end()) cache_erase(it);
    }
  }
  // Let a changed directory be re-prefetched on the next read there.
  {
    std::lock_guard lock(pf_mu_);
    for (const proto::InvalidationOp& op : batch->ops) pf_seen_.erase(std::string(split_parent(op.path).first));
  }
  InvalidationHook hook;
  {
    std::lock_guard lock(stats_mu_);
    ++stats_.invalidation_batches;
    stats_.invalidation_ops += batch->ops.size();
    stats_.generation = batch->generation;
    hook = hook_;
  }
  if (hook) hook(*batch);
}

}  // namespace wsld::agent
