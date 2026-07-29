#include "esm/metadata_hydration_wal.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <vector>

namespace esm {
namespace {
static_assert(sizeof(wchar_t) == 2,
              "metadata hydration WAL requires Windows UTF-16 wchar_t");

constexpr std::uint64_t file_magic = 0x314154454d4d5345ULL; // "ESMMETA1"
constexpr std::uint32_t transaction_magic = 0x3154584dU; // "MXT1" in little-endian bytes
constexpr std::uint16_t format_version = 1;
constexpr std::size_t file_header_size = 32;
constexpr std::size_t transaction_header_size = 64;
constexpr std::size_t update_size = 40;
constexpr std::size_t max_transaction_size = 16 * 1024 * 1024;
constexpr std::uint32_t complete_flag = 1;
constexpr std::uint64_t fnv_offset = 14695981039346656037ULL;
constexpr std::uint64_t fnv_prime = 1099511628211ULL;

struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
};

void put16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void put64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::uint16_t get16(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint16_t>(bytes[0]) |
           (static_cast<std::uint16_t>(bytes[1]) << 8U);
}

std::uint32_t get32(const std::uint8_t* bytes) noexcept {
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8)
        value |= static_cast<std::uint32_t>(*bytes++) << shift;
    return value;
}

std::uint64_t get64(const std::uint8_t* bytes) noexcept {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8)
        value |= static_cast<std::uint64_t>(*bytes++) << shift;
    return value;
}

void patch64(std::span<std::uint8_t> out, std::size_t offset,
             std::uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64; shift += 8)
        out[offset++] = static_cast<std::uint8_t>(value >> shift);
}

std::uint64_t checksum_update(std::uint64_t value,
                              std::span<const std::uint8_t> bytes) noexcept {
    for (const auto byte : bytes) {
        value ^= byte;
        value *= fnv_prime;
    }
    return value;
}

std::uint64_t checksum(std::span<const std::uint8_t> bytes) noexcept {
    return checksum_update(fnv_offset, bytes);
}

std::vector<std::uint8_t> file_header(std::uint64_t generation) {
    std::vector<std::uint8_t> out;
    out.reserve(file_header_size);
    put64(out, file_magic);
    put16(out, format_version);
    put16(out, static_cast<std::uint16_t>(file_header_size));
    put32(out, 0);
    put64(out, generation);
    put64(out, 0);
    patch64(out, 24, checksum(out));
    return out;
}

bool valid_file_header(std::span<const std::uint8_t> bytes,
                       std::uint64_t generation,
                       bool& generation_mismatch) noexcept {
    if (bytes.size() != file_header_size ||
        get64(bytes.data()) != file_magic ||
        get16(bytes.data() + 8) != format_version ||
        get16(bytes.data() + 10) != file_header_size ||
        get32(bytes.data() + 12) != 0) {
        return false;
    }
    std::array<std::uint8_t, file_header_size> copy{};
    std::copy(bytes.begin(), bytes.end(), copy.begin());
    const auto expected = get64(copy.data() + 24);
    patch64(copy, 24, 0);
    if (checksum(copy) != expected) return false;
    generation_mismatch = get64(bytes.data() + 16) != generation;
    return true;
}

bool exact_write(HANDLE file, std::span<const std::uint8_t> bytes,
                 std::uint32_t& error) noexcept {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD written = 0;
        const auto amount = static_cast<DWORD>((std::min)(
            bytes.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        if (!WriteFile(file, bytes.data() + offset, amount, &written, nullptr)) {
            error = GetLastError();
            return false;
        }
        if (written == 0) {
            error = ERROR_WRITE_FAULT;
            return false;
        }
        offset += written;
    }
    return true;
}

bool exact_read(HANDLE file, std::span<std::uint8_t> bytes,
                std::uint32_t& error) noexcept {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD read = 0;
        const auto amount = static_cast<DWORD>((std::min)(
            bytes.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        if (!ReadFile(file, bytes.data() + offset, amount, &read, nullptr)) {
            error = GetLastError();
            return false;
        }
        if (read == 0) {
            error = ERROR_HANDLE_EOF;
            return false;
        }
        offset += read;
    }
    return true;
}

std::vector<std::uint8_t> encode_transaction(
    std::uint64_t generation,
    std::uint64_t after_id,
    std::uint64_t next_id,
    std::size_t examined,
    std::span<const MetadataHydrationWalUpdate> updates,
    bool complete) {
    if (examined > (std::numeric_limits<std::uint32_t>::max)() ||
        updates.size() > (std::numeric_limits<std::uint32_t>::max)() ||
        updates.size() > examined ||
        updates.size() > max_transaction_size / update_size) {
        throw std::length_error("metadata hydration WAL transaction too large");
    }
    const auto payload_size = updates.size() * update_size;
    std::vector<std::uint8_t> out;
    out.reserve(transaction_header_size + payload_size);
    put32(out, transaction_magic);
    put16(out, format_version);
    put16(out, static_cast<std::uint16_t>(transaction_header_size));
    put32(out, static_cast<std::uint32_t>(payload_size));
    put32(out, complete ? complete_flag : 0);
    put64(out, generation);
    put64(out, after_id);
    put64(out, next_id);
    put32(out, static_cast<std::uint32_t>(examined));
    put32(out, static_cast<std::uint32_t>(updates.size()));
    put64(out, 0);
    put64(out, 0);
    for (const auto& update : updates) {
        put64(out, update.id);
        put64(out, update.path_fingerprint);
        put64(out, update.size);
        put64(out, static_cast<std::uint64_t>(update.last_write_time));
        put32(out, update.attributes);
        put32(out, 0);
    }
    patch64(out, 56, checksum(out));
    return out;
}

bool truncate_tail(const std::filesystem::path& path,
                   std::uint64_t valid_bytes,
                   std::uint64_t& discarded,
                   std::uint32_t& error) {
    std::error_code ec;
    const auto original = std::filesystem::file_size(path, ec);
    if (ec) {
        error = static_cast<std::uint32_t>(ec.value());
        return false;
    }
    if (original <= valid_bytes) return true;
    std::filesystem::resize_file(path, valid_bytes, ec);
    if (ec) {
        error = static_cast<std::uint32_t>(ec.value());
        return false;
    }
    discarded = original - valid_bytes;
    return true;
}
} // namespace

std::uint64_t metadata_path_fingerprint(std::wstring_view path) noexcept {
    if (path.empty()) return fnv_offset;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(path.data());
    return checksum_update(
        fnv_offset, {bytes, path.size() * sizeof(wchar_t)});
}

MetadataHydrationWalUpdate make_metadata_hydration_wal_update(
    const FileRecord& record) noexcept {
    MetadataHydrationWalUpdate update;
    update.id = record.id;
    update.path_fingerprint = metadata_path_fingerprint(record.path);
    update.size = record.size;
    update.last_write_time = record.last_write_time;
    update.attributes = record.directory
        ? record.attributes | FILE_ATTRIBUTE_DIRECTORY
        : record.attributes & ~FILE_ATTRIBUTE_DIRECTORY;
    return update;
}

std::filesystem::path metadata_hydration_wal_path(
    const std::filesystem::path& snapshot_path) {
    auto value = snapshot_path;
    value += L".metadata.wal";
    return value;
}

MetadataHydrationWalResult initialize_metadata_hydration_wal(
    const std::filesystem::path& path, std::uint64_t generation) {
    MetadataHydrationWalResult result;
    result.generation = generation;
    if (generation == 0) {
        result.error = ERROR_INVALID_PARAMETER;
        return result;
    }
    auto temporary = path;
    temporary += L".tmp";
    const auto header = file_header(generation);
    {
        Handle file{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                                nullptr)};
        if (file.value == INVALID_HANDLE_VALUE) {
            result.error = GetLastError();
            return result;
        }
        if (!exact_write(file.value, header, result.error) ||
            !FlushFileBuffers(file.value)) {
            if (result.error == ERROR_SUCCESS) result.error = GetLastError();
            DeleteFileW(temporary.c_str());
            return result;
        }
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        result.error = GetLastError();
        DeleteFileW(temporary.c_str());
        return result;
    }
    result.ok = true;
    result.valid_bytes = file_header_size;
    result.error = ERROR_SUCCESS;
    return result;
}

MetadataHydrationWalResult append_metadata_hydration_wal(
    const std::filesystem::path& path,
    std::uint64_t generation,
    std::uint64_t after_id,
    std::uint64_t next_id,
    std::size_t examined,
    std::span<const MetadataHydrationWalUpdate> updates,
    bool complete) {
    MetadataHydrationWalResult result;
    result.generation = generation;
    if (generation == 0 || next_id < after_id ||
        (examined == 0 && !complete)) {
        result.error = ERROR_INVALID_PARAMETER;
        return result;
    }
    try {
        const auto transaction = encode_transaction(
            generation, after_id, next_id, examined, updates, complete);
        Handle file{CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                                nullptr)};
        if (file.value == INVALID_HANDLE_VALUE) {
            result.error = GetLastError();
            return result;
        }
        std::array<std::uint8_t, file_header_size> header{};
        if (!exact_read(file.value, header, result.error)) return result;
        if (!valid_file_header(header, generation,
                               result.generation_mismatch)) {
            result.error = ERROR_INVALID_DATA;
            return result;
        }
        if (result.generation_mismatch) {
            result.error = ERROR_REVISION_MISMATCH;
            return result;
        }
        LARGE_INTEGER end{};
        if (!SetFilePointerEx(file.value, {}, &end, FILE_END)) {
            result.error = GetLastError();
            return result;
        }
        if (!exact_write(file.value, transaction, result.error) ||
            !FlushFileBuffers(file.value)) {
            if (result.error == ERROR_SUCCESS) result.error = GetLastError();
            return result;
        }
        result.ok = true;
        result.complete = complete;
        result.next_id = next_id;
        result.transactions = 1;
        result.examined = examined;
        result.updates = updates.size();
        result.valid_bytes = static_cast<std::uint64_t>(end.QuadPart) +
                             transaction.size();
        result.error = ERROR_SUCCESS;
    } catch (const std::bad_alloc&) {
        result.error = ERROR_NOT_ENOUGH_MEMORY;
    } catch (const std::length_error&) {
        result.error = ERROR_BUFFER_OVERFLOW;
    } catch (...) {
        result.error = ERROR_INVALID_DATA;
    }
    return result;
}

MetadataHydrationWalResult replay_metadata_hydration_wal(
    const std::filesystem::path& path,
    std::uint64_t generation,
    std::vector<FileRecord>& records) {
    MetadataHydrationWalResult result;
    result.generation = generation;
    if (generation == 0) {
        result.error = ERROR_INVALID_PARAMETER;
        return result;
    }
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            result.error = ERROR_FILE_NOT_FOUND;
            return result;
        }
        std::array<std::uint8_t, file_header_size> file_header_bytes{};
        input.read(reinterpret_cast<char*>(file_header_bytes.data()),
                   file_header_bytes.size());
        if (static_cast<std::size_t>(input.gcount()) !=
            file_header_bytes.size()) {
            result.error = ERROR_INVALID_DATA;
            return result;
        }
        if (!valid_file_header(file_header_bytes, generation,
                               result.generation_mismatch)) {
            result.error = ERROR_INVALID_DATA;
            return result;
        }
        if (result.generation_mismatch) {
            result.error = ERROR_REVISION_MISMATCH;
            return result;
        }
        result.valid_bytes = file_header_size;
        std::sort(records.begin(), records.end(),
                  [](const FileRecord& left, const FileRecord& right) {
                      return left.id < right.id;
                  });
        std::size_t record_position = 0;
        std::uint64_t previous_update_id = 0;
        bool have_previous_update = false;

        for (;;) {
            std::array<std::uint8_t, transaction_header_size> header{};
            input.read(reinterpret_cast<char*>(header.data()), header.size());
            const auto header_read = static_cast<std::size_t>(input.gcount());
            if (header_read == 0) break;
            if (header_read != header.size()) {
                result.torn_tail = true;
                break;
            }
            if (get32(header.data()) != transaction_magic ||
                get16(header.data() + 4) != format_version ||
                get16(header.data() + 6) != transaction_header_size ||
                (get32(header.data() + 12) & ~complete_flag) != 0 ||
                get64(header.data() + 48) != 0) {
                result.error = ERROR_INVALID_DATA;
                return result;
            }
            const auto payload_size = get32(header.data() + 8);
            const auto update_count = get32(header.data() + 44);
            if (payload_size > max_transaction_size ||
                payload_size != static_cast<std::uint64_t>(update_count) *
                                    update_size) {
                result.error = ERROR_INVALID_DATA;
                return result;
            }
            std::vector<std::uint8_t> payload(payload_size);
            if (payload_size != 0) {
                input.read(reinterpret_cast<char*>(payload.data()),
                           payload.size());
                if (static_cast<std::size_t>(input.gcount()) != payload.size()) {
                    result.torn_tail = true;
                    break;
                }
            }
            const auto expected_checksum = get64(header.data() + 56);
            patch64(header, 56, 0);
            auto actual_checksum = checksum(header);
            actual_checksum = checksum_update(actual_checksum, payload);
            if (actual_checksum != expected_checksum) {
                result.error = ERROR_CRC;
                return result;
            }
            const auto transaction_generation = get64(header.data() + 16);
            const auto after_id = get64(header.data() + 24);
            const auto next_id = get64(header.data() + 32);
            const auto examined = get32(header.data() + 40);
            const bool complete =
                (get32(header.data() + 12) & complete_flag) != 0;
            if (transaction_generation != generation ||
                after_id != result.next_id || next_id < after_id ||
                update_count > examined || (examined == 0 && !complete)) {
                result.error = ERROR_INVALID_DATA;
                return result;
            }

            std::size_t offset = 0;
            for (std::uint32_t i = 0; i < update_count; ++i) {
                MetadataHydrationWalUpdate update;
                update.id = get64(payload.data() + offset);
                update.path_fingerprint = get64(payload.data() + offset + 8);
                update.size = get64(payload.data() + offset + 16);
                update.last_write_time = static_cast<std::int64_t>(
                    get64(payload.data() + offset + 24));
                update.attributes = get32(payload.data() + offset + 32);
                if (get32(payload.data() + offset + 36) != 0 ||
                    (have_previous_update && update.id <= previous_update_id)) {
                    result.error = ERROR_INVALID_DATA;
                    return result;
                }
                previous_update_id = update.id;
                have_previous_update = true;
                while (record_position < records.size() &&
                       records[record_position].id < update.id) {
                    ++record_position;
                }
                if (record_position < records.size() &&
                    records[record_position].id == update.id &&
                    metadata_path_fingerprint(records[record_position].path) ==
                        update.path_fingerprint) {
                    auto& record = records[record_position];
                    record.size = update.size;
                    record.last_write_time = update.last_write_time;
                    record.attributes = update.attributes;
                    record.directory =
                        (update.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    ++result.applied;
                } else {
                    ++result.stale;
                }
                offset += update_size;
            }
            result.next_id = next_id;
            result.complete = complete;
            ++result.transactions;
            result.examined += examined;
            result.updates += update_count;
            result.valid_bytes += transaction_header_size + payload_size;
            if (complete) {
                // A completed generation must be the durable tail. Additional
                // transactions would indicate accidental generation reuse.
                const auto next = input.peek();
                if (next != std::char_traits<char>::eof()) {
                    result.error = ERROR_INVALID_DATA;
                    return result;
                }
                break;
            }
        }
        input.close();
        if (result.torn_tail &&
            !truncate_tail(path, result.valid_bytes,
                           result.discarded_tail_bytes, result.error)) {
            return result;
        }
        result.ok = true;
        result.error = ERROR_SUCCESS;
    } catch (const std::bad_alloc&) {
        result.error = ERROR_NOT_ENOUGH_MEMORY;
    } catch (...) {
        result.error = ERROR_INVALID_DATA;
    }
    return result;
}
} // namespace esm