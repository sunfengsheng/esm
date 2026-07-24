#pragma once
#include <cstdint>
#include <string>
namespace esm {
struct FileRecord {
    std::uint64_t id{};
    std::uint64_t parent_id{};
    std::uint64_t size{};
    std::int64_t last_write_time{};
    std::int64_t creation_time{};
    std::int64_t last_access_time{};
    std::int64_t change_time{};
    std::uint32_t attributes{};
    bool directory{};
    std::wstring name;
    std::wstring path;
};
}
