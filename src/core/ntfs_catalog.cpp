#include "esm/ntfs_catalog.hpp"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <functional>
#include <limits>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace esm {
namespace {
static_assert(std::is_standard_layout_v<CatalogBaseNode>);
static_assert(std::is_trivially_copyable_v<CatalogBaseNode>);
static_assert(sizeof(CatalogBaseNode) == 48);
static_assert(offsetof(CatalogBaseNode, id) == 0);
static_assert(offsetof(CatalogBaseNode, parent_id) == 8);
static_assert(offsetof(CatalogBaseNode, size) == 16);
static_assert(offsetof(CatalogBaseNode, last_write_time) == 24);
static_assert(offsetof(CatalogBaseNode, attributes) == 32);
static_assert(offsetof(CatalogBaseNode, name_offset) == 36);
static_assert(offsetof(CatalogBaseNode, name_length) == 40);
static_assert(offsetof(CatalogBaseNode, directory) == 44);
void checked_add_name_chars(std::size_t& total, std::size_t amount) {
    constexpr auto maximum =
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
    if (amount > maximum || total > maximum - amount) {
        throw std::length_error("catalog name arena exceeds 32-bit offsets");
    }
    total += amount;
}
} // namespace

NtfsCatalog::NtfsCatalog(
    std::wstring volume_root,
    std::uint64_t root_id,
    std::size_t auto_compaction_threshold)
    : volume_root_(std::move(volume_root)),
      root_id_(root_id),
      auto_compaction_threshold_(auto_compaction_threshold) {
    while (volume_root_.size() > 3 &&
           (volume_root_.back() == L'\\' || volume_root_.back() == L'/')) {
        volume_root_.pop_back();
    }
}

void NtfsCatalog::replace(const std::vector<FileRecord>& records) {
    std::vector<std::size_t> order(records.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&](std::size_t left,
                                               std::size_t right) {
        if (records[left].id != records[right].id) {
            return records[left].id < records[right].id;
        }
        return left < right;
    });

    std::size_t total_name_chars = 0;
    std::size_t unique_count = 0;
    for (std::size_t first = 0; first < order.size();) {
        std::size_t last = first + 1;
        while (last < order.size() &&
               records[order[last]].id == records[order[first]].id) {
            ++last;
        }
        checked_add_name_chars(total_name_chars,
                               records[order[last - 1]].name.size());
        ++unique_count;
        first = last;
    }

    std::vector<BaseNode> replacement;
    std::vector<wchar_t> names;
    replacement.reserve(unique_count);
    names.reserve(total_name_chars);
    for (std::size_t first = 0; first < order.size();) {
        std::size_t last = first + 1;
        while (last < order.size() &&
               records[order[last]].id == records[order[first]].id) {
            ++last;
        }
        const auto& record = records[order[last - 1]];
        BaseNode node;
        node.id = record.id;
        node.parent_id = record.parent_id;
        node.size = record.size;
        node.last_write_time = record.last_write_time;
        node.attributes = record.attributes;
        node.directory = record.directory ? 1 : 0;
        node.name_offset = static_cast<std::uint32_t>(names.size());
        node.name_length = static_cast<std::uint32_t>(record.name.size());
        names.insert(names.end(), record.name.begin(), record.name.end());
        replacement.push_back(node);
        first = last;
    }

    std::unique_lock lock(mutex_);
    base_nodes_ = std::move(replacement);
    base_names_ = std::move(names);
    mapped_base_ = {};
    overlay_.clear();
    overlay_.rehash(0);
    tombstones_.clear();
    tombstones_.rehash(0);
    live_size_ = base_nodes_.size();
    compaction_count_ = 0;
}

bool NtfsCatalog::replace_mapped(CatalogMappedBase storage) {
    if (!storage.owner ||
        (storage.node_count != 0 && storage.nodes == nullptr) ||
        (storage.name_count != 0 && storage.names == nullptr)) {
        return false;
    }
    std::uint64_t previous_id = 0;
    for (std::size_t i = 0; i < storage.node_count; ++i) {
        const auto& node = storage.nodes[i];
        if (node.id == 0 || (i != 0 && node.id <= previous_id) ||
            node.directory > 1 || node.reserved[0] != 0 ||
            node.reserved[1] != 0 || node.reserved[2] != 0 ||
            node.name_offset > storage.name_count ||
            node.name_length > storage.name_count - node.name_offset) {
            return false;
        }
        previous_id = node.id;
    }

    std::unique_lock lock(mutex_);
    base_nodes_ = {};
    base_names_ = {};
    mapped_base_ = std::move(storage);
    overlay_.clear();
    overlay_.rehash(0);
    tombstones_.clear();
    tombstones_.rehash(0);
    live_size_ = mapped_base_.node_count;
    compaction_count_ = 0;
    return true;
}

std::span<const NtfsCatalog::BaseNode>
NtfsCatalog::base_nodes_unlocked() const {
    if (mapped_base_.owner) {
        return {mapped_base_.nodes, mapped_base_.node_count};
    }
    return base_nodes_;
}

std::span<const wchar_t> NtfsCatalog::base_names_unlocked() const {
    if (mapped_base_.owner) {
        return {mapped_base_.names, mapped_base_.name_count};
    }
    return base_names_;
}

void NtfsCatalog::materialize_mapped_base_unlocked() {
    if (!mapped_base_.owner) return;
    const auto nodes = base_nodes_unlocked();
    const auto names = base_names_unlocked();
    std::vector<BaseNode> owned_nodes;
    std::vector<wchar_t> owned_names;
    if (!nodes.empty()) owned_nodes.assign(nodes.begin(), nodes.end());
    if (!names.empty()) owned_names.assign(names.begin(), names.end());
    base_nodes_ = std::move(owned_nodes);
    base_names_ = std::move(owned_names);
    mapped_base_ = {};
}

bool NtfsCatalog::materialize_mapped_base() {
    std::unique_lock lock(mutex_);
    if (!mapped_base_.owner) return false;
    materialize_mapped_base_unlocked();
    return true;
}

const NtfsCatalog::BaseNode* NtfsCatalog::find_base_unlocked(
    std::uint64_t id) const {
    const auto nodes = base_nodes_unlocked();
    const auto found = std::lower_bound(
        nodes.begin(), nodes.end(), id,
        [](const BaseNode& node, std::uint64_t value) {
            return node.id < value;
        });
    return found != nodes.end() && found->id == id ? &*found : nullptr;
}

std::wstring_view NtfsCatalog::base_name_unlocked(
    const BaseNode& node) const {
    if (node.name_length == 0) return {};
    const auto names = base_names_unlocked();
    return {names.data() + node.name_offset, node.name_length};
}

NtfsCatalog::NodeView NtfsCatalog::view_unlocked(
    const BaseNode& node) const {
    return {node.id,
            node.parent_id,
            node.size,
            node.last_write_time,
            node.attributes,
            node.directory != 0,
            base_name_unlocked(node)};
}

NtfsCatalog::NodeView NtfsCatalog::view_unlocked(const Node& node) {
    return {node.id,
            node.parent_id,
            node.size,
            node.last_write_time,
            node.attributes,
            node.directory,
            node.name};
}

bool NtfsCatalog::try_get_unlocked(
    std::uint64_t id, NodeView& result) const {
    if (const auto overlay = overlay_.find(id); overlay != overlay_.end()) {
        result = view_unlocked(overlay->second);
        return true;
    }
    if (tombstones_.find(id) != tombstones_.end()) return false;
    const auto* base = find_base_unlocked(id);
    if (!base) return false;
    result = view_unlocked(*base);
    return true;
}

NtfsCatalog::Node NtfsCatalog::materialize_unlocked(
    const BaseNode& node) const {
    Node result;
    result.id = node.id;
    result.parent_id = node.parent_id;
    result.size = node.size;
    result.last_write_time = node.last_write_time;
    result.attributes = node.attributes;
    result.directory = node.directory;
    result.name.assign(base_name_unlocked(node));
    return result;
}

NtfsCatalog::Node& NtfsCatalog::promote_unlocked(std::uint64_t id) {
    if (auto found = overlay_.find(id); found != overlay_.end()) {
        return found->second;
    }
    const auto* base = find_base_unlocked(id);
    if (!base || tombstones_.find(id) != tombstones_.end()) {
        throw std::logic_error("cannot promote missing catalog node");
    }
    auto [inserted, created] = overlay_.emplace(id, materialize_unlocked(*base));
    (void)created;
    return inserted->second;
}

CatalogApplyResult NtfsCatalog::apply(const UsnChangeBatch& batch) {
    CatalogApplyResult result;
    std::unique_lock lock(mutex_);
    std::unordered_set<std::uint64_t> pending_renames;
    std::unordered_set<std::uint64_t> changed_ids;
    std::unordered_set<std::uint64_t> path_roots;
    std::unordered_set<std::uint64_t> removed_ids;
    pending_renames.reserve(batch.changes.size() / 8 + 1);
    changed_ids.reserve(batch.changes.size() / 2 + 1);

    for (const auto& change : batch.changes) {
        if ((change.reason & USN_REASON_RENAME_OLD_NAME) != 0) {
            pending_renames.insert(change.file_id);
            continue;
        }

        if ((change.reason & USN_REASON_FILE_DELETE) != 0) {
            NodeView deleting;
            if (try_get_unlocked(change.file_id, deleting)) {
                if (deleting.directory) path_roots.insert(change.file_id);
                const bool in_base = find_base_unlocked(change.file_id) != nullptr;
                overlay_.erase(change.file_id);
                if (in_base) tombstones_.insert(change.file_id);
                else tombstones_.erase(change.file_id);
                removed_ids.insert(change.file_id);
                --live_size_;
                ++result.deleted;
            } else {
                ++result.ignored;
            }
            pending_renames.erase(change.file_id);
            changed_ids.erase(change.file_id);
            continue;
        }

        const bool create = (change.reason & USN_REASON_FILE_CREATE) != 0;
        const bool rename_new =
            (change.reason & USN_REASON_RENAME_NEW_NAME) != 0;
        NodeView existing;
        const bool found = try_get_unlocked(change.file_id, existing);

        if (!found) {
            if (!create && !rename_new) {
                ++result.ignored;
                continue;
            }
            Node node;
            node.id = change.file_id;
            node.parent_id = change.parent_id;
            node.attributes = change.attributes;
            node.directory =
                (change.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            node.name = change.name;
            tombstones_.erase(change.file_id);
            overlay_.insert_or_assign(node.id, std::move(node));
            changed_ids.insert(change.file_id);
            removed_ids.erase(change.file_id);
            ++live_size_;
            ++result.created;
            if (rename_new || pending_renames.erase(change.file_id) != 0) {
                ++result.renamed;
            }
            continue;
        }

        auto& node = promote_unlocked(change.file_id);
        if (rename_new) {
            node.parent_id = change.parent_id;
            node.name = change.name;
            if (node.directory) path_roots.insert(node.id);
            ++result.renamed;
            pending_renames.erase(change.file_id);
        } else if (create) {
            // A create can be followed by CLOSE or other reasons for the same
            // file in one batch. insert-or-update semantics keep replay
            // idempotent by file reference ID.
            node.parent_id = change.parent_id;
            node.name = change.name;
            ++result.updated;
        } else {
            ++result.updated;
        }
        node.attributes = change.attributes;
        node.directory =
            (change.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        changed_ids.insert(change.file_id);
        removed_ids.erase(change.file_id);
    }
    result.ignored += pending_renames.size();

    std::unordered_map<std::uint64_t, std::wstring> paths;
    paths.reserve(changed_ids.size() * 2 + path_roots.size() + 8);
    paths.emplace(root_id_, volume_root_);
    std::unordered_set<std::uint64_t> resolving;
    resolving.reserve(64);
    std::function<std::wstring(std::uint64_t, unsigned)> resolve =
        [&](std::uint64_t id, unsigned depth) -> std::wstring {
        if (id == root_id_) return volume_root_;
        if (const auto cached = paths.find(id); cached != paths.end()) {
            return cached->second;
        }
        if (depth > 512 || !resolving.insert(id).second) {
            return volume_root_ + L"\\$Cycle";
        }
        NodeView node;
        if (!try_get_unlocked(id, node)) {
            resolving.erase(id);
            return volume_root_ + L"\\$OrphanFiles";
        }
        auto path = resolve(node.parent_id, depth + 1);
        path.push_back(L'\\');
        path.append(node.name.data(), node.name.size());
        resolving.erase(id);
        paths.emplace(id, path);
        return path;
    };
    std::unordered_map<std::uint64_t, bool> path_affected_cache;
    path_affected_cache.reserve(path_roots.size() * 4 + 64);
    path_affected_cache.emplace(root_id_, false);
    for (const auto id : path_roots) path_affected_cache.insert_or_assign(id, true);
    std::unordered_set<std::uint64_t> resolving_affected;
    resolving_affected.reserve(64);
    std::function<bool(std::uint64_t, unsigned)> is_path_affected =
        [&](std::uint64_t id, unsigned depth) -> bool {
        if (const auto cached = path_affected_cache.find(id);
            cached != path_affected_cache.end()) {
            return cached->second;
        }
        if (depth > 512 || !resolving_affected.insert(id).second) {
            return false;
        }
        NodeView node;
        bool affected = false;
        if (try_get_unlocked(id, node) && node.parent_id != id) {
            affected = is_path_affected(node.parent_id, depth + 1);
        }
        resolving_affected.erase(id);
        path_affected_cache.emplace(id, affected);
        return affected;
    };
    const auto affected_by_path_root = [&](const NodeView& node) {
        return is_path_affected(node.parent_id, 0);
    };
    result.upserts.reserve(changed_ids.size());
    const auto append_upsert = [&](const NodeView& node) {
        FileRecord record;
        record.id = node.id;
        record.parent_id = node.parent_id;
        record.size = node.size;
        record.last_write_time = node.last_write_time;
        record.attributes = node.attributes;
        record.directory = node.directory;
        record.name.assign(node.name);
        record.path = resolve(node.parent_id, 0);
        record.path.push_back(L'\\');
        record.path.append(node.name.data(), node.name.size());
        result.upserts.push_back(std::move(record));
    };

    if (path_roots.empty()) {
        // Ordinary file activity scales with changed IDs only. Directory
        // renames take the full merged-base scan because descendant paths may
        // genuinely have changed.
        for (const auto id : changed_ids) {
            NodeView node;
            if (try_get_unlocked(id, node)) append_upsert(node);
        }
    } else {
        for (const auto& base : base_nodes_unlocked()) {
            if (tombstones_.find(base.id) != tombstones_.end() ||
                overlay_.find(base.id) != overlay_.end()) {
                continue;
            }
            const auto node = view_unlocked(base);
            if (changed_ids.find(node.id) != changed_ids.end() ||
                affected_by_path_root(node)) {
                append_upsert(node);
            }
        }
        for (const auto& [id, source] : overlay_) {
            const auto node = view_unlocked(source);
            if (changed_ids.find(id) != changed_ids.end() ||
                affected_by_path_root(node)) {
                append_upsert(node);
            }
        }
    }
    result.removed_ids.assign(removed_ids.begin(), removed_ids.end());
    if (should_compact_unlocked()) compact_unlocked();
    return result;
}

std::vector<FileRecord> NtfsCatalog::snapshot() const {
    std::shared_lock lock(mutex_);
    std::unordered_map<std::uint64_t, std::wstring> paths;
    paths.reserve(base_nodes_unlocked().size() / 8 + overlay_.size() + 1);
    paths.emplace(root_id_, volume_root_);
    std::unordered_set<std::uint64_t> resolving;
    resolving.reserve(64);

    std::function<std::wstring(std::uint64_t, unsigned)> resolve =
        [&](std::uint64_t id, unsigned depth) -> std::wstring {
        if (id == root_id_) return volume_root_;
        if (const auto cached = paths.find(id); cached != paths.end()) {
            return cached->second;
        }
        if (depth > 512 || !resolving.insert(id).second) {
            return volume_root_ + L"\\$Cycle";
        }
        NodeView node;
        if (!try_get_unlocked(id, node)) {
            resolving.erase(id);
            return volume_root_ + L"\\$OrphanFiles";
        }
        auto path = resolve(node.parent_id, depth + 1);
        path.push_back(L'\\');
        path.append(node.name.data(), node.name.size());
        resolving.erase(id);
        paths.emplace(id, path);
        return path;
    };

    std::vector<FileRecord> result;
    result.reserve(live_size_);
    const auto append = [&](const NodeView& node) {
        FileRecord record;
        record.id = node.id;
        record.parent_id = node.parent_id;
        record.size = node.size;
        record.last_write_time = node.last_write_time;
        record.attributes = node.attributes;
        record.directory = node.directory;
        record.name.assign(node.name);
        record.path = resolve(node.parent_id, 0);
        record.path.push_back(L'\\');
        record.path.append(node.name.data(), node.name.size());
        result.push_back(std::move(record));
    };
    for (const auto& base : base_nodes_unlocked()) {
        if (tombstones_.find(base.id) != tombstones_.end() ||
            overlay_.find(base.id) != overlay_.end()) {
            continue;
        }
        append(view_unlocked(base));
    }
    for (const auto& [id, node] : overlay_) {
        (void)id;
        append(view_unlocked(node));
    }
    return result;
}

std::vector<FileRecord> NtfsCatalog::storage_snapshot() const {
    std::shared_lock lock(mutex_);
    std::vector<FileRecord> result;
    result.reserve(live_size_);
    const auto append = [&](const NodeView& node) {
        FileRecord record;
        record.id = node.id;
        record.parent_id = node.parent_id;
        record.size = node.size;
        record.last_write_time = node.last_write_time;
        record.attributes = node.attributes;
        record.directory = node.directory;
        record.name.assign(node.name);
        result.push_back(std::move(record));
    };
    for (const auto& base : base_nodes_unlocked()) {
        if (tombstones_.find(base.id) != tombstones_.end() ||
            overlay_.find(base.id) != overlay_.end()) {
            continue;
        }
        append(view_unlocked(base));
    }
    for (const auto& [id, node] : overlay_) {
        (void)id;
        append(view_unlocked(node));
    }
    return result;
}

CatalogStorageReadView NtfsCatalog::storage_view() const {
    CatalogStorageReadView result;
    result.lock_ = std::shared_lock<std::shared_mutex>(mutex_);
    if (!overlay_.empty() || !tombstones_.empty()) return result;
    result.nodes_ = base_nodes_unlocked();
    result.names_ = base_names_unlocked();
    result.complete_ = result.nodes_.size() == live_size_;
    return result;
}

bool NtfsCatalog::should_compact_unlocked() const {
    return auto_compaction_threshold_ != 0 &&
           overlay_.size() + tombstones_.size() >=
               auto_compaction_threshold_;
}

void NtfsCatalog::compact_unlocked() {
    if (overlay_.empty() && tombstones_.empty()) return;

    std::size_t total_name_chars = 0;
    for (const auto& base : base_nodes_unlocked()) {
        if (tombstones_.find(base.id) == tombstones_.end() &&
            overlay_.find(base.id) == overlay_.end()) {
            checked_add_name_chars(total_name_chars, base.name_length);
        }
    }
    std::vector<const Node*> sorted_overlay;
    sorted_overlay.reserve(overlay_.size());
    for (const auto& [id, node] : overlay_) {
        (void)id;
        checked_add_name_chars(total_name_chars, node.name.size());
        sorted_overlay.push_back(&node);
    }
    std::sort(sorted_overlay.begin(), sorted_overlay.end(),
              [](const Node* left, const Node* right) {
        return left->id < right->id;
    });

    std::vector<BaseNode> compacted;
    std::vector<wchar_t> names;
    compacted.reserve(live_size_);
    names.reserve(total_name_chars);
    const auto append_base = [&](const BaseNode& source) {
        BaseNode node = source;
        node.name_offset = static_cast<std::uint32_t>(names.size());
        const auto name = base_name_unlocked(source);
        names.insert(names.end(), name.begin(), name.end());
        compacted.push_back(node);
    };
    const auto append_overlay = [&](const Node& source) {
        BaseNode node;
        node.id = source.id;
        node.parent_id = source.parent_id;
        node.size = source.size;
        node.last_write_time = source.last_write_time;
        node.attributes = source.attributes;
        node.directory = source.directory;
        node.name_offset = static_cast<std::uint32_t>(names.size());
        node.name_length = static_cast<std::uint32_t>(source.name.size());
        names.insert(names.end(), source.name.begin(), source.name.end());
        compacted.push_back(node);
    };

    std::size_t overlay_index = 0;
    for (const auto& base : base_nodes_unlocked()) {
        while (overlay_index < sorted_overlay.size() &&
               sorted_overlay[overlay_index]->id < base.id) {
            append_overlay(*sorted_overlay[overlay_index++]);
        }
        if (overlay_index < sorted_overlay.size() &&
            sorted_overlay[overlay_index]->id == base.id) {
            append_overlay(*sorted_overlay[overlay_index++]);
            continue;
        }
        if (tombstones_.find(base.id) == tombstones_.end()) {
            append_base(base);
        }
    }
    while (overlay_index < sorted_overlay.size()) {
        append_overlay(*sorted_overlay[overlay_index++]);
    }

    base_nodes_ = std::move(compacted);
    base_names_ = std::move(names);
    mapped_base_ = {};
    overlay_.clear();
    overlay_.rehash(0);
    tombstones_.clear();
    tombstones_.rehash(0);
    live_size_ = base_nodes_.size();
    ++compaction_count_;
}

bool NtfsCatalog::compact() {
    std::unique_lock lock(mutex_);
    if (overlay_.empty() && tombstones_.empty()) return false;
    compact_unlocked();
    return true;
}

void NtfsCatalog::set_auto_compaction_threshold(std::size_t threshold) {
    std::unique_lock lock(mutex_);
    auto_compaction_threshold_ = threshold;
    if (should_compact_unlocked()) compact_unlocked();
}

std::size_t NtfsCatalog::size() const {
    std::shared_lock lock(mutex_);
    return live_size_;
}

std::size_t NtfsCatalog::pending_delta_size() const {
    std::shared_lock lock(mutex_);
    return overlay_.size() + tombstones_.size();
}

CatalogStorageStats NtfsCatalog::storage_stats() const {
    std::shared_lock lock(mutex_);
    CatalogStorageStats result;
    result.live_nodes = live_size_;
    const auto nodes = base_nodes_unlocked();
    const auto names = base_names_unlocked();
    result.base_nodes = nodes.size();
    result.base_name_chars = names.size();
    result.overlay_nodes = overlay_.size();
    result.tombstones = tombstones_.size();
    result.compact_storage_bytes = mapped_base_.owner
        ? nodes.size_bytes() + names.size_bytes()
        : base_nodes_.capacity() * sizeof(BaseNode) +
              base_names_.capacity() * sizeof(wchar_t);
    result.compaction_count = compaction_count_;
    result.mapped_base = static_cast<bool>(mapped_base_.owner);
    result.mapped_file_bytes = mapped_base_.mapped_bytes;
    return result;
}
} // namespace esm

