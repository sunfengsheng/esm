#include "esm/journal_replay.hpp"
#include "esm/metadata_wal.hpp"

namespace esm {
JournalReplayResult replay_journal_batch(
    NtfsCatalog& catalog,
    MetadataIndex& index,
    const UsnJournalState& journal,
    const UsnChangeBatch& batch,
    const std::filesystem::path& checkpoint_path,
    const std::filesystem::path& wal_path,
    std::int64_t wal_start_usn) {
    JournalReplayResult result;
    result.next_usn = batch.next_usn;
    if (batch.error != 0) {
        result.error = batch.error;
        return result;
    }

    auto delta = catalog.apply(batch);
    result.created = delta.created;
    result.updated = delta.updated;
    result.renamed = delta.renamed;
    result.deleted = delta.deleted;
    result.ignored = delta.ignored;
    result.index_upserts = delta.upserts.size();
    result.index_removals = delta.removed_ids.size();

    index.apply_delta(std::move(delta.upserts), delta.removed_ids);

    if (!wal_path.empty()) {
        const auto appended = append_metadata_wal(
            wal_path, journal.journal_id, wal_start_usn, batch);
        if (!appended.ok) {
            result.error = appended.error;
            return result;
        }
    }

    const auto saved = save_checkpoint_atomic(
        checkpoint_path, {journal.journal_id, batch.next_usn});
    if (!saved.ok) {
        result.error = saved.error;
        return result;
    }
    result.checkpoint_saved = true;
    return result;
}
} // namespace esm
