#include "esm/named_pipe.hpp"

#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>
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

std::uint64_t elapsed_microseconds(
    std::chrono::steady_clock::time_point started,
    std::chrono::steady_clock::time_point finished =
        std::chrono::steady_clock::now()) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(finished - started)
            .count());
}

void publish_diagnostics(const PipeSearchDiagnosticsSink& sink,
                         const PipeSearchDiagnostics& diagnostics) noexcept {
    if (!sink) return;
    try {
        sink(diagnostics);
    } catch (...) {
        // Diagnostics must never terminate a pipe worker or make search fail.
    }
}

const wchar_t* sort_field_name(SortField field) {
    switch (field) {
    case SortField::relevance: return L"relevance";
    case SortField::name: return L"name";
    case SortField::path: return L"path";
    case SortField::size: return L"size";
    case SortField::last_write_time: return L"last_write_time";
    case SortField::attributes: return L"attributes";
    case SortField::extension: return L"extension";
    case SortField::type: return L"type";
    case SortField::creation_time: return L"creation_time";
    case SortField::last_access_time: return L"last_access_time";
    case SortField::change_time: return L"change_time";
    case SortField::run_count: return L"run_count";
    case SortField::last_open_time: return L"last_open_time";
    case SortField::file_list_name: return L"file_list_name";
    }
    return L"unknown";
}
} // namespace

std::wstring format_pipe_search_diagnostics(
    const PipeSearchDiagnostics& diagnostics) {
    const auto milliseconds = [](std::uint64_t microseconds) {
        return static_cast<double>(microseconds) / 1000.0;
    };
    std::wostringstream message;
    message << std::fixed << std::setprecision(3)
            << L"Slow search: request=" << diagnostics.request_id
            << L", total_ms=" << milliseconds(diagnostics.total_microseconds)
            << L", read_ms=" << milliseconds(diagnostics.read_microseconds)
            << L", decode_ms=" << milliseconds(diagnostics.decode_microseconds)
            << L", search_ms=" << milliseconds(diagnostics.search_microseconds)
            << L", encode_ms=" << milliseconds(diagnostics.encode_microseconds)
            << L", write_ms=" << milliseconds(diagnostics.write_microseconds)
            << L", results=" << diagnostics.result_count
            << L", limit=" << diagnostics.requested_limit
            << L", query_chars=" << diagnostics.query_characters
            << L", sort=" << sort_field_name(diagnostics.sort)
            << (diagnostics.descending ? L" desc" : L" asc")
            << L", flags="
            << (diagnostics.case_sensitive ? L"case," : L"")
            << (diagnostics.match_path ? L"path," : L"name,")
            << (diagnostics.whole_word ? L"whole-word," : L"")
            << (diagnostics.match_diacritics ? L"diacritics" : L"fold-diacritics")
            << L", error=" << diagnostics.error;
    return message.str();
}

std::wstring normalize_pipe_name(std::wstring_view name) {
    constexpr std::wstring_view prefix = L"\\\\.\\pipe\\";
    if (name.starts_with(prefix)) return std::wstring(name);
    return std::wstring(prefix) + std::wstring(name);
}

std::uint32_t serve_named_pipe_search_once(
    std::wstring_view pipe_name, const MetadataIndex& index,
    const PipeSearchDiagnosticsSink& diagnostics_sink) {
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

    const auto request_started = std::chrono::steady_clock::now();
    PipeSearchDiagnostics diagnostics;
    IpcFrame frame;
    std::string protocol_error;
    std::uint32_t io_error = ERROR_SUCCESS;
    const auto read_started = std::chrono::steady_clock::now();
    if (!read_frame(pipe.value, frame, protocol_error, io_error)) {
        DisconnectNamedPipe(pipe.value);
        return io_error;
    }
    diagnostics.read_microseconds = elapsed_microseconds(read_started);
    diagnostics.request_id = frame.header.request_id;

    IpcSearchResponse response;
    bool decode_timing_recorded = false;
    const auto decode_started = std::chrono::steady_clock::now();
    if (frame.header.type != IpcMessageType::search_request) {
        response.error = ERROR_INVALID_DATA;
    } else {
        IpcSearchRequest request;
        if (!decode_search_request(frame.payload, request, protocol_error)) {
            response.error = ERROR_INVALID_DATA;
        } else {
            diagnostics.requested_limit = request.limit;
            diagnostics.query_characters = request.query.size();
            diagnostics.case_sensitive = request.case_sensitive;
            diagnostics.match_path = request.match_path;
            diagnostics.whole_word = request.whole_word;
            diagnostics.match_diacritics = request.match_diacritics;
            diagnostics.sort = request.sort;
            diagnostics.descending = request.descending;
            diagnostics.decode_microseconds = elapsed_microseconds(decode_started);
            decode_timing_recorded = true;

            SearchOptions options;
            options.limit = request.limit;
            options.case_sensitive = request.case_sensitive;
            options.match_path = request.match_path;
            options.whole_word = request.whole_word;
            options.match_diacritics = request.match_diacritics;
            options.sort = request.sort;
            options.descending = request.descending;
            const auto search_started = std::chrono::steady_clock::now();
            response.results = index.search(request.query, options);
            // Keep the query hot path metadata-only. Filesystem stat calls can
            // block on sleeping disks, unavailable volumes, cloud placeholders,
            // or antivirus and must not delay the first result batch. The GUI
            // hydrates returned rows asynchronously after typing settles.
            diagnostics.search_microseconds =
                elapsed_microseconds(search_started);
            response.elapsed_microseconds = diagnostics.search_microseconds;
        }
    }
    if (!decode_timing_recorded)
        diagnostics.decode_microseconds = elapsed_microseconds(decode_started);

    std::vector<std::uint8_t> response_frame;
    try {
        const auto encode_started = std::chrono::steady_clock::now();
        const auto payload = encode_search_response(response);
        response_frame = encode_ipc_frame(
            IpcMessageType::search_response, frame.header.request_id, payload);
        diagnostics.encode_microseconds = elapsed_microseconds(encode_started);
    } catch (const std::exception&) {
        diagnostics.error = ERROR_BUFFER_OVERFLOW;
        diagnostics.result_count = response.results.size();
        diagnostics.total_microseconds = elapsed_microseconds(request_started);
        publish_diagnostics(diagnostics_sink, diagnostics);
        DisconnectNamedPipe(pipe.value);
        return ERROR_BUFFER_OVERFLOW;
    }

    const auto write_started = std::chrono::steady_clock::now();
    if (!write_exact(pipe.value, response_frame, io_error)) {
        diagnostics.write_microseconds = elapsed_microseconds(write_started);
        diagnostics.error = io_error;
        diagnostics.result_count = response.results.size();
        diagnostics.total_microseconds = elapsed_microseconds(request_started);
        publish_diagnostics(diagnostics_sink, diagnostics);
        DisconnectNamedPipe(pipe.value);
        return io_error;
    }
    FlushFileBuffers(pipe.value);
    diagnostics.write_microseconds = elapsed_microseconds(write_started);
    diagnostics.error = response.error;
    diagnostics.result_count = response.results.size();
    diagnostics.total_microseconds = elapsed_microseconds(request_started);
    publish_diagnostics(diagnostics_sink, diagnostics);
    DisconnectNamedPipe(pipe.value);
    return ERROR_SUCCESS;
}

std::uint32_t serve_named_pipe_search(
    std::wstring_view pipe_name, const MetadataIndex& index,
    std::atomic_bool& stop, std::size_t worker_count,
    PipeSearchDiagnosticsSink diagnostics) {
    if (worker_count == 0) return ERROR_INVALID_PARAMETER;

    std::atomic<std::uint32_t> fatal_error{ERROR_SUCCESS};
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i) {
        workers.emplace_back([&, pipe = std::wstring(pipe_name)] {
            while (!stop.load(std::memory_order_relaxed)) {
                const auto error =
                    serve_named_pipe_search_once(pipe, index, diagnostics);
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
