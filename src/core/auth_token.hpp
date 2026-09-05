#pragma once

#include <string>
#include <string_view>

namespace wsld {

/// Name of the environment variable carrying the per-mount shared secret. The
/// launcher generates a token, puts it in the environment of both the agent and
/// the client, and each presents/checks it during the Hello handshake. The
/// environment is used rather than argv because a command line is world-readable
/// (`/proc/<pid>/cmdline`, the Windows process list).
inline constexpr const char* kAuthTokenEnv = "WSLDRIVE_TOKEN";

/// A fresh 128-bit token as lowercase hex, from the platform CSPRNG
/// (BCryptGenRandom / getrandom). Returns "" if no entropy source is available,
/// which callers must treat as a failure rather than as "no authentication".
[[nodiscard]] std::string generate_auth_token();

/// Reads the token from the environment ("" when unset).
[[nodiscard]] std::string auth_token_from_env();

/// A fresh 128-bit nonce as lowercase hex, for one handshake. Same source as
/// generate_auth_token(); named separately because a nonce is public and a
/// token is not.
[[nodiscard]] std::string generate_auth_nonce();

/// Proof that the sender knows `token`, for one handshake.
///
/// The token itself never goes on the wire. Both nonces are mixed in, so a
/// proof is useless in any other session, and the side computing it is mixed in
/// too, so the server's proof cannot be replayed back as the client's.
[[nodiscard]] std::string auth_proof(std::string_view token, std::string_view client_nonce,
                                     std::string_view server_nonce, bool from_server);

/// Compares two secrets (or values derived from them) without leaking where
/// they first differ through timing.
[[nodiscard]] bool constant_time_equal(std::string_view a, std::string_view b) noexcept;

}  // namespace wsld
