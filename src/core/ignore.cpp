#include "core/ignore.hpp"

namespace wsld {

namespace {

// Glob match with '*' (any run of non-'/') and '?' (one non-'/'), over a single
// path segment or an already-segmented comparison. Iterative with backtracking.
bool glob_segment(std::string_view pat, std::string_view text) {
  std::size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
  while (t < text.size()) {
    if (p < pat.size() && (pat[p] == '?' || pat[p] == text[t])) {
      ++p;
      ++t;
    } else if (p < pat.size() && pat[p] == '*') {
      star = p++;
      mark = t;
    } else if (star != std::string_view::npos) {
      p = star + 1;
      t = ++mark;
    } else {
      return false;
    }
  }
  while (p < pat.size() && pat[p] == '*') ++p;
  return p == pat.size();
}

std::vector<std::string_view> split(std::string_view s) {
  std::vector<std::string_view> out;
  std::size_t i = 0;
  while (i < s.size()) {
    std::size_t j = s.find('/', i);
    if (j == std::string_view::npos) j = s.size();
    if (j > i) out.push_back(s.substr(i, j - i));
    i = j + 1;
  }
  return out;
}

}  // namespace

IgnoreRules IgnoreRules::parse(std::string_view text) {
  IgnoreRules rules;
  std::size_t i = 0;
  while (i < text.size()) {
    std::size_t j = text.find('\n', i);
    if (j == std::string_view::npos) j = text.size();
    std::string_view line = text.substr(i, j - i);
    i = j + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    // Trim leading/trailing spaces.
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
    if (line.empty() || line.front() == '#') continue;

    Pattern pat;
    if (line.back() == '/') {
      pat.dir_only = true;
      line.remove_suffix(1);
    }
    // A leading '/' or any interior '/' anchors the pattern to the root.
    if (!line.empty() && line.front() == '/') {
      pat.anchored = true;
      line.remove_prefix(1);
    } else if (line.find('/') != std::string_view::npos) {
      pat.anchored = true;
    }
    for (auto seg : split(line)) pat.segs.emplace_back(seg);
    if (!pat.segs.empty()) rules.patterns_.push_back(std::move(pat));
  }
  return rules;
}

bool IgnoreRules::ignored(std::string_view rel, bool is_dir) const {
  if (patterns_.empty() || rel.empty()) return false;
  const std::vector<std::string_view> segs = split(rel);
  if (segs.empty()) return false;

  for (const Pattern& pat : patterns_) {
    if (pat.anchored) {
      // Match the pattern segments against a prefix of the path.
      if (pat.segs.size() > segs.size()) continue;
      bool all = true;
      for (std::size_t k = 0; k < pat.segs.size(); ++k) {
        if (!glob_segment(pat.segs[k], segs[k])) {
          all = false;
          break;
        }
      }
      if (!all) continue;
      // If the pattern names a directory, the matched prefix must be a directory:
      // either it is an interior prefix (more segments follow) or the leaf is a dir.
      const bool prefix_is_dir = pat.segs.size() < segs.size() || is_dir;
      if (!pat.dir_only || prefix_is_dir) return true;
    } else {
      // Unanchored single-name pattern: match any segment.
      const std::string_view name = pat.segs.back();  // unanchored patterns have one segment
      for (std::size_t s = 0; s < segs.size(); ++s) {
        if (!glob_segment(name, segs[s])) continue;
        const bool seg_is_dir = s + 1 < segs.size() || is_dir;
        if (!pat.dir_only || seg_is_dir) return true;
      }
    }
  }
  return false;
}

}  // namespace wsld
