#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace esm::gui {
struct FormField {
    std::wstring label;
    std::wstring value;
    bool multiline{};
    bool read_only{};
};
struct FormCheck {
    std::wstring label;
    bool checked{};
};

bool show_form(HWND owner, const std::wstring& title,
               std::vector<FormField>& fields,
               std::vector<FormCheck>& checks,
               const std::wstring& accept_text = L"\u786e\u5b9a");
}
