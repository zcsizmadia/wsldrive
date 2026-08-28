#pragma once

#include <cstdint>
#include <expected>

namespace wsld {

enum class Errc : std::uint8_t {
  NotFound = 1,
  AlreadyExists,
  NotADirectory,
  IsADirectory,
  InvalidArgument,
  InvalidPath,
  Truncated,           // buffer ended before a complete value could be read
  BadMagic,            // frame did not start with the protocol magic
  UnsupportedVersion,  // peer speaks a protocol version we do not understand
  Corrupt,             // structurally invalid data (bad indices, bad varint, ...)
  IoError,
  TooLarge,
  ConnectionClosed,  // peer closed the stream cleanly
  Unsupported,       // feature not available on this platform / build
  Timeout,
  ProtocolError,     // peer sent a well-formed frame we did not expect
};

constexpr const char* to_string(Errc e) noexcept {
  switch (e) {
    case Errc::NotFound: return "not found";
    case Errc::AlreadyExists: return "already exists";
    case Errc::NotADirectory: return "not a directory";
    case Errc::IsADirectory: return "is a directory";
    case Errc::InvalidArgument: return "invalid argument";
    case Errc::InvalidPath: return "invalid path";
    case Errc::Truncated: return "truncated";
    case Errc::BadMagic: return "bad magic";
    case Errc::UnsupportedVersion: return "unsupported version";
    case Errc::Corrupt: return "corrupt";
    case Errc::IoError: return "i/o error";
    case Errc::TooLarge: return "too large";
    case Errc::ConnectionClosed: return "connection closed";
    case Errc::Unsupported: return "unsupported";
    case Errc::Timeout: return "timeout";
    case Errc::ProtocolError: return "protocol error";
  }
  return "unknown";
}

template <class T>
using Result = std::expected<T, Errc>;

[[nodiscard]] inline std::unexpected<Errc> fail(Errc e) noexcept { return std::unexpected(e); }

}  // namespace wsld
