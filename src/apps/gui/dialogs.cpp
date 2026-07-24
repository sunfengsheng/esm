#include "dialogs.hpp"
#include <algorithm>

namespace esm::gui {
namespace {
struct State {
    std::vector<FormField>* fields{};
    std::vector<FormCheck>* checks{};
    std::vector<HWND> edits;
    std::vector<HWND> checkboxes;
    bool accepted{};
};
std::wstring control_text(HWND window) {
    const int n = GetWindowTextLengthW(window);
    std::wstring value(static_cast<std::size_t>(std::max(0, n)) + 1, L'\0');
    const int copied = GetWindowTextW(window, value.data(), n + 1);
    value.resize(static_cast<std::size_t>(std::max(0, copied)));
    return value;
}
LRESULT CALLBACK form_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    State* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        state = static_cast<State*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK && state) {
            for (std::size_t i = 0; i < state->edits.size(); ++i)
                (*state->fields)[i].value = control_text(state->edits[i]);
            for (std::size_t i = 0; i < state->checkboxes.size(); ++i)
                (*state->checks)[i].checked =
                    SendMessageW(state->checkboxes[i], BM_GETCHECK, 0, 0) == BST_CHECKED;
            state->accepted = true;
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) {
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
void set_font(HWND window, HFONT font) {
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}
}

bool show_form(HWND owner, const std::wstring& title,
               std::vector<FormField>& fields,
               std::vector<FormCheck>& checks,
               const std::wstring& accept_text) {
    static bool registered = false;
    HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = form_proc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"EsmFormDialog";
        registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    if (!registered) return false;

    int content_height = 18;
    for (const auto& field : fields) content_height += 24 + (field.multiline ? 118 : 28);
    content_height += static_cast<int>(checks.size()) * 28 + 58;
    const int width = 600;
    const int height = std::clamp(content_height, 220, 760);
    RECT owner_rect{}; GetWindowRect(owner, &owner_rect);
    const int x = std::max<int>(0, static_cast<int>(owner_rect.left + ((owner_rect.right - owner_rect.left) - width) / 2));
    const int y = std::max<int>(0, static_cast<int>(owner_rect.top + ((owner_rect.bottom - owner_rect.top) - height) / 2));
    State state{&fields, &checks};
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        L"EsmFormDialog", title.c_str(), WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, width, height, owner, nullptr, instance, &state);
    if (!dialog) return false;
    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    int top = 14;
    const int control_width = width - 40;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        HWND label = CreateWindowExW(0, L"STATIC", fields[i].label.c_str(),
            WS_CHILD | WS_VISIBLE, 18, top, control_width, 20, dialog, nullptr, instance, nullptr);
        set_font(label, font); top += 22;
        DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL;
        int edit_height = 24;
        if (fields[i].multiline) {
            style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_MULTILINE |
                    ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN;
            edit_height = 112;
        }
        if (fields[i].read_only) style |= ES_READONLY;
        HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", fields[i].value.c_str(),
            style, 18, top, control_width, edit_height, dialog,
            reinterpret_cast<HMENU>(1000 + i), instance, nullptr);
        set_font(edit, font); state.edits.push_back(edit); top += edit_height + 10;
    }
    for (std::size_t i = 0; i < checks.size(); ++i) {
        HWND check = CreateWindowExW(0, L"BUTTON", checks[i].label.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            18, top, control_width, 22, dialog,
            reinterpret_cast<HMENU>(2000 + i), instance, nullptr);
        set_font(check, font);
        SendMessageW(check, BM_SETCHECK, checks[i].checked ? BST_CHECKED : BST_UNCHECKED, 0);
        state.checkboxes.push_back(check); top += 27;
    }
    const int button_y = height - 70;
    HWND ok = CreateWindowExW(0, L"BUTTON", accept_text.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        width - 194, button_y, 78, 26, dialog, reinterpret_cast<HMENU>(IDOK), instance, nullptr);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"取消",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        width - 106, button_y, 78, 26, dialog, reinterpret_cast<HMENU>(IDCANCEL), instance, nullptr);
    set_font(ok, font); set_font(cancel, font);
    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW); UpdateWindow(dialog);
    if (!state.edits.empty()) SetFocus(state.edits.front());
    MSG msg{};
    while (IsWindow(dialog) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dialog, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    EnableWindow(owner, TRUE); SetActiveWindow(owner);
    return state.accepted;
}
}
