#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace wsld {

/// A small, gitignore-flavoured matcher for `.wsldriveignore`. It decides which
/// paths the agent excludes from the served tree (skipped during scan and in
/// invalidations). Opt-in: with no rules, nothing is ignored.
///
/// Supported syntax (a practical subset, not full gitignore):
///   - blank lines and lines starting with `#` are ignored
///   - a trailing `/` restricts the pattern to directories
///   - a leading `/` (or any interior `/`) anchors the pattern to the root
///   - `*` matches any run of characters except `/`; `?` matches one such character
///   - a pattern with no `/` matches that basename at any depth
/// Not supported: `**`, negation (`!`), character classes.
class IgnoreRules {
 public:
  [[nodiscard]] static IgnoreRules parse(std::string_view text);

  [[nodiscard]] bool empty() const noexcept { return patterns_.empty(); }

  /// True if `rel` (normalised, '/'-separated, no leading slash) is excluded.
  /// `is_dir` refers to the last segment of `rel`.
  [[nodiscard]] bool ignored(std::string_view rel, bool is_dir) const;

 private:
  struct Pattern {
    std::vector<std::string> segs;  // pattern split on '/'
    bool anchored = false;          // match from the root rather than any basename
    bool dir_only = false;          // only matches directories
  };
  std::vector<Pattern> patterns_;
};

}  // namespace wsld
