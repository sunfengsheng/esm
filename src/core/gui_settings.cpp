#include "esm/gui_settings.hpp"

#include <algorithm>
#include <charconv>
#include <sstream>
#include <string>
#include <vector>

namespace esm {
namespace {
bool parse_int(std::string_view value, int& result) {
    if (value.empty()) return false;
    const auto* begin = value.data();
    const auto* end = begin + value.size();
    const auto parsed = std::from_chars(begin, end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool parse_bool(std::string_view value, bool& result) {
    int parsed = 0;
    if (!parse_int(value, parsed) || (parsed != 0 && parsed != 1))
        return false;
    result = parsed != 0;
    return true;
}

GuiColumnSettings* find_column(GuiSettings& settings, int id) {
    if (id < 0 || id >= static_cast<int>(gui_column_count)) return nullptr;
    for (auto& column : settings.columns) {
        if (static_cast<int>(column.id) == id) return &column;
    }
    return nullptr;
}
} // namespace

GuiSettings default_gui_settings() {
    GuiSettings settings;
    settings.columns = {{{GuiColumnId::name, 260, 0, true},
                         {GuiColumnId::path, 420, 1, true},
                         {GuiColumnId::size, 100, 2, true},
                         {GuiColumnId::last_write_time, 145, 3, true},
                         {GuiColumnId::type, 100, 4, false}}};
    return settings;
}

void normalize_gui_settings(GuiSettings& settings) {
    const auto defaults = default_gui_settings();
    std::array<GuiColumnSettings, gui_column_count> normalized =
        defaults.columns;
    std::array<bool, gui_column_count> seen{};
    for (const auto& requested : settings.columns) {
        const auto id = static_cast<std::size_t>(requested.id);
        if (id >= gui_column_count || seen[id]) continue;
        normalized[id] = requested;
        seen[id] = true;
    }
    settings.columns = normalized;

    for (auto& column : settings.columns) {
        column.width = std::clamp(column.width, 32, 4096);
        column.order =
            std::clamp(column.order, 0,
                       static_cast<int>(gui_column_count - 1));
    }

    std::array<std::size_t, gui_column_count> indexes{};
    for (std::size_t i = 0; i < indexes.size(); ++i) indexes[i] = i;
    std::stable_sort(indexes.begin(), indexes.end(), [&](std::size_t a,
                                                          std::size_t b) {
        if (settings.columns[a].order != settings.columns[b].order)
            return settings.columns[a].order < settings.columns[b].order;
        return static_cast<std::uint32_t>(settings.columns[a].id) <
               static_cast<std::uint32_t>(settings.columns[b].id);
    });
    for (std::size_t order = 0; order < indexes.size(); ++order)
        settings.columns[indexes[order]].order = static_cast<int>(order);

    if (std::none_of(settings.columns.begin(), settings.columns.end(),
                     [](const GuiColumnSettings& value) {
                         return value.visible;
                     })) {
        settings.columns[static_cast<std::size_t>(GuiColumnId::name)].visible =
            true;
    }
    settings.sort_field = std::clamp(settings.sort_field, 0, 13);
    settings.font_delta = std::clamp(settings.font_delta, -4, 8);
    settings.topmost_mode = std::clamp(settings.topmost_mode, 0, 2);
    settings.window_size_mode = std::clamp(settings.window_size_mode, 0, 3);
    settings.search_filter = std::clamp(settings.search_filter, 0, 107);
    settings.view_mode = std::clamp(settings.view_mode, 0, 3);
    settings.result_limit = std::clamp(settings.result_limit, 10, 100000);
    settings.window_width = std::clamp(settings.window_width, 480, 8192);
    settings.window_height = std::clamp(settings.window_height, 320, 8192);
}

std::string serialize_gui_settings(const GuiSettings& requested) {
    auto settings = requested;
    normalize_gui_settings(settings);
    std::ostringstream out;
    out << "esm_gui_settings=5\n"
        << "sort_field=" << settings.sort_field << '\n'
        << "descending=" << (settings.descending ? 1 : 0) << '\n'
        << "show_preview=" << (settings.show_preview ? 1 : 0) << '\n'
        << "show_status=" << (settings.show_status ? 1 : 0) << '\n'
        << "show_filter=" << (settings.show_filter ? 1 : 0) << '\n'
        << "topmost_mode=" << settings.topmost_mode << '\n'
        << "font_delta=" << settings.font_delta << '\n'
        << "window_size_mode=" << settings.window_size_mode << '\n'
        << "search_filter=" << settings.search_filter << '\n'
        << "view_mode=" << settings.view_mode << '\n'
        << "result_limit=" << settings.result_limit << '\n'
        << "regex_mode=" << (settings.regex_mode ? 1 : 0) << '\n'
        << "case_sensitive=" << (settings.case_sensitive ? 1 : 0) << '\n'
        << "whole_word=" << (settings.whole_word ? 1 : 0) << '\n'
        << "match_path=" << (settings.match_path ? 1 : 0) << '\n'
        << "match_diacritics=" << (settings.match_diacritics ? 1 : 0) << '\n'
        << "window_valid=" << (settings.window_valid ? 1 : 0) << '\n'
        << "window_x=" << settings.window_x << '\n'
        << "window_y=" << settings.window_y << '\n'
        << "window_width=" << settings.window_width << '\n'
        << "window_height=" << settings.window_height << '\n'
        << "maximized=" << (settings.maximized ? 1 : 0) << '\n';
    for (const auto& column : settings.columns) {
        out << "column=" << static_cast<std::uint32_t>(column.id) << ','
            << column.width << ',' << column.order << ','
            << (column.visible ? 1 : 0) << '\n';
    }
    return out.str();
}

bool parse_gui_settings(std::string_view text, GuiSettings& settings) {
    auto parsed = default_gui_settings();
    bool version_seen = false;
    int settings_version = 0;
    std::array<bool, gui_column_count> column_seen{};
    std::size_t offset = 0;
    while (offset <= text.size()) {
        const auto end = text.find('\n', offset);
        auto line = text.substr(offset, end == std::string_view::npos
                                           ? text.size() - offset
                                           : end - offset);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const auto separator = line.find('=');
        if (separator != std::string_view::npos) {
            const auto key = line.substr(0, separator);
            const auto value = line.substr(separator + 1);
            if (key == "esm_gui_settings") {
                version_seen = parse_int(value, settings_version) &&
                    (settings_version == 1 || settings_version == 2 ||
                     settings_version == 3 || settings_version == 4 ||
                     settings_version == 5);
            } else if (key == "sort_field") {
                (void)parse_int(value, parsed.sort_field);
            } else if (key == "descending") {
                (void)parse_bool(value, parsed.descending);
            } else if (key == "show_preview") {
                (void)parse_bool(value, parsed.show_preview);
            } else if (key == "show_status") {
                (void)parse_bool(value, parsed.show_status);
            } else if (key == "show_filter") {
                (void)parse_bool(value, parsed.show_filter);
            } else if (key == "topmost_mode") {
                (void)parse_int(value, parsed.topmost_mode);
            } else if (key == "font_delta") {
                (void)parse_int(value, parsed.font_delta);
            } else if (key == "window_size_mode") {
                (void)parse_int(value, parsed.window_size_mode);
            } else if (key == "search_filter") {
                (void)parse_int(value, parsed.search_filter);
            } else if (key == "view_mode") {
                (void)parse_int(value, parsed.view_mode);
            } else if (key == "result_limit") {
                (void)parse_int(value, parsed.result_limit);
            } else if (key == "regex_mode") {
                (void)parse_bool(value, parsed.regex_mode);
            } else if (key == "case_sensitive") {
                (void)parse_bool(value, parsed.case_sensitive);
            } else if (key == "whole_word") {
                (void)parse_bool(value, parsed.whole_word);
            } else if (key == "match_path") {
                (void)parse_bool(value, parsed.match_path);
            } else if (key == "match_diacritics") {
                (void)parse_bool(value, parsed.match_diacritics);
            } else if (key == "window_valid") {
                (void)parse_bool(value, parsed.window_valid);
            } else if (key == "window_x") {
                (void)parse_int(value, parsed.window_x);
            } else if (key == "window_y") {
                (void)parse_int(value, parsed.window_y);
            } else if (key == "window_width") {
                (void)parse_int(value, parsed.window_width);
            } else if (key == "window_height") {
                (void)parse_int(value, parsed.window_height);
            } else if (key == "maximized") {
                (void)parse_bool(value, parsed.maximized);
            } else if (key == "column") {
                int values[4]{};
                bool valid = true;
                std::size_t part_offset = 0;
                for (int i = 0; i < 4; ++i) {
                    const auto comma = value.find(',', part_offset);
                    const auto part = value.substr(
                        part_offset, comma == std::string_view::npos
                                         ? value.size() - part_offset
                                         : comma - part_offset);
                    if (!parse_int(part, values[i])) valid = false;
                    if (i < 3 && comma == std::string_view::npos) valid = false;
                    if (i == 3 && comma != std::string_view::npos) valid = false;
                    part_offset = comma == std::string_view::npos
                        ? value.size() : comma + 1;
                }
                if (valid && values[0] >= 0 &&
                    values[0] < static_cast<int>(gui_column_count) &&
                    (values[3] == 0 || values[3] == 1) &&
                    !column_seen[static_cast<std::size_t>(values[0])]) {
                    if (auto* column = find_column(parsed, values[0])) {
                        column->width = values[1];
                        column->order = values[2];
                        column->visible = values[3] != 0;
                        column_seen[static_cast<std::size_t>(values[0])] = true;
                    }
                }
            }
        }
        if (end == std::string_view::npos) break;
        offset = end + 1;
    }
    if (!version_seen) return false;
    // Version 1 shipped with path matching enabled by default, which forced
    // every ordinary filename query through a full-path scan. Version 2
    // migrates that old default to Everything-style filename-only matching;
    // users can still explicitly enable path matching from the Search menu.
    if (settings_version == 1) parsed.match_path = false;
    // Everything-style menus do not expose the old relevance-only mode.
    if (settings_version <= 4 && parsed.sort_field == 0) parsed.sort_field = 1;
    normalize_gui_settings(parsed);
    settings = parsed;
    return true;
}
} // namespace esm
