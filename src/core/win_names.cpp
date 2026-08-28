#include "core/win_names.hpp"

namespace wsld {

namespace {

// The reserved characters Windows forbids in a path component (besides '/'
// and '\\', which are separators and never appear inside one name here).
bool is_reserved(unsigned char b) noexcept {
  switch (b) {
    case '<':
    case '>':
    case ':':
    case '"':
    case '|':
    case '?':
    case '*':
    case '\\':
      return true;
    default:
      return b < 0x20;  // control characters
  }
}

void encode_utf8(char32_t cp, std::string& out) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

}  // namespace

bool needs_escaping(std::string_view raw) noexcept {
  for (char c : raw)
    if (is_reserved(static_cast<unsigned char>(c))) return true;
  if (!raw.empty() && (raw.back() == '.' || raw.back() == ' ')) return true;
  return false;
}

std::string escape_for_windows(std::string_view raw) {
  if (!needs_escaping(raw)) return std::string(raw);
  std::string out;
  out.reserve(raw.size() + 4);
  for (std::size_t i = 0; i < raw.size(); ++i) {
    const unsigned char b = static_cast<unsigned char>(raw[i]);
    const bool trailing_dot_space = (i + 1 == raw.size()) && (b == '.' || b == ' ');
    if (is_reserved(b) || trailing_dot_space)
      encode_utf8(static_cast<char32_t>(0xF000 + b), out);
    else
      out.push_back(static_cast<char>(b));
  }
  return out;
}

std::string unescape_from_windows(std::string_view shown) {
  std::string out;
  out.reserve(shown.size());
  std::size_t i = 0;
  while (i < shown.size()) {
    const unsigned char b0 = static_cast<unsigned char>(shown[i]);
    // A U+F000..U+F0FF code point encodes as EF 80-83 8x; decode and map back.
    if (b0 == 0xEF && i + 2 < shown.size()) {
      const unsigned char b1 = static_cast<unsigned char>(shown[i + 1]);
      const unsigned char b2 = static_cast<unsigned char>(shown[i + 2]);
      if ((b1 & 0xFC) == 0x80 && (b2 & 0xC0) == 0x80) {
        const char32_t cp = (static_cast<char32_t>(b0 & 0x0F) << 12) |
                            (static_cast<char32_t>(b1 & 0x3F) << 6) | (b2 & 0x3F);
        if (cp >= 0xF000 && cp <= 0xF0FF) {
          out.push_back(static_cast<char>(cp - 0xF000));
          i += 3;
          continue;
        }
      }
    }
    out.push_back(static_cast<char>(b0));
    ++i;
  }
  return out;
}

}  // namespace wsld
