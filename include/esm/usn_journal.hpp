#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
namespace esm {
struct UsnJournalState { bool available{}; std::uint64_t journal_id{}; std::int64_t first_usn{}; std::int64_t next_usn{}; std::int64_t lowest_valid_usn{}; std::int64_t max_usn{}; std::uint32_t error{}; };
struct UsnChange { std::uint64_t file_id{}; std::uint64_t parent_id{}; std::int64_t usn{}; std::uint32_t reason{}; std::uint32_t attributes{}; std::wstring name; };
struct UsnChangeBatch {
    std::int64_t next_usn{};
    std::uint32_t error{};
    std::uint32_t v1_error{};
    std::uint32_t v0_error{};
    std::uint8_t request_version{};
    std::vector<UsnChange> changes;
};
[[nodiscard]] UsnJournalState query_usn_journal(std::wstring_view volume);
[[nodiscard]] UsnChangeBatch read_usn_changes(std::wstring_view volume, const UsnJournalState& state, std::int64_t start_usn, std::size_t buffer_size = 1024 * 1024);
}
