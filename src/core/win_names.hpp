#pragma once

#include <string>
#include <string_view>

namespace wsld {

/// Maps between a raw POSIX filename (as stored on ext4) and the form shown on a
/// Windows volume. Characters that are illegal in Windows names — the reserved
/// set < > : " | ? *, ASCII control codes, and a trailing '.' or ' ' — are
/// remapped to the Unicode private-use area U+F0xx (raw byte + 0xF000), the same
/// convention WSL and Cygwin use, so they round-trip losslessly.
///
/// Names are UTF-8 on both sides (WinFsp-FUSE uses UTF-8). '/' is never present
/// in a single name and is never remapped.

/// raw ext4 name -> name safe to present on a Windows volume.
[[nodiscard]] std::string escape_for_windows(std::string_view raw);

/// Windows-presented name -> raw ext4 name. Inverse of escape_for_windows for
/// any input; also decodes stray U+F0xx code points a caller may pass back.
[[nodiscard]] std::string unescape_from_windows(std::string_view shown);

/// True if `raw` contains anything that escape_for_windows would remap.
[[nodiscard]] bool needs_escaping(std::string_view raw) noexcept;

}  // namespace wsld
