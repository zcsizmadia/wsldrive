#include "core/unicode.hpp"

namespace wsld {

namespace {

// Decodes one UTF-8 sequence starting at in[i]. On success returns the code point
// and advances i. On malformed input returns std::nullopt and leaves i untouched.
bool decode_utf8(std::string_view in, std::size_t& i, char32_t& cp) noexcept {
  const auto b0 = static_cast<unsigned char>(in[i]);
  std::size_t len;
  if (b0 < 0x80) {
    cp = b0;
    i += 1;
    return true;
  } else if ((b0 & 0xE0) == 0xC0) {
    len = 2;
    cp = b0 & 0x1F;
  } else if ((b0 & 0xF0) == 0xE0) {
    len = 3;
    cp = b0 & 0x0F;
  } else if ((b0 & 0xF8) == 0xF0) {
    len = 4;
    cp = b0 & 0x07;
  } else {
    return false;
  }
  if (i + len > in.size()) return false;
  for (std::size_t k = 1; k < len; ++k) {
    const auto b = static_cast<unsigned char>(in[i + k]);
    if ((b & 0xC0) != 0x80) return false;
    cp = (cp << 6) | (b & 0x3F);
  }
  // Reject overlong encodings and surrogates so that folding never produces a
  // different byte sequence for an equivalent, canonically encoded name.
  if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
      (cp >= 0xD800 && cp <= 0xDFFF))
    return false;
  i += len;
  return true;
}

void encode_utf8(char32_t cp, std::string& out) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

}  // namespace

char32_t casefold_codepoint(char32_t cp) noexcept {
  if (cp < 0x80) return (cp >= 'A' && cp <= 'Z') ? cp + 0x20 : cp;
  // Latin-1 Supplement: À..Þ except × (U+00D7).
  if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) return cp + 0x20;
  // Latin Extended-A. Mostly alternating upper/lower pairs with a few irregulars.
  if (cp >= 0x100 && cp <= 0x17F) {
    switch (cp) {
      case 0x130: return 0x69;   // İ -> i (simple fold, no dot-above)
      case 0x131: return 0x131;  // ı stays
      case 0x138: return 0x138;  // ĸ stays
      case 0x149: return 0x149;  // ŉ stays
      case 0x178: return 0xFF;   // Ÿ -> ÿ
      case 0x17F: return 0x73;   // ſ -> s
      default: break;
    }
    // U+0139..U+0148 and U+0179..U+017E: odd is upper, even is lower.
    if ((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E)) return (cp & 1) ? cp + 1 : cp;
    // Remaining pairs: even is upper, odd is lower.
    return (cp & 1) ? cp : cp + 1;
  }
  // Greek capitals Α..Ω except the unassigned U+03A2.
  if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) return cp + 0x20;
  // Cyrillic capitals.
  if (cp >= 0x410 && cp <= 0x42F) return cp + 0x20;
  if (cp >= 0x400 && cp <= 0x40F) return cp + 0x50;
  return cp;
}

bool is_ascii(std::string_view s) noexcept {
  for (const char c : s)
    if (static_cast<unsigned char>(c) >= 0x80) return false;
  return true;
}

void casefold_append(std::string_view in, std::string& out) {
  out.reserve(out.size() + in.size());
  std::size_t i = 0;
  while (i < in.size()) {
    const auto b = static_cast<unsigned char>(in[i]);
    if (b < 0x80) {
      out.push_back(static_cast<char>((b >= 'A' && b <= 'Z') ? b + 0x20 : b));
      ++i;
      continue;
    }
    char32_t cp;
    if (!decode_utf8(in, i, cp)) {
      out.push_back(static_cast<char>(b));
      ++i;
      continue;
    }
    encode_utf8(casefold_codepoint(cp), out);
  }
}

std::string casefold(std::string_view in) {
  std::string out;
  casefold_append(in, out);
  return out;
}

}  // namespace wsld
