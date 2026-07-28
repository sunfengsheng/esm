#include "esm/directory_scanner.hpp"
#include "esm/file_metadata.hpp"
#include "esm/file_list.hpp"
#include "esm/saved_search.hpp"
#include "esm/directory_watcher.hpp"
#include "esm/gui_settings.hpp"
#include "esm/index.hpp"
#include "esm/ipc_protocol.hpp"
#include "esm/named_pipe.hpp"
#include "esm/journal_checkpoint.hpp"
#include "esm/journal_replay.hpp"
#include "esm/metadata_snapshot.hpp"
#include "esm/metadata_wal.hpp"
#include "esm/ntfs_catalog.hpp"
#include "esm/volume_discovery.hpp"
#include <windows.h>
#include <winioctl.h>
#include "esm/query.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
esm::FileRecord record(std::uint64_t id, std::wstring name, std::wstring path, bool directory = false) {
    esm::FileRecord value;
    value.id = id;
    value.name = std::move(name);
    value.path = std::move(path);
    value.directory = directory;
    return value;
}

SYSTEMTIME local_system_time(std::uint64_t value) {
    FILETIME utc{};
    utc.dwLowDateTime = static_cast<DWORD>(value);
    utc.dwHighDateTime = static_cast<DWORD>(value >> 32U);
    FILETIME local{};
    SYSTEMTIME result{};
    require(FileTimeToLocalFileTime(&utc, &local) != FALSE &&
                FileTimeToSystemTime(&local, &result) != FALSE,
            "FILETIME should convert to local SYSTEMTIME");
    return result;
}

std::uint64_t current_file_time() {
    FILETIME value{};
    GetSystemTimeAsFileTime(&value);
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U) |
        value.dwLowDateTime;
}
void test_multi_volume_namespacing() {
    const auto c_id = esm::namespace_ntfs_file_id(
        L"\\\\?\\Volume{11111111-1111-1111-1111-111111111111}\\", 42);
    const auto c_id_again = esm::namespace_ntfs_file_id(
        L"\\\\?\\Volume{11111111-1111-1111-1111-111111111111}\\", 42);
    const auto d_id = esm::namespace_ntfs_file_id(
        L"\\\\?\\Volume{22222222-2222-2222-2222-222222222222}\\", 42);
    require(c_id == c_id_again,
            "volume-scoped file IDs should be deterministic");
    require(c_id != d_id,
            "same MFT file ID on different volumes should not collide");

    std::vector<esm::FileRecord> records;
    auto child = record(42, L"child.txt", L"C:\\child.txt");
    child.parent_id = 7;
    records.push_back(child);
    esm::namespace_ntfs_records(L"volume-c", records);
    require(records.front().id ==
                esm::namespace_ntfs_file_id(L"volume-c", 42),
            "record ID should be volume-scoped");
    require(records.front().parent_id ==
                esm::namespace_ntfs_file_id(L"volume-c", 7),
            "parent record ID should be volume-scoped");

    std::vector<std::uint64_t> removed_ids{42, 7};
    esm::namespace_ntfs_file_ids(L"volume-c", removed_ids);
    require(removed_ids[0] ==
                esm::namespace_ntfs_file_id(L"volume-c", 42) &&
                removed_ids[1] ==
                esm::namespace_ntfs_file_id(L"volume-c", 7),
            "removed IDs should use the same volume namespace as upserts");

    std::vector<std::uint64_t> other_volume_ids{42};
    esm::namespace_ntfs_file_ids(L"volume-d", other_volume_ids);
    require(removed_ids[0] != other_volume_ids[0],
            "delta removals from different volumes should not collide");
}

void test_ntfs_volume_discovery() {
    const auto discovery = esm::discover_mounted_ntfs_volumes();
    require(discovery.error == ERROR_SUCCESS,
            "mounted NTFS volume discovery should succeed");
    for (std::size_t index = 0; index < discovery.volumes.size(); ++index) {
        const auto& volume = discovery.volumes[index];
        require(!volume.root.empty() && !volume.mount_path.empty() &&
                    !volume.identity.empty(),
                "discovered NTFS volume metadata should be complete");
        if (index != 0)
            require(_wcsicmp(discovery.volumes[index - 1].root.c_str(),
                             volume.root.c_str()) < 0,
                    "discovered NTFS volumes should be sorted");
    }
}

void test_multi_volume_snapshot_round_trip() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("esm-multi-snapshot-" + std::to_string(suffix));
    const auto path = root / "mft-index.snapshot";
    std::filesystem::create_directories(root);

    esm::MetadataSnapshot snapshot;
    snapshot.root_id = 1;
    snapshot.volume = L"everything_sm-mft-multi-v1";
    snapshot.records.push_back(record(1, L"c.txt", L"C:\\c.txt"));
    snapshot.records.push_back(record(2, L"d.txt", L"D:\\d.txt"));
    const auto saved = esm::save_metadata_snapshot_atomic(path, snapshot);
    require(saved.ok, "multi-volume snapshot should save atomically");
    const auto loaded = esm::load_metadata_snapshot(path);
    require(loaded.ok &&
                loaded.snapshot.volume == L"everything_sm-mft-multi-v1" &&
                loaded.snapshot.records.size() == 2 &&
                loaded.snapshot.records[1].path == L"D:\\d.txt",
            "multi-volume snapshot should round-trip");
    require(loaded.snapshot.volume != L"everything_sm-mft-multi-v2",
            "snapshot marker mismatch should be detectable");
    std::filesystem::remove_all(root);
}

void test_gui_settings() {
    const auto defaults = esm::default_gui_settings();
    require(!defaults.case_sensitive && !defaults.whole_word &&
                !defaults.match_path,
            "default GUI search switches");
    esm::GuiSettings parsed;
    require(esm::parse_gui_settings(esm::serialize_gui_settings(defaults),
                                    parsed),
            "default GUI settings round trip parses");
    for (std::size_t i = 0; i < esm::gui_column_count; ++i) {
        require(parsed.columns[i].id == defaults.columns[i].id,
                "default GUI column ID round trip");
        require(parsed.columns[i].width == defaults.columns[i].width,
                "default GUI column width round trip");
        require(parsed.columns[i].order == defaults.columns[i].order,
                "default GUI column order round trip");
        require(parsed.columns[i].visible == defaults.columns[i].visible,
                "default GUI column visibility round trip");
    }

    auto custom = defaults;
    custom.columns[0] = {esm::GuiColumnId::name, 333, 4, true};
    custom.columns[1] = {esm::GuiColumnId::path, 777, 1, false};
    custom.columns[2] = {esm::GuiColumnId::size, 120, 0, true};
    custom.columns[3] = {esm::GuiColumnId::last_write_time, 180, 2, true};
    custom.columns[4] = {esm::GuiColumnId::type, 140, 3, true};
    custom.sort_field = 4;
    custom.descending = true;
    custom.show_preview = false;
    custom.show_status = false;
    custom.show_filter = false;
    custom.topmost_mode = 2;
    custom.font_delta = 3;
    custom.window_size_mode = 1;
    custom.search_filter = 6;
    custom.regex_mode = true;
    custom.case_sensitive = true;
    custom.whole_word = true;
    custom.match_path = false;
    custom.match_diacritics = true;
    custom.view_mode = 3;
    custom.result_limit = 4321;
    custom.window_valid = true;
    custom.window_x = -1200;
    custom.window_y = 45;
    custom.window_width = 1440;
    custom.window_height = 900;
    custom.maximized = true;
    require(esm::parse_gui_settings(esm::serialize_gui_settings(custom),
                                    parsed),
            "custom GUI settings round trip parses");
    require(parsed.columns[0].width == 333 && parsed.columns[0].order == 4,
            "custom GUI name column round trip");
    require(!parsed.columns[1].visible && parsed.columns[1].width == 777,
            "custom hidden GUI column round trip");
    require(parsed.sort_field == 4 && parsed.descending &&
                !parsed.show_preview && !parsed.show_status &&
                !parsed.show_filter && parsed.topmost_mode == 2 &&
                parsed.font_delta == 3 && parsed.window_size_mode == 1 &&
                parsed.search_filter == 6 && parsed.regex_mode &&
                parsed.case_sensitive && parsed.whole_word && !parsed.match_path &&
                parsed.match_diacritics && parsed.view_mode == 3 &&
                parsed.result_limit == 4321,
            "custom GUI search settings round trip");
    require(parsed.window_valid && parsed.window_x == -1200 &&
                parsed.window_y == 45 && parsed.window_width == 1440 &&
                parsed.window_height == 900 && parsed.maximized,
            "custom GUI window placement round trip");

    require(!esm::parse_gui_settings("esm_gui_settings=6\n", parsed),
            "unsupported GUI settings version rejected");
    require(esm::parse_gui_settings(
                "esm_gui_settings=1\nmatch_path=1\n", parsed) &&
                !parsed.case_sensitive && !parsed.whole_word &&
                !parsed.match_path,
            "version-1 path matching default migrates to fast filename mode");
    const std::string malformed =
        "esm_gui_settings=1\n"
        "unknown_key=is_ignored\n"
        "sort_field=99\n"
        "window_width=10\n"
        "window_height=99999\n"
        "column=0,1,4,0\n"
        "column=1,5000,4,0\n"
        "column=2,100,4,0\n"
        "column=3,100,4,0\n"
        "column=4,100,4,0\n";
    require(esm::parse_gui_settings(malformed, parsed),
            "clamped GUI settings parse");
    require(parsed.sort_field == 13 && parsed.window_width == 480 &&
                parsed.window_height == 8192,
            "GUI scalar settings clamp");
    require(parsed.columns[0].visible,
            "GUI settings retain at least one visible column");
    require(parsed.columns[0].width == 32 && parsed.columns[1].width == 4096,
            "GUI column widths clamp");
    std::array<bool, esm::gui_column_count> orders{};
    for (const auto& column : parsed.columns) {
        require(column.order >= 0 &&
                    column.order < static_cast<int>(esm::gui_column_count),
                "normalized GUI column order range");
        require(!orders[static_cast<std::size_t>(column.order)],
                "normalized GUI column orders are unique");
        orders[static_cast<std::size_t>(column.order)] = true;
    }

    auto duplicate_ids = defaults;
    duplicate_ids.columns[1].id = esm::GuiColumnId::name;
    duplicate_ids.columns[2].id = esm::GuiColumnId::path;
    duplicate_ids.columns[3].id = esm::GuiColumnId::size;
    duplicate_ids.columns[4].id = esm::GuiColumnId::last_write_time;
    esm::normalize_gui_settings(duplicate_ids);
    std::array<bool, esm::gui_column_count> ids{};
    for (const auto& column : duplicate_ids.columns) {
        const auto id = static_cast<std::size_t>(column.id);
        require(id < esm::gui_column_count && !ids[id],
                "normalized GUI column IDs are unique");
        ids[id] = true;
    }
}
void test_query_parser() {
    const auto query = esm::parse_query(L"name:\"annual report\" ext:.pdf !draft file:");
    require(query.valid, "query should be valid");
    require(query.terms.size() == 3, "query term count");
    require(query.terms[0].target == esm::MatchTarget::name, "name target");
    require(query.terms[0].value == L"annual report", "quoted value");
    require(query.terms[1].target == esm::MatchTarget::extension, "extension target");
    require(query.terms[1].value == L"pdf", "extension normalization");
    require(query.terms[2].excluded, "excluded term");
    require(query.directories_only.has_value() && !*query.directories_only, "file filter");
    require(!esm::parse_query(L"\"unterminated").valid, "unterminated quote should fail");
}

void test_everything_date_constants() {
    constexpr std::uint64_t ticks_per_second = 10000000ULL;
    constexpr std::uint64_t ticks_per_day = 86400ULL * ticks_per_second;

    const auto last_week = esm::parse_query(L"dm:lastweek");
    const auto past_week = esm::parse_query(L"dm:pastweek");
    require(last_week.valid && last_week.terms.size() == 1 &&
                last_week.terms[0].has_lower_bound &&
                last_week.terms[0].has_upper_bound,
            "lastweek is the previous complete calendar week");
    require(past_week.valid && past_week.terms.size() == 1 &&
                past_week.terms[0].has_lower_bound &&
                !past_week.terms[0].has_upper_bound,
            "pastweek is a rolling seven-day lower threshold");

    const auto next_week = esm::parse_query(L"dm:nextweek");
    require(next_week.valid && next_week.terms[0].has_lower_bound &&
                next_week.terms[0].has_upper_bound,
            "nextweek date constant parses");
    const auto next_week_start =
        local_system_time(next_week.terms[0].lower_bound);
    require(next_week_start.wHour == 0 && next_week_start.wMinute == 0 &&
                next_week_start.wSecond == 0,
            "nextweek starts at local midnight");
    const auto next_week_ticks = next_week.terms[0].upper_bound -
        next_week.terms[0].lower_bound;
    require(next_week_ticks >= 167ULL * 60 * 60 * ticks_per_second &&
                next_week_ticks <= 169ULL * 60 * 60 * ticks_per_second,
            "nextweek spans one local calendar week including DST changes");

    const auto next_month = esm::parse_query(L"dm:comingmonth");
    require(next_month.valid && next_month.terms[0].has_lower_bound &&
                next_month.terms[0].has_upper_bound &&
                local_system_time(next_month.terms[0].lower_bound).wDay == 1 &&
                local_system_time(next_month.terms[0].upper_bound).wDay == 1,
            "comingmonth covers the next complete calendar month");
    const auto next_year = esm::parse_query(L"dm:nextyear");
    const auto next_year_start =
        local_system_time(next_year.terms[0].lower_bound);
    require(next_year.valid && next_year_start.wMonth == 1 &&
                next_year_start.wDay == 1,
            "nextyear starts on January 1 of the next year");

    const auto before_last_hours = current_file_time();
    const auto last_hours = esm::parse_query(L"dm:last24hours");
    const auto after_last_hours = current_file_time();
    require(last_hours.valid && last_hours.terms[0].has_lower_bound &&
                !last_hours.terms[0].has_upper_bound &&
                last_hours.terms[0].lower_bound + 24ULL * 60 * 60 *
                    ticks_per_second + ticks_per_second >= before_last_hours &&
                last_hours.terms[0].lower_bound + 24ULL * 60 * 60 *
                    ticks_per_second <= after_last_hours + ticks_per_second,
            "last24hours uses an exact rolling lower threshold");

    const auto before_next_minutes = current_file_time();
    const auto next_minutes = esm::parse_query(L"dm:next30mins");
    const auto after_next_minutes = current_file_time();
    require(next_minutes.valid && next_minutes.terms[0].has_lower_bound &&
                next_minutes.terms[0].has_upper_bound &&
                next_minutes.terms[0].lower_bound + ticks_per_second >=
                    before_next_minutes &&
                next_minutes.terms[0].lower_bound <=
                    after_next_minutes + ticks_per_second &&
                next_minutes.terms[0].upper_bound -
                    next_minutes.terms[0].lower_bound ==
                    30ULL * 60 * ticks_per_second,
            "next30mins uses an exact rolling future interval");

    const auto last_weeks = esm::parse_query(L"dm:prev3weeks");
    const auto now = current_file_time();
    require(last_weeks.valid && last_weeks.terms[0].has_lower_bound &&
                !last_weeks.terms[0].has_upper_bound,
            "prev3weeks rolling date constant parses");
    const auto weeks_ago = now - last_weeks.terms[0].lower_bound;
    require(weeks_ago >= 20ULL * ticks_per_day &&
                weeks_ago <= 22ULL * ticks_per_day,
            "prev3weeks is approximately three local weeks before now");

    const auto next_months = esm::parse_query(L"dm:next2months");
    require(next_months.valid && next_months.terms[0].has_lower_bound &&
                next_months.terms[0].has_upper_bound,
            "next2months rolling future interval parses");

    SYSTEMTIME local_now{};
    GetLocalTime(&local_now);
    const auto january = esm::parse_query(L"dm:jan");
    const auto january_start = local_system_time(january.terms[0].lower_bound);
    const auto january_end = local_system_time(january.terms[0].upper_bound);
    require(january.valid && january_start.wYear == local_now.wYear &&
                january_start.wMonth == 1 && january_start.wDay == 1 &&
                january_end.wMonth == 2 && january_end.wDay == 1,
            "month-name constant selects that month in the current year");

    const auto tuesday = esm::parse_query(L"dm:tuesday");
    const auto tuesday_start =
        local_system_time(tuesday.terms[0].lower_bound);
    require(tuesday.valid && tuesday_start.wDayOfWeek == 2 &&
                tuesday_start.wHour == 0 && tuesday_start.wMinute == 0,
            "weekday-name constant selects that day in the current week");

    require(!esm::parse_query(L"dm:last0hours").valid &&
                !esm::parse_query(L"dm:last2hour").valid &&
                !esm::parse_query(L"dm:next99999999999999999999seconds").valid,
            "invalid rolling date constants are rejected");
}

void test_advanced_query_and_sorting() {
    auto parsed = esm::parse_query(L"(name:alpha OR name:beta) AND !ext:tmp");
    require(parsed.valid && !parsed.program.empty(), "boolean query parses");
    require(!esm::parse_query(L"alpha OR").valid, "dangling boolean operator rejected");
    require(!esm::parse_query(L"(alpha").valid, "unmatched parenthesis rejected");
    require(!esm::parse_query(L"len:many").valid,
            "invalid filename length rejected");
    const auto filename_functions = esm::parse_query(
        L"startwith:alpha endwith:.txt len:>=10 depth:<=2 parent:D:\\copy");
    require(filename_functions.valid && filename_functions.terms.size() == 5 &&
                filename_functions.terms[0].target == esm::MatchTarget::name_prefix &&
                filename_functions.terms[1].target == esm::MatchTarget::name_suffix &&
                filename_functions.terms[2].target == esm::MatchTarget::filename_length &&
                filename_functions.terms[3].target == esm::MatchTarget::path_depth &&
                filename_functions.terms[4].target == esm::MatchTarget::parent_path,
            "Everything filename and path functions parse");
    const auto compatibility_functions = esm::parse_query(
        L"count:25 root: ext:txt;.log size:1kb..10mb");
    require(compatibility_functions.valid &&
                compatibility_functions.max_results == 25 &&
                compatibility_functions.terms.size() == 3 &&
                compatibility_functions.terms[0].target ==
                    esm::MatchTarget::path_depth &&
                compatibility_functions.terms[1].alternatives.size() == 2 &&
                compatibility_functions.terms[1].alternatives[0] == L"txt" &&
                compatibility_functions.terms[1].alternatives[1] == L"log" &&
                compatibility_functions.terms[2].has_lower_bound &&
                compatibility_functions.terms[2].has_upper_bound,
            "Everything count, root, extension-list and range functions parse");
    require(!esm::parse_query(L"count:many alpha").valid,
            "invalid count rejected");
    require(esm::parse_query(L"<alpha|beta> !ext:tmp").valid,
            "Everything pipe and angle-bracket boolean syntax parses");
    const auto today_query = esm::parse_query(L"datemodified:today");
    require(today_query.valid && today_query.terms.size() == 1 &&
                today_query.terms[0].target ==
                    esm::MatchTarget::last_write_time &&
                today_query.terms[0].has_lower_bound &&
                today_query.terms[0].has_upper_bound &&
                !today_query.terms[0].upper_inclusive,
            "relative modified-date interval parses");
    const auto exact_day_query = esm::parse_query(L"dm:2026-07-28");
    require(exact_day_query.valid && exact_day_query.terms[0].has_lower_bound &&
                exact_day_query.terms[0].has_upper_bound,
            "calendar date expands to the full local day");
    require(esm::natural_compare(L"file2.txt", L"file10.txt") < 0,
            "natural numeric ordering");

    esm::MetadataIndex index;
    auto alpha = record(1, L"alpha2.txt", L"D:\\alpha2.txt");
    alpha.size = 2048;
    alpha.attributes = FILE_ATTRIBUTE_ARCHIVE;
    alpha.last_write_time = static_cast<std::int64_t>(
        today_query.terms[0].lower_bound);
    auto beta = record(2, L"beta10.log", L"D:\\beta10.log");
    beta.size = 8 * 1024 * 1024;
    beta.attributes = FILE_ATTRIBUTE_HIDDEN;
    auto alpha_ten = record(3, L"alpha10.txt", L"D:\\alpha10.txt");
    alpha_ten.size = 2048;
    auto duplicate = record(4, L"alpha2.txt", L"D:\\copy\\alpha2.txt");
    duplicate.size = 2048;
    index.replace({alpha, beta, alpha_ten, duplicate});

    esm::SearchOptions options;
    options.match_path = true;
    auto results = index.search(L"(alpha OR beta) size:>=1mb", options);
    require(results.size() == 1 && results[0].record.id == 2,
            "boolean and size predicate");
    results = index.search(L"regex:\"^alpha[0-9]+\\.txt$\"", options);
    require(results.size() == 3, "regular expression predicate");
    results = index.search(L"attr:hidden", options);
    require(results.size() == 1 && results[0].record.id == 2,
            "attribute predicate");
    results = index.search(L"startwith:alpha", options);
    require(results.size() == 3, "startwith filename function");
    results = index.search(L"endwith:.log", options);
    require(results.size() == 1 && results[0].record.id == 2,
            "endwith filename function");
    results = index.search(L"len:=11", options);
    require(results.size() == 1 && results[0].record.id == 3,
            "filename length function");
    results = index.search(L"depth:=0", options);
    require(results.size() == 3, "path depth function for volume-root files");
    results = index.search(L"parents:=1", options);
    require(results.size() == 1 && results[0].record.id == 4,
            "parents alias for path depth");
    results = index.search(L"parent:D:\\copy", options);
    require(results.size() == 1 && results[0].record.id == 4,
            "parent exact-folder function excludes subfolders");
    results = index.search(L"infolder:D:\\", options);
    require(results.size() == 3,
            "infolder alias accepts a volume root parent");
    results = index.search(L"root:", options);
    require(results.size() == 3,
            "root function matches entries directly below a volume root");
    results = index.search(L"ext:txt;log", options);
    require(results.size() == 4,
            "semicolon extension list matches every listed extension");
    results = index.search(L"ext:t*;log", options);
    require(results.size() == 4,
            "extension list supports per-extension wildcards");
    results = index.search(L"size:large", options);
    require(results.size() == 1 && results[0].record.id == 2,
            "Everything large size constant");
    results = index.search(L"size:1mb..10mb", options);
    require(results.size() == 1 && results[0].record.id == 2,
            "inclusive size range");
    results = index.search(L"dm:today", options);
    require(results.size() == 1 && results[0].record.id == 1,
            "today modified-date interval matches current local day");
    results = index.search(L"len:10-11", options);
    require(results.size() == 4, "numeric hyphen range");
    results = index.search(L"count:2 alpha", options);
    require(results.size() == 2, "count function caps query results");
    results = index.search(L"<alpha|beta> !ext:log", options);
    require(results.size() == 3,
            "pipe OR and angle-bracket grouping execute with NOT");
    results = index.search(L"alpha dupe:name-size", options);
    require(results.size() == 2, "duplicate name and size predicate");
    options.sort = esm::SortField::name;
    results = index.search(L"alpha", options);
    require(results.size() == 3 && results[0].record.name == L"alpha2.txt" &&
                results.back().record.name == L"alpha10.txt",
            "natural name sort");
}

void test_child_count_query_functions() {
    const auto parsed = esm::parse_query(
        L"child:alpha empty: childcount:1..3 childfilecount:>=1 "
        L"childfoldercount:=1");
    require(parsed.valid && parsed.terms.size() == 5 &&
                parsed.terms[0].target == esm::MatchTarget::child_name &&
                parsed.terms[1].target == esm::MatchTarget::direct_child_count &&
                parsed.terms[2].target == esm::MatchTarget::direct_child_count &&
                parsed.terms[3].target == esm::MatchTarget::child_file_count &&
                parsed.terms[4].target == esm::MatchTarget::child_folder_count,
            "Everything direct-child functions parse");
    require(!esm::parse_query(L"child:").valid,
            "empty child filename rejected");
    require(!esm::parse_query(L"childcount:many").valid,
            "invalid child count rejected");

    std::vector<esm::FileRecord> records;
    auto add = [&](std::uint64_t id, std::uint64_t parent_id,
                   std::wstring name, std::wstring path, bool directory) {
        auto value = record(id, std::move(name), std::move(path), directory);
        value.parent_id = parent_id;
        records.push_back(std::move(value));
    };
    add(1, 0, L"Root", LR"(D:\Root)", true);
    add(2, 1, L"Empty", LR"(D:\Root\Empty)", true);
    add(3, 1, L"Mixed", LR"(D:\Root\Mixed)", true);
    add(4, 3, L"Nested", LR"(D:\Root\Mixed\Nested)", true);
    add(5, 3, L"alpha.txt", LR"(D:\Root\Mixed\alpha.txt)", false);
    add(6, 4, L"beta.txt", LR"(D:\Root\Mixed\Nested\beta.txt)", false);
    add(7, 1, L"Files", LR"(D:\Root\Files)", true);
    add(8, 7, L"gamma.txt", LR"(D:\Root\Files\gamma.txt)", false);

    esm::MetadataIndex index;
    index.replace(std::move(records));
    esm::SearchOptions options;
    options.limit = 20;

    auto results = index.search(L"empty:", options);
    require(results.size() == 1 && results[0].record.id == 2,
            "empty function matches only empty folders");
    results = index.search(L"childcount:=2", options);
    require(results.size() == 1 && results[0].record.id == 3,
            "childcount includes direct files and folders");
    results = index.search(L"childfilecount:=1", options);
    require(results.size() == 3,
            "childfilecount counts direct files only");
    results = index.search(L"childfoldercount:=1", options);
    require(results.size() == 1 && results[0].record.id == 3,
            "childfoldercount counts direct folders only");
    results = index.search(L"file: childcount:=0", options);
    require(results.empty(), "child count functions never match files");
    results = index.search(L"child:alpha.txt", options);
    require(results.size() == 1 && results[0].record.id == 3,
            "child function matches a direct child filename");
    results = index.search(L"child:*.txt", options);
    require(results.size() == 3,
            "child function supports wildcard child filenames");
    results = index.search(L"child:Nested", options);
    require(results.size() == 1 && results[0].record.id == 3,
            "child function includes direct child folders");
    results = index.search(L"folder: !child:*.txt", options);
    require(results.size() == 2,
            "negated child function combines with folder filtering");

    auto live_child = record(9, L"live.txt", LR"(D:\Root\Empty\live.txt)");
    live_child.parent_id = 2;
    index.apply_delta({live_child}, {});
    require(index.search(L"empty:", options).empty(),
            "overlay child creation updates empty-folder semantics");
    results = index.search(L"child:live.txt", options);
    require(results.size() == 1 && results[0].record.id == 2,
            "overlay child creation updates child filename semantics");
    index.apply_delta({}, {9});
    results = index.search(L"empty:", options);
    require(results.size() == 1 && results[0].record.id == 2,
            "overlay child removal restores empty-folder semantics");
    require(index.search(L"child:live.txt", options).empty(),
            "overlay child removal clears child filename semantics");
}

void test_index_rvalue_replace_releases_source() {
    esm::MetadataIndex index;
    std::vector<esm::FileRecord> records;
    records.reserve(8);
    auto directory = record(10, L"folder", L"D:\\folder");
    directory.directory = true;
    records.push_back(std::move(directory));
    auto file = record(11, L"release-test.txt",
                       L"D:\\folder\\release-test.txt");
    file.parent_id = 10;
    records.push_back(std::move(file));

    index.replace(std::move(records));

    require(records.empty() && records.capacity() == 0,
            "rvalue index replacement releases source storage");
    const auto stats = index.storage_stats();
    require(stats.name_only_paths == 1,
            "base index stores child files as name-only paths");
    const auto results = index.search(L"path:folder release-test");
    require(results.size() == 1 &&
                results.front().record.path ==
                    L"D:\\folder\\release-test.txt",
            "componentized index reconstructs searchable paths");
}

void test_index_componentized_path_fallback_and_compaction() {
    esm::MetadataIndex orphan_index;
    auto orphan = record(21, L"orphan.txt", L"D:\\lost\\orphan.txt");
    orphan.parent_id = 999;
    std::vector<esm::FileRecord> orphan_records;
    orphan_records.push_back(std::move(orphan));
    orphan_index.replace(std::move(orphan_records));
    require(orphan_index.storage_stats().name_only_paths == 0,
            "orphan path keeps the complete fallback string");
    const auto orphan_results = orphan_index.search(L"path:lost orphan");
    require(orphan_results.size() == 1 &&
                orphan_results.front().record.parent_id == 999 &&
                orphan_results.front().record.path == L"D:\\lost\\orphan.txt",
            "orphan parent ID and fallback path remain searchable");

    esm::MetadataIndex compacted_index(0);
    auto directory = record(30, L"folder", L"D:\\folder");
    directory.directory = true;
    auto child = record(31, L"before.txt", L"D:\\folder\\before.txt");
    child.parent_id = 30;
    std::vector<esm::FileRecord> base;
    base.push_back(std::move(directory));
    base.push_back(std::move(child));
    compacted_index.replace(std::move(base));

    auto renamed = record(31, L"after.txt", L"D:\\folder\\after.txt");
    renamed.parent_id = 30;
    std::vector<esm::FileRecord> upserts;
    upserts.push_back(std::move(renamed));
    compacted_index.apply_delta(std::move(upserts), {});
    require(compacted_index.compact(),
            "overlay compaction rebuilds the compact base");
    require(compacted_index.storage_stats().name_only_paths == 1,
            "compaction restores name-only child paths");
    const auto renamed_results = compacted_index.search(L"path:folder after");
    require(renamed_results.size() == 1 &&
                renamed_results.front().record.parent_id == 30 &&
                renamed_results.front().record.path ==
                    L"D:\\folder\\after.txt",
            "componentized path survives overlay compaction");
}

void test_index_direct_ntfs_changes() {
    constexpr std::wstring_view identity = L"test-volume-direct-usn";
    constexpr std::uint64_t raw_root_id = 5;
    const auto scoped = [](std::uint64_t id) {
        return esm::namespace_ntfs_file_id(L"test-volume-direct-usn", id);
    };

    auto root = record(scoped(raw_root_id), L"", L"C:\\");
    root.parent_id = scoped(raw_root_id);
    root.directory = true;
    root.attributes = FILE_ATTRIBUTE_DIRECTORY;
    auto folder = record(scoped(10), L"folder", L"C:\\folder");
    folder.parent_id = scoped(raw_root_id);
    folder.directory = true;
    folder.attributes = FILE_ATTRIBUTE_DIRECTORY;
    auto child = record(scoped(11), L"before.txt",
                        L"C:\\folder\\before.txt");
    child.parent_id = scoped(10);

    esm::MetadataIndex index(0);
    std::vector<esm::FileRecord> base;
    base.push_back(std::move(root));
    base.push_back(std::move(folder));
    base.push_back(std::move(child));
    index.replace(std::move(base));

    esm::UsnChangeBatch create_batch;
    esm::UsnChange created;
    created.file_id = 12;
    created.parent_id = 10;
    created.reason = USN_REASON_FILE_CREATE;
    created.attributes = FILE_ATTRIBUTE_ARCHIVE;
    created.name = L"created.txt";
    create_batch.changes.push_back(std::move(created));
    index.apply_ntfs_changes(identity, L"C:\\", raw_root_id, create_batch);
    auto results = index.search(L"path:folder created");
    require(results.size() == 1 && results.front().record.id == scoped(12) &&
                results.front().record.path == L"C:\\folder\\created.txt",
            "direct USN create updates searchable namespaced index");

    esm::UsnChangeBatch rename_batch;
    esm::UsnChange old_name;
    old_name.file_id = 10;
    old_name.parent_id = raw_root_id;
    old_name.reason = USN_REASON_RENAME_OLD_NAME;
    old_name.attributes = FILE_ATTRIBUTE_DIRECTORY;
    old_name.name = L"folder";
    rename_batch.changes.push_back(std::move(old_name));
    esm::UsnChange new_name;
    new_name.file_id = 10;
    new_name.parent_id = raw_root_id;
    new_name.reason = USN_REASON_RENAME_NEW_NAME;
    new_name.attributes = FILE_ATTRIBUTE_DIRECTORY;
    new_name.name = L"renamed";
    rename_batch.changes.push_back(std::move(new_name));
    index.apply_ntfs_changes(identity, L"C:\\", raw_root_id, rename_batch);
    results = index.search(L"path:renamed before");
    require(results.size() == 1 &&
                results.front().record.path == L"C:\\renamed\\before.txt",
            "direct USN directory rename refreshes descendant paths");
    results = index.search(L"path:renamed created");
    require(results.size() == 1 &&
                results.front().record.path == L"C:\\renamed\\created.txt",
            "direct USN directory rename refreshes created descendants");

    esm::UsnChangeBatch delete_batch;
    esm::UsnChange deleted;
    deleted.file_id = 11;
    deleted.parent_id = 10;
    deleted.reason = USN_REASON_FILE_DELETE;
    deleted.name = L"before.txt";
    delete_batch.changes.push_back(std::move(deleted));
    index.apply_ntfs_changes(identity, L"C:\\", raw_root_id, delete_batch);
    require(index.search(L"before.txt").empty(),
            "direct USN delete suppresses the base record");
}

void test_direct_ntfs_change_metadata_hydration() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root_path = std::filesystem::temp_directory_path() /
        ("esm-usn-metadata-test-" + std::to_string(suffix));
    std::filesystem::create_directories(root_path);
    const auto file_path = root_path / "live-metadata.bin";
    { std::ofstream(file_path, std::ios::binary) << "live-metadata"; }

    constexpr std::wstring_view identity = L"test-volume-usn-metadata";
    constexpr std::uint64_t raw_root_id = 5;
    const auto scoped_root_id =
        esm::namespace_ntfs_file_id(identity, raw_root_id);
    auto root = record(scoped_root_id, L"", root_path.wstring(), true);
    root.parent_id = scoped_root_id;
    root.attributes = FILE_ATTRIBUTE_DIRECTORY;

    esm::MetadataIndex index(0);
    std::vector<esm::FileRecord> base;
    base.push_back(std::move(root));
    index.replace(std::move(base));

    esm::UsnChangeBatch created_batch;
    created_batch.changes.push_back(
        {12, raw_root_id, 1, USN_REASON_FILE_CREATE,
         FILE_ATTRIBUTE_ARCHIVE, L"live-metadata.bin"});
    index.apply_ntfs_changes(identity, root_path.wstring(), raw_root_id,
                             created_batch);

    auto results = index.search(L"name:live-metadata.bin size:=13");
    require(results.size() == 1 && results.front().record.size == 13 &&
                results.front().record.last_write_time != 0,
            "direct USN create hydrates size and write time");

    { std::ofstream(file_path, std::ios::binary | std::ios::app) << "-updated"; }
    esm::UsnChangeBatch updated_batch;
    updated_batch.changes.push_back(
        {12, raw_root_id, 2, USN_REASON_DATA_EXTEND,
         FILE_ATTRIBUTE_ARCHIVE, L"live-metadata.bin"});
    index.apply_ntfs_changes(identity, root_path.wstring(), raw_root_id,
                             updated_batch);
    results = index.search(L"name:live-metadata.bin size:=21");
    require(results.size() == 1 && results.front().record.size == 21,
            "direct USN data change refreshes file size");

    std::filesystem::remove_all(root_path);
}

void test_shared_directory_path_signatures() {
    esm::MetadataIndex index;
    auto root = record(1, L"shared", L"D:\\shared");
    root.directory = true;
    root.attributes = FILE_ATTRIBUTE_DIRECTORY;
    auto branch = record(2, L"branch", L"D:\\shared\\branch");
    branch.parent_id = 1;
    branch.directory = true;
    branch.attributes = FILE_ATTRIBUTE_DIRECTORY;
    auto needle = record(3, L"needle.txt",
                         L"D:\\shared\\branch\\needle.txt");
    needle.parent_id = 2;
    auto other = record(4, L"other.log",
                        L"D:\\shared\\branch\\other.log");
    other.parent_id = 2;
    std::vector<esm::FileRecord> records;
    records.push_back(std::move(root));
    records.push_back(std::move(branch));
    records.push_back(std::move(needle));
    records.push_back(std::move(other));
    index.replace(std::move(records));

    const auto stats = index.storage_stats();
    require(stats.name_only_paths == 3,
            "nested directories and files share parent-linked path components");
    require(stats.path_signature_count < stats.base_records,
            "directory path signatures are shared by child files");
    require(stats.path_signature_owner_bytes <=
                stats.base_records * sizeof(std::uint32_t),
            "path signature ownership is no larger than one uint32 per record");

    auto results = index.search(L"path:shared");
    require(results.size() == 4,
            "shared parent signature preserves ancestor path matches");
    results = index.search(L"path:branch");
    require(results.size() == 3,
            "shared parent signature preserves direct directory matches");
    results = index.search(L"path:needle");
    require(results.size() == 1 && results.front().record.id == 3,
            "name signature preserves filename path matches");
    results = index.search(L"path:\"shared\\branch\"");
    require(results.size() == 3,
            "separator path query falls back without false negatives");
    results = index.search(L"path:shared path:needle");
    require(results.size() == 1 && results.front().record.id == 3,
            "multiple mandatory path terms preserve mixed parent/name match");
}

void test_compressed_trigram_postings() {
    constexpr std::size_t record_count = 16'384;
    esm::MetadataIndex index;
    std::vector<esm::FileRecord> records;
    records.reserve(record_count);
    for (std::size_t i = 0; i < record_count; ++i) {
        const auto name = L"shared-trigram-payload-" +
            std::to_wstring(i) + L".txt";
        records.push_back(record(i + 1, name, L"D:\\posting\\" + name));
    }
    index.replace(std::move(records));

    const auto stats = index.storage_stats();
    require(stats.posting_entries > record_count,
            "trigram posting builder records shared grams");
    require(stats.posting_bytes <
                stats.posting_entries * sizeof(std::uint32_t),
            "delta-varint postings use less storage than uint32 entries");

    esm::SearchOptions options;
    options.limit = record_count;
    options.sort = esm::SortField::name;
    auto results = index.search(L"shared-trigram-payload", options);
    require(results.size() == record_count,
            "compressed postings preserve all natural-order matches");
    require(results.front().record.id == 1 &&
                results.back().record.id == record_count,
            "compressed postings preserve ascending natural order");

    options.descending = true;
    results = index.search(L"shared-trigram-payload", options);
    require(results.size() == record_count &&
                results.front().record.id == record_count &&
                results.back().record.id == 1,
            "compressed postings preserve descending natural order");

    options.sort = esm::SortField::relevance;
    options.descending = false;
    results = index.search(L"payload", options);
    require(results.size() == record_count,
            "compressed postings preserve relevance-query matches");
}

void test_wildcard() {
    require(esm::wildcard_match(L"*.cpp", L"main.cpp"), "star wildcard");
    require(esm::wildcard_match(L"file?.txt", L"file1.txt"), "question wildcard");
    require(!esm::wildcard_match(L"*.cpp", L"main.hpp"), "wildcard negative");
}
void test_unicode_substring_search() {
    esm::MetadataIndex index;
    std::vector<esm::FileRecord> records;
    records.push_back(record(1, L"alpha.txt", L"D:\\alpha.txt"));
    records.push_back(record(2, L"\u62a5\u544a.pdf",
                             L"D:\\\u6587\u6863\\\u62a5\u544a.pdf"));
    records.push_back(record(3, L"\u62a5\u62a5\u544a.txt",
                             L"D:\\\u62a5\u62a5\u544a.txt"));
    index.replace(std::move(records));

    esm::SearchOptions options;
    options.match_path = true;
    const auto results = index.search(L"\u62a5\u544a", options);
    require(results.size() == 2, "Unicode substring search count");
    require(results[0].record.id == 2 && results[1].record.id == 3,
            "Unicode substring search ids");
}

void test_diacritic_matching() {
    esm::MetadataIndex index;
    std::vector<esm::FileRecord> records;
    records.push_back(record(1, L"caf\u00e9.txt", L"D:\\caf\u00e9.txt"));
    index.replace(std::move(records));

    esm::SearchOptions options;
    options.match_diacritics = false;
    auto results = index.search(L"cafe", options);
    require(results.size() == 1 && results.front().record.id == 1,
            "diacritic-insensitive search matches cafe to cafe-accent");

    options.match_diacritics = true;
    require(index.search(L"cafe", options).empty(),
            "diacritic-sensitive search rejects missing accent");
    results = index.search(L"caf\u00e9", options);
    require(results.size() == 1 && results.front().record.id == 1,
            "diacritic-sensitive search accepts exact accent");

    esm::MetadataIndex boundary_index;
    boundary_index.replace({
        record(2, L"cafe\u0301x.txt", L"D:\\cafe\u0301x.txt")
    });
    options.match_diacritics = false;
    options.whole_word = true;
    require(boundary_index.search(L"cafe", options).empty(),
            "diacritic folding preserves whole-word boundaries");
}

void test_efu_round_trip() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("esm-efu-test-" + std::to_string(suffix));
    const auto path = root / "round-trip.efu";
    std::filesystem::create_directories(root);

    std::vector<esm::FileRecord> expected;
    auto file = record(1, L"\u62a5\u544a,\"final\".txt",
                       L"D:\\\u6587\u6863\\\u62a5\u544a,\"final\".txt");
    file.size = 123456789;
    file.last_write_time = 133333333333333333ll;
    file.creation_time = 133111111111111111ll;
    file.last_access_time = 133222222222222222ll;
    file.change_time = 133444444444444444ll;
    file.attributes = FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN;
    expected.push_back(file);
    auto directory = record(2, L"\u8d44\u6599,\"2026\"",
                            L"D:\\\u8d44\u6599,\"2026\"", true);
    directory.last_write_time = 133444444444444444ll;
    directory.attributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY;
    expected.push_back(directory);

    std::wstring error;
    require(esm::save_efu_file(path, expected, error),
            "EFU round trip save");
    std::vector<esm::FileRecord> actual;
    require(esm::load_efu_file(path, actual, error),
            "EFU round trip load");
    require(actual.size() == expected.size(), "EFU round trip record count");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        require(actual[i].path == expected[i].path &&
                    actual[i].name == expected[i].name &&
                    actual[i].size == expected[i].size &&
                    actual[i].last_write_time == expected[i].last_write_time &&
                    actual[i].creation_time == expected[i].creation_time &&
                    actual[i].last_access_time == expected[i].last_access_time &&
                    actual[i].change_time == expected[i].change_time &&
                    actual[i].attributes == expected[i].attributes &&
                    actual[i].directory == expected[i].directory,
                "EFU round trip record fields");
    }
    std::filesystem::remove_all(root);
}

void test_saved_search_round_trip() {
    std::vector<esm::SavedSearch> expected;
    esm::SavedSearch first;
    first.name = L"\u9879\u76ee%\t\u6536\u85cf";
    first.query = L"path:D:\\\u9879\u76ee\\nname:%report%";
    first.case_sensitive = true;
    first.whole_word = true;
    first.match_path = true;
    first.match_diacritics = true;
    first.regex = true;
    expected.push_back(first);
    expected.push_back({L"\u666e\u901a", L"*.txt", false, false, false, false, false});

    const auto serialized = esm::serialize_saved_searches(expected);
    std::vector<esm::SavedSearch> actual;
    require(esm::parse_saved_searches(serialized, actual),
            "saved search round trip parse");
    require(actual.size() == expected.size(),
            "saved search round trip count");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        require(actual[i].name == expected[i].name &&
                    actual[i].query == expected[i].query &&
                    actual[i].case_sensitive == expected[i].case_sensitive &&
                    actual[i].whole_word == expected[i].whole_word &&
                    actual[i].match_path == expected[i].match_path &&
                    actual[i].match_diacritics == expected[i].match_diacritics &&
                    actual[i].regex == expected[i].regex,
                "saved search round trip fields");
    }
}

void test_index_search() {
    esm::MetadataIndex index;
    std::vector<esm::FileRecord> records;
    records.push_back(record(1, L"Annual Report.pdf", L"D:\\Docs\\Annual Report.pdf"));
    records.push_back(record(2, L"Annual Report Draft.pdf", L"D:\\Temp\\Annual Report Draft.pdf"));
    records.push_back(record(3, L"readme.md", L"D:\\Projects\\readme.md"));
    records.push_back(record(4, L"Projects", L"D:\\Projects", true));
    index.replace(std::move(records));
    esm::SearchOptions options;
    options.match_path = true;
    auto results = index.search(L"annual ext:pdf !draft file:", options);
    require(results.size() == 1 && results.front().record.id == 1, "filtered search");
    results = index.search(L"path:projects folder:", options);
    require(results.size() == 1 && results.front().record.directory, "folder path query");
    results = index.search(L"*.md", options);
    require(results.size() == 1 && results.front().record.id == 3, "wildcard query");
    options.case_sensitive = true;
    results = index.search(L"annual", options);
    require(results.empty(), "case-sensitive query");
}



void test_simple_query_top_k() {
    esm::MetadataIndex index(0);
    std::vector<esm::FileRecord> records;
    for (std::uint64_t id = 1; id <= 200; ++id) {
        const auto name = L"xneedle_contains_" + std::to_wstring(id) + L".txt";
        records.push_back(record(id, name, L"D:\\contains\\" + name));
    }
    records.push_back(record(500, L"needle", L"D:\\prefix\\needle"));
    for (std::uint64_t id = 501; id <= 650; ++id) {
        const auto name = L"needle_prefix_" + std::to_wstring(id) + L".txt";
        records.push_back(record(id, name, L"D:\\prefix\\" + name));
    }
    index.replace(records);

    esm::SearchOptions options;
    options.limit = 10;
    auto results = index.search(L"needle", options);
    require(results.size() == options.limit,
            "simple top-k query should fill requested page");
    require(results.front().record.id == 500,
            "exact name should outrank prefix and contains matches");
    require(std::all_of(results.begin(), results.end(),
                        [](const esm::SearchResult& result) {
                            return result.record.name.starts_with(L"needle");
                        }),
            "prefix matches should outrank lower-id contains matches");

    std::vector<esm::FileRecord> updates;
    updates.push_back(record(5, L"renamed.txt", L"D:\\renamed.txt"));
    updates.push_back(record(250, L"xtoken_overlay.txt",
                             L"D:\\xtoken_overlay.txt"));
    index.apply_delta(std::move(updates), {});
    options.limit = 10;
    results = index.search(L"token", options);
    require(results.size() == 1 && results.front().record.id == 250,
            "ordered early exit should include overlay matches");

    options.case_sensitive = true;
    require(index.search(L"NEEDLE", options).empty(),
            "prefix accelerator preserves case-sensitive semantics");
}

void test_sorted_top_k_accelerators() {
    esm::MetadataIndex index(0);
    auto item10 = record(1, L"item10.txt", L"D:\\z\\item10.txt");
    item10.size = 300;
    auto item2 = record(2, L"item2.txt", L"D:\\b\\item2.txt");
    item2.size = 100;
    auto item2_case = record(3, L"Item2.txt", L"D:\\a\\Item2.txt");
    item2_case.size = 200;
    auto item02 = record(4, L"item02.txt", L"D:\\c\\item02.txt");
    item02.size = 50;
    auto contains = record(5, L"xitem1.txt", L"D:\\xitem1.txt");
    contains.size = 400;
    auto item20 = record(6, L"item20.txt", L"D:\\item20.txt");
    item20.size = 250;
    index.replace({item10, item2, item2_case, item02, contains, item20});

    esm::SearchOptions options;
    options.sort = esm::SortField::name;
    options.limit = 3;
    auto results = index.search(L"item", options);
    require(results.size() == 3 && results[0].record.id == 3 &&
                results[1].record.id == 2 && results[2].record.id == 4,
            "bounded natural name sort uses name/path order");

    options.descending = true;
    results = index.search(L"item", options);
    require(results.size() == 3 && results[0].record.id == 5 &&
                results[1].record.id == 6 && results[2].record.id == 1,
            "descending natural name order");

    auto replacement = record(2, L"item3.txt", L"D:\\d\\item3.txt");
    replacement.size = 125;
    auto overlay = record(7, L"item1.txt", L"D:\\overlay\\item1.txt");
    overlay.size = 25;
    index.apply_delta({replacement, overlay}, {6});
    options.descending = false;
    options.limit = 4;
    results = index.search(L"item", options);
    require(results.size() == 4 && results[0].record.id == 7 &&
                results[1].record.id == 3 && results[2].record.id == 4 &&
                results[3].record.id == 2,
            "name order merges overlay and suppresses replaced base record");

    options.case_sensitive = true;
    options.limit = 2;
    results = index.search(L"regex:\".*\"", options);
    require(results.size() == 2 && results[0].record.id == 3 &&
                results[1].record.id == 7,
            "case-sensitive name sort uses bounded fallback");

    options.case_sensitive = false;
    options.sort = esm::SortField::size;
    options.limit = 2;
    results = index.search(L"item", options);
    require(results.size() == 2 && results[0].record.id == 7 &&
                results[1].record.id == 4,
            "bounded numeric sort keeps smallest values");
    options.descending = true;
    results = index.search(L"item", options);
    require(results.size() == 2 && results[0].record.id == 5 &&
                results[1].record.id == 1,
            "bounded descending numeric sort keeps largest values");

    esm::MetadataIndex duplicate_index(0);
    duplicate_index.replace({
        record(10, L"duplicate.txt", L"D:\\a\\duplicate.txt"),
        record(11, L"duplicate.txt", L"D:\\b\\duplicate.txt"),
        record(12, L"other-duplicate.txt",
               L"D:\\c\\other-duplicate.txt")});
    options.sort = esm::SortField::name;
    options.descending = false;
    options.limit = 1;
    results = duplicate_index.search(L"duplicate dupe:name", options);
    require(results.size() == 1 && results[0].record.name == L"duplicate.txt",
            "duplicate queries bypass bounded collection until grouping");

    std::vector<esm::FileRecord> generated;
    generated.reserve(600);
    for (std::uint64_t id = 1; id <= 600; ++id) {
        std::wstring name;
        switch (id % 6) {
        case 0:
            name = std::to_wstring((id * 37) % 1'000) + L"-start.txt";
            break;
        case 1:
            name = L"file" + std::to_wstring((id * 91) % 10'000) + L".txt";
            break;
        case 2:
            name = L"file00" + std::to_wstring((id * 13) % 1'000) + L".txt";
            break;
        case 3:
            name = L"Alpha" + std::to_wstring((id * 17) % 500) + L".bin";
            break;
        case 4:
            name = L"鎶ュ憡" + std::to_wstring((id * 19) % 700) + L".pdf";
            break;
        default:
            name = L"prefix-" + std::to_wstring((id * 23) % 900) + L"-tail";
            break;
        }
        generated.push_back(record(10'000 + id, name,
            L"D:\\bucket" + std::to_wstring(id % 11) + L"\\" + name));
    }
    generated.push_back(record(20'001, L"佟digit.txt",
                               L"D:\\unicode\\佟digit.txt"));
    generated.push_back(record(20'002, L"2digit.txt",
                               L"D:\\unicode\\2digit.txt"));
    auto expected = generated;
    std::sort(expected.begin(), expected.end(),
              [](const esm::FileRecord& left, const esm::FileRecord& right) {
                  int order = esm::natural_compare(left.name, right.name, false);
                  if (!order) {
                      order = esm::natural_compare(left.path, right.path, false);
                  }
                  if (!order) {
                      order = left.id < right.id
                          ? -1 : (left.id > right.id ? 1 : 0);
                  }
                  return order < 0;
              });
    esm::MetadataIndex generated_index(0);
    std::reverse(generated.begin(), generated.end());
    generated_index.replace(generated);
    options.limit = 1'000;
    options.case_sensitive = false;
    options.sort = esm::SortField::name;
    results = generated_index.search(L"regex:\".*\"", options);
    require(results.size() == expected.size(),
            "natural order accelerator returns every generated record");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        require(results[i].record.id == expected[i].id,
                "radix natural order matches reference comparator");
    }
}

void test_diacritic_insensitive_top_k() {
    esm::MetadataIndex index(0);
    std::vector<esm::FileRecord> records;
    for (std::uint64_t id = 1; id <= 20; ++id) {
        const auto name = L"elan_prefix_" + std::to_wstring(id) + L".txt";
        records.push_back(record(id, name, L"D:\\raw\\" + name));
    }
    records.push_back(record(500, L"\u00e9lan", L"D:\\accent\\\u00e9lan"));
    records.push_back(record(501, L"\u00e9lan_notes.txt",
                             L"D:\\accent\\\u00e9lan_notes.txt"));
    index.replace(records);

    esm::SearchOptions options;
    options.limit = 3;
    options.match_diacritics = false;
    const auto results = index.search(L"elan", options);
    require(results.size() == options.limit,
            "ignore-diacritics prefix query fills top-k page");
    require(results.front().record.id == 500,
            "accent-folded exact name outranks raw prefix matches");
    const auto folded_prefix = index.search(L"elan_notes", options);
    require(folded_prefix.size() == 1 &&
                folded_prefix.front().record.id == 501,
            "accent-folded prefix entry remains searchable");
}

void test_path_query_top_k_early_exit() {
    esm::MetadataIndex index(0);
    std::vector<esm::FileRecord> records;
    for (std::uint64_t id = 1; id <= 200; ++id) {
        const auto name = L"unrelated_" + std::to_wstring(id) + L".txt";
        records.push_back(record(id, name, L"D:\\bucket\\" + name));
    }
    records.push_back(record(1000, L"bucket", L"D:\\elsewhere\\bucket"));
    index.replace(records);

    esm::SearchOptions options;
    options.limit = 3;
    options.match_diacritics = false;
    const auto results = index.search(L"path:bucket", options);
    require(results.size() == options.limit,
            "path top-k query fills requested page");
    require(results[0].record.id == 1000 &&
                results[1].record.id == 1 && results[2].record.id == 2,
            "path early exit keeps later high-relevance filename match");
}

void test_index_delta_overlay() {
    esm::MetadataIndex index;
    std::vector<esm::FileRecord> records;
    records.push_back(record(1, L"alpha.txt", L"D:\\alpha.txt"));
    records.push_back(record(2, L"beta.txt", L"D:\\beta.txt"));
    index.replace(std::move(records));

    std::vector<esm::FileRecord> upserts;
    upserts.push_back(record(2, L"gamma.txt", L"D:\\renamed\\gamma.txt"));
    upserts.push_back(record(3, L"delta.md", L"D:\\delta.md"));
    index.apply_delta(std::move(upserts), {1});

    esm::SearchOptions options;
    options.match_path = true;
    require(index.search(L"alpha", options).empty(),
            "removed base record hidden");
    require(index.search(L"beta", options).empty(),
            "overridden base name hidden");
    const auto renamed = index.search(L"gamma", options);
    require(renamed.size() == 1 && renamed.front().record.id == 2,
            "overlay update searchable");
    const auto added = index.search(L"delta", options);
    require(added.size() == 1 && added.front().record.id == 3,
            "overlay create searchable");
    require(index.size() == 2, "overlay live size");
    require(index.pending_delta_size() == 3, "overlay delta size");

    // Recreate a removed base id and then delete a newly added id.
    std::vector<esm::FileRecord> recreate;
    recreate.push_back(record(1, L"alpha2.txt", L"D:\\alpha2.txt"));
    index.apply_delta(std::move(recreate), {3});
    require(index.size() == 2, "overlay recreate and remove size");
    require(index.search(L"alpha2", options).size() == 1,
            "recreated base id searchable");
    require(index.search(L"delta", options).empty(),
            "removed overlay record hidden");

    // Trigram postings are in natural-name order rather than record-id order.
    // A low-id removal that sorts after a high-id live match must still remain
    // suppressed during relevance and non-name sorted searches.
    esm::MetadataIndex posting_order_index(0);
    posting_order_index.replace({
        record(100, L"a-rareposting-key.txt",
               L"D:\\a-rareposting-key.txt"),
        record(1, L"z-rareposting-key.txt",
               L"D:\\z-rareposting-key.txt")});
    posting_order_index.apply_delta({}, {1});
    esm::SearchOptions posting_options;
    posting_options.limit = 10;
    const auto posting_results =
        posting_order_index.search(L"rareposting", posting_options);
    require(posting_results.size() == 1 &&
                posting_results.front().record.id == 100,
            "trigram posting scan suppresses removals independent of id order");
}


void test_index_compaction() {
    esm::MetadataIndex index(0);
    std::vector<esm::FileRecord> records;
    records.push_back(record(1, L"alpha.txt", L"D:\\alpha.txt"));
    records.push_back(record(2, L"beta.txt", L"D:\\beta.txt"));
    records.push_back(record(3, L"gamma.txt", L"D:\\gamma.txt"));
    index.replace(std::move(records));

    std::vector<esm::FileRecord> first_delta;
    first_delta.push_back(record(2, L"beta2.txt", L"D:\\beta2.txt"));
    first_delta.push_back(record(4, L"delta.txt", L"D:\\delta.txt"));
    index.apply_delta(std::move(first_delta), {1});
    require(index.pending_delta_size() == 3, "manual compaction pending size");
    require(index.compact(), "manual compaction should run");
    require(index.pending_delta_size() == 0, "manual compaction clears delta");
    require(index.compaction_count() == 1, "manual compaction count");
    require(index.size() == 3, "manual compaction live size");
    require(index.search(L"alpha").empty(), "compaction keeps deletion");
    require(index.search(L"beta2").size() == 1,
            "compaction keeps replacement");
    require(index.search(L"delta").size() == 1,
            "compaction keeps insertion");
    require(!index.compact(), "empty compaction should be skipped");

    index.set_auto_compaction_threshold(2);
    std::vector<esm::FileRecord> second_delta;
    second_delta.push_back(record(3, L"gamma2.txt", L"D:\\gamma2.txt"));
    index.apply_delta(std::move(second_delta), {2});
    require(index.pending_delta_size() == 0,
            "automatic compaction clears threshold-sized delta");
    require(index.compaction_count() == 2, "automatic compaction count");
    require(index.size() == 2, "automatic compaction live size");
    require(index.search(L"beta2").empty(),
            "automatic compaction keeps deletion");
    require(index.search(L"gamma2").size() == 1,
            "automatic compaction keeps replacement");
}


void test_file_metadata_hydration() {
    const auto suffix = std::chrono::steady_clock::now()
                            .time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("esm-metadata-test-" + std::to_string(suffix));
    std::filesystem::create_directories(root);
    const auto file = root / "metadata.bin";
    {
        std::ofstream output(file, std::ios::binary);
        output << "metadata";
    }

    auto value = record(1, L"metadata.bin", file.wstring());
    require(esm::hydrate_file_metadata(value),
            "filesystem metadata hydration should succeed");
    require(value.size == 8 && value.last_write_time > 116444736000000000ll &&
                value.creation_time > 116444736000000000ll &&
                value.last_access_time > 116444736000000000ll &&
                value.change_time > 116444736000000000ll &&
                !value.directory &&
                (value.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0,
            "filesystem metadata hydration populates size and write time");

    auto missing = record(2, L"missing.bin", (root / "missing.bin").wstring());
    missing.size = 77;
    missing.last_write_time = 88;
    require(!esm::hydrate_file_metadata(missing) && missing.size == 77 &&
                missing.last_write_time == 88,
            "failed metadata hydration preserves indexed values");
    std::filesystem::remove_all(root);
}

void test_ipc_protocol_round_trip() {
    esm::IpcSearchRequest request;
    request.limit = 42;
    request.case_sensitive = true;
    request.match_path = true;
    request.whole_word = true;
    request.match_diacritics = false;
    request.sort = esm::SortField::change_time;
    request.descending = true;
    request.query = L"\u62a5\u544a ext:pdf";
    const auto request_payload = esm::encode_search_request(request);
    const auto request_frame = esm::encode_ipc_frame(
        esm::IpcMessageType::search_request, 77, request_payload);

    esm::IpcFrame decoded_frame;
    std::string error;
    require(esm::decode_ipc_frame(request_frame, decoded_frame, error),
            "IPC request frame decode");
    require(decoded_frame.header.request_id == 77,
            "IPC request id round trip");
    require(decoded_frame.header.type == esm::IpcMessageType::search_request,
            "IPC request type round trip");
    esm::IpcSearchRequest decoded_request;
    require(esm::decode_search_request(decoded_frame.payload,
                                       decoded_request, error),
            "IPC search request decode");
    require(decoded_request.limit == request.limit &&
            decoded_request.case_sensitive && decoded_request.match_path &&
            decoded_request.whole_word && !decoded_request.match_diacritics &&
            decoded_request.sort == esm::SortField::change_time &&
            decoded_request.descending &&
            decoded_request.query == request.query,
            "IPC search request fields round trip");

    esm::IpcSearchResponse response;
    response.elapsed_microseconds = 1234;
    response.results.push_back({
        record(9, L"\u62a5\u544a.pdf",
               L"D:\\\u6587\u6863\\\u62a5\u544a.pdf"), 88});
    response.results.back().record.size = 987654321;
    response.results.back().record.last_write_time = 133333333333333333ll;
    response.results.back().record.creation_time = 133111111111111111ll;
    response.results.back().record.last_access_time = 133222222222222222ll;
    response.results.back().record.change_time = 133444444444444444ll;
    const auto response_payload = esm::encode_search_response(response);
    esm::IpcSearchResponse decoded_response;
    require(esm::decode_search_response(response_payload,
                                        decoded_response, error),
            "IPC search response decode");
    require(decoded_response.elapsed_microseconds == 1234 &&
            decoded_response.results.size() == 1 &&
            decoded_response.results.front().score == 88 &&
            decoded_response.results.front().record.path ==
                L"D:\\\u6587\u6863\\\u62a5\u544a.pdf" &&
            decoded_response.results.front().record.size == 987654321 &&
            decoded_response.results.front().record.last_write_time ==
                133333333333333333ll &&
            decoded_response.results.front().record.creation_time ==
                133111111111111111ll &&
            decoded_response.results.front().record.last_access_time ==
                133222222222222222ll &&
            decoded_response.results.front().record.change_time ==
                133444444444444444ll,
            "IPC search response fields round trip");

    auto corrupt = request_frame;
    corrupt[0] ^= 0xff;
    require(!esm::decode_ipc_frame(corrupt, decoded_frame, error),
            "IPC corrupt magic rejected");
}

void test_named_pipe_search() {
    const auto suffix = std::chrono::steady_clock::now()
                            .time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("esm-pipe-metadata-test-" + std::to_string(suffix));
    std::filesystem::create_directories(root);
    const auto file = root / "pipe-metadata.bin";
    {
        std::ofstream output(file, std::ios::binary);
        output << "pipe metadata";
    }

    esm::MetadataIndex index;
    std::vector<esm::FileRecord> records;
    records.push_back(record(1, L"alpha.txt", L"D:\\alpha.txt"));
    records.push_back(record(2, L"pipe-metadata.bin", file.wstring()));
    index.replace(std::move(records));

    const auto pipe_name = L"everything_sm_test_" +
                           std::to_wstring(GetCurrentProcessId()) + L"_" +
                           std::to_wstring(suffix);
    std::uint32_t server_error = ERROR_SUCCESS;
    std::thread server([&] {
        server_error = esm::serve_named_pipe_search_once(pipe_name, index);
    });

    esm::IpcSearchRequest request;
    request.limit = 10;
    request.match_path = true;
    request.query = L"pipe-metadata";
    const auto result = esm::query_named_pipe_search(pipe_name, request, 5000);
    server.join();

    require(server_error == ERROR_SUCCESS, "named pipe server request");
    require(result.error == ERROR_SUCCESS, "named pipe client request");
    require(result.response.results.size() == 1 &&
                result.response.results.front().record.id == 2 &&
                result.response.results.front().record.size == 0 &&
                result.response.results.front().record.last_write_time == 0,
            "named pipe search avoids blocking filesystem metadata hydration");
    std::filesystem::remove_all(root);
}

void test_named_pipe_missing_server_error() {
    const auto suffix = std::chrono::steady_clock::now()
                            .time_since_epoch().count();
    const auto pipe_name = L"everything_sm_missing_" +
                           std::to_wstring(GetCurrentProcessId()) + L"_" +
                           std::to_wstring(suffix);
    esm::IpcSearchRequest request;
    request.limit = 10;
    request.query = L"alpha";
    const auto result = esm::query_named_pipe_search(pipe_name, request, 25);
    require(result.error == ERROR_FILE_NOT_FOUND,
            "missing named pipe reports file not found instead of a busy timeout");
}

void test_named_pipe_concurrent_search() {
    esm::MetadataIndex index;
    std::vector<esm::FileRecord> records;
    records.push_back(record(1, L"alpha.txt", L"D:\\alpha.txt"));
    records.push_back(record(2, L"beta.txt", L"D:\\beta.txt"));
    index.replace(std::move(records));

    const auto suffix = std::chrono::steady_clock::now()
                            .time_since_epoch().count();
    const auto pipe_name = L"everything_sm_concurrent_" +
                           std::to_wstring(GetCurrentProcessId()) + L"_" +
                           std::to_wstring(suffix);
    std::atomic_bool stop{false};
    std::uint32_t server_error = ERROR_SUCCESS;
    std::thread server([&] {
        server_error = esm::serve_named_pipe_search(
            pipe_name, index, stop, 4);
    });

    constexpr std::size_t client_count = 12;
    std::vector<esm::PipeSearchResult> results(client_count);
    std::vector<std::thread> clients;
    clients.reserve(client_count);
    for (std::size_t i = 0; i < client_count; ++i) {
        clients.emplace_back([&, i] {
            esm::IpcSearchRequest request;
            request.limit = 10;
            request.match_path = true;
            request.query = i % 2 == 0 ? L"alpha" : L"beta";
            results[i] = esm::query_named_pipe_search(
                pipe_name, request, 5'000);
        });
    }
    for (auto& client : clients) client.join();
    stop.store(true, std::memory_order_relaxed);
    server.join();

    require(server_error == ERROR_SUCCESS,
            "concurrent named pipe server shutdown");
    for (std::size_t i = 0; i < results.size(); ++i) {
        require(results[i].error == ERROR_SUCCESS,
                "concurrent named pipe client request");
        require(results[i].response.results.size() == 1,
                "concurrent named pipe result count");
        require(results[i].response.results.front().record.id ==
                    (i % 2 == 0 ? 1u : 2u),
                "concurrent named pipe result identity");
    }
}

void test_ntfs_catalog_updates() {
    esm::NtfsCatalog catalog(L"D:", 1);
    std::vector<esm::FileRecord> initial;
    initial.push_back(record(2, L"Old", L"D:\\Old", true));
    initial.back().parent_id = 1;
    initial.back().attributes = FILE_ATTRIBUTE_DIRECTORY;
    initial.push_back(record(3, L"alpha.txt", L"D:\\Old\\alpha.txt"));
    initial.back().parent_id = 2;
    initial.back().attributes = FILE_ATTRIBUTE_NORMAL;
    catalog.replace(std::move(initial));

    esm::UsnChangeBatch batch;
    batch.changes.push_back({2, 1, 100, USN_REASON_RENAME_OLD_NAME,
                             FILE_ATTRIBUTE_DIRECTORY, L"Old"});
    batch.changes.push_back({2, 1, 101, USN_REASON_RENAME_NEW_NAME,
                             FILE_ATTRIBUTE_DIRECTORY, L"New"});
    batch.changes.push_back({4, 2, 102, USN_REASON_FILE_CREATE,
                             FILE_ATTRIBUTE_NORMAL, L"beta.txt"});
    batch.changes.push_back({4, 2, 103, USN_REASON_CLOSE,
                             FILE_ATTRIBUTE_NORMAL, L"beta.txt"});
    batch.changes.push_back({3, 2, 104, USN_REASON_FILE_DELETE,
                             FILE_ATTRIBUTE_NORMAL, L"alpha.txt"});

    const auto applied = catalog.apply(batch);
    require(applied.renamed == 1, "catalog rename count");
    require(applied.created == 1, "catalog create count");
    require(applied.deleted == 1, "catalog delete count");
    require(catalog.size() == 2, "catalog size after updates");

    const auto snapshot = catalog.snapshot();
    const auto folder = std::find_if(snapshot.begin(), snapshot.end(),
        [](const auto& item) { return item.id == 2; });
    const auto created = std::find_if(snapshot.begin(), snapshot.end(),
        [](const auto& item) { return item.id == 4; });
    require(folder != snapshot.end() && folder->path == L"D:\\New",
            "renamed folder path");
    require(created != snapshot.end() &&
                created->path == L"D:\\New\\beta.txt",
            "child path follows renamed folder");
    require(std::none_of(snapshot.begin(), snapshot.end(),
        [](const auto& item) { return item.id == 3; }),
        "deleted file absent");

    // Replaying the same create/close pair is idempotent by file id.
    esm::UsnChangeBatch replay;
    replay.changes.push_back({4, 2, 105, USN_REASON_FILE_CREATE,
                              FILE_ATTRIBUTE_NORMAL, L"beta.txt"});
    replay.changes.push_back({4, 2, 106, USN_REASON_CLOSE,
                              FILE_ATTRIBUTE_NORMAL, L"beta.txt"});
    const auto replayed = catalog.apply(replay);
    require(replayed.updated >= 1, "catalog replay update");
    require(catalog.size() == 2, "catalog replay idempotence");
}


void test_ntfs_catalog_compact_overlay() {
    std::vector<esm::FileRecord> initial;
    initial.push_back(record(2, L"Old", L"D:\\Old", true));
    initial.back().parent_id = 1;
    initial.back().attributes = FILE_ATTRIBUTE_DIRECTORY;
    initial.push_back(record(3, L"alpha.txt", L"D:\\Old\\alpha.txt"));
    initial.back().parent_id = 2;
    initial.back().size = 11;
    initial.back().attributes = FILE_ATTRIBUTE_NORMAL;
    initial.push_back(record(5, L"revive.txt", L"D:\\Old\\revive.txt"));
    initial.back().parent_id = 2;
    initial.back().attributes = FILE_ATTRIBUTE_NORMAL;
    initial.push_back(record(6, L"deleted.txt", L"D:\\Old\\deleted.txt"));
    initial.back().parent_id = 2;
    initial.back().attributes = FILE_ATTRIBUTE_NORMAL;

    esm::NtfsCatalog catalog(L"D:", 1, 0);
    catalog.replace(initial);
    auto stats = catalog.storage_stats();
    require(stats.base_nodes == 4 && stats.overlay_nodes == 0 &&
                stats.tombstones == 0 && stats.live_nodes == 4,
            "catalog compact base initialized");
    require(stats.compact_storage_bytes > 0,
            "catalog compact storage measured");

    esm::UsnChangeBatch batch;
    batch.changes.push_back({2, 1, 100, USN_REASON_RENAME_OLD_NAME,
                             FILE_ATTRIBUTE_DIRECTORY, L"Old"});
    batch.changes.push_back({2, 1, 101, USN_REASON_RENAME_NEW_NAME,
                             FILE_ATTRIBUTE_DIRECTORY, L"New"});
    batch.changes.push_back({3, 2, 102, USN_REASON_BASIC_INFO_CHANGE,
                             FILE_ATTRIBUTE_HIDDEN, L""});
    batch.changes.push_back({4, 2, 103, USN_REASON_FILE_CREATE,
                             FILE_ATTRIBUTE_ARCHIVE, L"beta.txt"});
    batch.changes.push_back({5, 2, 104, USN_REASON_FILE_DELETE,
                             FILE_ATTRIBUTE_NORMAL, L"revive.txt"});
    batch.changes.push_back({6, 2, 105, USN_REASON_FILE_DELETE,
                             FILE_ATTRIBUTE_NORMAL, L"deleted.txt"});
    const auto applied = catalog.apply(batch);
    require(applied.renamed == 1 && applied.created == 1 &&
                applied.deleted == 2,
            "catalog compact overlay apply counts");
    require(catalog.size() == 3 && catalog.pending_delta_size() == 5,
            "catalog compact overlay live and pending sizes");

    esm::UsnChangeBatch revive;
    revive.changes.push_back({5, 2, 106, USN_REASON_FILE_CREATE,
                              FILE_ATTRIBUTE_ARCHIVE, L"revived.txt"});
    const auto revived = catalog.apply(revive);
    require(revived.created == 1 && catalog.size() == 4,
            "catalog tombstone revival");
    auto before = catalog.snapshot();
    const auto revived_record = std::find_if(before.begin(), before.end(),
        [](const auto& item) { return item.id == 5; });
    require(revived_record != before.end() &&
                revived_record->path == L"D:\\New\\revived.txt",
            "catalog revived path uses renamed base parent");

    esm::UsnChangeBatch remove_revived;
    remove_revived.changes.push_back({5, 2, 107, USN_REASON_FILE_DELETE,
                                      FILE_ATTRIBUTE_ARCHIVE,
                                      L"revived.txt"});
    require(catalog.apply(remove_revived).deleted == 1 &&
                catalog.size() == 3,
            "catalog revived node deleted again");

    before = catalog.snapshot();
    require(catalog.compact(), "catalog manual compaction performed");
    require(catalog.pending_delta_size() == 0,
            "catalog compaction clears overlay and tombstones");
    stats = catalog.storage_stats();
    require(stats.base_nodes == 3 && stats.live_nodes == 3 &&
                stats.compaction_count == 1,
            "catalog compaction statistics");
    require(!catalog.compact(), "catalog empty compaction is a no-op");

    const auto after = catalog.snapshot();
    require(after.size() == before.size(),
            "catalog compaction preserves record count");
    for (const auto& expected : before) {
        const auto found = std::find_if(after.begin(), after.end(),
            [&](const auto& item) { return item.id == expected.id; });
        require(found != after.end() && found->parent_id == expected.parent_id &&
                    found->attributes == expected.attributes &&
                    found->directory == expected.directory &&
                    found->name == expected.name && found->path == expected.path,
                "catalog compaction preserves records and paths");
    }
    const auto stored = catalog.storage_snapshot();
    require(std::all_of(stored.begin(), stored.end(),
                        [](const auto& item) { return item.path.empty(); }),
            "catalog persistence snapshot omits paths");

    esm::NtfsCatalog automatic(L"D:", 1, 2);
    automatic.replace(initial);
    esm::UsnChangeBatch auto_batch;
    auto_batch.changes.push_back({3, 2, 200,
                                  USN_REASON_BASIC_INFO_CHANGE,
                                  FILE_ATTRIBUTE_HIDDEN, L""});
    auto_batch.changes.push_back({6, 2, 201, USN_REASON_FILE_DELETE,
                                  FILE_ATTRIBUTE_NORMAL, L"deleted.txt"});
    require(automatic.apply(auto_batch).deleted == 1,
            "catalog automatic compaction apply");
    const auto automatic_stats = automatic.storage_stats();
    require(automatic.pending_delta_size() == 0 &&
                automatic_stats.compaction_count == 1 &&
                automatic.size() == 3,
            "catalog automatic compaction threshold");

    std::vector<esm::FileRecord> empty_name_records;
    empty_name_records.push_back(record(2, L"", L"", false));
    empty_name_records.back().parent_id = 1;
    empty_name_records.back().attributes = FILE_ATTRIBUTE_NORMAL;

    esm::NtfsCatalog empty_names(L"D:", 1, 0);
    empty_names.replace(empty_name_records);
    const auto empty_stats = empty_names.storage_stats();
    require(empty_stats.base_nodes == 1 &&
                empty_stats.base_name_chars == 0 &&
                empty_names.size() == 1,
            "catalog supports an empty compact name arena");
    const auto empty_stored = empty_names.storage_snapshot();
    require(empty_stored.size() == 1 && empty_stored.front().name.empty() &&
                empty_stored.front().path.empty(),
            "catalog empty-name persistence snapshot");
    const auto empty_snapshot = empty_names.snapshot();
    require(empty_snapshot.size() == 1 &&
                empty_snapshot.front().name.empty(),
            "catalog empty-name path snapshot");
}


void test_journal_replay_transaction() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      ("esm-replay-test-" + std::to_string(suffix));
    const auto checkpoint_path = root / "D.checkpoint";

    std::vector<esm::FileRecord> initial;
    initial.push_back(record(2, L"Old", L"D:\\Old", true));
    initial.back().parent_id = 1;
    initial.back().attributes = FILE_ATTRIBUTE_DIRECTORY;
    initial.push_back(record(3, L"alpha.txt", L"D:\\Old\\alpha.txt"));
    initial.back().parent_id = 2;
    initial.back().attributes = FILE_ATTRIBUTE_NORMAL;

    esm::NtfsCatalog catalog(L"D:", 1);
    catalog.replace(initial);
    esm::MetadataIndex index;
    index.replace(std::move(initial));

    esm::UsnJournalState journal;
    journal.available = true;
    journal.journal_id = 778899;
    journal.first_usn = 100;
    journal.next_usn = 1000;

    esm::UsnChangeBatch batch;
    batch.next_usn = 500;
    batch.changes.push_back({2, 1, 200, USN_REASON_RENAME_OLD_NAME,
                             FILE_ATTRIBUTE_DIRECTORY, L"Old"});
    batch.changes.push_back({2, 1, 201, USN_REASON_RENAME_NEW_NAME,
                             FILE_ATTRIBUTE_DIRECTORY, L"New"});
    batch.changes.push_back({4, 2, 202, USN_REASON_FILE_CREATE,
                             FILE_ATTRIBUTE_NORMAL, L"beta.txt"});

    const auto replay = esm::replay_journal_batch(
        catalog, index, journal, batch, checkpoint_path);
    require(replay.error == 0 && replay.checkpoint_saved,
            "journal replay checkpoint committed");
    require(replay.renamed == 1 && replay.created == 1,
            "journal replay counts");

    esm::SearchOptions options;
    options.match_path = true;
    const auto child = index.search(L"path:new alpha", options);
    require(child.size() == 1 && child.front().record.id == 3,
            "renamed directory descendants updated in index");
    const auto created = index.search(L"beta", options);
    require(created.size() == 1 && created.front().record.id == 4,
            "created file added to index");

    const auto checkpoint = esm::load_checkpoint(checkpoint_path);
    require(checkpoint.ok && checkpoint.checkpoint.journal_id == journal.journal_id,
            "replay checkpoint journal id");
    require(checkpoint.checkpoint.next_usn == batch.next_usn,
            "replay checkpoint next usn");
    std::filesystem::remove_all(root);
}

void test_journal_checkpoint() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      ("esm-checkpoint-test-" + std::to_string(suffix));
    const auto path = root / "D.checkpoint";
    const esm::JournalCheckpoint expected{123456789, 987654321};

    const auto volume_comparison = esm::compare_path_volumes(
        path, root / "another.checkpoint");
    require(volume_comparison.ok && volume_comparison.same_volume,
            "same checkpoint volume detection");

    const auto saved = esm::save_checkpoint_atomic(path, expected);
    require(saved.ok, "checkpoint save");
    const auto loaded = esm::load_checkpoint(path);
    require(loaded.ok, "checkpoint load");
    require(loaded.checkpoint.journal_id == expected.journal_id,
            "checkpoint journal id");
    require(loaded.checkpoint.next_usn == expected.next_usn,
            "checkpoint next usn");

    esm::UsnJournalState journal;
    journal.available = true;
    journal.journal_id = expected.journal_id;
    journal.first_usn = 900000000;
    journal.lowest_valid_usn = 0;
    journal.next_usn = 1000000000;
    require(esm::validate_checkpoint(journal, expected) ==
                esm::CheckpointStatus::valid,
            "valid checkpoint");

    auto changed = expected;
    ++changed.journal_id;
    require(esm::validate_checkpoint(journal, changed) ==
                esm::CheckpointStatus::journal_changed,
            "changed journal id");
    auto expired = expected;
    expired.next_usn = journal.first_usn - 1;
    require(esm::validate_checkpoint(journal, expired) ==
                esm::CheckpointStatus::expired,
            "expired checkpoint");
    auto ahead = expected;
    ahead.next_usn = journal.next_usn + 1;
    require(esm::validate_checkpoint(journal, ahead) ==
                esm::CheckpointStatus::ahead_of_journal,
            "checkpoint ahead of journal");

    { std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
      file.seekp(16); const char corrupt = '\x7f'; file.write(&corrupt, 1); }
    require(!esm::load_checkpoint(path).ok, "corrupt checkpoint rejected");
    std::filesystem::remove_all(root);
}

void test_metadata_snapshot() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      ("esm-snapshot-test-" + std::to_string(suffix));
    const auto checkpoint_path = root / "D.checkpoint";
    const auto snapshot_path = esm::metadata_snapshot_path(checkpoint_path);
    require(snapshot_path.filename() == "D.checkpoint.metadata",
            "snapshot path derivation");

    esm::MetadataSnapshot snapshot;
    snapshot.checkpoint = {123456789, 987654321};
    snapshot.root_id = 5;
    snapshot.volume = L"D:";
    auto first = record(5, L"", L"D:", true);
    first.parent_id = 5;
    first.attributes = FILE_ATTRIBUTE_DIRECTORY;
    auto second = record(8, L"\u62a5\u544a.txt",
                         L"D:\\docs\\\u62a5\u544a.txt");
    second.parent_id = 6;
    second.size = 42;
    second.last_write_time = 1234;
    second.attributes = FILE_ATTRIBUTE_ARCHIVE;
    snapshot.records.push_back(std::move(first));
    snapshot.records.push_back(std::move(second));

    const auto saved = esm::save_metadata_snapshot_atomic(
        snapshot_path, snapshot);
    require(saved.ok, "metadata snapshot save");
    require(!std::filesystem::exists(snapshot_path.wstring() + L".tmp"),
            "metadata snapshot temporary file moved");

    const auto legacy_mapped =
        esm::load_metadata_snapshot_mapped(snapshot_path);
    require(!legacy_mapped.ok &&
                legacy_mapped.error == ERROR_REVISION_MISMATCH,
            "legacy metadata snapshot requests fallback loading");

    auto loaded = esm::load_metadata_snapshot(snapshot_path);
    require(loaded.ok, "metadata snapshot load");
    require(loaded.snapshot.checkpoint.journal_id ==
                snapshot.checkpoint.journal_id &&
            loaded.snapshot.checkpoint.next_usn ==
                snapshot.checkpoint.next_usn,
            "metadata snapshot checkpoint");
    require(loaded.snapshot.root_id == snapshot.root_id &&
            loaded.snapshot.volume == snapshot.volume,
            "metadata snapshot identity");
    require(loaded.snapshot.records.size() == 2,
            "metadata snapshot record count");
    const auto& restored = loaded.snapshot.records[1];
    require(restored.id == 8 && restored.parent_id == 6 &&
            restored.size == 42 && restored.last_write_time == 1234 &&
            restored.attributes == FILE_ATTRIBUTE_ARCHIVE &&
            restored.name == L"\u62a5\u544a.txt" &&
            restored.path == L"D:\\docs\\\u62a5\u544a.txt",
            "metadata snapshot record fields");

    {
        std::fstream file(snapshot_path,
                          std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(64);
        char byte = 0;
        file.read(&byte, 1);
        file.clear();
        file.seekp(64);
        byte ^= 0x55;
        file.write(&byte, 1);
    }
    require(!esm::load_metadata_snapshot(snapshot_path).ok,
            "corrupt metadata snapshot rejected");

    require(esm::save_metadata_snapshot_atomic(snapshot_path, snapshot).ok,
            "metadata snapshot rewrite");
    const auto original_size = std::filesystem::file_size(snapshot_path);
    std::filesystem::resize_file(snapshot_path, original_size - 1);
    require(!esm::load_metadata_snapshot(snapshot_path).ok,
            "truncated metadata snapshot rejected");

    auto invalid = snapshot;
    invalid.root_id = 0;
    require(!esm::save_metadata_snapshot_atomic(snapshot_path, invalid).ok,
            "invalid metadata snapshot rejected");
    std::filesystem::remove_all(root);
}


void test_mapped_metadata_snapshot() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      ("esm-mapped-snapshot-test-" +
                       std::to_string(suffix));
    const auto snapshot_path = root / "D.compact.metadata";

    esm::MetadataSnapshot snapshot;
    snapshot.checkpoint = {777, 888};
    snapshot.root_id = 5;
    snapshot.volume = L"D:";
    auto directory = record(8, L"docs", L"", true);
    directory.parent_id = 5;
    directory.attributes = FILE_ATTRIBUTE_DIRECTORY;
    auto file = record(9, L"\u62a5\u544a.txt", L"");
    file.parent_id = 8;
    file.size = 42;
    file.last_write_time = 1234;
    file.attributes = FILE_ATTRIBUTE_ARCHIVE;
    snapshot.records.push_back(std::move(directory));
    snapshot.records.push_back(std::move(file));

    require(esm::save_metadata_snapshot_atomic(snapshot_path, snapshot).ok,
            "compact metadata snapshot save");
    auto mapped = esm::load_metadata_snapshot_mapped(snapshot_path);
    require(mapped.ok && mapped.snapshot.format_version == 2,
            "compact metadata snapshot mapped load");
    require(mapped.snapshot.checkpoint.journal_id == 777 &&
                mapped.snapshot.checkpoint.next_usn == 888 &&
                mapped.snapshot.root_id == 5 &&
                mapped.snapshot.volume == L"D:" &&
                mapped.snapshot.catalog.node_count == 2 &&
                mapped.snapshot.catalog.name_count ==
                    std::wstring(L"docs\u62a5\u544a.txt").size(),
            "mapped metadata snapshot identity and arenas");
    require(mapped.snapshot.catalog.nodes[0].id == 8 &&
                mapped.snapshot.catalog.nodes[1].id == 9 &&
                mapped.snapshot.catalog.nodes[1].name_offset == 4,
            "mapped metadata snapshot node table");

    const auto materialized = esm::load_metadata_snapshot(snapshot_path);
    require(materialized.ok && materialized.snapshot.records.size() == 2 &&
                materialized.snapshot.records[1].name == L"\u62a5\u544a.txt" &&
                materialized.snapshot.records[1].path.empty(),
            "compact snapshot compatibility materialization");

    const auto mapped_file_bytes = mapped.snapshot.catalog.mapped_bytes;
    esm::NtfsCatalog catalog(L"D:", 5, 0);
    require(catalog.replace_mapped(std::move(mapped.snapshot.catalog)),
            "catalog accepts mapped compact base");
    auto stats = catalog.storage_stats();
    require(stats.mapped_base && stats.mapped_file_bytes == mapped_file_bytes &&
                stats.base_nodes == 2 && stats.overlay_nodes == 0,
            "catalog reports mapped compact storage");
    const auto before = catalog.snapshot();
    require(before.size() == 2 && before[0].path == L"D:\\docs" &&
                before[1].path == L"D:\\docs\\\u62a5\u544a.txt",
            "catalog resolves paths directly from mapped base");

    esm::UsnChangeBatch batch;
    batch.changes.push_back({9, 8, 889, USN_REASON_BASIC_INFO_CHANGE,
                             FILE_ATTRIBUTE_HIDDEN, L""});
    require(catalog.apply(batch).updated == 1 &&
                catalog.pending_delta_size() == 1,
            "mapped catalog promotes changed nodes into overlay");
    require(catalog.compact(), "mapped catalog compaction");
    stats = catalog.storage_stats();
    require(!stats.mapped_base && stats.overlay_nodes == 0 &&
                stats.compaction_count == 1,
            "mapped catalog compaction releases file mapping");
    const auto after = catalog.snapshot();
    require(after.size() == 2 &&
                after[1].attributes == FILE_ATTRIBUTE_HIDDEN &&
                after[1].path == L"D:\\docs\\\u62a5\u544a.txt",
            "mapped catalog compaction preserves updated paths");

    require(esm::save_metadata_snapshot_atomic(snapshot_path, snapshot).ok,
            "compact metadata snapshot atomic replacement after unmap");
    {
        std::fstream corrupt(snapshot_path,
                             std::ios::in | std::ios::out | std::ios::binary);
        corrupt.seekp(80);
        char byte = 0;
        corrupt.read(&byte, 1);
        corrupt.clear();
        corrupt.seekp(80);
        byte ^= 0x5a;
        corrupt.write(&byte, 1);
    }
    require(!esm::load_metadata_snapshot_mapped(snapshot_path).ok &&
                !esm::load_metadata_snapshot(snapshot_path).ok,
            "corrupt compact mapped snapshot rejected");

    require(esm::save_metadata_snapshot_atomic(snapshot_path, snapshot).ok,
            "compact metadata snapshot rewrite");
    const auto original_size = std::filesystem::file_size(snapshot_path);
    std::filesystem::resize_file(snapshot_path, original_size - 1);
    require(!esm::load_metadata_snapshot_mapped(snapshot_path).ok,
            "truncated compact mapped snapshot rejected");
    std::filesystem::remove_all(root);
}


void test_streaming_catalog_snapshot() {
    const auto suffix = std::chrono::steady_clock::now()
                            .time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        (L"everything_sm_stream_snapshot_" + std::to_wstring(suffix));
    std::filesystem::create_directories(root);
    const auto snapshot_path = root / L"catalog.metadata";

    esm::NtfsCatalog catalog(L"D:", 1);
    std::vector<esm::FileRecord> records;
    records.push_back(record(2, L"docs", L"D:\\docs", true));
    records.back().parent_id = 1;
    records.back().attributes = FILE_ATTRIBUTE_DIRECTORY;
    records.push_back(record(3, L"report10.txt", L"D:\\docs\\report10.txt"));
    records.back().parent_id = 2;
    records.back().size = 4096;
    records.back().last_write_time = 123456789;
    catalog.replace(records);

    const auto saved = esm::save_metadata_catalog_snapshot_atomic(
        snapshot_path, {91, 700}, 1, L"D:", catalog);
    require(saved.ok, "streaming catalog snapshot save");
    const auto mapped = esm::load_metadata_snapshot_mapped(snapshot_path);
    require(mapped.ok && mapped.snapshot.format_version == 2,
            "streaming catalog snapshot maps as v2");
    require(mapped.snapshot.checkpoint.journal_id == 91 &&
                mapped.snapshot.checkpoint.next_usn == 700 &&
                mapped.snapshot.catalog.node_count == 2 &&
                mapped.snapshot.catalog.name_count ==
                    std::wstring_view(L"docsreport10.txt").size(),
            "streaming catalog snapshot metadata");

    esm::NtfsCatalog restored(L"D:", 1);
    require(restored.replace_mapped(std::move(mapped.snapshot.catalog)),
            "streaming catalog snapshot restore");
    const auto restored_records = restored.snapshot();
    require(restored_records.size() == 2 &&
                restored_records[1].path == L"D:\\docs\\report10.txt" &&
                restored_records[1].size == 4096,
            "streaming catalog snapshot contents");

    esm::UsnChangeBatch pending;
    pending.changes.push_back({4, 2, 701, USN_REASON_FILE_CREATE,
                               FILE_ATTRIBUTE_NORMAL, L"pending.txt"});
    (void)restored.apply(pending);
    const auto rejected = esm::save_metadata_catalog_snapshot_atomic(
        root / L"pending.metadata", {91, 701}, 1, L"D:", restored);
    require(!rejected.ok && rejected.error == ERROR_INVALID_STATE,
            "streaming snapshot rejects uncompacted overlay");
    std::filesystem::remove_all(root);
}

void test_metadata_wal_recovery() {
    const auto suffix = std::chrono::steady_clock::now()
                            .time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("esm-wal-test-" + std::to_string(suffix));
    std::filesystem::create_directories(root);
    const auto wal_path = root / "metadata.wal";

    esm::UsnChangeBatch first;
    first.next_usn = 200;
    first.changes.push_back({2, 10, 150, USN_REASON_FILE_CREATE,
                             FILE_ATTRIBUTE_ARCHIVE, L"created.txt"});
    auto appended = esm::append_metadata_wal(wal_path, 77, 100, first);
    require(appended.ok, "first WAL transaction append");

    esm::UsnChangeBatch second;
    second.next_usn = 300;
    second.changes.push_back({3, 10, 250, USN_REASON_FILE_CREATE,
                              FILE_ATTRIBUTE_ARCHIVE, L"second.log"});
    appended = esm::append_metadata_wal(wal_path, 77, 200, second);
    require(appended.ok, "second WAL transaction append");
    {
        std::ofstream torn(wal_path, std::ios::binary | std::ios::app);
        torn.write("WAL", 3);
    }

    esm::NtfsCatalog catalog(L"D:", 10);
    esm::MetadataIndex index;
    const auto replay = esm::replay_metadata_wal(
        wal_path, 77, 100, catalog, index);
    require(replay.ok && replay.torn_tail && replay.transactions == 2 &&
                replay.next_usn == 300 && replay.discarded_tail_bytes == 3 &&
                std::filesystem::file_size(wal_path) == replay.valid_bytes,
            "WAL replays complete transactions and truncates torn tail");
    require(index.search(L"created").size() == 1 &&
                index.search(L"second").size() == 1,
            "WAL replay updates searchable index");

    esm::NtfsCatalog replayed_catalog(L"D:", 10);
    esm::MetadataIndex replayed_index;
    const auto replay_again = esm::replay_metadata_wal(
        wal_path, 77, 100, replayed_catalog, replayed_index);
    require(replay_again.ok && !replay_again.torn_tail &&
                replay_again.discarded_tail_bytes == 0 &&
                replay_again.transactions == 2 && replay_again.next_usn == 300,
            "WAL second replay sees only complete transactions");

    const auto corrupt_path = root / "corrupt.wal";
    std::filesystem::copy_file(wal_path, corrupt_path);
    {
        std::fstream corrupt(corrupt_path,
                             std::ios::binary | std::ios::in | std::ios::out);
        require(static_cast<bool>(corrupt), "open WAL corruption fixture");
        corrupt.seekg(-1, std::ios::end);
        char byte = 0;
        corrupt.read(&byte, 1);
        byte ^= static_cast<char>(0x5a);
        corrupt.seekp(-1, std::ios::end);
        corrupt.write(&byte, 1);
    }
    esm::NtfsCatalog corrupt_catalog(L"D:", 10);
    esm::MetadataIndex corrupt_index;
    const auto corrupt_replay = esm::replay_metadata_wal(
        corrupt_path, 77, 100, corrupt_catalog, corrupt_index);
    require(!corrupt_replay.ok && corrupt_replay.error == ERROR_CRC &&
                !corrupt_replay.torn_tail,
            "WAL rejects a complete transaction with checksum corruption");

    const auto reset = esm::reset_metadata_wal(wal_path);
    require(reset.ok && std::filesystem::file_size(wal_path) == 0,
            "WAL consolidation reset");
    std::filesystem::remove_all(root);
}

void test_snapshot_wal_checkpoint_crash_recovery() {
    const auto suffix = std::chrono::steady_clock::now()
                            .time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("esm-checkpoint-crash-test-" + std::to_string(suffix));
    std::filesystem::create_directories(root);
    const auto snapshot_path = root / "catalog.metadata";
    const auto wal_path = root / "catalog.wal";

    esm::NtfsCatalog base(L"D:", 10);
    std::vector<esm::FileRecord> records;
    records.push_back(record(11, L"base.txt", L"D:\base.txt"));
    records.back().parent_id = 10;
    base.replace(records);
    require(esm::save_metadata_catalog_snapshot_atomic(
                snapshot_path, {77, 100}, 10, L"D:", base).ok,
            "base snapshot before WAL checkpoint");

    esm::UsnChangeBatch delta;
    delta.next_usn = 200;
    delta.changes.push_back({12, 10, 150, USN_REASON_FILE_CREATE,
                             FILE_ATTRIBUTE_ARCHIVE, L"after.txt"});
    require(esm::append_metadata_wal(wal_path, 77, 100, delta).ok,
            "append WAL before checkpoint consolidation");

    auto first_mapping = esm::load_metadata_snapshot_mapped(snapshot_path);
    require(first_mapping.ok, "map base snapshot for WAL replay");
    esm::NtfsCatalog consolidated(L"D:", 10);
    require(consolidated.replace_mapped(
                std::move(first_mapping.snapshot.catalog)),
            "restore base snapshot for consolidation");
    esm::MetadataIndex consolidated_index;
    consolidated_index.replace(consolidated.snapshot());
    const auto replay = esm::replay_metadata_wal(
        wal_path, 77, 100, consolidated, consolidated_index);
    require(replay.ok && replay.transactions == 1 && replay.next_usn == 200 &&
                consolidated_index.search(L"after").size() == 1,
            "replay WAL before checkpoint consolidation");

    require(consolidated.compact(), "compact catalog before checkpoint");
    require(esm::save_metadata_catalog_snapshot_atomic(
                snapshot_path, {77, 200}, 10, L"D:", consolidated).ok,
            "publish consolidated snapshot");

    // Simulate a crash after atomic snapshot publication but before WAL reset.
    auto second_mapping = esm::load_metadata_snapshot_mapped(snapshot_path);
    require(second_mapping.ok &&
                second_mapping.snapshot.checkpoint.next_usn == 200,
            "restart sees consolidated snapshot cursor");
    esm::NtfsCatalog restarted(L"D:", 10);
    require(restarted.replace_mapped(
                std::move(second_mapping.snapshot.catalog)),
            "restart restores consolidated snapshot");
    esm::MetadataIndex restarted_index;
    restarted_index.replace(restarted.snapshot());
    const auto replay_after_crash = esm::replay_metadata_wal(
        wal_path, 77, 200, restarted, restarted_index);
    require(replay_after_crash.ok && replay_after_crash.transactions == 0 &&
                replay_after_crash.next_usn == 200 &&
                restarted_index.search(L"after").size() == 1,
            "restart skips WAL transactions already present in snapshot");

    require(esm::reset_metadata_wal(wal_path).ok &&
                std::filesystem::file_size(wal_path) == 0,
            "checkpoint consolidation resets WAL after durable snapshot");
    std::filesystem::remove_all(root);
}

struct ChildProcess {
    PROCESS_INFORMATION information{};

    ChildProcess() = default;
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ~ChildProcess() {
        if (information.hProcess != nullptr) {
            if (WaitForSingleObject(information.hProcess, 0) == WAIT_TIMEOUT) {
                TerminateProcess(information.hProcess, 1);
                WaitForSingleObject(information.hProcess, 5'000);
            }
            CloseHandle(information.hProcess);
        }
        if (information.hThread != nullptr)
            CloseHandle(information.hThread);
    }
};

std::filesystem::path current_executable_directory() {
    std::wstring path(MAX_PATH, L'\0');
    const DWORD size = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    require(size != 0 && size < path.size(),
            "resolve test executable directory");
    path.resize(size);
    return std::filesystem::path(path).parent_path();
}

template <typename Predicate>
esm::PipeSearchResult wait_for_server_query(std::wstring_view pipe_name,
                                            std::wstring_view query,
                                            Predicate predicate) {
    esm::PipeSearchResult last;
    esm::IpcSearchRequest request;
    request.limit = 20;
    request.match_path = true;
    request.query = query;
    for (int attempt = 0; attempt < 100; ++attempt) {
        last = esm::query_named_pipe_search(pipe_name, request, 250);
        if (last.error == ERROR_SUCCESS &&
            last.response.error == ERROR_SUCCESS && predicate(last.response)) {
            return last;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return last;
}

void test_scan_server_reconciliation() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("esm-server-watch-test-" + std::to_string(suffix));
    std::filesystem::create_directories(root);
    const auto pipe_name = L"everything_sm_scan_refresh_" +
        std::to_wstring(GetCurrentProcessId()) + L"_" +
        std::to_wstring(suffix);
    const auto server =
        current_executable_directory() / "esm_server.exe";
    require(std::filesystem::exists(server),
            "scan server executable should be built for integration test");

    std::wstring command = L"\"" + server.wstring() + L"\" scan \"" +
        root.wstring() + L"\" \"" + pipe_name + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    ChildProcess child;
    require(CreateProcessW(
                nullptr, command.data(), nullptr, nullptr, FALSE,
                CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup,
                &child.information) != FALSE,
            "launch scan server integration process");

    const auto empty = wait_for_server_query(
        pipe_name, L"scan_refresh_alpha_unique",
        [](const esm::IpcSearchResponse& response) {
            return response.results.empty();
        });
    require(empty.error == ERROR_SUCCESS && empty.response.results.empty(),
            "scan server becomes queryable with empty initial index");

    const auto alpha = root / "scan_refresh_alpha_unique.txt";
    const auto beta = root / "scan_refresh_beta_unique.txt";
    { std::ofstream(alpha) << "alpha"; }
    const auto created = wait_for_server_query(
        pipe_name, L"scan_refresh_alpha_unique",
        [](const esm::IpcSearchResponse& response) {
            return response.results.size() == 1 &&
                   response.results.front().record.name ==
                       L"scan_refresh_alpha_unique.txt";
        });
    require(created.error == ERROR_SUCCESS &&
                created.response.results.size() == 1,
            "scan server reconciles created file");

    std::filesystem::rename(alpha, beta);
    const auto renamed = wait_for_server_query(
        pipe_name, L"scan_refresh_beta_unique",
        [](const esm::IpcSearchResponse& response) {
            return response.results.size() == 1 &&
                   response.results.front().record.name ==
                       L"scan_refresh_beta_unique.txt";
        });
    require(renamed.error == ERROR_SUCCESS &&
                renamed.response.results.size() == 1,
            "scan server reconciles renamed file");
    const auto old_name = wait_for_server_query(
        pipe_name, L"scan_refresh_alpha_unique",
        [](const esm::IpcSearchResponse& response) {
            return response.results.empty();
        });
    require(old_name.error == ERROR_SUCCESS &&
                old_name.response.results.empty(),
            "scan server removes old rename path");

    std::filesystem::remove(beta);
    const auto removed = wait_for_server_query(
        pipe_name, L"scan_refresh_beta_unique",
        [](const esm::IpcSearchResponse& response) {
            return response.results.empty();
        });
    require(removed.error == ERROR_SUCCESS &&
                removed.response.results.empty(),
            "scan server reconciles deleted file");

    const bool signaled = GenerateConsoleCtrlEvent(
        CTRL_BREAK_EVENT, child.information.dwProcessId) != FALSE;
    const DWORD wait = signaled
        ? WaitForSingleObject(child.information.hProcess, 5'000)
        : WAIT_TIMEOUT;
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(child.information.hProcess, 1);
        WaitForSingleObject(child.information.hProcess, 5'000);
    } else {
        DWORD exit_code = 1;
        require(GetExitCodeProcess(child.information.hProcess, &exit_code) &&
                    exit_code == 0,
                "scan server stops cleanly after CTRL_BREAK");
    }
    std::filesystem::remove_all(root);
}

void test_directory_watcher() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("esm-watch-test-" + std::to_string(suffix));
    std::filesystem::create_directories(root);

    esm::DirectoryWatcher watcher(root);
    require(watcher.ready(), "directory watcher should open temporary root");

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<esm::DirectoryChange> changes;
    bool worker_started = false;
    bool worker_stopped = false;
    bool worker_failed = false;
    bool overflowed = false;
    std::uint32_t worker_error = ERROR_SUCCESS;

    std::jthread worker([&](std::stop_token token) {
        {
            std::lock_guard lock(mutex);
            worker_started = true;
        }
        cv.notify_all();
        while (true) {
            auto result = watcher.wait(token);
            {
                std::lock_guard lock(mutex);
                overflowed = overflowed || result.overflowed;
                changes.insert(changes.end(),
                               std::make_move_iterator(result.changes.begin()),
                               std::make_move_iterator(result.changes.end()));
                if (!result.ok) {
                    worker_failed = true;
                    worker_error = result.error;
                }
                if (result.stopped) worker_stopped = true;
            }
            cv.notify_all();
            if (!result.ok || result.stopped) break;
        }
    });

    {
        std::unique_lock lock(mutex);
        require(cv.wait_for(lock, std::chrono::seconds(2),
                            [&] { return worker_started; }),
                "directory watcher worker starts");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const auto has_change = [&](esm::DirectoryChangeAction action,
                                std::wstring_view filename) {
        return std::any_of(changes.begin(), changes.end(),
                           [&](const esm::DirectoryChange& change) {
            return change.action == action &&
                   change.path.filename().wstring() == filename;
        });
    };
    const auto wait_for_change = [&](esm::DirectoryChangeAction action,
                                     std::wstring_view filename,
                                     const char* message) {
        std::unique_lock lock(mutex);
        require(cv.wait_for(lock, std::chrono::seconds(5), [&] {
                    return worker_failed || has_change(action, filename);
                }), message);
        require(!worker_failed, "directory watcher should not fail");
    };

    const auto alpha = root / "alpha.txt";
    const auto beta = root / "beta.txt";
    { std::ofstream(alpha) << "alpha"; }
    wait_for_change(esm::DirectoryChangeAction::added, L"alpha.txt",
                    "watcher reports file creation");

    std::filesystem::rename(alpha, beta);
    wait_for_change(esm::DirectoryChangeAction::renamed_old_name,
                    L"alpha.txt", "watcher reports old rename name");
    wait_for_change(esm::DirectoryChangeAction::renamed_new_name,
                    L"beta.txt", "watcher reports new rename name");

    std::filesystem::remove(beta);
    wait_for_change(esm::DirectoryChangeAction::removed, L"beta.txt",
                    "watcher reports file deletion");

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto stop_started = std::chrono::steady_clock::now();
    worker.request_stop();
    {
        std::unique_lock lock(mutex);
        require(cv.wait_for(lock, std::chrono::seconds(2), [&] {
                    return worker_stopped || worker_failed;
                }), "idle directory watcher stop should return promptly");
        require(!worker_failed && worker_error == ERROR_SUCCESS,
                "idle directory watcher stop should be clean");
    }
    worker.join();
    require(std::chrono::steady_clock::now() - stop_started <
                std::chrono::seconds(2),
            "directory watcher cancellation should not deadlock");
    require(!overflowed, "small watcher test should not overflow");
    std::filesystem::remove_all(root);
}

void test_scanner() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("esm-test-" + std::to_string(suffix));
    std::filesystem::create_directories(root / "nested");
    { std::ofstream(root / "alpha.txt") << "alpha";
      std::ofstream(root / "nested" / "beta.md") << "beta"; }

    const auto scan = esm::scan_directories({root});
    const auto second_scan = esm::scan_directories({root});
    require(scan.records.size() == 3,
            "scanner should see directory and two files");
    require(scan.root_id != 0, "scanner supplies a synthetic root ID");
    require(second_scan.records.size() == scan.records.size(),
            "repeat scanner entry count");

    const auto find_record = [](const esm::ScanResult& result,
                                std::wstring_view name)
        -> const esm::FileRecord& {
        const auto found = std::find_if(
            result.records.begin(), result.records.end(),
            [&](const esm::FileRecord& value) { return value.name == name; });
        if (found == result.records.end()) {
            throw std::runtime_error("scanner record not found");
        }
        return *found;
    };
    const auto& nested = find_record(scan, L"nested");
    const auto& alpha = find_record(scan, L"alpha.txt");
    const auto& beta = find_record(scan, L"beta.md");
    const auto& alpha_again = find_record(second_scan, L"alpha.txt");
    require(alpha.id == alpha_again.id,
            "synthetic scanner IDs are stable across scans");
    require(alpha.parent_id == scan.root_id &&
                nested.parent_id == scan.root_id &&
                beta.parent_id == nested.id,
            "scanner populates synthetic parent IDs");
    require(nested.directory &&
                (nested.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
            "scanner populates directory attributes");
    require(!alpha.directory && alpha.size == 5,
            "scanner populates file size");
    require(alpha.last_write_time > 116444736000000000ll,
            "scanner uses Windows FILETIME timestamps");
    std::filesystem::remove_all(root);
}
}
int main() {
    try {
        test_multi_volume_namespacing(); test_ntfs_volume_discovery(); test_multi_volume_snapshot_round_trip(); test_gui_settings(); test_query_parser(); test_everything_date_constants(); test_advanced_query_and_sorting(); test_child_count_query_functions(); test_wildcard(); test_unicode_substring_search(); test_diacritic_matching(); test_efu_round_trip(); test_saved_search_round_trip(); test_index_search(); test_index_rvalue_replace_releases_source(); test_index_componentized_path_fallback_and_compaction(); test_index_direct_ntfs_changes(); test_direct_ntfs_change_metadata_hydration(); test_shared_directory_path_signatures(); test_compressed_trigram_postings(); test_simple_query_top_k(); test_sorted_top_k_accelerators(); test_diacritic_insensitive_top_k(); test_path_query_top_k_early_exit(); test_index_delta_overlay(); test_index_compaction(); test_file_metadata_hydration(); test_ipc_protocol_round_trip(); test_named_pipe_search(); test_named_pipe_missing_server_error(); test_named_pipe_concurrent_search(); test_ntfs_catalog_updates(); test_ntfs_catalog_compact_overlay(); test_journal_replay_transaction(); test_journal_checkpoint(); test_metadata_snapshot(); test_mapped_metadata_snapshot(); test_streaming_catalog_snapshot(); test_metadata_wal_recovery(); test_snapshot_wal_checkpoint_crash_recovery(); test_directory_watcher(); test_scanner(); test_scan_server_reconciliation();
        std::cout << "all tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test failure: " << error.what() << "\n";
        return 1;
    }
}
