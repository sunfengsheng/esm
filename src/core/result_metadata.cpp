#include "esm/result_metadata.hpp"

namespace esm {
std::vector<ResultMetadataRequest>
make_result_metadata_requests(std::span<const SearchResult> results,
                              bool only_missing_basic_metadata) {
    std::vector<ResultMetadataRequest> requests;
    requests.reserve(results.size());
    for (std::size_t index = 0; index < results.size(); ++index) {
        if (only_missing_basic_metadata &&
            results[index].record.last_write_time != 0) {
            continue;
        }
        requests.push_back({index, results[index].record.path});
    }
    return requests;
}

std::size_t apply_result_metadata_updates(
    std::span<SearchResult> results,
    std::span<const ResultMetadataUpdate> updates) noexcept {
    std::size_t changed = 0;
    for (const auto& update : updates) {
        if (!update.succeeded || update.result_index >= results.size())
            continue;
        auto& record = results[update.result_index].record;
        record.size = update.size;
        record.last_write_time = update.last_write_time;
        record.creation_time = update.creation_time;
        record.last_access_time = update.last_access_time;
        record.change_time = update.change_time;
        record.attributes = update.attributes;
        record.directory = update.directory;
        ++changed;
    }
    return changed;
}
} // namespace esm
