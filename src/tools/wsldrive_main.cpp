// wsldrive: client CLI. For now a measurement tool for the transport + agent
// layer; the WinFsp mount front-end lands on top of RemoteRoot.
//
//   wsldrive fetch --connect tcp://127.0.0.1:7788 [--watch] [--read <path>] [--lookups N]
//   wsldrive fetch --listen hv://7788          (wait for the WSL2 agent to dial in)
#include "agent/client.hpp"
#include "net/frame_channel.hpp"
#include "net/socket.hpp"

#ifdef _WIN32
#include <windows.h>
#endif
#ifdef WSLDRIVE_HAVE_MOUNT
#include "mount/fuse_mount.hpp"
#include <atomic>
#include <filesystem>
#endif
#ifdef WSLDRIVE_HAVE_WINFSP
#include "platform/win/wsl_launch.hpp"
#endif
#if defined(WSLDRIVE_HAVE_MOUNT) && !defined(_WIN32)
#include <csignal>
#endif

#include <chrono>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

void usage() {
  std::fputs(
      "usage:\n"
      "  wsldrive fetch (--connect <endpoint> | --listen <endpoint>) [--watch] [--read <path>] [--lookups N]\n"
#ifdef _WIN32
      "  wsldrive doctor                                      (check WinFsp + WSL environment)\n"
#endif
#ifdef WSLDRIVE_HAVE_MOUNT
      "  wsldrive mount <mountpoint> --connect <endpoint>      (attach to a running agent)\n"
#endif
#ifdef WSLDRIVE_HAVE_WINFSP
      "  wsldrive mount <drive> --distro <name> --wsl-root <path> [--agent <path>] [--port N]\n"
      "                                                       (auto-launch the agent in the distro)\n"
#endif
      ,
      stderr);
}

double ms(std::chrono::nanoseconds d) { return std::chrono::duration<double, std::milli>(d).count(); }

#ifdef WSLDRIVE_HAVE_MOUNT
std::atomic<bool> g_mount_stop{false};
#ifdef _WIN32
BOOL WINAPI mount_ctrl_handler(DWORD type) {
  if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
    g_mount_stop.store(true);
    return TRUE;  // handled: don't let the default handler kill us before cleanup
  }
  return FALSE;
}
#else
extern "C" void mount_signal_handler(int) { g_mount_stop.store(true); }
#endif
void install_mount_signal_handler() {
#ifdef _WIN32
  ::SetConsoleCtrlHandler(mount_ctrl_handler, TRUE);
#else
  std::signal(SIGINT, mount_signal_handler);
  std::signal(SIGTERM, mount_signal_handler);
#endif
}
#endif

}  // namespace

int main(int argc, char** argv) {
  const std::string_view command = argc >= 2 ? std::string_view(argv[1]) : std::string_view{};

#ifdef _WIN32
  if (command == "doctor") {
    int problems = 0;
    // WinFsp: present at build time?
#ifdef WSLDRIVE_HAVE_WINFSP
    std::printf("[ok]   wsldrive was built with WinFsp support (mount available)\n");
#else
    std::printf("[warn] wsldrive was built without WinFsp; the mount command is unavailable\n");
    ++problems;
#endif
    // WinFsp runtime: registry InstallDir.
    {
      HKEY key{};
      wchar_t dir[MAX_PATH]{};
      DWORD size = sizeof(dir);
      bool found = false;
      if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\WinFsp", 0, KEY_READ | KEY_WOW64_32KEY, &key) ==
          ERROR_SUCCESS) {
        found = ::RegQueryValueExW(key, L"InstallDir", nullptr, nullptr, reinterpret_cast<LPBYTE>(dir), &size) ==
                ERROR_SUCCESS;
        ::RegCloseKey(key);
      }
      if (found)
        std::printf("[ok]   WinFsp runtime installed\n");
      else {
        std::printf("[fail] WinFsp runtime not found (install from https://winfsp.dev)\n");
        ++problems;
      }
    }
    // wsl.exe on PATH.
    {
      wchar_t path[MAX_PATH]{};
      if (::SearchPathW(nullptr, L"wsl.exe", nullptr, MAX_PATH, path, nullptr) > 0)
        std::printf("[ok]   wsl.exe found on PATH\n");
      else {
        std::printf("[fail] wsl.exe not found on PATH\n");
        ++problems;
      }
    }
    // WSL actually runs: `wsl.exe -e true`.
    {
      std::wstring cmd = L"wsl.exe -e true";
      cmd.push_back(L'\0');
      STARTUPINFOW si{};
      si.cb = sizeof(si);
      PROCESS_INFORMATION pi{};
      DWORD code = 1;
      if (::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        ::WaitForSingleObject(pi.hProcess, 10000);
        ::GetExitCodeProcess(pi.hProcess, &code);
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(pi.hThread);
      }
      if (code == 0)
        std::printf("[ok]   WSL is functional (ran a command in the default distro)\n");
      else {
        std::printf("[fail] could not run a command via WSL (exit %lu)\n", code);
        ++problems;
      }
    }
    std::printf("\n%s\n", problems == 0 ? "All checks passed. Mount with: wsldrive mount W: --distro <name> --wsl-root <path>"
                                        : "Some checks failed; resolve the items above before mounting.");
    return problems == 0 ? 0 : 1;
  }
#endif

#ifdef WSLDRIVE_HAVE_MOUNT
  if (command == "mount") {
    if (argc < 3) {
      usage();
      return 2;
    }
    const std::string mountpoint = argv[2];
    std::string connect, distro, wsl_root, agent_path;
    std::uint32_t port = 51789;
    bool have_distro = false;
    for (int i = 3; i < argc; ++i) {
      const std::string_view a = argv[i];
      auto val = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
      if (a == "--connect")
        connect = val();
      else if (a == "--distro") {
        distro = val();
        have_distro = true;
      } else if (a == "--wsl-root")
        wsl_root = val();
      else if (a == "--agent")
        agent_path = val();
      else if (a == "--port")
        port = static_cast<std::uint32_t>(std::stoul(val()));
      else {
        usage();
        return 2;
      }
    }

#ifdef WSLDRIVE_HAVE_WINFSP
    // Auto-launch mode (Windows only): start wsldrived in the distro, then connect.
    wsld::platform::WslAgent agent;
    if (have_distro) {
      if (wsl_root.empty()) {
        std::fprintf(stderr, "wsldrive: --wsl-root is required with --distro\n");
        return 2;
      }
      if (auto r = agent.start({.distro = distro, .agent_path = agent_path, .wsl_root = wsl_root, .port = port}); !r) {
        std::fprintf(stderr, "wsldrive: failed to launch agent in distro '%s'\n", distro.c_str());
        return 1;
      }
      connect = "tcp://127.0.0.1:" + std::to_string(port);
      std::printf("launched agent in %s, waiting for it to listen...\n", distro.empty() ? "(default distro)" : distro.c_str());
    } else
#endif
        if (connect.empty()) {
      usage();
      return 2;
    }

#ifndef _WIN32
    // Linux (Direction B): the FUSE mountpoint must exist as an empty directory.
    {
      std::error_code ec;
      std::filesystem::create_directories(mountpoint, ec);
    }
#endif

    auto ep = wsld::net::Endpoint::parse(connect);
    if (!ep) {
      std::fprintf(stderr, "wsldrive: bad endpoint '%s'\n", connect.c_str());
      return 2;
    }
    // Poll for the agent to accept connections (auto-launch needs a moment;
    // a direct --connect succeeds on the first try).
    (void)distro;
    (void)wsl_root;
    (void)agent_path;
    (void)port;
    wsld::Result<wsld::net::Socket> sock = wsld::fail(wsld::Errc::ConnectionClosed);
    const int attempts = have_distro ? 100 : 1;  // ~10 s when auto-launching
    (void)have_distro;
    for (int i = 0; i < attempts; ++i) {
      sock = wsld::net::connect(*ep);
      if (sock) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!sock) {
      std::fprintf(stderr, "wsldrive: connect failed: %s (os error %d)\n", wsld::to_string(sock.error()),
                   wsld::net::last_socket_error());
      return 1;
    }
    wsld::agent::RemoteRoot root(std::make_unique<wsld::net::FrameChannel>(std::move(*sock)));
    if (auto h = root.connect(); !h) {
      std::fprintf(stderr, "wsldrive: handshake failed: %s\n", wsld::to_string(h.error()));
      return 1;
    }
    if (auto r = root.fetch_snapshot(); !r) {
      std::fprintf(stderr, "wsldrive: snapshot failed: %s\n", wsld::to_string(r.error()));
      return 1;
    }
    const auto ts = root.with_tree([](const wsld::MetadataTree& t) { return t.stats(); });
    std::printf("mounting %s (%zu nodes) at %s ...\n", ep->to_string().c_str(), ts.nodes, mountpoint.c_str());
    wsld::mount::FuseMount fm(root);
    if (auto r = fm.mount(mountpoint); !r) {
      std::fprintf(stderr, "wsldrive: mount failed: %s\n", wsld::to_string(r.error()));
      return 1;
    }
    install_mount_signal_handler();
    std::printf("mounted. Ctrl+C to unmount.\n");
    std::fflush(stdout);
    while (fm.mounted() && root.connected() && !g_mount_stop.load())
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::printf("unmounting...\n");
    std::fflush(stdout);
    fm.unmount();  // agent (if auto-launched) is stopped by its destructor on return
    return 0;
  }
#endif

  if (command != "fetch") {
    usage();
    return 2;
  }
  std::string connect, listen, read_path;
  bool watch = false;
  long lookups = 0;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--connect")
      connect = next();
    else if (a == "--listen")
      listen = next();
    else if (a == "--watch")
      watch = true;
    else if (a == "--read")
      read_path = next();
    else if (a == "--lookups")
      lookups = std::stol(next());
    else {
      usage();
      return 2;
    }
  }
  if (connect.empty() == listen.empty()) {
    usage();
    return 2;
  }

  wsld::net::Socket sock;
  if (!connect.empty()) {
    auto ep = wsld::net::Endpoint::parse(connect);
    if (!ep) {
      std::fprintf(stderr, "wsldrive: bad endpoint '%s'\n", connect.c_str());
      return 2;
    }
    auto s = wsld::net::connect(*ep);
    if (!s) {
      std::fprintf(stderr, "wsldrive: connect failed: %s (os error %d)\n", wsld::to_string(s.error()),
                   wsld::net::last_socket_error());
      return 1;
    }
    sock = std::move(*s);
  } else {
    auto ep = wsld::net::Endpoint::parse(listen);
    if (!ep) {
      std::fprintf(stderr, "wsldrive: bad endpoint '%s'\n", listen.c_str());
      return 2;
    }
    auto l = wsld::net::Listener::bind(*ep);
    if (!l) {
      std::fprintf(stderr, "wsldrive: listen failed: %s (os error %d)\n", wsld::to_string(l.error()),
                   wsld::net::last_socket_error());
      return 1;
    }
    std::fprintf(stderr, "wsldrive: waiting for agent on %s\n", l->local().to_string().c_str());
    auto s = l->accept();
    if (!s) {
      std::fprintf(stderr, "wsldrive: accept failed\n");
      return 1;
    }
    sock = std::move(*s);
  }

  wsld::agent::RemoteRoot root(std::make_unique<wsld::net::FrameChannel>(std::move(sock)));
  auto hello = root.connect();
  if (!hello) {
    std::fprintf(stderr, "wsldrive: handshake failed: %s\n", wsld::to_string(hello.error()));
    return 1;
  }
  auto rtt = root.ping();
  std::printf("connected: protocol v%u, agent capabilities 0x%llx, ping %.3f ms\n", hello->protocol_version,
              static_cast<unsigned long long>(hello->capabilities), rtt ? ms(*rtt) : -1.0);

  if (auto r = root.fetch_snapshot(); !r) {
    std::fprintf(stderr, "wsldrive: snapshot failed: %s\n", wsld::to_string(r.error()));
    return 1;
  }
  const auto st = root.stats();
  const auto tree_stats = root.with_tree([](const wsld::MetadataTree& t) { return t.stats(); });
  std::printf("snapshot: %zu nodes, %zu distinct names (%zu bytes), %zu case collisions, %zu wire bytes, %.1f ms\n",
              tree_stats.nodes, tree_stats.names, tree_stats.name_bytes, tree_stats.case_collisions,
              st.snapshot_bytes, ms(st.last_snapshot_time));

  if (lookups > 0) {
    std::vector<std::string> paths;
    root.with_tree([&](const wsld::MetadataTree& t) {
      std::vector<wsld::NodeId> stack{t.root()};
      while (!stack.empty() && paths.size() < 200000) {
        const wsld::NodeId n = stack.back();
        stack.pop_back();
        t.for_each_child(n, [&](wsld::NodeId c) {
          if (t.node(c).is_dir())
            stack.push_back(c);
          else
            paths.push_back(t.path_of(c));
        });
      }
    });
    if (!paths.empty()) {
      std::mt19937 rng(42);
      std::vector<const std::string*> probes(4096);
      for (auto& p : probes) p = &paths[rng() % paths.size()];
      const auto t0 = std::chrono::steady_clock::now();
      std::size_t hits = 0;
      for (long i = 0; i < lookups; ++i)
        hits += root.with_tree([&](const wsld::MetadataTree& t) {
          return t.lookup(*probes[static_cast<std::size_t>(i) & 4095], wsld::LookupMode::CaseInsensitive).has_value();
        });
      const auto dt = std::chrono::steady_clock::now() - t0;
      std::printf("lookups: %ld case-insensitive path lookups under shared lock, %zu hits, %.1f ns each\n", lookups, hits,
                  static_cast<double>(dt.count()) / static_cast<double>(lookups));
    }
  }

  if (!read_path.empty()) {
    const auto t0 = std::chrono::steady_clock::now();
    auto data = root.read(read_path, 0, 16u << 20);
    const auto dt = std::chrono::steady_clock::now() - t0;
    if (!data)
      std::printf("read %s: %s\n", read_path.c_str(), wsld::to_string(data.error()));
    else
      std::printf("read %s: %zu bytes in %.2f ms (%.1f MiB/s)\n", read_path.c_str(), data->size(), ms(dt),
                  static_cast<double>(data->size()) / (1024.0 * 1024.0) / (ms(dt) / 1000.0));
  }

  if (watch) {
    std::printf("watching for invalidations (Ctrl+C to stop)...\n");
    root.set_invalidation_hook([&](const wsld::proto::InvalidationBatch& b) {
      const auto now = std::chrono::system_clock::now().time_since_epoch();
      std::printf("[gen %llu +%lld ms] %zu ops\n", static_cast<unsigned long long>(b.generation),
                  static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count() % 100000),
                  b.ops.size());
      for (const auto& op : b.ops)
        std::printf("   %s %s\n", op.kind == wsld::InvalidationKind::Upsert ? "upsert" : op.kind == wsld::InvalidationKind::Remove ? "remove" : "rescan",
                    op.path.c_str());
      std::fflush(stdout);
    });
    while (root.connected()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::printf("agent disconnected\n");
  }
  return 0;
}
