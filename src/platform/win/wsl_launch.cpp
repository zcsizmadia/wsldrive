#include "platform/win/wsl_launch.hpp"

#include "platform/win/wide.hpp"

#include <windows.h>

#include <string>

namespace wsld::platform {

namespace {

// Quotes an argument for a POSIX shell (the distro side runs the command).
std::string sh_quote(std::string_view s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'')
      out += "'\\''";
    else
      out.push_back(c);
  }
  out.push_back('\'');
  return out;
}

}  // namespace

std::string build_wsl_command(const WslAgentSpec& spec) {
  const std::string agent = spec.agent_path.empty() ? "wsldrived" : spec.agent_path;
  std::string cmd = "wsl.exe";
  if (!spec.distro.empty()) cmd += " -d " + spec.distro;
  // `--` ends wsl.exe option parsing; everything after runs in the distro.
  cmd += " -- ";
  cmd += sh_quote(agent);
  cmd += " --root " + sh_quote(spec.wsl_root);
  cmd += " --listen tcp://127.0.0.1:" + std::to_string(spec.port);
  cmd += " --exit-when-idle";  // agent terminates when this client's socket closes
  return cmd;
}

WslAgent::~WslAgent() { stop(); }

Result<void> WslAgent::start(const WslAgentSpec& spec) {
  spec_ = spec;
  std::wstring cmd = win::to_wide(build_wsl_command(spec));
  cmd.push_back(L'\0');  // CreateProcessW may modify the buffer

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  const BOOL ok = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                                   &si, &pi);
  if (!ok) return fail(Errc::IoError);
  ::CloseHandle(pi.hThread);
  proc_ = pi.hProcess;
  return {};
}

void WslAgent::stop() {
  if (proc_ == nullptr) return;
  // Best-effort: terminate the agent inside the distro (matched by its unique
  // listen port), then the wsl.exe client.
  const std::string kill = "wsl.exe" + (spec_.distro.empty() ? std::string() : " -d " + spec_.distro) +
                           " -- pkill -f " + [&] {
                             std::string pat = "wsldrived.*127.0.0.1:" + std::to_string(spec_.port);
                             std::string q = "'";
                             for (char c : pat) q += (c == '\'') ? std::string("'\\''") : std::string(1, c);
                             q.push_back('\'');
                             return q;
                           }();
  std::wstring wkill = win::to_wide(kill);
  wkill.push_back(L'\0');
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (::CreateProcessW(nullptr, wkill.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    ::WaitForSingleObject(pi.hProcess, 3000);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
  }
  ::TerminateProcess(static_cast<HANDLE>(proc_), 0);
  ::WaitForSingleObject(static_cast<HANDLE>(proc_), 2000);
  ::CloseHandle(static_cast<HANDLE>(proc_));
  proc_ = nullptr;
}

}  // namespace wsld::platform
