#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace esm {
enum class GuiColumnId : std::uint32_t {
    name = 0,
    path = 1,
    size = 2,
    last_write_time = 3,
    type = 4,
};

constexpr std::size_t gui_column_count = 5;

struct GuiColumnSettings {
    GuiColumnId id{GuiColumnId::name};
    int width{};
    int order{};
    bool visible{true};
};

struct GuiSettings {
    std::array<GuiColumnSettings, gui_column_count> columns{};
    int sort_field{1};
    bool descending{};
    bool show_preview{};
    bool show_status{true};
    bool show_filter{true};
    int topmost_mode{};
    int font_delta{};
    int window_size_mode{3};
    int search_filter{};
    int view_mode{}; // 0 details, 1 medium, 2 large, 3 extra large
    int result_limit{1000};
    bool regex_mode{};
    bool case_sensitive{};
    bool whole_word{};
    bool match_path{};
    bool match_diacritics{};
    bool window_valid{};
    int window_x{};
    int window_y{};
    int window_width{1180};
    int window_height{720};
    bool maximized{};
};

[[nodiscard]] GuiSettings default_gui_settings();
void normalize_gui_settings(GuiSettings& settings);
[[nodiscard]] std::string serialize_gui_settings(const GuiSettings& settings);
[[nodiscard]] bool parse_gui_settings(std::string_view text,
                                      GuiSettings& settings);
} // namespace esm
