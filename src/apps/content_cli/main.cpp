#include "esm/content_named_pipe.hpp"

#include <windows.h>

#include <iostream>
#include <string>

namespace {
void usage() {
    std::wcout
        << L"Usage:\n"
        << L"  esm_content_cli [--pipe <name>] status\n"
        << L"  esm_content_cli [--pipe <name>] search <query> [limit]\n";
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring pipe = L"everything_sm_content";
    int index = 1;
    if (index + 1 < argc && std::wstring_view(argv[index]) == L"--pipe") {
        pipe = argv[index + 1];
        index += 2;
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
