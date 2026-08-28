// wsldrive: client CLI. For now a measurement tool for the transport + agent
// layer; the WinFsp mount front-end lands on top of RemoteRoot.
//
//   wsldrive fetch --connect tcp://127.0.0.1:7788 [--watch] [--read <path>] [--lookups N]
//   wsldrive fetch --listen hv://7788          (wait for the WSL2 agent to dial in)
#include "agent/client.hpp"
#include "net/frame_channel.hpp"
#include "net/socket.hpp"

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
      "usage: wsldrive fetch (--connect <endpoint> | --listen <endpoint>) [--watch] [--read <path>] [--lookups N]\n",
      stderr);
}

double ms(std::chrono::nanoseconds d) { return std::chrono::duration<double, std::milli>(d).count(); }

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || std::string_view(argv[1]) != "fetch") {
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
