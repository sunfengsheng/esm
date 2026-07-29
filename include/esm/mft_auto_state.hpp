#pragma once

#include "esm/volume_discovery.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace esm {
struct MftAutoVolumeState {
    NtfsVolumeInfo volume;
    std::uint64_t root_id{};
    std::uint64_t journal_id{};
    std::int64_t cursor{};
    bool live{};
};

struct MftAutoState {
    std::uint64_t generation{};
    std::vector<MftAutoVolumeState> volumes;
};

struct MftAutoStateIoResult {
    bool ok{};
    bool generation_mismatch{};
    std::uint32_t error{};
    MftAutoState state;
};

[[nodiscard]] std::filesystem::path mft_auto_state_path(
    const std::filesystem::path& snapshot_path);
[[nodiscard]] MftAutoStateIoResult save_mft_auto_state_atomic(
    const std::filesystem::path& path, const MftAutoState& state);
[[nodiscard]] MftAutoStateIoResult load_mft_auto_state(
    const std::filesystem::path& path, std::uint64_t expected_generation);
} // namespace esm