#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace esm {
struct ContentDocument {
    std::filesystem::path path;
    std::string utf8_text;
    std::uint64_t size{};
    std::int64_t last_write_time{};
};

struct ContentHighlightRange {
    std::uint32_t start_utf16{};
    std::uint32_t length_utf16{};
};

struct ContentSearchHit {
    std::filesystem::path path;
    std::wstring snippet;
    std::vector<ContentHighlightRange> highlights;
    std::uint32_t relevance_percent{};
};

struct ContentSearchResponse {
    std::uint64_t estimated_matches{};
    std::vector<ContentSearchHit> hits;
};

struct ContentIndexStatus {
    std::uint64_t documents{};
    bool ready{};
    bool indexing{};
    std::wstring message;
};

class ContentIndex {
public:
    virtual ~ContentIndex() = default;
    virtual void upsert(const ContentDocument& document) = 0;
    virtual void remove(const std::filesystem::path& path) = 0;
    virtual void commit() = 0;
    [[nodiscard]] virtual ContentSearchResponse search(
        std::wstring_view query, std::size_t limit) const = 0;
    [[nodiscard]] virtual ContentIndexStatus status() const = 0;
    virtual void set_indexing(bool indexing, std::wstring message = {}) = 0;
};

class XapianContentIndex final : public ContentIndex {
public:
    explicit XapianContentIndex(std::filesystem::path database_path);
    ~XapianContentIndex() override;
    XapianContentIndex(const XapianContentIndex&) = delete;
    XapianContentIndex& operator=(const XapianContentIndex&) = delete;

    void upsert(const ContentDocument& document) override;
    void remove(const std::filesystem::path& path) override;
    void commit() override;
    [[nodiscard]] ContentSearchResponse search(
        std::wstring_view query, std::size_t limit) const override;
    [[nodiscard]] ContentIndexStatus status() const override;
    void set_indexing(bool indexing, std::wstring message = {}) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] bool is_supported_content_path(
    const std::filesystem::path& path);
[[nodiscard]] bool extract_plain_text_file(
    const std::filesystem::path& path,
    std::size_t maximum_bytes,
    ContentDocument& document,
    std::wstring& error);
} // namespace esm
