#include "esm/content_index.hpp"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace esm {
namespace {
constexpr std::uint32_t local_file_signature = 0x04034b50;
constexpr std::uint32_t central_file_signature = 0x02014b50;
constexpr std::uint32_t end_signature = 0x06054b50;

std::uint16_t read_u16(const std::vector<std::uint8_t>& data,
                       std::size_t offset) {
    return static_cast<std::uint16_t>(data[offset]) |
           (static_cast<std::uint16_t>(data[offset + 1]) << 8);
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& data,
                       std::size_t offset) {
    return static_cast<std::uint32_t>(data[offset]) |
           (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

bool range_valid(std::size_t offset, std::size_t length, std::size_t size) {
    return offset <= size && length <= size - offset;
}

bool load_zip_entry(const std::vector<std::uint8_t>& archive,
                    std::string_view requested_name,
                    std::size_t output_limit,
                    std::string& output,
                    std::wstring& error) {
    if (archive.size() < 22) {
        error = L"DOCX ZIP 结构过短";
        return false;
    }
    const auto search_start = archive.size() > 65'557
                                  ? archive.size() - 65'557
                                  : 0;
    std::size_t end_offset = std::string::npos;
    for (std::size_t cursor = archive.size() - 22;; --cursor) {
        if (read_u32(archive, cursor) == end_signature) {
            end_offset = cursor;
            break;
        }
        if (cursor == search_start) break;
    }
    if (end_offset == std::string::npos ||
        !range_valid(end_offset, 22, archive.size())) {
        error = L"DOCX ZIP 缺少中央目录";
        return false;
    }
    const auto entries = read_u16(archive, end_offset + 10);
    const auto central_size = read_u32(archive, end_offset + 12);
    const auto central_offset = read_u32(archive, end_offset + 16);
    if (!range_valid(central_offset, central_size, archive.size())) {
        error = L"DOCX ZIP 中央目录越界";
        return false;
    }

    std::size_t cursor = central_offset;
    for (std::uint16_t entry = 0; entry < entries; ++entry) {
        if (!range_valid(cursor, 46, archive.size()) ||
            read_u32(archive, cursor) != central_file_signature) {
            error = L"DOCX ZIP 中央目录损坏";
            return false;
        }
        const auto flags = read_u16(archive, cursor + 8);
        const auto method = read_u16(archive, cursor + 10);
        const auto compressed_size = read_u32(archive, cursor + 20);
        const auto uncompressed_size = read_u32(archive, cursor + 24);
        const auto name_length = read_u16(archive, cursor + 28);
        const auto extra_length = read_u16(archive, cursor + 30);
        const auto comment_length = read_u16(archive, cursor + 32);
        const auto local_offset = read_u32(archive, cursor + 42);
        const std::size_t record_length = 46ULL + name_length + extra_length +
                                          comment_length;
        if (!range_valid(cursor, record_length, archive.size())) {
            error = L"DOCX ZIP 文件项越界";
            return false;
        }
        const std::string_view name(
            reinterpret_cast<const char*>(archive.data() + cursor + 46),
            name_length);
        if (name == requested_name) {
            if ((flags & 0x1) != 0) {
                error = L"不支持加密 DOCX";
                return false;
            }
            if (uncompressed_size > output_limit) {
                error = L"DOCX 正文超过内容索引大小上限";
                return false;
            }
            if (!range_valid(local_offset, 30, archive.size()) ||
                read_u32(archive, local_offset) != local_file_signature) {
                error = L"DOCX ZIP 本地文件头损坏";
                return false;
            }
            const auto local_name_length = read_u16(archive, local_offset + 26);
            const auto local_extra_length = read_u16(archive, local_offset + 28);
            const std::size_t data_offset = 30ULL + local_offset +
                                            local_name_length +
                                            local_extra_length;
            if (!range_valid(data_offset, compressed_size, archive.size())) {
                error = L"DOCX ZIP 压缩数据越界";
                return false;
            }
            if (method == 0) {
                output.assign(
                    reinterpret_cast<const char*>(archive.data() + data_offset),
                    compressed_size);
                return true;
            }
            if (method != 8) {
                error = L"DOCX 使用了不支持的 ZIP 压缩算法";
                return false;
            }
            output.assign(uncompressed_size, '\0');
            z_stream stream{};
            stream.next_in = const_cast<Bytef*>(
                reinterpret_cast<const Bytef*>(archive.data() + data_offset));
            stream.avail_in = compressed_size;
            stream.next_out = reinterpret_cast<Bytef*>(output.data());
            stream.avail_out = uncompressed_size;
            if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
                error = L"初始化 DOCX 解压器失败";
                return false;
            }
            const int inflate_result = inflate(&stream, Z_FINISH);
            inflateEnd(&stream);
            if (inflate_result != Z_STREAM_END) {
                error = L"解压 DOCX 正文失败";
                return false;
            }
            output.resize(stream.total_out);
            return true;
        }
        cursor += record_length;
    }
    error = L"DOCX 中缺少 word/document.xml";
    return false;
}

void append_utf8_codepoint(std::string& output, std::uint32_t codepoint) {
    if (codepoint <= 0x7f) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0x10ffff) {
        output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
}

void append_xml_text(std::string_view text, std::string& output) {
    for (std::size_t index = 0; index < text.size();) {
        if (text[index] != '&') {
            output.push_back(text[index++]);
            continue;
        }
        const auto semicolon = text.find(';', index + 1);
        if (semicolon == std::string_view::npos) {
            output.push_back(text[index++]);
            continue;
        }
        const auto entity = text.substr(index + 1, semicolon - index - 1);
        if (entity == "amp") output.push_back('&');
        else if (entity == "lt") output.push_back('<');
        else if (entity == "gt") output.push_back('>');
        else if (entity == "quot") output.push_back('"');
        else if (entity == "apos") output.push_back('\'');
        else if (!entity.empty() && entity.front() == '#') {
            try {
                const bool hex = entity.size() > 1 &&
                                 (entity[1] == 'x' || entity[1] == 'X');
                const auto digits = entity.substr(hex ? 2 : 1);
                const auto value = std::stoul(std::string(digits), nullptr,
                                              hex ? 16 : 10);
                append_utf8_codepoint(output, value);
            } catch (...) {
                output.append(text.substr(index, semicolon - index + 1));
            }
        } else {
            output.append(text.substr(index, semicolon - index + 1));
        }
        index = semicolon + 1;
    }
}

std::string local_tag_name(std::string_view tag) {
    while (!tag.empty() && std::isspace(static_cast<unsigned char>(tag.front())))
        tag.remove_prefix(1);
    if (!tag.empty() && tag.front() == '/') tag.remove_prefix(1);
    const auto end = tag.find_first_of(" \t\r\n/>");
    tag = tag.substr(0, end);
    const auto colon = tag.rfind(':');
    if (colon != std::string_view::npos) tag.remove_prefix(colon + 1);
    return std::string(tag);
}

bool extract_word_xml(std::string_view xml, std::size_t output_limit,
                      std::string& output, std::wstring& error) {
    output.clear();
    bool in_text{};
    std::size_t cursor{};
    while (cursor < xml.size()) {
        const auto open = xml.find('<', cursor);
        if (open == std::string_view::npos) {
            if (in_text) append_xml_text(xml.substr(cursor), output);
            break;
        }
        if (in_text && open > cursor)
            append_xml_text(xml.substr(cursor, open - cursor), output);
        const auto close = xml.find('>', open + 1);
        if (close == std::string_view::npos) {
            error = L"DOCX document.xml 标签不完整";
            return false;
        }
        const auto raw_tag = xml.substr(open + 1, close - open - 1);
        const bool closing = !raw_tag.empty() && raw_tag.front() == '/';
        const bool self_closing = !raw_tag.empty() && raw_tag.back() == '/';
        const auto tag = local_tag_name(raw_tag);
        if (tag == "t") {
            in_text = !closing && !self_closing;
            if (closing && !output.empty() && output.back() != ' ' &&
                output.back() != '\n')
                output.push_back(' ');
        } else if (!closing && (tag == "tab")) {
            output.push_back('\t');
        } else if (!closing && (tag == "br" || tag == "cr")) {
            output.push_back('\n');
        } else if (closing && tag == "p") {
            while (!output.empty() && output.back() == ' ') output.pop_back();
            if (!output.empty() && output.back() != '\n') output.push_back('\n');
        }
        if (output.size() > output_limit) {
            error = L"DOCX 提取文本超过内容索引大小上限";
            return false;
        }
        cursor = close + 1;
    }
    while (!output.empty() &&
           (output.back() == ' ' || output.back() == '\n' ||
            output.back() == '\r' || output.back() == '\t'))
        output.pop_back();
    if (output.empty()) {
        error = L"DOCX 正文为空";
        return false;
    }
    return true;
}
} // namespace

bool extract_docx_file(const std::filesystem::path& path,
                       std::size_t maximum_bytes,
                       ContentDocument& document,
                       std::wstring& error) {
    document = {};
    error.clear();
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(path, filesystem_error) ||
        filesystem_error) {
        error = L"不是普通文件";
        return false;
    }
    const auto size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error || size > maximum_bytes ||
        size > static_cast<std::uintmax_t>((std::numeric_limits<std::size_t>::max)())) {
        error = L"DOCX 文件超过内容索引大小上限或无法读取";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = L"无法打开 DOCX";
        return false;
    }
    std::vector<std::uint8_t> archive(static_cast<std::size_t>(size));
    if (!archive.empty() &&
        !input.read(reinterpret_cast<char*>(archive.data()),
                    static_cast<std::streamsize>(archive.size()))) {
        error = L"读取 DOCX 失败";
        return false;
    }
    std::string document_xml;
    if (!load_zip_entry(archive, "word/document.xml", maximum_bytes,
                        document_xml, error))
        return false;
    std::string text;
    if (!extract_word_xml(document_xml, maximum_bytes, text, error))
        return false;
    const auto write_time = std::filesystem::last_write_time(path, filesystem_error);
    if (filesystem_error) {
        error = L"无法读取 DOCX 修改时间";
        return false;
    }
    document.path = path;
    document.utf8_text = std::move(text);
    document.size = size;
    document.last_write_time = write_time.time_since_epoch().count();
    return true;
}
} // namespace esm
