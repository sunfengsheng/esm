#include "esm/saved_search.hpp"

#include <charconv>
#include <windows.h>

namespace esm {
namespace {
std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                      static_cast<int>(value.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string result(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), n, nullptr, nullptr);
    return result;
}
std::wstring wide(std::string_view value) {
    if (value.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                      static_cast<int>(value.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring result(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), n);
    return result;
}
std::string escape(std::string value) {
    std::string result;
    result.reserve(value.size());
    for (unsigned char ch : value) {
        if (ch == '%' || ch == '\t' || ch == '\r' || ch == '\n') {
            constexpr char hex[] = "0123456789ABCDEF";
            result.push_back('%');
            result.push_back(hex[ch >> 4]);
            result.push_back(hex[ch & 15]);
        } else result.push_back(static_cast<char>(ch));
    }
    return result;
}
int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
    if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
    return -1;
}
std::string unescape(std::string_view value, bool& ok) {
    std::string result;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '%') { result.push_back(value[i]); continue; }
        if (i + 2 >= value.size()) { ok = false; return {}; }
        const int hi = hex_value(value[i + 1]), lo = hex_value(value[i + 2]);
        if (hi < 0 || lo < 0) { ok = false; return {}; }
        result.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
    }
    return result;
}
}

std::string serialize_saved_searches(const std::vector<SavedSearch>& values) {
    std::string result = "ESM_SAVED_SEARCHES\t1\n";
    for (const auto& value : values) {
        unsigned flags = (value.case_sensitive ? 1u : 0u) |
                         (value.whole_word ? 2u : 0u) |
                         (value.match_path ? 4u : 0u) |
                         (value.match_diacritics ? 8u : 0u) |
                         (value.regex ? 16u : 0u);
        result += escape(utf8(value.name)); result.push_back('\t');
        result += escape(utf8(value.query)); result.push_back('\t');
        result += std::to_string(flags); result.push_back('\n');
    }
    return result;
}

bool parse_saved_searches(std::string_view text,
                          std::vector<SavedSearch>& values) {
    values.clear();
    const auto first_end = text.find('\n');
    if (first_end == std::string_view::npos ||
        text.substr(0, first_end) != "ESM_SAVED_SEARCHES\t1") return false;
    std::size_t offset = first_end + 1;
    while (offset < text.size()) {
        auto end = text.find('\n', offset);
        if (end == std::string_view::npos) end = text.size();
        auto line = text.substr(offset, end - offset);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        offset = end + 1;
        if (line.empty()) continue;
        const auto tab1 = line.find('\t');
        const auto tab2 = tab1 == std::string_view::npos ? tab1 : line.find('\t', tab1 + 1);
        if (tab1 == std::string_view::npos || tab2 == std::string_view::npos) return false;
        bool ok = true;
        const auto name = unescape(line.substr(0, tab1), ok);
        const auto query = unescape(line.substr(tab1 + 1, tab2 - tab1 - 1), ok);
        if (!ok) return false;
        unsigned flags = 0;
        const auto flags_text = line.substr(tab2 + 1);
        const auto parsed = std::from_chars(flags_text.data(), flags_text.data() + flags_text.size(), flags);
        if (parsed.ec != std::errc{} || parsed.ptr != flags_text.data() + flags_text.size()) return false;
        SavedSearch value;
        value.name = wide(name); value.query = wide(query);
        value.case_sensitive = (flags & 1u) != 0;
        value.whole_word = (flags & 2u) != 0;
        value.match_path = (flags & 4u) != 0;
        value.match_diacritics = (flags & 8u) != 0;
        value.regex = (flags & 16u) != 0;
        values.push_back(std::move(value));
    }
    return true;
}
} // namespace esm
