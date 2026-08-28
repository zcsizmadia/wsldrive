#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace wsld {

/// Simple case folding used for case-insensitive name lookup (the NTFS-facing view).
///
/// ASCII is folded exactly. For non-ASCII we fold the Latin-1 Supplement, Latin
/// Extended-A, basic Greek and basic Cyrillic ranges algorithmically, which covers
/// the overwhelming majority of real file names without a locale dependency.
/// Anything else passes through unchanged. Full Unicode simple case folding via a
/// generated table is a known follow-up (see plan.md, semantics mapping).
///
/// Invalid UTF-8 bytes are copied through untouched so folding never fails.
void casefold_append(std::string_view in, std::string& out);

[[nodiscard]] std::string casefold(std::string_view in);

/// Returns true if `s` contains no byte >= 0x80.
[[nodiscard]] bool is_ascii(std::string_view s) noexcept;

/// Folds one code point. Exposed for tests.
[[nodiscard]] char32_t casefold_codepoint(char32_t cp) noexcept;

}  // namespace wsld
