#pragma once

#include "esm/index.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace esm {
struct ResultMetadataRequest {
    std::size_t result_index{};
    std::wstring path;
};

struct ResultMetadataUpdate {
    std::size_t result_index{};
    std::uint64_t size{};
    std::int64_t last_write_time{};
    std::int64_t creation_time{};
    std::int64_t last_access_time{};
    std::int64_t change_time{};
    std::uint32_t attributes{};
    bool directory{};
    bool succeeded{};
};

// Builds the lightweight handoff used by the GUI metadata worker. The result
// deliberately carries only the path and stable result slot, rather than a
// second deep copy of every SearchResult name/path pair. When requested, it
// keeps only records whose last-write timestamp is unknown; the service uses a
// zero timestamp as the boundary for pending basic metadata hydration.
[[nodiscard]] std::vector<ResultMetadataRequest>
make_result_metadata_requests(std::span<const SearchResult> results,
                              bool only_missing_basic_metadata = false);

// Applies numeric filesystem metadata in place while preserving result text,
// identity, score, and vector size. Invalid and unsuccessful updates are
// ignored. Returns the number of records changed.
[[nodiscard]] std::size_t apply_result_metadata_updates(
    std::span<SearchResult> results,
    std::span<const ResultMetadataUpdate> updates) noexcept;
} // namespace esm
