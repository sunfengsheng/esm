#include "esm/content_named_pipe.hpp"
#include "esm/content_settings.hpp"

#include <windows.h>

#include <iostream>
#include <string>

namespace {
void usage() {
    std::wcout
        << L"Usage:\n"
        << L"  esm_content_cli [--config <path>] [--pipe <name>] status\n"
        << L"  esm_content_cli [--config <path>] [--pipe <name>] search <query> [limit]\n";
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    auto config_path = esm::default_content_config_path();
    bool explicit_config = false;
    for (int argument_index = 1; argument_index + 1 < argc; ++argument_index) {
        if (std::wstring_view(argv[argument_index]) == L"--config") {
            config_path = argv[argument_index + 1];
            explicit_config = true;
            break;
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
        std::wcerr << L"Content configuration does not exist: " << config_path
                   << L"\n";
        return 2;
    }
    std::wstring pipe = settings.pipe_name;
    int index = 1;
    while (index < argc) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--config" && index + 1 < argc) {
            index += 2;
        } else if (argument == L"--pipe" && index + 1 < argc) {
            pipe = argv[index + 1];
            index += 2;
        } else {
            break;
        }
    }
    if (index >= argc) {
        usage();
        return 2;
    }
    const std::wstring command = argv[index++];
    if (command == L"status") {
        const auto result = esm::query_content_named_pipe_status(pipe);
        if (result.error != ERROR_SUCCESS) {
            std::wcerr << L"Content service query failed, error="
                       << result.error << L"\n";
            return 1;
        }
        std::wcout << L"documents=" << result.response.status.documents
                   << L" ready=" << result.response.status.ready
                   << L" indexing=" << result.response.status.indexing
                   << L" message=" << result.response.status.message << L"\n";
        return 0;
    }
    if (command == L"search" && index < argc) {
        esm::ContentIpcSearchRequest request;
        request.query = argv[index++];
        if (index < argc) {
            try {
                request.limit = static_cast<std::uint32_t>(std::stoul(argv[index]));
            } catch (const std::exception&) {
                usage();
                return 2;
            }
        }
        const auto result = esm::query_content_named_pipe_search(pipe, request);
        if (result.error != ERROR_SUCCESS) {
            std::wcerr << L"Content search failed, error=" << result.error;
            if (!result.response.message.empty())
                std::wcerr << L" message=" << result.response.message;
            std::wcerr << L"\n";
            return 1;
        }
        std::wcout << L"estimated="
                   << result.response.result.estimated_matches << L" returned="
                   << result.response.result.hits.size() << L"\n";
        for (const auto& hit : result.response.result.hits) {
            std::wcout << hit.relevance_percent << L"%\t" << hit.path.wstring()
                       << L"\t" << hit.snippet << L"\n";
        }
        return 0;
    }
    usage();
    return 2;
}
