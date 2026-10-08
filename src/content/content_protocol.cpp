#include "esm/content_protocol.hpp"

#include <windows.h>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace esm {
namespace {
class Writer {
public:
    void u8(std::uint8_t value) { bytes_.push_back(value); }
    void u16(std::uint16_t value) {
        for (int shift = 0; shift < 16; shift += 8)
            u8(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
    void u32(std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8)
            u8(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
    void u64(std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8)
            u8(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
    void raw(std::span<const std::uint8_t> value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    void string(std::string_view value) {
        if (value.size() > UINT32_MAX) throw std::length_error("IPC string too large");
        u32(static_cast<std::uint32_t>(value.size()));
        raw({reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
    }
    [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(bytes_); }
private:
    std::vector<std::uint8_t> bytes_;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    bool u8(std::uint8_t& value) {
        if (offset_ >= bytes_.size()) return false;
        value = bytes_[offset_++];
        return true;
    }
    bool u16(std::uint16_t& value) {
        value = 0;
        for (int shift = 0; shift < 16; shift += 8) {
            std::uint8_t byte{};
            if (!u8(byte)) return false;
            value |= static_cast<std::uint16_t>(byte) << shift;
        }
        return true;
    }
    bool u32(std::uint32_t& value) {
        value = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            std::uint8_t byte{};
            if (!u8(byte)) return false;
            value |= static_cast<std::uint32_t>(byte) << shift;
        }
        return true;
    }
    bool u64(std::uint64_t& value) {
        value = 0;
        for (int shift = 0; shift < 64; shift += 8) {
            std::uint8_t byte{};
            if (!u8(byte)) return false;
            value |= static_cast<std::uint64_t>(byte) << shift;
        }
        return true;
    }
    bool string(std::string& value) {
        std::uint32_t size{};
        if (!u32(size) || offset_ + size > bytes_.size()) return false;
        value.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
        offset_ += size;
        return true;
    }
    [[nodiscard]] bool finished() const { return offset_ == bytes_.size(); }
private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_{};
};

std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) throw std::runtime_error("UTF-16 to UTF-8 conversion failed");
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(),
                            required, nullptr, nullptr) != required)
        throw std::runtime_error("UTF-16 to UTF-8 conversion failed");
    return result;
}

bool utf8_to_wide(std::string_view value, std::wstring& result) {
    result.clear();
    if (value.empty()) return true;
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return false;
    result.resize(static_cast<std::size_t>(required));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                               static_cast<int>(value.size()), result.data(),
                               required) == required;
}

bool known_type(std::uint16_t type) {
    return type >= static_cast<std::uint16_t>(ContentIpcMessageType::search_request) &&
           type <= static_cast<std::uint16_t>(ContentIpcMessageType::shutdown_response);
}
} // namespace

std::vector<std::uint8_t> encode_content_frame(
    ContentIpcMessageType type, std::uint32_t request_id,
    std::span<const std::uint8_t> payload) {
    if (payload.size() > content_ipc_max_payload_size)
        throw std::length_error("content IPC payload too large");
    Writer writer;
    writer.u32(content_ipc_magic);
    writer.u16(content_ipc_version);
    writer.u16(static_cast<std::uint16_t>(type));
    writer.u32(request_id);
    writer.u32(static_cast<std::uint32_t>(payload.size()));
    writer.raw(payload);
    return writer.take();
}

bool decode_content_frame_header(std::span<const std::uint8_t> bytes,
                                 ContentIpcFrameHeader& header,
                                 std::string& error) {
    if (bytes.size() != content_ipc_header_size) {
        error = "invalid content IPC header size";
        return false;
    }
    Reader reader(bytes);
    std::uint32_t magic{};
    std::uint16_t version{};
    std::uint16_t type{};
    if (!reader.u32(magic) || !reader.u16(version) || !reader.u16(type) ||
        !reader.u32(header.request_id) || !reader.u32(header.payload_size)) {
        error = "truncated content IPC header";
        return false;
    }
    if (magic != content_ipc_magic) {
        error = "invalid content IPC magic";
        return false;
    }
    if (version != content_ipc_version) {
        error = "unsupported content IPC version";
        return false;
    }
    if (!known_type(type)) {
        error = "unknown content IPC message type";
        return false;
    }
    if (header.payload_size > content_ipc_max_payload_size) {
        error = "content IPC payload exceeds maximum";
        return false;
    }
    header.type = static_cast<ContentIpcMessageType>(type);
    return true;
}

bool decode_content_frame(std::span<const std::uint8_t> bytes,
                          ContentIpcFrame& frame, std::string& error) {
    if (bytes.size() < content_ipc_header_size) {
        error = "truncated content IPC frame";
        return false;
    }
    if (!decode_content_frame_header(bytes.first(content_ipc_header_size),
                                     frame.header, error)) return false;
    if (bytes.size() != content_ipc_header_size + frame.header.payload_size) {
        error = "content IPC frame length mismatch";
        return false;
    }
    frame.payload.assign(bytes.begin() + content_ipc_header_size, bytes.end());
    return true;
}

std::vector<std::uint8_t> encode_content_search_request(
    const ContentIpcSearchRequest& request) {
    Writer writer;
    writer.u32((std::min)(request.limit, content_ipc_max_results));
    writer.string(wide_to_utf8(request.query));
    return writer.take();
}

bool decode_content_search_request(std::span<const std::uint8_t> payload,
                                   ContentIpcSearchRequest& request,
                                   std::string& error) {
    Reader reader(payload);
    std::string query;
    if (!reader.u32(request.limit) || !reader.string(query) ||
        !reader.finished() || request.limit > content_ipc_max_results ||
        !utf8_to_wide(query, request.query)) {
        error = "invalid content search request";
        return false;
    }
    return true;
}

std::vector<std::uint8_t> encode_content_search_response(
    const ContentIpcSearchResponse& response) {
    Writer writer;
    writer.u32(response.error);
    writer.string(wide_to_utf8(response.message));
    writer.u64(response.result.estimated_matches);
    if (response.result.hits.size() > content_ipc_max_results)
        throw std::length_error("too many content search hits");
    writer.u32(static_cast<std::uint32_t>(response.result.hits.size()));
    for (const auto& hit : response.result.hits) {
        writer.string(wide_to_utf8(hit.path.wstring()));
        writer.string(wide_to_utf8(hit.snippet));
        writer.u32(hit.relevance_percent);
        writer.u32(static_cast<std::uint32_t>(hit.highlights.size()));
        for (const auto& range : hit.highlights) {
            writer.u32(range.start_utf16);
            writer.u32(range.length_utf16);
        }
    }
    auto bytes = writer.take();
    if (bytes.size() > content_ipc_max_payload_size)
        throw std::length_error("content search response too large");
    return bytes;
}

bool decode_content_search_response(std::span<const std::uint8_t> payload,
                                    ContentIpcSearchResponse& response,
                                    std::string& error) {
    Reader reader(payload);
    std::string message;
    std::uint32_t count{};
    if (!reader.u32(response.error) || !reader.string(message) ||
        !reader.u64(response.result.estimated_matches) || !reader.u32(count) ||
        count > content_ipc_max_results || !utf8_to_wide(message, response.message)) {
        error = "invalid content search response";
        return false;
    }
    response.result.hits.clear();
    response.result.hits.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::string path;
        std::string snippet;
        std::uint32_t range_count{};
        ContentSearchHit hit;
        std::wstring wide_path;
        if (!reader.string(path) || !reader.string(snippet) ||
            !reader.u32(hit.relevance_percent) || !reader.u32(range_count) ||
            range_count > 4096 || !utf8_to_wide(path, wide_path) ||
            !utf8_to_wide(snippet, hit.snippet)) {
            error = "invalid content search hit";
            return false;
        }
        hit.path = std::filesystem::path(wide_path);
        hit.highlights.reserve(range_count);
        for (std::uint32_t range_index = 0; range_index < range_count;
             ++range_index) {
            ContentHighlightRange range;
            if (!reader.u32(range.start_utf16) ||
                !reader.u32(range.length_utf16) ||
                static_cast<std::uint64_t>(range.start_utf16) +
                        range.length_utf16 > hit.snippet.size()) {
                error = "invalid content highlight range";
                return false;
            }
            hit.highlights.push_back(range);
        }
        response.result.hits.push_back(std::move(hit));
    }
    if (!reader.finished()) {
        error = "trailing content search response data";
        return false;
    }
    return true;
}

std::vector<std::uint8_t> encode_content_status_response(
    const ContentIpcStatusResponse& response) {
    Writer writer;
    writer.u32(response.error);
    writer.u64(response.status.documents);
    writer.u8(response.status.ready ? 1U : 0U);
    writer.u8(response.status.indexing ? 1U : 0U);
    writer.u16(0);
    writer.string(wide_to_utf8(response.status.message));
    return writer.take();
}

bool decode_content_status_response(std::span<const std::uint8_t> payload,
                                    ContentIpcStatusResponse& response,
                                    std::string& error) {
    Reader reader(payload);
    std::uint8_t ready{};
    std::uint8_t indexing{};
    std::uint16_t reserved{};
    std::string message;
    if (!reader.u32(response.error) || !reader.u64(response.status.documents) ||
        !reader.u8(ready) || !reader.u8(indexing) || !reader.u16(reserved) ||
        !reader.string(message) || !reader.finished() || ready > 1 ||
        indexing > 1 || reserved != 0 ||
        !utf8_to_wide(message, response.status.message)) {
        error = "invalid content status response";
        return false;
    }
    response.status.ready = ready != 0;
    response.status.indexing = indexing != 0;
    return true;
}

std::vector<std::uint8_t> encode_content_shutdown_response(
    const ContentIpcShutdownResponse& response) {
    Writer writer;
    writer.u32(response.error);
    writer.string(wide_to_utf8(response.message));
    return writer.take();
}

bool decode_content_shutdown_response(std::span<const std::uint8_t> payload,
                                      ContentIpcShutdownResponse& response,
                                      std::string& error) {
    Reader reader(payload);
    std::string message;
    if (!reader.u32(response.error) || !reader.string(message) ||
        !reader.finished() || !utf8_to_wide(message, response.message)) {
        error = "invalid content shutdown response";
        return false;
    }
    return true;
}
} // namespace esm
