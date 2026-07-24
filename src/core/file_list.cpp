#include "esm/file_list.hpp"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>

namespace esm {
namespace {
std::wstring from_utf8(std::string_view value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                          value.data(), static_cast<int>(value.size()),
                                          nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), count);
    return result;
}
std::string to_utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0,
                                          nullptr, nullptr);
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), count, nullptr, nullptr);
    return result;
}
std::vector<std::string> csv_row(std::string_view line) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (quoted) {
            if (ch == '"' && i + 1 < line.size() && line[i + 1] == '"') {
                field.push_back('"');
                ++i;
            } else if (ch == '"') {
                quoted = false;
            } else {
                field.push_back(ch);
            }
        } else if (ch == '"') {
            quoted = true;
        } else if (ch == ',') {
            fields.push_back(std::move(field));
            field.clear();
        } else if (ch != '\r') {
            field.push_back(ch);
        }
    }
    fields.push_back(std::move(field));
    return fields;
}
std::string csv_field(std::string value) {
    if (value.find_first_of(",\"\r\n") == std::string::npos) return value;
    std::string result{"\""};
    for (const char ch : value) {
        if (ch == '"') result += "\"\"";
        else result.push_back(ch);
    }
    result.push_back('"');
    return result;
}
template <typename T>
bool number(std::string_view value, T& output) {
    if (value.empty()) { output = 0; return true; }
    const auto result = std::from_chars(value.data(), value.data() + value.size(), output);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}
std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
    });
    return value;
}
}

bool load_efu_file(const std::filesystem::path& path,
                   std::vector<FileRecord>& records,
                   std::wstring& error) {
    records.clear();
    error.clear();
    std::ifstream input(path, std::ios::binary);
    if (!input) { error = L"无法打开文件列表。"; return false; }
    std::string line;
    if (!std::getline(input, line)) { error = L"文件列表为空。"; return false; }
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xef &&
        static_cast<unsigned char>(line[1]) == 0xbb &&
        static_cast<unsigned char>(line[2]) == 0xbf) line.erase(0, 3);
    const auto headers = csv_row(line);
    int filename = -1, size = -1, modified = -1, created = -1,
        accessed = -1, changed = -1, attributes = -1;
    for (int i = 0; i < static_cast<int>(headers.size()); ++i) {
        const auto header = lower_ascii(headers[static_cast<std::size_t>(i)]);
        if (header == "filename") filename = i;
        else if (header == "size") size = i;
        else if (header == "date modified") modified = i;
        else if (header == "date created") created = i;
        else if (header == "date accessed") accessed = i;
        else if (header == "date changed") changed = i;
        else if (header == "attributes") attributes = i;
    }
    if (filename < 0) { error = L"不是有效的 EFU 文件：缺少 Filename 列。"; return false; }
    std::uint64_t id = 1;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto fields = csv_row(line);
        if (filename >= static_cast<int>(fields.size())) continue;
        FileRecord record;
        record.id = id++;
        record.path = from_utf8(fields[static_cast<std::size_t>(filename)]);
        if (record.path.empty()) continue;
        const std::filesystem::path p(record.path);
        record.name = p.filename().wstring();
        if (record.name.empty()) record.name = record.path;
        if (size >= 0 && size < static_cast<int>(fields.size()))
            number(fields[static_cast<std::size_t>(size)], record.size);
        if (modified >= 0 && modified < static_cast<int>(fields.size()))
            number(fields[static_cast<std::size_t>(modified)], record.last_write_time);
        if (created >= 0 && created < static_cast<int>(fields.size()))
            number(fields[static_cast<std::size_t>(created)], record.creation_time);
        if (accessed >= 0 && accessed < static_cast<int>(fields.size()))
            number(fields[static_cast<std::size_t>(accessed)], record.last_access_time);
        if (changed >= 0 && changed < static_cast<int>(fields.size()))
            number(fields[static_cast<std::size_t>(changed)], record.change_time);
        if (attributes >= 0 && attributes < static_cast<int>(fields.size()))
            number(fields[static_cast<std::size_t>(attributes)], record.attributes);
        record.directory = (record.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        records.push_back(std::move(record));
    }
    return true;
}

bool save_efu_file(const std::filesystem::path& path,
                   std::span<const FileRecord> records,
                   std::wstring& error) {
    error.clear();
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) { error = L"无法写入文件列表。"; return false; }
    output << "\xEF\xBB\xBF"
           << "Filename,Size,Date Modified,Date Created,Date Accessed,Date Changed,Attributes\r\n";
    for (const auto& record : records) {
        output << csv_field(to_utf8(record.path)) << ',' << record.size << ','
               << record.last_write_time << ',' << record.creation_time << ','
               << record.last_access_time << ',' << record.change_time << ','
               << record.attributes << "\r\n";
    }
    if (!output) { error = L"写入文件列表时发生错误。"; return false; }
    return true;
}
} // namespace esm
