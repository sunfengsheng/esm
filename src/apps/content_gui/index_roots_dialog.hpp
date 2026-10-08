#pragma once

#include "esm/content_settings.hpp"

#include <windows.h>

#include <string>

namespace esm::content_gui {
// Returns true only when validated settings were accepted. A false return with
// an empty error means that the user cancelled the dialog.
[[nodiscard]] bool show_index_roots_dialog(
    HWND owner,
    ContentAppSettings& settings,
    std::wstring& error);
} // namespace esm::content_gui
