#include "net/frame_channel.hpp"

#include <cstring>

namespace wsld::net {

Result<void> FrameChannel::send(proto::MsgType type, std::uint64_t request_id, std::span<const std::byte> payload,
                                std::uint32_t flags) {
  if (payload.size() > proto::kMaxPayload) return fail(Errc::TooLarge);
  std::lock_guard lock(send_mu_);
  send_buf_.resize(proto::kHeaderSize + payload.size());
  proto::FrameHeader h;
  h.type = type;
  h.flags = flags;
  h.payload_len = static_cast<std::uint32_t>(payload.size());
  h.request_id = request_id;
  proto::encode_header(h, std::span<std::byte, proto::kHeaderSize>(send_buf_.data(), proto::kHeaderSize));
  if (!payload.empty()) std::memcpy(send_buf_.data() + proto::kHeaderSize, payload.data(), payload.size());
  auto r = sock_.send_all(send_buf_);
  if (r) {
    ++stats_.frames_sent;
    stats_.bytes_sent += send_buf_.size();
  }
  return r;
}

Result<void> FrameChannel::send_raw(std::span<const std::byte> frames) {
  // The header's length field is 32 bits and the receiver rejects anything
  // above kMaxPayload; a caller that laid out an oversized frame would only
  // learn about it when the peer dropped the connection.
  if (frames.size() > proto::kHeaderSize + proto::kMaxPayload) return fail(Errc::TooLarge);
  std::lock_guard lock(send_mu_);
  auto r = sock_.send_all(frames);
  if (r) {
    ++stats_.frames_sent;
    stats_.bytes_sent += frames.size();
  }
  return r;
}

Result<Frame> FrameChannel::receive(std::chrono::milliseconds timeout) {
  auto ready = sock_.wait_readable(timeout);
  if (!ready) return fail(ready.error());
  if (!*ready) return fail(Errc::Timeout);
  return receive();
}

Result<Frame> FrameChannel::receive() {
  const bool bounded = frame_timeout_.count() > 0;
  const auto deadline = std::chrono::steady_clock::now() + frame_timeout_;
  auto recv = [&](std::span<std::byte> buf) { return bounded ? sock_.recv_exact(buf, deadline) : sock_.recv_exact(buf); };
  std::array<std::byte, proto::kHeaderSize> hdr{};
  if (auto r = recv(hdr); !r) return fail(r.error());
  auto h = proto::decode_header(hdr);
  if (!h) return fail(h.error());
  if (h->payload_len > max_payload_) return fail(Errc::TooLarge);  // checked before any allocation
  recv_buf_.resize(h->payload_len);
  if (h->payload_len != 0) {
    if (auto r = recv(recv_buf_); !r) return fail(r.error());
  }
  ++stats_.frames_received;
  stats_.bytes_received += proto::kHeaderSize + h->payload_len;
  return Frame{*h, std::span<const std::byte>(recv_buf_.data(), recv_buf_.size())};
}

}  // namespace wsld::net
