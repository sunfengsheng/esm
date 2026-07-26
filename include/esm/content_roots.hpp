#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace esm {
class ContentPathFilter {
public:
    ContentPathFilter(std::filesystem::path root,
                      bool use_default_excludes = true,
                      std::vector<std::filesystem::path> excluded_paths = {});

    [[nodiscard]] const std::filesystem::path& root() const noexcept;
    [[nodiscard]] bool excluded(const std::filesystem::path& path) const;

private:
    std::filesystem::path root_;
    std::wstring lower_root_;
    bool use_default_excludes_{};
    std::vector<std::wstring> lower_excluded_paths_;
};

[[nodiscard]] std::vector<std::filesystem::path>
discover_fixed_content_roots();

[[nodiscard]] std::wstring content_root_database_key(
    const std::filesystem::path& root);
} // namespace esm
