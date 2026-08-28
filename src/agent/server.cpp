#include "agent/server.hpp"

#include "agent/scanner.hpp"
#include "core/path.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>

namespace wsld::agent {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

RootServer::RootServer(Options opts) : opts_(std::move(opts)), coalescer_(opts_.coalescer) {}

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
    default: return send_error(f.header.request_id, Errc::ProtocolError, "unexpected message type", ch);
  }
}

Result<void> RootServer::send_snapshot(std::uint64_t request_id, net::FrameChannel& ch) {
  std::vector<std::byte> frame;
  frame.reserve(1u << 20);
  std::uint32_t count = 0;
  std::size_t count_pos = 0;
  const std::uint64_t gen = generation_.load();
  Result<ScanStats> scanned;
  proto::write_frame(frame, proto::MsgType::Snapshot, request_id, [&](proto::Writer& w) {
    proto::write_snapshot_header(w, gen, 0);
    count_pos = w.size() - 4;
    scanned = scan_tree(opts_.root, [&](const SnapshotEntry& e) {
      proto::write_snapshot_entry(w, e);
      ++count;
    });
    w.patch_u32(count_pos, count);
  });
  if (!scanned) return send_error(request_id, scanned.error(), "scan failed", ch);
  if (frame.size() - proto::kHeaderSize > proto::kMaxPayload)
    return send_error(request_id, Errc::TooLarge, "tree exceeds one frame; chunked snapshots not yet implemented", ch);
  return ch.send_raw(frame);
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
