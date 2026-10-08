#include "index_roots_dialog.hpp"

#include <shlobj.h>

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

namespace esm::content_gui {
namespace {
constexpr wchar_t dialog_class[] = L"EsmContentIndexRootsDialog";
constexpr int control_roots = 3101;
constexpr int control_add_root = 3102;
constexpr int control_remove_root = 3103;
constexpr int control_all_fixed = 3104;
constexpr int control_excludes = 3105;
constexpr int control_add_exclude = 3106;
constexpr int control_remove_exclude = 3107;
constexpr int control_default_excludes = 3108;
constexpr int control_maximum_mib = 3109;

struct DialogState {
    HWND window{};
    HWND roots{};
    HWND excludes{};
    HWND all_fixed{};
    HWND default_excludes{};
    HWND maximum_mib{};
    HFONT font{};
    ContentAppSettings editing;
    ContentAppSettings* output{};
    bool original_all_fixed{};
    bool done{};
    bool applied{};
};

DialogState* state_from(HWND window) {
    return reinterpret_cast<DialogState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
}

HWND add_control(DialogState& state, DWORD extended_style,
                 const wchar_t* class_name, const wchar_t* text, DWORD style,
                 int x, int y, int width, int height, int identifier) {
    auto control = CreateWindowExW(
        extended_style, class_name, text, WS_CHILD | WS_VISIBLE | style,
        x, y, width, height, state.window,
        identifier == 0 ? nullptr : reinterpret_cast<HMENU>(identifier),
        GetModuleHandleW(nullptr), nullptr);
    if (control && state.font) {
        SendMessageW(control, WM_SETFONT,
                     reinterpret_cast<WPARAM>(state.font), TRUE);
    }
    return control;
}

void fill_list(HWND list, const std::vector<std::filesystem::path>& paths) {
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (const auto& path : paths) {
        SendMessageW(list, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(path.c_str()));
    }
}

std::filesystem::path choose_folder(HWND owner, const wchar_t* title) {
    BROWSEINFOW info{};
    info.hwndOwner = owner;
    info.lpszTitle = title;
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE |
                   BIF_NONEWFOLDERBUTTON;
    auto* item = SHBrowseForFolderW(&info);
    if (!item) return {};
    std::vector<wchar_t> path(32768);
    const bool ok = SHGetPathFromIDListW(item, path.data()) != FALSE;
    CoTaskMemFree(item);
    return ok ? std::filesystem::path(path.data()) : std::filesystem::path{};
}

void add_path(DialogState& state, bool root) {
    const auto selected = choose_folder(
        state.window, root ? L"选择要建立内容索引的根目录"
                           : L"选择要排除的目录");
    if (selected.empty()) return;
    auto& paths = root ? state.editing.roots : state.editing.excluded_paths;
    paths.push_back(selected);
    fill_list(root ? state.roots : state.excludes, paths);
    const auto index = static_cast<WPARAM>(paths.size() - 1);
    SendMessageW(root ? state.roots : state.excludes, LB_SETCURSEL, index, 0);
}

void remove_selected(DialogState& state, bool root) {
    auto list = root ? state.roots : state.excludes;
    const auto selected = SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (selected == LB_ERR) return;
    auto& paths = root ? state.editing.roots : state.editing.excluded_paths;
    if (static_cast<std::size_t>(selected) < paths.size()) {
        paths.erase(paths.begin() + selected);
        fill_list(list, paths);
        if (!paths.empty()) {
            const auto next = (std::min<std::size_t>)(
                static_cast<std::size_t>(selected), paths.size() - 1);
            SendMessageW(list, LB_SETCURSEL, next, 0);
        }
    }
}

bool read_maximum_mib(DialogState& state, std::size_t& maximum_bytes) {
    wchar_t text[32]{};
    GetWindowTextW(state.maximum_mib, text,
                   static_cast<int>(std::size(text)));
    wchar_t* end{};
    const auto value = std::wcstoul(text, &end, 10);
    while (end && *end == L' ') ++end;
    if (text[0] == L'\0' || !end || *end != L'\0' || value < 1 ||
        value > 64) {
        MessageBoxW(state.window,
                    L"单文件大小上限必须是 1–64 之间的整数。",
                    L"内容索引设置", MB_OK | MB_ICONWARNING);
        SetFocus(state.maximum_mib);
        SendMessageW(state.maximum_mib, EM_SETSEL, 0, -1);
        return false;
    }
    maximum_bytes = static_cast<std::size_t>(value) * 1024U * 1024U;
    return true;
}

void apply(DialogState& state) {
    state.editing.all_fixed =
        SendMessageW(state.all_fixed, BM_GETCHECK, 0, 0) == BST_CHECKED;
    state.editing.use_default_excludes =
        SendMessageW(state.default_excludes, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!read_maximum_mib(state, state.editing.maximum_bytes)) return;

    std::wstring validation_error;
    if (!validate_content_app_settings(state.editing, validation_error)) {
        MessageBoxW(state.window, validation_error.c_str(), L"内容索引设置",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    if (state.editing.all_fixed && !state.original_all_fixed) {
        const auto answer = MessageBoxW(
            state.window,
            L"索引所有固定磁盘可能需要较长时间，并会增加 CPU、"
            L"磁盘 I/O 和索引数据库占用。\n\n是否继续？",
            L"确认索引所有固定磁盘",
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (answer != IDYES) return;
    }
    *state.output = state.editing;
    state.applied = true;
    state.done = true;
    DestroyWindow(state.window);
}

LRESULT CALLBACK dialog_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
    auto* state = state_from(window);
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<DialogState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }
    case WM_CREATE: {
        state->font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                                  FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        add_control(*state, 0, L"STATIC", L"索引根：", SS_LEFT,
                    16, 14, 300, 20, 0);
        state->roots = add_control(
            *state, WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            LBS_NOINTEGRALHEIGHT | LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP, 16,
            36, 560, 140, control_roots);
        add_control(*state, 0, L"BUTTON", L"添加文件夹…",
                    BS_PUSHBUTTON | WS_TABSTOP, 590, 36, 120, 30,
                    control_add_root);
        add_control(*state, 0, L"BUTTON", L"移除",
                    BS_PUSHBUTTON | WS_TABSTOP, 590, 74, 120, 30,
                    control_remove_root);
        state->all_fixed = add_control(
            *state, 0, L"BUTTON", L"索引所有固定磁盘",
            BS_AUTOCHECKBOX | WS_TABSTOP, 16, 184, 300, 24,
            control_all_fixed);

        add_control(*state, 0, L"STATIC", L"排除目录：", SS_LEFT,
                    16, 222, 300, 20, 0);
        state->excludes = add_control(
            *state, WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            LBS_NOINTEGRALHEIGHT | LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP, 16,
            244, 560, 140, control_excludes);
        add_control(*state, 0, L"BUTTON", L"添加文件夹…",
                    BS_PUSHBUTTON | WS_TABSTOP, 590, 244, 120, 30,
                    control_add_exclude);
        add_control(*state, 0, L"BUTTON", L"移除",
                    BS_PUSHBUTTON | WS_TABSTOP, 590, 282, 120, 30,
                    control_remove_exclude);
        state->default_excludes = add_control(
            *state, 0, L"BUTTON",
            L"使用默认排除项（例如 Windows、.git、node_modules）",
            BS_AUTOCHECKBOX | WS_TABSTOP, 16, 394, 470, 24,
            control_default_excludes);

        add_control(*state, 0, L"STATIC", L"单文件大小上限：", SS_LEFT,
                    16, 434, 145, 24, 0);
        state->maximum_mib = add_control(
            *state, WS_EX_CLIENTEDGE, L"EDIT", L"",
            ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, 164, 430, 72, 28,
            control_maximum_mib);
        add_control(*state, 0, L"STATIC", L"MiB（1–64）", SS_LEFT, 244,
                    434, 120, 24, 0);
        add_control(
            *state, 0, L"STATIC",
            L"应用后将安全重启内容服务。旧分片不会自动删除；"
            L"排除项或大小上限变更不会清理已建索引的文档。",
            SS_LEFT, 16, 468, 694, 38, 0);
        add_control(*state, 0, L"BUTTON", L"应用", BS_DEFPUSHBUTTON | WS_TABSTOP,
                    478, 514, 110, 32, IDOK);
        add_control(*state, 0, L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP,
                    600, 514, 110, 32, IDCANCEL);

        fill_list(state->roots, state->editing.roots);
        fill_list(state->excludes, state->editing.excluded_paths);
        SendMessageW(state->all_fixed, BM_SETCHECK,
                     state->editing.all_fixed ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(state->default_excludes, BM_SETCHECK,
                     state->editing.use_default_excludes ? BST_CHECKED
                                                         : BST_UNCHECKED,
                     0);
        const auto mib = (std::max<std::size_t>)(
            1, state->editing.maximum_bytes / (1024U * 1024U));
        SetWindowTextW(state->maximum_mib, std::to_wstring(mib).c_str());
        return 0;
    }
    case WM_COMMAND:
        if (!state) break;
        switch (LOWORD(wparam)) {
        case control_add_root:
            add_path(*state, true);
            return 0;
        case control_remove_root:
            remove_selected(*state, true);
            return 0;
        case control_add_exclude:
            add_path(*state, false);
            return 0;
        case control_remove_exclude:
            remove_selected(*state, false);
            return 0;
        case IDOK:
            apply(*state);
            return 0;
        case IDCANCEL:
            state->done = true;
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (state) state->done = true;
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (state) {
            state->done = true;
            if (state->font) {
                DeleteObject(state->font);
                state->font = nullptr;
            }
        }
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool register_dialog_class(HINSTANCE instance, std::wstring& error) {
    WNDCLASSEXW existing{};
    existing.cbSize = sizeof(existing);
    if (GetClassInfoExW(instance, dialog_class, &existing)) return true;
    WNDCLASSEXW descriptor{};
    descriptor.cbSize = sizeof(descriptor);
    descriptor.lpfnWndProc = dialog_proc;
    descriptor.hInstance = instance;
    descriptor.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    descriptor.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    descriptor.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    descriptor.lpszClassName = dialog_class;
    if (RegisterClassExW(&descriptor)) return true;
    error = L"无法创建索引根设置窗口，错误码 " +
            std::to_wstring(GetLastError());
    return false;
}
} // namespace

bool show_index_roots_dialog(HWND owner, ContentAppSettings& settings,
                             std::wstring& error) {
    error.clear();
    const auto instance = GetModuleHandleW(nullptr);
    if (!register_dialog_class(instance, error)) return false;

    DialogState state;
    state.editing = settings;
    state.output = &settings;
    state.original_all_fixed = settings.all_fixed;

    RECT owner_rect{};
    GetWindowRect(owner, &owner_rect);
    constexpr int width = 746;
    constexpr int height = 600;
    const int x = owner_rect.left +
                  ((owner_rect.right - owner_rect.left) - width) / 2;
    const int y = owner_rect.top +
                  ((owner_rect.bottom - owner_rect.top) - height) / 2;
    const auto window = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, dialog_class,
        L"管理内容索引根",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
        (std::max)(0, x), (std::max)(0, y), width, height, owner, nullptr,
        instance, &state);
    if (!window) {
        error = L"无法打开索引根设置，错误码 " +
                std::to_wstring(GetLastError());
        return false;
    }

    EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    SetFocus(state.roots);
    MSG message{};
    while (!state.done) {
        const auto result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) {
            state.done = true;
            if (result == 0) PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
            continue;
        }
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (IsWindow(window)) DestroyWindow(window);
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    return state.applied;
}
} // namespace esm::content_gui
