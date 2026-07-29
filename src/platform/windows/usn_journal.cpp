#include "esm/usn_journal.hpp"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <vector>

namespace esm {
namespace {
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

// MinGW's winioctl.h currently exposes only the legacy
// READ_USN_JOURNAL_DATA typedef. Windows 8 and newer also accept this V1
// shape, which lets the caller explicitly request supported record versions.
struct ReadUsnJournalDataV1 {
    USN start_usn;
    DWORD reason_mask;
    DWORD return_only_on_close;
    DWORDLONG timeout;
    DWORDLONG bytes_to_wait_for;
    DWORDLONG journal_id;
    WORD min_major_version;
    WORD max_major_version;
};
static_assert(sizeof(READ_USN_JOURNAL_DATA) == 40 ||
              sizeof(READ_USN_JOURNAL_DATA) == sizeof(ReadUsnJournalDataV1));
static_assert(offsetof(ReadUsnJournalDataV1, journal_id) == 32);
static_assert(offsetof(ReadUsnJournalDataV1, min_major_version) == 40);
static_assert(sizeof(ReadUsnJournalDataV1) == 48);

constexpr DWORD tracked_reason_mask =
    USN_REASON_DATA_OVERWRITE |
    USN_REASON_DATA_EXTEND |
    USN_REASON_DATA_TRUNCATION |
    USN_REASON_NAMED_DATA_OVERWRITE |
    USN_REASON_NAMED_DATA_EXTEND |
    USN_REASON_NAMED_DATA_TRUNCATION |
    USN_REASON_FILE_CREATE |
    USN_REASON_FILE_DELETE |
    USN_REASON_EA_CHANGE |
    USN_REASON_SECURITY_CHANGE |
    USN_REASON_RENAME_OLD_NAME |
    USN_REASON_RENAME_NEW_NAME |
    USN_REASON_INDEXABLE_CHANGE |
    USN_REASON_BASIC_INFO_CHANGE |
    USN_REASON_HARD_LINK_CHANGE |
    USN_REASON_COMPRESSION_CHANGE |
    USN_REASON_ENCRYPTION_CHANGE |
    USN_REASON_OBJECT_ID_CHANGE |
    USN_REASON_REPARSE_POINT_CHANGE |
    USN_REASON_STREAM_CHANGE |
    USN_REASON_TRANSACTED_CHANGE |
    USN_REASON_CLOSE;

std::wstring device_name(std::wstring_view input) {
    if (input.size() < 2 || input[1] != L':') return {};
    wchar_t drive = input[0];
    if (drive >= L'a' && drive <= L'z') drive -= L'a' - L'A';
    return L"\\\\.\\" + std::wstring(1, drive) + L":";
}

Handle open_volume(std::wstring_view volume) {
    const auto device = device_name(volume);
    if (device.empty()) return {};
    return {CreateFileW(device.c_str(), GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, 0, nullptr)};
}

bool issue_read(HANDLE volume, const void* request, DWORD request_size,
                std::vector<unsigned char>& buffer, DWORD& returned,
                DWORD& error) {
    returned = 0;
    if (DeviceIoControl(volume, FSCTL_READ_USN_JOURNAL,
                        const_cast<void*>(request), request_size,
                        buffer.data(), static_cast<DWORD>(buffer.size()),
                        &returned, nullptr)) {
        error = ERROR_SUCCESS;
        return true;
    }
    error = GetLastError();
    return false;
}
} // namespace

UsnJournalState query_usn_journal(std::wstring_view volume) {
    UsnJournalState result;
    auto handle = open_volume(volume);
    if (handle.value == INVALID_HANDLE_VALUE) {
        result.error = GetLastError();
        return result;
    }

    USN_JOURNAL_DATA data{};
    DWORD returned = 0;
    if (!DeviceIoControl(handle.value, FSCTL_QUERY_USN_JOURNAL,
                         nullptr, 0, &data, sizeof(data), &returned, nullptr)) {
        result.error = GetLastError();
        return result;
    }

    result.available = true;
    result.journal_id = data.UsnJournalID;
    result.first_usn = data.FirstUsn;
    result.next_usn = data.NextUsn;
    result.lowest_valid_usn = data.LowestValidUsn;
    result.max_usn = data.MaxUsn;
    return result;
}

UsnChangeBatch read_usn_changes(std::wstring_view volume,
                                const UsnJournalState& state,
                                std::int64_t start_usn,
                                std::size_t buffer_size) {
    UsnChangeBatch result;
    const auto effective_start = std::max({start_usn, state.first_usn,
                                           state.lowest_valid_usn});
    result.next_usn = effective_start;

    auto handle = open_volume(volume);
    if (handle.value == INVALID_HANDLE_VALUE) {
        result.error = GetLastError();
        return result;
    }

    const auto bounded_size = std::clamp<std::size_t>(
        buffer_size, 64 * 1024,
        static_cast<std::size_t>(std::numeric_limits<DWORD>::max()));
    std::vector<unsigned char> buffer(bounded_size);
    DWORD returned = 0;

    ReadUsnJournalDataV1 request_v1{};
    request_v1.start_usn = static_cast<USN>(effective_start);
    request_v1.reason_mask = tracked_reason_mask;
    request_v1.return_only_on_close = FALSE;
    request_v1.timeout = 0;
    request_v1.bytes_to_wait_for = 0;
    request_v1.journal_id = state.journal_id;
    request_v1.min_major_version = 2;
    request_v1.max_major_version = 2;

    DWORD v1_error = ERROR_SUCCESS;
    bool read_ok = issue_read(handle.value, &request_v1,
                              static_cast<DWORD>(sizeof(request_v1)),
                              buffer, returned, v1_error);
    result.v1_error = v1_error;
    if (read_ok) {
        result.request_version = 1;
    } else {
        READ_USN_JOURNAL_DATA request_v0{};
        request_v0.StartUsn = static_cast<USN>(effective_start);
        request_v0.ReasonMask = tracked_reason_mask;
        request_v0.ReturnOnlyOnClose = FALSE;
        request_v0.Timeout = 0;
        request_v0.BytesToWaitFor = 0;
        request_v0.UsnJournalID = state.journal_id;

        DWORD v0_error = ERROR_SUCCESS;
        read_ok = issue_read(handle.value, &request_v0,
                             static_cast<DWORD>(sizeof(request_v0)),
                             buffer, returned, v0_error);
        result.v0_error = v0_error;
        if (read_ok) {
            result.request_version = 0;
        } else {
            result.error = v0_error;
            return result;
        }
    }

    if (returned < sizeof(USN)) return result;
    result.next_usn = *reinterpret_cast<const USN*>(buffer.data());

    DWORD offset = sizeof(USN);
    while (offset + sizeof(DWORD) + sizeof(WORD) * 2 <= returned) {
        const auto* bytes = buffer.data() + offset;
        const auto record_length = *reinterpret_cast<const DWORD*>(bytes);
        const auto major_version = *reinterpret_cast<const WORD*>(bytes + sizeof(DWORD));
        if (record_length == 0 || offset + record_length > returned) {
            result.error = ERROR_INVALID_DATA;
            break;
        }

        if (major_version == 2 && record_length >= sizeof(USN_RECORD)) {
            const auto* record = reinterpret_cast<const USN_RECORD*>(bytes);
            const auto name_end = static_cast<DWORD>(record->FileNameOffset) +
                                  static_cast<DWORD>(record->FileNameLength);
            if (name_end > record_length) {
                result.error = ERROR_INVALID_DATA;
                break;
            }

            UsnChange change;
            change.file_id = record->FileReferenceNumber;
            change.parent_id = record->ParentFileReferenceNumber;
            change.usn = record->Usn;
            change.reason = record->Reason;
            change.attributes = record->FileAttributes;
            const auto* name = reinterpret_cast<const wchar_t*>(
                reinterpret_cast<const unsigned char*>(record) +
                record->FileNameOffset);
            change.name.assign(name,
                               record->FileNameLength / sizeof(wchar_t));
            result.changes.push_back(std::move(change));
        }
        offset += record_length;
    }
    return result;
}
} // namespace esm

