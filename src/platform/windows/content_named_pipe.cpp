#include "esm/content_named_pipe.hpp"

#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#ifndef PIPE_REJECT_REMOTE_CLIENTS
#define PIPE_REJECT_REMOTE_CLIENTS 0x00000008
#endif

namespace esm {
namespace {
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() {
        if (value != nullptr && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(std::exchange(
        other.value, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this == &other) return *this;
        if (value != nullptr && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = std::exchange(other.value, INVALID_HANDLE_VALUE);
        return *this;
    }
};

struct LocalSecurityDescriptor {
    PSECURITY_DESCRIPTOR value{};
    ~LocalSecurityDescriptor() {
        if (value != nullptr) LocalFree(value);
    }
};

std::wstring normalize_content_pipe_name(std::wstring_view name) {
    constexpr std::wstring_view prefix = L"\\\\.\\pipe\\";
    if (name.starts_with(prefix)) return std::wstring(name);
    return std::wstring(prefix) + std::wstring(name);
}

bool make_security(LocalSecurityDescriptor& descriptor,
                   SECURITY_ATTRIBUTES& attributes, std::uint32_t& error) {
    constexpr wchar_t sddl[] =
        L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, &descriptor.value, nullptr)) {
        error = GetLastError();
        return false;
    }
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor.value;
    attributes.bInheritHandle = FALSE;
    error = ERROR_SUCCESS;
    return true;
}

bool read_exact(HANDLE handle, void* destination, std::size_t size,
                std::uint32_t& error) {
    auto* output = static_cast<std::uint8_t*>(destination);
    std::size_t offset = 0;
    while (offset < size) {
        const auto chunk = static_cast<DWORD>((std::min<std::size_t>)(
            size - offset, std::numeric_limits<DWORD>::max()));
        DWORD read{};
        if (!ReadFile(handle, output + offset, chunk, &read, nullptr)) {
            error = GetLastError();
            return false;
        }
        if (read == 0) {
            error = ERROR_BROKEN_PIPE;
            return false;
        }
        offset += read;
    }
    error = ERROR_SUCCESS;
    return true;
}

bool write_exact(HANDLE handle, std::span<const std::uint8_t> bytes,
                 std::uint32_t& error) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto chunk = static_cast<DWORD>((std::min<std::size_t>)(
            bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written{};
        if (!WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr)) {
            error = GetLastError();
            return false;
        }
        if (written == 0) {
            error = ERROR_WRITE_FAULT;
            return false;
        }
        offset += written;
    }
    error = ERROR_SUCCESS;
    return true;
}

bool read_frame(HANDLE handle, ContentIpcFrame& frame,
                std::string& protocol_error, std::uint32_t& error) {
    std::array<std::uint8_t, content_ipc_header_size> header_bytes{};
    if (!read_exact(handle, header_bytes.data(), header_bytes.size(), error))
        return false;
    ContentIpcFrameHeader header;
    if (!decode_content_frame_header(header_bytes, header, protocol_error)) {
        error = ERROR_INVALID_DATA;
        return false;
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(content_ipc_header_size + header.payload_size);
    bytes.insert(bytes.end(), header_bytes.begin(), header_bytes.end());
    bytes.resize(content_ipc_header_size + header.payload_size);
    if (header.payload_size != 0 &&
        !read_exact(handle, bytes.data() + content_ipc_header_size,
                    header.payload_size, error)) return false;
    if (!decode_content_frame(bytes, frame, protocol_error)) {
        error = ERROR_INVALID_DATA;
        return false;
    }
    error = ERROR_SUCCESS;
    return true;
}

Handle connect_to_pipe(std::wstring_view pipe_name, std::uint32_t timeout_ms,
                       std::uint32_t& error) {
    const auto normalized = normalize_content_pipe_name(pipe_name);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    for (;;) {
        Handle handle{CreateFileW(normalized.c_str(),
                                  GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                  nullptr)};
        if (handle.value != INVALID_HANDLE_VALUE) {
            error = ERROR_SUCCESS;
            return handle;
        }
        error = GetLastError();
        if (std::chrono::steady_clock::now() >= deadline) return {};
        if (error == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(normalized.c_str(), 50);
        } else if (error == ERROR_FILE_NOT_FOUND) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } else {
            return {};
        }
    }
}

std::uint32_t serve_once(std::wstring_view pipe_name, ContentIndex& index) {
    const auto normalized = normalize_content_pipe_name(pipe_name);
    LocalSecurityDescriptor descriptor;
    SECURITY_ATTRIBUTES security{};
    std::uint32_t error{};
    if (!make_security(descriptor, security, error)) return error;
    Handle pipe{CreateNamedPipeW(
        normalized.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, 256 * 1024, 64 * 1024, 0, &security)};
    if (pipe.value == INVALID_HANDLE_VALUE) return GetLastError();
    if (!ConnectNamedPipe(pipe.value, nullptr)) {
        error = GetLastError();
        if (error != ERROR_PIPE_CONNECTED) return error;
    }

    ContentIpcFrame request_frame;
    std::string protocol_error;
    if (!read_frame(pipe.value, request_frame, protocol_error, error)) {
        DisconnectNamedPipe(pipe.value);
        return error;
    }

    ContentIpcMessageType response_type{};
    std::vector<std::uint8_t> response_payload;
    try {
        if (request_frame.header.type == ContentIpcMessageType::search_request) {
            ContentIpcSearchResponse response;
            ContentIpcSearchRequest request;
            if (!decode_content_search_request(request_frame.payload, request,
                                               protocol_error)) {
                response.error = ERROR_INVALID_DATA;
                response.message = L"内容搜索请求格式无效";
            } else {
                response.result = index.search(request.query, request.limit);
            }
            response_type = ContentIpcMessageType::search_response;
            response_payload = encode_content_search_response(response);
        } else if (request_frame.header.type ==
                   ContentIpcMessageType::status_request) {
            ContentIpcStatusResponse response;
            if (!request_frame.payload.empty()) {
                response.error = ERROR_INVALID_DATA;
            } else {
                response.status = index.status();
            }
            response_type = ContentIpcMessageType::status_response;
            response_payload = encode_content_status_response(response);
        } else {
            DisconnectNamedPipe(pipe.value);
            return ERROR_INVALID_DATA;
        }
    } catch (const std::exception& exception) {
        if (request_frame.header.type == ContentIpcMessageType::search_request) {
            ContentIpcSearchResponse response;
            response.error = ERROR_INTERNAL_ERROR;
            (void)exception;
            response.message = L"\u5185\u5bb9\u670d\u52a1\u5185\u90e8\u9519\u8bef";
            response_type = ContentIpcMessageType::search_response;
            response_payload = encode_content_search_response(response);
        } else {
            ContentIpcStatusResponse response;
            response.error = ERROR_INTERNAL_ERROR;
            response.status.message = L"内容服务内部错误";
            response_type = ContentIpcMessageType::status_response;
            response_payload = encode_content_status_response(response);
        }
    } catch (...) {
        if (request_frame.header.type == ContentIpcMessageType::search_request) {
            ContentIpcSearchResponse response;
            response.error = ERROR_INTERNAL_ERROR;
            response.message = L"\u5185\u5bb9\u670d\u52a1\u53d1\u751f\u672a\u8bc6\u522b\u7684\u5185\u90e8\u9519\u8bef";
            response_type = ContentIpcMessageType::search_response;
            response_payload = encode_content_search_response(response);
        } else {
            ContentIpcStatusResponse response;
            response.error = ERROR_INTERNAL_ERROR;
            response.status.message = L"\u5185\u5bb9\u670d\u52a1\u53d1\u751f\u672a\u8bc6\u522b\u7684\u5185\u90e8\u9519\u8bef";
            response_type = ContentIpcMessageType::status_response;
            response_payload = encode_content_status_response(response);
        }
    }

    try {
        const auto response_frame = encode_content_frame(
            response_type, request_frame.header.request_id, response_payload);
        if (!write_exact(pipe.value, response_frame, error)) {
            DisconnectNamedPipe(pipe.value);
            return error;
        }
        FlushFileBuffers(pipe.value);
    } catch (const std::exception&) {
        DisconnectNamedPipe(pipe.value);
        return ERROR_BUFFER_OVERFLOW;
    }
    DisconnectNamedPipe(pipe.value);
    return ERROR_SUCCESS;
}

template <typename Result, typename Decoder>
Result query_pipe(std::wstring_view pipe_name, ContentIpcMessageType request_type,
                  ContentIpcMessageType response_type,
                  std::span<const std::uint8_t> payload,
                  std::uint32_t timeout_ms, Decoder decoder) {
    Result result;
    std::uint32_t io_error{};
    auto pipe = connect_to_pipe(pipe_name, timeout_ms, io_error);
    if (pipe.value == INVALID_HANDLE_VALUE) {
        result.error = io_error;
        return result;
    }
    try {
        const auto frame = encode_content_frame(request_type, 1, payload);
        if (!write_exact(pipe.value, frame, io_error)) {
            result.error = io_error;
            return result;
        }
    } catch (const std::exception& exception) {
        result.error = ERROR_INVALID_DATA;
        result.protocol_error = exception.what();
        return result;
    }
    ContentIpcFrame frame;
    if (!read_frame(pipe.value, frame, result.protocol_error, io_error)) {
        result.error = io_error;
        return result;
    }
    if (frame.header.type != response_type || frame.header.request_id != 1) {
        result.error = ERROR_INVALID_DATA;
        result.protocol_error = "unexpected content IPC response";
        return result;
    }
    if (!decoder(frame.payload, result.response, result.protocol_error)) {
        result.error = ERROR_INVALID_DATA;
        return result;
    }
    result.error = result.response.error;
    return result;
}
} // namespace

std::uint32_t serve_content_named_pipe(std::wstring_view pipe_name,
                                       ContentIndex& index,
                                       std::atomic_bool& stop) {
    constexpr std::size_t worker_count = 2;
    std::atomic<std::uint32_t> fatal_error{ERROR_SUCCESS};
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&, name = std::wstring(pipe_name)] {
            while (!stop.load(std::memory_order_relaxed)) {
                const auto error = serve_once(name, index);
                if (stop.load(std::memory_order_relaxed)) break;
                if (error == ERROR_ACCESS_DENIED || error == ERROR_INVALID_NAME ||
                    error == ERROR_NOT_ENOUGH_MEMORY) {
                    std::uint32_t expected = ERROR_SUCCESS;
                    fatal_error.compare_exchange_strong(expected, error);
                    stop.store(true, std::memory_order_relaxed);
                    break;
                }
            }
        });
    }
    while (!stop.load(std::memory_order_relaxed))
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        std::uint32_t wake_error{};
        auto wake = connect_to_pipe(pipe_name, 100, wake_error);
        (void)wake;
    }
    for (auto& worker : workers) worker.join();
    return fatal_error.load(std::memory_order_relaxed);
}

ContentPipeSearchResult query_content_named_pipe_search(
    std::wstring_view pipe_name, const ContentIpcSearchRequest& request,
    std::uint32_t timeout_ms) {
    try {
        const auto payload = encode_content_search_request(request);
        return query_pipe<ContentPipeSearchResult>(
            pipe_name, ContentIpcMessageType::search_request,
            ContentIpcMessageType::search_response, payload, timeout_ms,
            decode_content_search_response);
    } catch (const std::exception& exception) {
        ContentPipeSearchResult result;
        result.error = ERROR_INVALID_DATA;
        result.protocol_error = exception.what();
        return result;
    }
}

ContentPipeStatusResult query_content_named_pipe_status(
    std::wstring_view pipe_name, std::uint32_t timeout_ms) {
    return query_pipe<ContentPipeStatusResult>(
        pipe_name, ContentIpcMessageType::status_request,
        ContentIpcMessageType::status_response, {}, timeout_ms,
        decode_content_status_response);
}
} // namespace esm
