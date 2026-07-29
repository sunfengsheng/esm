#include "esm/gui_settings.hpp"
#include "esm/interactive_search_timing.hpp"
#include "esm/result_metadata.hpp"
#include "esm/file_list.hpp"
#include "esm/file_metadata.hpp"
#include "esm/saved_search.hpp"
#include "esm/directory_scanner.hpp"
#include "esm/index.hpp"
#include "dialogs.hpp"
#include "esm/named_pipe.hpp"
#include "resource.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <optional>

namespace {
constexpr UINT WM_ESM_TRAY = WM_APP + 1;
constexpr UINT WM_ESM_RESULTS = WM_APP + 2;
constexpr UINT WM_ESM_QUERY_CHANGED = WM_APP + 3;
constexpr UINT WM_ESM_COLUMN_MENU = WM_APP + 4;
constexpr UINT WM_ESM_SAVE_SETTINGS = WM_APP + 5;
constexpr UINT WM_ESM_APPLY_TOPMOST = WM_APP + 6;
constexpr UINT WM_ESM_METADATA = WM_APP + 7;
constexpr UINT_PTR SEARCH_TIMER = 1;
constexpr UINT SEARCH_TIMER_INTERVAL_MS = 10;
constexpr ULONGLONG RESULT_REFINEMENT_DEBOUNCE_MS = 250;
constexpr ULONGLONG HISTORY_DEBOUNCE_MS = 1000;
constexpr ULONGLONG METADATA_DEBOUNCE_MS = 250;
constexpr std::uint32_t INTERACTIVE_RESULT_LIMIT = 200;
constexpr ULONGLONG SEARCH_RETRY_BASE_MS = 500;
constexpr ULONGLONG SEARCH_RETRY_MAX_MS = 3000;
constexpr int HOTKEY_ID = 1;
constexpr int SEARCH_ID = 100;
constexpr int LIST_ID = 101;
constexpr int PREVIEW_ID = 102;
constexpr int STATUS_ID = 103;
constexpr int FILTER_ID = 104;
enum : UINT {
  CMD_NEW_WINDOW = 40001,
  CMD_OPEN_FILE_LIST,
  CMD_CLOSE_FILE_LIST,
  CMD_CLOSE_WINDOW,
  CMD_EXPORT,
  CMD_OPEN,
  CMD_LOCATION,
  CMD_CUT_FILE,
  CMD_COPY_FILE,
  CMD_COPY_PATH,
  CMD_COPY_NAME,
  CMD_COPY_PARENT,
  CMD_PASTE,
  CMD_COPY_TO_FOLDER,
  CMD_MOVE_TO_FOLDER,
  CMD_ADVANCED_COPY,
  CMD_ADVANCED_MOVE,
  CMD_SELECT_ALL,
  CMD_INVERT_SELECTION,
  CMD_CLEAR_SELECTION,
  CMD_RENAME,
  CMD_DELETE,
  CMD_PROPERTIES,
  CMD_PREVIEW,
  CMD_STATUS_BAR,
  CMD_FILTER_BAR,
  CMD_VIEW_EXTRA_LARGE,
  CMD_VIEW_LARGE,
  CMD_VIEW_MEDIUM,
  CMD_VIEW_DETAILS,
  CMD_WINDOW_SMALL,
  CMD_WINDOW_MEDIUM,
  CMD_WINDOW_LARGE,
  CMD_WINDOW_AUTO,
  CMD_FONT_INCREASE,
  CMD_FONT_DECREASE,
  CMD_FONT_NORMAL,
  CMD_SORT_RELEVANCE,
  CMD_SORT_NAME,
  CMD_SORT_PATH,
  CMD_SORT_SIZE,
  CMD_SORT_EXTENSION,
  CMD_SORT_TYPE,
  CMD_SORT_MODIFIED,
  CMD_SORT_CREATED,
  CMD_SORT_ACCESSED,
  CMD_SORT_ATTRIBUTES,
  CMD_SORT_CHANGED,
  CMD_SORT_RUN_COUNT,
  CMD_SORT_LAST_OPEN,
  CMD_SORT_FILE_LIST_NAME,
  CMD_SORT_ASCENDING,
  CMD_SORT_DESCENDING,
  CMD_HOME,
  CMD_REFRESH,
  CMD_TOPMOST_NEVER,
  CMD_TOPMOST_ALWAYS,
  CMD_TOPMOST_SEARCHING,
  CMD_FOCUS_SEARCH,
  CMD_CLEAR_SEARCH,
  CMD_MATCH_CASE,
  CMD_MATCH_WHOLE_WORD,
  CMD_MATCH_PATH,
  CMD_MATCH_DIACRITICS,
  CMD_REGEX,
  CMD_ADVANCED_SEARCH,
  CMD_ADD_FILTER,
  CMD_MANAGE_FILTERS,
  CMD_BOOKMARK_ADD,
  CMD_BOOKMARK_REMOVE_CURRENT,
  CMD_BOOKMARK_MANAGE,
  CMD_CONNECT_SERVICE,
  CMD_DISCONNECT_SERVICE,
  CMD_FILE_LIST_EDITOR,
  CMD_SERVICE_RECONNECT,
  CMD_OPEN_DATA_DIR,
  CMD_OPEN_LOG_DIR,
  CMD_OPTIONS,
  CMD_HELP_OVERVIEW,
  CMD_HELP_SYNTAX,
  CMD_HELP_REGEX,
  CMD_HELP_SHORTCUTS,
  CMD_HELP_README,
  CMD_HELP_COMMAND_LINE,
  CMD_HELP_WEBSITE,
  CMD_HELP_CHECK_UPDATES,
  CMD_ABOUT,
  CMD_SHOW,
  CMD_EXIT,
  CMD_COLUMN_FIRST = 40400,
  CMD_COLUMN_LAST = CMD_COLUMN_FIRST +
                    static_cast<UINT>(esm::gui_column_count) - 1,
  CMD_FILTER_FIRST = 40500,
  CMD_FILTER_LAST = CMD_FILTER_FIRST + 107,
  CMD_BOOKMARK_FIRST = 40600,
  CMD_BOOKMARK_LAST = CMD_BOOKMARK_FIRST + 99
};

using Bookmark = esm::SavedSearch;

struct RunHistoryEntry {
  std::uint64_t count{};
  std::int64_t last_open_time{};
};

struct SearchPayload {
  std::uint64_t generation{};
  std::wstring query;
  esm::PipeSearchResult result;
  std::uint32_t requested_limit{};
  bool final_results{true};
  bool needs_metadata_hydration{};
};
struct MetadataPayload {
  std::uint64_t generation{};
  std::vector<esm::ResultMetadataUpdate> updates;
};
struct App {
  HINSTANCE instance{};
  HWND window{}, search{}, search_edit{}, filter{}, list{}, preview{}, status{};
  HMENU main_menu{}, file_menu{}, edit_menu{}, view_menu{}, search_menu{},
      bookmarks_menu{}, tools_menu{}, help_menu{}, column_menu{}, sort_menu{},
      goto_menu{}, advanced_edit_menu{}, window_size_menu{}, font_size_menu{},
      topmost_menu{};
  HFONT ui_font{};
  HACCEL accelerators{};
  std::wstring pipe{L"everything_sm_service"};
  std::wstring last_query;
  std::wstring scheduled_query_text;
  std::vector<esm::SearchResult> results;
  std::vector<std::wstring> history;
  std::filesystem::path history_path;
  std::filesystem::path settings_path;
  std::filesystem::path service_pipe_path;
  std::filesystem::path bookmarks_path;
  std::filesystem::path filters_path;
  std::filesystem::path run_history_path;
  std::vector<Bookmark> bookmarks;
  std::vector<esm::SavedSearch> custom_filters;
  std::unordered_map<std::wstring, RunHistoryEntry> run_history;
  std::unique_ptr<esm::MetadataIndex> file_list_index;
  std::filesystem::path file_list_path;
  std::wstring local_pipe{L"everything_sm_service"};
  bool service_connected{true};
  esm::GuiSettings settings{esm::default_gui_settings()};
  std::vector<esm::GuiColumnId> visible_columns;
  std::atomic<std::uint64_t> generation{0};
  esm::SortField sort{esm::SortField::relevance};
  bool descending{};
  bool show_preview{};
  bool query_pending{};
  bool query_pending_final_results{true};
  ULONGLONG query_due_tick{};
  ULONGLONG last_query_input_tick{};
  bool refinement_pending{};
  ULONGLONG refinement_due_tick{};
  bool history_pending{};
  ULONGLONG history_due_tick{};
  std::wstring pending_history_query;
  bool service_query_in_flight{};
  HANDLE service_query_thread{};
  bool metadata_pending{};
  ULONGLONG metadata_due_tick{};
  bool metadata_in_flight{};
  bool metadata_only_missing_basic{};
  HANDLE metadata_thread{};
  std::shared_ptr<std::atomic_bool> metadata_cancel;
  unsigned service_retry_count{};
  bool closing{};
  HIMAGELIST small_images{}, large_images{}, extra_large_images{};
  std::unordered_map<std::wstring, int> icon_cache;
  NOTIFYICONDATAW tray{};
};
App *app(HWND window) {
  return reinterpret_cast<App *>(GetWindowLongPtrW(window, GWLP_USERDATA));
}
HICON load_app_icon(HINSTANCE instance, bool small) {
  const int width = GetSystemMetrics(small ? SM_CXSMICON : SM_CXICON);
  const int height = GetSystemMetrics(small ? SM_CYSMICON : SM_CYICON);
  auto icon = reinterpret_cast<HICON>(LoadImageW(
      instance, MAKEINTRESOURCEW(IDI_ESM_APP), IMAGE_ICON, width, height,
      LR_DEFAULTCOLOR | LR_SHARED));
  return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

struct ColumnDefinition {
  esm::GuiColumnId id;
  const wchar_t *title;
  int format;
  esm::SortField sort;
};
constexpr std::array<ColumnDefinition, esm::gui_column_count> column_definitions{{
    {esm::GuiColumnId::name, L"名称", LVCFMT_LEFT, esm::SortField::name},
    {esm::GuiColumnId::path, L"路径", LVCFMT_LEFT, esm::SortField::path},
    {esm::GuiColumnId::size, L"大小", LVCFMT_RIGHT, esm::SortField::size},
    {esm::GuiColumnId::last_write_time, L"修改时间", LVCFMT_LEFT,
     esm::SortField::last_write_time},
    {esm::GuiColumnId::type, L"类型", LVCFMT_LEFT,
     esm::SortField::extension},
}};

const ColumnDefinition &column_definition(esm::GuiColumnId id) {
  return column_definitions[static_cast<std::size_t>(id)];
}
esm::GuiColumnSettings &column_settings(App &a, esm::GuiColumnId id) {
  for (auto &column : a.settings.columns)
    if (column.id == id)
      return column;
  return a.settings.columns.front();
}
const esm::GuiColumnSettings &column_settings(const App &a,
                                               esm::GuiColumnId id) {
  for (const auto &column : a.settings.columns)
    if (column.id == id)
      return column;
  return a.settings.columns.front();
}

std::wstring text(HWND window) {
  const int n = GetWindowTextLengthW(window);
  if (n <= 0)
    return {};
  std::wstring value((std::size_t)n + 1, L'\0');
  const int copied = GetWindowTextW(window, value.data(), n + 1);
  value.resize(copied > 0 ? (std::size_t)copied : 0);
  return value;
}
std::wstring search_text(const App &a) { return text(a.search); }
HWND search_edit_control(const App &a) {
  return a.search_edit ? a.search_edit : a.search;
}
bool search_has_focus(const App &a) {
  const HWND focus = GetFocus();
  return focus == a.search || focus == a.search_edit;
}
bool edit_has_selection(HWND edit) {
  if (!edit)
    return false;
  DWORD begin = 0;
  DWORD end = 0;
  SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&begin),
               reinterpret_cast<LPARAM>(&end));
  return begin != end;
}
void focus_search(App &a) {
  HWND edit = search_edit_control(a);
  SetFocus(edit);
  SendMessageW(edit, EM_SETSEL, 0, -1);
}
void select_all_results(App &a) {
  if (a.results.empty())
    return;
  ListView_SetItemState(a.list, -1, LVIS_SELECTED, LVIS_SELECTED);
  ListView_SetItemState(a.list, 0, LVIS_FOCUSED, LVIS_FOCUSED);
  ListView_EnsureVisible(a.list, 0, FALSE);
}
void focus_first_result(App &a) {
  if (a.results.empty())
    return;
  if (ListView_GetNextItem(a.list, -1, LVNI_FOCUSED) < 0) {
    ListView_SetItemState(a.list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(a.list, 0, FALSE);
  }
  SetFocus(a.list);
}
std::wstring win_error(DWORD code) {
  wchar_t *raw = nullptr;
  DWORD n = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, 0, reinterpret_cast<wchar_t *>(&raw), 0, nullptr);
  std::wstring value = n && raw ? std::wstring(raw, n)
                                : L"Windows error " + std::to_wstring(code);
  if (raw)
    LocalFree(raw);
  while (!value.empty() && (value.back() == L'\r' || value.back() == L'\n'))
    value.pop_back();
  return value;
}
std::filesystem::path local_data() {
  PWSTR raw = nullptr;
  std::filesystem::path result;
  if (SUCCEEDED(
          SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw))) {
    result = std::filesystem::path(raw) / L"everything_sm";
    CoTaskMemFree(raw);
  }
  return result;
}
std::wstring from_utf8(std::string_view value) {
  if (value.empty())
    return {};
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        value.data(), (int)value.size(), nullptr, 0);
  if (count <= 0)
    return {};
  std::wstring result((std::size_t)count, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                      (int)value.size(), result.data(), count);
  return result;
}
std::string to_utf8(std::wstring_view value) {
  if (value.empty())
    return {};
  const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                        (int)value.size(), nullptr, 0, nullptr, nullptr);
  if (count <= 0)
    return {};
  std::string result((std::size_t)count, '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(),
                      result.data(), count, nullptr, nullptr);
  return result;
}
std::filesystem::path executable_path() {
  std::wstring buffer(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                          static_cast<DWORD>(buffer.size()));
  buffer.resize(length);
  return std::filesystem::path(buffer);
}
void load_saved_search_file(const std::filesystem::path& path,
                            std::vector<esm::SavedSearch>& values,
                            bool legacy_queries) {
  values.clear();
  std::ifstream in(path, std::ios::binary);
  if (!in) return;
  std::string bytes((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
  if (esm::parse_saved_searches(bytes, values)) return;
  if (!legacy_queries) return;
  std::istringstream lines(bytes);
  std::string line;
  while (std::getline(lines, line) && values.size() < 100) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    auto query = from_utf8(line);
    if (!query.empty()) values.push_back({query, query});
  }
}
void save_saved_search_file(const std::filesystem::path& path,
                            const std::vector<esm::SavedSearch>& values) {
  if (path.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  const auto bytes = esm::serialize_saved_searches(values);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
void load_bookmarks(App &a) {
  const auto dir = local_data();
  if (dir.empty()) return;
  a.bookmarks_path = dir / L"bookmarks.txt";
  a.filters_path = dir / L"filters.txt";
  load_saved_search_file(a.bookmarks_path, a.bookmarks, true);
  load_saved_search_file(a.filters_path, a.custom_filters, false);
}
void save_bookmarks(const App &a) {
  save_saved_search_file(a.bookmarks_path, a.bookmarks);
}
void save_filters(const App &a) {
  save_saved_search_file(a.filters_path, a.custom_filters);
}

std::wstring run_history_key(std::wstring value) {
  if (!value.empty()) CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
  return value;
}

void load_run_history(App &a) {
  const auto dir = local_data();
  if (dir.empty()) return;
  a.run_history_path = dir / L"run-history.txt";
  std::ifstream in(a.run_history_path, std::ios::binary);
  std::uint64_t count = 0;
  std::int64_t last_open = 0;
  std::string path;
  while (in >> count >> last_open >> std::quoted(path)) {
    auto wide = from_utf8(path);
    if (!wide.empty())
      a.run_history[run_history_key(std::move(wide))] = {count, last_open};
  }
}

void save_run_history(const App &a) {
  if (a.run_history_path.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(a.run_history_path.parent_path(), ec);
  std::ofstream out(a.run_history_path, std::ios::binary | std::ios::trunc);
  for (const auto &[path, entry] : a.run_history) {
    out << entry.count << ' ' << entry.last_open_time << ' '
        << std::quoted(to_utf8(path)) << '\n';
  }
}

std::int64_t current_file_time() {
  FILETIME value{};
  GetSystemTimeAsFileTime(&value);
  return static_cast<std::int64_t>(
      (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U) |
      static_cast<std::uint64_t>(value.dwLowDateTime));
}

void record_open(App &a, const esm::FileRecord &record) {
  auto &entry = a.run_history[run_history_key(record.path)];
  ++entry.count;
  entry.last_open_time = current_file_time();
  save_run_history(a);
}

void load_settings(App &a) {
  const auto dir = local_data();
  if (dir.empty())
    return;
  a.settings_path = dir / L"gui-settings.conf";
  a.service_pipe_path = dir / L"service-pipe.txt";
  if (a.pipe == L"everything_sm_service") {
    std::ifstream pipe_input(a.service_pipe_path, std::ios::binary);
    if (pipe_input) {
      std::string bytes((std::istreambuf_iterator<char>(pipe_input)),
                        std::istreambuf_iterator<char>());
      while (!bytes.empty() && (bytes.back() == '\r' || bytes.back() == '\n'))
        bytes.pop_back();
      const auto configured = from_utf8(bytes);
      if (!configured.empty()) a.pipe = configured;
    }
  }
  a.local_pipe = a.pipe;
  std::ifstream in(a.settings_path, std::ios::binary);
  if (in) {
    std::string bytes((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    esm::GuiSettings parsed;
    if (esm::parse_gui_settings(bytes, parsed))
      a.settings = parsed;
  }
  a.sort = static_cast<esm::SortField>(a.settings.sort_field);
  a.descending = a.settings.descending;
  a.show_preview = a.settings.show_preview;
}

void write_settings(const App &a) {
  if (a.settings_path.empty())
    return;
  std::error_code ec;
  std::filesystem::create_directories(a.settings_path.parent_path(), ec);
  const auto temporary = a.settings_path.wstring() + L".tmp";
  {
    std::ofstream out(std::filesystem::path(temporary),
                      std::ios::binary | std::ios::trunc);
    if (!out)
      return;
    const auto bytes = esm::serialize_gui_settings(a.settings);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    if (!out)
      return;
  }
  if (!MoveFileExW(temporary.c_str(), a.settings_path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temporary.c_str());
  }
}

void load_history(App &a) {
  auto dir = local_data();
  if (dir.empty())
    return;
  a.history_path = dir / L"gui-history.txt";
  std::wifstream in(a.history_path);
  std::wstring line;
  while (std::getline(in, line) && a.history.size() < 100)
    if (!line.empty())
      a.history.push_back(line);
}
void save_history(const App &a) {
  if (a.history_path.empty())
    return;
  std::error_code ec;
  std::filesystem::create_directories(a.history_path.parent_path(), ec);
  std::wofstream out(a.history_path, std::ios::trunc);
  for (auto &q : a.history)
    out << q << L'\n';
}
void remember(App &a, const std::wstring &q) {
  if (q.empty())
    return;
  a.history.erase(std::remove(a.history.begin(), a.history.end(), q),
                  a.history.end());
  a.history.insert(a.history.begin(), q);
  if (a.history.size() > 100)
    a.history.resize(100);
  if (a.search) {
    const auto existing = SendMessageW(a.search, CB_FINDSTRINGEXACT, -1,
                                       reinterpret_cast<LPARAM>(q.c_str()));
    if (existing != CB_ERR)
      SendMessageW(a.search, CB_DELETESTRING, existing, 0);
    SendMessageW(a.search, CB_INSERTSTRING, 0,
                 reinterpret_cast<LPARAM>(q.c_str()));
    while (SendMessageW(a.search, CB_GETCOUNT, 0, 0) > 100)
      SendMessageW(a.search, CB_DELETESTRING, 100, 0);
  }
  save_history(a);
}
std::wstring size_text(const esm::FileRecord &r) {
  if (r.directory || (r.size == 0 && r.last_write_time == 0))
    return {};
  const wchar_t *units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
  double value = (double)r.size;
  int unit = 0;
  while (value >= 1024 && unit < 4) {
    value /= 1024;
    ++unit;
  }
  std::wostringstream out;
  if (unit)
    out << std::fixed << std::setprecision(value < 10 ? 1 : 0) << value;
  else
    out << r.size;
  out << L' ' << units[unit];
  return out.str();
}
std::wstring time_text(std::int64_t raw) {
  if (!raw)
    return {};
  auto v = (std::uint64_t)raw;
  FILETIME ft{(DWORD)v, (DWORD)(v >> 32)};
  SYSTEMTIME utc{}, local{};
  if (!FileTimeToSystemTime(&ft, &utc) ||
      !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
    return {};
  wchar_t b[64]{};
  swprintf(b, std::size(b), L"%04u-%02u-%02u %02u:%02u", local.wYear,
           local.wMonth, local.wDay, local.wHour, local.wMinute);
  return b;
}
std::wstring type_text(const esm::FileRecord &r) {
  if (r.directory)
    return L"\u6587\u4ef6\u5939";
  auto ext = std::filesystem::path(r.name).extension().wstring();
  return ext.empty() ? L"文件" : ext.substr(1) + L" 文件";
}
int icon(App &a, const esm::FileRecord &r) {
  std::wstring key;
  if (r.directory) {
    key = L"<folder>";
  } else {
    const auto separator = r.name.find_last_of(L".");
    key = separator == std::wstring::npos ? L"<file>" : r.name.substr(separator);
    if (!key.empty())
      CharLowerBuffW(key.data(), static_cast<DWORD>(key.size()));
  }
  if (const auto found = a.icon_cache.find(key); found != a.icon_cache.end())
    return found->second;

  SHFILEINFOW info{};
  const DWORD attributes =
      r.directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
  const wchar_t *lookup = r.directory ? L"folder" : key.c_str();
  const auto value = SHGetFileInfoW(
      lookup, attributes, &info, sizeof(info),
      SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
  const int index = value ? info.iIcon : 0;
  a.icon_cache.emplace(std::move(key), index);
  return index;
}
std::vector<std::size_t> selected(const App &a) {
  std::vector<std::size_t> out;
  if (!a.list)
    return out;
  int i = -1;
  while ((i = ListView_GetNextItem(a.list, i, LVNI_SELECTED)) != -1)
    if ((std::size_t)i < a.results.size())
      out.push_back((std::size_t)i);
  return out;
}
const esm::FileRecord *focused(const App &a) {
  if (!a.list)
    return nullptr;
  int i = ListView_GetNextItem(a.list, -1, LVNI_FOCUSED);
  return i >= 0 && (std::size_t)i < a.results.size()
             ? &a.results[(std::size_t)i].record
             : nullptr;
}
void status(App &a, const std::wstring &value) {
  SendMessageW(a.status, SB_SETTEXTW, 0, (LPARAM)value.c_str());
}
bool open(App &a, const esm::FileRecord &r) {
  const auto result = ShellExecuteW(a.window, L"open", r.path.c_str(), nullptr,
                                    nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(result) > 32) {
    record_open(a, r);
    return true;
  }
  status(a, L"\u65e0\u6cd5\u6253\u5f00\uff1a" + r.path);
  return false;
}
void location(App &a, const esm::FileRecord &r) {
  if (r.directory) {
    open(a, r);
    return;
  }
  auto args = L"/select,\"" + r.path + L"\"";
  ShellExecuteW(a.window, L"open", L"explorer.exe", args.c_str(), nullptr,
                SW_SHOWNORMAL);
}
void clipboard_text(const std::wstring &value) {
  if (!OpenClipboard(nullptr))
    return;
  EmptyClipboard();
  auto bytes = (value.size() + 1) * sizeof(wchar_t);
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (h) {
    auto p = GlobalLock(h);
    memcpy(p, value.c_str(), bytes);
    GlobalUnlock(h);
    if (!SetClipboardData(CF_UNICODETEXT, h))
      GlobalFree(h);
  }
  CloseClipboard();
}
std::vector<std::wstring> selected_paths(const App &a) {
  std::vector<std::wstring> paths;
  for (const auto index : selected(a))
    paths.push_back(a.results[index].record.path);
  if (paths.empty())
    if (const auto *record = focused(a))
      paths.push_back(record->path);
  return paths;
}

void launch_new_window(const App &a) {
  const auto executable = executable_path();
  ShellExecuteW(a.window, L"open", executable.c_str(), a.pipe.c_str(),
                executable.parent_path().c_str(), SW_SHOWNORMAL);
}
void copy_selected_names(const App &a) {
  std::wstring value;
  auto indices = selected(a);
  if (indices.empty()) {
    const int focused_index = ListView_GetNextItem(a.list, -1, LVNI_FOCUSED);
    if (focused_index >= 0 && static_cast<std::size_t>(focused_index) < a.results.size())
      indices.push_back(static_cast<std::size_t>(focused_index));
  }
  for (const auto index : indices) {
    if (!value.empty())
      value += L"\r\n";
    value += a.results[index].record.name;
  }
  clipboard_text(value);
}
void copy_selected_parents(const App &a) {
  std::wstring value;
  for (const auto &path : selected_paths(a)) {
    if (!value.empty())
      value += L"\r\n";
    const std::filesystem::path item(path);
    value += item.has_parent_path() ? item.parent_path().wstring() : item.wstring();
  }
  clipboard_text(value);
}
void invert_selection(App &a) {
  for (int i = 0; i < static_cast<int>(a.results.size()); ++i) {
    const UINT state = ListView_GetItemState(a.list, i, LVIS_SELECTED);
    ListView_SetItemState(a.list, i, (state & LVIS_SELECTED) ? 0 : LVIS_SELECTED,
                          LVIS_SELECTED);
  }
}
void clear_selection(App &a) {
  ListView_SetItemState(a.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
}
void show_properties(const App &a) {
  const auto paths = selected_paths(a);
  if (paths.empty())
    return;
  SHELLEXECUTEINFOW info{sizeof(info)};
  info.fMask = SEE_MASK_INVOKEIDLIST;
  info.hwnd = a.window;
  info.lpVerb = L"properties";
  info.lpFile = paths.front().c_str();
  info.nShow = SW_SHOWNORMAL;
  ShellExecuteExW(&info);
}
std::wstring csv_field(std::wstring value) {
  std::size_t position = 0;
  while ((position = value.find(L'"', position)) != std::wstring::npos) {
    value.insert(position, 1, L'"');
    position += 2;
  }
  return L"\"" + value + L"\"";
}
void export_results(App &a) {
  wchar_t file_name[MAX_PATH] = L"everything_sm-results.efu";
  const wchar_t filter[] =
      L"Everything 文件列表 (*.efu)\0*.efu\0CSV 文件 (*.csv)\0*.csv\0"
      L"\u8def\u5f84\u5217\u8868 (*.txt)\0*.txt\0\u6240\u6709\u6587\u4ef6 (*.*)\0*.*\0\0";
  OPENFILENAMEW dialog{sizeof(dialog)};
  dialog.hwndOwner = a.window;
  dialog.lpstrFilter = filter;
  dialog.lpstrFile = file_name;
  dialog.nMaxFile = static_cast<DWORD>(std::size(file_name));
  dialog.lpstrDefExt = L"efu";
  dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
  if (!GetSaveFileNameW(&dialog))
    return;
  const std::filesystem::path destination(file_name);
  auto extension = destination.extension().wstring();
  if (!extension.empty())
    CharLowerBuffW(extension.data(), static_cast<DWORD>(extension.size()));
  if (extension == L".efu") {
    std::vector<esm::FileRecord> records;
    records.reserve(a.results.size());
    for (const auto &result : a.results)
      records.push_back(result.record);
    std::wstring error;
    if (!esm::save_efu_file(destination, records, error)) {
      MessageBoxW(a.window, error.c_str(), L"导出", MB_OK | MB_ICONERROR);
      return;
    }
    status(a, L"\u5df2\u5bfc\u51fa" + std::to_wstring(records.size()) + L" \u4e2a\u7ed3\u679c");
    return;
  }

  std::ofstream out(destination, std::ios::binary | std::ios::trunc);
  if (!out) {
    MessageBoxW(a.window, L"\u65e0\u6cd5\u521b\u5efa\u5bfc\u51fa\u6587\u4ef6\u3002", L"\u5bfc\u51fa",
                MB_OK | MB_ICONERROR);
    return;
  }
  const unsigned char bom[] = {0xef, 0xbb, 0xbf};
  out.write(reinterpret_cast<const char *>(bom), sizeof(bom));
  if (extension == L".txt") {
    for (const auto &result : a.results) {
      const auto bytes = to_utf8(result.record.path + L"\r\n");
      out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    status(a, L"\u5df2\u5bfc\u51fa" + std::to_wstring(a.results.size()) + L" \u4e2a\u7ed3\u679c");
    return;
  }

  auto write_row = [&](const std::vector<std::wstring> &fields) {
    std::wstring line;
    for (std::size_t i = 0; i < fields.size(); ++i) {
      if (i)
        line.push_back(L',');
      line += csv_field(fields[i]);
    }
    line += L"\r\n";
    const auto bytes = to_utf8(line);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  };
  write_row({L"名称", L"路径", L"大小", L"修改时间", L"创建时间",
             L"\u8bbf\u95ee\u65f6\u95f4", L"Change Time", L"\u5c5e\u6027", L"\u7c7b\u578b"});
  for (const auto &result : a.results) {
    const auto &record = result.record;
    write_row({record.name, record.path, size_text(record),
               time_text(record.last_write_time), time_text(record.creation_time),
               time_text(record.last_access_time), time_text(record.change_time),
               std::to_wstring(record.attributes), type_text(record)});
  }
  status(a, L"\u5df2\u5bfc\u51fa" + std::to_wstring(a.results.size()) + L" \u4e2a\u7ed3\u679c");
}

IDataObject *data_object(const std::vector<std::wstring> &paths) {
  if (paths.empty())
    return nullptr;
  std::vector<PIDLIST_ABSOLUTE> owned;
  std::vector<PCIDLIST_ABSOLUTE> items;
  owned.reserve(paths.size());
  items.reserve(paths.size());
  for (const auto &path : paths) {
    auto pidl = ILCreateFromPathW(path.c_str());
    if (!pidl) {
      for (auto value : owned)
        ILFree(value);
      return nullptr;
    }
    owned.push_back(pidl);
    items.push_back(pidl);
  }

  IShellItemArray *array = nullptr;
  IDataObject *object = nullptr;
  if (SUCCEEDED(SHCreateShellItemArrayFromIDLists(
          static_cast<UINT>(items.size()), items.data(), &array))) {
    array->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&object));
    array->Release();
  }
  for (auto value : owned)
    ILFree(value);
  return object;
}
class DropSource final : public IDropSource {
  std::atomic<ULONG> refs_{1};

public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
    if (!out)
      return E_POINTER;
    if (id == IID_IUnknown || id == IID_IDropSource) {
      *out = this;
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    auto n = --refs_;
    if (!n)
      delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL esc, DWORD keys) override {
    if (esc)
      return DRAGDROP_S_CANCEL;
    if (!(keys & MK_LBUTTON))
      return DRAGDROP_S_DROP;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override {
    return DRAGDROP_S_USEDEFAULTCURSORS;
  }
};
void copy_files(const App &a) {
  if (auto *object = data_object(selected_paths(a))) {
    OleSetClipboard(object);
    object->Release();
  }
}
void cut_files(const App &a) {
  if (auto *object = data_object(selected_paths(a))) {
    const CLIPFORMAT format = static_cast<CLIPFORMAT>(
        RegisterClipboardFormatW(L"Preferred DropEffect"));
    FORMATETC requested{format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    medium.tymed = TYMED_HGLOBAL;
    medium.hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (medium.hGlobal) {
      if (auto *effect = static_cast<DWORD *>(GlobalLock(medium.hGlobal))) {
        *effect = DROPEFFECT_MOVE;
        GlobalUnlock(medium.hGlobal);
        if (FAILED(object->SetData(&requested, &medium, TRUE)))
          GlobalFree(medium.hGlobal);
      } else {
        GlobalFree(medium.hGlobal);
      }
    }
    OleSetClipboard(object);
    object->Release();
  }
}
struct ClipboardFiles {
  std::vector<std::wstring> paths;
  bool move{};
};

bool clipboard_has_files() {
  return IsClipboardFormatAvailable(CF_HDROP) != FALSE;
}

ClipboardFiles clipboard_files(HWND owner) {
  ClipboardFiles result;
  if (!OpenClipboard(owner))
    return result;
  if (const auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP))) {
    const UINT count = DragQueryFileW(drop, 0xffffffffU, nullptr, 0);
    result.paths.reserve(count);
    for (UINT index = 0; index < count; ++index) {
      const UINT length = DragQueryFileW(drop, index, nullptr, 0);
      std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
      if (DragQueryFileW(drop, index, path.data(), length + 1)) {
        path.resize(length);
        result.paths.push_back(std::move(path));
      }
    }
  }
  const auto preferred = RegisterClipboardFormatW(L"Preferred DropEffect");
  if (preferred) {
    if (const auto memory = GetClipboardData(preferred)) {
      if (const auto effect = static_cast<const DWORD *>(GlobalLock(memory))) {
        result.move = (*effect & DROPEFFECT_MOVE) != 0;
        GlobalUnlock(memory);
      }
    }
  }
  CloseClipboard();
  return result;
}

void preview(App &a);

void drag_files(const App &a) {
  if (auto *object = data_object(selected_paths(a))) {
    auto *source = new DropSource;
    DWORD effect{};
    DoDragDrop(object, source, DROPEFFECT_COPY | DROPEFFECT_MOVE, &effect);
    source->Release();
    object->Release();
  }
}
void delete_files(App &a) {
  auto ids = selected(a);
  if (ids.empty())
    return;
  std::wstring paths;
  for (auto i : ids) {
    paths += a.results[i].record.path;
    paths.push_back(L'\0');
  }
  paths.push_back(L'\0');
  SHFILEOPSTRUCTW op{};
  op.hwnd = a.window;
  op.wFunc = FO_DELETE;
  op.pFrom = paths.c_str();
  op.fFlags = FOF_ALLOWUNDO;
  const int result = SHFileOperationW(&op);
  if (result != 0 || op.fAnyOperationsAborted)
    return;

  std::sort(ids.rbegin(), ids.rend());
  for (const auto index : ids)
    if (index < a.results.size())
      a.results.erase(a.results.begin() + static_cast<std::ptrdiff_t>(index));
  ListView_SetItemCountEx(a.list, static_cast<int>(a.results.size()),
                          LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
  InvalidateRect(a.list, nullptr, TRUE);
  preview(a);
  std::wostringstream message;
  message << a.results.size() << L" \u4e2a\u7ed3\u679c";
  status(a, message.str());
}
bool text_file(const std::filesystem::path &p) {
  auto e = p.extension().wstring();
  std::transform(e.begin(), e.end(), e.begin(), towlower);
  const wchar_t *values[] = {L".txt", L".md",   L".log",  L".cpp", L".c",
                             L".h",   L".hpp",  L".json", L".xml", L".yaml",
                             L".yml", L".ini",  L".py",   L".js",  L".ts",
                             L".css", L".html", L".csv"};
  return std::find(std::begin(values), std::end(values), e) != std::end(values);
}
void preview(App &a) {
  auto r = focused(a);
  if (!r) {
    SetWindowTextW(a.preview, L"");
    return;
  }
  std::wostringstream out;
  out << r->path << L"\r\n\r\n类型: " << type_text(*r) << L"\r\n大小: "
      << size_text(*r) << L"\r\n修改时间: " << time_text(r->last_write_time)
      << L"\r\n\u5c5e\u6027: 0x" << std::hex << r->attributes;
  if (!r->directory && text_file(r->path)) {
    std::ifstream in(std::filesystem::path(r->path), std::ios::binary);
    if (in) {
      std::string bytes(65536, '\0');
      in.read(bytes.data(), bytes.size());
      bytes.resize((std::size_t)in.gcount());
      int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                  (int)bytes.size(), nullptr, 0);
      if (n > 0) {
        std::wstring wide((std::size_t)n, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                            (int)bytes.size(), wide.data(), n);
        out << L"\r\n\r\n" << wide;
      }
    }
  }
  SetWindowTextW(a.preview, out.str().c_str());
}
void layout(App &a) {
  RECT r{};
  GetClientRect(a.window, &r);
  const int width = r.right - r.left;
  const int height = r.bottom - r.top;
  const int search_height = 27 + std::max(0, a.settings.font_delta * 2);
  const int status_height = a.settings.show_status
                                ? 22 + std::max(0, a.settings.font_delta)
                                : 0;
  const int content_height = std::max(0, height - search_height - status_height);
  const int filter_width = a.settings.show_filter ? 170 : 0;
  const int preview_width =
      a.show_preview ? std::clamp(width / 3, 260, 520) : 0;
  const int divider = a.show_preview ? 1 : 0;
  const int list_width = std::max(0, width - preview_width - divider);

  MoveWindow(a.search, 2, 2,
             std::max(0, width - 4 - (filter_width ? filter_width + 4 : 0)),
             360, TRUE);
  ShowWindow(a.filter, a.settings.show_filter ? SW_SHOW : SW_HIDE);
  if (a.settings.show_filter)
    MoveWindow(a.filter, std::max(2, width - filter_width - 2), 2,
               filter_width, 360, TRUE);
  MoveWindow(a.list, 0, search_height, list_width, content_height, TRUE);
  ShowWindow(a.preview, a.show_preview ? SW_SHOW : SW_HIDE);
  if (a.show_preview)
    MoveWindow(a.preview, list_width + divider, search_height, preview_width,
               content_height, TRUE);
  ShowWindow(a.status, a.settings.show_status ? SW_SHOW : SW_HIDE);
  if (a.settings.show_status)
    MoveWindow(a.status, 0, height - status_height, width, status_height, TRUE);
}
void save_settings(App &a, bool capture_columns = true,
                   bool capture_window = false);

void apply_view_mode(App& a) {
  if (!a.list) return;
  switch (a.settings.view_mode) {
  case 3:
    ListView_SetView(a.list, LV_VIEW_ICON);
    if (a.large_images) ListView_SetImageList(a.list, a.large_images, LVSIL_NORMAL);
    ListView_SetIconSpacing(a.list, 180, 120);
    break;
  case 2:
    ListView_SetView(a.list, LV_VIEW_ICON);
    if (a.large_images) ListView_SetImageList(a.list, a.large_images, LVSIL_NORMAL);
    ListView_SetIconSpacing(a.list, 140, 92);
    break;
  case 1:
    ListView_SetView(a.list, LV_VIEW_SMALLICON);
    if (a.small_images) ListView_SetImageList(a.list, a.small_images, LVSIL_SMALL);
    ListView_SetIconSpacing(a.list, 120, 28);
    break;
  default:
    ListView_SetView(a.list, LV_VIEW_DETAILS);
    if (a.small_images) ListView_SetImageList(a.list, a.small_images, LVSIL_SMALL);
    break;
  }
  InvalidateRect(a.list, nullptr, TRUE);
}
void set_view_mode(App& a, int mode) {
  a.settings.view_mode=std::clamp(mode,0,3); apply_view_mode(a); save_settings(a,false);
}
void apply_font(App &a) {
  NONCLIENTMETRICSW metrics{sizeof(metrics)};
  HFONT replacement = nullptr;
  if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics,
                            0)) {
    metrics.lfMessageFont.lfHeight -= a.settings.font_delta * 2;
    replacement = CreateFontIndirectW(&metrics.lfMessageFont);
  }
  HFONT font = replacement ? replacement
                           : reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
  for (HWND control : {a.search, a.search_edit, a.filter, a.list, a.preview,
                       a.status})
    if (control)
      SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  if (replacement) {
    if (a.ui_font)
      DeleteObject(a.ui_font);
    a.ui_font = replacement;
  }
  layout(a);
}
void apply_topmost(App &a) {
  const bool actively_searching =
      GetForegroundWindow() == a.window && search_has_focus(a);
  const bool topmost = a.settings.topmost_mode == 1 ||
                       (a.settings.topmost_mode == 2 && actively_searching);
  SetWindowPos(a.window, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
void resize_window(App &a, int mode) {
  a.settings.window_size_mode = std::clamp(mode, 0, 3);
  RECT dimensions{0, 0, 1180, 720};
  if (mode == 0)
    dimensions = {0, 0, 720, 480};
  else if (mode == 1)
    dimensions = {0, 0, 960, 640};
  else if (mode == 2)
    dimensions = {0, 0, 1440, 900};
  else {
    dimensions = {0, 0, 1180, 720};
    if (const HMONITOR monitor = MonitorFromWindow(a.window, MONITOR_DEFAULTTONEAREST)) {
      MONITORINFO info{sizeof(info)};
      if (GetMonitorInfoW(monitor, &info)) {
        dimensions.right = std::clamp<LONG>(
            (info.rcWork.right - info.rcWork.left) * 3 / 4, 900, 1500);
        dimensions.bottom = std::clamp<LONG>(
            (info.rcWork.bottom - info.rcWork.top) * 3 / 4, 600, 1000);
      }
    }
  }
  const int width = dimensions.right;
  const int height = dimensions.bottom;
  RECT current{};
  GetWindowRect(a.window, &current);
  const int x = current.left + ((current.right - current.left) - width) / 2;
  const int y = current.top + ((current.bottom - current.top) - height) / 2;
  ShowWindow(a.window, SW_RESTORE);
  SetWindowPos(a.window, nullptr, x, y, width, height,
               SWP_NOZORDER | SWP_NOACTIVATE);
}


void search(App &a, bool final_results = true);
void refresh_menu_state(App &a);
void apply_results(App &a, SearchPayload &p);
void start_metadata_hydration(App &a);
void sort_loaded_results(App &a);
esm::SortField service_sort_field(esm::SortField field);

std::optional<std::filesystem::path> choose_folder(HWND owner,
                                                   const wchar_t* title) {
  IFileDialog* dialog = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dialog)))) return std::nullopt;
  DWORD options{};
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  dialog->SetTitle(title);
  std::optional<std::filesystem::path> result;
  if (SUCCEEDED(dialog->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        result = std::filesystem::path(path);
        CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dialog->Release();
  return result;
}
bool paste_clipboard_files(App& a) {
  const auto* record = focused(a);
  if (!record)
    return false;
  auto clipboard = clipboard_files(a.window);
  if (clipboard.paths.empty())
    return false;
  const auto destination = record->directory
                               ? std::filesystem::path(record->path)
                               : std::filesystem::path(record->path).parent_path();
  if (destination.empty())
    return false;

  std::wstring source;
  for (const auto& path : clipboard.paths) {
    source += path;
    source.push_back(L'\0');
  }
  source.push_back(L'\0');
  std::wstring target = destination.wstring();
  target.push_back(L'\0');
  target.push_back(L'\0');

  SHFILEOPSTRUCTW operation{};
  operation.hwnd = a.window;
  operation.wFunc = clipboard.move ? FO_MOVE : FO_COPY;
  operation.pFrom = source.c_str();
  operation.pTo = target.c_str();
  operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR;
  const int result = SHFileOperationW(&operation);
  if (result != 0) {
    MessageBoxW(a.window, win_error(static_cast<DWORD>(result)).c_str(),
                L"粘贴失败", MB_OK | MB_ICONERROR);
    return false;
  }
  if (operation.fAnyOperationsAborted)
    return false;
  if (clipboard.move)
    OleSetClipboard(nullptr);
  status(a, clipboard.move ? L"已移动剪贴板文件" : L"已复制剪贴板文件");
  a.last_query.clear();
  search(a);
  return true;
}

void transfer_selected(App& a, bool move, bool advanced) {
  const auto paths = selected_paths(a);
  if (paths.empty()) return;

  std::optional<std::filesystem::path> destination;
  bool rename_on_collision = false;
  bool allow_undo = true;
  if (advanced) {
    std::vector<esm::gui::FormField> fields{
        {L"\u76ee\u6807\u6587\u4ef6\u5939", L""}};
    std::vector<esm::gui::FormCheck> checks{
        {L"\u540d\u79f0\u51b2\u7a81\u65f6\u81ea\u52a8\u91cd\u547d\u540d", true},
        {L"\u5141\u8bb8\u64a4\u9500\uff08\u653e\u5165\u56de\u6536\u7ad9\u64a4\u9500\u961f\u5217\uff09", true}};
    if (!esm::gui::show_form(
            a.window,
            move ? L"\u9ad8\u7ea7\u79fb\u52a8" : L"\u9ad8\u7ea7\u590d\u5236",
            fields, checks, move ? L"\u79fb\u52a8" : L"\u590d\u5236"))
      return;
    if (fields[0].value.empty()) {
      MessageBoxW(a.window, L"\u8bf7\u8f93\u5165\u76ee\u6807\u6587\u4ef6\u5939\u3002",
                  move ? L"\u9ad8\u7ea7\u79fb\u52a8" : L"\u9ad8\u7ea7\u590d\u5236",
                  MB_OK | MB_ICONINFORMATION);
      return;
    }
    destination = std::filesystem::path(fields[0].value);
    rename_on_collision = checks[0].checked;
    allow_undo = checks[1].checked;
  } else {
    destination = choose_folder(
        a.window, move ? L"\u9009\u62e9\u79fb\u52a8\u76ee\u6807\u6587\u4ef6\u5939"
                       : L"\u9009\u62e9\u590d\u5236\u76ee\u6807\u6587\u4ef6\u5939");
    if (!destination) return;
  }

  std::error_code create_error;
  std::filesystem::create_directories(*destination, create_error);
  if (create_error) {
    MessageBoxW(a.window, from_utf8(create_error.message()).c_str(),
                move ? L"\u79fb\u52a8\u5931\u8d25" : L"\u590d\u5236\u5931\u8d25",
                MB_OK | MB_ICONERROR);
    return;
  }

  std::wstring source;
  for (const auto& path : paths) {
    source += path;
    source.push_back(L'\0');
  }
  source.push_back(L'\0');
  std::wstring target = destination->wstring();
  target.push_back(L'\0');
  target.push_back(L'\0');

  SHFILEOPSTRUCTW operation{};
  operation.hwnd = a.window;
  operation.wFunc = move ? FO_MOVE : FO_COPY;
  operation.pFrom = source.c_str();
  operation.pTo = target.c_str();
  operation.fFlags = FOF_NOCONFIRMMKDIR;
  if (allow_undo) operation.fFlags |= FOF_ALLOWUNDO;
  if (rename_on_collision) operation.fFlags |= FOF_RENAMEONCOLLISION;
  if (!advanced) operation.fFlags |= FOF_SIMPLEPROGRESS;
  const int result = SHFileOperationW(&operation);
  if (result != 0) {
    MessageBoxW(a.window, win_error(static_cast<DWORD>(result)).c_str(),
                move ? L"\u79fb\u52a8\u5931\u8d25" : L"\u590d\u5236\u5931\u8d25",
                MB_OK | MB_ICONERROR);
  } else if (!operation.fAnyOperationsAborted) {
    status(a, move ? L"\u5df2\u79fb\u52a8\u6240\u9009\u9879"
                   : L"\u5df2\u590d\u5236\u6240\u9009\u9879");
  }
}
void open_file_list(App& a) {
  wchar_t file_name[32768]{};
  OPENFILENAMEW dialog{sizeof(dialog)};
  dialog.hwndOwner = a.window;
  dialog.lpstrFilter =
      L"Everything \u6587\u4ef6\u5217\u8868 (*.efu)\0*.efu\0CSV \u6587\u4ef6 (*.csv)\0*.csv\0\u6240\u6709\u6587\u4ef6\0*.*\0";
  dialog.lpstrFile = file_name;
  dialog.nMaxFile = static_cast<DWORD>(std::size(file_name));
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
  if (!GetOpenFileNameW(&dialog)) return;
  std::vector<esm::FileRecord> records;
  std::wstring error;
  if (!esm::load_efu_file(file_name, records, error)) {
    MessageBoxW(a.window, error.c_str(), L"\u6253\u5f00\u6587\u4ef6\u5217\u8868\u5931\u8d25",
                MB_OK | MB_ICONERROR);
    return;
  }
  a.file_list_index = std::make_unique<esm::MetadataIndex>();
  a.file_list_index->replace(records);
  a.file_list_path = file_name;
  a.last_query.clear();
  const auto title = L"everything_sm - " + a.file_list_path.filename().wstring();
  SetWindowTextW(a.window, title.c_str());
  search(a);
  status(a, L"\u5df2\u52a0\u8f7d\u6587\u4ef6\u5217\u8868\uff0c\u5171 " +
                std::to_wstring(records.size()) + L" \u9879");
}
void close_file_list(App& a) {
  a.file_list_index.reset();
  a.file_list_path.clear();
  SetWindowTextW(a.window, L"everything_sm");
  a.last_query.clear();
  search(a);
}
bool choose_efu_file(HWND owner, bool save, std::filesystem::path& value) {
  wchar_t file_name[32768]{};
  if (!value.empty())
    wcsncpy_s(file_name, value.wstring().c_str(), _TRUNCATE);
  OPENFILENAMEW dialog{sizeof(dialog)};
  dialog.hwndOwner = owner;
  dialog.lpstrFilter =
      save ? L"Everything \u6587\u4ef6\u5217\u8868 (*.efu)\0*.efu\0"
           : L"Everything \u6587\u4ef6\u5217\u8868 (*.efu)\0*.efu\0CSV \u6587\u4ef6 (*.csv)\0*.csv\0\u6240\u6709\u6587\u4ef6\0*.*\0";
  dialog.lpstrFile = file_name;
  dialog.nMaxFile = static_cast<DWORD>(std::size(file_name));
  dialog.lpstrDefExt = L"efu";
  dialog.Flags = OFN_PATHMUSTEXIST |
                 (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
  const BOOL accepted = save ? GetSaveFileNameW(&dialog)
                             : GetOpenFileNameW(&dialog);
  if (!accepted)
    return false;
  value = file_name;
  return true;
}

std::wstring normalized_path_key(const std::filesystem::path& path) {
  auto value = path.lexically_normal().wstring();
  std::replace(value.begin(), value.end(), L'/', L'\\');
  while (value.size() > 3 && !value.empty() && value.back() == L'\\')
    value.pop_back();
  if (!value.empty())
    CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
  return value;
}

bool path_is_same_or_child(std::wstring_view candidate,
                           std::wstring_view root) {
  if (root.empty() || candidate.size() < root.size() ||
      _wcsnicmp(candidate.data(), root.data(), root.size()) != 0)
    return false;
  return candidate.size() == root.size() || root.back() == L'\\' ||
         candidate[root.size()] == L'\\';
}

void deduplicate_file_list(std::vector<esm::FileRecord>& records) {
  std::unordered_set<std::wstring> paths;
  std::erase_if(records, [&](const esm::FileRecord& record) {
    return !paths.insert(normalized_path_key(record.path)).second;
  });
}

void create_file_list(App& a) {
  std::vector<esm::FileRecord> records;
  std::filesystem::path file_path;
  bool dirty = false;

  const int mode = MessageBoxW(
      a.window,
      L"\u8bf7\u9009\u62e9\u6587\u4ef6\u5217\u8868\u7f16\u8f91\u65b9\u5f0f\uff1a\n\n"
      L"\u201c\u662f\u201d\u2014\u2014\u6253\u5f00\u5e76\u7f16\u8f91\u73b0\u6709 EFU/CSV \u6587\u4ef6\u5217\u8868\n"
      L"\u201c\u5426\u201d\u2014\u2014\u65b0\u5efa\u7a7a\u767d EFU \u6587\u4ef6\u5217\u8868\n"
      L"\u201c\u53d6\u6d88\u201d\u2014\u2014\u8fd4\u56de",
      L"\u6587\u4ef6\u5217\u8868\u7f16\u8f91\u5668",
      MB_YESNOCANCEL | MB_ICONQUESTION);
  if (mode == IDCANCEL)
    return;
  if (mode == IDYES) {
    if (!choose_efu_file(a.window, false, file_path))
      return;
    std::wstring error;
    if (!esm::load_efu_file(file_path, records, error)) {
      MessageBoxW(a.window, error.c_str(),
                  L"\u6587\u4ef6\u5217\u8868\u7f16\u8f91\u5668",
                  MB_OK | MB_ICONERROR);
      return;
    }
    deduplicate_file_list(records);
  }

  for (;;) {
    std::wostringstream prompt;
    prompt << L"\u5f53\u524d\u6587\u4ef6\u5217\u8868\u5305\u542b " << records.size()
           << L" \u9879\u3002\n\n"
           << L"\u201c\u662f\u201d\u2014\u2014\u6dfb\u52a0\u6216\u91cd\u65b0\u626b\u63cf\u4e00\u4e2a\u6587\u4ef6\u5939\n"
           << L"\u201c\u5426\u201d\u2014\u2014\u6309\u8def\u5f84\u79fb\u9664\u8bb0\u5f55\n"
           << L"\u201c\u53d6\u6d88\u201d\u2014\u2014\u5b8c\u6210\u7f16\u8f91\u5e76\u4fdd\u5b58";
    const int action = MessageBoxW(
        a.window, prompt.str().c_str(),
        L"\u6587\u4ef6\u5217\u8868\u7f16\u8f91\u5668",
        MB_YESNOCANCEL | MB_ICONINFORMATION);
    if (action == IDYES) {
      const auto folder = choose_folder(
          a.window,
          L"\u9009\u62e9\u8981\u6dfb\u52a0\u5230\u6587\u4ef6\u5217\u8868\u7684\u6587\u4ef6\u5939");
      if (!folder)
        continue;
      status(a, L"\u6587\u4ef6\u5217\u8868\u7f16\u8f91\u5668\u6b63\u5728\u626b\u63cf " +
                    folder->wstring() + L" ...");
      const auto scan = esm::scan_directories({*folder});
      const auto root_key = normalized_path_key(*folder);
      std::erase_if(records, [&](const esm::FileRecord& record) {
        return path_is_same_or_child(normalized_path_key(record.path), root_key);
      });
      records.insert(records.end(), scan.records.begin(), scan.records.end());
      deduplicate_file_list(records);
      dirty = true;
      std::wostringstream completed;
      completed << L"\u5df2\u6dfb\u52a0 " << scan.records.size() << L" \u9879";
      if (scan.errors)
        completed << L"\uff0c\u6709 " << scan.errors << L" \u9879\u65e0\u6cd5\u8bfb\u53d6";
      status(a, completed.str());
      continue;
    }
    if (action == IDNO) {
      std::vector<esm::gui::FormField> fields{
          {L"\u8def\u5f84\u6216\u76ee\u5f55\u524d\u7f00", L""}};
      std::vector<esm::gui::FormCheck> checks{
          {L"\u540c\u65f6\u79fb\u9664\u8be5\u76ee\u5f55\u4e0b\u7684\u6240\u6709\u5b50\u9879", true}};
      if (!esm::gui::show_form(
              a.window, L"\u4ece\u6587\u4ef6\u5217\u8868\u79fb\u9664", fields,
              checks, L"\u79fb\u9664(&R)"))
        continue;
      if (fields[0].value.empty()) {
        MessageBoxW(a.window,
                    L"\u8bf7\u8f93\u5165\u8981\u79fb\u9664\u7684\u8def\u5f84\u3002",
                    L"\u6587\u4ef6\u5217\u8868\u7f16\u8f91\u5668",
                    MB_OK | MB_ICONWARNING);
        continue;
      }
      const auto remove_key = normalized_path_key(fields[0].value);
      const auto old_size = records.size();
      std::erase_if(records, [&](const esm::FileRecord& record) {
        const auto record_key = normalized_path_key(record.path);
        return checks[0].checked
                   ? path_is_same_or_child(record_key, remove_key)
                   : _wcsicmp(record_key.c_str(), remove_key.c_str()) == 0;
      });
      const auto removed = old_size - records.size();
      dirty = dirty || removed != 0;
      status(a, L"\u5df2\u4ece\u6587\u4ef6\u5217\u8868\u79fb\u9664 " +
                    std::to_wstring(removed) + L" \u9879");
      continue;
    }

    if (!dirty) {
      status(a, L"\u6587\u4ef6\u5217\u8868\u672a\u4fee\u6539");
      return;
    }

    std::filesystem::path destination = file_path;
    if (!file_path.empty()) {
      const int save_mode = MessageBoxW(
          a.window,
          (L"\u662f\u5426\u8986\u76d6\u4fdd\u5b58\u5230\uff1a\n" + file_path.wstring() +
           L"\n\n\u9009\u62e9\u201c\u5426\u201d\u53ef\u53e6\u5b58\u4e3a\u65b0\u6587\u4ef6\u3002").c_str(),
          L"\u4fdd\u5b58\u6587\u4ef6\u5217\u8868",
          MB_YESNOCANCEL | MB_ICONQUESTION);
      if (save_mode == IDCANCEL)
        continue;
      if (save_mode == IDNO)
        destination.clear();
    }
    if (destination.empty()) {
      destination = file_path.empty() ? std::filesystem::path(L"file-list.efu")
                                      : file_path;
      destination.replace_extension(L".efu");
      if (!choose_efu_file(a.window, true, destination))
        continue;
    }

    std::wstring error;
    if (!esm::save_efu_file(destination, records, error)) {
      MessageBoxW(a.window, error.c_str(),
                  L"\u4fdd\u5b58\u6587\u4ef6\u5217\u8868\u5931\u8d25",
                  MB_OK | MB_ICONERROR);
      continue;
    }
    file_path = destination;
    status(a, L"\u5df2\u4fdd\u5b58\u6587\u4ef6\u5217\u8868\uff0c\u5171 " +
                  std::to_wstring(records.size()) + L" \u9879\uff1a" +
                  file_path.wstring());

    if (MessageBoxW(
            a.window,
            L"\u6587\u4ef6\u5217\u8868\u5df2\u4fdd\u5b58\u3002\u662f\u5426\u7acb\u5373\u5728\u5f53\u524d\u641c\u7d22\u7a97\u53e3\u6253\u5f00\uff1f",
            L"\u6587\u4ef6\u5217\u8868\u7f16\u8f91\u5668",
            MB_YESNO | MB_ICONQUESTION) == IDYES) {
      a.file_list_index = std::make_unique<esm::MetadataIndex>();
      a.file_list_index->replace(records);
      a.file_list_path = file_path;
      a.last_query.clear();
      const auto title = L"everything_sm - " + file_path.filename().wstring();
      SetWindowTextW(a.window, title.c_str());
      search(a);
    }
    return;
  }
}

std::wstring quote_value(std::wstring_view value) {
  std::wstring out = L"\"";
  for (const wchar_t ch : value) { if (ch == L'\\' || ch == L'\"') out.push_back(L'\\'); out.push_back(ch); }
  out.push_back(L'\"'); return out;
}
void advanced_search(App& a) {
  std::vector<esm::gui::FormField> fields{{L"\u5305\u542b\u6240\u6709\u8fd9\u4e9b\u8bcd", L""},
      {L"\u5305\u542b\u5b8c\u6574\u77ed\u8bed", L""}, {L"\u5305\u542b\u4efb\u610f\u8fd9\u4e9b\u8bcd\uff08\u7528\u7a7a\u683c\u5206\u9694\uff09", L""},
      {L"\u4e0d\u5305\u542b\u8fd9\u4e9b\u8bcd\uff08\u7528\u7a7a\u683c\u5206\u9694\uff09", L""}, {L"\u6269\u5c55\u540d\uff08\u4f8b\u5982 pdf docx\uff09", L""},
      {L"\u6700\u5c0f\u5927\u5c0f\uff08\u4f8b\u5982 10mb\uff09", L""},
      {L"\u6700\u5927\u5927\u5c0f\uff08\u4f8b\u5982 1gb\uff09", L""},
      {L"\u6587\u4ef6\u540d\u5f00\u5934\u4e3a", L""},
      {L"\u6587\u4ef6\u540d\u7ed3\u5c3e\u4e3a", L""},
      {L"\u76f4\u63a5\u4f4d\u4e8e\u6587\u4ef6\u5939\uff08\u4e0d\u542b\u5b50\u6587\u4ef6\u5939\uff09", L""}};
  std::vector<esm::gui::FormCheck> checks{{L"\u533a\u5206\u5927\u5c0f\u5199", a.settings.case_sensitive},
      {L"\u5168\u5b57\u5339\u914d", a.settings.whole_word}, {L"\u5339\u914d\u8def\u5f84", a.settings.match_path},
      {L"\u5339\u914d\u53d8\u97f3\u6807\u8bb0", a.settings.match_diacritics}};
  if (!esm::gui::show_form(a.window, L"\u9ad8\u7ea7\u641c\u7d22", fields, checks, L"\u641c\u7d22")) return;
  std::vector<std::wstring> clauses;
  if (!fields[0].value.empty()) clauses.push_back(fields[0].value);
  if (!fields[1].value.empty()) clauses.push_back(quote_value(fields[1].value));
  auto words = [](std::wstring value) { std::wistringstream in(value); std::vector<std::wstring> out; std::wstring word; while (in >> word) out.push_back(word); return out; };
  const auto any = words(fields[2].value);
  if (!any.empty()) { std::wstring c=L"("; for(size_t i=0;i<any.size();++i){if(i)c+=L" OR ";c+=quote_value(any[i]);} c+=L")"; clauses.push_back(c); }
  for (const auto& word : words(fields[3].value)) clauses.push_back(L"NOT " + quote_value(word));
  const auto exts = words(fields[4].value);
  if (!exts.empty()) { std::wstring c=L"("; for(size_t i=0;i<exts.size();++i){if(i)c+=L" OR ";c+=L"ext:"+exts[i];} c+=L")"; clauses.push_back(c); }
  if (!fields[5].value.empty()) clauses.push_back(L"size:>=" + fields[5].value);
  if (!fields[6].value.empty()) clauses.push_back(L"size:<=" + fields[6].value);
  if (!fields[7].value.empty())
    clauses.push_back(L"startwith:" + quote_value(fields[7].value));
  if (!fields[8].value.empty())
    clauses.push_back(L"endwith:" + quote_value(fields[8].value));
  if (!fields[9].value.empty())
    clauses.push_back(L"parent:" + quote_value(fields[9].value));
  std::wstring query;
  for (const auto& clause : clauses) { if (!query.empty()) query += L" AND "; query += clause; }
  a.settings.case_sensitive=checks[0].checked; a.settings.whole_word=checks[1].checked;
  a.settings.match_path=checks[2].checked; a.settings.match_diacritics=checks[3].checked;
  a.settings.regex_mode=false; save_settings(a,false);
  SetWindowTextW(a.search, query.c_str()); focus_search(a);
}
std::wstring saved_search_lines(const std::vector<esm::SavedSearch>& values) {
  std::wstring output;
  for (const auto& value : values) {
    output += value.name + L"\t" + value.query + L"\t";
    output += value.case_sensitive ? L"1\t" : L"0\t";
    output += value.whole_word ? L"1\t" : L"0\t";
    output += value.match_path ? L"1\t" : L"0\t";
    output += value.match_diacritics ? L"1\t" : L"0\t";
    output += value.regex ? L"1\r\n" : L"0\r\n";
  }
  return output;
}

bool saved_search_flag(std::wstring_view value) {
  return value == L"1" || value == L"true" || value == L"yes";
}

void parse_saved_search_lines(std::wstring_view text,
                              std::vector<esm::SavedSearch>& values) {
  values.clear();
  std::wistringstream input{std::wstring(text)};
  std::wstring line;
  while (std::getline(input, line) && values.size() < 100) {
    if (!line.empty() && line.back() == L'\r') line.pop_back();
    if (line.empty()) continue;
    std::vector<std::wstring> parts;
    std::size_t begin = 0;
    for (;;) {
      const auto tab = line.find(L'\t', begin);
      parts.push_back(line.substr(begin, tab == std::wstring::npos
                                            ? std::wstring::npos
                                            : tab - begin));
      if (tab == std::wstring::npos) break;
      begin = tab + 1;
    }
    const auto name = parts.empty() ? std::wstring{} : parts[0];
    const auto query = parts.size() < 2 ? name : parts[1];
    if (query.empty()) continue;
    esm::SavedSearch value{name.empty() ? query : name, query};
    if (parts.size() >= 7) {
      value.case_sensitive = saved_search_flag(parts[2]);
      value.whole_word = saved_search_flag(parts[3]);
      value.match_path = saved_search_flag(parts[4]);
      value.match_diacritics = saved_search_flag(parts[5]);
      value.regex = saved_search_flag(parts[6]);
    }
    values.push_back(std::move(value));
  }
}

void cancel_service_query(App &a) {
  if (a.service_query_in_flight && a.service_query_thread)
    (void)CancelSynchronousIo(a.service_query_thread);
}

void schedule_search(App &a) {
  // The edit control and its parent combo box can both report one user edit.
  // Coalesce those notifications before changing the generation or timer.
  auto current_text = search_text(a);
  if (current_text == a.scheduled_query_text)
    return;
  a.scheduled_query_text = std::move(current_text);

  // Do not cancel the synchronous named-pipe request on every keypress. The
  // server may still be completing that abandoned request, and a burst of
  // cancellations can temporarily occupy every pipe worker. Keep at most one
  // request in flight and let WM_ESM_RESULTS immediately launch the newest
  // coalesced query instead.
  ++a.generation; // Invalidate search/metadata work for the previous text now.
  a.metadata_pending = false;
  if (a.metadata_cancel) a.metadata_cancel->store(true);
  a.history_pending = false;
  a.refinement_pending = false;
  a.service_retry_count = 0;
  a.query_pending = true;
  a.query_pending_final_results = false;
  const auto now = GetTickCount64();
  const auto debounce =
      esm::interactive_search_debounce_ms(now, a.last_query_input_tick);
  a.last_query_input_tick = now;
  a.query_due_tick = now + debounce;
  a.refinement_due_tick = now + RESULT_REFINEMENT_DEBOUNCE_MS;
}

bool retryable_pipe_error(DWORD error) {
  return error == ERROR_SEM_TIMEOUT || error == ERROR_PIPE_BUSY ||
         error == ERROR_FILE_NOT_FOUND || error == ERROR_BROKEN_PIPE ||
         error == ERROR_NO_DATA || error == ERROR_PIPE_NOT_CONNECTED;
}

void schedule_service_retry(App &a) {
  ++a.service_retry_count;
  const auto delay = std::min<ULONGLONG>(
      SEARCH_RETRY_BASE_MS * a.service_retry_count, SEARCH_RETRY_MAX_MS);
  a.query_pending = true;
  a.query_due_tick = GetTickCount64() + delay;
}
std::wstring filter_expression(const App& a, int filter) {
  switch (filter) {
  case 1:
    return L"(ext:mp3 OR ext:wav OR ext:flac OR ext:aac OR ext:m4a OR ext:ogg OR ext:wma)";
  case 2:
    return L"(ext:zip OR ext:7z OR ext:rar OR ext:tar OR ext:gz OR ext:bz2 OR ext:xz)";
  case 3:
    return L"(ext:doc OR ext:docx OR ext:xls OR ext:xlsx OR ext:ppt OR ext:pptx OR ext:pdf OR ext:txt OR ext:md OR ext:rtf)";
  case 4:
    return L"(ext:exe OR ext:com OR ext:bat OR ext:cmd OR ext:msi OR ext:ps1)";
  case 5:
    return L"folder:";
  case 6:
    return L"(ext:jpg OR ext:jpeg OR ext:png OR ext:gif OR ext:bmp OR ext:webp OR ext:tif OR ext:tiff OR ext:svg OR ext:ico)";
  case 7:
    return L"(ext:mp4 OR ext:mkv OR ext:avi OR ext:mov OR ext:wmv OR ext:webm OR ext:m4v OR ext:flv)";
  default:
    if (filter >= 8 && static_cast<std::size_t>(filter - 8) < a.custom_filters.size())
      return a.custom_filters[static_cast<std::size_t>(filter - 8)].query;
    return {};
  }
}
std::wstring regex_expression(std::wstring query) {
  std::wstring escaped;
  escaped.reserve(query.size() + 8);
  for (const wchar_t ch : query) {
    if (ch == L'\\' || ch == L'"')
      escaped.push_back(L'\\');
    escaped.push_back(ch);
  }
  return L"regex:\"" + escaped + L"\"";
}
void search(App &a, bool final_results) {
  if (!a.file_list_index && a.service_query_in_flight) {
    a.query_pending = true;
    a.query_pending_final_results = final_results;
    return;
  }
  a.query_pending = false;
  a.query_pending_final_results = true;
  auto q = search_text(a);
  const auto filter = filter_expression(a, a.settings.search_filter);
  std::wstring effective = q;
  if (a.settings.regex_mode && !effective.empty())
    effective = regex_expression(std::move(effective));
  if (!filter.empty())
    effective = effective.empty() ? filter : L"(" + effective + L") AND " + filter;

  const auto configured_limit = static_cast<std::uint32_t>(std::min(
      a.settings.result_limit, static_cast<int>(esm::ipc_max_search_results)));
  const bool partial_results = !final_results && !a.file_list_index &&
                               configured_limit > INTERACTIVE_RESULT_LIMIT;
  const bool is_final_results = !partial_results;
  const auto limit = partial_results
                         ? std::min(configured_limit, INTERACTIVE_RESULT_LIMIT)
                         : configured_limit;
  const auto cache_key =
      q + L"\x1f" + std::to_wstring(a.settings.search_filter) + L"\x1f" +
      (a.settings.regex_mode ? L"1" : L"0") + L"\x1f" +
      (a.file_list_index ? a.file_list_path.wstring() : L"service") + L"\x1f" +
      (is_final_results ? L"full" : L"interactive");
  if (cache_key == a.last_query)
    return;
  a.last_query = cache_key;
  if (is_final_results)
    a.refinement_pending = false;
  apply_topmost(a);
  auto generation = ++a.generation;
  if (effective.empty()) {
    a.metadata_pending = false;
    a.refinement_pending = false;
    a.history_pending = false;
    a.results.clear();
    ListView_SetItemCountEx(a.list, 0, 0);
    status(a, L"\u8bf7\u8f93\u5165\u67e5\u8be2");
    return;
  }
  status(a, L"\u6b63\u5728\u641c\u7d22\u2026");
  auto pipe = a.pipe;
  auto sort = a.file_list_index ? a.sort : service_sort_field(a.sort);
  const bool needs_metadata_hydration =
      !a.file_list_index &&
      (a.sort == esm::SortField::creation_time ||
       a.sort == esm::SortField::last_access_time ||
       a.sort == esm::SortField::change_time);
  auto descending = a.descending;
  const bool case_sensitive = a.settings.case_sensitive;
  const bool whole_word = a.settings.whole_word;
  const bool match_path = a.settings.match_path;
  const bool match_diacritics = a.settings.match_diacritics;
  if (a.file_list_index) {
    SearchPayload payload;
    payload.query = q;
    payload.generation = generation;
    payload.requested_limit = limit;
    payload.final_results = true;
    esm::SearchOptions options;
    options.limit = limit;
    options.case_sensitive = case_sensitive;
    options.whole_word = whole_word;
    options.match_path = match_path;
    options.match_diacritics = match_diacritics;
    options.sort = sort;
    options.descending = descending;
    const auto started = std::chrono::steady_clock::now();
    payload.result.response.results = a.file_list_index->search(effective, options);
    payload.result.response.elapsed_microseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
    apply_results(a, payload);
    return;
  }
  a.service_query_in_flight = true;
  HWND window = a.window;
  std::thread query_thread(
      [q, effective, pipe, sort, descending, case_sensitive, whole_word,
       match_path, match_diacritics, limit, generation, window,
       is_final_results, needs_metadata_hydration] {
        auto p = std::make_unique<SearchPayload>();
        p->query = q;
        p->generation = generation;
        p->requested_limit = limit;
        p->final_results = is_final_results;
        p->needs_metadata_hydration = needs_metadata_hydration;
        esm::IpcSearchRequest request;
        request.limit = limit;
        request.case_sensitive = case_sensitive;
        request.whole_word = whole_word;
        request.match_path = match_path;
        request.match_diacritics = match_diacritics;
        request.sort = sort;
        request.descending = descending;
        request.query = effective;
        p->result = esm::query_named_pipe_search(pipe, request, 5000);
        if (PostMessageW(window, WM_ESM_RESULTS, 0,
                         reinterpret_cast<LPARAM>(p.get())))
          p.release();
      });
  HANDLE thread_handle = nullptr;
  if (DuplicateHandle(GetCurrentProcess(),
                      reinterpret_cast<HANDLE>(query_thread.native_handle()),
                      GetCurrentProcess(), &thread_handle, 0, FALSE,
                      DUPLICATE_SAME_ACCESS)) {
    if (a.service_query_thread)
      CloseHandle(a.service_query_thread);
    a.service_query_thread = thread_handle;
  }
  query_thread.detach();
}

void start_metadata_hydration(App &a) {
  if (!a.metadata_pending || a.metadata_in_flight || a.query_pending ||
      a.service_query_in_flight || a.closing || a.results.empty()) {
    return;
  }

  const auto generation = a.generation.load();
  auto requests = esm::make_result_metadata_requests(
      a.results, a.metadata_only_missing_basic);
  const HWND window = a.window;
  auto cancel = std::make_shared<std::atomic_bool>(false);
  a.metadata_cancel = cancel;
  a.metadata_pending = false;
  a.metadata_in_flight = true;
  std::thread metadata_thread(
      [generation, window, cancel, requests = std::move(requests)]() mutable {
        auto payload = std::make_unique<MetadataPayload>();
        payload->generation = generation;
        payload->updates.resize(requests.size());

        std::atomic_size_t next{};
        const auto available = std::thread::hardware_concurrency();
        const auto worker_count = std::min<std::size_t>(
            requests.size(), std::clamp<std::size_t>(available == 0 ? 1 : available,
                                                     1, 4));
        constexpr std::size_t claim_size = 16;
        std::vector<std::jthread> workers;
        workers.reserve(worker_count);
        for (std::size_t worker = 0; worker < worker_count; ++worker) {
          workers.emplace_back([&] {
            for (;;) {
              if (cancel->load(std::memory_order_relaxed))
                break;
              const auto begin =
                  next.fetch_add(claim_size, std::memory_order_relaxed);
              if (begin >= requests.size())
                break;
              const auto end = std::min(begin + claim_size, requests.size());
              for (auto index = begin; index < end; ++index) {
                if (cancel->load(std::memory_order_relaxed))
                  break;
                esm::FileRecord record;
                record.path = requests[index].path;
                auto &update = payload->updates[index];
                update.result_index = requests[index].result_index;
                update.succeeded = esm::hydrate_file_metadata(record);
                if (!update.succeeded)
                  continue;
                update.size = record.size;
                update.last_write_time = record.last_write_time;
                update.creation_time = record.creation_time;
                update.last_access_time = record.last_access_time;
                update.change_time = record.change_time;
                update.attributes = record.attributes;
                update.directory = record.directory;
              }
            }
          });
        }
        for (auto &worker : workers)
          worker.join();

        if (PostMessageW(window, WM_ESM_METADATA, 0,
                         reinterpret_cast<LPARAM>(payload.get()))) {
          payload.release();
        }
      });

  HANDLE thread_handle = nullptr;
  if (DuplicateHandle(GetCurrentProcess(),
                      reinterpret_cast<HANDLE>(metadata_thread.native_handle()),
                      GetCurrentProcess(), &thread_handle, 0, FALSE,
                      DUPLICATE_SAME_ACCESS)) {
    if (a.metadata_thread)
      CloseHandle(a.metadata_thread);
    a.metadata_thread = thread_handle;
  }
  metadata_thread.detach();
}

void apply_results(App &a, SearchPayload &p) {
  if (p.generation != a.generation.load())
    return;
  if (p.result.error) {
    if (!a.file_list_index) {
      a.last_query.clear();
      if (retryable_pipe_error(p.result.error)) {
        const bool busy = p.result.error == ERROR_SEM_TIMEOUT ||
                          p.result.error == ERROR_PIPE_BUSY;
        a.service_connected = busy;
        a.query_pending_final_results = p.final_results;
        schedule_service_retry(a);
        status(a, busy
                      ? L"\u641c\u7d22\u670d\u52a1\u7e41\u5fd9\uff0c"
                        L"\u6b63\u5728\u81ea\u52a8\u91cd\u8bd5\u2026"
                      : L"\u6b63\u5728\u7b49\u5f85\u641c\u7d22\u670d\u52a1\uff0c"
                        L"\u7a0d\u540e\u81ea\u52a8\u91cd\u8bd5\u2026");
      } else {
        a.service_connected = false;
        status(a, L"\u670d\u52a1\u4e0d\u53ef\u7528: " + win_error(p.result.error));
      }
      refresh_menu_state(a);
    }
    return;
  }
  if (!a.file_list_index) {
    a.service_connected = true;
    a.service_retry_count = 0;
  }
  a.results = std::move(p.result.response.results);
  const bool response_is_complete = esm::interactive_search_response_is_complete(
      p.final_results, a.results.size(), p.requested_limit);
  if (a.file_list_index &&
      (a.sort == esm::SortField::creation_time ||
       a.sort == esm::SortField::last_access_time ||
       a.sort == esm::SortField::change_time)) {
    for (auto &result : a.results)
      (void)esm::hydrate_file_metadata(result.record);
  }
  const bool missing_basic_metadata =
      !a.file_list_index &&
      std::any_of(a.results.begin(), a.results.end(), [](const auto &result) {
        return result.record.last_write_time == 0;
      });
  a.metadata_pending =
      response_is_complete && !a.results.empty() &&
      (p.needs_metadata_hydration || missing_basic_metadata);
  a.metadata_only_missing_basic =
      a.metadata_pending && !p.needs_metadata_hydration;
  if (a.metadata_pending)
    a.metadata_due_tick = GetTickCount64() + METADATA_DEBOUNCE_MS;

  a.refinement_pending =
      !response_is_complete &&
      p.requested_limit < static_cast<std::uint32_t>(a.settings.result_limit);
  if (response_is_complete) {
    a.pending_history_query = p.query;
    a.history_pending = !p.query.empty();
    a.history_due_tick = GetTickCount64() + HISTORY_DEBOUNCE_MS;
  }

  sort_loaded_results(a);
  ListView_SetItemCountEx(a.list, static_cast<int>(a.results.size()),
                          LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
  InvalidateRect(a.list, nullptr, FALSE);
  std::wostringstream output;
  output << a.results.size();
  if (!response_is_complete)
    output << L"+";
  output << L" \u4e2a\u7ed3\u679c\uff0c" << std::fixed << std::setprecision(2)
         << static_cast<double>(p.result.response.elapsed_microseconds) / 1000
         << L" ms";
  status(a, output.str());
  refresh_menu_state(a);
}

void update_sort_arrows(App &a, int selected_column) {
  HWND header = ListView_GetHeader(a.list);
  int count = Header_GetItemCount(header);
  for (int i = 0; i < count; ++i) {
    HDITEMW item{};
    item.mask = HDI_FORMAT;
    if (!Header_GetItem(header, i, &item))
      continue;
    item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
    if (i == selected_column)
      item.fmt |= a.descending ? HDF_SORTDOWN : HDF_SORTUP;
    Header_SetItem(header, i, &item);
  }
}
void write_service_pipe(const App& a) {
  if (a.service_pipe_path.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(a.service_pipe_path.parent_path(), ec);
  std::ofstream out(a.service_pipe_path, std::ios::binary | std::ios::trunc);
  const auto bytes = to_utf8(a.local_pipe);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  out.put('\n');
}

void capture_column_settings(App &a) {
  if (!a.list || a.visible_columns.empty())
    return;
  const int count = static_cast<int>(a.visible_columns.size());
  for (int physical = 0; physical < count; ++physical) {
    const int width = ListView_GetColumnWidth(a.list, physical);
    if (width > 0)
      column_settings(a, a.visible_columns[physical]).width = width;
  }

  std::vector<int> order(static_cast<std::size_t>(count));
  if (!ListView_GetColumnOrderArray(a.list, count, order.data()))
    for (int i = 0; i < count; ++i)
      order[static_cast<std::size_t>(i)] = i;
  std::vector<esm::GuiColumnId> visible_display;
  visible_display.reserve(a.visible_columns.size());
  for (const int physical : order)
    if (physical >= 0 && physical < count)
      visible_display.push_back(a.visible_columns[physical]);
  if (visible_display.size() != a.visible_columns.size())
    visible_display = a.visible_columns;

  std::array<esm::GuiColumnId, esm::gui_column_count> global{};
  for (std::size_t i = 0; i < global.size(); ++i)
    global[i] = a.settings.columns[i].id;
  std::stable_sort(global.begin(), global.end(), [&](auto left, auto right) {
    return column_settings(a, left).order < column_settings(a, right).order;
  });
  std::size_t visible_index = 0;
  for (auto &id : global)
    if (column_settings(a, id).visible)
      id = visible_display[visible_index++];
  for (std::size_t order_index = 0; order_index < global.size(); ++order_index)
    column_settings(a, global[order_index]).order =
        static_cast<int>(order_index);
}

void capture_window_settings(App &a) {
  if (!a.window)
    return;
  WINDOWPLACEMENT placement{};
  placement.length = sizeof(placement);
  if (!GetWindowPlacement(a.window, &placement))
    return;
  const auto &rect = placement.rcNormalPosition;
  a.settings.window_valid = true;
  a.settings.window_x = rect.left;
  a.settings.window_y = rect.top;
  a.settings.window_width = rect.right - rect.left;
  a.settings.window_height = rect.bottom - rect.top;
  a.settings.maximized = IsZoomed(a.window) != FALSE;
}

void save_settings(App &a, bool capture_columns,
                   bool capture_window) {
  if (capture_columns)
    capture_column_settings(a);
  if (capture_window)
    capture_window_settings(a);
  a.settings.sort_field = static_cast<int>(a.sort);
  a.settings.descending = a.descending;
  a.settings.show_preview = a.show_preview;
  esm::normalize_gui_settings(a.settings);
  write_settings(a);
}

void rebuild_columns(App &a, bool capture_existing) {
  if (capture_existing)
    capture_column_settings(a);
  while (ListView_DeleteColumn(a.list, 0)) {
  }
  esm::normalize_gui_settings(a.settings);

  a.visible_columns.clear();
  for (const auto &definition : column_definitions) {
    const auto &setting = column_settings(a, definition.id);
    if (!setting.visible)
      continue;
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    column.pszText = const_cast<wchar_t *>(definition.title);
    column.cx = setting.width;
    column.fmt = definition.format;
    const int physical = static_cast<int>(a.visible_columns.size());
    if (ListView_InsertColumn(a.list, physical, &column) >= 0)
      a.visible_columns.push_back(definition.id);
  }

  std::vector<esm::GuiColumnId> display_columns = a.visible_columns;
  std::stable_sort(display_columns.begin(), display_columns.end(),
                   [&](auto left, auto right) {
                     return column_settings(a, left).order <
                            column_settings(a, right).order;
                   });
  std::vector<int> display_order;
  display_order.reserve(display_columns.size());
  for (const auto id : display_columns) {
    const auto found =
        std::find(a.visible_columns.begin(), a.visible_columns.end(), id);
    if (found != a.visible_columns.end())
      display_order.push_back(
          static_cast<int>(std::distance(a.visible_columns.begin(), found)));
  }
  if (!display_order.empty())
    ListView_SetColumnOrderArray(a.list,
                                 static_cast<int>(display_order.size()),
                                 display_order.data());

  int sorted_column = -1;
  for (std::size_t i = 0; i < a.visible_columns.size(); ++i)
    if (column_definition(a.visible_columns[i]).sort == a.sort)
      sorted_column = static_cast<int>(i);
  update_sort_arrows(a, sorted_column);
  InvalidateRect(a.list, nullptr, TRUE);
}

void append_column_items(App &a, HMENU menu) {
  const auto visible_count = std::count_if(
      a.settings.columns.begin(), a.settings.columns.end(),
      [](const esm::GuiColumnSettings &column) { return column.visible; });
  for (const auto &definition : column_definitions) {
    const auto &setting = column_settings(a, definition.id);
    UINT flags = MF_STRING | (setting.visible ? MF_CHECKED : 0);
    if (setting.visible && visible_count == 1)
      flags |= MF_GRAYED;
    AppendMenuW(menu, flags,
                CMD_COLUMN_FIRST + static_cast<UINT>(definition.id),
                definition.title);
  }
}

void show_column_menu(App &a) {
  capture_column_settings(a);
  POINT point{};
  GetCursorPos(&point);
  HMENU menu = CreatePopupMenu();
  append_column_items(a, menu);
  SetForegroundWindow(a.window);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, a.window, nullptr);
  DestroyMenu(menu);
}

void toggle_column(App &a, esm::GuiColumnId id) {
  capture_column_settings(a);
  auto &setting = column_settings(a, id);
  const auto visible_count = std::count_if(
      a.settings.columns.begin(), a.settings.columns.end(),
      [](const esm::GuiColumnSettings &column) { return column.visible; });
  if (setting.visible && visible_count == 1)
    return;
  setting.visible = !setting.visible;
  rebuild_columns(a, false);
  save_settings(a, false);
}

void sort_by_column(App &a, int column) {
  if (column < 0 || column >= static_cast<int>(a.visible_columns.size()))
    return;
  const auto field =
      column_definition(a.visible_columns[static_cast<std::size_t>(column)])
          .sort;
  if (a.sort == field)
    a.descending = !a.descending;
  else {
    a.sort = field;
    a.descending = false;
  }
  update_sort_arrows(a, column);
  save_settings(a);
  a.last_query.clear();
  search(a);
}
void context(App &a, POINT p) {
  HMENU m = CreatePopupMenu();
  const auto selection = selected(a);
  const bool has_item = focused(a) != nullptr;
  const bool has_action_item = !selection.empty() || has_item;
  const UINT item_flags = has_item ? MF_STRING : MF_STRING | MF_GRAYED;
  const UINT selection_flags =
      has_action_item ? MF_STRING : MF_STRING | MF_GRAYED;
  const UINT paste_flags = has_item && clipboard_has_files()
                               ? MF_STRING
                               : MF_STRING | MF_GRAYED;
  AppendMenuW(m, item_flags, CMD_OPEN, L"打开");
  AppendMenuW(m, item_flags, CMD_LOCATION, L"\u6253\u5f00\u6240\u5728\u76ee\u5f55");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, selection_flags, CMD_CUT_FILE, L"剪切");
  AppendMenuW(m, selection_flags, CMD_COPY_FILE, L"复制");
  AppendMenuW(m, paste_flags, CMD_PASTE, L"粘贴");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, selection_flags, CMD_COPY_PATH, L"复制完整路径");
  AppendMenuW(m, selection_flags, CMD_COPY_NAME, L"复制名称");
  AppendMenuW(m, selection_flags, CMD_COPY_PARENT, L"\u590d\u5236\u6240\u5728\u8def\u5f84");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, selection_flags, CMD_COPY_TO_FOLDER, L"\u590d\u5236\u5230\u6587\u4ef6\u5939\u2026");
  AppendMenuW(m, selection_flags, CMD_MOVE_TO_FOLDER, L"\u79fb\u52a8\u5230\u6587\u4ef6\u5939\u2026");
  const bool can_rename =
      selection.size() == 1 && !a.visible_columns.empty() &&
      a.visible_columns.front() == esm::GuiColumnId::name;
  AppendMenuW(m, can_rename ? MF_STRING : MF_STRING | MF_GRAYED, CMD_RENAME,
              L"\u91cd\u547d\u540d");
  AppendMenuW(m, selection_flags, CMD_DELETE, L"删除到回收站");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, selection_flags, CMD_PROPERTIES, L"\u5c5e\u6027");
  AppendMenuW(m, MF_STRING | (a.show_preview ? MF_CHECKED : 0), CMD_PREVIEW,
              L"显示预览");
  TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, a.window, nullptr);
  DestroyMenu(m);
}
void show(App &a) {
  ShowWindow(a.window, SW_SHOW);
  if (IsIconic(a.window))
    ShowWindow(a.window, SW_RESTORE);
  SetForegroundWindow(a.window);
  focus_search(a);
}

int natural_compare_gui(std::wstring_view left, std::wstring_view right,
                        bool case_sensitive, bool match_diacritics) {
  DWORD flags = SORT_DIGITSASNUMBERS;
  if (!case_sensitive) flags |= NORM_IGNORECASE;
  if (!match_diacritics) flags |= NORM_IGNORENONSPACE;
  const int result = CompareStringEx(
      LOCALE_NAME_USER_DEFAULT, flags, left.data(), static_cast<int>(left.size()),
      right.data(), static_cast<int>(right.size()), nullptr, nullptr, 0);
  return result == CSTR_LESS_THAN ? -1 : result == CSTR_GREATER_THAN ? 1 : 0;
}

std::wstring_view extension_view(const esm::FileRecord &record) {
  const auto dot = record.name.find_last_of(L'.');
  return dot == std::wstring::npos ? std::wstring_view{}
                                   : std::wstring_view(record.name).substr(dot + 1);
}

RunHistoryEntry run_history_for(const App &a, const esm::FileRecord &record) {
  const auto found = a.run_history.find(run_history_key(record.path));
  return found == a.run_history.end() ? RunHistoryEntry{} : found->second;
}

void sort_loaded_results(App &a) {
  // Service results already arrive in the requested order. Re-sorting them on
  // the UI thread for every keystroke adds work and delays edit painting.
  if (!a.file_list_index && service_sort_field(a.sort) == a.sort)
    return;
  if (a.sort == esm::SortField::relevance)
    return;
  const auto compare_text = [&](std::wstring_view left, std::wstring_view right) {
    return natural_compare_gui(left, right, a.settings.case_sensitive,
                               a.settings.match_diacritics);
  };
  const auto compare = [&](const esm::SearchResult &left,
                           const esm::SearchResult &right) {
    int order = 0;
    switch (a.sort) {
    case esm::SortField::name:
      order = compare_text(left.record.name, right.record.name);
      break;
    case esm::SortField::path:
      order = compare_text(left.record.path, right.record.path);
      break;
    case esm::SortField::size:
      order = left.record.size < right.record.size ? -1
              : left.record.size > right.record.size ? 1 : 0;
      break;
    case esm::SortField::extension:
      order = compare_text(extension_view(left.record), extension_view(right.record));
      break;
    case esm::SortField::type:
      order = compare_text(type_text(left.record), type_text(right.record));
      break;
    case esm::SortField::last_write_time:
      order = left.record.last_write_time < right.record.last_write_time ? -1
              : left.record.last_write_time > right.record.last_write_time ? 1 : 0;
      break;
    case esm::SortField::creation_time:
      order = left.record.creation_time < right.record.creation_time ? -1
              : left.record.creation_time > right.record.creation_time ? 1 : 0;
      break;
    case esm::SortField::last_access_time:
      order = left.record.last_access_time < right.record.last_access_time ? -1
              : left.record.last_access_time > right.record.last_access_time ? 1 : 0;
      break;
    case esm::SortField::attributes:
      order = left.record.attributes < right.record.attributes ? -1
              : left.record.attributes > right.record.attributes ? 1 : 0;
      break;
    case esm::SortField::change_time:
      order = left.record.change_time < right.record.change_time ? -1
              : left.record.change_time > right.record.change_time ? 1 : 0;
      break;
    case esm::SortField::run_count: {
      const auto l = run_history_for(a, left.record).count;
      const auto r = run_history_for(a, right.record).count;
      order = l < r ? -1 : l > r ? 1 : 0;
      break;
    }
    case esm::SortField::last_open_time: {
      const auto l = run_history_for(a, left.record).last_open_time;
      const auto r = run_history_for(a, right.record).last_open_time;
      order = l < r ? -1 : l > r ? 1 : 0;
      break;
    }
    case esm::SortField::file_list_name: {
      const auto name = a.file_list_path.filename().wstring();
      order = compare_text(name, name);
      break;
    }
    case esm::SortField::relevance:
      break;
    }
    if (!order) order = compare_text(left.record.path, right.record.path);
    if (!order) order = left.record.id < right.record.id ? -1
                         : left.record.id > right.record.id ? 1 : 0;
    return a.descending ? order > 0 : order < 0;
  };
  std::stable_sort(a.results.begin(), a.results.end(), compare);
}

esm::SortField service_sort_field(esm::SortField field) {
  switch (field) {
  case esm::SortField::type:
    return esm::SortField::extension;
  case esm::SortField::creation_time:
  case esm::SortField::last_access_time:
  case esm::SortField::change_time:
  case esm::SortField::run_count:
  case esm::SortField::last_open_time:
  case esm::SortField::file_list_name:
    return esm::SortField::relevance;
  default:
    return field;
  }
}

UINT sort_command(esm::SortField field) {
  switch (field) {
  case esm::SortField::name: return CMD_SORT_NAME;
  case esm::SortField::path: return CMD_SORT_PATH;
  case esm::SortField::size: return CMD_SORT_SIZE;
  case esm::SortField::last_write_time: return CMD_SORT_MODIFIED;
  case esm::SortField::attributes: return CMD_SORT_ATTRIBUTES;
  case esm::SortField::extension: return CMD_SORT_EXTENSION;
  case esm::SortField::type: return CMD_SORT_TYPE;
  case esm::SortField::creation_time: return CMD_SORT_CREATED;
  case esm::SortField::last_access_time: return CMD_SORT_ACCESSED;
  case esm::SortField::change_time: return CMD_SORT_CHANGED;
  case esm::SortField::run_count: return CMD_SORT_RUN_COUNT;
  case esm::SortField::last_open_time: return CMD_SORT_LAST_OPEN;
  case esm::SortField::file_list_name: return CMD_SORT_FILE_LIST_NAME;
  default: return CMD_SORT_RELEVANCE;
  }
}
void set_sort(App &a, esm::SortField field) {
  if (a.sort != field) {
    a.sort = field;
    a.descending = false;
  }
  rebuild_columns(a, false);
  save_settings(a);
  a.last_query.clear();
  search(a);
}
void rebuild_filter_controls(App& a) {
  constexpr const wchar_t* builtins[] = {L"\u6240\u6709", L"\u97f3\u9891", L"\u538b\u7f29\u6587\u4ef6", L"\u6587\u6863", L"\u53ef\u6267\u884c\u6587\u4ef6", L"\u6587\u4ef6\u5939", L"\u56fe\u7247", L"\u89c6\u9891"};
  if (a.filter) {
    SendMessageW(a.filter, CB_RESETCONTENT, 0, 0);
    for (const auto* name : builtins) SendMessageW(a.filter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
    for (const auto& filter : a.custom_filters) SendMessageW(a.filter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(filter.name.c_str()));
  }
  if (a.search_menu) {
    while (GetMenuItemCount(a.search_menu) > 11) DeleteMenu(a.search_menu, 11, MF_BYPOSITION);
    for (UINT i=0;i<std::size(builtins);++i) AppendMenuW(a.search_menu, MF_STRING, CMD_FILTER_FIRST+i, builtins[i]);
    if (!a.custom_filters.empty()) AppendMenuW(a.search_menu, MF_SEPARATOR, 0, nullptr);
    for (std::size_t i=0;i<a.custom_filters.size() && i<100;++i)
      AppendMenuW(a.search_menu, MF_STRING, CMD_FILTER_FIRST+8+static_cast<UINT>(i), a.custom_filters[i].name.c_str());
  }
  const int maximum = 7 + static_cast<int>(a.custom_filters.size());
  a.settings.search_filter = std::clamp(a.settings.search_filter, 0, maximum);
  if (a.filter) SendMessageW(a.filter, CB_SETCURSEL, a.settings.search_filter, 0);
  DrawMenuBar(a.window);
}
void set_filter(App &a, int filter) {
  const int maximum = 7 + static_cast<int>(a.custom_filters.size());
  a.settings.search_filter = std::clamp(filter, 0, maximum);
  if (a.filter) SendMessageW(a.filter, CB_SETCURSEL, a.settings.search_filter, 0);
  if (a.settings.search_filter >= 8) {
    const auto& saved = a.custom_filters[static_cast<std::size_t>(a.settings.search_filter-8)];
    a.settings.case_sensitive=saved.case_sensitive; a.settings.whole_word=saved.whole_word;
    a.settings.match_path=saved.match_path; a.settings.match_diacritics=saved.match_diacritics;
    a.settings.regex_mode=saved.regex;
  }
  refresh_menu_state(a);
  save_settings(a, false);
  a.last_query.clear(); search(a);
}
void add_filter(App& a) {
  std::vector<esm::gui::FormField> fields{{L"\u7b5b\u9009\u5668\u540d\u79f0", search_text(a)}, {L"\u641c\u7d22\u6761\u4ef6", search_text(a), true}};
  std::vector<esm::gui::FormCheck> checks{{L"\u533a\u5206\u5927\u5c0f\u5199",a.settings.case_sensitive},{L"\u5168\u5b57\u5339\u914d",a.settings.whole_word},{L"\u5339\u914d\u8def\u5f84",a.settings.match_path},{L"\u5339\u914d\u53d8\u97f3\u6807\u8bb0",a.settings.match_diacritics},{L"\u6b63\u5219\u8868\u8fbe\u5f0f",a.settings.regex_mode}};
  if (!esm::gui::show_form(a.window,L"\u6dfb\u52a0\u5230\u7b5b\u9009\u5668",fields,checks,L"\u6dfb\u52a0") || fields[1].value.empty()) return;
  if(fields[0].value.empty())fields[0].value=fields[1].value;
  a.custom_filters.push_back({fields[0].value,fields[1].value,checks[0].checked,checks[1].checked,checks[2].checked,checks[3].checked,checks[4].checked});
  save_filters(a); rebuild_filter_controls(a); set_filter(a,7+static_cast<int>(a.custom_filters.size()));
}
void manage_filters(App& a) {
  std::vector<esm::gui::FormField> fields{{L"\u6bcf\u884c\u4e00\u4e2a\uff1a\u540d\u79f0<Tab>\u641c\u7d22\u6761\u4ef6",saved_search_lines(a.custom_filters),true}};
  std::vector<esm::gui::FormCheck> checks;
  if(!esm::gui::show_form(a.window,L"\u7ba1\u7406\u7b5b\u9009\u5668",fields,checks,L"\u4fdd\u5b58"))return;
  parse_saved_search_lines(fields[0].value,a.custom_filters); save_filters(a); rebuild_filter_controls(a); a.last_query.clear(); search(a);
}

void rebuild_bookmark_menu(App &a) {
  if (!a.bookmarks_menu)
    return;
  while (GetMenuItemCount(a.bookmarks_menu) > 0)
    DeleteMenu(a.bookmarks_menu, 0, MF_BYPOSITION);
  AppendMenuW(a.bookmarks_menu, MF_STRING, CMD_BOOKMARK_ADD,
              L"\u6dfb\u52a0\u5230\u4e66\u7b7e(&A)\u2026\tCtrl+D");
  AppendMenuW(a.bookmarks_menu, MF_STRING, CMD_BOOKMARK_MANAGE,
              L"\u7ba1\u7406\u4e66\u7b7e(&O)\u2026\tCtrl+Shift+B");
  if (!a.bookmarks.empty())
    AppendMenuW(a.bookmarks_menu, MF_SEPARATOR, 0, nullptr);
  for (std::size_t i = 0; i < a.bookmarks.size() && i < 100; ++i) {
    std::wstring title = a.bookmarks[i].name.empty() ? a.bookmarks[i].query : a.bookmarks[i].name;
    if (title.size() > 80)
      title = title.substr(0, 77) + L"\u2026";
    AppendMenuW(a.bookmarks_menu, MF_STRING,
                CMD_BOOKMARK_FIRST + static_cast<UINT>(i), title.c_str());
  }
}
void add_bookmark(App &a) {
  const auto query = search_text(a);
  if (query.empty()) { MessageBoxW(a.window,L"\u8bf7\u5148\u8f93\u5165\u8981\u4fdd\u5b58\u7684\u641c\u7d22\u3002",L"\u6dfb\u52a0\u5230\u4e66\u7b7e",MB_OK|MB_ICONINFORMATION); return; }
  std::vector<esm::gui::FormField> fields{{L"\u4e66\u7b7e\u540d\u79f0",query},{L"\u641c\u7d22",query,true}};
  std::vector<esm::gui::FormCheck> checks{{L"\u533a\u5206\u5927\u5c0f\u5199",a.settings.case_sensitive},{L"\u5168\u5b57\u5339\u914d",a.settings.whole_word},{L"\u5339\u914d\u8def\u5f84",a.settings.match_path},{L"\u5339\u914d\u53d8\u97f3\u6807\u8bb0",a.settings.match_diacritics},{L"\u6b63\u5219\u8868\u8fbe\u5f0f",a.settings.regex_mode}};
  if(!esm::gui::show_form(a.window,L"\u6dfb\u52a0\u5230\u4e66\u7b7e",fields,checks,L"\u6dfb\u52a0")||fields[1].value.empty())return;
  if(fields[0].value.empty())fields[0].value=fields[1].value;
  auto found=std::find_if(a.bookmarks.begin(),a.bookmarks.end(),[&](const Bookmark& b){return b.name==fields[0].value;});
  Bookmark value{fields[0].value,fields[1].value,checks[0].checked,checks[1].checked,checks[2].checked,checks[3].checked,checks[4].checked};
  if(found==a.bookmarks.end())a.bookmarks.push_back(std::move(value));else *found=std::move(value);
  save_bookmarks(a); rebuild_bookmark_menu(a); status(a,L"\u5df2\u6dfb\u52a0\u4e66\u7b7e: "+fields[0].value);
}
void remove_current_bookmark(App &a) {
  const auto query = search_text(a);
  const auto old_size = a.bookmarks.size();
  a.bookmarks.erase(std::remove_if(a.bookmarks.begin(), a.bookmarks.end(),
                                   [&](const Bookmark &bookmark) {
                                     return bookmark.query == query;
                                   }),
                    a.bookmarks.end());
  if (a.bookmarks.size() != old_size) {
    save_bookmarks(a);
    rebuild_bookmark_menu(a);
    status(a, L"\u5df2\u5220\u9664\u5f53\u524d\u67e5\u8be2\u4e66\u7b7e");
  }
}

void refresh_menu_state(App &a) {
  const bool has_focus = focused(a) != nullptr;
  const auto selection = selected(a);
  const bool has_selection = !selection.empty();
  const bool has_results = !a.results.empty();
  const bool search_focus = search_has_focus(a);
  const bool text_selected = search_focus && edit_has_selection(search_edit_control(a));
  const bool has_action_item = has_selection || focused(a) != nullptr;
  const bool can_paste_text = search_focus && IsClipboardFormatAvailable(CF_UNICODETEXT);
  const bool can_paste_files = !search_focus && focused(a) && clipboard_has_files();
  const bool can_rename = selection.size() == 1 && !a.visible_columns.empty() &&
                          a.visible_columns.front() == esm::GuiColumnId::name;
  const UINT focused_state = MF_BYCOMMAND | (has_focus ? MF_ENABLED : MF_GRAYED);
  EnableMenuItem(a.file_menu, CMD_EXPORT,
                 MF_BYCOMMAND | (has_results ? MF_ENABLED : MF_GRAYED));
  EnableMenuItem(a.file_menu, CMD_CLOSE_FILE_LIST,
                 MF_BYCOMMAND | (a.file_list_index ? MF_ENABLED : MF_GRAYED));
  for (const UINT command : {CMD_OPEN, CMD_LOCATION})
    EnableMenuItem(a.edit_menu, command, focused_state);
  EnableMenuItem(a.edit_menu, CMD_CUT_FILE,
                 MF_BYCOMMAND |
                     ((search_focus ? text_selected : has_action_item)
                          ? MF_ENABLED
                          : MF_GRAYED));
  EnableMenuItem(a.edit_menu, CMD_COPY_FILE,
                 MF_BYCOMMAND |
                     ((search_focus ? text_selected : has_action_item)
                          ? MF_ENABLED
                          : MF_GRAYED));
  for (const UINT command : {CMD_COPY_TO_FOLDER, CMD_MOVE_TO_FOLDER})
    EnableMenuItem(a.edit_menu, command,
                   MF_BYCOMMAND | (has_action_item ? MF_ENABLED : MF_GRAYED));
  for (const UINT command : {CMD_ADVANCED_COPY, CMD_ADVANCED_MOVE})
    EnableMenuItem(a.advanced_edit_menu, command,
                   MF_BYCOMMAND | (has_action_item ? MF_ENABLED : MF_GRAYED));
  EnableMenuItem(a.edit_menu, CMD_PASTE,
                 MF_BYCOMMAND |
                     ((can_paste_text || can_paste_files) ? MF_ENABLED
                                                         : MF_GRAYED));
  EnableMenuItem(a.edit_menu, CMD_RENAME,
                 MF_BYCOMMAND | (can_rename ? MF_ENABLED : MF_GRAYED));
  EnableMenuItem(a.edit_menu, CMD_SELECT_ALL,
                 MF_BYCOMMAND | ((has_results || search_focus) ? MF_ENABLED : MF_GRAYED));
  EnableMenuItem(a.edit_menu, CMD_INVERT_SELECTION,
                 MF_BYCOMMAND | (has_results ? MF_ENABLED : MF_GRAYED));

  CheckMenuItem(a.view_menu, CMD_FILTER_BAR,
                MF_BYCOMMAND |
                    (a.settings.show_filter ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(a.view_menu, CMD_PREVIEW,
                MF_BYCOMMAND | (a.show_preview ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(a.view_menu, CMD_STATUS_BAR,
                MF_BYCOMMAND |
                    (a.settings.show_status ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuRadioItem(a.window_size_menu, CMD_WINDOW_SMALL, CMD_WINDOW_AUTO,
                     CMD_WINDOW_SMALL + a.settings.window_size_mode,
                     MF_BYCOMMAND);
  CheckMenuItem(a.font_size_menu, CMD_FONT_NORMAL,
                MF_BYCOMMAND |
                    (a.settings.font_delta == 0 ? MF_CHECKED : MF_UNCHECKED));
  EnableMenuItem(a.font_size_menu, CMD_FONT_INCREASE,
                 MF_BYCOMMAND |
                     (a.settings.font_delta < 8 ? MF_ENABLED : MF_GRAYED));
  EnableMenuItem(a.font_size_menu, CMD_FONT_DECREASE,
                 MF_BYCOMMAND |
                     (a.settings.font_delta > -4 ? MF_ENABLED : MF_GRAYED));
  CheckMenuRadioItem(a.sort_menu, CMD_SORT_NAME, CMD_SORT_FILE_LIST_NAME,
                     sort_command(a.sort), MF_BYCOMMAND);
  CheckMenuRadioItem(a.sort_menu, CMD_SORT_ASCENDING, CMD_SORT_DESCENDING,
                     a.descending ? CMD_SORT_DESCENDING : CMD_SORT_ASCENDING,
                     MF_BYCOMMAND);
  CheckMenuRadioItem(a.topmost_menu, CMD_TOPMOST_NEVER, CMD_TOPMOST_SEARCHING,
                     CMD_TOPMOST_NEVER + a.settings.topmost_mode, MF_BYCOMMAND);
  CheckMenuRadioItem(a.view_menu, CMD_VIEW_EXTRA_LARGE, CMD_VIEW_DETAILS,
                     a.settings.view_mode == 3 ? CMD_VIEW_EXTRA_LARGE :
                     a.settings.view_mode == 2 ? CMD_VIEW_LARGE :
                     a.settings.view_mode == 1 ? CMD_VIEW_MEDIUM : CMD_VIEW_DETAILS,
                     MF_BYCOMMAND);

  CheckMenuItem(a.search_menu, CMD_MATCH_CASE,
                MF_BYCOMMAND |
                    (a.settings.case_sensitive ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(a.search_menu, CMD_MATCH_WHOLE_WORD,
                MF_BYCOMMAND |
                    (a.settings.whole_word ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(a.search_menu, CMD_MATCH_PATH,
                MF_BYCOMMAND |
                    (a.settings.match_path ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(a.search_menu, CMD_MATCH_DIACRITICS,
                MF_BYCOMMAND |
                    (a.settings.match_diacritics ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(a.search_menu, CMD_REGEX,
                MF_BYCOMMAND |
                    (a.settings.regex_mode ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuRadioItem(a.search_menu, CMD_FILTER_FIRST, CMD_FILTER_LAST,
                     CMD_FILTER_FIRST + a.settings.search_filter, MF_BYCOMMAND);

  const auto query = search_text(a);
  EnableMenuItem(a.bookmarks_menu, CMD_BOOKMARK_ADD,
                 MF_BYCOMMAND | (!query.empty() ? MF_ENABLED : MF_GRAYED));
  EnableMenuItem(a.tools_menu, CMD_CONNECT_SERVICE,
                 MF_BYCOMMAND | (!a.service_connected ? MF_ENABLED : MF_GRAYED));
  EnableMenuItem(a.tools_menu, CMD_DISCONNECT_SERVICE,
                 MF_BYCOMMAND | (a.service_connected ? MF_ENABLED : MF_GRAYED));
}

void create_main_menu(App &a) {
  a.main_menu = CreateMenu();
  a.file_menu = CreatePopupMenu();
  a.edit_menu = CreatePopupMenu();
  a.view_menu = CreatePopupMenu();
  a.search_menu = CreatePopupMenu();
  a.bookmarks_menu = CreatePopupMenu();
  a.tools_menu = CreatePopupMenu();
  a.help_menu = CreatePopupMenu();
  a.sort_menu = CreatePopupMenu();
  a.goto_menu = CreatePopupMenu();
  a.window_size_menu = CreatePopupMenu();
  a.font_size_menu = CreatePopupMenu();
  a.topmost_menu = CreatePopupMenu();

  AppendMenuW(a.file_menu, MF_STRING, CMD_NEW_WINDOW,
              L"新建搜索窗口(&N)\tCtrl+N");
  AppendMenuW(a.file_menu, MF_STRING, CMD_OPEN_FILE_LIST,
              L"打开文件列表(&O)…\tCtrl+O");
  AppendMenuW(a.file_menu, MF_STRING, CMD_CLOSE_FILE_LIST,
              L"关闭文件列表(&B)");
  AppendMenuW(a.file_menu, MF_STRING, CMD_CLOSE_WINDOW,
              L"关闭(&C)\tCtrl+W");
  AppendMenuW(a.file_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(a.file_menu, MF_STRING, CMD_EXPORT,
              L"导出(&E)…\tCtrl+S");
  AppendMenuW(a.file_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(a.file_menu, MF_STRING, CMD_EXIT, L"\u9000\u51fa(&X)\tCtrl+Q");

  a.advanced_edit_menu = CreatePopupMenu();
  AppendMenuW(a.edit_menu, MF_STRING, CMD_CUT_FILE, L"剪切(&T)\tCtrl+X");
  AppendMenuW(a.edit_menu, MF_STRING, CMD_COPY_FILE, L"复制(&C)\tCtrl+C");
  AppendMenuW(a.edit_menu, MF_STRING, CMD_PASTE, L"粘贴(&P)\tCtrl+V");
  AppendMenuW(a.edit_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(a.edit_menu, MF_STRING, CMD_COPY_TO_FOLDER,
              L"\u590d\u5236\u5230\u6587\u4ef6\u5939(&F)\u2026");
  AppendMenuW(a.edit_menu, MF_STRING, CMD_MOVE_TO_FOLDER,
              L"\u79fb\u52a8\u5230\u6587\u4ef6\u5939(&V)\u2026");
  AppendMenuW(a.edit_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(a.edit_menu, MF_STRING, CMD_SELECT_ALL, L"\u5168\u9009(&A)\tCtrl+A");
  AppendMenuW(a.edit_menu, MF_STRING, CMD_INVERT_SELECTION, L"\u53cd\u9009(&I)");
  AppendMenuW(a.edit_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(a.advanced_edit_menu, MF_STRING, CMD_ADVANCED_COPY, L"\u9ad8\u7ea7\u590d\u5236(&C)\u2026");
  AppendMenuW(a.advanced_edit_menu, MF_STRING, CMD_ADVANCED_MOVE, L"\u9ad8\u7ea7\u79fb\u52a8(&M)\u2026");
  AppendMenuW(a.edit_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(a.advanced_edit_menu),
              L"高级(&E)");

  AppendMenuW(a.view_menu, MF_STRING, CMD_FILTER_BAR, L"筛选器(&F)");
  AppendMenuW(a.view_menu, MF_STRING, CMD_PREVIEW, L"预览(&P)\tAlt+P");
  AppendMenuW(a.view_menu, MF_STRING, CMD_STATUS_BAR, L"状态栏(&B)");
  AppendMenuW(a.view_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(a.view_menu, MF_STRING, CMD_VIEW_EXTRA_LARGE,
              L"超大图标(&X)\tCtrl+Shift+1");
  AppendMenuW(a.view_menu, MF_STRING, CMD_VIEW_LARGE,
              L"\u5927\u56fe\u6807(&L)\tCtrl+Shift+2");
  AppendMenuW(a.view_menu, MF_STRING, CMD_VIEW_MEDIUM,
              L"中等图标(&M)\tCtrl+Shift+3");
  AppendMenuW(a.view_menu, MF_STRING, CMD_VIEW_DETAILS,
              L"详情(&D)\tCtrl+Shift+6");
  AppendMenuW(a.view_menu, MF_SEPARATOR, 0, nullptr);

  AppendMenuW(a.window_size_menu, MF_STRING, CMD_WINDOW_SMALL,
              L"\u5c0f(&S)\tAlt+1");
  AppendMenuW(a.window_size_menu, MF_STRING, CMD_WINDOW_MEDIUM,
              L"\u4e2d\u7b49(&N)\tAlt+2");
  AppendMenuW(a.window_size_menu, MF_STRING, CMD_WINDOW_LARGE,
              L"\u5927(&L)\tAlt+3");
  AppendMenuW(a.window_size_menu, MF_STRING, CMD_WINDOW_AUTO,
              L"自动(&A)\tAlt+4");
  AppendMenuW(a.view_menu, MF_POPUP,
              reinterpret_cast<UINT_PTR>(a.window_size_menu), L"窗口大小(&W)");

  AppendMenuW(a.font_size_menu, MF_STRING, CMD_FONT_INCREASE,
              L"增大(&I)\tCtrl+=");
  AppendMenuW(a.font_size_menu, MF_STRING, CMD_FONT_DECREASE,
              L"减小(&D)\tCtrl+-");
  AppendMenuW(a.font_size_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(a.font_size_menu, MF_STRING, CMD_FONT_NORMAL,
              L"正常(&N)\tCtrl+0");
  AppendMenuW(a.view_menu, MF_POPUP,
              reinterpret_cast<UINT_PTR>(a.font_size_menu), L"字号(&T)");
  AppendMenuW(a.view_menu, MF_SEPARATOR, 0, nullptr);

  AppendMenuW(a.sort_menu, MF_STRING, CMD_SORT_NAME, L"名称\tCtrl+1");
  AppendMenuW(a.sort_menu, MF_STRING, CMD_SORT_PATH, L"路径\tCtrl+2");
  AppendMenuW(a.sort_menu, MF_STRING, CMD_SORT_SIZE, L"大小\tCtrl+3");
  AppendMenuW(a.sort_menu, MF_STRING, CMD_SORT_EXTENSION, L"扩展名\tCtrl+4");
  AppendMenuW(a.sort_menu, MF_STRING, CMD_SORT_TYPE, L"类型\tCtrl+5");
  AppendMenuW(a.sort_menu, MF_STRING, CMD_SORT_MODIFIED, L"修改时间\tCtrl+6");
  AppendMenuW(a.sort_menu, MF_STRING, CMD_SORT_CREATED, L"创建时间\tCtrl+7");
  AppendMenuW(a.sort_menu, MF_STRING, CMD_SORT_ACCESSED, L"访问时间");
  AppendMenuW(a.sort_menu, MF_STRING, CMD_SORT_ATTRIBUTES, L"属性\tCtrl+8");
  AppendMenuW(a.sort_menu, MF_STRING, CMD_SORT_CHANGED, L"最近修改时间\tCtrl+9");
  AppendMenuW(a.sort_menu, MF_STRING, CMD_SORT_RUN_COUNT, L"运行次数");
  AppendMenuW(a.sort_menu, MF_STRING, CMD_SORT_LAST_OPEN, L"最近打开时间");
  AppendMenuW(a.sort_menu, MF_STRING, CMD_SORT_FILE_LIST_NAME, L"\u6587\u4ef6\u5217\u8868\u540d");
  AppendMenuW(a.sort_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(a.sort_menu, MF_STRING, CMD_SORT_ASCENDING, L"升序(&A)");
  AppendMenuW(a.sort_menu, MF_STRING, CMD_SORT_DESCENDING, L"降序(&D)");
  AppendMenuW(a.view_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(a.sort_menu),
              L"排序(&S)");
  AppendMenuW(a.goto_menu, MF_STRING, CMD_HOME, L"首页(&H)\tAlt+Home");
  AppendMenuW(a.view_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(a.goto_menu),
              L"前往(&G)");
  AppendMenuW(a.view_menu, MF_STRING, CMD_REFRESH, L"刷新(&R)\tF5");
  AppendMenuW(a.view_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(a.topmost_menu, MF_STRING, CMD_TOPMOST_NEVER, L"从不(&N)");
  AppendMenuW(a.topmost_menu, MF_STRING, CMD_TOPMOST_ALWAYS, L"总是(&A)");
  AppendMenuW(a.topmost_menu, MF_STRING, CMD_TOPMOST_SEARCHING, L"\u641c\u7d22\u65f6(&S)");
  AppendMenuW(a.view_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(a.topmost_menu),
              L"置顶(&T)");

  AppendMenuW(a.search_menu, MF_STRING, CMD_MATCH_CASE,
              L"\u533a\u5206\u5927\u5c0f\u5199(&C)\tCtrl+I");
  AppendMenuW(a.search_menu, MF_STRING, CMD_MATCH_WHOLE_WORD,
              L"全字匹配(&W)\tCtrl+B");
  AppendMenuW(a.search_menu, MF_STRING, CMD_MATCH_PATH,
              L"匹配路径(&P)\tCtrl+U");
  AppendMenuW(a.search_menu, MF_STRING, CMD_MATCH_DIACRITICS,
              L"匹配变音标记(&D)\tCtrl+M");
  AppendMenuW(a.search_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(a.search_menu, MF_STRING, CMD_REGEX,
              L"\u4f7f\u7528\u6b63\u5219\u8868\u8fbe\u5f0f(&R)\tCtrl+R");
  AppendMenuW(a.search_menu, MF_STRING, CMD_ADVANCED_SEARCH, L"\u9ad8\u7ea7\u641c\u7d22(&A)\u2026");
  AppendMenuW(a.search_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(a.search_menu, MF_STRING, CMD_ADD_FILTER, L"\u6dfb\u52a0\u5230\u7b5b\u9009\u5668(&A)\u2026");
  AppendMenuW(a.search_menu, MF_STRING, CMD_MANAGE_FILTERS,
              L"管理筛选器(&O)…\tCtrl+Shift+F");
  AppendMenuW(a.search_menu, MF_SEPARATOR, 0, nullptr);

  rebuild_bookmark_menu(a);

  AppendMenuW(a.tools_menu, MF_STRING, CMD_CONNECT_SERVICE,
              L"\u8fde\u63a5\u641c\u7d22\u670d\u52a1\u5668(&C)\u2026");
  AppendMenuW(a.tools_menu, MF_STRING, CMD_DISCONNECT_SERVICE,
              L"\u65ad\u5f00\u641c\u7d22\u670d\u52a1\u5668(&D)");
  AppendMenuW(a.tools_menu, MF_STRING, CMD_FILE_LIST_EDITOR,
              L"\u6587\u4ef6\u5217\u8868\u7f16\u8f91\u5668(&E)\u2026");
  AppendMenuW(a.tools_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(a.tools_menu, MF_STRING, CMD_OPTIONS, L"选项(&O)…\tCtrl+P");

  AppendMenuW(a.help_menu, MF_STRING, CMD_HELP_OVERVIEW, L"帮助(&H)\tF1");
  AppendMenuW(a.help_menu, MF_STRING, CMD_HELP_SYNTAX, L"搜索语法(&S)");
  AppendMenuW(a.help_menu, MF_STRING, CMD_HELP_REGEX, L"\u6b63\u5219\u8868\u8fbe\u5f0f\u8bed\u6cd5(&R)");
  AppendMenuW(a.help_menu, MF_STRING, CMD_HELP_COMMAND_LINE, L"命令行选项(&C)");
  AppendMenuW(a.help_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(a.help_menu, MF_STRING, CMD_HELP_WEBSITE, L"项目主页(&W)");
  AppendMenuW(a.help_menu, MF_STRING, CMD_HELP_CHECK_UPDATES, L"\u68c0\u67e5\u66f4\u65b0(&U)\u2026");
  AppendMenuW(a.help_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(a.help_menu, MF_STRING, CMD_ABOUT,
              L"关于 everything_sm(&A)\tCtrl+F1");

  AppendMenuW(a.main_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(a.file_menu),
              L"文件(&F)");
  AppendMenuW(a.main_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(a.edit_menu),
              L"编辑(&E)");
  AppendMenuW(a.main_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(a.view_menu),
              L"视图(&V)");
  AppendMenuW(a.main_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(a.search_menu),
              L"搜索(&S)");
  AppendMenuW(a.main_menu, MF_POPUP,
              reinterpret_cast<UINT_PTR>(a.bookmarks_menu), L"书签(&B)");
  AppendMenuW(a.main_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(a.tools_menu),
              L"工具(&T)");
  AppendMenuW(a.main_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(a.help_menu),
              L"帮助(&H)");
  SetMenu(a.window, a.main_menu);
  rebuild_filter_controls(a);
  refresh_menu_state(a);
}

LRESULT CALLBACK search_subclass(HWND window, UINT message, WPARAM wp,
                                 LPARAM lp, UINT_PTR, DWORD_PTR parent) {
  auto *a = app(reinterpret_cast<HWND>(parent));
  if (message == WM_KEYDOWN && a) {
    if (wp == VK_ESCAPE) {
      if (GetWindowTextLengthW(window) > 0)
        SetWindowTextW(window, L"");
      return 0;
    }
    if (wp == VK_DOWN && !a->results.empty()) {
      focus_first_result(*a);
      return 0;
    }
    if (wp == VK_RETURN && !a->results.empty()) {
      const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
      focus_first_result(*a);
      PostMessageW(a->window, WM_COMMAND,
                   control ? CMD_LOCATION : CMD_OPEN, 0);
      return 0;
    }
  }
  const auto result = DefSubclassProc(window, message, wp, lp);
  if (a && (message == WM_SETFOCUS || message == WM_KILLFOCUS))
    apply_topmost(*a);
  switch (message) {
  case WM_SETTEXT:
  case WM_CHAR:
  case WM_PASTE:
  case WM_CUT:
  case WM_CLEAR:
    PostMessageW(reinterpret_cast<HWND>(parent), WM_ESM_QUERY_CHANGED, 0, 0);
    break;
  default:
    break;
  }
  return result;
}

LRESULT CALLBACK list_subclass(HWND window, UINT message, WPARAM wp, LPARAM lp,
                               UINT_PTR, DWORD_PTR parent) {
  auto *a = app(reinterpret_cast<HWND>(parent));
  if (message == WM_KEYDOWN && a) {
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (wp == VK_RETURN) {
      PostMessageW(a->window, WM_COMMAND, control ? CMD_LOCATION : CMD_OPEN, 0);
      return 0;
    }
    if (wp == VK_F2) {
      PostMessageW(a->window, WM_COMMAND, CMD_RENAME, 0);
      return 0;
    }
    if (wp == VK_DELETE) {
      PostMessageW(a->window, WM_COMMAND, CMD_DELETE, 0);
      return 0;
    }
    if (control && wp == 'C') {
      PostMessageW(a->window, WM_COMMAND,
                   shift ? CMD_COPY_PATH : CMD_COPY_FILE, 0);
      return 0;
    }
    if (control && wp == 'A') {
      select_all_results(*a);
      return 0;
    }
  }
  const auto result = DefSubclassProc(window, message, wp, lp);
  if (a && message == WM_SETFOCUS)
    apply_topmost(*a);
  return result;
}

LRESULT CALLBACK header_subclass(HWND window, UINT message, WPARAM wp,
                                  LPARAM lp, UINT_PTR, DWORD_PTR parent) {
  if (message == WM_CONTEXTMENU) {
    PostMessageW(reinterpret_cast<HWND>(parent), WM_ESM_COLUMN_MENU, 0, 0);
    return 0;
  }
  return DefSubclassProc(window, message, wp, lp);
}

void controls(App &a) {
  a.search = CreateWindowExW(
      0, WC_COMBOBOXW, nullptr,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN |
          CBS_AUTOHSCROLL,
      0, 0, 0, 360, a.window, reinterpret_cast<HMENU>(SEARCH_ID), nullptr,
      nullptr);
  COMBOBOXINFO combo_info{sizeof(combo_info)};
  if (GetComboBoxInfo(a.search, &combo_info))
    a.search_edit = combo_info.hwndItem;
  SetWindowSubclass(a.search_edit ? a.search_edit : a.search, search_subclass, 1,
                    reinterpret_cast<DWORD_PTR>(a.window));
  for (const auto &query : a.history)
    SendMessageW(a.search, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(query.c_str()));
  a.filter = CreateWindowExW(
      0, WC_COMBOBOXW, nullptr,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
      0, 0, 0, 240, a.window, reinterpret_cast<HMENU>(FILTER_ID), nullptr,
      nullptr);
  constexpr const wchar_t *filter_names[] = {
      L"\u6240\u6709", L"\u97f3\u9891", L"\u538b\u7f29\u6587\u4ef6", L"\u6587\u6863", L"\u53ef\u6267\u884c\u6587\u4ef6", L"\u6587\u4ef6\u5939", L"\u56fe\u7247", L"\u89c6\u9891"};
  for (const auto *name : filter_names)
    SendMessageW(a.filter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
  SendMessageW(a.filter, CB_SETCURSEL, a.settings.search_filter, 0);
  a.list =
      CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
                          LVS_OWNERDATA | LVS_SHOWSELALWAYS | LVS_EDITLABELS,
                      0, 0, 0, 0, a.window, (HMENU)LIST_ID, nullptr, nullptr);
  a.preview = CreateWindowExW(
      WS_EX_CLIENTEDGE, WC_EDITW, nullptr,
      WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
          WS_VSCROLL | WS_HSCROLL,
      0, 0, 0, 0, a.window, (HMENU)PREVIEW_ID, nullptr, nullptr);
  a.status = CreateWindowExW(0, STATUSCLASSNAMEW, L"\u8bf7\u8f93\u5165\u67e5\u8be2",
                             WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
                             a.window, (HMENU)STATUS_ID, nullptr, nullptr);
  SendMessageW(a.search, CB_SETCUEBANNER, 0,
               reinterpret_cast<LPARAM>(L"搜索文件和文件夹"));
  if (a.search_edit)
    SendMessageW(a.search_edit, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"搜索文件和文件夹"));
  SetWindowSubclass(a.list, list_subclass, 1,
                    reinterpret_cast<DWORD_PTR>(a.window));
  ListView_SetExtendedListViewStyle(
      a.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP |
                  LVS_EX_HEADERDRAGDROP);
  rebuild_columns(a, false);
  SetWindowSubclass(ListView_GetHeader(a.list), header_subclass, 1,
                    reinterpret_cast<DWORD_PTR>(a.window));
  SHFILEINFOW info{};
  a.small_images = (HIMAGELIST)SHGetFileInfoW(
      L"C:\\", FILE_ATTRIBUTE_DIRECTORY, &info, sizeof(info),
      SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
  a.large_images = (HIMAGELIST)SHGetFileInfoW(
      L"C:\\", FILE_ATTRIBUTE_DIRECTORY, &info, sizeof(info),
      SHGFI_SYSICONINDEX | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES);
  if (a.small_images) ListView_SetImageList(a.list, a.small_images, LVSIL_SMALL);
  if (a.large_images) ListView_SetImageList(a.list, a.large_images, LVSIL_NORMAL);
  apply_view_mode(a);
  apply_font(a);
}
LRESULT notify(App &a, NMHDR *h) {
  if (h->hwndFrom == ListView_GetHeader(a.list)) {
    switch (h->code) {
    case HDN_ENDTRACKA:
    case HDN_ENDTRACKW:
    case HDN_ENDDRAG:
      PostMessageW(a.window, WM_ESM_SAVE_SETTINGS, 0, 0);
      break;
    default:
      break;
    }
    return 0;
  }
  if (h->hwndFrom != a.list)
    return 0;
  switch (h->code) {
  case LVN_GETDISPINFOW: {
    auto d = (NMLVDISPINFOW *)h;
    auto i = (std::size_t)d->item.iItem;
    if (i >= a.results.size())
      return 0;
    auto &r = a.results[i].record;
    static thread_local std::wstring value;
    if (d->item.mask & LVIF_IMAGE)
      d->item.iImage = icon(a, r);
    if (!(d->item.mask & LVIF_TEXT))
      return 0;
    if (d->item.iSubItem < 0 ||
        d->item.iSubItem >= static_cast<int>(a.visible_columns.size())) {
      value.clear();
    } else {
      switch (a.visible_columns[static_cast<std::size_t>(d->item.iSubItem)]) {
      case esm::GuiColumnId::name:
        value = r.name;
        break;
      case esm::GuiColumnId::path:
        value = std::filesystem::path(r.path).parent_path().wstring();
        break;
      case esm::GuiColumnId::size:
        value = size_text(r);
        break;
      case esm::GuiColumnId::last_write_time:
        value = time_text(r.last_write_time);
        break;
      case esm::GuiColumnId::type:
        value = type_text(r);
        break;
      }
    }
    d->item.pszText = value.data();
    return 0;
  }
  case LVN_ITEMCHANGED:
    preview(a);
    return 0;
  case LVN_COLUMNCLICK: {
    auto c = (NMLISTVIEW *)h;
    sort_by_column(a, c->iSubItem);
    return 0;
  }
  case NM_DBLCLK:
    if (auto r = focused(a))
      open(a, *r);
    return 0;
  case NM_RCLICK: {
    POINT p{};
    GetCursorPos(&p);
    context(a, p);
    return 0;
  }
  case LVN_BEGINDRAG:
    drag_files(a);
    return 0;
  case LVN_ENDLABELEDITW: {
    auto d = (NMLVDISPINFOW *)h;
    if (!d->item.pszText)
      return FALSE;
    auto i = (std::size_t)d->item.iItem;
    if (i >= a.results.size())
      return FALSE;
    auto &r = a.results[i].record;
    auto dest = std::filesystem::path(r.path).parent_path() / d->item.pszText;
    if (!MoveFileExW(r.path.c_str(), dest.c_str(), 0)) {
      MessageBoxW(a.window, win_error(GetLastError()).c_str(), L"\u91cd\u547d\u540d\u5931\u8d25",
                  MB_ICONERROR);
      return FALSE;
    }
    r.name = d->item.pszText;
    r.path = dest.wstring();
    return TRUE;
  }
  default:
    return 0;
  }
}
void open_directory(const std::filesystem::path &path) {
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
void manage_bookmarks(App &a) {
  std::vector<esm::gui::FormField> fields{{L"\u6bcf\u884c\u4e00\u4e2a\uff1a\u540d\u79f0<Tab>\u641c\u7d22",saved_search_lines(a.bookmarks),true}};
  std::vector<esm::gui::FormCheck> checks;
  if(!esm::gui::show_form(a.window,L"\u7ba1\u7406\u4e66\u7b7e",fields,checks,L"\u4fdd\u5b58"))return;
  parse_saved_search_lines(fields[0].value,a.bookmarks); save_bookmarks(a); rebuild_bookmark_menu(a);
}
void show_search_syntax(App &a) {
  MessageBoxW(
      a.window,
      L"everything_sm 搜索语法\n\n"
      L"普通文本：report\n"
      L"短语：\"annual report\"\n"
      L"字段：name:、path:、ext:、file:、folder:\n"
      L"\u5e03\u5c14\uff1aAND\u3001OR\u3001NOT\u3001\u62ec\u53f7\n"
      L"大小：size:>10mb\n"
      L"时间：dm:>=2026-01-01\n"
      L"属性：attrib:hidden\n"
      L"正则：regex:\"^report.*\\.pdf$\"\n\n"
      L"\u641c\u7d22\u83dc\u5355\u4e2d\u7684\u9009\u9879\u53ef\u5207\u6362\u5927\u5c0f\u5199\u3001\u5168\u5b57\u3001\u8def\u5f84\u548c\u6b63\u5219\u6a21\u5f0f\u3002",
      L"搜索语法", MB_OK | MB_ICONINFORMATION);
}
void show_shortcuts(App &a) {
  MessageBoxW(
      a.window,
      L"Ctrl+N  新建窗口        Ctrl+W  关闭窗口\n"
      L"Ctrl+S  导出结果        Ctrl+Q  退出\n"
      L"Enter   打开            Ctrl+Enter  打开所在目录\n"
      L"Ctrl+C/X 复制/剪切      Ctrl+Shift+C  复制完整路径\n"
      L"F2 \u91cd\u547d\u540d  Del \u5220\u9664  Alt+Enter \u5c5e\u6027\n"
      L"Alt+P 预览             F5 刷新\n"
      L"Ctrl+I/B/U/R \u5207\u6362\u5927\u5c0f\u5199/\u5168\u5b57/\u8def\u5f84/\u6b63\u5219\n"
      L"Ctrl+1/2/3/4/6/8 排序\n"
      L"Ctrl+D 添加书签        Ctrl+Shift+B 管理书签\n"
      L"Ctrl+Alt+Space 全局显示窗口",
      L"\u5feb\u6377\u952e", MB_OK | MB_ICONINFORMATION);
}

bool search_service_available(std::wstring_view pipe, DWORD timeout_ms,
                              DWORD& error) {
  const auto normalized = esm::normalize_pipe_name(pipe);
  if (WaitNamedPipeW(normalized.c_str(), timeout_ms)) {
    error = ERROR_SUCCESS;
    return true;
  }
  error = GetLastError();
  // ERROR_SEM_TIMEOUT means the pipe exists but every instance is busy. The
  // service is still alive, so do not report a transient load spike as a
  // disconnected service.
  if (error == ERROR_SEM_TIMEOUT || error == ERROR_PIPE_BUSY) {
    error = ERROR_SUCCESS;
    return true;
  }
  return false;
}

void connect_search_service(App& a) {
  std::vector<esm::gui::FormField> fields{
      {L"\u641c\u7d22\u670d\u52a1 Named Pipe \u540d\u79f0", a.pipe}};
  std::vector<esm::gui::FormCheck> checks;
  if (!esm::gui::show_form(a.window, L"\u8fde\u63a5\u641c\u7d22\u670d\u52a1\u5668",
                           fields, checks, L"\u8fde\u63a5"))
    return;
  if (fields[0].value.empty()) {
    MessageBoxW(a.window, L"Pipe \u540d\u79f0\u4e0d\u80fd\u4e3a\u7a7a\u3002",
                L"\u8fde\u63a5\u641c\u7d22\u670d\u52a1\u5668",
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  DWORD connection_error = ERROR_SUCCESS;
  if (!search_service_available(fields[0].value, 1500, connection_error)) {
    a.service_connected = false;
    MessageBoxW(a.window,
                (L"\u65e0\u6cd5\u8fde\u63a5\u641c\u7d22\u670d\u52a1: " + win_error(connection_error)).c_str(),
                L"\u8fde\u63a5\u641c\u7d22\u670d\u52a1", MB_OK | MB_ICONERROR);
    return;
  }
  a.pipe = fields[0].value;
  a.local_pipe = a.pipe;
  write_service_pipe(a);
  a.file_list_index.reset();
  a.file_list_path.clear();
  SetWindowTextW(a.window, L"everything_sm");
  a.service_connected = true;
  a.last_query.clear();
  status(a, L"\u5df2\u8fde\u63a5\u641c\u7d22\u670d\u52a1");
  search(a);
}

void disconnect_search_service(App& a) {
  a.service_connected = false;
  ++a.generation;
  a.results.clear();
  ListView_SetItemCountEx(a.list, 0, 0);
  status(a, L"\u5df2\u65ad\u5f00\u641c\u7d22\u670d\u52a1");
}

void reconnect_search_service(App& a) {
  const auto pipe = a.local_pipe.empty() ? L"everything_sm_service" : a.local_pipe;
  DWORD connection_error = ERROR_SUCCESS;
  if (!search_service_available(pipe, 1500, connection_error)) {
    a.service_connected = false;
    status(a, L"\u670d\u52a1\u4e0d\u53ef\u7528: " + win_error(connection_error));
    return;
  }
  a.pipe = pipe;
  a.service_connected = true;
  a.last_query.clear();
  status(a, L"\u5df2\u91cd\u65b0\u8fde\u63a5\u641c\u7d22\u670d\u52a1");
  search(a);
}

bool parse_result_limit(const std::wstring& value, int& result) {
  if (value.empty()) return false;
  wchar_t* end = nullptr;
  const long parsed = wcstol(value.c_str(), &end, 10);
  if (!end || *end != L'\0' || parsed < 10 ||
      parsed > static_cast<long>(esm::ipc_max_search_results))
    return false;
  result = static_cast<int>(parsed);
  return true;
}

void show_options(App& a) {
  std::vector<esm::gui::FormField> fields{
      {L"\u6700\u5927\u663e\u793a\u7ed3\u679c\u6570\uff0810-1000\uff09",
       std::to_wstring(a.settings.result_limit)},
      {L"\u9ed8\u8ba4\u641c\u7d22\u670d\u52a1 Named Pipe", a.local_pipe}};
  std::vector<esm::gui::FormCheck> checks{
      {L"\u663e\u793a\u7b5b\u9009\u5668\u680f", a.settings.show_filter},
      {L"\u663e\u793a\u72b6\u6001\u680f", a.settings.show_status},
      {L"\u663e\u793a\u9884\u89c8\u9762\u677f", a.show_preview},
      {L"\u533a\u5206\u5927\u5c0f\u5199", a.settings.case_sensitive},
      {L"\u5168\u5b57\u5339\u914d", a.settings.whole_word},
      {L"\u5339\u914d\u8def\u5f84", a.settings.match_path},
      {L"\u5339\u914d\u53d8\u97f3\u6807\u8bb0", a.settings.match_diacritics},
      {L"\u4f7f\u7528\u6b63\u5219\u8868\u8fbe\u5f0f", a.settings.regex_mode}};
  if (!esm::gui::show_form(a.window, L"\u9009\u9879", fields, checks,
                           L"\u5e94\u7528"))
    return;
  int result_limit = 0;
  if (!parse_result_limit(fields[0].value, result_limit)) {
    MessageBoxW(a.window, L"\u7ed3\u679c\u6570\u5fc5\u987b\u662f 10 \u5230 1000 \u4e4b\u95f4\u7684\u6574\u6570\u3002",
                L"\u9009\u9879", MB_OK | MB_ICONWARNING);
    return;
  }
  if (fields[1].value.empty()) {
    MessageBoxW(a.window, L"Pipe \u540d\u79f0\u4e0d\u80fd\u4e3a\u7a7a\u3002",
                L"\u9009\u9879", MB_OK | MB_ICONWARNING);
    return;
  }
  a.settings.result_limit = result_limit;
  a.settings.show_filter = checks[0].checked;
  a.settings.show_status = checks[1].checked;
  a.show_preview = checks[2].checked;
  a.settings.case_sensitive = checks[3].checked;
  a.settings.whole_word = checks[4].checked;
  a.settings.match_path = checks[5].checked;
  a.settings.match_diacritics = checks[6].checked;
  a.settings.regex_mode = checks[7].checked;
  const bool pipe_changed = a.local_pipe != fields[1].value;
  a.local_pipe = fields[1].value;
  if (pipe_changed) {
    a.pipe = a.local_pipe;
    DWORD connection_error = ERROR_SUCCESS;
    a.service_connected =
        search_service_available(a.pipe, 300, connection_error);
    a.last_query.clear();
  }
  write_service_pipe(a);
  esm::normalize_gui_settings(a.settings);
  layout(a);
  apply_view_mode(a);
  save_settings(a, false);
  a.last_query.clear();
  search(a);
}

void show_command_line_help(App& a) {
  MessageBoxW(
      a.window,
      L"everything_sm \u547d\u4ee4\u884c\u5de5\u5177\n\n"
      L"esm_cli scan <\u76ee\u5f55> <\u67e5\u8be2>\n"
      L"esm_cli query <Pipe\u540d> <\u67e5\u8be2>\n"
      L"esm_cli mft <\u5377> <\u67e5\u8be2>\n"
      L"esm_cli journal <\u5377> [checkpoint]\n\n"
      L"esm_gui.exe [Pipe\u540d]\n"
      L"esm_service.exe status|start|stop|install-mft-auto|uninstall\n\n"
      L"\u66f4\u5b8c\u6574\u7684\u53c2\u6570\u8bf7\u67e5\u770b\u5b89\u88c5\u76ee\u5f55\u4e2d\u7684 README.md\u3002",
      L"\u547d\u4ee4\u884c\u9009\u9879", MB_OK | MB_ICONINFORMATION);
}

void open_readme(App& a) {
  const auto readme = executable_path().parent_path() / L"README.md";
  if (!std::filesystem::exists(readme)) {
    MessageBoxW(a.window, L"\u672a\u627e\u5230 README.md\u3002", L"everything_sm",
                MB_OK | MB_ICONWARNING);
    return;
  }
  ShellExecuteW(a.window, L"open", readme.c_str(), nullptr, nullptr,
                SW_SHOWNORMAL);
}

LRESULT CALLBACK proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
  App *a = app(w);
  switch (m) {
  case WM_NCCREATE: {
    a = (App *)((CREATESTRUCTW *)lp)->lpCreateParams;
    a->window = w;
    SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)a);
    return TRUE;
  }
  case WM_CREATE:
    controls(*a);
    create_main_menu(*a);
    a->tray.cbSize = sizeof(a->tray);
    a->tray.hWnd = w;
    a->tray.uID = 1;
    a->tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    a->tray.uCallbackMessage = WM_ESM_TRAY;
    a->tray.hIcon = load_app_icon(a->instance, true);
    wcscpy_s(a->tray.szTip, L"everything_sm 搜索");
    Shell_NotifyIconW(NIM_ADD, &a->tray);
    RegisterHotKey(w, HOTKEY_ID, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,
                   VK_SPACE);
    SetTimer(w, SEARCH_TIMER, SEARCH_TIMER_INTERVAL_MS, nullptr);
    layout(*a);
    apply_topmost(*a);
    return 0;
  case WM_SIZE:
    if (a)
      layout(*a);
    return 0;
  case WM_ACTIVATE:
    if (a)
      PostMessageW(w, WM_ESM_APPLY_TOPMOST, 0, 0);
    break;
  case WM_TIMER:
    if (a && wp == SEARCH_TIMER) {
      const auto now = GetTickCount64();
      if (a->query_pending && now >= a->query_due_tick)
        search(*a, a->query_pending_final_results);
      if (a->refinement_pending && now >= a->refinement_due_tick &&
          !a->query_pending && !a->service_query_in_flight) {
        a->refinement_pending = false;
        search(*a, true);
      }
      if (a->metadata_pending && now >= a->metadata_due_tick)
        start_metadata_hydration(*a);
      if (a->history_pending && now >= a->history_due_tick &&
          !a->query_pending && !a->service_query_in_flight &&
          search_text(*a) == a->pending_history_query) {
        auto query = std::move(a->pending_history_query);
        a->history_pending = false;
        remember(*a, query);
      }
    }
    return 0;
  case WM_HOTKEY:
    if (a)
      show(*a);
    return 0;
  case WM_ESM_QUERY_CHANGED:
    if (a)
      schedule_search(*a);
    return 0;
  case WM_ESM_COLUMN_MENU:
    if (a)
      show_column_menu(*a);
    return 0;
  case WM_ESM_SAVE_SETTINGS:
    if (a)
      save_settings(*a);
    return 0;
  case WM_ESM_APPLY_TOPMOST:
    if (a)
      apply_topmost(*a);
    return 0;
  case WM_INITMENUPOPUP:
    if (a) {
      if (reinterpret_cast<HMENU>(wp) == a->bookmarks_menu) {
        load_bookmarks(*a);
        rebuild_bookmark_menu(*a);
      }
      refresh_menu_state(*a);
    }
    return 0;
  case WM_COMMAND: {
    if (!a)
      return 0;
    if (LOWORD(wp) == SEARCH_ID &&
        (HIWORD(wp) == CBN_EDITCHANGE || HIWORD(wp) == CBN_SELENDOK)) {
      schedule_search(*a);
      return 0;
    }
    if (LOWORD(wp) == FILTER_ID && HIWORD(wp) == CBN_SELENDOK) {
      const int selected_filter = static_cast<int>(
          SendMessageW(a->filter, CB_GETCURSEL, 0, 0));
      set_filter(*a, selected_filter < 0 ? 0 : selected_filter);
      return 0;
    }
    const UINT command_id = LOWORD(wp);
    if (command_id >= CMD_COLUMN_FIRST && command_id <= CMD_COLUMN_LAST) {
      toggle_column(
          *a, static_cast<esm::GuiColumnId>(command_id - CMD_COLUMN_FIRST));
      return 0;
    }
    if (command_id >= CMD_FILTER_FIRST && command_id <= CMD_FILTER_LAST) {
      set_filter(*a, static_cast<int>(command_id - CMD_FILTER_FIRST));
      return 0;
    }
    if (command_id >= CMD_BOOKMARK_FIRST && command_id <= CMD_BOOKMARK_LAST) {
      const auto index = static_cast<std::size_t>(command_id - CMD_BOOKMARK_FIRST);
      if (index < a->bookmarks.size()) {
        const auto& bookmark = a->bookmarks[index];
        a->settings.case_sensitive = bookmark.case_sensitive;
        a->settings.whole_word = bookmark.whole_word;
        a->settings.match_path = bookmark.match_path;
        a->settings.match_diacritics = bookmark.match_diacritics;
        a->settings.regex_mode = bookmark.regex;
        SetWindowTextW(a->search, bookmark.query.c_str());
        save_settings(*a, false);
        focus_search(*a);
        a->last_query.clear();
        search(*a);
      }
      return 0;
    }
    const bool search_focused = search_has_focus(*a);
    switch (command_id) {
    case CMD_NEW_WINDOW:
      launch_new_window(*a);
      break;
    case CMD_OPEN_FILE_LIST:
      open_file_list(*a);
      break;
    case CMD_CLOSE_FILE_LIST:
      close_file_list(*a);
      break;
    case CMD_CLOSE_WINDOW:
      save_settings(*a, true, true);
      ShowWindow(a->window, SW_HIDE);
      break;
    case CMD_EXPORT:
      export_results(*a);
      break;
    case CMD_OPEN:
      if (auto r = focused(*a))
        open(*a, *r);
      break;
    case CMD_LOCATION:
      if (auto r = focused(*a))
        location(*a, *r);
      break;
    case CMD_CUT_FILE:
      if (search_focused)
        SendMessageW(search_edit_control(*a), WM_CUT, 0, 0);
      else
        cut_files(*a);
      break;
    case CMD_COPY_FILE:
      if (search_focused)
        SendMessageW(search_edit_control(*a), WM_COPY, 0, 0);
      else
        copy_files(*a);
      break;
    case CMD_PASTE:
      if (search_focused)
        SendMessageW(search_edit_control(*a), WM_PASTE, 0, 0);
      else
        paste_clipboard_files(*a);
      break;
    case CMD_COPY_TO_FOLDER:
      transfer_selected(*a, false, false);
      break;
    case CMD_MOVE_TO_FOLDER:
      transfer_selected(*a, true, false);
      break;
    case CMD_ADVANCED_COPY:
      transfer_selected(*a, false, true);
      break;
    case CMD_ADVANCED_MOVE:
      transfer_selected(*a, true, true);
      break;
    case CMD_COPY_PATH: {
      std::wstring value;
      for (const auto &path : selected_paths(*a)) {
        if (!value.empty())
          value += L"\r\n";
        value += path;
      }
      clipboard_text(value);
      break;
    }
    case CMD_COPY_NAME:
      copy_selected_names(*a);
      break;
    case CMD_COPY_PARENT:
      copy_selected_parents(*a);
      break;
    case CMD_SELECT_ALL:
      if (search_focused)
        SendMessageW(search_edit_control(*a), EM_SETSEL, 0, -1);
      else
        select_all_results(*a);
      break;
    case CMD_INVERT_SELECTION:
      invert_selection(*a);
      break;
    case CMD_CLEAR_SELECTION:
      clear_selection(*a);
      break;
    case CMD_RENAME: {
      const auto ids = selected(*a);
      if (ids.size() == 1 && !a->visible_columns.empty() &&
          a->visible_columns.front() == esm::GuiColumnId::name)
        ListView_EditLabel(a->list, static_cast<int>(ids.front()));
      break;
    }
    case CMD_DELETE:
      delete_files(*a);
      break;
    case CMD_PROPERTIES:
      show_properties(*a);
      break;
    case CMD_PREVIEW:
      a->show_preview = !a->show_preview;
      layout(*a);
      save_settings(*a);
      break;
    case CMD_STATUS_BAR:
      a->settings.show_status = !a->settings.show_status;
      layout(*a);
      save_settings(*a, false);
      break;
    case CMD_FILTER_BAR:
      a->settings.show_filter = !a->settings.show_filter;
      layout(*a);
      save_settings(*a, false);
      break;
    case CMD_VIEW_EXTRA_LARGE:
      set_view_mode(*a, 3);
      break;
    case CMD_VIEW_LARGE:
      set_view_mode(*a, 2);
      break;
    case CMD_VIEW_MEDIUM:
      set_view_mode(*a, 1);
      break;
    case CMD_VIEW_DETAILS:
      set_view_mode(*a, 0);
      break;
    case CMD_WINDOW_SMALL:
    case CMD_WINDOW_MEDIUM:
    case CMD_WINDOW_LARGE:
    case CMD_WINDOW_AUTO:
      resize_window(*a, static_cast<int>(command_id - CMD_WINDOW_SMALL));
      save_settings(*a, true, true);
      break;
    case CMD_FONT_INCREASE:
      a->settings.font_delta = std::min(8, a->settings.font_delta + 1);
      apply_font(*a);
      save_settings(*a, false);
      break;
    case CMD_FONT_DECREASE:
      a->settings.font_delta = std::max(-4, a->settings.font_delta - 1);
      apply_font(*a);
      save_settings(*a, false);
      break;
    case CMD_FONT_NORMAL:
      a->settings.font_delta = 0;
      apply_font(*a);
      save_settings(*a, false);
      break;
    case CMD_SORT_RELEVANCE:
      set_sort(*a, esm::SortField::relevance);
      break;
    case CMD_SORT_NAME:
      set_sort(*a, esm::SortField::name);
      break;
    case CMD_SORT_PATH:
      set_sort(*a, esm::SortField::path);
      break;
    case CMD_SORT_SIZE:
      set_sort(*a, esm::SortField::size);
      break;
    case CMD_SORT_EXTENSION:
      set_sort(*a, esm::SortField::extension);
      break;
    case CMD_SORT_TYPE:
      set_sort(*a, esm::SortField::type);
      break;
    case CMD_SORT_MODIFIED:
      set_sort(*a, esm::SortField::last_write_time);
      break;
    case CMD_SORT_CREATED:
      set_sort(*a, esm::SortField::creation_time);
      break;
    case CMD_SORT_ACCESSED:
      set_sort(*a, esm::SortField::last_access_time);
      break;
    case CMD_SORT_ATTRIBUTES:
      set_sort(*a, esm::SortField::attributes);
      break;
    case CMD_SORT_CHANGED:
      set_sort(*a, esm::SortField::change_time);
      break;
    case CMD_SORT_RUN_COUNT:
      set_sort(*a, esm::SortField::run_count);
      break;
    case CMD_SORT_LAST_OPEN:
      set_sort(*a, esm::SortField::last_open_time);
      break;
    case CMD_SORT_FILE_LIST_NAME:
      set_sort(*a, esm::SortField::file_list_name);
      break;
    case CMD_SORT_ASCENDING:
    case CMD_SORT_DESCENDING:
      a->descending = command_id == CMD_SORT_DESCENDING;
      rebuild_columns(*a, false);
      save_settings(*a);
      a->last_query.clear();
      search(*a);
      break;
    case CMD_HOME:
      SetWindowTextW(a->search, L"");
      set_filter(*a, 0);
      focus_search(*a);
      break;
    case CMD_REFRESH:
      a->last_query.clear();
      search(*a);
      break;
    case CMD_SERVICE_RECONNECT:
      reconnect_search_service(*a);
      break;
    case CMD_TOPMOST_NEVER:
    case CMD_TOPMOST_ALWAYS:
    case CMD_TOPMOST_SEARCHING:
      a->settings.topmost_mode =
          static_cast<int>(command_id - CMD_TOPMOST_NEVER);
      apply_topmost(*a);
      save_settings(*a, false);
      break;
    case CMD_FOCUS_SEARCH:
      focus_search(*a);
      break;
    case CMD_CLEAR_SEARCH:
      SetWindowTextW(a->search, L"");
      focus_search(*a);
      break;
    case CMD_MATCH_CASE:
      a->settings.case_sensitive = !a->settings.case_sensitive;
      save_settings(*a, false);
      a->last_query.clear();
      search(*a);
      break;
    case CMD_MATCH_WHOLE_WORD:
      a->settings.whole_word = !a->settings.whole_word;
      save_settings(*a, false);
      a->last_query.clear();
      search(*a);
      break;
    case CMD_MATCH_PATH:
      a->settings.match_path = !a->settings.match_path;
      save_settings(*a, false);
      a->last_query.clear();
      search(*a);
      break;
    case CMD_MATCH_DIACRITICS:
      a->settings.match_diacritics = !a->settings.match_diacritics;
      save_settings(*a, false);
      a->last_query.clear();
      search(*a);
      break;
    case CMD_REGEX:
      a->settings.regex_mode = !a->settings.regex_mode;
      save_settings(*a, false);
      a->last_query.clear();
      search(*a);
      break;
    case CMD_ADVANCED_SEARCH:
      advanced_search(*a);
      break;
    case CMD_ADD_FILTER:
      add_filter(*a);
      break;
    case CMD_MANAGE_FILTERS:
      manage_filters(*a);
      break;
    case CMD_HELP_OVERVIEW:
      open_readme(*a);
      break;
    case CMD_HELP_SYNTAX:
      show_search_syntax(*a);
      break;
    case CMD_BOOKMARK_ADD:
      add_bookmark(*a);
      break;
    case CMD_BOOKMARK_REMOVE_CURRENT:
      remove_current_bookmark(*a);
      break;
    case CMD_BOOKMARK_MANAGE:
      manage_bookmarks(*a);
      break;
    case CMD_CONNECT_SERVICE:
      connect_search_service(*a);
      break;
    case CMD_DISCONNECT_SERVICE:
      disconnect_search_service(*a);
      break;
    case CMD_FILE_LIST_EDITOR:
      create_file_list(*a);
      break;
    case CMD_OPEN_DATA_DIR: {
      wchar_t data[MAX_PATH]{};
      if (GetEnvironmentVariableW(L"ProgramData", data,
                                  static_cast<DWORD>(std::size(data))))
        open_directory(std::filesystem::path(data) / L"everything_sm" / L"indexes");
      break;
    }
    case CMD_OPEN_LOG_DIR: {
      wchar_t data[MAX_PATH]{};
      if (GetEnvironmentVariableW(L"ProgramData", data,
                                  static_cast<DWORD>(std::size(data))))
        open_directory(std::filesystem::path(data) / L"everything_sm" / L"logs");
      break;
    }
    case CMD_OPTIONS:
      show_options(*a);
      break;
    case CMD_HELP_REGEX:
      MessageBoxW(a->window,
                  L"\u542f\u7528\u201c\u641c\u7d22 > \u4f7f\u7528\u6b63\u5219\u8868\u8fbe\u5f0f\u201d\u540e\uff0c\u641c\u7d22\u6846\u5185\u5bb9\u4f1a\u4f5c\u4e3a\u5b8c\u6574\u6b63\u5219\u8868\u8fbe\u5f0f\u5904\u7406\u3002\n\n"
                  L"示例：^report.*\\.pdf$\n"
                  L"也可以直接使用语法：regex:\"表达式\"",
                  L"\u6b63\u5219\u8868\u8fbe\u5f0f\u8bed\u6cd5", MB_OK | MB_ICONINFORMATION);
      break;
    case CMD_HELP_SHORTCUTS:
      show_shortcuts(*a);
      break;
    case CMD_HELP_README:
      open_readme(*a);
      break;
    case CMD_HELP_COMMAND_LINE:
      show_command_line_help(*a);
      break;
    case CMD_HELP_WEBSITE:
      open_readme(*a);
      break;
    case CMD_HELP_CHECK_UPDATES:
      MessageBoxW(
          a->window,
          L"\u5f53\u524d\u5f00\u53d1\u7248\u5c1a\u672a\u914d\u7f6e\u5728\u7ebf\u66f4\u65b0\u6e90\u3002\u8bf7\u4f7f\u7528\u65b0\u7684 NSIS \u5b89\u88c5\u5305\u8986\u76d6\u5b89\u88c5\u3002",
          L"\u68c0\u67e5\u66f4\u65b0", MB_OK | MB_ICONINFORMATION);
      break;
    case CMD_ABOUT:
      MessageBoxW(a->window,
                  L"everything_sm 0.1.0\n\n"
                  L"Windows 全局文件名与路径搜索工具\n"
                  L"支持多卷 NTFS 索引、实时搜索、托盘、预览、书签和持久化设置。\n\n"
                  L"Clean-room implementation\uff0c\u4e0d\u5305\u542b Everything \u6e90\u4ee3\u7801\u3002",
                  L"关于 everything_sm", MB_OK | MB_ICONINFORMATION);
      break;
    case CMD_SHOW:
      show(*a);
      break;
    case CMD_EXIT:
      save_settings(*a, true, true);
      a->closing = true;
      DestroyWindow(w);
      break;
    }
    refresh_menu_state(*a);
    return 0;
  }
  case WM_NOTIFY:
    return a ? notify(*a, (NMHDR *)lp) : 0;
  case WM_ESM_RESULTS: {
    std::unique_ptr<SearchPayload> p((SearchPayload *)lp);
    if (a && !a->closing) {
      const bool superseded = a->query_pending;
      a->service_query_in_flight = false;
      if (a->service_query_thread) {
        CloseHandle(a->service_query_thread);
        a->service_query_thread = nullptr;
      }
      if (!superseded)
        apply_results(*a, *p);
      if (a->query_pending && GetTickCount64() >= a->query_due_tick)
        search(*a, a->query_pending_final_results);
    }
    return 0;
  }
  case WM_ESM_METADATA: {
    std::unique_ptr<MetadataPayload> p((MetadataPayload *)lp);
    if (a) {
      a->metadata_in_flight = false;
      if (a->metadata_thread) {
        CloseHandle(a->metadata_thread);
        a->metadata_thread = nullptr;
      }
      a->metadata_cancel.reset();
      if (!a->closing && p->generation == a->generation.load() &&
          !a->query_pending && !a->service_query_in_flight) {
        if (esm::apply_result_metadata_updates(a->results, p->updates) != 0) {
          sort_loaded_results(*a);
          InvalidateRect(a->list, nullptr, FALSE);
        }
      }
      if (a->metadata_pending &&
          GetTickCount64() >= a->metadata_due_tick) {
        start_metadata_hydration(*a);
      }
    }
    return 0;
  }
  case WM_ESM_TRAY:
    if (!a)
      return 0;
    if (LOWORD(lp) == WM_LBUTTONDBLCLK)
      show(*a);
    else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
      POINT p{};
      GetCursorPos(&p);
      HMENU menu = CreatePopupMenu();
      AppendMenuW(menu, MF_STRING, CMD_SHOW, L"显示搜索窗口");
      AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      AppendMenuW(menu, MF_STRING, CMD_EXIT, L"\u9000\u51fa");
      SetForegroundWindow(w);
      TrackPopupMenu(menu, TPM_RIGHTBUTTON, p.x, p.y, 0, w, nullptr);
      DestroyMenu(menu);
    }
    return 0;
  case WM_CLOSE:
    if (a && !a->closing) {
      save_settings(*a, true, true);
      ShowWindow(w, SW_HIDE);
      return 0;
    }
    break;
  case WM_DESTROY:
    if (a) {
      save_settings(*a, true, true);
      a->closing = true;
      ++a->generation;
      if (a->metadata_cancel) a->metadata_cancel->store(true);
      cancel_service_query(*a);
      if (a->service_query_thread) {
        CloseHandle(a->service_query_thread);
        a->service_query_thread = nullptr;
      }
      if (a->metadata_thread) {
        CloseHandle(a->metadata_thread);
        a->metadata_thread = nullptr;
      }
      KillTimer(w, SEARCH_TIMER);
      UnregisterHotKey(w, HOTKEY_ID);
      Shell_NotifyIconW(NIM_DELETE, &a->tray);
      if (a->ui_font) {
        DeleteObject(a->ui_font);
        a->ui_font = nullptr;
      }
    }
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(w, m, wp, lp);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command,
                    int show_mode) {
  HRESULT ole = OleInitialize(nullptr);
  INITCOMMONCONTROLSEX cc{sizeof(cc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES |
                                          ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&cc);
  App a;
  a.instance = instance;
  if (command && *command)
    a.pipe = command;
  load_history(a);
  load_settings(a);
  load_bookmarks(a);
  load_run_history(a);
  WNDCLASSEXW wc{sizeof(wc)};
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = proc;
  wc.hInstance = instance;
  wc.hIcon = load_app_icon(instance, false);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.lpszClassName = L"EsmSearchWindow";
  wc.hIconSm = load_app_icon(instance, true);
  if (!RegisterClassExW(&wc))
    return 1;
  int window_x = CW_USEDEFAULT;
  int window_y = CW_USEDEFAULT;
  int window_width = 1180;
  int window_height = 720;
  if (a.settings.window_valid) {
    RECT saved{a.settings.window_x, a.settings.window_y,
               a.settings.window_x + a.settings.window_width,
               a.settings.window_y + a.settings.window_height};
    if (MonitorFromRect(&saved, MONITOR_DEFAULTTONULL)) {
      window_x = a.settings.window_x;
      window_y = a.settings.window_y;
      window_width = a.settings.window_width;
      window_height = a.settings.window_height;
    }
  }
  HWND w = CreateWindowExW(
      0, wc.lpszClassName, L"everything_sm",
      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, window_x, window_y, window_width,
      window_height, nullptr, nullptr, instance, &a);
  if (!w)
    return 1;
  DWORD initial_connection_error = ERROR_SUCCESS;
  a.service_connected =
      search_service_available(a.pipe, 300, initial_connection_error);
  refresh_menu_state(a);
  if (a.settings.maximized && show_mode != SW_SHOWMINIMIZED &&
      show_mode != SW_MINIMIZE && show_mode != SW_SHOWMINNOACTIVE)
    show_mode = SW_SHOWMAXIMIZED;
  const ACCEL accelerator_entries[] = {
      {FVIRTKEY | FCONTROL, 'N', CMD_NEW_WINDOW},
      {FVIRTKEY | FCONTROL, 'O', CMD_OPEN_FILE_LIST},
      {FVIRTKEY | FCONTROL, 'W', CMD_CLOSE_WINDOW},
      {FVIRTKEY | FCONTROL, 'S', CMD_EXPORT},
      {FVIRTKEY | FCONTROL, 'Q', CMD_EXIT},
      {FVIRTKEY | FCONTROL, 'L', CMD_FOCUS_SEARCH},
      {FVIRTKEY, VK_F3, CMD_FOCUS_SEARCH},
      {FVIRTKEY, VK_F5, CMD_REFRESH},
      {FVIRTKEY, VK_F1, CMD_HELP_OVERVIEW},
      {FVIRTKEY | FCONTROL, VK_F1, CMD_ABOUT},
      {FVIRTKEY | FALT, 'P', CMD_PREVIEW},
      {FVIRTKEY | FALT, VK_RETURN, CMD_PROPERTIES},
      {FVIRTKEY | FALT, VK_HOME, CMD_HOME},
      {FVIRTKEY | FCONTROL, 'I', CMD_MATCH_CASE},
      {FVIRTKEY | FCONTROL, 'B', CMD_MATCH_WHOLE_WORD},
      {FVIRTKEY | FCONTROL, 'U', CMD_MATCH_PATH},
      {FVIRTKEY | FCONTROL, 'M', CMD_MATCH_DIACRITICS},
      {FVIRTKEY | FCONTROL, 'R', CMD_REGEX},
      {FVIRTKEY | FCONTROL, 'D', CMD_BOOKMARK_ADD},
      {FVIRTKEY | FCONTROL | FSHIFT, 'B', CMD_BOOKMARK_MANAGE},
      {FVIRTKEY | FCONTROL | FSHIFT, 'F', CMD_MANAGE_FILTERS},
      {FVIRTKEY | FCONTROL, 'P', CMD_OPTIONS},
      {FVIRTKEY | FCONTROL, 'X', CMD_CUT_FILE},
      {FVIRTKEY | FCONTROL, 'C', CMD_COPY_FILE},
      {FVIRTKEY | FCONTROL, 'V', CMD_PASTE},
      {FVIRTKEY | FCONTROL | FSHIFT, 'C', CMD_COPY_PATH},
      {FVIRTKEY | FCONTROL, 'A', CMD_SELECT_ALL},
      {FVIRTKEY | FCONTROL, '1', CMD_SORT_NAME},
      {FVIRTKEY | FCONTROL, '2', CMD_SORT_PATH},
      {FVIRTKEY | FCONTROL, '3', CMD_SORT_SIZE},
      {FVIRTKEY | FCONTROL, '4', CMD_SORT_EXTENSION},
      {FVIRTKEY | FCONTROL, '5', CMD_SORT_TYPE},
      {FVIRTKEY | FCONTROL, '6', CMD_SORT_MODIFIED},
      {FVIRTKEY | FCONTROL, '7', CMD_SORT_CREATED},
      {FVIRTKEY | FCONTROL, '8', CMD_SORT_ATTRIBUTES},
      {FVIRTKEY | FCONTROL, '9', CMD_SORT_CHANGED},
      {FVIRTKEY | FCONTROL, VK_OEM_PLUS, CMD_FONT_INCREASE},
      {FVIRTKEY | FCONTROL, VK_OEM_MINUS, CMD_FONT_DECREASE},
      {FVIRTKEY | FCONTROL, '0', CMD_FONT_NORMAL},
      {FVIRTKEY | FCONTROL | FSHIFT, '1', CMD_VIEW_EXTRA_LARGE},
      {FVIRTKEY | FCONTROL | FSHIFT, '2', CMD_VIEW_LARGE},
      {FVIRTKEY | FCONTROL | FSHIFT, '3', CMD_VIEW_MEDIUM},
      {FVIRTKEY | FCONTROL | FSHIFT, '6', CMD_VIEW_DETAILS},
      {FVIRTKEY | FALT, '1', CMD_WINDOW_SMALL},
      {FVIRTKEY | FALT, '2', CMD_WINDOW_MEDIUM},
      {FVIRTKEY | FALT, '3', CMD_WINDOW_LARGE},
      {FVIRTKEY | FALT, '4', CMD_WINDOW_AUTO},
  };
  a.accelerators = CreateAcceleratorTableW(
      const_cast<LPACCEL>(accelerator_entries),
      static_cast<int>(std::size(accelerator_entries)));
  ShowWindow(w, show_mode);
  UpdateWindow(w);
  focus_search(a);
  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!a.accelerators || !TranslateAcceleratorW(w, a.accelerators, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  if (a.accelerators)
    DestroyAcceleratorTable(a.accelerators);
  if (SUCCEEDED(ole))
    OleUninitialize();
  return (int)msg.wParam;
}



