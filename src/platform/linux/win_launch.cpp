#include "platform/linux/win_launch.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace wsld::platform {

namespace {
std::string first_guid(std::string_view s) {
  auto is_hex = [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  };
  for (std::size_t i = 0; i + 36 <= s.size(); ++i) {
    bool ok = true;
    for (std::size_t j = 0; j < 36 && ok; ++j) {
      const char c = s[i + j];
      ok = (j == 8 || j == 13 || j == 18 || j == 23) ? (c == '-') : is_hex(c);
    }
    if (ok) return std::string(s.substr(i, 36));
  }
  return {};
}
}  // namespace

std::string build_win_agent_command(const WinAgentSpec& spec) {
  return spec.exe + " --root " + spec.win_root + " --connect " + spec.connect + " --exit-when-idle";
}

std::string discover_wsl_vm_guid() {
  std::FILE* p = ::popen("hcsdiag.exe list 2>/dev/null", "r");
  if (p == nullptr) return {};
  std::string out;
  char buf[512];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
  ::pclose(p);
  std::size_t start = 0;
  std::string fallback;
  for (std::size_t i = 0; i <= out.size(); ++i) {
    if (i == out.size() || out[i] == '\n') {
      const std::string_view line(out.data() + start, i - start);
      if (const std::string g = first_guid(line); !g.empty()) {
        if (line.find("WSL") != std::string_view::npos) return g;
        if (fallback.empty()) fallback = g;
      }
      start = i + 1;
    }
  }
  return fallback;
}

WinAgent::~WinAgent() { stop(); }

Result<void> WinAgent::start(const WinAgentSpec& spec) {
  const pid_t pid = ::fork();
  if (pid < 0) return fail(Errc::IoError);
  if (pid == 0) {
    // Child: run the Windows agent via WSL interop (binfmt routes the .exe).
    // The token goes in the environment, not argv: a command line is readable by
    // any local process. WSLENV forwards it across the interop boundary.
    if (!spec.token.empty()) {
      ::setenv("WSLDRIVE_TOKEN", spec.token.c_str(), 1);
      const char* existing = ::getenv("WSLENV");
      std::string wslenv = existing != nullptr ? std::string(existing) : std::string();
      if (wslenv.find("WSLDRIVE_TOKEN") == std::string::npos) {
        if (!wslenv.empty()) wslenv.push_back(':');
        wslenv += "WSLDRIVE_TOKEN/w";  // /w: pass from WSL to the Windows process
      }
      ::setenv("WSLENV", wslenv.c_str(), 1);
    }
    ::execl(spec.exe.c_str(), spec.exe.c_str(), "--root", spec.win_root.c_str(), "--connect", spec.connect.c_str(),
            "--exit-when-idle", static_cast<char*>(nullptr));
    _exit(127);
  }
  pid_ = pid;
  return {};
}

void WinAgent::stop() {
  if (pid_ <= 0) return;
  ::kill(pid_, SIGTERM);
  int status = 0;
  ::waitpid(pid_, &status, 0);
  pid_ = -1;
}

}  // namespace wsld::platform
