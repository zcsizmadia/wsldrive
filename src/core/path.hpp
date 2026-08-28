#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace wsld {

[[nodiscard]] constexpr bool is_path_separator(char c) noexcept { return c == '/' || c == '\\'; }

/// Normalises a relative path for use as a tree/coalescer key:
/// backslashes become '/', repeated separators collapse, leading and trailing
/// separators are stripped, and "." components are dropped. ".." is kept verbatim
/// (callers decide whether to allow it). The result is appended to `out`.
void normalize_path_append(std::string_view in, std::string& out);

[[nodiscard]] std::string normalize_path(std::string_view in);

/// Splits a normalised path into (parent, leaf). For "a/b/c" returns ("a/b", "c");
/// for "c" returns ("", "c"); for "" returns ("", "").
[[nodiscard]] std::pair<std::string_view, std::string_view> split_parent(std::string_view path) noexcept;

/// True if `child` equals `parent` or lies strictly below it ("a/b" is under "a").
/// Both must be normalised. An empty parent is the root and contains everything.
[[nodiscard]] bool path_is_under(std::string_view child, std::string_view parent) noexcept;

}  // namespace wsld
