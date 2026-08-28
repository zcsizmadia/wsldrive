#include "agent/client.hpp"

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
    ch_->shutdown();
    ch_->close();
    if (reader_.joinable()) reader_.join();
    fail_all_pending();
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
  std::vector<std::byte> payload;
  proto::Writer w(payload);
  proto::write_hello(w, proto::Hello{.protocol_version = proto::kVersion, .capabilities = 0, .agent = "wsldrive/0.1"});
  auto p = request(proto::MsgType::Hello, payload, timeout);
  if (!p) return fail(p.error());
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
  auto p = request(proto::MsgType::SnapshotRequest, {}, timeout);
  if (!p) return fail(p.error());
  if ((*p)->type != proto::MsgType::Snapshot) return fail(Errc::ProtocolError);

  std::vector<SnapshotEntry> entries;
  proto::Reader r((*p)->payload);
  auto hdr = proto::read_snapshot_header(r);
  if (!hdr) return fail(hdr.error());
  entries.reserve(hdr->count);
  for (std::uint32_t i = 0; i < hdr->count; ++i) {
    auto e = proto::read_snapshot_entry(r);
    if (!e) return fail(e.error());
    entries.push_back(*e);
  }
  {
    std::unique_lock lock(tree_mu_);
    if (auto res = tree_.load_snapshot(entries); !res) return res;
  }
  std::lock_guard lock(stats_mu_);
  stats_.generation = hdr->generation;
  stats_.snapshot_bytes = (*p)->payload.size();
  stats_.last_snapshot_time = std::chrono::steady_clock::now() - t0;
  return {};
}

Result<std::vector<std::byte>> RemoteRoot::read(std::string_view path, std::uint64_t offset, std::uint32_t length,
                                                std::chrono::milliseconds timeout) {
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

Result<std::chrono::nanoseconds> RemoteRoot::ping(std::chrono::milliseconds timeout) {
  const auto t0 = std::chrono::steady_clock::now();
  auto p = request(proto::MsgType::Ping, {}, timeout);
  if (!p) return fail(p.error());
  if ((*p)->type != proto::MsgType::Pong) return fail(Errc::ProtocolError);
  return std::chrono::steady_clock::now() - t0;
}

Result<std::shared_ptr<RemoteRoot::Pending>> RemoteRoot::request(proto::MsgType type, std::span<const std::byte> payload,
                                                                std::chrono::milliseconds timeout) {
  if (closed_) return fail(Errc::ConnectionClosed);
  const std::uint64_t id = next_request_.fetch_add(1);
  auto pending = std::make_shared<Pending>();
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
    if (e && e->code != 0 && e->code <= static_cast<std::uint32_t>(Errc::ProtocolError))
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
    std::shared_ptr<Pending> pending;
    {
      std::lock_guard lock(pending_mu_);
      if (auto it = pending_.find(f->header.request_id); it != pending_.end()) {
        pending = it->second;
        pending_.erase(it);
      }
    }
    if (!pending) continue;  // late reply to a timed-out request
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
  {
    std::unique_lock lock(tree_mu_);
    for (const proto::InvalidationOp& op : batch->ops) {
      switch (op.kind) {
        case InvalidationKind::Upsert: (void)tree_.upsert_path(op.path, op.attr); break;
        case InvalidationKind::Remove: (void)tree_.remove_path(op.path); break;
        case InvalidationKind::Rescan: break;  // caller decides when to refetch; see stats()
      }
    }
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
