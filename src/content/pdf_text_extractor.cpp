#include "esm/content_index.hpp"

#include <windows.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esm {
namespace {
bool inflate_pdf_stream(std::string_view compressed, std::size_t limit,
                        std::string& output) {
    output.clear();
    z_stream stream{};
    stream.next_in = reinterpret_cast<Bytef*>(
        const_cast<char*>(compressed.data()));
    stream.avail_in = static_cast<uInt>((std::min<std::size_t>)(
        compressed.size(), (std::numeric_limits<uInt>::max)()));
    if (inflateInit(&stream) != Z_OK) return false;
    std::vector<char> buffer(16 * 1024);
    int result = Z_OK;
    while (result == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream.avail_out = static_cast<uInt>(buffer.size());
        result = inflate(&stream, Z_NO_FLUSH);
        const auto produced = buffer.size() - stream.avail_out;
        if (produced > limit - output.size()) {
            inflateEnd(&stream);
            return false;
        }
        output.append(buffer.data(), produced);
    }
    inflateEnd(&stream);
    return result == Z_STREAM_END;
}

int hexadecimal(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool parse_literal_string(std::string_view input, std::size_t& cursor,
                          std::string& bytes) {
    if (cursor >= input.size() || input[cursor] != '(') return false;
    ++cursor;
    int nesting = 1;
    while (cursor < input.size() && nesting > 0) {
        const char value = input[cursor++];
        if (value == '\\') {
            if (cursor >= input.size()) break;
            char escaped = input[cursor++];
            if (escaped == 'n') bytes.push_back('\n');
            else if (escaped == 'r') bytes.push_back('\r');
            else if (escaped == 't') bytes.push_back('\t');
            else if (escaped == 'b') bytes.push_back('\b');
            else if (escaped == 'f') bytes.push_back('\f');
            else if (escaped == '\r') {
                if (cursor < input.size() && input[cursor] == '\n') ++cursor;
            } else if (escaped == '\n') {
                // A backslash followed by a line ending is continuation.
            } else if (escaped >= '0' && escaped <= '7') {
                int octal = escaped - '0';
                for (int count = 0; count < 2 && cursor < input.size() &&
                                    input[cursor] >= '0' && input[cursor] <= '7';
                     ++count) {
                    octal = octal * 8 + (input[cursor++] - '0');
                }
                bytes.push_back(static_cast<char>(octal & 0xff));
            } else {
                bytes.push_back(escaped);
            }
        } else if (value == '(') {
            ++nesting;
            bytes.push_back(value);
        } else if (value == ')') {
            --nesting;
            if (nesting > 0) bytes.push_back(value);
        } else {
            bytes.push_back(value);
        }
    }
    return nesting == 0;
}

bool parse_hex_string(std::string_view input, std::size_t& cursor,
                      std::string& bytes) {
    if (cursor >= input.size() || input[cursor] != '<' ||
        (cursor + 1 < input.size() && input[cursor + 1] == '<'))
        return false;
    ++cursor;
    int high = -1;
    while (cursor < input.size()) {
        const char value = input[cursor++];
        if (value == '>') {
            if (high >= 0) bytes.push_back(static_cast<char>(high << 4));
            return true;
        }
        if (std::isspace(static_cast<unsigned char>(value))) continue;
        const int digit = hexadecimal(value);
        if (digit < 0) return false;
        if (high < 0)
            high = digit;
        else {
            bytes.push_back(static_cast<char>((high << 4) | digit));
            high = -1;
        }
    }
    return false;
}

bool utf16_to_utf8(std::wstring_view value, std::string& output) {
    if (value.empty()) return true;
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return false;
    const auto original = output.size();
    output.resize(original + static_cast<std::size_t>(required));
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                               static_cast<int>(value.size()),
                               output.data() + original, required, nullptr,
                               nullptr) == required;
}

bool append_pdf_bytes(std::string_view bytes, std::string& output) {
    if (bytes.empty()) return true;
    std::wstring wide;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xfe &&
        static_cast<unsigned char>(bytes[1]) == 0xff) {
        for (std::size_t index = 2; index + 1 < bytes.size(); index += 2) {
            wide.push_back(static_cast<wchar_t>(
                (static_cast<unsigned char>(bytes[index]) << 8) |
                static_cast<unsigned char>(bytes[index + 1])));
        }
    } else {
        const int required = MultiByteToWideChar(
            1252, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        if (required <= 0) return false;
        wide.resize(static_cast<std::size_t>(required));
        if (MultiByteToWideChar(1252, 0, bytes.data(),
                                static_cast<int>(bytes.size()), wide.data(),
                                required) != required)
            return false;
    }
    return utf16_to_utf8(wide, output);
}

void append_separator(std::string& output) {
    if (!output.empty() && output.back() != ' ' && output.back() != '\n')
        output.push_back(' ');
}

bool extract_text_operators(std::string_view stream, std::size_t limit,
                            std::string& output) {
    bool in_text_object{};
    std::size_t cursor{};
    while (cursor < stream.size()) {
        const char value = stream[cursor];
        if (std::isspace(static_cast<unsigned char>(value)) || value == '[' ||
            value == ']') {
            ++cursor;
            continue;
        }
        if (value == '%') {
            const auto end = stream.find_first_of("\r\n", cursor + 1);
            cursor = end == std::string_view::npos ? stream.size() : end;
            continue;
        }
        if (in_text_object && (value == '(' || value == '<')) {
            std::string bytes;
            const bool parsed = value == '(' ? parse_literal_string(stream, cursor, bytes)
                                             : parse_hex_string(stream, cursor, bytes);
            if (parsed && !bytes.empty()) {
                append_separator(output);
                if (!append_pdf_bytes(bytes, output)) return false;
                if (output.size() > limit) return false;
                continue;
            }
        }
        const auto token_end = stream.find_first_of(
            " \t\r\n[]()<>/%", cursor);
        if (token_end == cursor) {
            ++cursor;
            continue;
        }
        const auto end = token_end == std::string_view::npos ? stream.size()
                                                             : token_end;
        const auto token = stream.substr(cursor, end - cursor);
        if (token == "BT") in_text_object = true;
        else if (token == "ET") {
            in_text_object = false;
            if (!output.empty() && output.back() != '\n') output.push_back('\n');
        } else if (in_text_object && (token == "Td" || token == "TD" ||
                                      token == "T*" || token == "'" ||
                                      token == "\"")) {
            append_separator(output);
        }
        cursor = end;
    }
    return true;
}

std::optional<std::size_t> direct_length(std::string_view dictionary) {
    const auto marker = dictionary.find("/Length");
    if (marker == std::string_view::npos) return std::nullopt;
    std::size_t cursor = marker + 7;
    while (cursor < dictionary.size() &&
           std::isspace(static_cast<unsigned char>(dictionary[cursor])))
        ++cursor;
    const auto start = cursor;
    while (cursor < dictionary.size() &&
           std::isdigit(static_cast<unsigned char>(dictionary[cursor])))
        ++cursor;
    if (cursor == start) return std::nullopt;
    try {
        return static_cast<std::size_t>(
            std::stoull(std::string(dictionary.substr(start, cursor - start))));
    } catch (...) {
        return std::nullopt;
    }
}
} // namespace

bool extract_basic_pdf_file(const std::filesystem::path& path,
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
        error = L"PDF 文件超过内容索引大小上限或无法读取";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    std::string data(static_cast<std::size_t>(size), '\0');
    if (!input || (!data.empty() &&
                   !input.read(data.data(), static_cast<std::streamsize>(data.size())))) {
        error = L"读取 PDF 失败";
        return false;
    }
    if (!data.starts_with("%PDF-")) {
        error = L"不是有效的 PDF 文件头";
        return false;
    }
    if (data.find("/Encrypt") != std::string::npos) {
        error = L"内置 PDF 提取器不支持加密 PDF";
        return false;
    }

    std::string text;
    std::size_t cursor{};
    while ((cursor = data.find("stream", cursor)) != std::string::npos) {
        const auto dictionary_start = data.rfind("<<", cursor);
        const auto dictionary_end = data.rfind(">>", cursor);
        if (dictionary_start == std::string::npos ||
            dictionary_end == std::string::npos ||
            dictionary_end < dictionary_start) {
            cursor += 6;
            continue;
        }
        const auto dictionary = std::string_view(data).substr(
            dictionary_start, dictionary_end + 2 - dictionary_start);
        std::size_t stream_start = cursor + 6;
        if (stream_start < data.size() && data[stream_start] == '\r') ++stream_start;
        if (stream_start < data.size() && data[stream_start] == '\n') ++stream_start;
        std::size_t stream_size{};
        if (const auto length = direct_length(dictionary);
            length && *length <= data.size() - stream_start) {
            stream_size = *length;
        } else {
            const auto stream_end = data.find("endstream", stream_start);
            if (stream_end == std::string::npos) break;
            stream_size = stream_end - stream_start;
            while (stream_size > 0 &&
                   (data[stream_start + stream_size - 1] == '\r' ||
                    data[stream_start + stream_size - 1] == '\n'))
                --stream_size;
        }
        const auto raw_stream = std::string_view(data).substr(stream_start,
                                                               stream_size);
        std::string decoded;
        const bool flate = dictionary.find("/FlateDecode") != std::string_view::npos;
        const auto content = flate && inflate_pdf_stream(raw_stream, maximum_bytes, decoded)
                                 ? std::string_view(decoded)
                                 : (!flate ? raw_stream : std::string_view{});
        if (!content.empty() && !extract_text_operators(content, maximum_bytes, text)) {
            error = L"PDF 文本超过内容索引大小上限或编码无效";
            return false;
        }
        cursor = stream_start + stream_size;
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    if (text.empty()) {
        error = L"内置 PDF 提取器没有找到可解码文本；扫描版 PDF 需要 OCR";
        return false;
    }
    const auto write_time = std::filesystem::last_write_time(path, filesystem_error);
    if (filesystem_error) {
        error = L"无法读取 PDF 修改时间";
        return false;
    }
    document.path = path;
    document.utf8_text = std::move(text);
    document.size = size;
    document.last_write_time = write_time.time_since_epoch().count();
    return true;
}
} // namespace esm
