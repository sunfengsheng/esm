#include "esm/content_index.hpp"

#include <windows.h>
#include <filter.h>
#include <filterr.h>
#include <ntquery.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <string>
#include <system_error>

namespace esm {
namespace {
class ComScope {
public:
    ComScope() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() {
        if (result_ == S_OK || result_ == S_FALSE) CoUninitialize();
    }
    [[nodiscard]] bool usable() const noexcept {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }
    [[nodiscard]] HRESULT result() const noexcept { return result_; }
private:
    HRESULT result_{};
};

class FilterScope {
public:
    ~FilterScope() {
        if (filter_) filter_->Release();
    }
    IFilter** put() noexcept { return &filter_; }
    IFilter* get() const noexcept { return filter_; }
private:
    IFilter* filter_{};
};

std::wstring hresult_message(HRESULT result) {
    wchar_t* buffer{};
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(result), 0,
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring message;
    if (length && buffer) {
        message.assign(buffer, buffer + length);
        while (!message.empty() &&
               (message.back() == L'\r' || message.back() == L'\n' ||
                message.back() == L' ')) {
            message.pop_back();
        }
    }
    if (buffer) LocalFree(buffer);
    if (message.empty()) {
        wchar_t code[16]{};
        swprintf_s(code, L"0x%08lX", static_cast<unsigned long>(result));
        message = code;
    }
    return message;
}

bool utf16_to_utf8(std::wstring_view input, std::string& output) {
    output.clear();
    if (input.empty()) return true;
    if (input.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        return false;
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) return false;
    output.resize(static_cast<std::size_t>(required));
    return WideCharToMultiByte(
               CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
               static_cast<int>(input.size()), output.data(), required, nullptr,
               nullptr) == required;
}

bool populate_file_metadata(const std::filesystem::path& path,
                            ContentDocument& document,
                            std::wstring& error) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        error = L"路径不是普通文件";
        return false;
    }
    const auto file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        error = L"无法读取文件大小：" + std::to_wstring(ec.value());
        return false;
    }
    const auto write_time = std::filesystem::last_write_time(path, ec);
    if (ec) {
        error = L"无法读取修改时间：" + std::to_wstring(ec.value());
        return false;
    }
    document.path = path;
    document.size = file_size;
    document.last_write_time = write_time.time_since_epoch().count();
    return true;
}
} // namespace

bool extract_windows_ifilter_file(const std::filesystem::path& path,
                                  std::size_t maximum_bytes,
                                  ContentDocument& document,
                                  std::wstring& error) {
    document = {};
    error.clear();
    if (maximum_bytes == 0) {
        error = L"内容大小上限为 0";
        return false;
    }
    if (!populate_file_metadata(path, document, error)) return false;
    if (document.size > maximum_bytes) {
        error = L"文件超过内容索引大小上限";
        return false;
    }

    ComScope com;
    if (!com.usable()) {
        error = L"初始化 COM 失败：" + hresult_message(com.result());
        return false;
    }

    FilterScope filter;
    void* loaded{};
    const HRESULT load_result = LoadIFilter(path.c_str(), nullptr, &loaded);
    if (FAILED(load_result) || !loaded) {
        error = L"系统未提供此文档格式的 IFilter：" +
                hresult_message(load_result);
        return false;
    }
    *filter.put() = static_cast<IFilter*>(loaded);

    ULONG output_flags{};
    const HRESULT init_result = filter.get()->Init(
        IFILTER_INIT_CANON_PARAGRAPHS | IFILTER_INIT_HARD_LINE_BREAKS |
            IFILTER_INIT_CANON_HYPHENS | IFILTER_INIT_CANON_SPACES |
            IFILTER_INIT_INDEXING_ONLY,
        0, nullptr, &output_flags);
    if (FAILED(init_result)) {
        error = L"初始化文档 IFilter 失败：" + hresult_message(init_result);
        return false;
    }

    std::wstring extracted;
    extracted.reserve((std::min<std::size_t>)(maximum_bytes / 2, 256 * 1024));
    constexpr std::size_t buffer_characters = 4096;
    std::array<wchar_t, buffer_characters> buffer{};
    bool truncated = false;
    for (;;) {
        STAT_CHUNK chunk{};
        const HRESULT chunk_result = filter.get()->GetChunk(&chunk);
        if (chunk_result == FILTER_E_END_OF_CHUNKS) break;
        if (FAILED(chunk_result)) {
            error = L"读取文档内容块失败：" + hresult_message(chunk_result);
            return false;
        }
        if ((chunk.flags & CHUNK_TEXT) == 0) continue;
        if (!extracted.empty()) extracted.push_back(L'\n');
        for (;;) {
            ULONG count = static_cast<ULONG>(buffer.size());
            const HRESULT text_result = filter.get()->GetText(&count, buffer.data());
            if (text_result == FILTER_E_NO_MORE_TEXT ||
                text_result == FILTER_E_NO_TEXT) {
                break;
            }
            if (FAILED(text_result)) {
                error = L"提取文档文本失败：" + hresult_message(text_result);
                return false;
            }
            if (count > buffer.size()) count = static_cast<ULONG>(buffer.size());
            if (count > 0 && buffer[count - 1] == L'\0') --count;
            extracted.append(buffer.data(), count);
            // UTF-8 may require up to four bytes per UTF-16 code unit. Stop before
            // unbounded third-party filters can overrun the configured text cap.
            if (extracted.size() > maximum_bytes) {
                extracted.resize(maximum_bytes);
                truncated = true;
                break;
            }
            if (text_result == FILTER_S_LAST_TEXT) break;
        }
        if (truncated) break;
    }

    if (extracted.empty()) {
        error = L"文档中没有可索引文本（扫描版 PDF 需要 OCR，当前未实现）";
        return false;
    }
    if (!utf16_to_utf8(extracted, document.utf8_text)) {
        error = L"文档文本无法转换为 UTF-8";
        return false;
    }
    if (document.utf8_text.size() > maximum_bytes) {
        error = L"提取文本超过内容索引大小上限";
        document = {};
        return false;
    }
    return !document.utf8_text.empty();
}

bool extract_content_file(const std::filesystem::path& path,
                          std::size_t maximum_bytes,
                          ContentDocument& document,
                          std::wstring& error) {
    const auto extension = path.extension().wstring();
    std::wstring normalized(extension);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](wchar_t value) { return static_cast<wchar_t>(towlower(value)); });
    if (normalized == L".docx") {
        std::wstring ifilter_error;
        if (extract_windows_ifilter_file(path, maximum_bytes, document,
                                         ifilter_error))
            return true;
        std::wstring docx_error;
        if (extract_docx_file(path, maximum_bytes, document, docx_error))
            return true;
        error = L"Word IFilter 失败：" + ifilter_error +
                L"；内置 DOCX 提取失败：" + docx_error;
        return false;
    }
    if (normalized == L".pdf") {
        std::wstring ifilter_error;
        if (extract_windows_ifilter_file(path, maximum_bytes, document,
                                         ifilter_error))
            return true;
        std::wstring pdf_error;
        if (extract_basic_pdf_file(path, maximum_bytes, document, pdf_error))
            return true;
        error = L"PDF IFilter 失败：" + ifilter_error +
                L"；内置 PDF 提取失败：" + pdf_error;
        return false;
    }
    if (normalized == L".doc")
        return extract_windows_ifilter_file(path, maximum_bytes, document, error);
    return extract_plain_text_file(path, maximum_bytes, document, error);
}
} // namespace esm
