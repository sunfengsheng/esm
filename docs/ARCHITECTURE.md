# Architecture

```text
Desktop / CLI / SDK <--named pipe--> Index service
                                      |-- Query engine
                                      |-- Metadata index
                                      |-- NTFS MFT + USN provider
                                      |-- Fallback scanner/watcher
                                      `-- Extraction queue --> Xapian content DB
```

## Planned process boundaries

- `esm_service`: privileged discovery, persistent metadata, journal ingestion and querying.
- `esm_extract_worker`: low-privilege resource-limited content extraction.
- `esm_gui`: unprivileged native Win32 desktop application.
- `esm_cli`: administration, diagnostics and querying.

The current phase has four executable surfaces. `esm_cli live` keeps discovery and queries in one process for diagnostics, `esm_server` is the foreground development host, `esm_service` is an installed LocalSystem SCM service, and `esm_gui` is an unprivileged native desktop client. The query hosts expose the same local Named Pipe protocol to both `esm_cli query` and `esm_gui`.

## Identity

Path is mutable and cannot be the primary key. NTFS records use volume identity plus file reference number and sequence. Hard links require one physical identity to map to multiple directory entries. Other providers use a stable provider identity when available and otherwise a persisted synthetic identity.

## Windows fallback scanner/watcher

For roots that are not using the privileged NTFS MFT/USN path, `esm_server scan` combines an authoritative recursive scanner with a recursive overlapped `ReadDirectoryChangesW` watcher. The watcher uses a 4-64 KiB notification buffer, a separate completion event, stop-token cancellation through `CancelIoEx`, strict `FILE_NOTIFY_INFORMATION` bounds validation, and explicit overflow reporting. A dedicated watcher thread remains responsive while a separate refresh thread waits for a 150 ms quiet period and then rebuilds the fallback index from a full scan. If events arrive during that scan, the generation counter immediately schedules another reconciliation, avoiding a watcher gap caused by doing the scan on the notification thread.

Fallback records derive fixed FNV-1a synthetic IDs from normalized invariant-lowercase absolute paths and apply the same operation to parent paths. Metadata comes from `GetFileAttributesExW`, including Windows attributes, 64-bit size, and native `FILETIME`. Directory reparse points are represented but recursion is disabled at those entries. These identities are stable across repeated scans while a path is unchanged, but are not physical file identities: a rename creates a new synthetic ID. Notification records are treated only as invalidation hints; they are not committed directly as authoritative state.

This first provider stage does not claim complete FAT/exFAT semantics, network-share reconnect/recovery, cloud placeholders, offline/removable-volume lifecycle, provider-specific incremental deltas, automatic multi-volume discovery, or Linux/macOS backends.

## Index split

- Metadata index: names, paths, timestamps, sizes, attributes and properties.
- Content index: extracted terms, language analysis and snippets.

Search plans query either index and merge by stable document identity.

## NTFS catalog representation

`NtfsCatalog` separates stable catalog state from recent Journal mutations:

```text
owned ID-sorted vector<BaseNode> or mapped v2 node table
                         + contiguous wchar_t name arena
                         + unordered_map mutation overlay
                         + base-ID tombstones
```

The base is immutable between compactions and contains fixed-width metadata plus name offset/length pairs; it does not allocate a `std::wstring` per stable node. The same span-based catalog code reads either owned vectors or node/name ranges backed by a read-only v2 file mapping. Base lookup is a binary search by file-reference ID. The overlay is checked first, a tombstone hides a deleted base node, and only then is the base consulted. Updating a base node promotes it into the overlay, while newly created nodes enter the overlay directly.

Compaction merges the logical overlay/base view into new owned sorted vectors, rebuilds the contiguous name arena, and releases an attached file mapping. It can be requested explicitly and runs automatically at 100,000 overlay entries plus tombstones by default. A mapped base with no deltas can instead be materialized without a logical compaction when an atomic snapshot replacement needs the old file mapping closed. Catalog snapshots iterate the same merged logical view. Persistence records contain node metadata and names but omit complete paths.

Paths are reconstructed on demand by following parent IDs with orphan and cycle protection. A directory rename identifies affected descendants and uses a parent-chain result cache so siblings sharing the same ancestry do not repeatedly traverse that chain. Stable catalog storage is compact and can remain memory-mapped after restart. Recent Journal batches are also persisted in an append-only WAL, while periodic checkpoint consolidation still rewrites a complete compact base image.

## Reliability invariants

1. Journal cursors advance only after the mutable catalog and searchable index accept the associated batch.
2. A persistent metadata snapshot contains the catalog state and the exact Journal cursor that belongs to that state.
3. Rename is an atomic old-name removal plus new-name insertion.
4. Query snapshots never observe half-applied batches.
5. Full MFT reconciliation can rebuild derived state from authoritative volume state.
6. Parsers run with explicit byte, count, string, time and memory limits.

## USN checkpoint invariants

`FSCTL_READ_USN_JOURNAL` must start from a checkpoint emitted by Windows: `FirstUsn`, `NextUsn`, or the leading `USN` returned by an earlier read. A numeric value manufactured inside the apparent `[FirstUsn, NextUsn]` range is not guaranteed to identify a record boundary and may be rejected with `ERROR_INVALID_PARAMETER`.

The race-free bootstrap sequence is therefore:

1. Query the journal and save its exact `NextUsn`.
2. Enumerate the MFT into the initial metadata index.
3. Replay journal records beginning at the saved `NextUsn`.
4. Persist each exact next-USN returned by Windows after the batch is applied atomically.


## Live-index session

`LiveIndexSession` owns the mutable NTFS catalog, compact metadata index, Journal identity and exact cursor. `esm_server live` and `esm_service` use the same recovery order:

1. Query the current USN Journal.
2. Attempt to load `<checkpoint>.metadata` and verify its magic/version, checksum, volume identity, NTFS root ID, Journal ID and embedded exact `NextUsn`. Version 2 is opened as a read-only mapping; version 1 uses the compatibility loader.
3. If valid, attach the v2 node table/name arena directly to `NtfsCatalog` (or rebuild owned catalog vectors for v1), reconstruct the searchable index, stream complete WAL transactions beginning at the embedded cursor, and then replay the live Journal to a fresh boundary. A successfully loaded v1 image is immediately rewritten as v2.
4. If missing, corrupt, truncated, expired, from another volume, or from another Journal, capture an exact pre-MFT `NextUsn`, enumerate the MFT, build both in-memory structures, and replay from that captured boundary.
5. Serve queries only after catch-up completes; continue polling in a background follower while queries use shared-lock snapshots.
6. Before replacement, compact pending overlay/tombstone state or materialize an unchanged mapped base so the old mapping is released. Stream the fixed node table and UTF-16 name arena directly to `.tmp`, flush it, and publish with `MoveFileExW(..., MOVEFILE_WRITE_THROUGH)`. Only after the snapshot and checkpoint are durable is the WAL reset. Polling automatically consolidates at 64 MiB; the SCM service also refreshes after five minutes or 100,000 changes and performs a final save during clean shutdown.

The snapshot deliberately stores file-reference IDs, parent IDs and metadata rather than a full path string for every record. Version 2 uses an 80-byte header, a fixed 48-byte `CatalogBaseNode` table, a contiguous UTF-16 name arena, the UTF-16 volume name, and a trailing whole-file checksum. Nodes are strictly ID-sorted and reference names by 32-bit offset/length. Version 1 remains load-compatible and migrates automatically. Snapshot creation streams the compact arenas directly and no longer materializes a full temporary `vector<FileRecord>`. The base snapshot is still a complete state image; future work includes delta snapshots, shared catalog/search strings, faster parallel checksums and periodic reconciliation.


## WAL and checkpoint consolidation

`<checkpoint>.wal` is a sequence of independently checksummed transactions. Each transaction records Journal identity, exact start/next USNs, change count and a bounded payload (64 MiB maximum). Append uses write-through file handles and flushes before the external checkpoint advances. Recovery reads a 52-byte header and one transaction payload at a time, validates continuity and checksum, applies only transactions newer than the loaded snapshot cursor, and updates the searchable overlay.

A short final header or payload is a torn append: recovery keeps all complete transactions and truncates the physical file to `valid_bytes`. A fully present transaction with a bad checksum is corruption and returns `ERROR_CRC`; it is not silently discarded. Consolidation publishes a new atomic snapshot first, saves its exact checkpoint second, and resets the WAL last. Therefore a crash in the publication-to-reset window is safe: restart loads the newer snapshot and skips the still-present older WAL transactions by cursor. Tests cover torn-tail truncation, repeat replay, complete-transaction CRC failure and this checkpoint crash window.

## Named Pipe IPC

`esm_server scan <root> [pipe-name]`, `esm_server mft <volume> [pipe-name]`, `esm_server live ...`, and the installed `esm_service` serve search requests. `esm_cli query <pipe-name> <query>` and `esm_gui [pipe-name]` are current clients.

Protocol properties:

- fixed frame header with magic, protocol version, message type, request ID and payload size;
- strict UTF-8 strings on the wire;
- 4 MiB maximum payload and 1,000 maximum requested results;
- exact byte-stream reads/writes with bounded client connection timeout;
- `PIPE_REJECT_REMOTE_CLIENTS`, so the pipe cannot accept remote clients;
- a protected DACL grants SYSTEM/administrators full control and authenticated local users read/write access;
- four worker instances accept independent clients concurrently;
- a shared stop flag plus wake connections unblock `ConnectNamedPipe` and join every worker during shutdown;
- request flags include path/case/whole-word behavior plus a versioned sort field and direction;
- malformed frames, flags, limits and UTF-8 are rejected before search execution.

This is now a bounded local transport integrated with the SCM lifecycle and persistent snapshot recovery, but not the final authorization model. The service still needs per-user/client policy and per-request cancellation.



## Native desktop client

`esm_gui` is a Win32 unprivileged Pipe client. A compact history-enabled search combo drives debounced asynchronous searches; generation IDs discard stale replies. A classic native menu bar and keyboard navigation expose the same file operations without adding a custom toolkit. Results use an owner-data `ListView`, Shell system-image-list icons and server-side column sorting. The current desktop layer also includes an original navy/teal multi-resolution Windows icon, tray/hotkey activation, status timing, persisted history storage, persisted column visibility/order/width, sort/preview/window state, persisted case-sensitive/whole-word/path matching switches, basic text preview, custom file operations/context menu, and multi-file Shell data objects shared by clipboard and OLE drag operations. It intentionally does not impersonate proprietary Everything UI resources. Shell-native context menus and Windows preview handlers, accessibility/DPI/dark-mode refinement, and GUI automation tests remain product work.

## Checkpoint placement invariant

The exact checkpoint and its sibling metadata snapshot (`<checkpoint>.metadata`) must not be stored on the NTFS volume being indexed. Updating either file on that volume produces another USN record, which can trigger another persistence write and form a self-sustaining loop. `esm_cli live`, `esm_server live`, and `esm_service` reject same-volume checkpoint paths; the snapshot inherits the same external placement.

Keeping persistence outside the indexed volume also lets one atomic snapshot represent a catalog state plus its embedded Journal cursor without observing its own storage writes. A future transactional store may relax the prototype restriction only if internal writes are isolated from ingestion or handled as an explicit no-write replay case.


