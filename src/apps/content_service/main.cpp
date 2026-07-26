#include "esm/content_index.hpp"
#include "esm/content_named_pipe.hpp"
#include "esm/directory_watcher.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace {
std::atomic_bool stop_requested{false};

BOOL WINAPI console_handler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT ||
        signal == CTRL_CLOSE_EVENT || signal == CTRL_SHUTDOWN_EVENT) {
        stop_requested.store(true, std::memory_order_relaxed);
        return TRUE;
    }
    return FALSE;
}

std::filesystem::path default_database_path() {
    wchar_t buffer[32768]{};
    const DWORD length = GetEnvironmentVariableW(
        L"PROGRAMDATA", buffer, static_cast<DWORD>(std::size(buffer)));
    if (length != 0 && length < std::size(buffer)) {
        return std::filesystem::path(buffer) / L"everything_sm" / L"content" /
               L"xapian";
    }
    return std::filesystem::temp_directory_path() / L"everything_sm-content" /
           L"xapian";
}

bool index_path(esm::ContentIndex& index, const std::filesystem::path& path,
                std::size_t maximum_bytes, std::size_t& indexed,
                std::size_t& skipped) {
    if (!esm::is_supported_content_path(path)) {
        ++skipped;
        return false;
    }
    esm::ContentDocument document;
    std::wstring error;
    if (!esm::extract_plain_text_file(path, maximum_bytes, document, error)) {
        ++skipped;
        return false;
    }
    index.upsert(document);
    ++indexed;
    return true;
}

void initial_scan(esm::ContentIndex& index, const std::filesystem::path& root,
                  std::size_t maximum_bytes) {
    index.set_indexing(true, L"正在建立纯文本内容索引");
    std::size_t indexed{};
    std::size_t skipped{};
    std::size_t pending_commit{};
    std::error_code error;
    const auto options = std::filesystem::directory_options::skip_permission_denied;
    for (std::filesystem::recursive_directory_iterator iterator(root, options, error),
         end;
         iterator != end && !stop_requested.load(std::memory_order_relaxed);
         iterator.increment(error)) {
        if (error) {
            ++skipped;
            error.clear();
            continue;
        }
        if (!iterator->is_regular_file(error)) {
            error.clear();
            continue;
        }
        if (index_path(index, iterator->path(), maximum_bytes, indexed, skipped)) {
            ++pending_commit;
            if (pending_commit >= 500) {
                index.commit();
                pending_commit = 0;
                index.set_indexing(
                    true, L"正在建立内容索引，已处理 " +
                              std::to_wstring(indexed) + L" 个文件");
            }
        }
    }
    index.commit();
    index.set_indexing(false,
                       L"内容索引就绪：" + std::to_wstring(indexed) +
                           L" 个文件，跳过 " + std::to_wstring(skipped));
    std::wcout << L"Initial content scan: indexed=" << indexed
               << L", skipped=" << skipped << L"\n";
}

void watch_root(esm::ContentIndex& index, const std::filesystem::path& root,
                std::size_t maximum_bytes, std::stop_token stop_token) {
    esm::DirectoryWatcher watcher(root);
    if (!watcher.ready()) {
        index.set_indexing(false,
                           L"内容索引就绪，但目录监视启动失败，错误码 " +
                               std::to_wstring(watcher.error()));
        return;
    }
    while (!stop_requested.load(std::memory_order_relaxed) &&
           !stop_token.stop_requested()) {
        const auto result = watcher.wait(stop_token);
        if (stop_requested.load(std::memory_order_relaxed) ||
            stop_token.stop_requested()) break;
        if (!result.ok) {
            if (result.stopped) break;
            index.set_indexing(false,
                               L"目录监视失败，错误码 " +
                                   std::to_wstring(result.error));
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        std::size_t indexed{};
        std::size_t skipped{};
        bool changed = false;
        for (const auto& change : result.changes) {
            try {
                if (change.action == esm::DirectoryChangeAction::removed ||
                    change.action ==
                        esm::DirectoryChangeAction::renamed_old_name) {
                    if (esm::is_supported_content_path(change.path)) {
                        index.remove(change.path);
                        changed = true;
                    }
                } else if (change.action == esm::DirectoryChangeAction::added ||
                           change.action == esm::DirectoryChangeAction::modified ||
                           change.action ==
                               esm::DirectoryChangeAction::renamed_new_name) {
                    if (index_path(index, change.path, maximum_bytes, indexed,
                                   skipped)) changed = true;
                }
            } catch (const std::exception&) {
                ++skipped;
            }
        }
        if (changed) index.commit();
        if (result.overflowed) {
            index.set_indexing(false,
                               L"目录通知溢出；当前原型需要重启服务进行校准");
        }
    }
}

void print_usage() {
    std::wcout
        << L"Usage:\n"
        << L"  esm_content_service --root <directory> [--db <directory>]\n"
        << L"                      [--pipe <name>] [--max-mib <1-64>]\n\n"
        << L"Example:\n"
        << L"  esm_content_service --root D:\\work --pipe "
           L"everything_sm_content\n";
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    std::filesystem::path root;
    auto database = default_database_path();
    std::wstring pipe = L"everything_sm_content";
    std::size_t maximum_bytes = 4 * 1024 * 1024;

    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        if (argument == L"--root" && index + 1 < argc) {
            root = argv[++index];
        } else if (argument == L"--db" && index + 1 < argc) {
            database = argv[++index];
        } else if (argument == L"--pipe" && index + 1 < argc) {
            pipe = argv[++index];
        } else if (argument == L"--max-mib" && index + 1 < argc) {
            try {
                const auto mib = std::stoull(argv[++index]);
                if (mib == 0 || mib > 64) {
                    print_usage();
                    return 2;
                }
                maximum_bytes = static_cast<std::size_t>(mib) * 1024 * 1024;
            } catch (const std::exception&) {
                print_usage();
                return 2;
            }
        } else if (argument == L"--help" || argument == L"-h") {
            print_usage();
            return 0;
        } else {
            print_usage();
            return 2;
        }
    }
    if (root.empty()) {
        print_usage();
        return 2;
    }

    std::error_code error;
    root = std::filesystem::absolute(root, error).lexically_normal();
    if (error || !std::filesystem::is_directory(root, error)) {
        std::wcerr << L"Invalid content root: " << root << L"\n";
        return 2;
    }

    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    SetConsoleCtrlHandler(console_handler, TRUE);
    try {
        esm::XapianContentIndex index(database);
        initial_scan(index, root, maximum_bytes);
        if (stop_requested.load(std::memory_order_relaxed)) return 0;

        std::jthread watcher([&](std::stop_token token) {
            watch_root(index, root, maximum_bytes, token);
        });
        std::wcout << L"Content service ready\n"
                   << L"  root: " << root << L"\n"
                   << L"  db:   " << database << L"\n"
                   << L"  pipe: " << pipe << L"\n";
        const auto pipe_error = esm::serve_content_named_pipe(
            pipe, index, stop_requested);
        stop_requested.store(true, std::memory_order_relaxed);
        watcher.request_stop();
        if (watcher.joinable()) watcher.join();
        if (pipe_error != ERROR_SUCCESS) {
            std::wcerr << L"Content pipe stopped with error " << pipe_error
                       << L"\n";
            return 1;
        }
    } catch (const std::exception& exception) {
        std::cerr << "Content service failed: " << exception.what() << "\n";
        return 1;
    }
    return 0;
}
