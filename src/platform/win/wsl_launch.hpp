#pragma once

#include "core/error.hpp"

#include <chrono>
#include <cstdint>
#include <string>

namespace wsld::platform {

/// How to launch the Linux agent inside a WSL2 distro.
struct WslAgentSpec {
  std::string distro;      // e.g. "Ubuntu"; empty = the default distro
  std::string agent_path;  // path to wsldrived inside the distro (default: "wsldrived" on PATH)
  std::string wsl_root;    // the ext4 directory to serve
  std::uint32_t port = 0;  // port the agent listens on
  std::string listen;      // full listen endpoint; empty = tcp://127.0.0.1:<port>
  // Per-mount shared secret. Passed through the environment (forwarded into the
  // distro via WSLENV) rather than argv, which is world-readable.
  std::string token;
};

/// Best-effort discovery of the running WSL2 utility VM's GUID (via `hcsdiag
/// list`). Returns "" if it cannot be determined (e.g. not elevated / not a
/// Hyper-V administrator). Needed to reach the guest over AF_HYPERV.
[[nodiscard]] std::string discover_wsl_vm_guid();

/// Builds the `wsl.exe ...` command line that launches the agent. Pure, so the
/// argument construction can be unit-tested without spawning anything.
[[nodiscard]] std::string build_wsl_command(const WslAgentSpec& spec);

/// Waits until the distro answers `wsl.exe -e true` (i.e. the utility VM and the
/// distro have started), retrying until success or `timeout`. Returns true once
/// ready. WSL2 starts lazily on first invocation, so at logon after a reboot the
/// VM may not be up yet — calling this before launching the agent closes that
/// cold-boot race. `distro` empty = the default distro.
[[nodiscard]] bool wait_wsl_ready(const std::string& distro, std::chrono::milliseconds timeout);

/// Launches and supervises `wsldrived` inside a distro via wsl.exe. Destruction
/// (or stop()) terminates the agent and the wsl.exe client.
class WslAgent {
 public:
  WslAgent() = default;
  ~WslAgent();
  WslAgent(const WslAgent&) = delete;
  WslAgent& operator=(const WslAgent&) = delete;

  /// Spawns the agent. Does not wait for it to be listening — the caller polls
  /// the port (the agent binds loopback, reachable from Windows via WSL
  /// localhost forwarding).
  [[nodiscard]] Result<void> start(const WslAgentSpec& spec);
  void stop();
  [[nodiscard]] bool running() const noexcept { return proc_ != nullptr; }

 private:
  void* proc_ = nullptr;  // HANDLE to the wsl.exe process
  WslAgentSpec spec_;
};

}  // namespace wsld::platform
