#include "esm/content_roots.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <unordered_set>

namespace esm {
namespace {
std::filesystem::path normalized_path(const std::filesystem::path& path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error) absolute = path;
    return absolute.lexically_normal();
}

std::wstring invariant_lower(std::wstring_view value) {
    if (value.empty()) return {};
    const int required = LCMapStringEx(
        LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr, 0);
    if (required <= 0) return std::wstring(value);
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(),
                      static_cast<int>(value.size()), result.data(), required,
                      nullptr, nullptr, 0) != required) {
        return std::wstring(value);
    }
    return result;
}

bool lower_path_is_within(std::wstring_view path, std::wstring_view root) {
    if (path == root) return true;
    std::wstring prefix(root);
    if (!prefix.empty() && prefix.back() != L'\\' && prefix.back() != L'/')
        prefix.push_back(L'\\');
    return path.size() > prefix.size() &&
           path.compare(0, prefix.size(), prefix) == 0;
}

std::uint64_t fnv1a(std::wstring_view value) {
    constexpr std::uint64_t offset = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offset;
    for (const wchar_t character : value) {
        const auto code = static_cast<std::uint16_t>(character);
        hash ^= static_cast<std::uint8_t>(code & 0xffU);
        hash *= prime;
        hash ^= static_cast<std::uint8_t>((code >> 8U) & 0xffU);
        hash *= prime;
    }
    return hash;
}

std::wstring hex64(std::uint64_t value) {
    std::wostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill(L'0') << value;
    return stream.str();
}
} // namespace

ContentPathFilter::ContentPathFilter(
    std::filesystem::path root, bool use_default_excludes,
    std::vector<std::filesystem::path> excluded_paths)
    : root_(normalized_path(root)),
      lower_root_(invariant_lower(root_.wstring())),
      use_default_excludes_(use_default_excludes) {
    lower_excluded_paths_.reserve(excluded_paths.size());
    for (auto& excluded : excluded_paths) {
        if (excluded.empty()) continue;
        if (!excluded.is_absolute()) excluded = root_ / excluded;
        lower_excluded_paths_.push_back(
            invariant_lower(normalized_path(excluded).wstring()));
    }
    std::sort(lower_excluded_paths_.begin(), lower_excluded_paths_.end());
    lower_excluded_paths_.erase(
        std::unique(lower_excluded_paths_.begin(),
                    lower_excluded_paths_.end()),
        lower_excluded_paths_.end());
}

const std::filesystem::path& ContentPathFilter::root() const noexcept {
    return root_;
}

bool ContentPathFilter::excluded(const std::filesystem::path& path) const {
    const auto normalized = normalized_path(path);
    const auto lower = invariant_lower(normalized.wstring());
    if (!lower_path_is_within(lower, lower_root_)) return true;
    for (const auto& excluded_path : lower_excluded_paths_) {
        if (lower_path_is_within(lower, excluded_path)) return true;
    }
    if (!use_default_excludes_ || lower == lower_root_) return false;

    static const std::unordered_set<std::wstring> top_level{
        L"windows", L"program files", L"program files (x86)",
        L"programdata", L"$recycle.bin", L"system volume information",
        L"recovery"};
    static const std::unordered_set<std::wstring> anywhere{
        L".git", L".svn", L"node_modules", L".cache", L"__pycache__"};

    const auto relative = normalized.lexically_relative(root_);
    if (relative.empty() || *relative.begin() == L"..") return true;
    bool first = true;
    for (const auto& component : relative) {
        const auto name = invariant_lower(component.wstring());
        if ((first && top_level.contains(name)) || anywhere.contains(name))
            return true;
        first = false;
    }
    return false;
}

std::vector<std::filesystem::path> discover_fixed_content_roots() {
    const DWORD required = GetLogicalDriveStringsW(0, nullptr);
    if (required == 0) return {};
    std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1, L'\0');
    if (GetLogicalDriveStringsW(static_cast<DWORD>(buffer.size()),
                                buffer.data()) == 0) {
        return {};
    }

    std::vector<std::filesystem::path> roots;
    for (const wchar_t* cursor = buffer.data(); *cursor != L'\0';
         cursor += std::wcslen(cursor) + 1) {
        if (GetDriveTypeW(cursor) != DRIVE_FIXED) continue;
        const std::filesystem::path root(cursor);
        std::error_code error;
        if (std::filesystem::is_directory(root, error) && !error)
            roots.push_back(root.lexically_normal());
    }
    std::sort(roots.begin(), roots.end(), [](const auto& left, const auto& right) {
        return invariant_lower(left.wstring()) < invariant_lower(right.wstring());
    });
    return roots;
}

std::wstring content_root_database_key(const std::filesystem::path& root) {
    const auto normalized = normalized_path(root);
    const auto root_name = normalized.root_path();
    if (normalized == root_name && !root_name.empty()) {
        DWORD serial{};
        const auto root_text = root_name.wstring();
        if (GetVolumeInformationW(root_text.c_str(), nullptr, 0, &serial,
                                  nullptr, nullptr, nullptr, 0)) {
            wchar_t drive = L'v';
            const auto drive_name = root_name.root_name().wstring();
            if (!drive_name.empty() && std::iswalpha(drive_name.front()))
                drive = static_cast<wchar_t>(std::towlower(drive_name.front()));
            std::wostringstream stream;
            stream << drive << L"-" << std::hex << std::setw(8)
                   << std::setfill(L'0') << serial;
            return stream.str();
        }
    }
    return L"path-" + hex64(fnv1a(invariant_lower(normalized.wstring())));
}
} // namespace esm
