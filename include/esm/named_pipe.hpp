#pragma once

#include "esm/index.hpp"
#include "esm/ipc_protocol.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace esm {
struct PipeSearchResult {
    std::uint32_t error{};
    std::string protocol_error;
    IpcSearchResponse response;
};

// Protocol-independent timings for one accepted named-pipe request. Keeping
// these diagnostics outside IpcSearchResponse preserves IPC v1 compatibility.
// Query text is deliberately not retained so service logs cannot disclose file
// names or paths; query_characters is sufficient to spot unbounded requests.
struct PipeSearchDiagnostics {
    std::uint32_t request_id{};
    std::uint32_t error{};
    std::uint32_t requested_limit{};
    std::size_t query_characters{};
    std::size_t result_count{};
    bool case_sensitive{};
    bool match_path{};
    bool whole_word{};
    bool match_diacritics{true};
    SortField sort{SortField::relevance};
    bool descending{};
    std::uint64_t read_microseconds{};
    std::uint64_t decode_microseconds{};
    std::uint64_t search_microseconds{};
    std::uint64_t encode_microseconds{};
    std::uint64_t write_microseconds{};
    std::uint64_t total_microseconds{};
};

using PipeSearchDiagnosticsSink =
    std::function<void(const PipeSearchDiagnostics&)>;

[[nodiscard]] std::wstring format_pipe_search_diagnostics(
    const PipeSearchDiagnostics& diagnostics);

[[nodiscard]] std::wstring normalize_pipe_name(std::wstring_view name);

// Returns the SID of the account that owns the current process token. The
// string form is suitable for an SDDL ACE and for persisting in an SCM command
// line. error is ERROR_SUCCESS only when a non-empty SID is returned.
[[nodiscard]] std::wstring current_process_user_sid(std::uint32_t& error);

// Builds the local-only search Pipe DACL. SYSTEM and administrators retain
// full control; only the selected user SID receives query read/write access.
[[nodiscard]] std::wstring local_pipe_security_sddl(
    std::wstring_view allowed_user_sid);

// Creates one local-only pipe instance, serves exactly one search request,
// writes one response, then disconnects.
[[nodiscard]] std::uint32_t serve_named_pipe_search_once(
    std::wstring_view pipe_name,
    const MetadataIndex& index,
    const PipeSearchDiagnosticsSink& diagnostics = {},
    std::wstring_view allowed_user_sid = {});

// Runs multiple local pipe instances concurrently until stop becomes true.
// Setting stop wakes workers blocked in ConnectNamedPipe and joins them before
// returning, which makes this suitable for console and Windows Service hosts.
[[nodiscard]] std::uint32_t serve_named_pipe_search(
    std::wstring_view pipe_name,
    const MetadataIndex& index,
    std::atomic_bool& stop,
    std::size_t worker_count = 4,
    PipeSearchDiagnosticsSink diagnostics = {},
    std::wstring_view allowed_user_sid = {});

[[nodiscard]] PipeSearchResult query_named_pipe_search(
    std::wstring_view pipe_name,
    const IpcSearchRequest& request,
    std::uint32_t timeout_ms = 5'000);
} // namespace esm
