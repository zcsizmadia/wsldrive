#pragma once

#include "core/error.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace wsld::net {

#ifdef _WIN32
using native_socket = std::uintptr_t;
inline constexpr native_socket kInvalidSocket = ~native_socket{0};
#else
using native_socket = int;
inline constexpr native_socket kInvalidSocket = -1;
#endif

/// Process-wide network initialisation (WSAStartup on Windows). Idempotent.
void init_network();

/// Where to connect to or listen on.
///
/// Textual forms:
///   tcp://host:port          loopback or LAN; works in all WSL networking modes
///   vsock://cid:port         Linux AF_VSOCK; cid "host" = 2 (the Windows host), "any" for listening
///   hv://port                Windows AF_HYPERV listening on the VSOCK-template service for `port`
///   hv://{vm-guid}:port      Windows AF_HYPERV connecting to a specific VM
struct Endpoint {
  enum class Kind : std::uint8_t { Tcp, Vsock, HyperV };

  Kind kind = Kind::Tcp;
  std::string host;         // Tcp: host name or address
  std::string vm_id;        // HyperV: VM GUID (empty = wildcard / children)
  std::uint32_t cid = 0;    // Vsock
  std::uint32_t port = 0;   // all kinds

  [[nodiscard]] static Result<Endpoint> parse(std::string_view text);
  [[nodiscard]] std::string to_string() const;
};

/// Thin RAII wrapper over a connected stream socket. Blocking I/O.
class Socket {
 public:
  Socket() = default;
  explicit Socket(native_socket s) noexcept : s_(s) {}
  ~Socket() { close(); }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& o) noexcept : s_(o.s_) { o.s_ = kInvalidSocket; }
  Socket& operator=(Socket&& o) noexcept;

  [[nodiscard]] bool valid() const noexcept { return s_ != kInvalidSocket; }
  [[nodiscard]] native_socket native() const noexcept { return s_; }

  /// Sends until the whole buffer is written.
  [[nodiscard]] Result<void> send_all(std::span<const std::byte> data) noexcept;
  /// Receives up to buf.size() bytes; returns ConnectionClosed on EOF.
  [[nodiscard]] Result<std::size_t> recv_some(std::span<std::byte> buf) noexcept;
  /// Receives exactly buf.size() bytes.
  [[nodiscard]] Result<void> recv_exact(std::span<std::byte> buf) noexcept;
  /// Same, but gives up with Errc::Timeout once `deadline` passes - so a peer
  /// that trickles bytes cannot park the caller forever.
  [[nodiscard]] Result<void> recv_exact(std::span<std::byte> buf,
                                        std::chrono::steady_clock::time_point deadline) noexcept;
  /// Waits until the socket is readable or `timeout` elapses. Returns true if
  /// readable, false on timeout. Lets a receive loop poll a stop flag.
  [[nodiscard]] Result<bool> wait_readable(std::chrono::milliseconds timeout) noexcept;

  /// Bounds how long a single send may block (SO_SNDTIMEO). Without it a peer
  /// that stops reading fills its socket buffer and parks the sender for good.
  /// Zero clears the bound. A send that times out reports Errc::Timeout and
  /// may have written part of the buffer, so the stream is only good for
  /// dropping afterwards.
  void set_send_timeout(std::chrono::milliseconds timeout) noexcept;

  void set_nodelay(bool on) noexcept;
  void shutdown() noexcept;
  void close() noexcept;

 private:
  native_socket s_ = kInvalidSocket;
};

[[nodiscard]] Result<Socket> connect(const Endpoint& ep);

class Listener {
 public:
  Listener() = default;
  ~Listener() { close(); }
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;
  Listener(Listener&& o) noexcept : s_(o.s_), local_(std::move(o.local_)) { o.s_ = kInvalidSocket; }
  Listener& operator=(Listener&& o) noexcept;

  [[nodiscard]] static Result<Listener> bind(const Endpoint& ep, int backlog = 16);
  [[nodiscard]] Result<Socket> accept();
  /// Accepts within `timeout`; returns Errc::Timeout if none arrived. Lets an
  /// accept loop poll a stop flag instead of relying on unblocking accept().
  [[nodiscard]] Result<Socket> accept(std::chrono::milliseconds timeout);
  /// The bound endpoint; for tcp with port 0 this carries the assigned port.
  [[nodiscard]] const Endpoint& local() const noexcept { return local_; }
  [[nodiscard]] bool valid() const noexcept { return s_ != kInvalidSocket; }
  /// Unblocks a pending accept() and releases the socket.
  void close() noexcept;

 private:
  native_socket s_ = kInvalidSocket;
  Endpoint local_;
};

/// Last OS-level socket error, for diagnostics.
[[nodiscard]] int last_socket_error() noexcept;

}  // namespace wsld::net
