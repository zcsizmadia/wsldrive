#pragma once

#include <string>
#include <string_view>

namespace wsld::platform::win {

/// UTF-16 <-> UTF-8 conversions via the Win32 code-page APIs. Invalid sequences
/// are replaced rather than rejected so file names never fail to convert.
[[nodiscard]] std::string to_utf8(std::wstring_view w);
[[nodiscard]] std::wstring to_wide(std::string_view s);

}  // namespace wsld::platform::win
