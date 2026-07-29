#include "esm/content_index.hpp"

#include <xapian.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace esm {
namespace {
std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) throw std::runtime_error("UTF-16 to UTF-8 conversion failed");
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(),
                            required, nullptr, nullptr) != required) {
        throw std::runtime_error("UTF-16 to UTF-8 conversion failed");
    }
    return result;
}

std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) throw std::runtime_error("UTF-8 to UTF-16 conversion failed");
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(),
                            required) != required) {
        throw std::runtime_error("UTF-8 to UTF-16 conversion failed");
    }
    return result;
}

std::string path_to_utf8(const std::filesystem::path& path) {
    return wide_to_utf8(path.wstring());
}

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

bool path_is_within(const std::filesystem::path& path,
                    const std::filesystem::path& root) {
    const auto normalized = invariant_lower(normalized_path(path).wstring());
    auto prefix = invariant_lower(normalized_path(root).wstring());
    if (normalized == prefix) return true;
    if (!prefix.empty() && prefix.back() != L'\\' && prefix.back() != L'/')
        prefix.push_back(L'\\');
    return normalized.size() > prefix.size() &&
           normalized.compare(0, prefix.size(), prefix) == 0;
}

std::string unique_path_term(const std::filesystem::path& path) {
    constexpr std::uint64_t offset = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    const auto lowered = invariant_lower(normalized_path(path).wstring());
    std::uint64_t hash = offset;
    for (const wchar_t character : lowered) {
        const auto value = static_cast<std::uint16_t>(character);
        hash ^= static_cast<std::uint8_t>(value & 0xffU);
        hash *= prime;
        hash ^= static_cast<std::uint8_t>((value >> 8U) & 0xffU);
        hash *= prime;
    }
    std::ostringstream stream;
    stream << 'Q' << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

void append_u32(std::string& output, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        output.push_back(static_cast<char>((value >> shift) & 0xffU));
}

bool read_u32(std::string_view input, std::size_t& offset,
              std::uint32_t& value) {
    if (offset + 4 > input.size()) return false;
    value = 0;
    for (int shift = 0; shift < 32; shift += 8)
        value |= static_cast<std::uint32_t>(
                     static_cast<unsigned char>(input[offset++])) << shift;
    return true;
}

std::string pack_document_data(const std::filesystem::path& path,
                               std::string_view text) {
    const auto path_bytes = path_to_utf8(normalized_path(path));
    if (path_bytes.size() > UINT32_MAX || text.size() > UINT32_MAX)
        throw std::length_error("content document data is too large");
    std::string result;
    result.reserve(8 + path_bytes.size() + text.size());
    append_u32(result, static_cast<std::uint32_t>(path_bytes.size()));
    result.append(path_bytes);
    append_u32(result, static_cast<std::uint32_t>(text.size()));
    result.append(text);
    return result;
}

bool unpack_document_data(std::string_view data, std::filesystem::path& path,
                          std::string& text) {
    std::size_t offset = 0;
    std::uint32_t path_size{};
    std::uint32_t text_size{};
    if (!read_u32(data, offset, path_size) ||
        offset + path_size > data.size()) return false;
    const auto path_bytes = data.substr(offset, path_size);
    offset += path_size;
    if (!read_u32(data, offset, text_size) ||
        offset + text_size != data.size()) return false;
    path = std::filesystem::path(utf8_to_wide(path_bytes));
    text.assign(data.substr(offset, text_size));
    return true;
}

std::string html_unescape(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    for (std::size_t index = 0; index < input.size();) {
        if (input[index] == '&') {
            struct Entity { std::string_view encoded; char decoded; };
            constexpr std::array<Entity, 5> entities{{
                {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
                {"&quot;", '"'}, {"&#39;", '\''},
            }};
            bool matched = false;
            for (const auto& entity : entities) {
                if (input.substr(index, entity.encoded.size()) == entity.encoded) {
                    output.push_back(entity.decoded);
                    index += entity.encoded.size();
                    matched = true;
                    break;
                }
            }
            if (matched) continue;
        }
        output.push_back(input[index++]);
    }
    return output;
}

void decode_snippet(std::string_view marked, std::wstring& text,
                    std::vector<ContentHighlightRange>& ranges) {
    text.clear();
    ranges.clear();
    std::string segment;
    bool highlighted = false;
    auto flush = [&] {
        if (segment.empty()) return;
        const auto decoded = html_unescape(segment);
        const auto wide = utf8_to_wide(decoded);
        const auto start = text.size();
        text += wide;
        if (highlighted && !wide.empty()) {
            ranges.push_back({static_cast<std::uint32_t>(start),
                              static_cast<std::uint32_t>(wide.size())});
        }
        segment.clear();
    };
    for (const char byte : marked) {
        if (byte == '\x01') {
            flush();
            highlighted = true;
        } else if (byte == '\x02') {
            flush();
            highlighted = false;
        } else {
            segment.push_back(byte);
        }
    }
    flush();
}

bool valid_utf8(std::string_view value) {
    if (value.empty()) return true;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                               static_cast<int>(value.size()), nullptr, 0) > 0;
}

std::string local_bytes_to_utf8(std::string_view value) {
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(
        CP_ACP, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_ACP, 0, value.data(),
                            static_cast<int>(value.size()), wide.data(),
                            required) != required) return {};
    return wide_to_utf8(wide);
}

std::string utf16_bytes_to_utf8(std::string_view bytes, bool big_endian) {
    if ((bytes.size() % 2) != 0) bytes.remove_suffix(1);
    std::wstring wide;
    wide.reserve(bytes.size() / 2);
    for (std::size_t index = 0; index + 1 < bytes.size(); index += 2) {
        const auto first = static_cast<unsigned char>(bytes[index]);
        const auto second = static_cast<unsigned char>(bytes[index + 1]);
        const auto value = big_endian
            ? static_cast<std::uint16_t>((first << 8U) | second)
            : static_cast<std::uint16_t>(first | (second << 8U));
        wide.push_back(static_cast<wchar_t>(value));
    }
    return wide_to_utf8(wide);
}
} // namespace

struct XapianContentIndex::Impl {
    explicit Impl(std::filesystem::path requested_path)
        : database_path(std::move(requested_path)) {
        std::error_code error;
        std::filesystem::create_directories(database_path.parent_path(), error);
        database = std::make_unique<Xapian::WritableDatabase>(
            path_to_utf8(database_path), Xapian::DB_CREATE_OR_OPEN);
        ready = true;
        message = L"内容索引已就绪";
    }

    std::filesystem::path database_path;
    mutable std::mutex mutex;
    mutable std::shared_mutex committed_revision_mutex;
    std::unique_ptr<Xapian::WritableDatabase> database;
    bool ready{};
    bool indexing{};
    std::wstring message;
};

XapianContentIndex::XapianContentIndex(std::filesystem::path database_path)
    : impl_(std::make_unique<Impl>(std::move(database_path))) {}

XapianContentIndex::~XapianContentIndex() = default;

void XapianContentIndex::upsert(const ContentDocument& content) {
    std::scoped_lock lock(impl_->mutex);
    Xapian::Document document;
    document.add_boolean_term(unique_path_term(content.path));
    document.set_data(pack_document_data(content.path, content.utf8_text));
    document.add_value(0, Xapian::sortable_serialise(
                              static_cast<double>(content.size)));
    document.add_value(1, Xapian::sortable_serialise(
                              static_cast<double>(content.last_write_time)));

    Xapian::TermGenerator generator;
    generator.set_document(document);
    generator.set_flags(Xapian::TermGenerator::FLAG_NGRAMS);
    generator.set_stemming_strategy(Xapian::TermGenerator::STEM_NONE);
    generator.index_text(content.utf8_text);
    impl_->database->replace_document(unique_path_term(content.path), document);
}

void XapianContentIndex::remove(const std::filesystem::path& path) {
    std::scoped_lock lock(impl_->mutex);
    impl_->database->delete_document(unique_path_term(path));
}

void XapianContentIndex::commit() {
    std::unique_lock revision_lock(impl_->committed_revision_mutex);
    std::scoped_lock lock(impl_->mutex);
    impl_->database->commit();
}

ContentSearchResponse XapianContentIndex::search(
    std::wstring_view query_text, std::size_t limit) const {
    ContentSearchResponse empty_response;
    if (query_text.empty() || limit == 0) return empty_response;

    std::filesystem::path database_path;
    {
        std::scoped_lock lock(impl_->mutex);
        database_path = impl_->database_path;
    }

    // Keep this read snapshot stable while the service publishes a writer
    // revision. Multiple searches may still run concurrently.
    std::shared_lock revision_lock(impl_->committed_revision_mutex);

    constexpr unsigned maximum_attempts = 3;
    for (unsigned attempt = 0; attempt < maximum_attempts; ++attempt) {
        try {
            ContentSearchResponse response;
            Xapian::Database database(path_to_utf8(database_path));
            Xapian::QueryParser parser;
            parser.set_database(database);
            parser.set_default_op(Xapian::Query::OP_AND);
            parser.set_stemming_strategy(Xapian::QueryParser::STEM_NONE);
            const unsigned flags = Xapian::QueryParser::FLAG_DEFAULT |
                                   Xapian::QueryParser::FLAG_PHRASE |
                                   Xapian::QueryParser::FLAG_BOOLEAN |
                                   Xapian::QueryParser::FLAG_LOVEHATE |
                                   Xapian::QueryParser::FLAG_WILDCARD |
                                   Xapian::QueryParser::FLAG_NGRAMS;
            const auto query =
                parser.parse_query(wide_to_utf8(query_text), flags);
            Xapian::Enquire enquire(database);
            enquire.set_query(query);
            const auto bounded_limit = static_cast<Xapian::doccount>(
                (std::min<std::size_t>)(limit, 500));
            const auto matches = enquire.get_mset(0, bounded_limit);
            response.estimated_matches = matches.get_matches_estimated();
            response.hits.reserve(matches.size());

            for (auto iterator = matches.begin(); iterator != matches.end();
                 ++iterator) {
                std::filesystem::path path;
                std::string text;
                if (!unpack_document_data(iterator.get_document().get_data(),
                                          path, text)) continue;
                ContentSearchHit hit;
                hit.path = std::move(path);
                hit.relevance_percent = iterator.get_percent();
                const unsigned snippet_flags =
                    Xapian::MSet::SNIPPET_BACKGROUND_MODEL |
                    Xapian::MSet::SNIPPET_EXHAUSTIVE |
                    Xapian::MSet::SNIPPET_NGRAMS;
                const auto marked = matches.snippet(
                    text, 260, Xapian::Stem(), snippet_flags, "\x01", "\x02",
                    "...");
                decode_snippet(marked, hit.snippet, hit.highlights);
                response.hits.push_back(std::move(hit));
            }
            return response;
        } catch (const Xapian::DatabaseModifiedError& error) {
            if (attempt + 1 == maximum_attempts) {
                throw std::runtime_error(
                    "content database kept changing during query: " +
                    error.get_description());
            }
        } catch (const Xapian::Error& error) {
            throw std::runtime_error("Xapian content query failed: " +
                                     error.get_description());
        }
    }
    return empty_response;
}

ContentIndexStatus XapianContentIndex::status() const {
    std::scoped_lock lock(impl_->mutex);
    ContentIndexStatus result;
    result.documents = impl_->database->get_doccount();
    result.ready = impl_->ready;
    result.indexing = impl_->indexing;
    result.message = impl_->message;
    return result;
}

void XapianContentIndex::set_indexing(bool indexing, std::wstring message) {
    std::scoped_lock lock(impl_->mutex);
    impl_->indexing = indexing;
    impl_->message = std::move(message);
    impl_->ready = true;
}

struct ShardedContentIndex::Impl {
    struct Shard {
        std::filesystem::path root;
        std::unique_ptr<XapianContentIndex> index;
    };

    explicit Impl(std::vector<ContentIndexShardSpec> specs) {
        if (specs.empty())
            throw std::invalid_argument("at least one content index shard is required");
        shards.reserve(specs.size());
        for (auto& spec : specs) {
            if (spec.root.empty() || spec.database_path.empty())
                throw std::invalid_argument("content shard root and database path are required");
            auto root = normalized_path(spec.root);
            for (const auto& existing : shards) {
                if (path_is_within(root, existing.root) ||
                    path_is_within(existing.root, root)) {
                    throw std::invalid_argument("content shard roots must not overlap");
                }
            }
            shards.push_back(
                {std::move(root),
                 std::make_unique<XapianContentIndex>(
                     normalized_path(spec.database_path))});
        }
    }

    std::size_t route(const std::filesystem::path& path) const {
        for (std::size_t index = 0; index < shards.size(); ++index) {
            if (path_is_within(path, shards[index].root)) return index;
        }
        throw std::invalid_argument("content document is outside configured roots");
    }

    std::vector<Shard> shards;
};

ShardedContentIndex::ShardedContentIndex(
    std::vector<ContentIndexShardSpec> shards)
    : impl_(std::make_unique<Impl>(std::move(shards))) {}

ShardedContentIndex::~ShardedContentIndex() = default;

void ShardedContentIndex::upsert(const ContentDocument& document) {
    impl_->shards[impl_->route(document.path)].index->upsert(document);
}

void ShardedContentIndex::remove(const std::filesystem::path& path) {
    impl_->shards[impl_->route(path)].index->remove(path);
}

void ShardedContentIndex::commit() {
    for (auto& shard : impl_->shards) shard.index->commit();
}

ContentSearchResponse ShardedContentIndex::search(
    std::wstring_view query, std::size_t limit) const {
    ContentSearchResponse response;
    if (query.empty() || limit == 0) return response;
    for (const auto& shard : impl_->shards) {
        auto partial = shard.index->search(query, limit);
        const auto remaining =
            (std::numeric_limits<std::uint64_t>::max)() -
            response.estimated_matches;
        response.estimated_matches +=
            (std::min)(remaining, partial.estimated_matches);
        response.hits.insert(response.hits.end(),
                             std::make_move_iterator(partial.hits.begin()),
                             std::make_move_iterator(partial.hits.end()));
    }
    std::stable_sort(response.hits.begin(), response.hits.end(),
                     [](const ContentSearchHit& left,
                        const ContentSearchHit& right) {
                         if (left.relevance_percent != right.relevance_percent)
                             return left.relevance_percent >
                                    right.relevance_percent;
                         return invariant_lower(left.path.wstring()) <
                                invariant_lower(right.path.wstring());
                     });
    if (response.hits.size() > limit) response.hits.resize(limit);
    return response;
}

ContentIndexStatus ShardedContentIndex::status() const {
    ContentIndexStatus aggregate;
    aggregate.ready = true;
    std::vector<std::wstring> messages;
    for (const auto& shard : impl_->shards) {
        const auto current = shard.index->status();
        const auto remaining =
            (std::numeric_limits<std::uint64_t>::max)() - aggregate.documents;
        aggregate.documents += (std::min)(remaining, current.documents);
        aggregate.ready = aggregate.ready && current.ready;
        aggregate.indexing = aggregate.indexing || current.indexing;
        if (!current.message.empty()) messages.push_back(current.message);
    }
    if (aggregate.indexing) {
        aggregate.message = L"正在建立多卷内容索引（" +
                            std::to_wstring(impl_->shards.size()) + L" 个根）";
    } else if (aggregate.ready) {
        aggregate.message = L"多卷内容索引已就绪（" +
                            std::to_wstring(impl_->shards.size()) + L" 个根）";
    } else if (!messages.empty()) {
        aggregate.message = messages.front();
    }
    return aggregate;
}

void ShardedContentIndex::set_indexing(bool indexing, std::wstring message) {
    for (auto& shard : impl_->shards)
        shard.index->set_indexing(indexing, message);
}

std::size_t ShardedContentIndex::shard_count() const noexcept {
    return impl_->shards.size();
}

ContentIndex& ShardedContentIndex::shard(std::size_t index) {
    return *impl_->shards.at(index).index;
}

const ContentIndex& ShardedContentIndex::shard(std::size_t index) const {
    return *impl_->shards.at(index).index;
}

const std::filesystem::path& ShardedContentIndex::shard_root(
    std::size_t index) const {
    return impl_->shards.at(index).root;
}

bool is_supported_content_path(const std::filesystem::path& path) {
    static const std::unordered_set<std::wstring> supported{
        L".txt", L".md", L".log", L".csv", L".json", L".xml",
        L".yaml", L".yml", L".ini", L".cfg", L".c", L".cc",
        L".cpp", L".cxx", L".h", L".hpp", L".cs", L".java",
        L".py", L".js", L".ts", L".tsx", L".jsx", L".rs", L".go",
        L".cmake", L".ps1", L".bat", L".cmd", L".sql", L".html",
        L".htm", L".css", L".toml", L".pdf", L".doc", L".docx"};
    auto extension = invariant_lower(path.extension().wstring());
    return supported.contains(extension);
}

bool extract_plain_text_file(const std::filesystem::path& path,
                             std::size_t maximum_bytes,
                             ContentDocument& document,
                             std::wstring& error) {
    error.clear();
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(path, filesystem_error)) {
        error = L"不是普通文件";
        return false;
    }
    const auto size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error) {
        error = L"无法读取文件大小";
        return false;
    }
    if (size > maximum_bytes) {
        error = L"文件超过内容索引大小限制";
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = L"无法打开文件";
        return false;
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!bytes.empty()) {
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
            error = L"读取文件失败";
            return false;
        }
    }

    std::string text;
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xef &&
        static_cast<unsigned char>(bytes[1]) == 0xbb &&
        static_cast<unsigned char>(bytes[2]) == 0xbf) {
        text.assign(bytes.begin() + 3, bytes.end());
    } else if (bytes.size() >= 2 &&
               static_cast<unsigned char>(bytes[0]) == 0xff &&
               static_cast<unsigned char>(bytes[1]) == 0xfe) {
        text = utf16_bytes_to_utf8(std::string_view(bytes).substr(2), false);
    } else if (bytes.size() >= 2 &&
               static_cast<unsigned char>(bytes[0]) == 0xfe &&
               static_cast<unsigned char>(bytes[1]) == 0xff) {
        text = utf16_bytes_to_utf8(std::string_view(bytes).substr(2), true);
    } else {
        const auto probe_size = (std::min<std::size_t>)(bytes.size(), 4096);
        if (std::find(bytes.begin(), bytes.begin() + probe_size, '\0') !=
            bytes.begin() + probe_size) {
            error = L"检测到二进制内容";
            return false;
        }
        text = valid_utf8(bytes) ? bytes : local_bytes_to_utf8(bytes);
    }
    if (!valid_utf8(text)) {
        error = L"无法解码文本编码";
        return false;
    }

    document.path = normalized_path(path);
    document.utf8_text = std::move(text);
    document.size = size;
    document.last_write_time = 0;
    return true;
}
} // namespace esm
