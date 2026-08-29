#pragma once

#include "core/protocol.hpp"
#include "net/socket.hpp"

#include <chrono>
#include <cstddef>
#include <mutex>
#include <span>
#include <vector>

namespace wsld::net {

/// A received frame. `payload` points into the channel's receive buffer and is
/// valid until the next call to `receive`.
struct Frame {
  proto::FrameHeader header;
  std::span<const std::byte> payload;
};

/// Sends and receives protocol frames over a stream socket.
///
/// `send*` is thread-safe (serialised by a mutex) so that a watcher thread can
/// push invalidations while the request thread is busy. `receive` must be
/// called from one thread at a time.
class FrameChannel {
 public:
  explicit FrameChannel(Socket sock) noexcept : sock_(std::move(sock)) {}

  /// Sends a header + payload as one write.
  [[nodiscard]] Result<void> send(proto::MsgType type, std::uint64_t request_id, std::span<const std::byte> payload,
                                  std::uint32_t flags = 0);

  /// Sends one or more frames already laid out with proto::write_frame.
  [[nodiscard]] Result<void> send_raw(std::span<const std::byte> frames);

  /// Convenience: builds the payload with `body(Writer&)` and sends it.
  template <class F>
  [[nodiscard]] Result<void> send_with(proto::MsgType type, std::uint64_t request_id, F&& body) {
    std::vector<std::byte> buf;
    buf.reserve(256);
    proto::write_frame(buf, type, request_id, std::forward<F>(body));
    return send_raw(buf);
  }

  [[nodiscard]] Result<Frame> receive();
  /// Waits up to `timeout` for a frame to begin arriving; returns Errc::Timeout
  /// if none did. Once a header starts, the full frame is read (blocking).
  [[nodiscard]] Result<Frame> receive(std::chrono::milliseconds timeout);

  /// Bounds what receive() will accept: frames with a payload above
  /// `max_payload` are rejected (TooLarge), and once a frame has started
  /// arriving the rest must be in within `frame_timeout` (0 = no limit). A
  /// server tightens both until the peer has authenticated - an unauthenticated
  /// peer must not be able to reserve 64 MiB or park a session thread with a
  /// half-sent frame.
  void set_receive_limits(std::uint32_t max_payload, std::chrono::milliseconds frame_timeout) noexcept {
    max_payload_ = max_payload;
    frame_timeout_ = frame_timeout;
  }

  void shutdown() noexcept { sock_.shutdown(); }
  void close() noexcept { sock_.close(); }
  [[nodiscard]] bool valid() const noexcept { return sock_.valid(); }

  struct Stats {
    std::uint64_t frames_sent = 0, bytes_sent = 0, frames_received = 0, bytes_received = 0;
  };
  [[nodiscard]] Stats stats() const noexcept { return stats_; }

 private:
  Socket sock_;
  std::mutex send_mu_;
  std::vector<std::byte> send_buf_;
  std::vector<std::byte> recv_buf_;
  Stats stats_;
  std::uint32_t max_payload_ = proto::kMaxPayload;
  std::chrono::milliseconds frame_timeout_{0};
};

}  // namespace wsld::net
