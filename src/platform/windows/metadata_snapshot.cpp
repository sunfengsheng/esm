#include "esm/metadata_snapshot.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <system_error>
#include <type_traits>

namespace esm {
namespace {
static_assert(sizeof(wchar_t) == 2,
              "metadata snapshots require Windows UTF-16 wchar_t");

constexpr std::uint64_t magic = 0x3150414e534d5345ULL; // "ESMSNAP1"
constexpr std::uint32_t legacy_format_version = 1;
constexpr std::uint32_t compact_format_version = 2;
constexpr std::size_t legacy_fixed_header_size = 56;
constexpr std::size_t compact_fixed_header_size = 80;
constexpr std::size_t legacy_fixed_record_size = 48;
constexpr std::size_t compact_fixed_record_size = sizeof(CatalogBaseNode);
constexpr std::size_t checksum_size = 8;
constexpr std::size_t io_buffer_size = 1024 * 1024;
static_assert(std::is_trivially_copyable_v<CatalogBaseNode>);
static_assert(compact_fixed_header_size % alignof(CatalogBaseNode) == 0);
static_assert(compact_fixed_record_size % alignof(wchar_t) == 0);
constexpr std::uint64_t max_snapshot_size = 8ULL * 1024 * 1024 * 1024;
constexpr std::uint64_t max_record_count = 100'000'000;
constexpr std::uint32_t max_string_characters = 1'048'576;
constexpr std::uint32_t corrupt_data_error = ERROR_INVALID_DATA;
constexpr std::uint64_t fnv_offset = 14695981039346656037ULL;
constexpr std::uint64_t fnv_prime = 1099511628211ULL;

struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
};

struct MappedFile {
    HANDLE file{INVALID_HANDLE_VALUE};
    HANDLE mapping{};
    const unsigned char* view{};
    std::size_t size{};

    ~MappedFile() {
        if (view != nullptr) UnmapViewOfFile(view);
        if (mapping != nullptr) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
};

void hash_bytes(std::uint64_t& hash, const unsigned char* data,
                std::size_t size) noexcept {
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= fnv_prime;
    }
}

void put_u32(std::array<unsigned char, 8>& bytes, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes[i] = static_cast<unsigned char>(value >> (i * 8));
}

void put_u64(std::array<unsigned char, 8>& bytes, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        bytes[i] = static_cast<unsigned char>(value >> (i * 8));
}

std::uint32_t get_u32(const unsigned char* bytes) noexcept {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= static_cast<std::uint32_t>(bytes[i]) << (i * 8);
    return value;
}

std::uint64_t get_u64(const unsigned char* bytes) noexcept {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8);
    return value;
}

bool write_exact(HANDLE file, const void* data, std::size_t size,
                 std::uint32_t& error) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::size_t offset = 0;
    while (offset < size) {
        const DWORD chunk = static_cast<DWORD>(
            (std::min)(size - offset,
                       static_cast<std::size_t>(
                           (std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (!WriteFile(file, bytes + offset, chunk, &written, nullptr)) {
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

class SnapshotWriter {
public:
    explicit SnapshotWriter(HANDLE file)
        : file_(file), buffer_(io_buffer_size) {}

    bool bytes(const void* data, std::size_t size, std::uint32_t& error) {
        const auto* input = static_cast<const unsigned char*>(data);
        hash_bytes(hash_, input, size);
        while (size != 0) {
            if (used_ == 0 && size >= buffer_.size()) {
                if (!write_exact(file_, input, size, error)) return false;
                return true;
            }
            const auto available = buffer_.size() - used_;
            const auto chunk = (std::min)(available, size);
            std::copy_n(input, chunk, buffer_.data() + used_);
            used_ += chunk;
            input += chunk;
            size -= chunk;
            if (used_ == buffer_.size() && !flush(error)) return false;
        }
        error = ERROR_SUCCESS;
        return true;
    }

    bool flush(std::uint32_t& error) {
        if (used_ == 0) {
            error = ERROR_SUCCESS;
            return true;
        }
        if (!write_exact(file_, buffer_.data(), used_, error)) return false;
        used_ = 0;
        return true;
    }

    bool u8(std::uint8_t value, std::uint32_t& error) {
        return bytes(&value, 1, error);
    }

    bool u32(std::uint32_t value, std::uint32_t& error) {
        std::array<unsigned char, 8> encoded{};
        put_u32(encoded, value);
        return bytes(encoded.data(), 4, error);
    }

    bool u64(std::uint64_t value, std::uint32_t& error) {
        std::array<unsigned char, 8> encoded{};
        put_u64(encoded, value);
        return bytes(encoded.data(), 8, error);
    }

    bool i64(std::int64_t value, std::uint32_t& error) {
        return u64(static_cast<std::uint64_t>(value), error);
    }

    std::uint64_t hash() const noexcept { return hash_; }

private:
    HANDLE file_;
    std::vector<unsigned char> buffer_;
    std::size_t used_{};
    std::uint64_t hash_{fnv_offset};
};

class SnapshotReader {
public:
    explicit SnapshotReader(HANDLE file)
        : file_(file), buffer_(io_buffer_size) {}

    bool bytes(void* data, std::size_t size, std::uint32_t& error) {
        return consume(data, size, true, error);
    }

    bool raw_bytes(void* data, std::size_t size, std::uint32_t& error) {
        return consume(data, size, false, error);
    }

    bool u8(std::uint8_t& value, std::uint32_t& error) {
        return bytes(&value, 1, error);
    }

    bool u32(std::uint32_t& value, std::uint32_t& error) {
        std::array<unsigned char, 4> encoded{};
        if (!bytes(encoded.data(), encoded.size(), error)) return false;
        value = get_u32(encoded.data());
        return true;
    }

    bool u64(std::uint64_t& value, std::uint32_t& error) {
        std::array<unsigned char, 8> encoded{};
        if (!bytes(encoded.data(), encoded.size(), error)) return false;
        value = get_u64(encoded.data());
        return true;
    }

    bool i64(std::int64_t& value, std::uint32_t& error) {
        std::uint64_t encoded = 0;
        if (!u64(encoded, error)) return false;
        value = static_cast<std::int64_t>(encoded);
        return true;
    }

    std::uint64_t hash() const noexcept { return hash_; }

private:
    bool refill(std::uint32_t& error) {
        DWORD read = 0;
        if (!ReadFile(file_, buffer_.data(),
                      static_cast<DWORD>(buffer_.size()), &read, nullptr)) {
            error = GetLastError();
            return false;
        }
        if (read == 0) {
            error = corrupt_data_error;
            return false;
        }
        offset_ = 0;
        available_ = read;
        return true;
    }

    bool consume(void* data, std::size_t size, bool include_in_hash,
                 std::uint32_t& error) {
        auto* output = static_cast<unsigned char*>(data);
        const auto* hash_start = output;
        const auto original_size = size;
        while (size != 0) {
            if (offset_ == available_ && !refill(error)) return false;
            const auto chunk = (std::min)(
                size, static_cast<std::size_t>(available_ - offset_));
            std::copy_n(buffer_.data() + offset_, chunk, output);
            offset_ += chunk;
            output += chunk;
            size -= chunk;
        }
        if (include_in_hash) hash_bytes(hash_, hash_start, original_size);
        error = ERROR_SUCCESS;
        return true;
    }

    HANDLE file_;
    std::vector<unsigned char> buffer_;
    std::size_t offset_{};
    std::size_t available_{};
    std::uint64_t hash_{fnv_offset};
};

bool add_size(std::uint64_t& total, std::uint64_t value) noexcept {
    if (value > (std::numeric_limits<std::uint64_t>::max)() - total)
        return false;
    total += value;
    return true;
}

bool string_bytes(std::wstring_view value, std::uint64_t& bytes) noexcept {
    if (value.size() > max_string_characters ||
        value.size() > (std::numeric_limits<std::uint32_t>::max)())
        return false;
    bytes = static_cast<std::uint64_t>(value.size()) * sizeof(wchar_t);
    return true;
}

bool compute_payload_size(const MetadataSnapshot& snapshot,
                          std::uint64_t& payload_size) noexcept {
    if (snapshot.records.size() > max_record_count) return false;
    std::uint64_t volume_bytes = 0;
    if (!string_bytes(snapshot.volume, volume_bytes)) return false;
    payload_size = 4;
    if (!add_size(payload_size, volume_bytes)) return false;
    for (const auto& record : snapshot.records) {
        std::uint64_t name_bytes = 0;
        std::uint64_t path_bytes = 0;
        if (!string_bytes(record.name, name_bytes) ||
            !string_bytes(record.path, path_bytes) ||
            !add_size(payload_size, legacy_fixed_record_size) ||
            !add_size(payload_size, name_bytes) ||
            !add_size(payload_size, path_bytes)) {
            return false;
        }
    }
    return payload_size <= max_snapshot_size - legacy_fixed_header_size -
                               checksum_size;
}

std::filesystem::path temporary_path(const std::filesystem::path& path) {
    auto temporary = path;
    temporary += L".tmp";
    return temporary;
}

bool write_string(SnapshotWriter& writer, std::wstring_view value,
                  std::uint32_t& error) {
    if (!writer.u32(static_cast<std::uint32_t>(value.size()), error))
        return false;
    return value.empty() || writer.bytes(
        value.data(), value.size() * sizeof(wchar_t), error);
}

bool read_string(SnapshotReader& reader, std::wstring& value,
                 std::uint64_t& payload_remaining,
                 std::uint32_t& error) {
    if (payload_remaining < 4) {
        error = corrupt_data_error;
        return false;
    }
    std::uint32_t length = 0;
    if (!reader.u32(length, error)) return false;
    payload_remaining -= 4;
    if (length > max_string_characters) {
        error = corrupt_data_error;
        return false;
    }
    const std::uint64_t byte_count =
        static_cast<std::uint64_t>(length) * sizeof(wchar_t);
    if (byte_count > payload_remaining ||
        byte_count > (std::numeric_limits<std::size_t>::max)()) {
        error = corrupt_data_error;
        return false;
    }
    value.resize(length);
    if (byte_count != 0 && !reader.bytes(
            value.data(), static_cast<std::size_t>(byte_count), error))
        return false;
    payload_remaining -= byte_count;
    return true;
}


bool compact_snapshot_shape(const MetadataSnapshot& snapshot,
                            std::uint64_t& payload_size,
                            std::uint64_t& name_characters) noexcept {
    if (snapshot.records.size() > max_record_count ||
        snapshot.volume.size() > max_string_characters ||
        snapshot.volume.size() >
            (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    name_characters = 0;
    std::uint64_t previous_id = 0;
    for (std::size_t i = 0; i < snapshot.records.size(); ++i) {
        const auto& record = snapshot.records[i];
        if (!record.path.empty() || record.id == 0 ||
            (i != 0 && record.id <= previous_id) ||
            record.name.size() > max_string_characters ||
            record.name.size() >
                (std::numeric_limits<std::uint32_t>::max)() ||
            name_characters >
                (std::numeric_limits<std::uint32_t>::max)() -
                    record.name.size()) {
            return false;
        }
        name_characters += record.name.size();
        previous_id = record.id;
    }

    const auto record_bytes =
        static_cast<std::uint64_t>(snapshot.records.size()) *
        compact_fixed_record_size;
    const auto name_bytes = name_characters * sizeof(wchar_t);
    const auto volume_bytes =
        static_cast<std::uint64_t>(snapshot.volume.size()) * sizeof(wchar_t);
    payload_size = record_bytes;
    return add_size(payload_size, name_bytes) &&
           add_size(payload_size, volume_bytes) &&
           payload_size <= max_snapshot_size - compact_fixed_header_size -
                               checksum_size;
}

MetadataSnapshotIoResult save_compact_snapshot_atomic(
    const std::filesystem::path& path,
    const MetadataSnapshot& snapshot,
    std::uint64_t payload_size,
    std::uint64_t name_characters) {
    MetadataSnapshotIoResult result;
    std::error_code directory_error;
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, directory_error);
        if (directory_error) {
            result.error = static_cast<std::uint32_t>(directory_error.value());
            return result;
        }
    }

    const auto temporary = temporary_path(path);
    Handle file{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) {
        result.error = GetLastError();
        return result;
    }

    SnapshotWriter writer(file.value);
    std::uint32_t error = ERROR_SUCCESS;
    if (!writer.u64(magic, error) ||
        !writer.u32(compact_format_version, error) ||
        !writer.u32(0, error) ||
        !writer.u64(snapshot.checkpoint.journal_id, error) ||
        !writer.i64(snapshot.checkpoint.next_usn, error) ||
        !writer.u64(snapshot.root_id, error) ||
        !writer.u64(snapshot.records.size(), error) ||
        !writer.u64(payload_size, error) ||
        !writer.u32(static_cast<std::uint32_t>(snapshot.volume.size()), error) ||
        !writer.u32(static_cast<std::uint32_t>(compact_fixed_record_size), error) ||
        !writer.u64(name_characters, error) ||
        !writer.u64(0, error)) {
        result.error = error;
        return result;
    }

    const std::array<unsigned char, 3> reserved{};
    std::uint32_t name_offset = 0;
    for (const auto& record : snapshot.records) {
        if (!writer.u64(record.id, error) ||
            !writer.u64(record.parent_id, error) ||
            !writer.u64(record.size, error) ||
            !writer.i64(record.last_write_time, error) ||
            !writer.u32(record.attributes, error) ||
            !writer.u32(name_offset, error) ||
            !writer.u32(static_cast<std::uint32_t>(record.name.size()), error) ||
            !writer.u8(record.directory ? 1 : 0, error) ||
            !writer.bytes(reserved.data(), reserved.size(), error)) {
            result.error = error;
            return result;
        }
        name_offset += static_cast<std::uint32_t>(record.name.size());
    }
    for (const auto& record : snapshot.records) {
        if (!record.name.empty() &&
            !writer.bytes(record.name.data(),
                          record.name.size() * sizeof(wchar_t), error)) {
            result.error = error;
            return result;
        }
    }
    if (!snapshot.volume.empty() &&
        !writer.bytes(snapshot.volume.data(),
                      snapshot.volume.size() * sizeof(wchar_t), error)) {
        result.error = error;
        return result;
    }

    std::array<unsigned char, 8> checksum{};
    put_u64(checksum, writer.hash());
    if (!writer.flush(error) ||
        !write_exact(file.value, checksum.data(), checksum.size(), error) ||
        !FlushFileBuffers(file.value)) {
        result.error = error == ERROR_SUCCESS ? GetLastError() : error;
        return result;
    }
    CloseHandle(file.value);
    file.value = INVALID_HANDLE_VALUE;
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        result.error = GetLastError();
        return result;
    }
    result.ok = true;
    result.error = ERROR_SUCCESS;
    return result;
}

MetadataSnapshotIoResult save_compact_catalog_atomic(
    const std::filesystem::path& path,
    JournalCheckpoint checkpoint,
    std::uint64_t root_id,
    std::wstring_view volume,
    std::span<const CatalogBaseNode> nodes,
    std::span<const wchar_t> names) {
    MetadataSnapshotIoResult result;
    if (root_id == 0 || volume.empty() ||
        nodes.size() > max_record_count ||
        volume.size() > max_string_characters ||
        names.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        result.error = ERROR_INVALID_DATA;
        return result;
    }

    std::uint64_t previous_id = 0;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        const auto name_end = static_cast<std::uint64_t>(node.name_offset) +
                              node.name_length;
        if (node.id == 0 || (i != 0 && node.id <= previous_id) ||
            name_end > names.size()) {
            result.error = ERROR_INVALID_DATA;
            return result;
        }
        previous_id = node.id;
    }

    std::uint64_t payload_size = nodes.size_bytes();
    if (!add_size(payload_size, names.size_bytes()) ||
        !add_size(payload_size, volume.size() * sizeof(wchar_t)) ||
        payload_size > max_snapshot_size - compact_fixed_header_size -
                           checksum_size) {
        result.error = ERROR_INVALID_DATA;
        return result;
    }

    std::error_code directory_error;
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, directory_error);
        if (directory_error) {
            result.error = static_cast<std::uint32_t>(directory_error.value());
            return result;
        }
    }

    const auto temporary = temporary_path(path);
    Handle file{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) {
        result.error = GetLastError();
        return result;
    }

    SnapshotWriter writer(file.value);
    std::uint32_t error = ERROR_SUCCESS;
    if (!writer.u64(magic, error) ||
        !writer.u32(compact_format_version, error) ||
        !writer.u32(0, error) ||
        !writer.u64(checkpoint.journal_id, error) ||
        !writer.i64(checkpoint.next_usn, error) ||
        !writer.u64(root_id, error) ||
        !writer.u64(nodes.size(), error) ||
        !writer.u64(payload_size, error) ||
        !writer.u32(static_cast<std::uint32_t>(volume.size()), error) ||
        !writer.u32(static_cast<std::uint32_t>(compact_fixed_record_size), error) ||
        !writer.u64(names.size(), error) ||
        !writer.u64(0, error) ||
        (!nodes.empty() && !writer.bytes(nodes.data(), nodes.size_bytes(), error)) ||
        (!names.empty() && !writer.bytes(names.data(), names.size_bytes(), error)) ||
        !writer.bytes(volume.data(), volume.size() * sizeof(wchar_t), error)) {
        result.error = error;
        return result;
    }

    std::array<unsigned char, 8> checksum{};
    put_u64(checksum, writer.hash());
    if (!writer.flush(error) ||
        !write_exact(file.value, checksum.data(), checksum.size(), error) ||
        !FlushFileBuffers(file.value)) {
        result.error = error == ERROR_SUCCESS ? GetLastError() : error;
        return result;
    }
    CloseHandle(file.value);
    file.value = INVALID_HANDLE_VALUE;
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        result.error = GetLastError();
        return result;
    }
    result.ok = true;
    result.error = ERROR_SUCCESS;
    return result;
}

} // namespace

std::filesystem::path metadata_snapshot_path(
    const std::filesystem::path& checkpoint_path) {
    auto path = checkpoint_path;
    path += L".metadata";
    return path;
}

MetadataSnapshotIoResult save_metadata_snapshot_atomic(
    const std::filesystem::path& path,
    const MetadataSnapshot& snapshot) {
    MetadataSnapshotIoResult result;
    std::uint64_t compact_payload_size = 0;
    std::uint64_t compact_name_characters = 0;
    if (snapshot.root_id != 0 && !snapshot.volume.empty() &&
        compact_snapshot_shape(snapshot, compact_payload_size,
                               compact_name_characters)) {
        return save_compact_snapshot_atomic(
            path, snapshot, compact_payload_size, compact_name_characters);
    }

    std::uint64_t payload_size = 0;
    if (snapshot.root_id == 0 || snapshot.volume.empty() ||
        !compute_payload_size(snapshot, payload_size)) {
        result.error = ERROR_INVALID_DATA;
        return result;
    }

    std::error_code directory_error;
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, directory_error);
        if (directory_error) {
            result.error = static_cast<std::uint32_t>(directory_error.value());
            return result;
        }
    }

    const auto temporary = temporary_path(path);
    Handle file{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) {
        result.error = GetLastError();
        return result;
    }

    SnapshotWriter writer(file.value);
    std::uint32_t error = ERROR_SUCCESS;
    const auto write_header = [&] {
        return writer.u64(magic, error) &&
               writer.u32(legacy_format_version, error) &&
               writer.u32(0, error) &&
               writer.u64(snapshot.checkpoint.journal_id, error) &&
               writer.i64(snapshot.checkpoint.next_usn, error) &&
               writer.u64(snapshot.root_id, error) &&
               writer.u64(snapshot.records.size(), error) &&
               writer.u64(payload_size, error);
    };
    if (!write_header() || !write_string(writer, snapshot.volume, error)) {
        result.error = error;
        return result;
    }

    const std::array<unsigned char, 3> reserved{};
    for (const auto& record : snapshot.records) {
        if (!writer.u64(record.id, error) ||
            !writer.u64(record.parent_id, error) ||
            !writer.u64(record.size, error) ||
            !writer.i64(record.last_write_time, error) ||
            !writer.u32(record.attributes, error) ||
            !writer.u8(record.directory ? 1 : 0, error) ||
            !writer.bytes(reserved.data(), reserved.size(), error) ||
            !write_string(writer, record.name, error) ||
            !write_string(writer, record.path, error)) {
            result.error = error;
            return result;
        }
    }

    std::array<unsigned char, 8> checksum{};
    put_u64(checksum, writer.hash());
    if (!writer.flush(error) ||
        !write_exact(file.value, checksum.data(), checksum.size(), error) ||
        !FlushFileBuffers(file.value)) {
        result.error = error == ERROR_SUCCESS ? GetLastError() : error;
        return result;
    }
    CloseHandle(file.value);
    file.value = INVALID_HANDLE_VALUE;

    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        result.error = GetLastError();
        return result;
    }
    result.ok = true;
    result.error = ERROR_SUCCESS;
    return result;
}

MetadataSnapshotIoResult save_metadata_catalog_snapshot_atomic(
    const std::filesystem::path& path,
    JournalCheckpoint checkpoint,
    std::uint64_t root_id,
    std::wstring_view volume,
    const NtfsCatalog& catalog) {
    auto view = catalog.storage_view();
    if (!view) return {false, ERROR_INVALID_STATE};
    return save_compact_catalog_atomic(path, checkpoint, root_id, volume,
                                       view.nodes(), view.names());
}

MappedMetadataSnapshotLoadResult load_metadata_snapshot_mapped(
    const std::filesystem::path& path) {
    MappedMetadataSnapshotLoadResult result;
    try {
        auto mapped = std::make_shared<MappedFile>();
        mapped->file = CreateFileW(
            path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (mapped->file == INVALID_HANDLE_VALUE) {
            result.error = GetLastError();
            return result;
        }

        LARGE_INTEGER file_size{};
        if (!GetFileSizeEx(mapped->file, &file_size)) {
            result.error = GetLastError();
            return result;
        }
        if (file_size.QuadPart < 16 ||
            static_cast<std::uint64_t>(file_size.QuadPart) > max_snapshot_size ||
            static_cast<std::uint64_t>(file_size.QuadPart) >
                (std::numeric_limits<std::size_t>::max)()) {
            result.error = corrupt_data_error;
            return result;
        }
        mapped->size = static_cast<std::size_t>(file_size.QuadPart);
        mapped->mapping = CreateFileMappingW(
            mapped->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapped->mapping == nullptr) {
            result.error = GetLastError();
            return result;
        }
        mapped->view = static_cast<const unsigned char*>(
            MapViewOfFile(mapped->mapping, FILE_MAP_READ, 0, 0, 0));
        if (mapped->view == nullptr) {
            result.error = GetLastError();
            return result;
        }

        const auto* bytes = mapped->view;
        const auto read_magic = get_u64(bytes);
        const auto version = get_u32(bytes + 8);
        if (read_magic != magic) {
            result.error = corrupt_data_error;
            return result;
        }
        if (version != compact_format_version) {
            result.error = version == legacy_format_version
                ? ERROR_REVISION_MISMATCH
                : corrupt_data_error;
            return result;
        }
        if (mapped->size < compact_fixed_header_size + checksum_size ||
            get_u32(bytes + 12) != 0) {
            result.error = corrupt_data_error;
            return result;
        }

        const auto journal_id = get_u64(bytes + 16);
        const auto next_usn = static_cast<std::int64_t>(get_u64(bytes + 24));
        const auto root_id = get_u64(bytes + 32);
        const auto record_count = get_u64(bytes + 40);
        const auto payload_size = get_u64(bytes + 48);
        const auto volume_characters = get_u32(bytes + 56);
        const auto record_size = get_u32(bytes + 60);
        const auto name_characters = get_u64(bytes + 64);
        const auto header_reserved = get_u64(bytes + 72);
        if (root_id == 0 || record_count > max_record_count ||
            volume_characters == 0 ||
            volume_characters > max_string_characters ||
            record_size != compact_fixed_record_size ||
            name_characters >
                (std::numeric_limits<std::uint32_t>::max)() ||
            header_reserved != 0 ||
            payload_size > max_snapshot_size - compact_fixed_header_size -
                               checksum_size) {
            result.error = corrupt_data_error;
            return result;
        }

        std::uint64_t calculated_payload =
            record_count * compact_fixed_record_size;
        if (!add_size(calculated_payload,
                      name_characters * sizeof(wchar_t)) ||
            !add_size(calculated_payload,
                      static_cast<std::uint64_t>(volume_characters) *
                          sizeof(wchar_t)) ||
            calculated_payload != payload_size ||
            compact_fixed_header_size + payload_size + checksum_size !=
                mapped->size) {
            result.error = corrupt_data_error;
            return result;
        }

        std::uint64_t checksum = fnv_offset;
        hash_bytes(checksum, bytes, mapped->size - checksum_size);
        if (get_u64(bytes + mapped->size - checksum_size) != checksum) {
            result.error = corrupt_data_error;
            return result;
        }

        const auto* nodes = reinterpret_cast<const CatalogBaseNode*>(
            bytes + compact_fixed_header_size);
        const auto* names = reinterpret_cast<const wchar_t*>(
            bytes + compact_fixed_header_size +
            record_count * compact_fixed_record_size);
        const auto* volume_text = names + name_characters;
        std::uint64_t previous_id = 0;
        for (std::uint64_t i = 0; i < record_count; ++i) {
            const auto& node = nodes[i];
            if (node.id == 0 || (i != 0 && node.id <= previous_id) ||
                node.directory > 1 || node.reserved[0] != 0 ||
                node.reserved[1] != 0 || node.reserved[2] != 0 ||
                node.name_length > max_string_characters ||
                node.name_offset > name_characters ||
                node.name_length > name_characters - node.name_offset) {
                result.error = corrupt_data_error;
                return result;
            }
            previous_id = node.id;
        }

        MappedMetadataSnapshot snapshot;
        snapshot.checkpoint = {journal_id, next_usn};
        snapshot.root_id = root_id;
        snapshot.volume.assign(volume_text, volume_characters);
        snapshot.catalog.owner = std::static_pointer_cast<const void>(mapped);
        snapshot.catalog.nodes = nodes;
        snapshot.catalog.node_count = static_cast<std::size_t>(record_count);
        snapshot.catalog.names = names;
        snapshot.catalog.name_count =
            static_cast<std::size_t>(name_characters);
        snapshot.catalog.mapped_bytes = mapped->size;
        snapshot.format_version = compact_format_version;
        result.snapshot = std::move(snapshot);
        result.ok = true;
        result.error = ERROR_SUCCESS;
        return result;
    } catch (const std::bad_alloc&) {
        result.error = ERROR_NOT_ENOUGH_MEMORY;
        return result;
    } catch (...) {
        result.error = ERROR_UNHANDLED_EXCEPTION;
        return result;
    }
}

MetadataSnapshotLoadResult load_metadata_snapshot(
    const std::filesystem::path& path) {
    MetadataSnapshotLoadResult result;
    auto mapped = load_metadata_snapshot_mapped(path);
    if (mapped.ok) {
        try {
            MetadataSnapshot snapshot;
            snapshot.checkpoint = mapped.snapshot.checkpoint;
            snapshot.root_id = mapped.snapshot.root_id;
            snapshot.volume = mapped.snapshot.volume;
            snapshot.records.reserve(mapped.snapshot.catalog.node_count);
            for (std::size_t i = 0;
                 i < mapped.snapshot.catalog.node_count; ++i) {
                const auto& node = mapped.snapshot.catalog.nodes[i];
                FileRecord record;
                record.id = node.id;
                record.parent_id = node.parent_id;
                record.size = node.size;
                record.last_write_time = node.last_write_time;
                record.attributes = node.attributes;
                record.directory = node.directory != 0;
                if (node.name_length != 0) {
                    record.name.assign(
                        mapped.snapshot.catalog.names + node.name_offset,
                        node.name_length);
                }
                snapshot.records.push_back(std::move(record));
            }
            result.snapshot = std::move(snapshot);
            result.ok = true;
            result.error = ERROR_SUCCESS;
            return result;
        } catch (const std::bad_alloc&) {
            result.error = ERROR_NOT_ENOUGH_MEMORY;
            return result;
        }
    }
    if (mapped.error != ERROR_REVISION_MISMATCH) {
        result.error = mapped.error;
        return result;
    }
    Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                            nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) {
        result.error = GetLastError();
        return result;
    }

    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(file.value, &file_size)) {
        result.error = GetLastError();
        return result;
    }
    if (file_size.QuadPart < static_cast<LONGLONG>(
            legacy_fixed_header_size + checksum_size) ||
        static_cast<std::uint64_t>(file_size.QuadPart) > max_snapshot_size) {
        result.error = corrupt_data_error;
        return result;
    }

    SnapshotReader reader(file.value);
    std::uint32_t error = ERROR_SUCCESS;
    std::uint64_t read_magic = 0;
    std::uint32_t version = 0;
    std::uint32_t reserved = 0;
    std::uint64_t journal_id = 0;
    std::int64_t next_usn = 0;
    std::uint64_t root_id = 0;
    std::uint64_t record_count = 0;
    std::uint64_t payload_size = 0;
    if (!reader.u64(read_magic, error) ||
        !reader.u32(version, error) ||
        !reader.u32(reserved, error) ||
        !reader.u64(journal_id, error) ||
        !reader.i64(next_usn, error) ||
        !reader.u64(root_id, error) ||
        !reader.u64(record_count, error) ||
        !reader.u64(payload_size, error)) {
        result.error = error;
        return result;
    }
    const std::uint64_t expected_size = legacy_fixed_header_size + payload_size +
                                        checksum_size;
    if (read_magic != magic || version != legacy_format_version || reserved != 0 ||
        root_id == 0 || record_count > max_record_count ||
        payload_size > max_snapshot_size - legacy_fixed_header_size - checksum_size ||
        expected_size != static_cast<std::uint64_t>(file_size.QuadPart)) {
        result.error = corrupt_data_error;
        return result;
    }

    MetadataSnapshot snapshot;
    snapshot.checkpoint = {journal_id, next_usn};
    snapshot.root_id = root_id;
    std::uint64_t remaining = payload_size;
    if (!read_string(reader, snapshot.volume, remaining, error) ||
        snapshot.volume.empty()) {
        result.error = error == ERROR_SUCCESS ? corrupt_data_error : error;
        return result;
    }
    if (record_count > remaining / legacy_fixed_record_size) {
        result.error = corrupt_data_error;
        return result;
    }

    try {
        snapshot.records.reserve(static_cast<std::size_t>(record_count));
        for (std::uint64_t i = 0; i < record_count; ++i) {
            if (remaining < legacy_fixed_record_size) {
                result.error = corrupt_data_error;
                return result;
            }
            FileRecord record;
            std::uint8_t directory = 0;
            std::array<unsigned char, 3> record_reserved{};
            if (!reader.u64(record.id, error) ||
                !reader.u64(record.parent_id, error) ||
                !reader.u64(record.size, error) ||
                !reader.i64(record.last_write_time, error) ||
                !reader.u32(record.attributes, error) ||
                !reader.u8(directory, error) ||
                !reader.bytes(record_reserved.data(), record_reserved.size(),
                              error)) {
                result.error = error;
                return result;
            }
            remaining -= 40;
            if (directory > 1 || record_reserved[0] != 0 ||
                record_reserved[1] != 0 || record_reserved[2] != 0) {
                result.error = corrupt_data_error;
                return result;
            }
            record.directory = directory != 0;
            if (!read_string(reader, record.name, remaining, error) ||
                !read_string(reader, record.path, remaining, error)) {
                result.error = error;
                return result;
            }
            snapshot.records.push_back(std::move(record));
        }
    } catch (const std::bad_alloc&) {
        result.error = ERROR_NOT_ENOUGH_MEMORY;
        return result;
    }
    if (remaining != 0) {
        result.error = corrupt_data_error;
        return result;
    }

    std::array<unsigned char, 8> checksum{};
    if (!reader.raw_bytes(checksum.data(), checksum.size(), error)) {
        result.error = error;
        return result;
    }
    if (get_u64(checksum.data()) != reader.hash()) {
        result.error = corrupt_data_error;
        return result;
    }

    result.snapshot = std::move(snapshot);
    result.ok = true;
    result.error = ERROR_SUCCESS;
    return result;
}
} // namespace esm




