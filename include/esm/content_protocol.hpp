#pragma once

#include "esm/content_index.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace esm {
inline constexpr std::uint32_t content_ipc_magic = 0x434d5345U; // ESMC
inline constexpr std::uint16_t content_ipc_version = 1;
inline constexpr std::size_t content_ipc_header_size = 16;
inline constexpr std::size_t content_ipc_max_payload_size = 4 * 1024 * 1024;
inline constexpr std::uint32_t content_ipc_max_results = 500;

enum class ContentIpcMessageType : std::uint16_t {
    search_request = 1,
    search_response = 2,
    status_request = 3,
    status_response = 4,
    shutdown_request = 5,
    shutdown_response = 6,
};

struct ContentIpcFrameHeader {
    ContentIpcMessageType type{ContentIpcMessageType::search_request};
    std::uint32_t request_id{};
    std::uint32_t payload_size{};
};

struct ContentIpcFrame {
    ContentIpcFrameHeader header;
    std::vector<std::uint8_t> payload;
};

struct ContentIpcSearchRequest {
    std::wstring query;
    std::uint32_t limit{100};
};

struct ContentIpcSearchResponse {
    std::uint32_t error{};
    std::wstring message;
    ContentSearchResponse result;
};

struct ContentIpcStatusResponse {
    std::uint32_t error{};
    ContentIndexStatus status;
};

struct ContentIpcShutdownResponse {
    std::uint32_t error{};
    std::wstring message;
};

[[nodiscard]] std::vector<std::uint8_t> encode_content_frame(
    ContentIpcMessageType type,
    std::uint32_t request_id,
    std::span<const std::uint8_t> payload);
[[nodiscard]] bool decode_content_frame_header(
    std::span<const std::uint8_t> bytes,
    ContentIpcFrameHeader& header,
    std::string& error);
[[nodiscard]] bool decode_content_frame(
    std::span<const std::uint8_t> bytes,
    ContentIpcFrame& frame,
    std::string& error);

[[nodiscard]] std::vector<std::uint8_t> encode_content_search_request(
    const ContentIpcSearchRequest& request);
[[nodiscard]] bool decode_content_search_request(
    std::span<const std::uint8_t> payload,
    ContentIpcSearchRequest& request,
    std::string& error);
[[nodiscard]] std::vector<std::uint8_t> encode_content_search_response(
    const ContentIpcSearchResponse& response);
[[nodiscard]] bool decode_content_search_response(
    std::span<const std::uint8_t> payload,
    ContentIpcSearchResponse& response,
    std::string& error);
[[nodiscard]] std::vector<std::uint8_t> encode_content_status_response(
    const ContentIpcStatusResponse& response);
[[nodiscard]] bool decode_content_status_response(
    std::span<const std::uint8_t> payload,
    ContentIpcStatusResponse& response,
    std::string& error);
[[nodiscard]] std::vector<std::uint8_t> encode_content_shutdown_response(
    const ContentIpcShutdownResponse& response);
[[nodiscard]] bool decode_content_shutdown_response(
    std::span<const std::uint8_t> payload,
    ContentIpcShutdownResponse& response,
    std::string& error);
} // namespace esm
