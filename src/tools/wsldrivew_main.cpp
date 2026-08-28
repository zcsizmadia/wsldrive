// wsldrivew.exe — a windowless launcher (a GUI-subsystem app, like pythonw.exe).
//
// The scheduled at-logon mount task runs the real console command through this so
// no console window appears at logon. It launches whatever command line it is
// given with CREATE_NO_WINDOW, waits for it, and propagates the exit code (so the
// task still tracks the mount and its retry-on-failure works). wsldrive.exe stays
// a normal console CLI; only the background auto-start goes through here.
//
// Usage:  wsldrivew.exe <command> [args...]
//   e.g.  wsldrivew.exe "C:\Program Files\wsldrive\wsldrive.exe" mount W: --distro Ubuntu ...
//         wsldrivew.exe wsl.exe -d Ubuntu -- bash -lc "..."

#include <windows.h>

#include <string>
#include <vector>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR lpCmdLine, int) {
  if (lpCmdLine == nullptr || lpCmdLine[0] == L'\0') return 2;  // nothing to run

  std::wstring cmd = lpCmdLine;               // the full command line to launch
  std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
  mutable_cmd.push_back(L'\0');               // CreateProcessW may modify the buffer

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!::CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &pi)) {
    return 1;
  }
  ::CloseHandle(pi.hThread);
  ::WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 0;
  ::GetExitCodeProcess(pi.hProcess, &code);
  ::CloseHandle(pi.hProcess);
  return static_cast<int>(code);
}
