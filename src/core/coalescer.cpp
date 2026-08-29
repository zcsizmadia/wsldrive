#include "core/coalescer.hpp"

#include "core/path.hpp"

#include <algorithm>

namespace wsld {

void Coalescer::push(const FsEvent& ev, clock::time_point now) {
  if (pending_.empty() && !overflow_) first_ = now;
  last_ = now;

  if (ev.kind == FsEventKind::Overflow) {
    overflow_ = true;
    pending_.clear();
    return;
  }
  if (overflow_) return;  // the rescan will cover it

  InvalidationKind kind;
  bool appeared = false;
  switch (ev.kind) {
    case FsEventKind::Created:
    case FsEventKind::RenamedTo:
      kind = InvalidationKind::Upsert;
      appeared = true;
      break;
    case FsEventKind::Modified:
      kind = InvalidationKind::Upsert;
      break;
    case FsEventKind::Removed:
    case FsEventKind::RenamedFrom:
      kind = InvalidationKind::Remove;
      break;
    case FsEventKind::Overflow:
    default:
      return;
  }

  if (auto it = pending_.find(ev.path); it != pending_.end()) {
    // A Modified after a Created keeps the entry "new"; a removal resets it.
    const bool still_new = kind == InvalidationKind::Upsert && (appeared || it->second.appeared);
    it->second = Entry{kind, ++seq_, still_new};
  } else {
    pending_.emplace(std::string(ev.path), Entry{kind, ++seq_, appeared});
  }
}

bool Coalescer::ready(clock::time_point now) const noexcept {
  if (overflow_) return true;
  if (pending_.empty()) return false;
  if (pending_.size() >= opts_.max_pending) return true;
  return now - last_ >= opts_.quiet_period || now - first_ >= opts_.max_latency;
}

std::optional<Coalescer::clock::time_point> Coalescer::deadline() const noexcept {
  if (overflow_) return last_;
  if (pending_.empty()) return std::nullopt;
  if (pending_.size() >= opts_.max_pending) return last_;
  return std::min(last_ + opts_.quiet_period, first_ + opts_.max_latency);
}

std::vector<PlannedOp> Coalescer::take() {
  std::vector<PlannedOp> out;
  if (overflow_) {
    out.push_back(PlannedOp{InvalidationKind::Rescan, std::string{}});
    overflow_ = false;
    pending_.clear();
    return out;
  }
  if (pending_.empty()) return out;

  struct Item {
    std::string_view path;
    InvalidationKind kind;
    std::uint64_t seq;
    bool appeared;
  };
  std::vector<Item> items;
  items.reserve(pending_.size());
  for (const auto& [path, e] : pending_) items.push_back(Item{path, e.kind, e.seq, e.appeared});

  // Sort by path so that a removed directory is immediately followed by its
  // descendants; drop descendants whose last event predates the removal.
  std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.path < b.path; });
  std::vector<Item> kept;
  kept.reserve(items.size());
  for (std::size_t i = 0; i < items.size(); ++i) {
    const Item& it = items[i];
    kept.push_back(it);
    if (it.kind != InvalidationKind::Remove) continue;
    std::size_t j = i + 1;
    while (j < items.size() && path_is_under(items[j].path, it.path)) {
      if (items[j].seq > it.seq) kept.push_back(items[j]);  // re-created after the removal
      ++j;
    }
    i = j - 1;
  }

  // Emit in arrival order of each path's final event.
  std::sort(kept.begin(), kept.end(), [](const Item& a, const Item& b) { return a.seq < b.seq; });
  out.reserve(kept.size());
  for (const Item& it : kept) out.push_back(PlannedOp{it.kind, std::string(it.path), it.appeared});

  pending_.clear();
  return out;
}

}  // namespace wsld
