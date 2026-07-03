// version.hpp
// C++ view of the version. The value itself lives in version.h (the single
// source shared with app.rc); this only exposes it as sony::kVersion.
#pragma once

#include "version.h"

#include <string_view>

namespace sony {

inline constexpr std::wstring_view kVersion = SHT_VERSION_WSTRING;

} // namespace sony
