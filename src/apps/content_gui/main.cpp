#include "esm/content_named_pipe.hpp"
#include "esm/content_settings.hpp"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr wchar_t window_class[] = L"EsmContentSearchWindow";
constexpr UINT message_search_complete = WM_APP + 1;
constexpr UINT message_status_complete = WM_APP + 2;
constexpr UINT_PTR search_timer = 1;
constexpr UINT_PTR status_timer = 2;
constexpr int control_search = 101;
constexpr int control_results = 102;
constexpr int control_status = 103;

struct SearchCompletion {
    std::uint64_t generation{};
    esm::ContentPipeSearchResult result;
};

struct StatusCompletion {
    esm::ContentPipeStatusResult result;
};

struct App {
    HWND window{};
    HWND search{};
    HWND results{};
    HWND status{};
    HFONT font{};
    std::wstring pipe{esm::default_content_pipe_name};
    std::filesystem::path config_path{esm::default_content_config_path()};
    bool service_start_attempted{};
    bool pipe_overridden{};
    std::atomic<std::uint64_t> generation{};
    std::vector<esm::ContentSearchHit> hits;
};

App* app_from(HWND window) {
    return reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

void set_status(App& app, std::wstring text) {
    SetWindowTextW(app.status, text.c_str());
}

std::wstring window_text(HWND window) {
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(window, result.data(), length + 1);
    result.resize(static_cast<std::size_t>((std::max)(copied, 0)));
    return result;
}

void clear_results(App& app) {
    app.hits.clear();
    ListView_DeleteAllItems(app.results);
}

void add_column(HWND list, int index, int width, const wchar_t* title) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<wchar_t*>(title);
    column.cx = width;
    column.iSubItem = index;
    ListView_InsertColumn(list, index, &column);
}

void populate_results(App& app, esm::ContentSearchResponse response) {
    SendMessageW(app.results, WM_SETREDRAW, FALSE, 0);
    clear_results(app);
    app.hits = std::move(response.hits);
    for (std::size_t index = 0; index < app.hits.size(); ++index) {
        const auto& hit = app.hits[index];
        const auto name = hit.path.filename().wstring();
        const auto parent = hit.path.parent_path().wstring();
        const auto relevance = std::to_wstring(hit.relevance_percent) + L"%";
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(index);
        item.pszText = const_cast<wchar_t*>(name.c_str());
        ListView_InsertItem(app.results, &item);
        ListView_SetItemText(app.results, static_cast<int>(index), 1,
                             const_cast<wchar_t*>(parent.c_str()));
        ListView_SetItemText(app.results, static_cast<int>(index), 2,
                             const_cast<wchar_t*>(hit.snippet.c_str()));
        ListView_SetItemText(app.results, static_cast<int>(index), 3,
                             const_cast<wchar_t*>(relevance.c_str()));
    }
    SendMessageW(app.results, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(app.results, nullptr, TRUE);
    set_status(app, L"找到约 " + std::to_wstring(response.estimated_matches) +
                        L" 个内容匹配，显示 " +
                        std::to_wstring(app.hits.size()) + L" 条");
}

void start_search(App& app) {
    const auto query = window_text(app.search);
    const auto generation = app.generation.fetch_add(1) + 1;
    if (query.empty()) {
        clear_results(app);
        set_status(app, L"输入内容关键词开始搜索");
        return;
    }
    set_status(app, L"正在搜索内容……");
    const auto pipe = app.pipe;
    const auto window = app.window;
    std::thread([query, generation, pipe, window] {
        auto completion = std::make_unique<SearchCompletion>();
        completion->generation = generation;
        esm::ContentIpcSearchRequest request;
        request.query = query;
        request.limit = 100;
        completion->result = esm::query_content_named_pipe_search(
            pipe, request, 5'000);
        if (!PostMessageW(window, message_search_complete, 0,
                          reinterpret_cast<LPARAM>(completion.get()))) return;
        completion.release();
    }).detach();
}

void request_status(App& app) {
    const auto pipe = app.pipe;
    const auto window = app.window;
    std::thread([pipe, window] {
        auto completion = std::make_unique<StatusCompletion>();
        completion->result = esm::query_content_named_pipe_status(pipe, 500);
        if (!PostMessageW(window, message_status_complete, 0,
                          reinterpret_cast<LPARAM>(completion.get()))) return;
        completion.release();
    }).detach();
}

void draw_snippet_cell(App& app, NMLVCUSTOMDRAW& custom) {
    const auto row = static_cast<std::size_t>(custom.nmcd.dwItemSpec);
    if (row >= app.hits.size()) return;
    RECT rectangle{};
    ListView_GetSubItemRect(app.results, static_cast<int>(row), 2,
                            LVIR_BOUNDS, &rectangle);
    const bool selected = (ListView_GetItemState(
        app.results, static_cast<int>(row), LVIS_SELECTED) & LVIS_SELECTED) != 0;
    const COLORREF background = selected
        ? GetSysColor(COLOR_HIGHLIGHT)
        : GetSysColor(COLOR_WINDOW);
    const COLORREF foreground = selected
        ? GetSysColor(COLOR_HIGHLIGHTTEXT)
        : GetSysColor(COLOR_WINDOWTEXT);
    const COLORREF highlight_background = selected
        ? RGB(255, 176, 64)
        : RGB(255, 230, 128);
    const COLORREF highlight_foreground = RGB(24, 24, 24);
    HBRUSH brush = CreateSolidBrush(background);
    FillRect(custom.nmcd.hdc, &rectangle, brush);
    DeleteObject(brush);
    SaveDC(custom.nmcd.hdc);
    IntersectClipRect(custom.nmcd.hdc, rectangle.left, rectangle.top,
                      rectangle.right, rectangle.bottom);
    SetBkMode(custom.nmcd.hdc, TRANSPARENT);
    SelectObject(custom.nmcd.hdc, app.font);

    const auto& hit = app.hits[row];
    int x = rectangle.left + 6;
    const int y = rectangle.top + 3;
    std::size_t offset = 0;
    auto draw_segment = [&](std::wstring_view segment, bool highlighted) {
        if (segment.empty() || x >= rectangle.right) return;
        SIZE extent{};
        GetTextExtentPoint32W(custom.nmcd.hdc, segment.data(),
                              static_cast<int>(segment.size()), &extent);
        if (highlighted) {
            RECT highlight_rect{x, rectangle.top + 1,
                                (std::min)(x + extent.cx, rectangle.right),
                                rectangle.bottom - 1};
            HBRUSH highlight_brush = CreateSolidBrush(highlight_background);
            FillRect(custom.nmcd.hdc, &highlight_rect, highlight_brush);
            DeleteObject(highlight_brush);
            SetTextColor(custom.nmcd.hdc, highlight_foreground);
        } else {
            SetTextColor(custom.nmcd.hdc, foreground);
        }
        TextOutW(custom.nmcd.hdc, x, y, segment.data(),
                 static_cast<int>(segment.size()));
        x += extent.cx;
    };
    for (const auto& range : hit.highlights) {
        const auto start = (std::min<std::size_t>)(range.start_utf16,
                                                   hit.snippet.size());
        const auto end = (std::min<std::size_t>)(
            start + range.length_utf16, hit.snippet.size());
        if (start > offset)
            draw_segment(std::wstring_view(hit.snippet).substr(offset,
                                                               start - offset),
                         false);
        if (end > start)
            draw_segment(std::wstring_view(hit.snippet).substr(start,
                                                               end - start),
                         true);
        offset = end;
    }
    if (offset < hit.snippet.size())
        draw_segment(std::wstring_view(hit.snippet).substr(offset), false);
    RestoreDC(custom.nmcd.hdc, -1);
}

LRESULT handle_custom_draw(App& app, NMLVCUSTOMDRAW& custom) {
    switch (custom.nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
        return CDRF_NOTIFYSUBITEMDRAW;
    default:
        if (custom.nmcd.dwDrawStage ==
                (CDDS_ITEMPREPAINT | CDDS_SUBITEM) &&
            custom.iSubItem == 2) {
            draw_snippet_cell(app, custom);
            return CDRF_SKIPDEFAULT;
        }
        return CDRF_DODEFAULT;
    }
}

void layout(App& app) {
    RECT client{};
    GetClientRect(app.window, &client);
    constexpr int margin = 8;
    constexpr int search_height = 30;
    constexpr int status_height = 24;
    MoveWindow(app.search, margin, margin,
               (std::max<LONG>)(0, client.right - margin * 2), search_height, TRUE);
    MoveWindow(app.results, margin, margin + search_height + 6,
               (std::max<LONG>)(0, client.right - margin * 2),
               (std::max<LONG>)(0, client.bottom - search_height - status_height -
                                  margin * 2 - 10),
               TRUE);
    MoveWindow(app.status, margin, client.bottom - status_height - 2,
               (std::max<LONG>)(0, client.right - margin * 2), status_height, TRUE);
}

std::filesystem::path current_executable_directory() {
    std::vector<wchar_t> buffer(32768);
    const auto length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    return std::filesystem::path(
               std::wstring_view(buffer.data(), static_cast<std::size_t>(length)))
        .parent_path();
}

bool ensure_content_service_running(App& app, std::wstring& error) {
    error.clear();
    if (app.service_start_attempted) return true;
    app.service_start_attempted = true;

    auto settings = esm::default_content_app_settings();
    if (std::filesystem::exists(app.config_path)) {
        if (!esm::load_content_app_settings(app.config_path, settings, error))
            return false;
    } else if (!esm::save_content_app_settings(app.config_path, settings,
                                               error)) {
        return false;
    }
    if (!app.pipe_overridden) app.pipe = settings.pipe_name;

    const auto service_path =
        current_executable_directory() / L"esm_content_service.exe";
    if (!std::filesystem::is_regular_file(service_path)) {
        error = L"\u627e\u4e0d\u5230\u72ec\u7acb\u5185\u5bb9\u670d\u52a1\uff1a" + service_path.wstring();
        return false;
    }
    std::wstring command = L"\"" + service_path.wstring() +
                           L"\" --config \"" +
                           app.config_path.wstring() + L"\"";
    if (app.pipe_overridden)
        command += L" --pipe \"" + app.pipe + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(service_path.c_str(), command.data(), nullptr, nullptr,
                        FALSE, CREATE_NO_WINDOW, nullptr,
                        service_path.parent_path().c_str(), &startup, &process)) {
        error = L"\u65e0\u6cd5\u542f\u52a8\u72ec\u7acb\u5185\u5bb9\u670d\u52a1\uff0c\u9519\u8bef\u7801 " +
                std::to_wstring(GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
    auto* app = app_from(window);
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        app = static_cast<App*>(create->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(app));
    }
    switch (message) {
    case WM_CREATE: {
        app->search = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(control_search),
            nullptr, nullptr);
        SendMessageW(app->search, EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(L"输入文件内容，例如：Xapian 数据库"));
        app->results = CreateWindowExW(
            WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS |
                LVS_SINGLESEL,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(control_results),
            nullptr, nullptr);
        ListView_SetExtendedListViewStyle(
            app->results, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                              LVS_EX_LABELTIP);
        add_column(app->results, 0, 190, L"名称");
        add_column(app->results, 1, 300, L"路径");
        add_column(app->results, 2, 560, L"内容匹配");
        add_column(app->results, 3, 80, L"相关度");
        app->status = CreateWindowExW(
            0, L"STATIC", L"正在连接独立内容服务……",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(control_status), nullptr, nullptr);
        app->font = CreateFontW(
            -15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        SendMessageW(app->search, WM_SETFONT,
                     reinterpret_cast<WPARAM>(app->font), TRUE);
        SendMessageW(app->results, WM_SETFONT,
                     reinterpret_cast<WPARAM>(app->font), TRUE);
        SendMessageW(app->status, WM_SETFONT,
                     reinterpret_cast<WPARAM>(app->font), TRUE);
        SetTimer(window, status_timer, 2000, nullptr);
        std::wstring service_error;
        if (!ensure_content_service_running(*app, service_error))
            set_status(*app, std::move(service_error));
        request_status(*app);
        layout(*app);
        SetFocus(app->search);
        return 0;
    }
    case WM_SIZE:
        if (app) layout(*app);
        return 0;
    case WM_COMMAND:
        if (app && LOWORD(wparam) == control_search &&
            HIWORD(wparam) == EN_CHANGE) {
            KillTimer(window, search_timer);
            SetTimer(window, search_timer, 180, nullptr);
        }
        return 0;
    case WM_TIMER:
        if (!app) break;
        if (wparam == search_timer) {
            KillTimer(window, search_timer);
            start_search(*app);
        } else if (wparam == status_timer) {
            request_status(*app);
        }
        return 0;
    case WM_NOTIFY:
        if (app) {
            const auto* header = reinterpret_cast<NMHDR*>(lparam);
            if (header->hwndFrom == app->results &&
                header->code == NM_CUSTOMDRAW) {
                return handle_custom_draw(
                    *app, *reinterpret_cast<NMLVCUSTOMDRAW*>(lparam));
            }
            if (header->hwndFrom == app->results && header->code == NM_DBLCLK) {
                const int selected = ListView_GetNextItem(
                    app->results, -1, LVNI_SELECTED);
                if (selected >= 0 &&
                    static_cast<std::size_t>(selected) < app->hits.size()) {
                    ShellExecuteW(window, L"open",
                                  app->hits[static_cast<std::size_t>(selected)]
                                      .path.c_str(),
                                  nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
        }
        return 0;
    case message_search_complete:
        if (app) {
            std::unique_ptr<SearchCompletion> completion(
                reinterpret_cast<SearchCompletion*>(lparam));
            if (completion->generation != app->generation.load()) return 0;
            if (completion->result.error != ERROR_SUCCESS) {
                set_status(*app,
                           L"内容服务不可用或查询失败，错误码 " +
                               std::to_wstring(completion->result.error));
            } else {
                populate_results(*app,
                                 std::move(completion->result.response.result));
            }
        }
        return 0;
    case message_status_complete:
        if (app) {
            std::unique_ptr<StatusCompletion> completion(
                reinterpret_cast<StatusCompletion*>(lparam));
            if (completion->result.error != ERROR_SUCCESS) {
                if (window_text(app->search).empty())
                    set_status(*app, L"独立内容服务不可用；现有文件名搜索不受影响");
            } else if (window_text(app->search).empty()) {
                const auto& status = completion->result.response.status;
                set_status(*app, status.message + L"，文档数 " +
                                     std::to_wstring(status.documents));
            }
        }
        return 0;
    case WM_DESTROY:
        if (app && app->font) DeleteObject(app->font);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);
    App app;
    int argc{};
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int index = 1; index < argc; ++index) {
            const std::wstring_view argument(argv[index]);
            if (argument == L"--config" && index + 1 < argc) {
                app.config_path = argv[++index];
            }
        }
        auto settings = esm::default_content_app_settings();
        std::wstring settings_error;
        if (std::filesystem::exists(app.config_path) &&
            esm::load_content_app_settings(app.config_path, settings,
                                           settings_error)) {
            app.pipe = settings.pipe_name;
        }
        for (int index = 1; index < argc; ++index) {
            const std::wstring_view argument(argv[index]);
            if (argument == L"--config" && index + 1 < argc) {
                ++index;
            } else if (argument == L"--pipe" && index + 1 < argc) {
                app.pipe = argv[++index];
                app.pipe_overridden = true;
            } else if (argc == 2 && !argument.starts_with(L"--")) {
                app.pipe = argument;
                app.pipe_overridden = true;
            }
        }
        LocalFree(argv);
    }

    WNDCLASSEXW window_class_info{};
    window_class_info.cbSize = sizeof(window_class_info);
    window_class_info.lpfnWndProc = window_proc;
    window_class_info.hInstance = instance;
    window_class_info.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class_info.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    window_class_info.hIconSm = window_class_info.hIcon;
    window_class_info.hbrBackground =
        reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class_info.lpszClassName = window_class;
    if (!RegisterClassExW(&window_class_info)) return 1;

    HWND window = CreateWindowExW(
        0, window_class, L"Everything SM 内容搜索",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        1180, 680, nullptr, nullptr, instance, &app);
    if (!window) return 1;
    ShowWindow(window, show);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
