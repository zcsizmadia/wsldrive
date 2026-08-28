#include "agent/server.hpp"

#include "agent/scanner.hpp"
#include "core/path.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <system_error>

namespace wsld::agent {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

RootServer::RootServer(Options opts) : opts_(std::move(opts)), coalescer_(opts_.coalescer) {
  // Load .wsldriveignore from the served root, if present.
  std::error_code ec;
  const fs::path ignore_file = opts_.root / ".wsldriveignore";
  if (fs::is_regular_file(ignore_file, ec)) {
    if (std::FILE* fp =
#ifdef _WIN32
            _wfopen(ignore_file.c_str(), L"rb")
#else
            std::fopen(ignore_file.c_str(), "rb")
#endif
    ) {
      std::string text;
      char buf[4096];
      std::size_t n;
      while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) text.append(buf, n);
      std::fclose(fp);
      ignore_ = IgnoreRules::parse(text);
    }
  }
}

RootServer::~RootServer() { stop(); }

Result<void> RootServer::start() {
  if (!opts_.watch) return {};
  auto w = platform::make_watcher(opts_.root, [this](const FsEvent& ev) { on_event(ev); });
  if (!w) {
    if (w.error() == Errc::Unsupported) return {};
    return fail(w.error());
  }
  watcher_ = std::move(*w);
  flusher_ = std::thread([this] { flush_loop(); });
  return {};
}

void RootServer::stop() {
  if (stopping_.exchange(true)) return;
  if (watcher_) watcher_->stop();
  {
    std::lock_guard lock(coalescer_mu_);
    coalescer_cv_.notify_all();
  }
  if (flusher_.joinable()) flusher_.join();
  // Unblock every session thread parked in receive(); serve() then returns.
  std::lock_guard lock(peers_mu_);
  for (net::FrameChannel* peer : peers_) peer->shutdown();
}

void RootServer::on_event(const FsEvent& ev) {
  std::lock_guard lock(coalescer_mu_);
  coalescer_.push(ev, Coalescer::clock::now());
  coalescer_cv_.notify_one();
}

void RootServer::flush_loop() {
  std::unique_lock lock(coalescer_mu_);
  while (!stopping_) {
    if (coalescer_.empty()) {
      coalescer_cv_.wait(lock, [&] { return stopping_ || !coalescer_.empty(); });
      continue;
    }
    const auto now = Coalescer::clock::now();
    if (!coalescer_.ready(now)) {
      coalescer_cv_.wait_until(lock, *coalescer_.deadline());
      continue;
    }
    std::vector<PlannedOp> planned = coalescer_.take();
    lock.unlock();

    // Resolve attributes outside the lock: one stat per touched path.
    proto::InvalidationBatch batch;
    batch.generation = generation_.fetch_add(1) + 1;
    batch.ops.reserve(planned.size());
    for (PlannedOp& op : planned) {
      // Ignored paths are absent from the client's tree; skip their events. (Kind
      // does not tell us dir vs file for removes, so test both interpretations.)
      if (!ignore_.empty() && op.kind != InvalidationKind::Rescan &&
          (ignore_.ignored(op.path, /*is_dir=*/false) || ignore_.ignored(op.path, /*is_dir=*/true)))
        continue;
      proto::InvalidationOp out;
      out.kind = op.kind;
      out.path = std::move(op.path);
      if (out.kind == InvalidationKind::Upsert) {
        auto attr = read_attributes(join_relative(opts_.root, out.path));
        if (!attr) {
          out.kind = InvalidationKind::Remove;  // vanished between the event and now
        } else if (attr->kind == NodeKind::Other) {
          continue;
        } else {
          out.attr = *attr;
        }
      }
      batch.ops.push_back(std::move(out));
    }
    if (!batch.ops.empty()) {
      std::vector<std::byte> frame;
      proto::write_frame(frame, proto::MsgType::Invalidation, 0, [&](proto::Writer& w) { proto::write_invalidation(w, batch); });
      broadcast(frame);
    }
    lock.lock();
  }
}

void RootServer::broadcast(const std::vector<std::byte>& frame) {
  std::lock_guard lock(peers_mu_);
  for (net::FrameChannel* peer : peers_) (void)peer->send_raw(frame);
}

void RootServer::serve(net::FrameChannel& ch) {
  {
    std::lock_guard lock(peers_mu_);
    if (stopping_) return;
    peers_.push_back(&ch);
  }
  for (;;) {
    auto f = ch.receive(std::chrono::milliseconds(200));
    if (!f) {
      if (f.error() == Errc::Timeout) {
        if (stopping_) break;
        continue;
      }
      break;  // peer disconnected or I/O error
    }
    if (auto r = handle(*f, ch); !r) break;
  }
  {
    std::lock_guard lock(peers_mu_);
    peers_.erase(std::remove(peers_.begin(), peers_.end(), &ch), peers_.end());
  }
}

Result<void> RootServer::handle(const net::Frame& f, net::FrameChannel& ch) {
  switch (f.header.type) {
    case proto::MsgType::Hello: {
      proto::Reader r(f.payload);
      auto hello = proto::read_hello(r);
      if (!hello) return fail(hello.error());
      if (hello->protocol_version != proto::kVersion) {
        (void)send_error(f.header.request_id, Errc::UnsupportedVersion, "protocol version mismatch", ch);
        return fail(Errc::UnsupportedVersion);
      }
      return ch.send_with(proto::MsgType::HelloAck, f.header.request_id, [&](proto::Writer& w) {
        proto::write_hello(w, proto::Hello{.protocol_version = proto::kVersion,
                                           .capabilities = watching() ? 1u : 0u,
                                           .agent = "wsldrived/0.1"});
      });
    }
    case proto::MsgType::Ping: return ch.send(proto::MsgType::Pong, f.header.request_id, {});
    case proto::MsgType::SnapshotRequest: return send_snapshot(f.header.request_id, ch);
    case proto::MsgType::ReadRequest: return send_read(f, ch);
    case proto::MsgType::WriteRequest:
    case proto::MsgType::CreateRequest:
    case proto::MsgType::MkdirRequest:
    case proto::MsgType::UnlinkRequest:
    case proto::MsgType::RmdirRequest:
    case proto::MsgType::RenameRequest:
    case proto::MsgType::TruncateRequest:
      return handle_mutation(f, ch);
    default: return send_error(f.header.request_id, Errc::ProtocolError, "unexpected message type", ch);
  }
}

Result<std::filesystem::path> RootServer::resolve(std::string_view rel) const {
  const std::string norm = normalize_path(rel);
  if (norm.find("..") != std::string::npos) return fail(Errc::InvalidPath);
  return join_relative(opts_.root, norm);
}

namespace {
Errc map_fs_error(const std::error_code& ec) {
  // Compare against std::errc conditions (not error_codes): this uses the
  // category's equivalence, so it works whether ec is generic (POSIX) or
  // system_category (Win32) as std::filesystem produces on Windows.
  if (ec == std::errc::no_such_file_or_directory) return Errc::NotFound;
  if (ec == std::errc::file_exists) return Errc::AlreadyExists;
  if (ec == std::errc::not_a_directory) return Errc::NotADirectory;
  if (ec == std::errc::is_a_directory) return Errc::IsADirectory;
  if (ec == std::errc::directory_not_empty) return Errc::AlreadyExists;
  if (ec == std::errc::invalid_argument) return Errc::InvalidArgument;
  return Errc::IoError;
}
}  // namespace

Result<void> RootServer::handle_mutation(const net::Frame& f, net::FrameChannel& ch) {
  namespace fs = std::filesystem;
  const std::uint64_t id = f.header.request_id;
  proto::Reader r(f.payload);
  auto reject = [&](Errc e, std::string_view d) { return send_error(id, e, d, ch); };

  switch (f.header.type) {
    case proto::MsgType::CreateRequest: {
      auto q = proto::read_create_request(r);
      if (!q) return fail(q.error());
      auto full = resolve(q->path);
      if (!full) return reject(full.error(), q->path);
      {
        std::FILE* fp =
#ifdef _WIN32
            _wfopen(full->c_str(), L"wb");
#else
            std::fopen(full->c_str(), "wb");
#endif
        if (fp == nullptr) return reject(Errc::IoError, q->path);
        std::fclose(fp);
      }
      auto attr = read_attributes(*full);
      if (!attr) return reject(attr.error(), q->path);
      return ch.send_with(proto::MsgType::CreateResponse, id, [&](proto::Writer& w) { proto::write_attributes(w, *attr); });
    }
    case proto::MsgType::WriteRequest: {
      auto q = proto::read_write_request(r);
      if (!q) return fail(q.error());
      auto full = resolve(q->path);
      if (!full) return reject(full.error(), q->path);
      std::FILE* fp =
#ifdef _WIN32
          _wfopen(full->c_str(), L"r+b");
      if (fp == nullptr) fp = _wfopen(full->c_str(), L"w+b");
#else
          std::fopen(full->c_str(), "r+b");
      if (fp == nullptr) fp = std::fopen(full->c_str(), "w+b");
#endif
      if (fp == nullptr) return reject(Errc::IoError, q->path);
#ifdef _WIN32
      _fseeki64(fp, static_cast<long long>(q->offset), SEEK_SET);
#else
      fseeko(fp, static_cast<off_t>(q->offset), SEEK_SET);
#endif
      const std::size_t n = q->data.empty() ? 0 : std::fwrite(q->data.data(), 1, q->data.size(), fp);
      std::fclose(fp);
      if (n != q->data.size()) return reject(Errc::IoError, q->path);
      return ch.send_with(proto::MsgType::WriteResponse, id,
                          [&](proto::Writer& w) { proto::write_write_response(w, proto::WriteResponse{n}); });
    }
    case proto::MsgType::TruncateRequest: {
      auto q = proto::read_truncate_request(r);
      if (!q) return fail(q.error());
      auto full = resolve(q->path);
      if (!full) return reject(full.error(), q->path);
      std::error_code ec;
      fs::resize_file(*full, q->size, ec);
      if (ec) return reject(map_fs_error(ec), q->path);
      return ch.send(proto::MsgType::Ok, id, {});
    }
    case proto::MsgType::MkdirRequest: {
      auto q = proto::read_mkdir_request(r);
      if (!q) return fail(q.error());
      auto full = resolve(q->path);
      if (!full) return reject(full.error(), q->path);
      std::error_code ec;
      if (!fs::create_directory(*full, ec) || ec) return reject(ec ? map_fs_error(ec) : Errc::AlreadyExists, q->path);
      return ch.send(proto::MsgType::Ok, id, {});
    }
    case proto::MsgType::UnlinkRequest: {
      auto q = proto::read_path_request(r);
      if (!q) return fail(q.error());
      auto full = resolve(q->path);
      if (!full) return reject(full.error(), q->path);
      std::error_code ec;
      if (fs::is_directory(*full, ec)) return reject(Errc::IsADirectory, q->path);
      if (!fs::remove(*full, ec) || ec) return reject(ec ? map_fs_error(ec) : Errc::NotFound, q->path);
      return ch.send(proto::MsgType::Ok, id, {});
    }
    case proto::MsgType::RmdirRequest: {
      auto q = proto::read_path_request(r);
      if (!q) return fail(q.error());
      auto full = resolve(q->path);
      if (!full) return reject(full.error(), q->path);
      std::error_code ec;
      if (!fs::is_directory(*full, ec)) return reject(Errc::NotADirectory, q->path);
      if (!fs::remove(*full, ec) || ec) return reject(ec ? map_fs_error(ec) : Errc::NotFound, q->path);
      return ch.send(proto::MsgType::Ok, id, {});
    }
    case proto::MsgType::RenameRequest: {
      auto q = proto::read_rename_request(r);
      if (!q) return fail(q.error());
      auto from = resolve(q->from);
      auto to = resolve(q->to);
      if (!from) return reject(from.error(), q->from);
      if (!to) return reject(to.error(), q->to);
      std::error_code ec;
      fs::rename(*from, *to, ec);
      if (ec) return reject(map_fs_error(ec), q->to);
      return ch.send(proto::MsgType::Ok, id, {});
    }
    default:
      return reject(Errc::ProtocolError, "not a mutation");
  }
}

Result<void> RootServer::send_snapshot(std::uint64_t request_id, net::FrameChannel& ch) {
  const std::uint64_t gen = generation_.load();
  const std::size_t budget = opts_.snapshot_chunk_bytes;

  // Each frame is self-describing: SnapshotHeader{gen, entries-in-this-frame}
  // followed by that many entries. The kFlagMore flag is set on every frame but
  // the last, so the client accumulates until it sees a frame without it.
  std::vector<std::byte> entries;  // encoded entries buffered for the current frame
  std::uint32_t count = 0;
  Result<void> send_rc{};

  auto flush = [&](bool more) -> Result<void> {
    std::vector<std::byte> frame;
    proto::write_frame(
        frame, proto::MsgType::Snapshot, request_id,
        [&](proto::Writer& w) {
          proto::write_snapshot_header(w, gen, count);
          w.raw(entries);
        },
        more ? proto::kFlagMore : 0u);
    entries.clear();
    count = 0;
    return ch.send_raw(frame);
  };

  auto scanned = scan_tree(
      opts_.root,
      [&](const SnapshotEntry& e) {
        if (!send_rc) return;  // a previous flush failed; stop buffering
        proto::Writer w(entries);
        proto::write_snapshot_entry(w, e);
        ++count;
        if (entries.size() >= budget) send_rc = flush(/*more=*/true);
      },
      ignore_.empty() ? agent::SkipPredicate{}
                      : [this](std::string_view rel, bool is_dir) { return ignore_.ignored(rel, is_dir); });
  if (!scanned) return send_error(request_id, scanned.error(), "scan failed", ch);
  if (!send_rc) return send_rc;
  return flush(/*more=*/false);  // final frame (possibly empty) terminates the stream
}

Result<void> RootServer::send_read(const net::Frame& f, net::FrameChannel& ch) {
  proto::Reader r(f.payload);
  auto q = proto::read_read_request(r);
  if (!q) return fail(q.error());
  const std::string rel = normalize_path(q->path);
  if (rel.find("..") != std::string::npos) return send_error(f.header.request_id, Errc::InvalidPath, "'..' not allowed", ch);
  const fs::path full = join_relative(opts_.root, rel);

  std::error_code ec;
  const auto size = fs::file_size(full, ec);
  if (ec) return send_error(f.header.request_id, Errc::NotFound, rel, ch);

  const std::uint64_t offset = std::min<std::uint64_t>(q->offset, size);
  const std::uint64_t want = std::min<std::uint64_t>(q->length, size - offset);
  if (want > proto::kMaxPayload - 64) return send_error(f.header.request_id, Errc::TooLarge, "read too large", ch);

  std::vector<std::byte> data(static_cast<std::size_t>(want));
  if (want != 0) {
#ifdef _WIN32
    std::FILE* fp = _wfopen(full.c_str(), L"rb");
#else
    std::FILE* fp = std::fopen(full.c_str(), "rb");
#endif
    if (fp == nullptr) return send_error(f.header.request_id, Errc::IoError, rel, ch);
#ifdef _WIN32
    _fseeki64(fp, static_cast<long long>(offset), SEEK_SET);
#else
    fseeko(fp, static_cast<off_t>(offset), SEEK_SET);
#endif
    const std::size_t got = std::fread(data.data(), 1, data.size(), fp);
    std::fclose(fp);
    data.resize(got);
  }
  return ch.send_with(proto::MsgType::ReadResponse, f.header.request_id, [&](proto::Writer& w) {
    proto::write_read_response(w, proto::ReadResponse{.file_size = size, .data = data});
  });
}

Result<void> RootServer::send_error(std::uint64_t request_id, Errc code, std::string_view detail, net::FrameChannel& ch) {
  return ch.send_with(proto::MsgType::Error, request_id, [&](proto::Writer& w) {
    proto::write_error(w, proto::ErrorMessage{.code = static_cast<std::uint32_t>(code), .detail = detail});
  });
}

}  // namespace wsld::agent
