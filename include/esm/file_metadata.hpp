#pragma once

#include "esm/file_record.hpp"

namespace esm {
// Refreshes size, creation/access/write/change times, attributes, and
// directory state from the filesystem. Existing indexed values are left untouched when the path is no
// longer accessible.
[[nodiscard]] bool hydrate_file_metadata(FileRecord& record) noexcept;
} // namespace esm
