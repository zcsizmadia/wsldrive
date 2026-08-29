#pragma once

#include <string>

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

}  // namespace wsld
