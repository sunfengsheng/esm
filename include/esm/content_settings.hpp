#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace esm {
inline constexpr wchar_t default_content_pipe_name[] =
    L"everything_sm_content_service";
inline constexpr std::size_t default_content_maximum_bytes =
    4U * 1024U * 1024U;

struct ContentAppSettings {
    std::vector<std::filesystem::path> roots;
    std::vector<std::filesystem::path> excluded_paths;
    std::filesystem::path database_root;
    std::wstring pipe_name{default_content_pipe_name};
    std::size_t maximum_bytes{default_content_maximum_bytes};
    bool all_fixed{};
    bool use_default_excludes{true};
};

[[nodiscard]] std::filesystem::path default_content_app_data_root();
[[nodiscard]] std::filesystem::path default_content_config_path();
[[nodiscard]] ContentAppSettings default_content_app_settings();
void normalize_content_app_settings(ContentAppSettings& settings);
[[nodiscard]] bool load_content_app_settings(
    const std::filesystem::path& path,
    ContentAppSettings& settings,
    std::wstring& error);
[[nodiscard]] bool save_content_app_settings(
    const std::filesystem::path& path,
    const ContentAppSettings& settings,
    std::wstring& error);
} // namespace esm
