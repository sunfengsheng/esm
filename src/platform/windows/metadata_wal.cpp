#include "esm/metadata_wal.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>
#include <vector>

namespace esm {
namespace {
static_assert(sizeof(wchar_t) == 2,
              "metadata WAL requires Windows UTF-16 wchar_t");

constexpr std::uint32_t magic = 0x314c4157; // WAL1
constexpr std::uint16_t version = 1;
constexpr std::size_t header_size = 52;
constexpr std::size_t fixed_change_size = 36;
constexpr std::size_t max_transaction_size = 64 * 1024 * 1024;
constexpr std::uint64_t fnv_offset = 1469598103934665603ULL;
constexpr std::uint64_t fnv_prime = 1099511628211ULL;

std::uint64_t checksum_update(std::uint64_t value,
                              std::span<const std::uint8_t> bytes) {
    for (const auto byte : bytes) {
        value ^= byte;
        value *= fnv_prime;
    }
    return value;
}

std::uint64_t checksum(std::span<const std::uint8_t> bytes) {
    return checksum_update(fnv_offset, bytes);
}

void put16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void put64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::uint16_t get16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>(bytes[0]) |
           (static_cast<std::uint16_t>(bytes[1]) << 8);
}

std::uint32_t get32(const std::uint8_t* bytes) {
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8)
        value |= static_cast<std::uint32_t>(*bytes++) << shift;
    return value;
}

std::uint64_t get64(const std::uint8_t* bytes) {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8)
        value |= static_cast<std::uint64_t>(*bytes++) << shift;
    return value;
}

void patch64(std::span<std::uint8_t> out, std::size_t offset,
             std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        out[offset++] = static_cast<std::uint8_t>(value >> shift);
}

struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
};

bool exact_write(HANDLE handle, std::span<const std::uint8_t> bytes,
                 DWORD& error) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD written = 0;
        const auto amount = static_cast<DWORD>((std::min)(
            bytes.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        if (!WriteFile(handle, bytes.data() + offset, amount, &written,
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
    return true;
}

std::vector<std::uint8_t> encode(std::uint64_t journal,
                                 std::int64_t start,
                                 const UsnChangeBatch& batch) {
    if (batch.changes.size() >
        (std::numeric_limits<std::uint32_t>::max)()) {
        throw std::length_error("WAL has too many changes");
    }

    std::vector<std::uint8_t> payload;
    for (const auto& change : batch.changes) {
        if (change.name.size() >
            (std::numeric_limits<std::uint32_t>::max)()) {
            throw std::length_error("WAL name too long");
        }
        const auto name_bytes = change.name.size() * sizeof(wchar_t);
        if (payload.size() > max_transaction_size - fixed_change_size ||
            name_bytes > max_transaction_size - fixed_change_size -
                             payload.size()) {
            throw std::length_error("WAL transaction too large");
        }
        put64(payload, change.file_id);
        put64(payload, change.parent_id);
        put64(payload, static_cast<std::uint64_t>(change.usn));
        put32(payload, change.reason);
        put32(payload, change.attributes);
        put32(payload, static_cast<std::uint32_t>(change.name.size()));
        const auto* raw = reinterpret_cast<const std::uint8_t*>(
            change.name.data());
        payload.insert(payload.end(), raw, raw + name_bytes);
    }

    std::vector<std::uint8_t> out;
    out.reserve(header_size + payload.size());
    put32(out, magic);
    put16(out, version);
    put16(out, static_cast<std::uint16_t>(header_size));
    put32(out, static_cast<std::uint32_t>(payload.size()));
    put64(out, journal);
    put64(out, static_cast<std::uint64_t>(start));
    put64(out, static_cast<std::uint64_t>(batch.next_usn));
    put32(out, static_cast<std::uint32_t>(batch.changes.size()));
    put32(out, 0);
    put64(out, 0);
    out.insert(out.end(), payload.begin(), payload.end());
    patch64(out, 44, checksum(out));
    return out;
}

bool decode_changes(std::span<const std::uint8_t> bytes,
                    std::uint32_t count,
                    UsnChangeBatch& batch) {
    std::size_t offset = 0;
    batch.changes.clear();
    batch.changes.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (offset > bytes.size() ||
            bytes.size() - offset < fixed_change_size) {
            return false;
        }
        UsnChange change;
        change.file_id = get64(bytes.data() + offset);
        offset += 8;
        change.parent_id = get64(bytes.data() + offset);
        offset += 8;
        change.usn = static_cast<std::int64_t>(get64(bytes.data() + offset));
        offset += 8;
        change.reason = get32(bytes.data() + offset);
        offset += 4;
        change.attributes = get32(bytes.data() + offset);
        offset += 4;
        const auto characters = get32(bytes.data() + offset);
        offset += 4;
        if (characters > (bytes.size() - offset) / sizeof(wchar_t))
            return false;
        change.name.resize(characters);
        const auto name_bytes = static_cast<std::size_t>(characters) *
                                sizeof(wchar_t);
        if (name_bytes != 0)
            std::memcpy(change.name.data(), bytes.data() + offset, name_bytes);
        offset += name_bytes;
        batch.changes.push_back(std::move(change));
    }
    return offset == bytes.size();
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

std::filesystem::path metadata_wal_path(
    const std::filesystem::path& checkpoint) {
    auto value = checkpoint;
    value += L".wal";
    return value;
}

MetadataWalResult append_metadata_wal(
    const std::filesystem::path& path,
    std::uint64_t journal,
    std::int64_t start,
    const UsnChangeBatch& batch) {
    MetadataWalResult result;
    result.next_usn = start;
    if (batch.error || batch.next_usn <= start) {
        result.error = batch.error ? batch.error : ERROR_INVALID_DATA;
        return result;
    }
    try {
        std::error_code ec;
        if (const auto parent = path.parent_path(); !parent.empty()) {
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                result.error = static_cast<std::uint32_t>(ec.value());
                return result;
            }
        }
        const auto bytes = encode(journal, start, batch);
        Handle file{CreateFileW(path.c_str(), FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (file.value == INVALID_HANDLE_VALUE) {
            result.error = GetLastError();
            return result;
        }
        DWORD error = ERROR_SUCCESS;
        if (!exact_write(file.value, bytes, error) ||
            !FlushFileBuffers(file.value)) {
            result.error = error ? error : GetLastError();
            return result;
        }
        result.ok = true;
        result.transactions = 1;
        result.changes = batch.changes.size();
        result.valid_bytes = bytes.size();
        result.next_usn = batch.next_usn;
        return result;
    } catch (const std::bad_alloc&) {
        result.error = ERROR_NOT_ENOUGH_MEMORY;
    } catch (...) {
        result.error = ERROR_INVALID_DATA;
    }
    return result;
}

MetadataWalResult replay_metadata_wal(
    const std::filesystem::path& path,
    std::uint64_t journal,
    std::int64_t start,
    NtfsCatalog& catalog,
    MetadataIndex& index) {
    MetadataWalResult result;
    result.next_usn = start;
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec)) {
                result.ok = true;
                return result;
            }
            result.error = ERROR_OPEN_FAILED;
            return result;
        }

        while (true) {
            std::array<std::uint8_t, header_size> header{};
            input.read(reinterpret_cast<char*>(header.data()), header.size());
            const auto header_bytes = static_cast<std::size_t>(input.gcount());
            if (header_bytes == 0) {
                if (!input.eof() && input.fail()) {
                    result.error = ERROR_READ_FAULT;
                    return result;
                }
                break;
            }
            if (header_bytes != header.size()) {
                result.torn_tail = true;
                break;
            }
            if (get32(header.data()) != magic ||
                get16(header.data() + 4) != version ||
                get16(header.data() + 6) != header_size ||
                get32(header.data() + 40) != 0) {
                result.error = ERROR_INVALID_DATA;
                return result;
            }

            const auto payload_size = get32(header.data() + 8);
            if (payload_size > max_transaction_size) {
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

            const auto expected = get64(header.data() + 44);
            patch64(header, 44, 0);
            auto actual = checksum(header);
            actual = checksum_update(actual, payload);
            if (actual != expected) {
                result.error = ERROR_CRC;
                return result;
            }

            const auto transaction_journal = get64(header.data() + 12);
            const auto transaction_start = static_cast<std::int64_t>(
                get64(header.data() + 20));
            const auto transaction_next = static_cast<std::int64_t>(
                get64(header.data() + 28));
            const auto count = get32(header.data() + 36);
            if (transaction_journal != journal ||
                transaction_next <= transaction_start) {
                result.error = ERROR_INVALID_DATA;
                return result;
            }

            result.valid_bytes += header_size + payload_size;
            if (transaction_next <= result.next_usn) continue;
            if (transaction_start != result.next_usn) {
                result.error = ERROR_INVALID_DATA;
                return result;
            }

            UsnChangeBatch batch;
            batch.next_usn = transaction_next;
            if (!decode_changes(payload, count, batch)) {
                result.error = ERROR_INVALID_DATA;
                return result;
            }
            auto delta = catalog.apply(batch);
            index.apply_delta(std::move(delta.upserts), delta.removed_ids);
            result.next_usn = transaction_next;
            ++result.transactions;
            result.changes += batch.changes.size();
        }
        input.close();
        if (result.torn_tail &&
            !truncate_tail(path, result.valid_bytes,
                           result.discarded_tail_bytes, result.error)) {
            return result;
        }
        result.ok = true;
        result.error = ERROR_SUCCESS;
        return result;
    } catch (const std::bad_alloc&) {
        result.error = ERROR_NOT_ENOUGH_MEMORY;
    } catch (...) {
        result.error = ERROR_INVALID_DATA;
    }
    return result;
}

MetadataWalResult reset_metadata_wal(const std::filesystem::path& path) {
    MetadataWalResult result;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        result.ok = true;
        return result;
    }
    auto temporary = path;
    temporary += L".reset.tmp";
    {
        Handle file{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                                nullptr)};
        if (file.value == INVALID_HANDLE_VALUE) {
            result.error = GetLastError();
            return result;
        }
        if (!FlushFileBuffers(file.value)) {
            result.error = GetLastError();
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
    result.error = ERROR_SUCCESS;
    return result;
}
} // namespace esm
