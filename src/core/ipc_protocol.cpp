#include "esm/ipc_protocol.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace esm {
namespace {
class Writer {
public:
    void u8(std::uint8_t value) { bytes_.push_back(value); }
    void u16(std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value));
        u8(static_cast<std::uint8_t>(value >> 8U));
    }
    void u32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            u8(static_cast<std::uint8_t>(value >> shift));
    }
    void u64(std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8)
            u8(static_cast<std::uint8_t>(value >> shift));
    }
    void i32(std::int32_t value) { u32(static_cast<std::uint32_t>(value)); }
    void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }
    void raw(std::span<const std::uint8_t> value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    void string(std::string_view value) {
        if (value.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("IPC string exceeds 32-bit length");
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
        if (offset_ + 1 > bytes_.size()) return false;
        value = bytes_[offset_++];
        return true;
    }
    bool u16(std::uint16_t& value) {
        if (offset_ + 2 > bytes_.size()) return false;
        value = static_cast<std::uint16_t>(bytes_[offset_]) |
                (static_cast<std::uint16_t>(bytes_[offset_ + 1]) << 8U);
        offset_ += 2;
        return true;
    }
    bool u32(std::uint32_t& value) {
        if (offset_ + 4 > bytes_.size()) return false;
        value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8)
            value |= static_cast<std::uint32_t>(bytes_[offset_++]) << shift;
        return true;
    }
    bool u64(std::uint64_t& value) {
        if (offset_ + 8 > bytes_.size()) return false;
        value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8)
            value |= static_cast<std::uint64_t>(bytes_[offset_++]) << shift;
        return true;
    }
    bool i32(std::int32_t& value) {
        std::uint32_t raw{};
        if (!u32(raw)) return false;
        value = static_cast<std::int32_t>(raw);
        return true;
    }
    bool i64(std::int64_t& value) {
        std::uint64_t raw{};
        if (!u64(raw)) return false;
        value = static_cast<std::int64_t>(raw);
        return true;
    }
    bool string(std::string& value) {
        std::uint32_t length{};
        if (!u32(length) || offset_ + length > bytes_.size()) return false;
        value.assign(reinterpret_cast<const char*>(bytes_.data() + offset_),
                     length);
        offset_ += length;
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
    if (required <= 0) throw std::runtime_error("invalid UTF-16 string");
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(),
                            required, nullptr, nullptr) != required) {
        throw std::runtime_error("UTF-16 to UTF-8 conversion failed");
    }
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
    return MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), required) == required;
}

bool validate_payload_size(std::size_t size, std::string& error) {
    if (size > ipc_max_payload_size) {
        error = "IPC payload exceeds configured maximum";
        return false;
    }
    return true;
}
} // namespace

std::vector<std::uint8_t> encode_ipc_frame(
    IpcMessageType type,
    std::uint32_t request_id,
    std::span<const std::uint8_t> payload) {
    if (payload.size() > ipc_max_payload_size)
        throw std::length_error("IPC payload exceeds configured maximum");
    Writer writer;
    writer.u32(ipc_magic);
    writer.u16(ipc_version);
    writer.u16(static_cast<std::uint16_t>(type));
    writer.u32(request_id);
    writer.u32(static_cast<std::uint32_t>(payload.size()));
    writer.raw(payload);
    return writer.take();
}

bool decode_ipc_frame_header(std::span<const std::uint8_t> bytes,
                             IpcFrameHeader& header,
                             std::string& error) {
    if (bytes.size() != ipc_frame_header_size) {
        error = "invalid IPC frame header size";
        return false;
    }
    Reader reader(bytes);
    std::uint32_t magic{};
    std::uint16_t version{};
    std::uint16_t type{};
    if (!reader.u32(magic) || !reader.u16(version) || !reader.u16(type) ||
        !reader.u32(header.request_id) || !reader.u32(header.payload_size)) {
        error = "truncated IPC frame header";
        return false;
    }
    if (magic != ipc_magic) {
        error = "invalid IPC magic";
        return false;
    }
    if (version != ipc_version) {
        error = "unsupported IPC version";
        return false;
    }
    if (type != static_cast<std::uint16_t>(IpcMessageType::search_request) &&
        type != static_cast<std::uint16_t>(IpcMessageType::search_response)) {
        error = "unknown IPC message type";
        return false;
    }
    if (!validate_payload_size(header.payload_size, error)) return false;
    header.type = static_cast<IpcMessageType>(type);
    return true;
}

bool decode_ipc_frame(std::span<const std::uint8_t> bytes,
                      IpcFrame& frame,
                      std::string& error) {
    if (bytes.size() < ipc_frame_header_size) {
        error = "truncated IPC frame";
        return false;
    }
    if (!decode_ipc_frame_header(bytes.first(ipc_frame_header_size),
                                 frame.header, error)) {
        return false;
    }
    if (bytes.size() != ipc_frame_header_size + frame.header.payload_size) {
        error = "IPC frame length mismatch";
        return false;
    }
    frame.payload.assign(bytes.begin() + ipc_frame_header_size, bytes.end());
    return true;
}

std::vector<std::uint8_t> encode_search_request(
    const IpcSearchRequest& request) {
    Writer writer;
    writer.u32(std::min(request.limit, ipc_max_search_results));
    std::uint32_t flags = 0;
    if (request.case_sensitive) flags |= 1U;
    if (request.match_path) flags |= 2U;
    if (request.whole_word) flags |= 4U;
    if (request.match_diacritics) flags |= 8U;
    writer.u32(flags);
    writer.string(wide_to_utf8(request.query));
    writer.u8(static_cast<std::uint8_t>(request.sort));
    writer.u8(request.descending ? 1U : 0U);
    writer.u16(0);
    auto payload = writer.take();
    if (payload.size() > ipc_max_payload_size)
        throw std::length_error("search request exceeds IPC maximum");
    return payload;
}

bool decode_search_request(std::span<const std::uint8_t> payload,
                           IpcSearchRequest& request,
                           std::string& error) {
    Reader reader(payload);
    std::uint32_t flags{};
    std::string query;
    if (!reader.u32(request.limit) || !reader.u32(flags) ||
        !reader.string(query)) {
        error = "invalid search request payload";
        return false;
    }
    request.sort = SortField::relevance;
    request.descending = false;
    if (!reader.finished()) {
        std::uint8_t sort{};
        std::uint8_t descending{};
        std::uint16_t reserved{};
        if (!reader.u8(sort) || !reader.u8(descending) ||
            !reader.u16(reserved) || !reader.finished() ||
            sort > static_cast<std::uint8_t>(SortField::file_list_name) ||
            descending > 1 || reserved != 0) {
            error = "invalid search request options";
            return false;
        }
        request.sort = static_cast<SortField>(sort);
        request.descending = descending != 0;
    }
    if (request.limit > ipc_max_search_results) {
        error = "search result limit exceeds maximum";
        return false;
    }
    if ((flags & ~15U) != 0) {
        error = "search request contains unknown flags";
        return false;
    }
    request.case_sensitive = (flags & 1U) != 0;
    request.match_path = (flags & 2U) != 0;
    request.whole_word = (flags & 4U) != 0;
    request.match_diacritics = (flags & 8U) != 0;
    if (!utf8_to_wide(query, request.query)) {
        error = "search query is not valid UTF-8";
        return false;
    }
    return true;
}

std::vector<std::uint8_t> encode_search_response(
    const IpcSearchResponse& response) {
    if (response.results.size() > ipc_max_search_results)
        throw std::length_error("search response result count exceeds maximum");
    Writer writer;
    writer.u32(response.error);
    writer.u64(response.elapsed_microseconds);
    writer.u32(static_cast<std::uint32_t>(response.results.size()));
    for (const auto& result : response.results) {
        const auto& record = result.record;
        writer.i32(result.score);
        writer.u64(record.id);
        writer.u64(record.parent_id);
        writer.u64(record.size);
        writer.i64(record.last_write_time);
        writer.i64(record.creation_time);
        writer.i64(record.last_access_time);
        writer.i64(record.change_time);
        writer.u32(record.attributes);
        writer.u8(record.directory ? 1U : 0U);
        writer.string(wide_to_utf8(record.name));
        writer.string(wide_to_utf8(record.path));
    }
    auto payload = writer.take();
    if (payload.size() > ipc_max_payload_size)
        throw std::length_error("search response exceeds IPC maximum");
    return payload;
}

bool decode_search_response(std::span<const std::uint8_t> payload,
                            IpcSearchResponse& response,
                            std::string& error) {
    Reader reader(payload);
    std::uint32_t count{};
    if (!reader.u32(response.error) ||
        !reader.u64(response.elapsed_microseconds) || !reader.u32(count)) {
        error = "invalid search response header";
        return false;
    }
    if (count > ipc_max_search_results) {
        error = "search response result count exceeds maximum";
        return false;
    }
    std::vector<SearchResult> results;
    results.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        SearchResult result;
        std::uint8_t directory{};
        std::string name;
        std::string path;
        if (!reader.i32(result.score) || !reader.u64(result.record.id) ||
            !reader.u64(result.record.parent_id) ||
            !reader.u64(result.record.size) ||
            !reader.i64(result.record.last_write_time) ||
            !reader.i64(result.record.creation_time) ||
            !reader.i64(result.record.last_access_time) ||
            !reader.i64(result.record.change_time) ||
            !reader.u32(result.record.attributes) || !reader.u8(directory) ||
            !reader.string(name) || !reader.string(path)) {
            error = "truncated search response result";
            return false;
        }
        if (directory > 1 || !utf8_to_wide(name, result.record.name) ||
            !utf8_to_wide(path, result.record.path)) {
            error = "invalid search response result";
            return false;
        }
        result.record.directory = directory != 0;
        results.push_back(std::move(result));
    }
    if (!reader.finished()) {
        error = "unexpected trailing bytes in search response";
        return false;
    }
    response.results = std::move(results);
    return true;
}
} // namespace esm
