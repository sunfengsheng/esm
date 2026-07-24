#pragma once

#include "esm/index.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace esm {
constexpr std::uint32_t ipc_magic = 0x314D5345; // "ESM1" little-endian
constexpr std::uint16_t ipc_version = 1;
constexpr std::size_t ipc_frame_header_size = 16;
constexpr std::size_t ipc_max_payload_size = 4 * 1024 * 1024;
constexpr std::uint32_t ipc_max_search_results = 1'000;

enum class IpcMessageType : std::uint16_t {
    search_request = 1,
    search_response = 2,
};

struct IpcFrameHeader {
    IpcMessageType type{IpcMessageType::search_request};
    std::uint32_t request_id{};
    std::uint32_t payload_size{};
};

struct IpcFrame {
    IpcFrameHeader header;
    std::vector<std::uint8_t> payload;
};

struct IpcSearchRequest {
    std::uint32_t limit{100};
    bool case_sensitive{};
    bool match_path{true};
    bool whole_word{};
    bool match_diacritics{true};
    SortField sort{SortField::relevance};
    bool descending{};
    std::wstring query;
};

struct IpcSearchResponse {
    std::uint32_t error{};
    std::uint64_t elapsed_microseconds{};
    std::vector<SearchResult> results;
};

[[nodiscard]] std::vector<std::uint8_t> encode_ipc_frame(
    IpcMessageType type,
    std::uint32_t request_id,
    std::span<const std::uint8_t> payload);
[[nodiscard]] bool decode_ipc_frame_header(
    std::span<const std::uint8_t> bytes,
    IpcFrameHeader& header,
    std::string& error);
[[nodiscard]] bool decode_ipc_frame(
    std::span<const std::uint8_t> bytes,
    IpcFrame& frame,
    std::string& error);

[[nodiscard]] std::vector<std::uint8_t> encode_search_request(
    const IpcSearchRequest& request);
[[nodiscard]] bool decode_search_request(
    std::span<const std::uint8_t> payload,
    IpcSearchRequest& request,
    std::string& error);
[[nodiscard]] std::vector<std::uint8_t> encode_search_response(
    const IpcSearchResponse& response);
[[nodiscard]] bool decode_search_response(
    std::span<const std::uint8_t> payload,
    IpcSearchResponse& response,
    std::string& error);
} // namespace esm
