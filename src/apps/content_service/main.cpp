#include "esm/content_index.hpp"
#include "esm/content_named_pipe.hpp"
#include "esm/content_roots.hpp"
#include "esm/content_settings.hpp"
#include "esm/directory_watcher.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

std::filesystem::path normalize_path(const std::filesystem::path& path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error) absolute = path;
    return absolute.lexically_normal();
}

std::wstring lower_path(std::wstring value) {
    if (value.empty()) return value;
    const int required = LCMapStringEx(
        LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr, 0);
    if (required <= 0) return value;
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(),
                      static_cast<int>(value.size()), result.data(), required,
                      nullptr, nullptr, 0) != required) return value;
    return result;
}

bool path_is_within(const std::filesystem::path& path,
                    const std::filesystem::path& root) {
    const auto normalized = lower_path(normalize_path(path).wstring());
    auto prefix = lower_path(normalize_path(root).wstring());
    if (normalized == prefix) return true;
    if (!prefix.empty() && prefix.back() != L'\\' && prefix.back() != L'/')
        prefix.push_back(L'\\');
    return normalized.size() > prefix.size() &&
           normalized.compare(0, prefix.size(), prefix) == 0;
}

std::vector<std::filesystem::path> normalize_roots(
    std::vector<std::filesystem::path> roots) {
    for (auto& root : roots) root = normalize_path(root);
    std::sort(roots.begin(), roots.end(), [](const auto& left, const auto& right) {
        const auto left_text = lower_path(left.wstring());
        const auto right_text = lower_path(right.wstring());
        if (left_text.size() != right_text.size())
            return left_text.size() < right_text.size();
        return left_text < right_text;
    });
    std::vector<std::filesystem::path> result;
    for (auto& root : roots) {
        bool covered = false;
        for (const auto& existing : result) {
            if (path_is_within(root, existing)) {
                covered = true;
                break;
            }
        }
        if (!covered) result.push_back(std::move(root));
    }
    return result;
}

bool index_path(esm::ContentIndex& index, const esm::ContentPathFilter& filter,
                const std::filesystem::path& path,
                std::size_t maximum_bytes, std::size_t& indexed,
                std::size_t& skipped) {
    if (filter.excluded(path) || !esm::is_supported_content_path(path)) {
        ++skipped;
        return false;
    }
    esm::ContentDocument document;
    std::wstring error;
    if (!esm::extract_content_file(path, maximum_bytes, document, error)) {
        ++skipped;
        return false;
    }
    index.upsert(document);
    ++indexed;
    return true;
}

bool initial_scan(esm::ContentIndex& index, const std::filesystem::path& root,
                  const esm::ContentPathFilter& filter,
                  std::size_t maximum_bytes) {
    index.set_indexing(true, L"正在扫描 " + root.wstring());
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
        const auto path = iterator->path();
        if (filter.excluded(path)) {
            if (iterator->is_directory(error)) iterator.disable_recursion_pending();
            error.clear();
            ++skipped;
            continue;
        }
        if (!iterator->is_regular_file(error)) {
            error.clear();
            continue;
        }
        if (index_path(index, filter, path, maximum_bytes, indexed, skipped)) {
            ++pending_commit;
            if (pending_commit >= 500) {
                index.commit();
                pending_commit = 0;
                index.set_indexing(
                    true, L"正在扫描 " + root.wstring() + L"，已索引 " +
                              std::to_wstring(indexed) + L" 个文件");
            }
        }
    }
    index.commit();
    if (stop_requested.load(std::memory_order_relaxed)) return false;
    index.set_indexing(false,
                       L"内容索引就绪：" + std::to_wstring(indexed) +
                           L" 个文件，跳过 " + std::to_wstring(skipped));
    std::wcout << L"Initial content scan: root=" << root
               << L", indexed=" << indexed << L", skipped=" << skipped
               << L"\n";
    return true;
}

void watch_root(esm::ContentIndex& index, const std::filesystem::path& root,
                const esm::ContentPathFilter& filter,
                std::size_t maximum_bytes, std::stop_token stop_token) {
    esm::DirectoryWatcher watcher(root);
    if (!watcher.ready()) {
        index.set_indexing(false,
                           L"内容索引可查询，但目录监视启动失败，错误码 " +
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
            if (filter.excluded(change.path)) continue;
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
                    if (index_path(index, filter, change.path, maximum_bytes,
                                   indexed, skipped)) changed = true;
                }
            } catch (const std::exception&) {
                ++skipped;
            }
        }
        if (changed) index.commit();
        if (result.overflowed) {
            index.set_indexing(false,
                               L"目录通知溢出；当前版本需要重启内容服务校准");
        }
    }
}

void scan_and_watch(esm::ContentIndex& index,
                    const std::filesystem::path& root,
                    esm::ContentPathFilter filter,
                    std::size_t maximum_bytes, std::stop_token stop_token) {
    try {
        if (!initial_scan(index, root, filter, maximum_bytes)) return;
        watch_root(index, root, filter, maximum_bytes, stop_token);
    } catch (const std::exception& exception) {
        std::cerr << "Content worker failed for " << root.string() << ": "
                  << exception.what() << "\n";
        index.set_indexing(false, L"内容索引工作线程失败，请查看服务日志");
    } catch (...) {
        std::cerr << "Content worker failed for " << root.string()
                  << ": unknown non-standard exception\n";
        index.set_indexing(
            false,
            L"\u5185\u5bb9\u7d22\u5f15\u5de5\u4f5c\u7ebf\u7a0b\u53d1\u751f\u672a\u8bc6\u522b\u9519\u8bef\uff0c\u8bf7\u67e5\u770b\u670d\u52a1\u65e5\u5fd7");
    }
}

void print_usage() {
    std::wcout
        << L"Usage:\n"
        << L"  esm_content_service [--config <content.ini>]\n"
        << L"  esm_content_service --root <directory> [--root <directory> ...]\n"
        << L"  esm_content_service --all-fixed\n"
        << L"      [--db <directory> | --db-root <directory>]\n"
        << L"      [--exclude <path> ...] [--no-default-excludes]\n"
        << L"      [--config <content.ini>] [--pipe <name>]\n"
        << L"      [--max-mib <1-64>]\n\n"
        << L"Notes:\n"
        << L"  With no root arguments, settings are loaded from --config or\n"
        << L"  %LOCALAPPDATA%\\everything_sm_content\\content.ini.\n"
        << L"  --db is only valid for one root. Multi-root mode creates one\n"
        << L"  Xapian database per root below --db-root\\volumes.\n\n"
        << L"Examples:\n"
        << L"  esm_content_service --root D:\\work\n"
        << L"  esm_content_service --all-fixed --db-root D:\\esm-content\n";
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    auto config_path = esm::default_content_config_path();
    bool explicit_config = false;
    for (int argument_index = 1; argument_index < argc; ++argument_index) {
        if (std::wstring_view(argv[argument_index]) == L"--config" &&
            argument_index + 1 < argc) {
            config_path = argv[++argument_index];
            explicit_config = true;
        }
    }

    auto settings = esm::default_content_app_settings();
    std::wstring settings_error;
    if (std::filesystem::exists(config_path)) {
        if (!esm::load_content_app_settings(config_path, settings,
                                            settings_error)) {
            std::wcerr << settings_error << L"\n";
            return 2;
        }
    } else if (explicit_config) {
        std::wcerr << L"Content configuration does not exist: "
                   << config_path << L"\n";
        return 2;
    } else if (!esm::save_content_app_settings(config_path, settings,
                                               settings_error)) {
        std::wcerr << settings_error << L"\n";
        return 2;
    }

    auto roots = settings.roots;
    auto excluded_paths = settings.excluded_paths;
    std::filesystem::path direct_database;
    auto database_root = settings.database_root;
    bool direct_database_set = false;
    bool database_root_set = false;
    bool roots_overridden = false;
    bool all_fixed = settings.all_fixed;
    bool use_default_excludes = settings.use_default_excludes;
    std::wstring pipe = settings.pipe_name;
    std::size_t maximum_bytes = settings.maximum_bytes;

    for (int argument_index = 1; argument_index < argc; ++argument_index) {
        const std::wstring argument = argv[argument_index];
        if (argument == L"--config" && argument_index + 1 < argc) {
            ++argument_index;
        } else if (argument == L"--root" && argument_index + 1 < argc) {
            if (!roots_overridden) {
                roots.clear();
                all_fixed = false;
                roots_overridden = true;
            }
            roots.emplace_back(argv[++argument_index]);
        } else if (argument == L"--all-fixed") {
            all_fixed = true;
        } else if (argument == L"--db" && argument_index + 1 < argc) {
            direct_database = argv[++argument_index];
            direct_database_set = true;
        } else if (argument == L"--db-root" && argument_index + 1 < argc) {
            database_root = argv[++argument_index];
            database_root_set = true;
        } else if (argument == L"--exclude" && argument_index + 1 < argc) {
            excluded_paths.emplace_back(argv[++argument_index]);
        } else if (argument == L"--no-default-excludes") {
            use_default_excludes = false;
        } else if (argument == L"--pipe" && argument_index + 1 < argc) {
            pipe = argv[++argument_index];
        } else if (argument == L"--max-mib" && argument_index + 1 < argc) {
            try {
                const auto mib = std::stoull(argv[++argument_index]);
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

    if (direct_database_set && database_root_set) {
        std::wcerr << L"--db and --db-root cannot be used together\n";
        return 2;
    }
    if (all_fixed) {
        auto discovered = esm::discover_fixed_content_roots();
        roots.insert(roots.end(), discovered.begin(), discovered.end());
    }
    roots = normalize_roots(std::move(roots));
    if (roots.empty()) {
        std::wcerr << L"No content roots are configured in " << config_path
                   << L"\n";
        print_usage();
        return 2;
    }
    if (roots.size() > 1 && direct_database_set) {
        std::wcerr << L"--db is only valid when exactly one root is indexed\n";
        return 2;
    }

    for (const auto& root : roots) {
        std::error_code error;
        if (!std::filesystem::is_directory(root, error) || error) {
            std::wcerr << L"Invalid content root: " << root << L"\n";
            return 2;
        }
    }

    database_root = normalize_path(database_root);
    std::vector<esm::ContentIndexShardSpec> shard_specs;
    shard_specs.reserve(roots.size());
    for (const auto& root : roots) {
        auto database = direct_database_set
                            ? normalize_path(direct_database)
                            : database_root / L"volumes" /
                                  esm::content_root_database_key(root) /
                                  L"xapian";
        shard_specs.push_back({root, std::move(database)});
    }

    auto mutex_name = std::wstring(L"Local\\EverythingSmContentService-") + pipe;
    std::replace(mutex_name.begin(), mutex_name.end(), L'\\', L'_');
    const HANDLE instance_mutex = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
    if (!instance_mutex) {
        std::wcerr << L"Unable to create content service instance mutex\n";
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(instance_mutex);
        return 0;
    }

    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    SetConsoleCtrlHandler(console_handler, TRUE);
    int exit_code = 0;
    try {
        esm::ShardedContentIndex index(shard_specs);
        std::vector<std::jthread> workers;
        workers.reserve(index.shard_count());
        for (std::size_t shard_index = 0; shard_index < index.shard_count();
             ++shard_index) {
            auto root_excludes = excluded_paths;
            for (const auto& spec : shard_specs) {
                if (path_is_within(spec.database_path,
                                   index.shard_root(shard_index))) {
                    root_excludes.push_back(spec.database_path);
                }
            }
            esm::ContentPathFilter filter(index.shard_root(shard_index),
                                          use_default_excludes,
                                          std::move(root_excludes));
            index.shard(shard_index)
                .set_indexing(true, L"\u7b49\u5f85\u540e\u53f0\u626b\u63cf " +
                                        index.shard_root(shard_index).wstring());
            workers.emplace_back(
                [&index, shard_index, filter = std::move(filter),
                 maximum_bytes](std::stop_token token) mutable {
                    scan_and_watch(index.shard(shard_index),
                                   index.shard_root(shard_index),
                                   std::move(filter), maximum_bytes, token);
                });
        }

        std::wcout << L"Content service started; initial scans run in background\n"
                   << L"  config: " << config_path << L"\n"
                   << L"  pipe:   " << pipe << L"\n";
        for (std::size_t shard_index = 0; shard_index < shard_specs.size();
             ++shard_index) {
            std::wcout << L"  root: " << shard_specs[shard_index].root << L"\n"
                       << L"  db:   " << shard_specs[shard_index].database_path
                       << L"\n";
        }

        const auto pipe_error =
            esm::serve_content_named_pipe(pipe, index, stop_requested);
        stop_requested.store(true, std::memory_order_relaxed);
        for (auto& worker : workers) worker.request_stop();
        for (auto& worker : workers) {
            if (worker.joinable()) worker.join();
        }
        if (pipe_error != ERROR_SUCCESS) {
            std::wcerr << L"Content pipe stopped with error " << pipe_error
                       << L"\n";
            exit_code = 1;
        }
    } catch (const std::exception& exception) {
        std::cerr << "Content service failed: " << exception.what() << "\n";
        exit_code = 1;
    } catch (...) {
        std::cerr << "Content service failed: unknown non-standard exception\n";
        exit_code = 1;
    }
    CloseHandle(instance_mutex);
    return exit_code;
}
