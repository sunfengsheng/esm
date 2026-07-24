#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <vector>

namespace esm {
enum class DirectoryChangeAction {
    added,
    removed,
    modified,
    renamed_old_name,
    renamed_new_name,
};

struct DirectoryChange {
    DirectoryChangeAction action{DirectoryChangeAction::modified};
    std::filesystem::path path;
};

struct DirectoryWatchResult {
    bool ok{};
    bool stopped{};
    bool overflowed{};
    std::uint32_t error{};
    std::vector<DirectoryChange> changes;
};

// Recursive Windows directory notification provider. One thread may call
// wait() at a time. A stop request cancels the outstanding overlapped read.
class DirectoryWatcher {
public:
    explicit DirectoryWatcher(
        std::filesystem::path root,
        std::size_t buffer_bytes = 64 * 1024);
    ~DirectoryWatcher();

    DirectoryWatcher(const DirectoryWatcher&) = delete;
    DirectoryWatcher& operator=(const DirectoryWatcher&) = delete;
    DirectoryWatcher(DirectoryWatcher&&) noexcept;
    DirectoryWatcher& operator=(DirectoryWatcher&&) noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::uint32_t error() const noexcept;
    [[nodiscard]] const std::filesystem::path& root() const noexcept;
    [[nodiscard]] DirectoryWatchResult wait(std::stop_token stop_token);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace esm
