#include "esm/result_metadata.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {
template <class Function>
double elapsed_ms(Function&& function) {
    const auto started = std::chrono::steady_clock::now();
    function();
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - started)
        .count();
}

double mib(std::size_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}
} // namespace

int main(int argc, char** argv) {
    std::size_t count = 100000;
    if (argc >= 2) {
        try {
            count = std::max<std::size_t>(1, std::stoull(argv[1]));
        } catch (...) {
            std::cerr << "invalid result count\n";
            return 2;
        }
    }

    std::vector<esm::SearchResult> results;
    results.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        esm::SearchResult result;
        result.score = static_cast<int>(index % 1000);
        result.record.id = index + 1;
        result.record.name = L"quarterly_report_" + std::to_wstring(index) +
                             L"_final_document.txt";
        result.record.path = L"C:\\Users\\benchmark\\Documents\\department_" +
                             std::to_wstring(index % 128) + L"\\project_" +
                             std::to_wstring(index % 4096) + L"\\" +
                             result.record.name;
        results.push_back(std::move(result));
    }

    std::vector<esm::SearchResult> legacy_copy;
    const auto legacy_copy_ms = elapsed_ms([&] { legacy_copy = results; });

    std::vector<esm::ResultMetadataRequest> requests;
    const auto request_build_ms = elapsed_ms(
        [&] { requests = esm::make_result_metadata_requests(results); });

    std::size_t legacy_text_capacity = 0;
    for (const auto& result : legacy_copy) {
        legacy_text_capacity += result.record.name.capacity();
        legacy_text_capacity += result.record.path.capacity();
    }
    std::size_t request_text_capacity = 0;
    for (const auto& request : requests)
        request_text_capacity += request.path.capacity();

    const auto legacy_bytes = legacy_copy.capacity() * sizeof(esm::SearchResult) +
                              legacy_text_capacity * sizeof(wchar_t);
    const auto request_bytes =
        requests.capacity() * sizeof(esm::ResultMetadataRequest) +
        request_text_capacity * sizeof(wchar_t);
    const auto update_bytes =
        count * sizeof(esm::ResultMetadataUpdate);

    std::cout << "results=" << count << '\n'
              << std::fixed << std::setprecision(2)
              << "legacy_deep_copy_ms=" << legacy_copy_ms << '\n'
              << "lightweight_request_build_ms=" << request_build_ms << '\n'
              << "legacy_handoff_estimated_mb=" << mib(legacy_bytes) << '\n'
              << "lightweight_request_estimated_mb=" << mib(request_bytes)
              << '\n'
              << "numeric_update_payload_estimated_mb=" << mib(update_bytes)
              << '\n';
    return 0;
}
