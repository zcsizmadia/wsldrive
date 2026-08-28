#include "core/coalescer.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace wsld {
namespace {

using namespace std::chrono_literals;
using tp = Coalescer::clock::time_point;

const tp t0 = tp{} + 1s;

std::vector<PlannedOp> ops(std::initializer_list<std::pair<InvalidationKind, const char*>> list) {
  std::vector<PlannedOp> out;
  for (const auto& [k, p] : list) out.push_back(PlannedOp{k, p});
  return out;
}

TEST(Coalescer, IdleIsNotReady) {
  Coalescer c;
  EXPECT_TRUE(c.empty());
  EXPECT_FALSE(c.ready(t0));
  EXPECT_FALSE(c.deadline().has_value());
  EXPECT_TRUE(c.take().empty());
}

TEST(Coalescer, QuietPeriodAndMaxLatency) {
  Coalescer c(Coalescer::Options{.max_pending = 100, .quiet_period = 2ms, .max_latency = 25ms});
  c.push({FsEventKind::Modified, "a"}, t0);
  EXPECT_FALSE(c.ready(t0 + 1ms));
  EXPECT_EQ(*c.deadline(), t0 + 2ms);
  EXPECT_TRUE(c.ready(t0 + 2ms));

  // A steady trickle keeps resetting the quiet timer until max_latency hits.
  Coalescer d(Coalescer::Options{.max_pending = 100, .quiet_period = 2ms, .max_latency = 25ms});
  tp now = t0;
  for (int i = 0; i < 24; ++i) {
    d.push({FsEventKind::Modified, "a"}, now);
    EXPECT_FALSE(d.ready(now + 1ms)) << i;
    now += 1ms;
  }
  EXPECT_EQ(*d.deadline(), t0 + 25ms);
  EXPECT_TRUE(d.ready(t0 + 25ms));
}

TEST(Coalescer, MaxPendingForcesReady) {
  Coalescer c(Coalescer::Options{.max_pending = 3, .quiet_period = 1h, .max_latency = 1h});
  c.push({FsEventKind::Created, "a"}, t0);
  c.push({FsEventKind::Created, "b"}, t0);
  EXPECT_FALSE(c.ready(t0));
  c.push({FsEventKind::Created, "c"}, t0);
  EXPECT_TRUE(c.ready(t0));
  EXPECT_EQ(*c.deadline(), t0);
}

TEST(Coalescer, RepeatedEventsCollapse) {
  Coalescer c;
  c.push({FsEventKind::Created, "f"}, t0);
  c.push({FsEventKind::Modified, "f"}, t0);
  c.push({FsEventKind::Modified, "f"}, t0);
  c.push({FsEventKind::Modified, "g"}, t0);
  EXPECT_EQ(c.pending(), 2u);
  EXPECT_EQ(c.take(), ops({{InvalidationKind::Upsert, "f"}, {InvalidationKind::Upsert, "g"}}));
  EXPECT_TRUE(c.empty());
}

TEST(Coalescer, LastEventWinsAndOrderFollowsLastEvent) {
  Coalescer c;
  c.push({FsEventKind::Modified, "a"}, t0);
  c.push({FsEventKind::Modified, "b"}, t0);
  c.push({FsEventKind::Removed, "a"}, t0);  // a's final event is now after b's
  EXPECT_EQ(c.take(), ops({{InvalidationKind::Upsert, "b"}, {InvalidationKind::Remove, "a"}}));

  c.push({FsEventKind::Removed, "x"}, t0);
  c.push({FsEventKind::Created, "x"}, t0);  // deleted then re-created -> refresh
  EXPECT_EQ(c.take(), ops({{InvalidationKind::Upsert, "x"}}));
}

TEST(Coalescer, RenameBecomesRemovePlusUpsert) {
  Coalescer c;
  c.push({FsEventKind::RenamedFrom, "old.txt"}, t0);
  c.push({FsEventKind::RenamedTo, "new.txt"}, t0);
  EXPECT_EQ(c.take(), ops({{InvalidationKind::Remove, "old.txt"}, {InvalidationKind::Upsert, "new.txt"}}));
}

TEST(Coalescer, RemovedDirectoryDropsOlderDescendants) {
  Coalescer c;
  c.push({FsEventKind::Modified, "dir/a"}, t0);
  c.push({FsEventKind::Modified, "dir/sub/b"}, t0);
  c.push({FsEventKind::Modified, "directory/unrelated"}, t0);  // shares a prefix but is not under "dir"
  c.push({FsEventKind::Removed, "dir/sub"}, t0);
  c.push({FsEventKind::Removed, "dir"}, t0);
  c.push({FsEventKind::Created, "dir/c"}, t0);  // re-created after the removal: must survive
  EXPECT_EQ(c.take(), ops({{InvalidationKind::Upsert, "directory/unrelated"},
                           {InvalidationKind::Remove, "dir"},
                           {InvalidationKind::Upsert, "dir/c"}}));
}

TEST(Coalescer, OverflowCollapsesToRescan) {
  Coalescer c(Coalescer::Options{.max_pending = 100, .quiet_period = 1h, .max_latency = 1h});
  c.push({FsEventKind::Modified, "a"}, t0);
  c.push({FsEventKind::Overflow, ""}, t0);
  c.push({FsEventKind::Modified, "b"}, t0);  // ignored; the rescan covers it
  EXPECT_TRUE(c.overflowed());
  EXPECT_TRUE(c.ready(t0));
  EXPECT_EQ(c.pending(), 1u);
  EXPECT_EQ(c.take(), ops({{InvalidationKind::Rescan, ""}}));
  EXPECT_FALSE(c.overflowed());
  EXPECT_TRUE(c.empty());
}

}  // namespace
}  // namespace wsld
