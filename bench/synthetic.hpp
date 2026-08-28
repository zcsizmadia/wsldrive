#pragma once

#include "core/metadata_tree.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace wsld::bench {

inline constexpr Attributes kDirAttr{.size = 0, .mtime_ns = 1'700'000'000'000'000'000LL, .mode = 0755,
                                     .kind = NodeKind::Directory};
inline constexpr Attributes kFileAttr{.size = 4096, .mtime_ns = 1'700'000'000'000'000'000LL, .mode = 0644,
                                      .kind = NodeKind::File};

/// A synthetic source tree shaped like a real repository: `dirs` top-level
/// directories, each with `sub` subdirectories, each holding `files` files.
struct SyntheticTree {
  MetadataTree tree;
  std::vector<std::string> file_paths;  // '/'-separated, one per file
  std::vector<std::string> dir_paths;

  static SyntheticTree build(int dirs, int sub, int files) {
    SyntheticTree s;
    s.file_paths.reserve(static_cast<std::size_t>(dirs) * static_cast<std::size_t>(sub) *
                         static_cast<std::size_t>(files));
    for (int d = 0; d < dirs; ++d) {
      const std::string dname = "module_" + std::to_string(d);
      const NodeId dn = *s.tree.insert(s.tree.root(), dname, kDirAttr);
      s.dir_paths.push_back(dname);
      for (int u = 0; u < sub; ++u) {
        const std::string sname = "component" + std::to_string(u);
        const NodeId sn = *s.tree.insert(dn, sname, kDirAttr);
        s.dir_paths.push_back(dname + "/" + sname);
        for (int f = 0; f < files; ++f) {
          const std::string fname = "Source_File_" + std::to_string(f) + (f % 2 ? ".cpp" : ".hpp");
          (void)*s.tree.insert(sn, fname, kFileAttr);
          s.file_paths.push_back(dname + "/" + sname + "/" + fname);
        }
      }
    }
    return s;
  }
};

/// Lowercases ASCII in place; used to produce case-insensitive probe paths.
inline std::string lowered(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
  return s;
}

}  // namespace wsld::bench
