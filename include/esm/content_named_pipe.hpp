#pragma once

#include "esm/content_index.hpp"
#include "esm/content_protocol.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace esm {
struct ContentPipeSearchResult {
    std::uint32_t error{};
    std::string protocol_error;
    ContentIpcSearchResponse response;
};

struct ContentPipeStatusResult {
    std::uint32_t error{};
    std::string protocol_error;
    ContentIpcStatusResponse response;
};

[[nodiscard]] std::uint32_t serve_content_named_pipe(
    std::wstring_view pipe_name,
    ContentIndex& index,
    std::atomic_bool& stop);
[[nodiscard]] ContentPipeSearchResult query_content_named_pipe_search(
    std::wstring_view pipe_name,
    const ContentIpcSearchRequest& request,
    std::uint32_t timeout_ms = 5'000);
[[nodiscard]] ContentPipeStatusResult query_content_named_pipe_status(
    std::wstring_view pipe_name,
    std::uint32_t timeout_ms = 2'000);
} // namespace esm
