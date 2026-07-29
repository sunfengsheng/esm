#include "esm/mft_auto_state.hpp"

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
              "MFT auto state requires Windows UTF-16 wchar_t");

constexpr std::uint64_t magic = 0x314f54554154464dULL; // "MFTAUTO1"
constexpr std::uint16_t version = 1;
constexpr std::size_t header_size = 40;
constexpr std::size_t entry_fixed_size = 56;
constexpr std::size_t max_file_size = 4 * 1024 * 1024;
constexpr std::size_t max_volumes = 1024;
constexpr std::size_t max_string_characters = 32768;
constexpr std::uint32_t live_flag = 1;
constexpr std::uint32_t removable_flag = 2;
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

std::uint64_t checksum(std::span<const std::uint8_t> bytes) noexcept {
    std::uint64_t value = fnv_offset;
    for (const auto byte : bytes) {
        value ^= byte;
        value *= fnv_prime;
    }
    return value;
}

void append_string(std::vector<std::uint8_t>& out,
                   std::wstring_view value) {
    if (value.size() > max_string_characters) {
        throw std::length_error("MFT auto state string too long");
    }
    if (value.empty()) return;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(value.data());
    out.insert(out.end(), bytes, bytes + value.size() * sizeof(wchar_t));
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

bool read_string(std::span<const std::uint8_t> payload,
                 std::size_t& offset,
                 std::uint32_t characters,
                 std::wstring& value) {
    if (characters > max_string_characters ||
        characters > (payload.size() - offset) / sizeof(wchar_t)) {
        return false;
    }
    value.resize(characters);
    const auto bytes = static_cast<std::size_t>(characters) * sizeof(wchar_t);
    if (bytes != 0) std::memcpy(value.data(), payload.data() + offset, bytes);
    offset += bytes;
    return true;
}
} // namespace

std::filesystem::path mft_auto_state_path(
    const std::filesystem::path& snapshot_path) {
    auto value = snapshot_path;
    value += L".state";
    return value;
}

MftAutoStateIoResult save_mft_auto_state_atomic(
    const std::filesystem::path& path, const MftAutoState& state) {
    MftAutoStateIoResult result;
    if (state.generation == 0 || state.volumes.empty() ||
        state.volumes.size() > max_volumes) {
        result.error = ERROR_INVALID_PARAMETER;
        return result;
    }
    try {
        std::vector<std::uint8_t> payload;
        for (const auto& item : state.volumes) {
            if (item.volume.root.empty() || item.volume.identity.empty() ||
                item.root_id == 0 ||
                item.volume.root.size() > max_string_characters ||
                item.volume.mount_path.size() > max_string_characters ||
                item.volume.identity.size() > max_string_characters) {
                result.error = ERROR_INVALID_PARAMETER;
                return result;
            }
            const auto root_characters = static_cast<std::uint32_t>(
                item.volume.root.size());
            const auto mount_characters = static_cast<std::uint32_t>(
                item.volume.mount_path.size());
            const auto identity_characters = static_cast<std::uint32_t>(
                item.volume.identity.size());
            put32(payload, root_characters);
            put32(payload, mount_characters);
            put32(payload, identity_characters);
            put32(payload, 0);
            put64(payload, item.root_id);
            put64(payload, item.journal_id);
            put64(payload, static_cast<std::uint64_t>(item.cursor));
            put32(payload, item.volume.serial_number);
            put32(payload, (item.live ? live_flag : 0) |
                           (item.volume.removable ? removable_flag : 0));
            put64(payload, 0);
            append_string(payload, item.volume.root);
            append_string(payload, item.volume.mount_path);
            append_string(payload, item.volume.identity);
            if (payload.size() > max_file_size - header_size) {
                throw std::length_error("MFT auto state too large");
            }
        }

        std::vector<std::uint8_t> bytes;
        bytes.reserve(header_size + payload.size());
        put64(bytes, magic);
        put16(bytes, version);
        put16(bytes, static_cast<std::uint16_t>(header_size));
        put32(bytes, static_cast<std::uint32_t>(state.volumes.size()));
        put64(bytes, state.generation);
        put64(bytes, payload.size());
        put64(bytes, 0);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        patch64(bytes, 32, checksum(bytes));

        auto temporary = path;
        temporary += L".tmp";
        {
            Handle file{CreateFileW(
                temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr)};
            if (file.value == INVALID_HANDLE_VALUE) {
                result.error = GetLastError();
                return result;
            }
            if (!exact_write(file.value, bytes, result.error) ||
                !FlushFileBuffers(file.value)) {
                if (result.error == ERROR_SUCCESS) result.error = GetLastError();
                DeleteFileW(temporary.c_str());
                return result;
            }
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING |
                             MOVEFILE_WRITE_THROUGH)) {
            result.error = GetLastError();
            DeleteFileW(temporary.c_str());
            return result;
        }
        result.ok = true;
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

MftAutoStateIoResult load_mft_auto_state(
    const std::filesystem::path& path, std::uint64_t expected_generation) {
    MftAutoStateIoResult result;
    if (expected_generation == 0) {
        result.error = ERROR_INVALID_PARAMETER;
        return result;
    }
    try {
        std::error_code size_error;
        const auto file_size = std::filesystem::file_size(path, size_error);
        if (size_error) {
            result.error = static_cast<std::uint32_t>(size_error.value());
            return result;
        }
        if (file_size < header_size || file_size > max_file_size) {
            result.error = ERROR_INVALID_DATA;
            return result;
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file_size));
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            result.error = ERROR_FILE_NOT_FOUND;
            return result;
        }
        input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        if (static_cast<std::size_t>(input.gcount()) != bytes.size()) {
            result.error = ERROR_READ_FAULT;
            return result;
        }
        const auto expected_checksum = get64(bytes.data() + 32);
        patch64(bytes, 32, 0);
        if (get64(bytes.data()) != magic ||
            get16(bytes.data() + 8) != version ||
            get16(bytes.data() + 10) != header_size ||
            checksum(bytes) != expected_checksum) {
            result.error = ERROR_INVALID_DATA;
            return result;
        }
        const auto volume_count = get32(bytes.data() + 12);
        const auto generation = get64(bytes.data() + 16);
        const auto payload_size = get64(bytes.data() + 24);
        if (volume_count == 0 || volume_count > max_volumes ||
            payload_size != bytes.size() - header_size) {
            result.error = ERROR_INVALID_DATA;
            return result;
        }
        if (generation != expected_generation) {
            result.generation_mismatch = true;
            result.error = ERROR_REVISION_MISMATCH;
            return result;
        }

        MftAutoState state;
        state.generation = generation;
        state.volumes.reserve(volume_count);
        const std::span<const std::uint8_t> payload(
            bytes.data() + header_size, bytes.size() - header_size);
        std::size_t offset = 0;
        for (std::uint32_t i = 0; i < volume_count; ++i) {
            if (offset > payload.size() ||
                payload.size() - offset < entry_fixed_size) {
                result.error = ERROR_INVALID_DATA;
                return result;
            }
            const auto* fixed = payload.data() + offset;
            const auto root_characters = get32(fixed);
            const auto mount_characters = get32(fixed + 4);
            const auto identity_characters = get32(fixed + 8);
            const auto reserved = get32(fixed + 12);
            MftAutoVolumeState item;
            item.root_id = get64(fixed + 16);
            item.journal_id = get64(fixed + 24);
            item.cursor = static_cast<std::int64_t>(get64(fixed + 32));
            item.volume.serial_number = get32(fixed + 40);
            const auto flags = get32(fixed + 44);
            if (reserved != 0 || get64(fixed + 48) != 0 ||
                (flags & ~(live_flag | removable_flag)) != 0 ||
                item.root_id == 0) {
                result.error = ERROR_INVALID_DATA;
                return result;
            }
            item.live = (flags & live_flag) != 0;
            item.volume.removable = (flags & removable_flag) != 0;
            offset += entry_fixed_size;
            if (!read_string(payload, offset, root_characters,
                             item.volume.root) ||
                !read_string(payload, offset, mount_characters,
                             item.volume.mount_path) ||
                !read_string(payload, offset, identity_characters,
                             item.volume.identity) ||
                item.volume.root.empty() || item.volume.identity.empty()) {
                result.error = ERROR_INVALID_DATA;
                return result;
            }
            state.volumes.push_back(std::move(item));
        }
        if (offset != payload.size()) {
            result.error = ERROR_INVALID_DATA;
            return result;
        }
        result.state = std::move(state);
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