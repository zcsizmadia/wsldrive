// wsldrived: serves a directory tree to wsldrive clients.
//
//   wsldrived --root <dir> --listen tcp://127.0.0.1:7788
//   wsldrived --root <dir> --connect vsock://host:7788      (WSL2 guest dialling the Windows host)
#include "agent/server.hpp"
#include "net/frame_channel.hpp"
#include "net/socket.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

void usage() {
  std::fputs(
      "usage: wsldrived --root <dir> (--listen <endpoint> | --connect <endpoint>) [--no-watch]\n"
      "  endpoints: tcp://host:port | vsock://cid:port | hv://port\n",
      stderr);
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path root;
  std::string listen, connect;
  bool watch = true;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto next = [&](std::string& out) {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      out = argv[++i];
    };
    if (a == "--root") {
      std::string s;
      next(s);
      root = std::filesystem::path(s);
    } else if (a == "--listen") {
      next(listen);
    } else if (a == "--connect") {
      next(connect);
    } else if (a == "--no-watch") {
      watch = false;
    } else {
      usage();
      return 2;
    }
  }
  if (root.empty() || (listen.empty() == connect.empty())) {
    usage();
    return 2;
  }

  wsld::agent::RootServer server({.root = root, .watch = watch});
  if (auto r = server.start(); !r) {
    std::fprintf(stderr, "wsldrived: cannot start: %s\n", wsld::to_string(r.error()));
    return 1;
  }
  std::fprintf(stderr, "wsldrived: serving %s (%s)\n", root.string().c_str(),
               server.watching() ? "live invalidations" : "no watcher on this platform");

  if (!connect.empty()) {
    auto ep = wsld::net::Endpoint::parse(connect);
    if (!ep) {
      std::fprintf(stderr, "wsldrived: bad endpoint '%s'\n", connect.c_str());
      return 2;
    }
    auto sock = wsld::net::connect(*ep);
    if (!sock) {
      std::fprintf(stderr, "wsldrived: connect to %s failed: %s (os error %d)\n", ep->to_string().c_str(),
                   wsld::to_string(sock.error()), wsld::net::last_socket_error());
      return 1;
    }
    wsld::net::FrameChannel ch(std::move(*sock));
    server.serve(ch);
    return 0;
  }

  auto ep = wsld::net::Endpoint::parse(listen);
  if (!ep) {
    std::fprintf(stderr, "wsldrived: bad endpoint '%s'\n", listen.c_str());
    return 2;
  }
  auto listener = wsld::net::Listener::bind(*ep);
  if (!listener) {
    std::fprintf(stderr, "wsldrived: listen on %s failed: %s (os error %d)\n", ep->to_string().c_str(),
                 wsld::to_string(listener.error()), wsld::net::last_socket_error());
    return 1;
  }
  std::fprintf(stderr, "wsldrived: listening on %s\n", listener->local().to_string().c_str());
  std::vector<std::thread> sessions;
  for (;;) {
    auto sock = listener->accept();
    if (!sock) {
      if (sock.error() == wsld::Errc::Timeout) continue;
      break;
    }
    sessions.emplace_back([&server, s = std::move(*sock)]() mutable {
      wsld::net::FrameChannel ch(std::move(s));
      server.serve(ch);
    });
  }
  for (auto& t : sessions) t.join();
  return 0;
}
