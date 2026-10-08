#include "esm/content_named_pipe.hpp"
#include "esm/content_settings.hpp"
#include "index_roots_dialog.hpp"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <richedit.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
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
constexpr int control_status_text = 103;
constexpr int control_clear = 104;
constexpr int control_open = 105;
constexpr int control_open_folder = 106;
constexpr int control_status_dot = 107;
constexpr int control_result_count = 108;
constexpr int control_preview_toggle = 109;
constexpr int control_preview_title = 110;
constexpr int control_preview_path = 111;
constexpr int control_preview_text = 112;
constexpr int command_copy_path = 2001;
constexpr int command_toggle_preview = 2002;
constexpr int command_manage_index_roots = 2003;

enum class ServiceState { unavailable, indexing, ready, searching };

struct SearchCompletion {
    std::uint64_t generation{};
    esm::ContentPipeSearchResult result;
};

struct StatusCompletion {
    esm::ContentPipeStatusResult result;
};

struct App {
    HWND window{};
    HWND search_label{};
    HWND search{};
    HWND clear{};
    HWND open{};
    HWND open_folder{};
    HWND results{};
    HWND status_dot{};
    HWND status_text{};
    HWND result_count{};
    HWND preview_toggle{};
    HWND preview_title{};
    HWND preview_path{};
    HWND preview_text{};
    HMENU view_menu{};
    HMODULE rich_edit_library{};
    bool preview_visible{true};
    HFONT font{};
    HIMAGELIST system_images{};
    std::wstring pipe{esm::default_content_pipe_name};
    std::filesystem::path config_path{esm::default_content_config_path()};
    bool service_start_attempted{};
    bool pipe_overridden{};
    std::atomic<std::uint64_t> generation{};
    std::vector<esm::ContentSearchHit> hits;
    ServiceState service_state{ServiceState::unavailable};
    std::wstring service_text{L"正在连接独立内容服务……"};
    std::wstring activity_text;

    std::mutex work_mutex;
    std::condition_variable work_ready;
    bool shutting_down{};
    bool search_pending{};
    bool status_pending{};
    std::wstring pending_query;
    std::uint64_t pending_generation{};
    std::jthread worker;
};

App* app_from(HWND window) {
    return reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

std::wstring window_text(HWND window) {
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(window, result.data(), length + 1);
    result.resize(static_cast<std::size_t>((std::max)(copied, 0)));
    return result;
}

void update_status_bar(App& app) {
    const auto& text = app.activity_text.empty() ? app.service_text
                                                 : app.activity_text;
    SetWindowTextW(app.status_text, text.c_str());
    InvalidateRect(app.status_dot, nullptr, TRUE);
}

void set_activity(App& app, std::wstring text) {
    app.activity_text = std::move(text);
    update_status_bar(app);
}

void set_result_count(App& app, std::uint64_t estimated = 0) {
    std::wstring text;
    if (!app.hits.empty() || estimated != 0) {
        text = L"约 " + std::to_wstring(estimated) + L" 个匹配 / 显示 " +
               std::to_wstring(app.hits.size()) + L" 条";
    }
    SetWindowTextW(app.result_count, text.c_str());
}

void update_action_state(App& app) {
    const int selected = ListView_GetNextItem(app.results, -1, LVNI_SELECTED);
    const BOOL enabled = selected >= 0 ? TRUE : FALSE;
    EnableWindow(app.open, enabled);
    EnableWindow(app.open_folder, enabled);
}

void set_empty_preview(App& app) {
    if (!app.preview_title) return;
    SetWindowTextW(app.preview_title, L"内容预览");
    SetWindowTextW(app.preview_path, L"");
    SetWindowTextW(app.preview_text,
                   L"选择搜索结果后，这里显示索引中的匹配内容。");
    if (app.rich_edit_library && app.preview_text) {
        CHARFORMAT2W base{};
        base.cbSize = sizeof(base);
        base.dwMask = CFM_COLOR | CFM_BACKCOLOR | CFM_BOLD;
        base.crTextColor = GetSysColor(COLOR_WINDOWTEXT);
        base.crBackColor = GetSysColor(COLOR_WINDOW);
        SendMessageW(app.preview_text, EM_SETSEL, 0, -1);
        SendMessageW(app.preview_text, EM_SETCHARFORMAT, SCF_SELECTION,
                     reinterpret_cast<LPARAM>(&base));
        SendMessageW(app.preview_text, EM_SETSEL, 0, 0);
    }
}

void clear_results(App& app) {
    app.hits.clear();
    ListView_DeleteAllItems(app.results);
    set_result_count(app);
    update_action_state(app);
    set_empty_preview(app);
}

void update_preview(App& app);

void add_column(HWND list, int index, int width, const wchar_t* title) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<wchar_t*>(title);
    column.cx = width;
    column.iSubItem = index;
    ListView_InsertColumn(list, index, &column);
}

int file_icon_index(const std::filesystem::path& path) {
    SHFILEINFOW info{};
    if (!SHGetFileInfoW(path.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                        SHGFI_SYSICONINDEX | SHGFI_SMALLICON |
                            SHGFI_USEFILEATTRIBUTES)) {
        return 0;
    }
    return info.iIcon;
}

void populate_results(App& app, esm::ContentSearchResponse response) {
    SendMessageW(app.results, WM_SETREDRAW, FALSE, 0);
    clear_results(app);
    const auto estimated = response.estimated_matches;
    app.hits = std::move(response.hits);
    for (std::size_t index = 0; index < app.hits.size(); ++index) {
        const auto& hit = app.hits[index];
        const auto name = hit.path.filename().wstring();
        const auto parent = hit.path.parent_path().wstring();
        const auto relevance = std::to_wstring(hit.relevance_percent) + L"%";
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_IMAGE;
        item.iItem = static_cast<int>(index);
        item.iImage = file_icon_index(hit.path);
        item.pszText = const_cast<wchar_t*>(name.c_str());
        ListView_InsertItem(app.results, &item);
        ListView_SetItemText(app.results, static_cast<int>(index), 1,
                             const_cast<wchar_t*>(parent.c_str()));
        ListView_SetItemText(app.results, static_cast<int>(index), 2,
                             const_cast<wchar_t*>(hit.snippet.c_str()));
        ListView_SetItemText(app.results, static_cast<int>(index), 3,
                             const_cast<wchar_t*>(relevance.c_str()));
    }
    if (!app.hits.empty()) {
        ListView_SetItemState(app.results, 0, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
    }
    SendMessageW(app.results, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(app.results, nullptr, TRUE);
    set_result_count(app, estimated);
    set_activity(app, app.hits.empty() ? L"没有找到内容匹配"
                                       : L"查询完成");
    update_action_state(app);
    update_preview(app);
}

void queue_search(App& app) {
    const auto query = window_text(app.search);
    const auto generation = app.generation.fetch_add(1) + 1;
    if (query.empty()) {
        {
            std::lock_guard lock(app.work_mutex);
            app.search_pending = false;
            app.pending_query.clear();
        }
        clear_results(app);
        set_activity(app, {});
        return;
    }
    set_activity(app, L"正在搜索内容……");
    app.service_state = ServiceState::searching;
    InvalidateRect(app.status_dot, nullptr, TRUE);
    {
        std::lock_guard lock(app.work_mutex);
        app.pending_query = query;
        app.pending_generation = generation;
        app.search_pending = true;
    }
    app.work_ready.notify_one();
}

void queue_status(App& app) {
    {
        std::lock_guard lock(app.work_mutex);
        app.status_pending = true;
    }
    app.work_ready.notify_one();
}

void start_worker(App& app) {
    app.worker = std::jthread([&app](std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            bool do_search{};
            bool do_status{};
            std::wstring query;
            std::uint64_t generation{};
            {
                std::unique_lock lock(app.work_mutex);
                app.work_ready.wait(lock, [&] {
                    return app.shutting_down || app.search_pending ||
                           app.status_pending;
                });
                if (app.shutting_down || stop_token.stop_requested()) break;
                if (app.search_pending) {
                    do_search = true;
                    query = app.pending_query;
                    generation = app.pending_generation;
                    app.search_pending = false;
                } else if (app.status_pending) {
                    do_status = true;
                    app.status_pending = false;
                }
            }
            if (do_search) {
                auto completion = std::make_unique<SearchCompletion>();
                completion->generation = generation;
                esm::ContentIpcSearchRequest request;
                request.query = std::move(query);
                request.limit = 100;
                completion->result = esm::query_content_named_pipe_search(
                    app.pipe, request, 2'500);
                if (PostMessageW(app.window, message_search_complete, 0,
                                 reinterpret_cast<LPARAM>(completion.get()))) {
                    completion.release();
                }
            } else if (do_status) {
                auto completion = std::make_unique<StatusCompletion>();
                completion->result =
                    esm::query_content_named_pipe_status(app.pipe, 600);
                if (PostMessageW(app.window, message_status_complete, 0,
                                 reinterpret_cast<LPARAM>(completion.get()))) {
                    completion.release();
                }
            }
        }
    });
}

void stop_worker(App& app) {
    {
        std::lock_guard lock(app.work_mutex);
        app.shutting_down = true;
    }
    app.worker.request_stop();
    app.work_ready.notify_all();
}

int selected_index(const App& app) {
    return ListView_GetNextItem(app.results, -1, LVNI_SELECTED);
}

const esm::ContentSearchHit* selected_hit(const App& app) {
    const int selected = selected_index(app);
    if (selected < 0 || static_cast<std::size_t>(selected) >= app.hits.size())
        return nullptr;
    return &app.hits[static_cast<std::size_t>(selected)];
}

void update_preview(App& app) {
    const auto* hit = selected_hit(app);
    if (!hit) {
        set_empty_preview(app);
        return;
    }

    const std::wstring title = L"内容预览 · " + hit->path.filename().wstring();
    SetWindowTextW(app.preview_title, title.c_str());
    SetWindowTextW(app.preview_path, hit->path.c_str());
    SetWindowTextW(app.preview_text, hit->snippet.c_str());

    CHARFORMAT2W base{};
    base.cbSize = sizeof(base);
    base.dwMask = CFM_COLOR | CFM_BACKCOLOR | CFM_BOLD;
    base.dwEffects = 0;
    base.crTextColor = GetSysColor(COLOR_WINDOWTEXT);
    base.crBackColor = GetSysColor(COLOR_WINDOW);
    SendMessageW(app.preview_text, EM_SETSEL, 0, -1);
    SendMessageW(app.preview_text, EM_SETCHARFORMAT, SCF_SELECTION,
                 reinterpret_cast<LPARAM>(&base));

    CHARFORMAT2W highlight = base;
    highlight.dwEffects = CFE_BOLD;
    highlight.crTextColor = RGB(24, 24, 24);
    highlight.crBackColor = RGB(255, 230, 128);
    for (const auto& range : hit->highlights) {
        const auto start = (std::min<std::size_t>)(range.start_utf16,
                                                   hit->snippet.size());
        const auto end = (std::min<std::size_t>)(
            start + range.length_utf16, hit->snippet.size());
        if (end <= start) continue;
        SendMessageW(app.preview_text, EM_SETSEL, static_cast<WPARAM>(start),
                     static_cast<LPARAM>(end));
        SendMessageW(app.preview_text, EM_SETCHARFORMAT, SCF_SELECTION,
                     reinterpret_cast<LPARAM>(&highlight));
    }
    SendMessageW(app.preview_text, EM_SETSEL, 0, 0);
    SendMessageW(app.preview_text, EM_HIDESELECTION, TRUE, 0);
}

void open_selected(App& app) {
    const auto* hit = selected_hit(app);
    if (!hit) return;
    ShellExecuteW(app.window, L"open", hit->path.c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
}

void open_selected_folder(App& app) {
    const auto* hit = selected_hit(app);
    if (!hit) return;
    const std::wstring arguments = L"/select,\"" + hit->path.wstring() + L"\"";
    ShellExecuteW(app.window, L"open", L"explorer.exe", arguments.c_str(),
                  nullptr, SW_SHOWNORMAL);
}

void copy_selected_path(App& app) {
    const auto* hit = selected_hit(app);
    if (!hit) return;
    const auto path = hit->path.wstring();
    if (!OpenClipboard(app.window)) return;
    EmptyClipboard();
    const auto bytes = (path.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory) {
        if (void* target = GlobalLock(memory)) {
            memcpy(target, path.c_str(), bytes);
            GlobalUnlock(memory);
            if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
    set_activity(app, L"已复制完整路径");
}

void show_result_menu(App& app) {
    if (!selected_hit(app)) return;
    POINT point{};
    GetCursorPos(&point);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, control_open, L"打开(&O)");
    AppendMenuW(menu, MF_STRING, control_open_folder, L"打开所在目录(&F)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, command_copy_path, L"复制完整路径(&C)");
    const auto command = TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, app.window,
        nullptr);
    DestroyMenu(menu);
    if (command == control_open)
        open_selected(app);
    else if (command == control_open_folder)
        open_selected_folder(app);
    else if (command == command_copy_path)
        copy_selected_path(app);
}

void draw_snippet_cell(App& app, NMLVCUSTOMDRAW& custom) {
    const auto row = static_cast<std::size_t>(custom.nmcd.dwItemSpec);
    if (row >= app.hits.size()) return;
    RECT rectangle{};
    ListView_GetSubItemRect(app.results, static_cast<int>(row), 2, LVIR_BOUNDS,
                            &rectangle);
    const bool selected =
        (ListView_GetItemState(app.results, static_cast<int>(row),
                               LVIS_SELECTED) &
         LVIS_SELECTED) != 0;
    const COLORREF background =
        selected ? GetSysColor(COLOR_HIGHLIGHT) : GetSysColor(COLOR_WINDOW);
    const COLORREF foreground = selected ? GetSysColor(COLOR_HIGHLIGHTTEXT)
                                         : GetSysColor(COLOR_WINDOWTEXT);
    const COLORREF highlight_background =
        selected ? RGB(255, 176, 64) : RGB(255, 230, 128);
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
            draw_segment(
                std::wstring_view(hit.snippet).substr(start, end - start), true);
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
            (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
            if (custom.iSubItem == 2) {
                draw_snippet_cell(app, custom);
                return CDRF_SKIPDEFAULT;
            }
        }
        return CDRF_DODEFAULT;
    }
}

void layout(App& app) {
    RECT client{};
    GetClientRect(app.window, &client);
    constexpr int margin = 8;
    constexpr int row_height = 30;
    constexpr int button_gap = 6;
    constexpr int clear_width = 32;
    constexpr int open_width = 72;
    constexpr int folder_width = 96;
    constexpr int preview_button_width = 92;
    constexpr int status_height = 24;
    constexpr int label_width = 48;
    constexpr int pane_gap = 8;
    const int client_width = static_cast<int>(client.right);
    const int client_height = static_cast<int>(client.bottom);
    const int right_buttons = clear_width + open_width + folder_width +
                              preview_button_width + button_gap * 4;
    const int search_width =
        (std::max)(120, client_width - margin * 2 - label_width - right_buttons);

    MoveWindow(app.search_label, margin, margin, label_width - 4, row_height, TRUE);
    MoveWindow(app.search, margin + label_width, margin, search_width, row_height,
               TRUE);
    int x = margin + label_width + search_width + button_gap;
    MoveWindow(app.clear, x, margin, clear_width, row_height, TRUE);
    x += clear_width + button_gap;
    MoveWindow(app.open, x, margin, open_width, row_height, TRUE);
    x += open_width + button_gap;
    MoveWindow(app.open_folder, x, margin, folder_width, row_height, TRUE);
    x += folder_width + button_gap;
    MoveWindow(app.preview_toggle, x, margin, preview_button_width, row_height,
               TRUE);

    const int content_y = margin + row_height + 7;
    const int content_height = (std::max)(
        0, client_height - row_height - status_height - margin * 2 - 9);
    int preview_width{};
    if (app.preview_visible) {
        preview_width = (std::clamp)(client_width / 3, 280, 430);
        preview_width = (std::min)(preview_width,
                                   (std::max)(0, client_width - 430));
    }
    const int list_width = (std::max)(
        0, client_width - margin * 2 -
               (app.preview_visible ? preview_width + pane_gap : 0));
    MoveWindow(app.results, margin, content_y, list_width, content_height, TRUE);

    const int preview_x = margin + list_width + pane_gap;
    const int title_height = 26;
    const int path_height = 42;
    const int preview_text_y = content_y + title_height + path_height + 4;
    const int preview_text_height =
        (std::max)(0, content_height - title_height - path_height - 4);
    const int show = app.preview_visible ? SW_SHOW : SW_HIDE;
    ShowWindow(app.preview_title, show);
    ShowWindow(app.preview_path, show);
    ShowWindow(app.preview_text, show);
    if (app.preview_visible) {
        MoveWindow(app.preview_title, preview_x, content_y, preview_width,
                   title_height, TRUE);
        MoveWindow(app.preview_path, preview_x, content_y + title_height,
                   preview_width, path_height, TRUE);
        MoveWindow(app.preview_text, preview_x, preview_text_y, preview_width,
                   preview_text_height, TRUE);
    }

    const int status_y = client_height - status_height;
    MoveWindow(app.status_dot, margin, status_y + 4, 16, 16, TRUE);
    MoveWindow(app.status_text, margin + 22, status_y,
               (std::max)(0, client_width - 360), status_height, TRUE);
    MoveWindow(app.result_count, (std::max)(margin, client_width - 340), status_y,
               332, status_height, TRUE);

    ListView_SetColumnWidth(app.results, 0,
                            (std::max)(130, list_width * 20 / 100));
    ListView_SetColumnWidth(app.results, 1,
                            (std::max)(170, list_width * 26 / 100));
    ListView_SetColumnWidth(app.results, 2,
                            (std::max)(220, list_width * 46 / 100));
    ListView_SetColumnWidth(app.results, 3, 82);
}

void set_preview_visible(App& app, bool visible) {
    app.preview_visible = visible;
    SetWindowTextW(app.preview_toggle, visible ? L"隐藏预览" : L"显示预览");
    if (app.view_menu) {
        CheckMenuItem(app.view_menu, command_toggle_preview,
                      MF_BYCOMMAND | (visible ? MF_CHECKED : MF_UNCHECKED));
        DrawMenuBar(app.window);
    }
    layout(app);
    if (visible) update_preview(app);
}

bool start_content_service(const App& app, std::wstring& error) {
    const auto executable = std::filesystem::path([] {
        std::wstring buffer(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        buffer.resize(length);
        return buffer;
    }()).parent_path() / L"esm_content_service.exe";
    if (!std::filesystem::exists(executable)) {
        error = L"未找到独立内容服务：" + executable.wstring();
        return false;
    }
    std::wstring command = L"\"" + executable.wstring() + L"\" --config \"" +
                           app.config_path.wstring() + L"\"";
    if (app.pipe_overridden)
        command += L" --pipe \"" + app.pipe + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    if (!CreateProcessW(executable.c_str(), mutable_command.data(), nullptr,
                        nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        executable.parent_path().c_str(), &startup, &process)) {
        error = L"启动独立内容服务失败，错误码 " +
                std::to_wstring(GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool ensure_content_service_running(App& app, std::wstring& error) {
    auto probe = esm::query_content_named_pipe_status(app.pipe, 250);
    if (probe.error == ERROR_SUCCESS) return true;
    if (app.service_start_attempted) {
        error = L"独立内容服务仍不可用，错误码 " +
                std::to_wstring(probe.error);
        return false;
    }
    app.service_start_attempted = true;
    return start_content_service(app, error);
}

std::wstring content_service_mutex_name(std::wstring_view pipe) {
    auto name = std::wstring(L"Local\\EverythingSmContentService-") +
                std::wstring(pipe);
    std::replace(name.begin(), name.end(), L'\\', L'_');
    return name;
}

bool wait_for_content_service_exit(std::wstring_view pipe,
                                   std::uint32_t timeout_ms) {
    const auto mutex_name = content_service_mutex_name(pipe);
    const auto deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        const HANDLE mutex =
            OpenMutexW(SYNCHRONIZE, FALSE, mutex_name.c_str());
        if (!mutex) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND) return true;
        } else {
            CloseHandle(mutex);
        }
        if (GetTickCount64() >= deadline) return false;
        Sleep(50);
    }
}

bool tokens_have_same_user(HANDLE process, std::wstring& error) {
    HANDLE current_token{};
    HANDLE process_token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &current_token) ||
        !OpenProcessToken(process, TOKEN_QUERY, &process_token)) {
        error = L"无法校验旧内容服务的用户身份，错误码 " +
                std::to_wstring(GetLastError());
        if (current_token) CloseHandle(current_token);
        if (process_token) CloseHandle(process_token);
        return false;
    }
    auto read_user = [](HANDLE token, std::vector<std::uint8_t>& bytes) {
        DWORD required{};
        GetTokenInformation(token, TokenUser, nullptr, 0, &required);
        if (required == 0) return false;
        bytes.resize(required);
        return GetTokenInformation(token, TokenUser, bytes.data(), required,
                                   &required) != FALSE;
    };
    std::vector<std::uint8_t> current_user;
    std::vector<std::uint8_t> process_user;
    const bool read = read_user(current_token, current_user) &&
                      read_user(process_token, process_user);
    CloseHandle(current_token);
    CloseHandle(process_token);
    if (!read) {
        error = L"无法读取旧内容服务的用户身份，错误码 " +
                std::to_wstring(GetLastError());
        return false;
    }
    const auto* current =
        reinterpret_cast<const TOKEN_USER*>(current_user.data());
    const auto* target =
        reinterpret_cast<const TOKEN_USER*>(process_user.data());
    if (!EqualSid(current->User.Sid, target->User.Sid)) {
        error = L"旧内容服务不属于当前用户，已拒绝强制停止。";
        return false;
    }
    return true;
}

enum class LegacyStopResult { stopped, canceled, failed };

LegacyStopResult stop_legacy_content_service(HWND owner,
                                             std::uint32_t process_id,
                                             std::wstring& error) {
    error.clear();
    if (process_id == 0 || process_id == GetCurrentProcessId()) {
        error = L"旧内容服务进程标识无效。";
        return LegacyStopResult::failed;
    }
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION |
                                           PROCESS_TERMINATE | SYNCHRONIZE,
                                       FALSE, process_id);
    if (!process) {
        error = L"无法打开旧内容服务进程，错误码 " +
                std::to_wstring(GetLastError());
        return LegacyStopResult::failed;
    }

    std::wstring image_path(32768, L'\0');
    DWORD image_length = static_cast<DWORD>(image_path.size());
    if (!QueryFullProcessImageNameW(process, 0, image_path.data(),
                                    &image_length)) {
        error = L"无法校验旧内容服务路径，错误码 " +
                std::to_wstring(GetLastError());
        CloseHandle(process);
        return LegacyStopResult::failed;
    }
    image_path.resize(image_length);
    const auto filename = std::filesystem::path(image_path).filename().wstring();
    if (CompareStringOrdinal(filename.c_str(), -1,
                             L"esm_content_service.exe", -1, TRUE) !=
        CSTR_EQUAL ||
        !tokens_have_same_user(process, error)) {
        if (error.empty())
            error = L"Named Pipe 所属进程不是可验证的内容服务。";
        CloseHandle(process);
        return LegacyStopResult::failed;
    }

    const auto answer = MessageBoxW(
        owner,
        (L"当前运行的是不支持安全重启的旧版内容服务：\n" +
         image_path +
         L"\n\n为了立即应用新索引根，需要停止一次旧进程。"
         L"这可能中断当前扫描，之后会使用新版服务继续建立索引。\n\n"
         L"是否继续？")
            .c_str(),
        L"停止旧版内容服务",
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (answer != IDYES) {
        CloseHandle(process);
        return LegacyStopResult::canceled;
    }
    if (!TerminateProcess(process, ERROR_PROCESS_ABORTED)) {
        error = L"无法停止旧内容服务，错误码 " +
                std::to_wstring(GetLastError());
        CloseHandle(process);
        return LegacyStopResult::failed;
    }
    const auto wait = WaitForSingleObject(process, 10'000);
    CloseHandle(process);
    if (wait != WAIT_OBJECT_0) {
        error = L"旧内容服务未在 10 秒内退出。";
        return LegacyStopResult::failed;
    }
    return LegacyStopResult::stopped;
}

void manage_index_roots(App& app) {
    auto settings = esm::default_content_app_settings();
    std::wstring error;
    if (std::filesystem::exists(app.config_path) &&
        !esm::load_content_app_settings(app.config_path, settings, error)) {
        MessageBoxW(app.window, error.c_str(), L"内容索引设置",
                    MB_OK | MB_ICONERROR);
        return;
    }
    error.clear();
    if (!esm::content_gui::show_index_roots_dialog(app.window, settings,
                                                   error)) {
        if (!error.empty()) {
            MessageBoxW(app.window, error.c_str(), L"内容索引设置",
                        MB_OK | MB_ICONERROR);
        }
        return;
    }
    if (!esm::save_content_app_settings(app.config_path, settings, error)) {
        MessageBoxW(app.window, error.c_str(), L"保存内容索引设置失败",
                    MB_OK | MB_ICONERROR);
        return;
    }

    app.generation.fetch_add(1, std::memory_order_relaxed);
    clear_results(app);
    const auto active_pipe = app.pipe;
    std::uint32_t active_service_process_id{};
    const auto process_id_error =
        esm::query_content_named_pipe_server_process_id(
            active_pipe, active_service_process_id, 500);
    const auto shutdown =
        esm::request_content_named_pipe_shutdown(active_pipe, 2'000);
    const bool service_was_absent =
        shutdown.error == ERROR_FILE_NOT_FOUND ||
        shutdown.error == ERROR_PIPE_NOT_CONNECTED;
    if (shutdown.error != ERROR_SUCCESS && !service_was_absent) {
        if (process_id_error != ERROR_SUCCESS) {
            error = L"设置已保存，但无法确认旧内容服务进程（错误码 " +
                    std::to_wstring(process_id_error) + L"）。";
            MessageBoxW(app.window, error.c_str(), L"内容服务需要重启",
                        MB_OK | MB_ICONWARNING);
            return;
        }
        const auto legacy_result = stop_legacy_content_service(
            app.window, active_service_process_id, error);
        if (legacy_result == LegacyStopResult::canceled) {
            app.service_text = L"配置已保存，但旧版内容服务仍在使用旧配置";
            update_status_bar(app);
            MessageBoxW(
                app.window,
                L"新配置已保存，但尚未应用。旧版内容服务是独立后台进程，"
                L"只关闭 GUI 不会停止它。请再次点击“应用”并确认停止旧服务。",
                L"索引根尚未应用", MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (legacy_result == LegacyStopResult::failed) {
            MessageBoxW(app.window, error.c_str(), L"无法停止旧版内容服务",
                        MB_OK | MB_ICONERROR);
            return;
        }
    }
    if (!service_was_absent &&
        !wait_for_content_service_exit(active_pipe, 10'000)) {
        MessageBoxW(app.window,
                    L"设置已保存，但内容服务未在 10 秒内退出。"
                    L"请稍后重新打开内容搜索。",
                    L"内容服务停止超时", MB_OK | MB_ICONWARNING);
        return;
    }

    if (!app.pipe_overridden) app.pipe = settings.pipe_name;
    app.service_start_attempted = false;
    app.service_state = ServiceState::indexing;
    app.service_text = L"配置已保存，正在按新范围启动内容索引……";
    app.activity_text.clear();
    update_status_bar(app);
    if (!ensure_content_service_running(app, error)) {
        app.service_state = ServiceState::unavailable;
        app.service_text = error;
        update_status_bar(app);
        MessageBoxW(app.window, error.c_str(), L"启动内容服务失败",
                    MB_OK | MB_ICONERROR);
        return;
    }
    queue_status(app);
}

void draw_status_dot(const App& app, const DRAWITEMSTRUCT& draw) {
    COLORREF color = RGB(210, 55, 55);
    if (app.service_state == ServiceState::ready)
        color = RGB(35, 170, 85);
    else if (app.service_state == ServiceState::indexing ||
             app.service_state == ServiceState::searching)
        color = RGB(235, 164, 32);
    FillRect(draw.hDC, &draw.rcItem,
             reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1));
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(100, 100, 100));
    const auto old_brush = SelectObject(draw.hDC, brush);
    const auto old_pen = SelectObject(draw.hDC, pen);
    Ellipse(draw.hDC, 2, 2, 14, 14);
    SelectObject(draw.hDC, old_brush);
    SelectObject(draw.hDC, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
    App* app = app_from(window);
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        app = static_cast<App*>(create->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(app));
        return TRUE;
    }
    case WM_CREATE: {
        HMENU menu_bar = CreateMenu();
        HMENU index_menu = CreatePopupMenu();
        app->view_menu = CreatePopupMenu();
        if (menu_bar && index_menu && app->view_menu) {
            AppendMenuW(index_menu, MF_STRING, command_manage_index_roots,
                        L"管理索引根(&R)…");
            AppendMenuW(menu_bar, MF_POPUP,
                        reinterpret_cast<UINT_PTR>(index_menu), L"索引(&I)");
            AppendMenuW(app->view_menu, MF_STRING | MF_CHECKED,
                        command_toggle_preview,
                        L"预览窗格\tCtrl+Shift+P");
            AppendMenuW(menu_bar, MF_POPUP,
                        reinterpret_cast<UINT_PTR>(app->view_menu),
                        L"查看(&V)");
            SetMenu(window, menu_bar);
        } else {
            if (app->view_menu) {
                DestroyMenu(app->view_menu);
                app->view_menu = nullptr;
            }
            if (index_menu) DestroyMenu(index_menu);
            if (menu_bar) DestroyMenu(menu_bar);
        }
        app->search_label = CreateWindowExW(
            0, L"STATIC", L"搜索：", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
            0, 0, 0, 0, window, nullptr, nullptr, nullptr);
        app->search = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(control_search), nullptr,
            nullptr);
        SendMessageW(app->search, EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(L"输入关键词搜索文件内容（Ctrl+L）"));
        app->clear = CreateWindowExW(0, L"BUTTON", L"×", WS_CHILD | WS_VISIBLE,
                                     0, 0, 0, 0, window,
                                     reinterpret_cast<HMENU>(control_clear),
                                     nullptr, nullptr);
        app->open = CreateWindowExW(0, L"BUTTON", L"打开", WS_CHILD | WS_VISIBLE,
                                    0, 0, 0, 0, window,
                                    reinterpret_cast<HMENU>(control_open), nullptr,
                                    nullptr);
        app->open_folder = CreateWindowExW(
            0, L"BUTTON", L"打开目录", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
            window, reinterpret_cast<HMENU>(control_open_folder), nullptr,
            nullptr);
        app->results = CreateWindowExW(
            WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS |
                LVS_SINGLESEL,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(control_results),
            nullptr, nullptr);
        ListView_SetExtendedListViewStyle(
            app->results, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                              LVS_EX_LABELTIP | LVS_EX_HEADERDRAGDROP);
        SHFILEINFOW shell_info{};
        app->system_images = reinterpret_cast<HIMAGELIST>(SHGetFileInfoW(
            L".txt", FILE_ATTRIBUTE_NORMAL, &shell_info, sizeof(shell_info),
            SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES));
        if (app->system_images)
            ListView_SetImageList(app->results, app->system_images, LVSIL_SMALL);
        add_column(app->results, 0, 220, L"名称");
        add_column(app->results, 1, 300, L"路径");
        add_column(app->results, 2, 480, L"内容匹配");
        add_column(app->results, 3, 82, L"相关度");
        app->status_dot = CreateWindowExW(
            0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 0, 0, 0,
            0, window, reinterpret_cast<HMENU>(control_status_dot), nullptr,
            nullptr);
        app->status_text = CreateWindowExW(
            0, L"STATIC", L"正在连接独立内容服务……",
            WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, 0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(control_status_text), nullptr, nullptr);
        app->result_count = CreateWindowExW(
            0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_CENTERIMAGE,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(control_result_count),
            nullptr, nullptr);
        app->preview_toggle = CreateWindowExW(
            0, L"BUTTON", L"隐藏预览", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
            window, reinterpret_cast<HMENU>(control_preview_toggle), nullptr,
            nullptr);
        app->preview_title = CreateWindowExW(
            0, L"STATIC", L"内容预览",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, 0,
            0, 0, 0, window, reinterpret_cast<HMENU>(control_preview_title),
            nullptr, nullptr);
        app->preview_path = CreateWindowExW(
            0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(control_preview_path), nullptr, nullptr);
        app->rich_edit_library = LoadLibraryW(L"Msftedit.dll");
        const wchar_t* preview_class =
            app->rich_edit_library ? MSFTEDIT_CLASS : L"EDIT";
        app->preview_text = CreateWindowExW(
            WS_EX_CLIENTEDGE, preview_class, L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                ES_AUTOVSCROLL | ES_WANTRETURN,
            0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(control_preview_text), nullptr, nullptr);
        if (app->rich_edit_library && app->preview_text) {
            SendMessageW(app->preview_text, EM_SETBKGNDCOLOR, 0,
                         GetSysColor(COLOR_WINDOW));
        }
        app->font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        for (HWND control : {app->search_label, app->search, app->clear, app->open,
                             app->open_folder, app->results, app->status_text,
                             app->result_count, app->preview_toggle,
                             app->preview_title, app->preview_path,
                             app->preview_text}) {
            SendMessageW(control, WM_SETFONT,
                         reinterpret_cast<WPARAM>(app->font), TRUE);
        }
        set_empty_preview(*app);
        update_action_state(*app);
        start_worker(*app);
        SetTimer(window, status_timer, 2000, nullptr);
        std::wstring service_error;
        if (!ensure_content_service_running(*app, service_error)) {
            app->service_text = std::move(service_error);
            update_status_bar(*app);
        }
        queue_status(*app);
        layout(*app);
        SetFocus(app->search);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize.x = 760;
        info->ptMinTrackSize.y = 420;
        return 0;
    }
    case WM_SIZE:
        if (app) layout(*app);
        return 0;
    case WM_COMMAND:
        if (!app) break;
        if (LOWORD(wparam) == control_search && HIWORD(wparam) == EN_CHANGE) {
            KillTimer(window, search_timer);
            SetTimer(window, search_timer, 160, nullptr);
        } else if (LOWORD(wparam) == control_clear) {
            SetWindowTextW(app->search, L"");
            SetFocus(app->search);
        } else if (LOWORD(wparam) == control_open) {
            open_selected(*app);
        } else if (LOWORD(wparam) == control_open_folder) {
            open_selected_folder(*app);
        } else if (LOWORD(wparam) == control_preview_toggle ||
                   LOWORD(wparam) == command_toggle_preview) {
            set_preview_visible(*app, !app->preview_visible);
        } else if (LOWORD(wparam) == command_manage_index_roots) {
            manage_index_roots(*app);
        }
        return 0;
    case WM_TIMER:
        if (!app) break;
        if (wparam == search_timer) {
            KillTimer(window, search_timer);
            queue_search(*app);
        } else if (wparam == status_timer) {
            queue_status(*app);
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
            if (header->hwndFrom == app->results && header->code == NM_DBLCLK)
                open_selected(*app);
            else if (header->hwndFrom == app->results &&
                     header->code == NM_RCLICK)
                show_result_menu(*app);
            else if (header->hwndFrom == app->results &&
                     header->code == LVN_ITEMCHANGED) {
                update_action_state(*app);
                update_preview(*app);
            }
        }
        return 0;
    case WM_DRAWITEM:
        if (app && wparam == control_status_dot) {
            draw_status_dot(*app, *reinterpret_cast<DRAWITEMSTRUCT*>(lparam));
            return TRUE;
        }
        break;
    case message_search_complete:
        if (app) {
            std::unique_ptr<SearchCompletion> completion(
                reinterpret_cast<SearchCompletion*>(lparam));
            if (completion->generation != app->generation.load()) return 0;
            if (completion->result.error != ERROR_SUCCESS) {
                app->service_state = ServiceState::unavailable;
                set_activity(*app, L"内容查询失败，错误码 " +
                                       std::to_wstring(completion->result.error));
            } else {
                populate_results(*app,
                                 std::move(completion->result.response.result));
                queue_status(*app);
            }
        }
        return 0;
    case message_status_complete:
        if (app) {
            std::unique_ptr<StatusCompletion> completion(
                reinterpret_cast<StatusCompletion*>(lparam));
            if (completion->result.error != ERROR_SUCCESS) {
                app->service_state = ServiceState::unavailable;
                app->service_text = L"独立内容服务不可用；文件名搜索不受影响";
            } else {
                const auto& status = completion->result.response.status;
                app->service_state = status.indexing ? ServiceState::indexing
                                                     : ServiceState::ready;
                app->service_text = status.message + L"，文档数 " +
                                    std::to_wstring(status.documents);
            }
            if (window_text(app->search).empty()) app->activity_text.clear();
            update_status_bar(*app);
        }
        return 0;
    case WM_DESTROY:
        if (app) {
            KillTimer(window, search_timer);
            KillTimer(window, status_timer);
            stop_worker(*app);
            if (app->font) {
                DeleteObject(app->font);
                app->font = nullptr;
            }
        }
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
            if (argument == L"--config" && index + 1 < argc)
                app.config_path = argv[++index];
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
    window_class_info.hIconSm =
        static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                      16, 16, LR_DEFAULTCOLOR));
    window_class_info.hbrBackground =
        reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
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
        if (message.message == WM_KEYDOWN) {
            if ((GetKeyState(VK_CONTROL) & 0x8000) &&
                (GetKeyState(VK_SHIFT) & 0x8000) && message.wParam == 'P') {
                set_preview_visible(app, !app.preview_visible);
                continue;
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && message.wParam == 'L') {
                SetFocus(app.search);
                SendMessageW(app.search, EM_SETSEL, 0, -1);
                continue;
            }
            if (message.wParam == VK_ESCAPE) {
                SetWindowTextW(app.search, L"");
                SetFocus(app.search);
                continue;
            }
            if (message.wParam == VK_RETURN) {
                if (GetFocus() == app.results)
                    open_selected(app);
                else {
                    KillTimer(app.window, search_timer);
                    queue_search(app);
                }
                continue;
            }
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (app.rich_edit_library) FreeLibrary(app.rich_edit_library);
    return static_cast<int>(message.wParam);
}
