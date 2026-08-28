#pragma once

#include "core/error.hpp"

#include <string>

namespace wsld::platform {

/// How to launch the Windows agent (wsldrived.exe) from inside WSL via interop,
/// for Direction B (WSL mounts a Windows path). The agent serves a Windows path
/// and dials back to this WSL client's listener.
struct WinAgentSpec {
  std::string exe;      // Linux-visible path to wsldrived.exe, e.g. /mnt/c/.../wsldrived.exe
  std::string win_root; // the Windows path to serve, e.g. C:\project
  std::string connect;  // endpoint the agent connects to (this client's listener)
};

/// Builds the argv (space-joined, for logging/tests) used to launch the agent.
/// Pure — no process is spawned.
[[nodiscard]] std::string build_win_agent_command(const WinAgentSpec& spec);

/// Best-effort discovery of the running WSL2 VM's GUID from inside WSL, by
/// running `hcsdiag.exe list` over interop. Returns "" if unavailable (e.g. the
/// Windows side is not a Hyper-V administrator). AF_HYPERV needs this GUID.
[[nodiscard]] std::string discover_wsl_vm_guid();

/// Launches and supervises the Windows agent via interop.
class WinAgent {
 public:
  WinAgent() = default;
  ~WinAgent();
  WinAgent(const WinAgent&) = delete;
  WinAgent& operator=(const WinAgent&) = delete;

  [[nodiscard]] Result<void> start(const WinAgentSpec& spec);
  void stop();

 private:
  int pid_ = -1;
};

}  // namespace wsld::platform
