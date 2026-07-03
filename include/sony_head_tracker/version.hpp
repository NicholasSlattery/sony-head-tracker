// version.hpp
// Single C++-side source of truth for the application version. The resource
// script (app.rc) and manifest (app.manifest) still carry their own copies;
// keeping those in sync from one place is tracked as a separate build task.
#pragma once

#include <string_view>

namespace sony {

inline constexpr std::wstring_view kVersion = L"1.3.0";

} // namespace sony
