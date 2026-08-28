#include "core/path.hpp"

namespace wsld {

void normalize_path_append(std::string_view in, std::string& out) {
  out.reserve(out.size() + in.size());
  const std::size_t start = out.size();
  std::size_t i = 0;
  const std::size_t n = in.size();
  while (i < n) {
    while (i < n && is_path_separator(in[i])) ++i;
    std::size_t j = i;
    while (j < n && !is_path_separator(in[j])) ++j;
    const std::string_view comp = in.substr(i, j - i);
    i = j;
    if (comp.empty() || comp == ".") continue;
    if (out.size() > start) out.push_back('/');
    out.append(comp);
  }
}

std::string normalize_path(std::string_view in) {
  std::string out;
  normalize_path_append(in, out);
  return out;
}

std::pair<std::string_view, std::string_view> split_parent(std::string_view path) noexcept {
  const std::size_t pos = path.rfind('/');
  if (pos == std::string_view::npos) return {std::string_view{}, path};
  return {path.substr(0, pos), path.substr(pos + 1)};
}

bool path_is_under(std::string_view child, std::string_view parent) noexcept {
  if (parent.empty()) return true;
  if (child.size() < parent.size()) return false;
  if (child.compare(0, parent.size(), parent) != 0) return false;
  return child.size() == parent.size() || child[parent.size()] == '/';
}

}  // namespace wsld
