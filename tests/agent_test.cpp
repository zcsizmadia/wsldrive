#include "agent/client.hpp"
#include "agent/scanner.hpp"
#include "agent/server.hpp"
#include "platform/watcher.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace wsld::agent {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

void write_file(const fs::path& p, std::string_view content) {
  std::ofstream(p, std::ios::binary) << content;
}

class AgentTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() /
            ("wsldrive-agent-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    fs::remove_all(root_);
    fs::create_directories(root_ / "src" / "include");
    fs::create_directories(root_ / "Docs");
    write_file(root_ / "README.md", "# readme\n");
    write_file(root_ / "src" / "main.cpp", "int main() {}\n");
    write_file(root_ / "src" / "include" / "a.hpp", "#pragma once\n");
    write_file(root_ / "Docs" / "Guide.txt", std::string(100000, 'g'));
  }
  void TearDown() override { fs::remove_all(root_); }

  fs::path root_;
};

TEST_F(AgentTest, ScanTreeProducesLoadableSnapshot) {
  std::vector<std::string> names;
  std::vector<SnapshotEntry> entries;
  auto stats = scan_tree(root_, [&](const SnapshotEntry& e) {
    names.emplace_back(e.name);
    entries.push_back(e);
  });
  ASSERT_TRUE(stats.has_value());
  EXPECT_EQ(stats->files, 4u);
  EXPECT_EQ(stats->directories, 3u);
  for (std::size_t i = 0; i < entries.size(); ++i) entries[i].name = names[i];

  MetadataTree t;
  ASSERT_TRUE(t.load_snapshot(entries).has_value());
  EXPECT_EQ(t.size(), 8u);
  const auto a = t.lookup("src/include/a.hpp");
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(t.node(*a).attr.kind, NodeKind::File);
  EXPECT_EQ(t.node(*a).attr.size, 13u);
  EXPECT_GT(t.node(*a).attr.mtime_ns, 1'600'000'000'000'000'000LL);
  EXPECT_TRUE(t.node(*t.lookup("Docs")).is_dir());
  EXPECT_EQ(t.node(*t.lookup("Docs/Guide.txt")).attr.size, 100000u);
  EXPECT_TRUE(t.lookup("docs/guide.TXT", LookupMode::CaseInsensitive).has_value());
  EXPECT_EQ(scan_tree(root_ / "README.md", [](const SnapshotEntry&) {}).error(), Errc::NotADirectory);
}

TEST_F(AgentTest, ReadAttributes) {
  auto a = read_attributes(root_ / "README.md");
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->kind, NodeKind::File);
  EXPECT_EQ(a->size, 9u);
  EXPECT_NE(a->mode & 0444u, 0u);
  EXPECT_EQ(read_attributes(root_ / "nope").error(), Errc::NotFound);
  EXPECT_EQ(join_relative(root_, "src/main.cpp"), root_ / "src" / "main.cpp");
}

/// Runs a RootServer on a loopback TCP listener with one accept thread.
class LoopbackServer {
 public:
  explicit LoopbackServer(fs::path root, bool watch) : server_({.root = std::move(root), .watch = watch}) {
    auto l = net::Listener::bind(*net::Endpoint::parse("tcp://127.0.0.1:0"));
    if (!l) throw std::runtime_error("bind failed");
    listener_ = std::move(*l);
    endpoint_ = listener_.local();
    if (auto r = server_.start(); !r) throw std::runtime_error("server start failed");
    acceptor_ = std::thread([this] {
      while (!stopping_.load()) {
        auto s = listener_.accept(std::chrono::milliseconds(50));
        if (!s) {
          if (s.error() == Errc::Timeout) continue;
          break;
        }
        sessions_.emplace_back([this, sock = std::move(*s)]() mutable {
          net::FrameChannel ch(std::move(sock));
          server_.serve(ch);
        });
      }
    });
  }
  ~LoopbackServer() {
    stopping_.store(true);  // acceptor polls this via accept(timeout); no accept-unblock races
    acceptor_.join();
    listener_.close();
    server_.stop();  // shuts down live sessions so their threads unwind
    for (auto& t : sessions_) t.join();
  }
  [[nodiscard]] net::Endpoint endpoint() const { return endpoint_; }
  [[nodiscard]] RootServer& server() { return server_; }

 private:
  RootServer server_;
  net::Listener listener_;
  net::Endpoint endpoint_;
  std::atomic<bool> stopping_{false};
  std::thread acceptor_;
  std::vector<std::thread> sessions_;
};

std::unique_ptr<RemoteRoot> connect_client(const net::Endpoint& ep) {
  auto sock = net::connect(ep);
  if (!sock) return nullptr;
  return std::make_unique<RemoteRoot>(std::make_unique<net::FrameChannel>(std::move(*sock)));
}

TEST_F(AgentTest, EndToEndSnapshotAndRead) {
  LoopbackServer srv(root_, /*watch=*/false);
  auto client = connect_client(srv.endpoint());
  ASSERT_NE(client, nullptr);

  auto hello = client->connect();
  ASSERT_TRUE(hello.has_value()) << to_string(hello.error());
  EXPECT_EQ(hello->protocol_version, proto::kVersion);
  EXPECT_EQ(hello->capabilities, 0u);  // no watcher requested

  auto rtt = client->ping();
  ASSERT_TRUE(rtt.has_value());
  EXPECT_LT(*rtt, 1s);

  ASSERT_TRUE(client->fetch_snapshot().has_value());
  EXPECT_EQ(client->with_tree([](const MetadataTree& t) { return t.size(); }), 8u);
  EXPECT_TRUE(client->with_tree([](const MetadataTree& t) { return t.lookup("src/include/a.hpp").has_value(); }));
  EXPECT_GT(client->stats().snapshot_bytes, 0u);

  auto data = client->read("src/main.cpp", 0, 1 << 20);
  ASSERT_TRUE(data.has_value()) << to_string(data.error());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(data->data()), data->size()), "int main() {}\n");

  auto tail = client->read("Docs/Guide.txt", 99990, 1000);
  ASSERT_TRUE(tail.has_value());
  EXPECT_EQ(tail->size(), 10u);

  auto past_end = client->read("Docs/Guide.txt", 1 << 30, 10);
  ASSERT_TRUE(past_end.has_value());
  EXPECT_TRUE(past_end->empty());

  EXPECT_EQ(client->read("missing.txt", 0, 10).error(), Errc::NotFound);
  EXPECT_EQ(client->read("../etc/passwd", 0, 10).error(), Errc::InvalidPath);

  // A second client sees the same tree.
  auto client2 = connect_client(srv.endpoint());
  ASSERT_NE(client2, nullptr);
  ASSERT_TRUE(client2->connect().has_value());
  ASSERT_TRUE(client2->fetch_snapshot().has_value());
  EXPECT_EQ(client2->with_tree([](const MetadataTree& t) { return t.size(); }), 8u);

  client->close();
  EXPECT_FALSE(client->connected());
  EXPECT_EQ(client->ping().error(), Errc::ConnectionClosed);
}

TEST_F(AgentTest, ServerDisconnectFailsPendingRequests) {
  auto srv = std::make_unique<LoopbackServer>(root_, false);
  auto client = connect_client(srv->endpoint());
  ASSERT_NE(client, nullptr);
  ASSERT_TRUE(client->connect().has_value());
  srv.reset();  // closes the listener and session threads
  auto r = client->ping(2s);
  EXPECT_FALSE(r.has_value());
}

#ifdef _WIN32
TEST_F(AgentTest, LiveInvalidationsReachTheClient) {
  LoopbackServer srv(root_, /*watch=*/true);
  ASSERT_TRUE(srv.server().watching());
  auto client = connect_client(srv.endpoint());
  ASSERT_NE(client, nullptr);
  auto hello = client->connect();
  ASSERT_TRUE(hello.has_value());
  EXPECT_EQ(hello->capabilities, 1u);
  ASSERT_TRUE(client->fetch_snapshot().has_value());

  std::mutex mu;
  std::condition_variable cv;
  std::vector<proto::InvalidationOp> seen;
  client->set_invalidation_hook([&](const proto::InvalidationBatch& b) {
    std::lock_guard lock(mu);
    seen.insert(seen.end(), b.ops.begin(), b.ops.end());
    cv.notify_all();
  });
  auto wait_for = [&](auto pred) {
    std::unique_lock lock(mu);
    return cv.wait_for(lock, 10s, [&] { return pred(seen); });
  };

  // Create.
  write_file(root_ / "src" / "new_file.cpp", "// new\n");
  ASSERT_TRUE(wait_for([](const auto& ops) {
    for (const auto& op : ops)
      if (op.kind == InvalidationKind::Upsert && op.path == "src/new_file.cpp" && op.attr.size == 7) return true;
    return false;
  }));
  EXPECT_TRUE(client->with_tree([](const MetadataTree& t) {
    const auto n = t.lookup("src/new_file.cpp");
    return n.has_value() && t.node(*n).attr.size == 7;
  }));

  // Remove a directory subtree.
  fs::remove_all(root_ / "src" / "include");
  ASSERT_TRUE(wait_for([](const auto& ops) {
    for (const auto& op : ops)
      if (op.kind == InvalidationKind::Remove && op.path == "src/include") return true;
    return false;
  }));
  EXPECT_FALSE(client->with_tree([](const MetadataTree& t) { return t.lookup("src/include/a.hpp").has_value(); }));

  // Rename.
  fs::rename(root_ / "README.md", root_ / "README.rst");
  ASSERT_TRUE(wait_for([](const auto& ops) {
    bool removed = false, added = false;
    for (const auto& op : ops) {
      removed |= op.kind == InvalidationKind::Remove && op.path == "README.md";
      added |= op.kind == InvalidationKind::Upsert && op.path == "README.rst";
    }
    return removed && added;
  }));
  EXPECT_TRUE(client->with_tree([](const MetadataTree& t) {
    return !t.lookup("README.md").has_value() && t.lookup("README.rst").has_value();
  }));
  EXPECT_GE(client->stats().invalidation_batches, 1u);
  EXPECT_GT(client->stats().generation, 1u);
}

TEST(Win32Watcher, ReportsEventsAndStopsCleanly) {
  const fs::path root = fs::temp_directory_path() / "wsldrive-watcher-test";
  fs::remove_all(root);
  fs::create_directories(root / "sub");

  std::mutex mu;
  std::condition_variable cv;
  std::vector<std::pair<FsEventKind, std::string>> events;
  auto w = platform::make_watcher(root, [&](const FsEvent& ev) {
    std::lock_guard lock(mu);
    events.emplace_back(ev.kind, std::string(ev.path));
    cv.notify_all();
  });
  ASSERT_TRUE(w.has_value());

  write_file(root / "sub" / "f.txt", "x");
  {
    std::unique_lock lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, 5s, [&] {
      for (const auto& [k, p] : events)
        if (k == FsEventKind::Created && p == "sub/f.txt") return true;
      return false;
    }));
  }
  (*w)->stop();
  (*w)->stop();  // idempotent
  w->reset();
  fs::remove_all(root);
  EXPECT_FALSE(platform::make_watcher(root / "does-not-exist", [](const FsEvent&) {}).has_value());
}
#endif

}  // namespace
}  // namespace wsld::agent
