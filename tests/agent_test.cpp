#include "agent/client.hpp"
#include "agent/scanner.hpp"
#include "agent/server.hpp"
#include "platform/watcher.hpp"
#ifdef _WIN32
#include "platform/win/wsl_launch.hpp"
#else
#include "platform/linux/win_launch.hpp"
#endif

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <span>
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

TEST_F(AgentTest, SingleFileSystemScanDoesNotPruneOrdinaryTrees) {
  // Staying on one filesystem must not change the result for a normal tree that
  // has no mount points in it: on (the default) and off must agree.
  auto count = [&](bool one_fs) {
    std::size_t n = 0;
    auto st = scan_tree(root_, [&](const SnapshotEntry&) { ++n; }, {}, one_fs);
    EXPECT_TRUE(st.has_value());
    return n;
  };
  const std::size_t with_limit = count(true);
  const std::size_t without = count(false);
  EXPECT_EQ(with_limit, without);
  EXPECT_GT(with_limit, 0u);

  // The root's own filesystem id must be resolvable, or the limit silently
  // disables itself and mount points would be traversed after all.
  EXPECT_TRUE(device_id(root_).has_value());
  EXPECT_FALSE(device_id(root_ / "definitely-not-here").has_value());

#ifndef _WIN32
  // /proc is a pseudo-filesystem, so it must report a different id than / —
  // this is exactly the comparison that keeps it out of a `--wsl-root /` scan.
  const auto slash = device_id("/");
  const auto proc = device_id("/proc");
  if (slash && proc) { EXPECT_NE(*slash, *proc); }
#endif
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
  explicit LoopbackServer(fs::path root, bool watch, std::size_t chunk_bytes = (4u << 20))
      : server_({.root = std::move(root), .watch = watch, .coalescer = {}, .snapshot_chunk_bytes = chunk_bytes}) {
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

std::string read_disk(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::span<const std::byte> as_bytes(std::string_view s) { return std::as_bytes(std::span{s.data(), s.size()}); }

// Counts entries under root (files + directories), matching what scan_tree emits
// for a tree with no sockets/fifos/devices.
std::size_t scan_count(const fs::path& root) {
  std::size_t n = 0;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    ++n;
  return n;
}

#ifndef _WIN32
TEST_F(AgentTest, ScannerReportsSymlinks) {
  std::error_code ec;
  fs::create_symlink("main.cpp", root_ / "src" / "link_to_main", ec);
  ASSERT_FALSE(ec);
  bool saw_symlink = false;
  std::vector<std::string> names;
  std::vector<SnapshotEntry> entries;
  auto stats = scan_tree(root_, [&](const SnapshotEntry& e) {
    names.emplace_back(e.name);
    entries.push_back(e);
    if (e.attr.kind == NodeKind::Symlink) saw_symlink = true;
  });
  ASSERT_TRUE(stats.has_value());
  EXPECT_TRUE(saw_symlink);
  EXPECT_EQ(stats->symlinks, 1u);
  for (std::size_t i = 0; i < entries.size(); ++i) entries[i].name = names[i];
  MetadataTree t;
  ASSERT_TRUE(t.load_snapshot(entries).has_value());
  const auto link = t.lookup("src/link_to_main");
  ASSERT_TRUE(link.has_value());
  EXPECT_EQ(t.node(*link).attr.kind, NodeKind::Symlink);
}
#endif

TEST_F(AgentTest, IgnoreRulesExcludeFromSnapshot) {
  write_file(root_ / ".wsldriveignore", "build/\n*.log\n");
  fs::create_directories(root_ / "build" / "obj");
  write_file(root_ / "build" / "obj" / "x.o", "obj");
  write_file(root_ / "app.log", "log");
  write_file(root_ / "keep.txt", "keep");

  LoopbackServer srv(root_, /*watch=*/false);  // loads .wsldriveignore at construction
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());

  c->with_tree([](const MetadataTree& t) {
    EXPECT_FALSE(t.lookup("build").has_value());
    EXPECT_FALSE(t.lookup("build/obj/x.o").has_value());
    EXPECT_FALSE(t.lookup("app.log").has_value());
    EXPECT_TRUE(t.lookup("keep.txt").has_value());
    EXPECT_TRUE(t.lookup("README.md").has_value());  // seeded, not ignored
  });
}

TEST_F(AgentTest, ChunkedSnapshotReassembles) {
  // Add enough entries that a tiny per-frame budget forces many frames.
  for (int i = 0; i < 200; ++i) write_file(root_ / ("chunk_" + std::to_string(i) + ".dat"), "x");
  fs::create_directories(root_ / "deep" / "a" / "b");
  write_file(root_ / "deep" / "a" / "b" / "leaf.txt", "found");
  const std::size_t expected = scan_count(root_);

  LoopbackServer srv(root_, /*watch=*/false, /*chunk_bytes=*/256);  // ~a handful of entries per frame
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());

  EXPECT_EQ(c->with_tree([](const MetadataTree& t) { return t.size(); }), expected + 1);  // +root
  EXPECT_TRUE(c->with_tree([](const MetadataTree& t) { return t.lookup("deep/a/b/leaf.txt").has_value(); }));
  EXPECT_TRUE(c->with_tree([](const MetadataTree& t) { return t.lookup("chunk_199.dat").has_value(); }));
  // The reply really was split across many frames.
  EXPECT_GT(c->stats().snapshot_bytes, 256u);
}

TEST_F(AgentTest, ReadCacheEvictsLeastRecentlyUsed) {
  // Eviction must drop the least-recently-*used* entry, not the oldest-inserted:
  // re-reading a file has to refresh its recency.
  // One file per directory on purpose: a read miss bulk-fetches the target's
  // siblings, which would otherwise re-cache everything and mask the ordering.
  const std::string body(1000, 'x');
  for (const char* d : {"d1", "d2", "d3"}) {
    fs::create_directories(root_ / "lru" / d);
    write_file(root_ / "lru" / d / "f", body);
  }

  LoopbackServer srv(root_, /*watch=*/false);
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());
  c->set_read_cache_limit(2500);  // room for two of these files, not three

  ASSERT_TRUE(c->read("lru/d1/f", 0, 16).has_value());
  ASSERT_TRUE(c->read("lru/d2/f", 0, 16).has_value());
  ASSERT_TRUE(c->read("lru/d1/f", 0, 16).has_value());  // refreshes d1; d2 is now LRU
  ASSERT_TRUE(c->read("lru/d3/f", 0, 16).has_value());  // pushes past the budget

  const auto hits_before = c->stats().read_cache_hits;
  ASSERT_TRUE(c->read("lru/d1/f", 0, 16).has_value());
  EXPECT_GT(c->stats().read_cache_hits, hits_before) << "recently used entry should have survived";

  const auto misses_before = c->stats().read_cache_misses;
  ASSERT_TRUE(c->read("lru/d2/f", 0, 16).has_value());
  EXPECT_GT(c->stats().read_cache_misses, misses_before) << "least-recently-used entry should have been evicted";
  EXPECT_LE(c->stats().read_cache_bytes, 2500u) << "cache must stay within its budget";

  // Lowering the budget to nothing must clear it down (one entry may remain).
  c->set_read_cache_limit(0);
  EXPECT_LE(c->stats().read_cache_bytes, body.size());
}

TEST_F(AgentTest, RejectsPathsOutsideTheServedRoot) {
  // A peer must not be able to name anything outside the root. `..` was already
  // refused, but an absolute/drive-letter path contains no `..` at all and
  // fs::path::operator/ REPLACES the left side when the right has a root name,
  // so `C:/Windows/...` used to escape the root completely.
  LoopbackServer srv(root_, /*watch=*/false);
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());

  for (const char* escape : {"../outside.txt", "a/../../outside.txt", "C:/Windows/win.ini",
                             "C:\\Windows\\win.ini", "D:/x", "sub:stream"}) {
    auto r = c->read(escape, 0, 64);
    EXPECT_FALSE(r.has_value()) << "escaped the root: " << escape;
  }
  // Bulk reads use the same gate: an escaping path yields no data, and a legal
  // sibling in the same request still succeeds.
  auto many = c->read_many({"C:/Windows/win.ini", "../outside.txt", "README.md"});
  ASSERT_TRUE(many.has_value());
  EXPECT_FALSE((*many)[0].has_value());
  EXPECT_FALSE((*many)[1].has_value());
  ASSERT_TRUE((*many)[2].has_value());

  // A name that merely contains ".." is legal and must still be served.
  write_file(root_ / "foo..bar", "dots");
  auto ok = c->read("foo..bar", 0, 64);
  ASSERT_TRUE(ok.has_value());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(ok->data()), ok->size()), "dots");
}

TEST_F(AgentTest, WriteThroughMutations) {
  LoopbackServer srv(root_, /*watch=*/false);  // isolate the optimistic-update path
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());

  auto size_in_tree = [&](std::string_view path) -> std::int64_t {
    return c->with_tree([&](const MetadataTree& t) -> std::int64_t {
      const auto id = t.lookup(path, LookupMode::Exact);
      return id ? static_cast<std::int64_t>(t.node(*id).attr.size) : -1;
    });
  };

  // create + write + read back, verifying disk, mirror, and the read path.
  ASSERT_TRUE(c->create_file("top.txt").has_value());
  EXPECT_TRUE(fs::exists(root_ / "top.txt"));
  auto wrote = c->write("top.txt", 0, as_bytes("hello world"));
  ASSERT_TRUE(wrote.has_value());
  EXPECT_EQ(*wrote, 11u);
  EXPECT_EQ(read_disk(root_ / "top.txt"), "hello world");
  EXPECT_EQ(size_in_tree("top.txt"), 11);
  auto back = c->read("top.txt", 0, 100);
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(back->data()), back->size()), "hello world");

  // write at an offset extends the file.
  ASSERT_TRUE(c->write("top.txt", 11, as_bytes("!!!")).has_value());
  EXPECT_EQ(read_disk(root_ / "top.txt"), "hello world!!!");
  EXPECT_EQ(size_in_tree("top.txt"), 14);

  // truncate.
  ASSERT_TRUE(c->truncate("top.txt", 5).has_value());
  EXPECT_EQ(read_disk(root_ / "top.txt"), "hello");
  EXPECT_EQ(size_in_tree("top.txt"), 5);

  // mkdir + nested create/write.
  ASSERT_TRUE(c->mkdir("sub2").has_value());
  EXPECT_TRUE(fs::is_directory(root_ / "sub2"));
  ASSERT_TRUE(c->create_file("sub2/x.txt").has_value());
  ASSERT_TRUE(c->write("sub2/x.txt", 0, as_bytes("data")).has_value());
  EXPECT_EQ(read_disk(root_ / "sub2" / "x.txt"), "data");
  EXPECT_TRUE(c->with_tree([](const MetadataTree& t) { return t.lookup("sub2/x.txt").has_value(); }));

  // rename.
  ASSERT_TRUE(c->rename("top.txt", "top2.txt").has_value());
  EXPECT_FALSE(fs::exists(root_ / "top.txt"));
  EXPECT_EQ(read_disk(root_ / "top2.txt"), "hello");
  EXPECT_TRUE(c->with_tree([](const MetadataTree& t) {
    return !t.lookup("top.txt").has_value() && t.lookup("top2.txt").has_value();
  }));

  // unlink + rmdir (rmdir refuses non-empty).
  EXPECT_EQ(c->rmdir("sub2").error(), Errc::AlreadyExists);  // directory_not_empty
  ASSERT_TRUE(c->unlink("sub2/x.txt").has_value());
  ASSERT_TRUE(c->rmdir("sub2").has_value());
  EXPECT_FALSE(fs::exists(root_ / "sub2"));
  ASSERT_TRUE(c->unlink("top2.txt").has_value());
  EXPECT_FALSE(fs::exists(root_ / "top2.txt"));

  // error cases.
  EXPECT_EQ(c->unlink("nope.txt").error(), Errc::NotFound);
  EXPECT_EQ(c->rmdir("README.md").error(), Errc::NotADirectory);
  EXPECT_EQ(c->unlink("src").error(), Errc::IsADirectory);
  EXPECT_EQ(c->rename("nope.txt", "whatever.txt").error(), Errc::NotFound);
  EXPECT_EQ(c->create_file("../escape.txt").error(), Errc::InvalidPath);
  EXPECT_EQ(c->write("../escape.txt", 0, as_bytes("x")).error(), Errc::InvalidPath);
}

TEST_F(AgentTest, ReadCacheServesRepeatReads) {
  LoopbackServer srv(root_, /*watch=*/false);
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());
  auto str = [](const std::vector<std::byte>& v) {
    return std::string(reinterpret_cast<const char*>(v.data()), v.size());
  };

  auto r1 = c->read("README.md", 0, 100);  // seeded "# readme\n"
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(str(*r1), "# readme\n");
  EXPECT_EQ(c->stats().read_cache_misses, 1u);
  EXPECT_EQ(c->stats().read_cache_hits, 0u);

  auto r2 = c->read("README.md", 0, 100);  // served from cache
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(str(*r2), "# readme\n");
  EXPECT_EQ(c->stats().read_cache_hits, 1u);

  auto r3 = c->read("README.md", 2, 4);  // partial slice from cache
  ASSERT_TRUE(r3.has_value());
  EXPECT_EQ(str(*r3), "read");
  EXPECT_EQ(c->stats().read_cache_hits, 2u);

  // A write (even same-size) drops the cache entry; the next read refetches.
  ASSERT_TRUE(c->write("README.md", 0, as_bytes("XXXXX")).has_value());
  auto r4 = c->read("README.md", 0, 100);
  ASSERT_TRUE(r4.has_value());
  EXPECT_EQ(str(*r4).substr(0, 5), "XXXXX");
  EXPECT_EQ(c->stats().read_cache_misses, 2u);
}

TEST_F(AgentTest, ReadManyBulkFetch) {
  LoopbackServer srv(root_, /*watch=*/false);
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());
  auto res = c->read_many({"README.md", "src/main.cpp", "does/not/exist", "src/include/a.hpp"});
  ASSERT_TRUE(res.has_value());
  ASSERT_EQ(res->size(), 4u);
  ASSERT_TRUE((*res)[0].has_value());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>((*res)[0]->data()), (*res)[0]->size()), "# readme\n");
  ASSERT_TRUE((*res)[1].has_value());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>((*res)[1]->data()), (*res)[1]->size()), "int main() {}\n");
  EXPECT_FALSE((*res)[2].has_value());  // missing file reported as no-value
  ASSERT_TRUE((*res)[3].has_value());
}

TEST_F(AgentTest, PrefetchWarmsSiblings) {
  // Many files in one directory; reading one should prefetch the rest.
  fs::create_directories(root_ / "pf");
  for (int i = 0; i < 40; ++i) write_file(root_ / "pf" / ("f" + std::to_string(i) + ".txt"), "data" + std::to_string(i));
  LoopbackServer srv(root_, /*watch=*/false);
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());

  ASSERT_TRUE(c->read("pf/f0.txt", 0, 100).has_value());  // triggers directory prefetch
  // Wait for the background prefetch to populate the cache.
  bool warmed = false;
  for (int t = 0; t < 100 && !warmed; ++t) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    warmed = c->stats().read_cache_bytes > 0 &&
             c->with_tree([](const MetadataTree&) { return true; }) &&
             c->read("pf/f39.txt", 0, 100).has_value() && c->stats().read_cache_hits >= 1;
  }
  // A sibling we never explicitly read should now be a cache hit.
  const auto hits_before = c->stats().read_cache_hits;
  auto r = c->read("pf/f20.txt", 0, 100);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(r->data()), r->size()), "data20");
  EXPECT_GT(c->stats().read_cache_hits, hits_before) << "sibling should have been prefetched";
}

TEST_F(AgentTest, WarmCachePrefetchesWholeTree) {
  // A tree spread across several directories; warm_cache() should populate the
  // read cache for all of them so the first read of any file is a cache hit.
  for (int d = 0; d < 3; ++d) {
    fs::create_directories(root_ / ("wc" + std::to_string(d)));
    for (int i = 0; i < 10; ++i)
      write_file(root_ / ("wc" + std::to_string(d)) / ("f" + std::to_string(i) + ".txt"),
                 "d" + std::to_string(d) + "f" + std::to_string(i));
  }
  LoopbackServer srv(root_, /*watch=*/false);
  auto c = connect_client(srv.endpoint());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->connect().has_value());
  ASSERT_TRUE(c->fetch_snapshot().has_value());

  const std::size_t queued = c->warm_cache();
  EXPECT_GT(queued, 0u);
  c->wait_prefetch_idle(10s);

  const auto st = c->stats();
  EXPECT_GT(st.prefetch_files, 0u) << "prefetcher should have fetched files";
  EXPECT_GT(st.prefetch_bytes, 0u);

  // A file we never explicitly read is served from the warmed cache (a hit, no
  // new miss).
  const auto misses_before = c->stats().read_cache_misses;
  auto r = c->read("wc2/f7.txt", 0, 100);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(r->data()), r->size()), "d2f7");
  EXPECT_EQ(c->stats().read_cache_misses, misses_before) << "warmed file should not miss";
  EXPECT_GE(c->stats().read_cache_hits, 1u);
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

TEST_F(AgentTest, LiveInvalidationsReachTheClient) {
  LoopbackServer srv(root_, /*watch=*/true);
  if (!srv.server().watching()) GTEST_SKIP() << "no filesystem watcher on this platform/environment";
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

#ifdef _WIN32
TEST(WslLaunch, BuildsCommand) {
  const std::string cmd = platform::build_wsl_command(
      {.distro = "Ubuntu", .agent_path = "/opt/wsldrived", .wsl_root = "/home/u/proj", .port = 51789});
  EXPECT_NE(cmd.find("wsl.exe -d Ubuntu -- "), std::string::npos);
  EXPECT_NE(cmd.find("'/opt/wsldrived'"), std::string::npos);
  EXPECT_NE(cmd.find("--root '/home/u/proj'"), std::string::npos);
  EXPECT_NE(cmd.find("--listen tcp://127.0.0.1:51789"), std::string::npos);
  EXPECT_NE(cmd.find("--exit-when-idle"), std::string::npos);

  // Default distro is omitted; default agent falls back to PATH lookup.
  const std::string dflt = platform::build_wsl_command({.distro = "", .agent_path = "", .wsl_root = "/x", .port = 1});
  EXPECT_EQ(dflt.find(" -d "), std::string::npos);
  EXPECT_NE(dflt.find("-- 'wsldrived'"), std::string::npos);

  // A single quote in a path is escaped so the distro shell keeps it literal.
  const std::string q = platform::build_wsl_command({.distro = "", .agent_path = "", .wsl_root = "/a'b", .port = 1});
  EXPECT_NE(q.find("'/a'\\''b'"), std::string::npos);

  // An explicit listen endpoint (e.g. vsock for the Hyper-V socket transport) is used verbatim.
  const std::string v = platform::build_wsl_command(
      {.distro = "Ubuntu", .agent_path = "", .wsl_root = "/x", .port = 5700, .listen = "vsock://any:5700"});
  EXPECT_NE(v.find("--listen vsock://any:5700"), std::string::npos);
  EXPECT_EQ(v.find("tcp://"), std::string::npos);
}

#else
TEST(WinLaunch, BuildsCommand) {
  const std::string cmd = platform::build_win_agent_command(
      {.exe = "/mnt/c/wsldrived.exe", .win_root = "C:/proj", .connect = "hv://{guid}:5700"});
  EXPECT_NE(cmd.find("/mnt/c/wsldrived.exe"), std::string::npos);
  EXPECT_NE(cmd.find("--root C:/proj"), std::string::npos);
  EXPECT_NE(cmd.find("--connect hv://{guid}:5700"), std::string::npos);
  EXPECT_NE(cmd.find("--exit-when-idle"), std::string::npos);
}
#endif

#ifdef _WIN32
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
