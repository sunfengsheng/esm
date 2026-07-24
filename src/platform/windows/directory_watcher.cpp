#include "esm/directory_watcher.hpp"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <mutex>
#include <utility>

namespace esm {
namespace {
constexpr std::size_t minimum_buffer_bytes = 4 * 1024;
constexpr std::size_t maximum_buffer_bytes = 64 * 1024;
constexpr DWORD notification_filter =
    FILE_NOTIFY_CHANGE_FILE_NAME |
    FILE_NOTIFY_CHANGE_DIR_NAME |
    FILE_NOTIFY_CHANGE_ATTRIBUTES |
    FILE_NOTIFY_CHANGE_SIZE |
    FILE_NOTIFY_CHANGE_LAST_WRITE |
    FILE_NOTIFY_CHANGE_CREATION;

struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() {
        if (value != nullptr && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(std::exchange(
        other.value, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this == &other) return *this;
        if (value != nullptr && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
        value = std::exchange(other.value, INVALID_HANDLE_VALUE);
        return *this;
    }
};

bool map_action(DWORD value, DirectoryChangeAction& action) noexcept {
    switch (value) {
    case FILE_ACTION_ADDED:
        action = DirectoryChangeAction::added;
        return true;
    case FILE_ACTION_REMOVED:
        action = DirectoryChangeAction::removed;
        return true;
    case FILE_ACTION_MODIFIED:
        action = DirectoryChangeAction::modified;
        return true;
    case FILE_ACTION_RENAMED_OLD_NAME:
        action = DirectoryChangeAction::renamed_old_name;
        return true;
    case FILE_ACTION_RENAMED_NEW_NAME:
        action = DirectoryChangeAction::renamed_new_name;
        return true;
    default:
        return false;
    }
}
} // namespace

struct DirectoryWatcher::Impl {
    std::filesystem::path root;
    std::vector<std::byte> buffer;
    Handle directory;
    Handle completion_event;
    Handle stop_event;
    std::uint32_t startup_error{};
    std::mutex wait_mutex;

    Impl(std::filesystem::path requested_root, std::size_t buffer_bytes) {
        std::error_code ec;
        root = std::filesystem::absolute(requested_root, ec);
        if (ec) {
            startup_error = static_cast<std::uint32_t>(ec.value());
            return;
        }
        root = root.lexically_normal();
        const auto attributes = GetFileAttributesW(root.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            startup_error = GetLastError();
            return;
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            startup_error = ERROR_DIRECTORY;
            return;
        }

        buffer_bytes = (std::max)(minimum_buffer_bytes,
                                  (std::min)(maximum_buffer_bytes,
                                             buffer_bytes));
        buffer.resize(buffer_bytes);
        directory = Handle(CreateFileW(
            root.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr));
        if (directory.value == INVALID_HANDLE_VALUE) {
            startup_error = GetLastError();
            return;
        }
        completion_event = Handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (completion_event.value == nullptr) {
            startup_error = GetLastError();
            return;
        }
        stop_event = Handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (stop_event.value == nullptr) {
            startup_error = GetLastError();
            return;
        }
    }

    [[nodiscard]] bool ready() const noexcept {
        return startup_error == ERROR_SUCCESS &&
               directory.value != INVALID_HANDLE_VALUE &&
               completion_event.value != nullptr &&
               stop_event.value != nullptr;
    }

    DirectoryWatchResult wait(std::stop_token token) {
        std::lock_guard wait_lock(wait_mutex);
        DirectoryWatchResult result;
        if (!ready()) {
            result.error = startup_error == ERROR_SUCCESS
                ? ERROR_INVALID_HANDLE : startup_error;
            return result;
        }
        if (token.stop_requested()) {
            result.ok = true;
            result.stopped = true;
            return result;
        }

        ResetEvent(completion_event.value);
        ResetEvent(stop_event.value);
        OVERLAPPED overlapped{};
        overlapped.hEvent = completion_event.value;
        const auto issued = ReadDirectoryChangesW(
            directory.value, buffer.data(),
            static_cast<DWORD>(buffer.size()), TRUE,
            notification_filter, nullptr, &overlapped, nullptr);
        if (!issued) {
            const auto error = GetLastError();
            if (error != ERROR_IO_PENDING) {
                result.error = error;
                return result;
            }
        }

        std::stop_callback stop_callback(token, [event = stop_event.value] {
            SetEvent(event);
        });
        HANDLE events[] = {completion_event.value, stop_event.value};
        const auto wait_result = WaitForMultipleObjects(
            static_cast<DWORD>(std::size(events)), events, FALSE, INFINITE);
        if (wait_result == WAIT_OBJECT_0 + 1) {
            CancelIoEx(directory.value, &overlapped);
            DWORD ignored = 0;
            (void)GetOverlappedResult(directory.value, &overlapped,
                                      &ignored, TRUE);
            result.ok = true;
            result.stopped = true;
            result.error = ERROR_SUCCESS;
            return result;
        }
        if (wait_result != WAIT_OBJECT_0) {
            const auto error = wait_result == WAIT_FAILED
                ? GetLastError() : ERROR_INVALID_DATA;
            CancelIoEx(directory.value, &overlapped);
            DWORD ignored = 0;
            (void)GetOverlappedResult(directory.value, &overlapped,
                                      &ignored, TRUE);
            result.error = error;
            return result;
        }

        DWORD transferred = 0;
        if (!GetOverlappedResult(directory.value, &overlapped,
                                 &transferred, FALSE)) {
            const auto error = GetLastError();
            if (error == ERROR_NOTIFY_ENUM_DIR) {
                result.ok = true;
                result.overflowed = true;
                return result;
            }
            if (error == ERROR_OPERATION_ABORTED && token.stop_requested()) {
                result.ok = true;
                result.stopped = true;
                return result;
            }
            result.error = error;
            return result;
        }
        if (transferred == 0) {
            result.ok = true;
            result.overflowed = true;
            return result;
        }
        if (transferred > buffer.size()) {
            result.error = ERROR_INVALID_DATA;
            return result;
        }

        std::size_t offset = 0;
        constexpr auto prefix_size = offsetof(FILE_NOTIFY_INFORMATION, FileName);
        while (offset < transferred) {
            if (transferred - offset < prefix_size) {
                result.error = ERROR_INVALID_DATA;
                return result;
            }
            const auto* item = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(
                buffer.data() + offset);
            if ((item->FileNameLength % sizeof(wchar_t)) != 0) {
                result.error = ERROR_INVALID_DATA;
                return result;
            }
            const auto name_bytes = static_cast<std::size_t>(
                item->FileNameLength);
            if (name_bytes > transferred - offset - prefix_size) {
                result.error = ERROR_INVALID_DATA;
                return result;
            }
            DirectoryChangeAction action;
            if (!map_action(item->Action, action)) {
                result.error = ERROR_INVALID_DATA;
                return result;
            }
            const auto characters = name_bytes / sizeof(wchar_t);
            std::wstring relative(item->FileName, characters);
            result.changes.push_back({action,
                (root / std::filesystem::path(relative)).lexically_normal()});

            if (item->NextEntryOffset == 0) {
                offset = transferred;
                break;
            }
            if (item->NextEntryOffset < prefix_size ||
                item->NextEntryOffset > transferred - offset) {
                result.error = ERROR_INVALID_DATA;
                return result;
            }
            offset += item->NextEntryOffset;
        }
        result.ok = true;
        result.error = ERROR_SUCCESS;
        return result;
    }
};

DirectoryWatcher::DirectoryWatcher(std::filesystem::path root,
                                   std::size_t buffer_bytes)
    : impl_(std::make_unique<Impl>(std::move(root), buffer_bytes)) {}
DirectoryWatcher::~DirectoryWatcher() = default;
DirectoryWatcher::DirectoryWatcher(DirectoryWatcher&&) noexcept = default;
DirectoryWatcher& DirectoryWatcher::operator=(DirectoryWatcher&&) noexcept = default;

bool DirectoryWatcher::ready() const noexcept {
    return impl_ && impl_->ready();
}
std::uint32_t DirectoryWatcher::error() const noexcept {
    return impl_ ? impl_->startup_error : ERROR_INVALID_HANDLE;
}
const std::filesystem::path& DirectoryWatcher::root() const noexcept {
    static const std::filesystem::path empty;
    return impl_ ? impl_->root : empty;
}
DirectoryWatchResult DirectoryWatcher::wait(std::stop_token stop_token) {
    if (!impl_) {
        DirectoryWatchResult result;
        result.error = ERROR_INVALID_HANDLE;
        return result;
    }
    return impl_->wait(stop_token);
}
} // namespace esm
