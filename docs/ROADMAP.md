# Product parity roadmap

Baseline target: Everything 1.5.0.1418b Beta as observed on 2026-07-23. The target is versioned so upstream changes do not silently move acceptance criteria.

## Definition of parity

A feature is complete only when behavior has an executable compatibility test, edge cases are covered, performance is measured, and restart/crash recovery is tested where persistent state is involved.

## M1 - Searchable metadata index

- [x] Recursive fallback scanner
- [x] In-memory filename/path index
- [x] Quoted, scoped, excluded, wildcard, file/folder queries
- [x] Unit tests and synthetic benchmark
- [x] Compact string arena and stable record IDs
- [x] Persistent checksummed metadata snapshot with embedded exact USN cursor
- [x] Fixed-layout v2 snapshot with direct read-only node/name mapping and v1 migration
- [x] Fast snapshot restart followed by Journal-tail catch-up
- [ ] Natural sort and all metadata columns
- [x] Checksummed append-only WAL with streaming replay and torn-tail truncation
- [x] Crash-safe snapshot/WAL checkpoint consolidation and 64 MiB automatic threshold
- [ ] Generalized incremental metadata database / delta snapshot format

## M2 - NTFS-speed indexing

- [x] Enumerate MFT with `FSCTL_ENUM_USN_DATA` (requires elevated service for live-volume validation)
- [x] Reconstruct paths from file and parent reference numbers
- [x] Detect path cycles and place unresolved records in an orphan bucket
- [ ] Represent every hard-link directory entry independently
- [ ] Define complete NTFS reparse-point, junction, symlink and mount-point policy
- [x] Fallback scanner indexes reparse entries without traversing them
- [x] Read and atomically checkpoint the USN Journal
- [x] Apply create/delete/rename/update events transactionally
- [x] Compact incremental index overlays automatically
- [x] Reject corrupt, expired, wrong-volume, or wrong-Journal snapshots and rebuild from MFT
- [x] Replace the high-memory mutable catalog with a compact base + overlay
- [x] Memory-map the immutable catalog base snapshot
- [x] Prototype continuous journal catch-up in the in-process `live` CLI
- [x] Move race-free bootstrap and continuous Journal following into `esm_server live`
- [ ] Add periodic full reconciliation
- [x] Local-only bounded Named Pipe query protocol and CLI/server prototype
- [x] Protected Pipe DACL for elevated-server/non-elevated-client queries
- [x] Concurrent Pipe worker pool and clean host shutdown primitive
- [x] Windows SCM Service install/start/status/stop/uninstall lifecycle
- [x] LocalSystem service host with start-pending checkpoints and clean stop handling
- [ ] Per-user client authorization
- [x] Windows `ReadDirectoryChangesW` fallback watcher with debounced full reconciliation
- [ ] Provider-specific incremental FAT/exFAT and network-share semantics
- [ ] Removable/offline volume lifecycle, cloud providers and multi-volume discovery
- [ ] Linux/macOS providers

## M3 - Everything-compatible query engine

- [ ] Boolean operators and precedence
- [ ] Complete wildcard and regex behavior
- [ ] Case, whole-word, diacritic and path toggles
- [ ] Size/date/attribute/property functions
- [ ] Macros, bookmarks and filters
- [x] Persisted desktop query-history storage
- [ ] Search-history UI and query refinement cache
- [ ] Duplicate-file functions
- [ ] Natural sorting and fast incremental query refinement

## M4 - Desktop product

- [x] Native Windows search window with asynchronous live results
- [x] Classic compact menu/search/results/status layout with search-history dropdown and keyboard navigation
- [x] Global `Ctrl+Alt+Space` hotkey and system tray
- [x] Fixed metadata columns, Shell file icons and server-side column sorting
- [x] Basic text preview and custom context menu
- [x] Copy path/file, rename, recycle delete, double-click open and open-location
- [x] Column visibility/order/width, sort/preview/window settings persistence and multi-file Shell clipboard/drag data objects
- [ ] Shell-native context menu and Windows preview-handler integration
- [ ] Installer, auto-start, update and diagnostics

## M5 - Servers and integrations

- [x] CLI client
- [x] Versioned binary search IPC prototype
- [x] Native GUI client over the same IPC
- [x] Everything-style seven-menu GUI, persistent view/search switches, category filter selector, bookmarks, result export, and original multi-size Windows icon
- [ ] Stable public SDK / IPC compatibility contract
- [ ] HTTP server
- [ ] ETP-compatible server/client behavior
- [ ] Multi-machine result federation

## M6 - Content and property indexing

- [ ] Crash-isolated extraction workers
- [ ] Plain text/source code and Windows IFilter
- [ ] PDF and OOXML extractors
- [ ] Xapian content index
- [ ] Chinese tokenization and character n-grams
- [ ] Snippets, highlighting and OCR extension point

## Clean-room policy

Reproduce behavior, not proprietary code, branding, icons or binaries. Public documentation, observable black-box behavior, documented Windows APIs, and officially published SDK/protocol material are acceptable inputs.


