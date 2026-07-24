#include "esm/named_pipe.hpp"
#include "esm/file_metadata.hpp"

#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <thread>
#include <vector>

#ifndef PIPE_REJECT_REMOTE_CLIENTS
#define PIPE_REJECT_REMOTE_CLIENTS 0x00000008
#endif

namespace esm {
namespace {
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
};

struct LocalSecurityDescriptor {
    PSECURITY_DESCRIPTOR value{};
    ~LocalSecurityDescriptor() {
        if (value != nullptr) LocalFree(value);
    }
};

bool make_pipe_security_attributes(LocalSecurityDescriptor& descriptor,
                                   SECURITY_ATTRIBUTES& attributes,
                                   std::uint32_t& error) {
    // The transport is query-only and PIPE_REJECT_REMOTE_CLIENTS prevents
    // network clients. SYSTEM and administrators retain full control, while
    // authenticated local users receive only the read/write access required
    // to exchange request and response frames with an elevated service.
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
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
            size - offset, std::numeric_limits<DWORD>::max()));
        DWORD read = 0;
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
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data() + offset, chunk, &written,
                       nullptr)) {
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

bool read_frame(HANDLE handle, IpcFrame& frame, std::string& protocol_error,
                std::uint32_t& error) {
    std::array<std::uint8_t, ipc_frame_header_size> header_bytes{};
    if (!read_exact(handle, header_bytes.data(), header_bytes.size(), error))
        return false;
    IpcFrameHeader header;
    if (!decode_ipc_frame_header(header_bytes, header, protocol_error)) {
        error = ERROR_INVALID_DATA;
        return false;
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(ipc_frame_header_size + header.payload_size);
    bytes.insert(bytes.end(), header_bytes.begin(), header_bytes.end());
    bytes.resize(ipc_frame_header_size + header.payload_size);
    if (header.payload_size != 0 &&
        !read_exact(handle, bytes.data() + ipc_frame_header_size,
                    header.payload_size, error)) {
        return false;
    }
    if (!decode_ipc_frame(bytes, frame, protocol_error)) {
        error = ERROR_INVALID_DATA;
        return false;
    }
    error = ERROR_SUCCESS;
    return true;
}

Handle connect_to_pipe(std::wstring_view pipe_name,
                       std::uint32_t timeout_ms,
                       std::uint32_t& error) {
    const auto normalized = normalize_pipe_name(pipe_name);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    for (;;) {
        HANDLE handle = CreateFileW(
            normalized.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            error = ERROR_SUCCESS;
            return {handle};
        }
        error = GetLastError();
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return {};
        }
        const auto remaining = std::chrono::duration_cast<
            std::chrono::milliseconds>(deadline - now);
        const auto wait_ms = static_cast<DWORD>(std::min<std::int64_t>(
            remaining.count(), 100));
        if (error == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(normalized.c_str(), wait_ms);
        } else if (error == ERROR_FILE_NOT_FOUND) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } else {
            return {};
        }
    }
}
} // namespace

std::wstring normalize_pipe_name(std::wstring_view name) {
    constexpr std::wstring_view prefix = L"\\\\.\\pipe\\";
    if (name.starts_with(prefix)) return std::wstring(name);
    return std::wstring(prefix) + std::wstring(name);
}

std::uint32_t serve_named_pipe_search_once(std::wstring_view pipe_name,
                                           const MetadataIndex& index) {
    const auto normalized = normalize_pipe_name(pipe_name);
    LocalSecurityDescriptor descriptor;
    SECURITY_ATTRIBUTES security{};
    std::uint32_t security_error = ERROR_SUCCESS;
    if (!make_pipe_security_attributes(descriptor, security, security_error))
        return security_error;

    Handle pipe{CreateNamedPipeW(
        normalized.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, &security)};
    if (pipe.value == INVALID_HANDLE_VALUE) return GetLastError();

    if (!ConnectNamedPipe(pipe.value, nullptr)) {
        const auto error = GetLastError();
        if (error != ERROR_PIPE_CONNECTED) return error;
    }

    IpcFrame frame;
    std::string protocol_error;
    std::uint32_t io_error = ERROR_SUCCESS;
    if (!read_frame(pipe.value, frame, protocol_error, io_error)) {
        DisconnectNamedPipe(pipe.value);
        return io_error;
    }

    IpcSearchResponse response;
    if (frame.header.type != IpcMessageType::search_request) {
        response.error = ERROR_INVALID_DATA;
    } else {
        IpcSearchRequest request;
        if (!decode_search_request(frame.payload, request, protocol_error)) {
            response.error = ERROR_INVALID_DATA;
        } else {
            SearchOptions options;
            options.limit = request.limit;
            options.case_sensitive = request.case_sensitive;
            options.match_path = request.match_path;
            options.whole_word = request.whole_word;
    options.match_diacritics = request.match_diacritics;
            options.sort = request.sort;
            options.descending = request.descending;
            const auto started = std::chrono::steady_clock::now();
            response.results = index.search(request.query, options);
            // The fast MFT name enumeration does not contain file size or the
            // current last-write timestamp. Hydrate only returned rows so the
            // GUI gets real metadata without stat-ing the complete catalog.
            for (auto& result : response.results) {
                (void)hydrate_file_metadata(result.record);
            }
            response.elapsed_microseconds = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - started).count());
        }
    }

    try {
        const auto payload = encode_search_response(response);
        const auto response_frame = encode_ipc_frame(
            IpcMessageType::search_response, frame.header.request_id, payload);
        if (!write_exact(pipe.value, response_frame, io_error)) {
            DisconnectNamedPipe(pipe.value);
            return io_error;
        }
        FlushFileBuffers(pipe.value);
    } catch (const std::exception&) {
        DisconnectNamedPipe(pipe.value);
        return ERROR_BUFFER_OVERFLOW;
    }
    DisconnectNamedPipe(pipe.value);
    return ERROR_SUCCESS;
}

std::uint32_t serve_named_pipe_search(std::wstring_view pipe_name,
                                      const MetadataIndex& index,
                                      std::atomic_bool& stop,
                                      std::size_t worker_count) {
    if (worker_count == 0) return ERROR_INVALID_PARAMETER;

    std::atomic<std::uint32_t> fatal_error{ERROR_SUCCESS};
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i) {
        workers.emplace_back([&, pipe = std::wstring(pipe_name)] {
            while (!stop.load(std::memory_order_relaxed)) {
                const auto error = serve_named_pipe_search_once(pipe, index);
                if (stop.load(std::memory_order_relaxed)) break;
                // Client disconnects and malformed requests affect only that
                // connection. Pipe creation/security failures are host-fatal.
                if (error == ERROR_ACCESS_DENIED ||
                    error == ERROR_INVALID_NAME ||
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

    // Each connection wakes one worker blocked in ConnectNamedPipe. Closing
    // the client immediately makes its read return ERROR_BROKEN_PIPE; because
    // stop is already set, the worker exits without treating it as a fault.
    for (std::size_t i = 0; i < worker_count; ++i) {
        std::uint32_t wake_error = ERROR_SUCCESS;
        auto wake = connect_to_pipe(pipe_name, 100, wake_error);
        if (wake.value == INVALID_HANDLE_VALUE &&
            wake_error != ERROR_SEM_TIMEOUT &&
            wake_error != ERROR_FILE_NOT_FOUND &&
            wake_error != ERROR_PIPE_BUSY) {
            std::uint32_t expected = ERROR_SUCCESS;
            fatal_error.compare_exchange_strong(expected, wake_error);
        }
    }
    for (auto& worker : workers) worker.join();
    return fatal_error.load(std::memory_order_relaxed);
}

PipeSearchResult query_named_pipe_search(std::wstring_view pipe_name,
                                         const IpcSearchRequest& request,
                                         std::uint32_t timeout_ms) {
    PipeSearchResult result;
    std::uint32_t io_error = ERROR_SUCCESS;
    auto pipe = connect_to_pipe(pipe_name, timeout_ms, io_error);
    if (pipe.value == INVALID_HANDLE_VALUE) {
        result.error = io_error;
        return result;
    }

    try {
        const auto payload = encode_search_request(request);
        const auto frame = encode_ipc_frame(
            IpcMessageType::search_request, 1, payload);
        if (!write_exact(pipe.value, frame, io_error)) {
            result.error = io_error;
            return result;
        }
    } catch (const std::exception& error) {
        result.error = ERROR_INVALID_DATA;
        result.protocol_error = error.what();
        return result;
    }

    IpcFrame response_frame;
    if (!read_frame(pipe.value, response_frame, result.protocol_error,
                    io_error)) {
        result.error = io_error;
        return result;
    }
    if (response_frame.header.type != IpcMessageType::search_response ||
        response_frame.header.request_id != 1) {
        result.error = ERROR_INVALID_DATA;
        result.protocol_error = "unexpected IPC response frame";
        return result;
    }
    if (!decode_search_response(response_frame.payload, result.response,
                                result.protocol_error)) {
        result.error = ERROR_INVALID_DATA;
        return result;
    }
    result.error = result.response.error;
    return result;
}
} // namespace esm
