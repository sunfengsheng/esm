#include "esm/content_index.hpp"
#include "esm/content_named_pipe.hpp"
#include "esm/content_roots.hpp"
#include "esm/content_protocol.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    require(required > 0, "failed to size UTF-8 test conversion");
    std::string result(static_cast<std::size_t>(required), '\0');
    require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                static_cast<int>(value.size()), result.data(),
                                required, nullptr, nullptr) == required,
            "failed to convert UTF-16 test data to UTF-8");
    return result;
}

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(std::wstring_view label) {
        const auto nonce = static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path() /
                (std::wstring(label) + L"-" + std::to_wstring(GetCurrentProcessId()) +
                 L"-" + std::to_wstring(nonce));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

void write_bytes(const std::filesystem::path& path,
                 const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "failed to create content test file");
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    require(static_cast<bool>(output), "failed to write content test file");
}

void write_utf8(const std::filesystem::path& path, std::string_view text) {
    write_bytes(path, std::vector<std::uint8_t>(text.begin(), text.end()));
}

void test_protocol_round_trip() {
    esm::ContentIpcSearchRequest request;
    request.query = L"\u5185\u5bb9\u7d22\u5f15 AND Xapian";
    request.limit = 37;
    const auto request_payload = esm::encode_content_search_request(request);

    esm::ContentIpcSearchRequest decoded_request;
    std::string error;
    require(esm::decode_content_search_request(request_payload, decoded_request,
                                               error),
            "content search request should decode");
    require(decoded_request.query == request.query &&
                decoded_request.limit == request.limit,
            "content search request should round-trip");

    const auto frame_bytes = esm::encode_content_frame(
        esm::ContentIpcMessageType::search_request, 73, request_payload);
    esm::ContentIpcFrame frame;
    require(esm::decode_content_frame(frame_bytes, frame, error),
            "content IPC frame should decode");
    require(frame.header.type == esm::ContentIpcMessageType::search_request &&
                frame.header.request_id == 73 &&
                frame.payload == request_payload,
            "content IPC frame should round-trip");

    esm::ContentIpcSearchResponse response;
    response.error = ERROR_SUCCESS;
    response.message = L"\u67e5\u8be2\u5b8c\u6210";
    response.result.estimated_matches = 11;
    esm::ContentSearchHit hit;
    hit.path = L"D:\\\u8d44\u6599\\\u4e2d\u6587.txt";
    hit.snippet = L"\u8fd9\u91cc\u662f\U0001f600\u5185\u5bb9\u7d22\u5f15\u6458\u8981";
    hit.relevance_percent = 91;
    hit.highlights.push_back({6, 4});
    response.result.hits.push_back(hit);

    const auto response_payload = esm::encode_content_search_response(response);
    esm::ContentIpcSearchResponse decoded_response;
    require(esm::decode_content_search_response(response_payload, decoded_response,
                                                error),
            "content search response should decode");
    require(decoded_response.message == response.message &&
                decoded_response.result.estimated_matches == 11 &&
                decoded_response.result.hits.size() == 1,
            "content search response metadata should round-trip");
    const auto& decoded_hit = decoded_response.result.hits.front();
    require(decoded_hit.path == hit.path && decoded_hit.snippet == hit.snippet &&
                decoded_hit.relevance_percent == 91 &&
                decoded_hit.highlights.size() == 1 &&
                decoded_hit.highlights.front().start_utf16 == 6 &&
                decoded_hit.highlights.front().length_utf16 == 4,
            "content hit and UTF-16 highlight should round-trip");

    esm::ContentIpcStatusResponse status;
    status.error = ERROR_SUCCESS;
    status.status.documents = 123;
    status.status.ready = true;
    status.status.indexing = false;
    status.status.message = L"\u5185\u5bb9\u7d22\u5f15\u5df2\u5c31\u7eea";
    const auto status_payload = esm::encode_content_status_response(status);
    esm::ContentIpcStatusResponse decoded_status;
    require(esm::decode_content_status_response(status_payload, decoded_status,
                                                error),
            "content status response should decode");
    require(decoded_status.status.documents == 123 &&
                decoded_status.status.ready && !decoded_status.status.indexing &&
                decoded_status.status.message == status.status.message,
            "content status response should round-trip");
}

class FakeContentIndex final : public esm::ContentIndex {
public:
    void upsert(const esm::ContentDocument&) override {}
    void remove(const std::filesystem::path&) override {}
    void commit() override {}

    [[nodiscard]] esm::ContentSearchResponse search(
        std::wstring_view query, std::size_t limit) const override {
        esm::ContentSearchResponse response;
        if (query.empty() || limit == 0) return response;
        response.estimated_matches = 1;
        esm::ContentSearchHit hit;
        hit.path = L"D:\\content\\sample.txt";
        hit.snippet = L"prefix \u5185\u5bb9\u7d22\u5f15 suffix";
        hit.highlights.push_back({7, 4});
        hit.relevance_percent = 88;
        response.hits.push_back(std::move(hit));
        return response;
    }

    [[nodiscard]] esm::ContentIndexStatus status() const override {
        return {9, true, false, L"\u5185\u5bb9\u670d\u52a1\u5df2\u5c31\u7eea"};
    }

    void set_indexing(bool, std::wstring) override {}
};

void test_named_pipe_round_trip() {
    FakeContentIndex index;
    std::atomic_bool stop{false};
    std::atomic<std::uint32_t> server_error{ERROR_SUCCESS};
    const auto pipe = L"everything_sm_content_test_" +
                      std::to_wstring(GetCurrentProcessId()) + L"_" +
                      std::to_wstring(static_cast<unsigned long long>(
                          std::chrono::steady_clock::now()
                              .time_since_epoch().count()));
    std::thread server([&] {
        server_error.store(esm::serve_content_named_pipe(pipe, index, stop),
                           std::memory_order_relaxed);
    });

    try {
        const auto status = esm::query_content_named_pipe_status(pipe, 2'000);
        require(status.error == ERROR_SUCCESS &&
                    status.response.status.documents == 9 &&
                    status.response.status.message ==
                        L"\u5185\u5bb9\u670d\u52a1\u5df2\u5c31\u7eea",
                "content status should cross the named pipe");

        esm::ContentIpcSearchRequest request;
        request.query = L"\u5185\u5bb9\u7d22\u5f15";
        request.limit = 10;
        const auto search = esm::query_content_named_pipe_search(
            pipe, request, 2'000);
        require(search.error == ERROR_SUCCESS &&
                    search.response.result.hits.size() == 1 &&
                    search.response.result.hits.front().relevance_percent == 88 &&
                    search.response.result.hits.front().highlights.size() == 1,
                "content search should cross the named pipe");
    } catch (...) {
        stop.store(true, std::memory_order_relaxed);
        server.join();
        throw;
    }

    stop.store(true, std::memory_order_relaxed);
    server.join();
    require(server_error.load(std::memory_order_relaxed) == ERROR_SUCCESS,
            "content named pipe server should stop cleanly");
}

void test_plain_text_extraction() {
    TemporaryDirectory directory(L"everything-sm-content-extract");
    esm::ContentDocument document;
    std::wstring error;

    const auto utf8_path = directory.path() / L"utf8.txt";
    const auto utf8_text = utf8(L"hello Xapian \u5185\u5bb9");
    write_utf8(utf8_path, utf8_text);
    require(esm::extract_plain_text_file(utf8_path, 1024, document, error),
            "UTF-8 text should be extracted");
    require(document.utf8_text == utf8_text,
            "UTF-8 text should remain unchanged");
    require(document.path.is_absolute() &&
                document.size == document.utf8_text.size(),
            "extracted document metadata should be populated");

    const auto utf16_path = directory.path() / L"utf16le.txt";
    const std::vector<std::uint8_t> utf16le{
        0xff, 0xfe, 'A', 0x00, 0x85, 0x51, 0xb9, 0x5b}; // A followed by two CJK characters
    write_bytes(utf16_path, utf16le);
    require(esm::extract_plain_text_file(utf16_path, 1024, document, error),
            "UTF-16LE text should be extracted");
    require(document.utf8_text == utf8(L"A\u5185\u5bb9"),
            "UTF-16LE text should convert to UTF-8");

    const auto binary_path = directory.path() / L"binary.txt";
    write_bytes(binary_path, {'A', 0x00, 'B'});
    require(!esm::extract_plain_text_file(binary_path, 1024, document, error) &&
                !error.empty(),
            "NUL-containing binary data should be skipped");

    const auto large_path = directory.path() / L"large.txt";
    write_utf8(large_path, "12345");
    require(!esm::extract_plain_text_file(large_path, 4, document, error) &&
                !error.empty(),
            "files above the configured size limit should be skipped");

    require(esm::is_supported_content_path(L"sample.TXT") &&
                esm::is_supported_content_path(L"sample.md") &&
                !esm::is_supported_content_path(L"sample.pdf"),
            "supported content extensions should be case-insensitive and explicit");
}

void test_content_path_filter_and_root_keys() {
    TemporaryDirectory directory(L"everything-sm-content-filter");
    const auto root = directory.path() / L"root";
    std::filesystem::create_directories(root / L"docs");

    esm::ContentPathFilter defaults(root);
    require(!defaults.excluded(root / L"docs" / L"notes.txt"),
            "ordinary content paths should be included");
    require(defaults.excluded(root / L"Windows" / L"system.log"),
            "top-level Windows should be excluded by default");
    require(defaults.excluded(root / L"project" / L".git" / L"config"),
            ".git directories should be excluded at any depth");
    require(defaults.excluded(root / L"project" / L"node_modules" / L"x.js"),
            "node_modules should be excluded at any depth");
    require(defaults.excluded(directory.path() / L"outside.txt"),
            "paths outside the configured root should be excluded");

    esm::ContentPathFilter custom(
        root, false,
        {std::filesystem::path(L"private"), root / L"docs" / L"generated"});
    require(!custom.excluded(root / L"Windows" / L"user-created.txt"),
            "default exclusions should be disableable");
    require(custom.excluded(root / L"private" / L"secret.txt") &&
                custom.excluded(root / L"docs" / L"generated" / L"out.txt"),
            "relative and absolute custom exclusions should be honored");

    const auto first = esm::content_root_database_key(root);
    const auto repeat = esm::content_root_database_key(root);
    const auto other = esm::content_root_database_key(directory.path() / L"other");
    require(!first.empty() && first == repeat && first != other,
            "content root database keys should be stable and path-specific");
}

void test_sharded_xapian_index() {
    TemporaryDirectory directory(L"everything-sm-content-shards");
    const auto root_a = directory.path() / L"root-a";
    const auto root_b = directory.path() / L"root-b";
    std::filesystem::create_directories(root_a);
    std::filesystem::create_directories(root_b);

    esm::ShardedContentIndex index({
        {root_a, directory.path() / L"db-a"},
        {root_b, directory.path() / L"db-b"},
    });
    require(index.shard_count() == 2 && index.shard_root(0) == root_a &&
                index.shard_root(1) == root_b,
            "sharded index should retain root-to-database mapping");

    const auto path_a = root_a / L"alpha.txt";
    const auto path_b = root_b / L"beta.txt";
    index.upsert({path_a, "shared marker alpha", 19, 10});
    index.upsert({path_b, "shared marker beta", 18, 20});
    index.commit();

    auto results = index.search(L"shared marker", 10);
    require(results.estimated_matches == 2 && results.hits.size() == 2,
            "global content queries should merge matches from all shards");
    results = index.search(L"shared marker", 1);
    require(results.estimated_matches == 2 && results.hits.size() == 1,
            "global content query limits should apply after shard merge");

    index.shard(0).set_indexing(true, L"root a indexing");
    index.shard(1).set_indexing(false, L"root b ready");
    auto status = index.status();
    require(status.documents == 2 && status.ready && status.indexing,
            "sharded status should sum documents and aggregate indexing state");

    index.remove(path_b);
    index.commit();
    require(index.search(L"beta", 10).hits.empty() &&
                index.status().documents == 1,
            "remove should route to the shard that owns the path");

    bool outside_rejected = false;
    try {
        index.remove(directory.path() / L"outside.txt");
    } catch (const std::invalid_argument&) {
        outside_rejected = true;
    }
    require(outside_rejected,
            "documents outside all configured roots should be rejected");
}

void test_xapian_search_during_commits() {
    TemporaryDirectory directory(L"everything-sm-content-concurrency");
    esm::XapianContentIndex index(directory.path() / L"db");
    const auto path = directory.path() / L"changing.txt";
    index.upsert({path, "stable marker revision 0", 24, 0});
    index.commit();

    std::atomic_bool start{false};
    std::atomic_bool writer_done{false};
    std::atomic_bool writer_failed{false};
    std::thread writer([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        try {
            for (std::int64_t revision = 1; revision <= 60; ++revision) {
                const auto text =
                    "stable marker revision " + std::to_string(revision);
                index.upsert({path, text, text.size(), revision});
                index.commit();
                std::this_thread::yield();
            }
        } catch (...) {
            writer_failed.store(true, std::memory_order_release);
        }
        writer_done.store(true, std::memory_order_release);
    });

    start.store(true, std::memory_order_release);
    std::size_t searches = 0;
    bool search_failed = false;
    while (!writer_done.load(std::memory_order_acquire) || searches < 120) {
        try {
            const auto response = index.search(L"stable marker", 10);
            if (response.hits.size() != 1 || response.hits.front().path != path) {
                search_failed = true;
                break;
            }
        } catch (...) {
            search_failed = true;
            break;
        }
        ++searches;
    }
    writer.join();

    require(!writer_failed.load(std::memory_order_acquire),
            "content writer should survive concurrent searches");
    require(!search_failed && searches >= 120,
            "content search should survive concurrent commits");
}

void test_xapian_index_lifecycle() {
    TemporaryDirectory directory(L"everything-sm-content-xapian");
    const auto database_path = directory.path() / L"db";
    const auto english_path = directory.path() / L"alpha.txt";
    const auto chinese_path = directory.path() / L"\u4e2d\u6587.md";

    {
        esm::XapianContentIndex index(database_path);
        index.set_indexing(true, L"\u6d4b\u8bd5\u7d22\u5f15\u4e2d");
        index.upsert({english_path,
                      "Xapian provides a dedicated full text database. "
                      "filename search remains isolated.",
                      87, 10});
        index.upsert({chinese_path,
                      utf8(L"\u8fd9\u91cc\u4f7f\u7528 Xapian \u5efa\u7acb"
                           L"\u72ec\u7acb\u7684\u6587\u4ef6\u5185\u5bb9\u7d22\u5f15\u3002"
                           L"\u666e\u901a\u6587\u4ef6\u540d\u641c\u7d22\u4e0d\u4f1a"
                           L"\u7ecf\u8fc7\u5185\u5bb9\u670d\u52a1\u3002"),
                      96, 20});
        index.commit();
        index.set_indexing(false, L"\u6d4b\u8bd5\u7d22\u5f15\u5c31\u7eea");

        const auto english = index.search(L"dedicated database", 20);
        require(english.hits.size() == 1 &&
                    english.hits.front().path == english_path &&
                    !english.hits.front().snippet.empty() &&
                    !english.hits.front().highlights.empty(),
                "English terms should return a highlighted snippet");

        const auto chinese = index.search(L"\u5185\u5bb9\u7d22\u5f15", 20);
        if (chinese.hits.size() != 1 ||
            chinese.hits.front().path != chinese_path ||
            chinese.hits.front().snippet.empty() ||
            chinese.hits.front().highlights.empty()) {
            std::wcerr << L"CJK diagnostics: hits=" << chinese.hits.size();
            if (!chinese.hits.empty()) {
                std::wcerr << L" path=" << chinese.hits.front().path
                           << L" expected=" << chinese_path
                           << L" snippet=" << chinese.hits.front().snippet
                           << L" highlights="
                           << chinese.hits.front().highlights.size();
            }
            std::wcerr << L"\n";
        }
        require(chinese.hits.size() == 1 &&
                    chinese.hits.front().path == chinese_path &&
                    !chinese.hits.front().snippet.empty() &&
                    !chinese.hits.front().highlights.empty(),
                "CJK n-gram terms should return a highlighted snippet");

        index.upsert({english_path, "replacement marker omega", 24, 30});
        index.commit();
        require(index.search(L"dedicated database", 20).hits.empty(),
                "upsert should replace terms from the old document");
        require(index.search(L"replacement marker", 20).hits.size() == 1,
                "upsert should make replacement content searchable");

        index.remove(chinese_path);
        index.commit();
        require(index.search(L"\u5185\u5bb9\u7d22\u5f15", 20).hits.empty(),
                "removed documents should no longer be searchable");

        const auto status = index.status();
        require(status.documents == 1 && status.ready && !status.indexing &&
                    status.message == L"\u6d4b\u8bd5\u7d22\u5f15\u5c31\u7eea",
                "content index status should reflect committed documents");
    }
}
} // namespace

int main() {
    try {
        test_protocol_round_trip();
        test_named_pipe_round_trip();
        test_plain_text_extraction();
        test_content_path_filter_and_root_keys();
        test_sharded_xapian_index();
        test_xapian_search_during_commits();
        test_xapian_index_lifecycle();
        std::cout << "all content tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "content test failure: " << error.what() << "\n";
        return 1;
    }
}
