#pragma once

#include "esm/index.hpp"
#include "esm/ipc_protocol.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace esm {
struct PipeSearchResult {
    std::uint32_t error{};
    std::string protocol_error;
    IpcSearchResponse response;
};

[[nodiscard]] std::wstring normalize_pipe_name(std::wstring_view name);

// Creates one local-only pipe instance, serves exactly one search request,
// writes one response, then disconnects.
[[nodiscard]] std::uint32_t serve_named_pipe_search_once(
    std::wstring_view pipe_name,
    const MetadataIndex& index);

// Runs multiple local pipe instances concurrently until stop becomes true.
// Setting stop wakes workers blocked in ConnectNamedPipe and joins them before
// returning, which makes this suitable for console and Windows Service hosts.
[[nodiscard]] std::uint32_t serve_named_pipe_search(
    std::wstring_view pipe_name,
    const MetadataIndex& index,
    std::atomic_bool& stop,
    std::size_t worker_count = 4);

[[nodiscard]] PipeSearchResult query_named_pipe_search(
    std::wstring_view pipe_name,
    const IpcSearchRequest& request,
    std::uint32_t timeout_ms = 5'000);
} // namespace esm
