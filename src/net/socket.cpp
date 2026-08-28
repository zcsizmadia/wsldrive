#include "net/socket.hpp"

#include <charconv>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <hvsocket.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <cerrno>
#include <linux/vm_sockets.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace wsld::net {

namespace {

#ifdef _WIN32
using socklen_type = int;
using buf_ptr = char*;
using cbuf_ptr = const char*;

void close_native(native_socket s) noexcept { ::closesocket(static_cast<SOCKET>(s)); }

// The Hyper-V socket "VSOCK template" service GUID: Data1 carries the port and
// the remainder is fixed. A Linux guest reaches it via AF_VSOCK on the same port.
GUID vsock_service_id(std::uint32_t port) noexcept {
  GUID g{port, 0xfacb, 0x11e6, {0xbd, 0x58, 0x64, 0x00, 0x6a, 0x79, 0x86, 0xd3}};
  return g;
}

constexpr GUID kHvGuidWildcard{0x00000000, 0x0000, 0x0000, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};

bool parse_guid(std::string_view text, GUID& out) noexcept {
  std::string s(text);
  if (!s.empty() && s.front() == '{') s.erase(0, 1);
  if (!s.empty() && s.back() == '}') s.pop_back();
  if (s.size() != 36) return false;
  unsigned long d1;
  unsigned d2, d3, b[8];
  if (std::sscanf(s.c_str(), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", &d1, &d2, &d3, &b[0], &b[1], &b[2],
                  &b[3], &b[4], &b[5], &b[6], &b[7]) != 11)
    return false;
  out.Data1 = static_cast<unsigned long>(d1);
  out.Data2 = static_cast<unsigned short>(d2);
  out.Data3 = static_cast<unsigned short>(d3);
  for (int i = 0; i < 8; ++i) out.Data4[i] = static_cast<unsigned char>(b[i]);
  return true;
}
#else
using socklen_type = socklen_t;
using buf_ptr = void*;
using cbuf_ptr = const void*;

void close_native(native_socket s) noexcept { ::close(s); }
#endif

// select() on one socket for readability. Returns 1 readable, 0 timeout, <0 error.
int wait_read(native_socket s, std::chrono::milliseconds timeout) noexcept {
#ifdef _WIN32
  const SOCKET fd = static_cast<SOCKET>(s);
#else
  const int fd = s;
#endif
  fd_set set;
  FD_ZERO(&set);
  FD_SET(fd, &set);
  timeval tv{};
  tv.tv_sec = static_cast<long>(timeout.count() / 1000);
  tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
#ifdef _WIN32
  return ::select(0, &set, nullptr, nullptr, &tv);
#else
  return ::select(fd + 1, &set, nullptr, nullptr, &tv);
#endif
}

bool parse_u32(std::string_view s, std::uint32_t& out) noexcept {
  if (s.empty()) return false;
  auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
  return ec == std::errc{} && p == s.data() + s.size();
}

}  // namespace

void init_network() {
#ifdef _WIN32
  static std::once_flag once;
  std::call_once(once, [] {
    WSADATA data;
    (void)::WSAStartup(MAKEWORD(2, 2), &data);
  });
#endif
}

int last_socket_error() noexcept {
#ifdef _WIN32
  return ::WSAGetLastError();
#else
  return errno;
#endif
}

// --- Endpoint ------------------------------------------------------------------

Result<Endpoint> Endpoint::parse(std::string_view text) {
  Endpoint ep;
  const auto scheme_end = text.find("://");
  if (scheme_end == std::string_view::npos) return fail(Errc::InvalidArgument);
  const std::string_view scheme = text.substr(0, scheme_end);
  std::string_view rest = text.substr(scheme_end + 3);
  if (rest.empty()) return fail(Errc::InvalidArgument);

  if (scheme == "tcp") {
    ep.kind = Kind::Tcp;
    const auto colon = rest.rfind(':');
    if (colon == std::string_view::npos || colon == 0) return fail(Errc::InvalidArgument);
    ep.host = std::string(rest.substr(0, colon));
    if (ep.host.size() >= 2 && ep.host.front() == '[' && ep.host.back() == ']')
      ep.host = ep.host.substr(1, ep.host.size() - 2);
    if (!parse_u32(rest.substr(colon + 1), ep.port) || ep.port > 65535) return fail(Errc::InvalidArgument);
    return ep;
  }
  if (scheme == "vsock") {
    ep.kind = Kind::Vsock;
    const auto colon = rest.find(':');
    if (colon == std::string_view::npos) return fail(Errc::InvalidArgument);
    const std::string_view cid = rest.substr(0, colon);
    if (cid == "host")
      ep.cid = 2;
    else if (cid == "any")
      ep.cid = 0xFFFFFFFFu;
    else if (!parse_u32(cid, ep.cid))
      return fail(Errc::InvalidArgument);
    if (!parse_u32(rest.substr(colon + 1), ep.port)) return fail(Errc::InvalidArgument);
    return ep;
  }
  if (scheme == "hv") {
    ep.kind = Kind::HyperV;
    const auto colon = rest.rfind(':');
    if (colon == std::string_view::npos) {
      if (!parse_u32(rest, ep.port)) return fail(Errc::InvalidArgument);
      return ep;
    }
    ep.vm_id = std::string(rest.substr(0, colon));
    if (!parse_u32(rest.substr(colon + 1), ep.port)) return fail(Errc::InvalidArgument);
    return ep;
  }
  return fail(Errc::InvalidArgument);
}

std::string Endpoint::to_string() const {
  switch (kind) {
    case Kind::Tcp: return "tcp://" + (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
    case Kind::Vsock: return "vsock://" + (cid == 2 ? std::string("host") : cid == 0xFFFFFFFFu ? std::string("any") : std::to_string(cid)) + ":" + std::to_string(port);
    case Kind::HyperV: return "hv://" + (vm_id.empty() ? std::string() : vm_id + ":") + std::to_string(port);
  }
  return {};
}

// --- Socket -----------------------------------------------------------------------

Socket& Socket::operator=(Socket&& o) noexcept {
  if (this != &o) {
    close();
    s_ = o.s_;
    o.s_ = kInvalidSocket;
  }
  return *this;
}

Result<void> Socket::send_all(std::span<const std::byte> data) noexcept {
  while (!data.empty()) {
#ifdef _WIN32
    const int chunk = static_cast<int>(std::min<std::size_t>(data.size(), 1u << 30));
    const int n = ::send(static_cast<SOCKET>(s_), reinterpret_cast<cbuf_ptr>(data.data()), chunk, 0);
    if (n == SOCKET_ERROR) return fail(Errc::IoError);
#else
    const ssize_t n = ::send(s_, data.data(), data.size(), MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      return fail(errno == EPIPE || errno == ECONNRESET ? Errc::ConnectionClosed : Errc::IoError);
    }
#endif
    data = data.subspan(static_cast<std::size_t>(n));
  }
  return {};
}

Result<std::size_t> Socket::recv_some(std::span<std::byte> buf) noexcept {
  if (buf.empty()) return std::size_t{0};
#ifdef _WIN32
  const int chunk = static_cast<int>(std::min<std::size_t>(buf.size(), 1u << 30));
  const int n = ::recv(static_cast<SOCKET>(s_), reinterpret_cast<buf_ptr>(buf.data()), chunk, 0);
  if (n == SOCKET_ERROR) {
    const int err = ::WSAGetLastError();
    return fail(err == WSAECONNRESET || err == WSAECONNABORTED || err == WSAESHUTDOWN ? Errc::ConnectionClosed
                                                                                        : Errc::IoError);
  }
#else
  ssize_t n;
  do {
    n = ::recv(s_, buf.data(), buf.size(), 0);
  } while (n < 0 && errno == EINTR);
  if (n < 0) return fail(errno == ECONNRESET ? Errc::ConnectionClosed : Errc::IoError);
#endif
  if (n == 0) return fail(Errc::ConnectionClosed);
  return static_cast<std::size_t>(n);
}

Result<void> Socket::recv_exact(std::span<std::byte> buf) noexcept {
  while (!buf.empty()) {
    auto n = recv_some(buf);
    if (!n) return fail(n.error());
    buf = buf.subspan(*n);
  }
  return {};
}

Result<bool> Socket::wait_readable(std::chrono::milliseconds timeout) noexcept {
  const int n = wait_read(s_, timeout);
  if (n > 0) return true;
  if (n == 0) return false;
  return fail(Errc::ConnectionClosed);
}

void Socket::set_nodelay(bool on) noexcept {
  const int v = on ? 1 : 0;
#ifdef _WIN32
  (void)::setsockopt(static_cast<SOCKET>(s_), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&v), sizeof v);
#else
  (void)::setsockopt(s_, IPPROTO_TCP, TCP_NODELAY, &v, sizeof v);
#endif
}

void Socket::shutdown() noexcept {
  if (!valid()) return;
#ifdef _WIN32
  (void)::shutdown(static_cast<SOCKET>(s_), SD_BOTH);
#else
  (void)::shutdown(s_, SHUT_RDWR);
#endif
}

void Socket::close() noexcept {
  if (valid()) {
    close_native(s_);
    s_ = kInvalidSocket;
  }
}

// --- connect ------------------------------------------------------------------------

Result<Socket> connect(const Endpoint& ep) {
  init_network();
  switch (ep.kind) {
    case Endpoint::Kind::Tcp: {
      addrinfo hints{};
      hints.ai_family = AF_UNSPEC;
      hints.ai_socktype = SOCK_STREAM;
      hints.ai_protocol = IPPROTO_TCP;
      addrinfo* res = nullptr;
      const std::string port = std::to_string(ep.port);
      if (::getaddrinfo(ep.host.c_str(), port.c_str(), &hints, &res) != 0 || res == nullptr) return fail(Errc::NotFound);
      Socket out;
      for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        const native_socket s = static_cast<native_socket>(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (s == kInvalidSocket) continue;
        Socket candidate(s);
        if (::connect(static_cast<decltype(::socket(0, 0, 0))>(s), ai->ai_addr, static_cast<socklen_type>(ai->ai_addrlen)) == 0) {
          out = std::move(candidate);
          break;
        }
      }
      ::freeaddrinfo(res);
      if (!out.valid()) return fail(Errc::IoError);
      out.set_nodelay(true);
      return out;
    }
    case Endpoint::Kind::Vsock: {
#ifdef _WIN32
      return fail(Errc::Unsupported);
#else
      const native_socket s = ::socket(AF_VSOCK, SOCK_STREAM, 0);
      if (s == kInvalidSocket) return fail(Errc::Unsupported);
      Socket out(s);
      sockaddr_vm addr{};
      addr.svm_family = AF_VSOCK;
      addr.svm_cid = ep.cid;
      addr.svm_port = ep.port;
      if (::connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) return fail(Errc::IoError);
      return out;
#endif
    }
    case Endpoint::Kind::HyperV: {
#ifdef _WIN32
      SOCKADDR_HV addr{};
      addr.Family = AF_HYPERV;
      addr.ServiceId = vsock_service_id(ep.port);
      if (ep.vm_id.empty() || !parse_guid(ep.vm_id, addr.VmId)) return fail(Errc::InvalidArgument);
      const SOCKET s = ::socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
      if (s == INVALID_SOCKET) return fail(Errc::Unsupported);
      Socket out(static_cast<native_socket>(s));
      if (::connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) return fail(Errc::IoError);
      return out;
#else
      return fail(Errc::Unsupported);
#endif
    }
  }
  return fail(Errc::InvalidArgument);
}

// --- Listener -----------------------------------------------------------------------

Listener& Listener::operator=(Listener&& o) noexcept {
  if (this != &o) {
    close();
    s_ = o.s_;
    local_ = std::move(o.local_);
    o.s_ = kInvalidSocket;
  }
  return *this;
}

Result<Listener> Listener::bind(const Endpoint& ep, int backlog) {
  init_network();
  Listener l;
  l.local_ = ep;
  switch (ep.kind) {
    case Endpoint::Kind::Tcp: {
      addrinfo hints{};
      hints.ai_family = AF_UNSPEC;
      hints.ai_socktype = SOCK_STREAM;
      hints.ai_protocol = IPPROTO_TCP;
      hints.ai_flags = AI_PASSIVE;
      addrinfo* res = nullptr;
      const std::string port = std::to_string(ep.port);
      if (::getaddrinfo(ep.host.empty() ? nullptr : ep.host.c_str(), port.c_str(), &hints, &res) != 0 || res == nullptr)
        return fail(Errc::NotFound);
      for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        const auto s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (static_cast<native_socket>(s) == kInvalidSocket) continue;
#ifndef _WIN32
        const int one = 1;
        (void)::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
        if (::bind(s, ai->ai_addr, static_cast<socklen_type>(ai->ai_addrlen)) == 0 && ::listen(s, backlog) == 0) {
          l.s_ = static_cast<native_socket>(s);
          sockaddr_storage bound{};
          socklen_type len = sizeof bound;
          if (::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
            if (bound.ss_family == AF_INET)
              l.local_.port = ntohs(reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
            else if (bound.ss_family == AF_INET6)
              l.local_.port = ntohs(reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port);
          }
          break;
        }
        close_native(static_cast<native_socket>(s));
      }
      ::freeaddrinfo(res);
      if (!l.valid()) return fail(Errc::IoError);
      return l;
    }
    case Endpoint::Kind::Vsock: {
#ifdef _WIN32
      return fail(Errc::Unsupported);
#else
      const native_socket s = ::socket(AF_VSOCK, SOCK_STREAM, 0);
      if (s == kInvalidSocket) return fail(Errc::Unsupported);
      sockaddr_vm addr{};
      addr.svm_family = AF_VSOCK;
      addr.svm_cid = ep.cid == 0 ? VMADDR_CID_ANY : ep.cid;
      addr.svm_port = ep.port;
      if (::bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0 || ::listen(s, backlog) != 0) {
        close_native(s);
        return fail(Errc::IoError);
      }
      l.s_ = s;
      return l;
#endif
    }
    case Endpoint::Kind::HyperV: {
#ifdef _WIN32
      SOCKADDR_HV addr{};
      addr.Family = AF_HYPERV;
      addr.ServiceId = vsock_service_id(ep.port);
      addr.VmId = kHvGuidWildcard;
      if (!ep.vm_id.empty() && !parse_guid(ep.vm_id, addr.VmId)) return fail(Errc::InvalidArgument);
      const SOCKET s = ::socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
      if (s == INVALID_SOCKET) return fail(Errc::Unsupported);
      if (::bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0 || ::listen(s, backlog) != 0) {
        ::closesocket(s);
        return fail(Errc::IoError);
      }
      l.s_ = static_cast<native_socket>(s);
      return l;
#else
      return fail(Errc::Unsupported);
#endif
    }
  }
  return fail(Errc::InvalidArgument);
}

Result<Socket> Listener::accept(std::chrono::milliseconds timeout) {
  if (!valid()) return fail(Errc::ConnectionClosed);
  const int n = wait_read(s_, timeout);
  if (n == 0) return fail(Errc::Timeout);
  if (n < 0) return fail(Errc::ConnectionClosed);
  return accept();
}

Result<Socket> Listener::accept() {
  if (!valid()) return fail(Errc::ConnectionClosed);
#ifdef _WIN32
  const SOCKET s = ::accept(static_cast<SOCKET>(s_), nullptr, nullptr);
  if (s == INVALID_SOCKET) return fail(::WSAGetLastError() == WSAEINTR ? Errc::ConnectionClosed : Errc::IoError);
  Socket out(static_cast<native_socket>(s));
#else
  int s;
  do {
    s = ::accept(s_, nullptr, nullptr);
  } while (s < 0 && errno == EINTR);
  if (s < 0) return fail(errno == EBADF || errno == EINVAL ? Errc::ConnectionClosed : Errc::IoError);
  Socket out(s);
#endif
  if (local_.kind == Endpoint::Kind::Tcp) out.set_nodelay(true);
  return out;
}

void Listener::close() noexcept {
  if (valid()) {
    close_native(s_);
    s_ = kInvalidSocket;
  }
}

}  // namespace wsld::net
