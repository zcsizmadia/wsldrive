#include "platform/win/wsl_launch.hpp"

#include "platform/win/wide.hpp"

#include <windows.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>

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

namespace {
// Extracts the first GUID-shaped token (8-4-4-4-12 hex) from a string.
std::string first_guid(std::string_view s) {
  auto is_hex = [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  };
  for (std::size_t i = 0; i + 36 <= s.size(); ++i) {
    bool ok = true;
    for (std::size_t j = 0; j < 36 && ok; ++j) {
      const char c = s[i + j];
      if (j == 8 || j == 13 || j == 18 || j == 23)
        ok = c == '-';
      else
        ok = is_hex(c);
    }
    if (ok) return std::string(s.substr(i, 36));
  }
  return {};
}
}  // namespace

std::string discover_wsl_vm_guid() {
  std::FILE* pipe = _popen("hcsdiag list 2>&1", "r");
  if (pipe == nullptr) return {};
  std::string out;
  char buf[512];
  while (std::fgets(buf, sizeof(buf), pipe) != nullptr) out += buf;
  _pclose(pipe);
  // hcsdiag prints one compute system per block; the WSL utility VM's line
  // contains "WSL". Prefer a line mentioning WSL; fall back to the first GUID.
  std::size_t start = 0;
  std::string fallback;
  for (std::size_t i = 0; i <= out.size(); ++i) {
    if (i == out.size() || out[i] == '\n') {
      const std::string_view line(out.data() + start, i - start);
      const std::string g = first_guid(line);
      if (!g.empty()) {
        if (line.find("WSL") != std::string_view::npos) return g;
        if (fallback.empty()) fallback = g;
      }
      start = i + 1;
    }
  }
  return fallback;
}

std::string build_wsl_command(const WslAgentSpec& spec) {
  const std::string agent = spec.agent_path.empty() ? "wsldrived" : spec.agent_path;
  std::string cmd = "wsl.exe";
  if (!spec.distro.empty()) cmd += " -d " + spec.distro;
  // `--` ends wsl.exe option parsing; everything after runs in the distro.
  cmd += " -- ";
  cmd += sh_quote(agent);
  cmd += " --root " + sh_quote(spec.wsl_root);
  const std::string listen = spec.listen.empty() ? ("tcp://127.0.0.1:" + std::to_string(spec.port)) : spec.listen;
  cmd += " --listen " + listen;
  cmd += " --exit-when-idle";  // agent terminates when this client's socket closes
  return cmd;
}

namespace {
// True only when `wsl -l -q` succeeds AND does not list `distro` — i.e. the distro
// is definitely not installed (as opposed to merely not started yet). Returns
// false on any uncertainty (couldn't enumerate), so callers never fast-fail on a
// transient early-boot hiccup.
bool distro_definitely_absent(const std::string& distro) {
  if (distro.empty()) return false;  // default distro: cannot name-check
  ::_putenv_s("WSL_UTF8", "1");      // make wsl.exe emit UTF-8, not UTF-16
  std::FILE* pipe = ::_popen("wsl.exe -l -q 2>NUL", "r");
  if (pipe == nullptr) return false;
  std::string out;
  char buf[512];
  while (std::fgets(buf, sizeof(buf), pipe) != nullptr) out += buf;
  const int rc = ::_pclose(pipe);
  if (rc != 0 || out.empty()) return false;  // could not enumerate -> unknown
  if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF) out.erase(0, 3);  // strip UTF-8 BOM
  auto lower = [](std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  const std::string want = lower(distro);
  std::size_t start = 0;
  for (std::size_t i = 0; i <= out.size(); ++i) {
    if (i == out.size() || out[i] == '\n') {
      std::string line(out, start, i - start);
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\0')) line.pop_back();
      std::size_t b = 0;
      while (b < line.size() && line[b] == ' ') ++b;
      if (lower(line.substr(b)) == want) return false;  // present
      start = i + 1;
    }
  }
  return true;  // enumerated fine, distro not among them
}
}  // namespace

bool wait_wsl_ready(const std::string& distro, std::chrono::milliseconds timeout) {
  if (distro_definitely_absent(distro)) return false;  // fast-fail: not installed
  std::string cmd = "wsl.exe";
  if (!distro.empty()) cmd += " -d " + distro;
  cmd += " -e true";  // succeeds (exit 0) only once the distro is actually up
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    std::wstring wcmd = win::to_wide(cmd);
    wcmd.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    DWORD code = 1;
    if (::CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
      ::WaitForSingleObject(pi.hProcess, 20000);
      ::GetExitCodeProcess(pi.hProcess, &code);
      ::CloseHandle(pi.hProcess);
      ::CloseHandle(pi.hThread);
    }
    if (code == 0) return true;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
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
                             std::string pat = "wsldrived.*:" + std::to_string(spec_.port);
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
