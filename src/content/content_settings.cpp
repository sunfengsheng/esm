#include "esm/content_settings.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <limits>
#include <system_error>

namespace esm {
namespace {
constexpr wchar_t settings_section[] = L"content";
constexpr int settings_schema_version = 1;
constexpr std::size_t maximum_roots = 128;
constexpr std::size_t maximum_excludes = 512;
constexpr std::size_t maximum_indexed_file_bytes = 64U * 1024U * 1024U;

std::filesystem::path environment_path(std::wstring_view name) {
    std::array<wchar_t, 32768> buffer{};
    const auto length = GetEnvironmentVariableW(
        std::wstring(name).c_str(), buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    return std::filesystem::path(std::wstring_view(buffer.data(), length));
}

std::wstring read_string(const std::filesystem::path& path,
                         std::wstring_view key,
                         std::wstring_view fallback = {}) {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const auto copied = GetPrivateProfileStringW(
            settings_section, std::wstring(key).c_str(),
            std::wstring(fallback).c_str(), buffer.data(),
            static_cast<DWORD>(buffer.size()), path.c_str());
        if (copied + 1 < buffer.size() || buffer.size() >= 32768) {
            return std::wstring(buffer.data(), copied);
        }
        buffer.resize((std::min<std::size_t>)(buffer.size() * 2, 32768));
    }
}

bool write_string(const std::filesystem::path& path, std::wstring_view key,
                  std::wstring_view value) {
    return WritePrivateProfileStringW(settings_section,
                                      std::wstring(key).c_str(),
                                      std::wstring(value).c_str(),
                                      path.c_str()) != FALSE;
}

std::size_t read_count(const std::filesystem::path& path,
                       std::wstring_view key, std::size_t maximum) {
    const auto value = GetPrivateProfileIntW(
        settings_section, std::wstring(key).c_str(), 0, path.c_str());
    return (std::min<std::size_t>)(value, maximum);
}

bool write_path_list(const std::filesystem::path& path,
                     std::wstring_view count_key,
                     std::wstring_view item_prefix,
                     const std::vector<std::filesystem::path>& values) {
    if (!write_string(path, count_key, std::to_wstring(values.size())))
        return false;
    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto key = std::wstring(item_prefix) + std::to_wstring(index);
        if (!write_string(path, key, values[index].wstring())) return false;
    }
    return true;
}

std::vector<std::filesystem::path> read_path_list(
    const std::filesystem::path& path, std::wstring_view count_key,
    std::wstring_view item_prefix, std::size_t maximum) {
    const auto count = read_count(path, count_key, maximum);
    std::vector<std::filesystem::path> values;
    values.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto key = std::wstring(item_prefix) + std::to_wstring(index);
        auto value = read_string(path, key);
        if (!value.empty()) values.emplace_back(std::move(value));
    }
    return values;
}
} // namespace

std::filesystem::path default_content_app_data_root() {
    auto local_app_data = environment_path(L"LOCALAPPDATA");
    if (!local_app_data.empty())
        return local_app_data / L"everything_sm_content";
    return std::filesystem::temp_directory_path() / L"everything_sm_content";
}

std::filesystem::path default_content_config_path() {
    return default_content_app_data_root() / L"content.ini";
}

ContentAppSettings default_content_app_settings() {
    ContentAppSettings settings;
    settings.database_root = default_content_app_data_root() / L"index";
    const auto profile = environment_path(L"USERPROFILE");
    if (!profile.empty()) settings.roots.push_back(profile);
    return settings;
}

void normalize_content_app_settings(ContentAppSettings& settings) {
    if (settings.database_root.empty())
        settings.database_root = default_content_app_data_root() / L"index";
    if (settings.pipe_name.empty()) settings.pipe_name = default_content_pipe_name;
    settings.maximum_bytes = std::clamp(
        settings.maximum_bytes, static_cast<std::size_t>(1024),
        maximum_indexed_file_bytes);
    if (settings.roots.size() > maximum_roots)
        settings.roots.resize(maximum_roots);
    if (settings.excluded_paths.size() > maximum_excludes)
        settings.excluded_paths.resize(maximum_excludes);
}

bool load_content_app_settings(const std::filesystem::path& path,
                               ContentAppSettings& settings,
                               std::wstring& error) {
    error.clear();
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(path, filesystem_error)) {
        error = L"内容搜索配置文件不存在：" + path.wstring();
        return false;
    }
    const auto version = GetPrivateProfileIntW(
        settings_section, L"version", 0, path.c_str());
    if (version != settings_schema_version) {
        error = L"不支持的内容搜索配置版本：" + std::to_wstring(version);
        return false;
    }

    auto loaded = default_content_app_settings();
    loaded.roots = read_path_list(path, L"root_count", L"root", maximum_roots);
    loaded.excluded_paths = read_path_list(
        path, L"exclude_count", L"exclude", maximum_excludes);
    const auto database_root = read_string(path, L"database_root");
    if (!database_root.empty()) loaded.database_root = database_root;
    loaded.pipe_name = read_string(path, L"pipe_name", default_content_pipe_name);
    loaded.maximum_bytes = static_cast<std::size_t>(GetPrivateProfileIntW(
        settings_section, L"maximum_bytes",
        static_cast<INT>(default_content_maximum_bytes), path.c_str()));
    loaded.all_fixed = GetPrivateProfileIntW(
                           settings_section, L"all_fixed", 0, path.c_str()) != 0;
    loaded.use_default_excludes =
        GetPrivateProfileIntW(settings_section, L"use_default_excludes", 1,
                              path.c_str()) != 0;
    normalize_content_app_settings(loaded);
    settings = std::move(loaded);
    return true;
}

bool save_content_app_settings(const std::filesystem::path& path,
                               const ContentAppSettings& requested,
                               std::wstring& error) {
    error.clear();
    auto settings = requested;
    normalize_content_app_settings(settings);
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code filesystem_error;
        std::filesystem::create_directories(parent, filesystem_error);
        if (filesystem_error) {
            error = L"无法创建内容搜索配置目录：" + parent.wstring();
            return false;
        }
    }

    if (!write_string(path, L"version", std::to_wstring(settings_schema_version)) ||
        !write_string(path, L"database_root", settings.database_root.wstring()) ||
        !write_string(path, L"pipe_name", settings.pipe_name) ||
        !write_string(path, L"maximum_bytes",
                      std::to_wstring(settings.maximum_bytes)) ||
        !write_string(path, L"all_fixed", settings.all_fixed ? L"1" : L"0") ||
        !write_string(path, L"use_default_excludes",
                      settings.use_default_excludes ? L"1" : L"0") ||
        !write_path_list(path, L"root_count", L"root", settings.roots) ||
        !write_path_list(path, L"exclude_count", L"exclude",
                         settings.excluded_paths)) {
        error = L"无法写入内容搜索配置文件：" + path.wstring();
        return false;
    }
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    return true;
}
} // namespace esm
