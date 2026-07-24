# everything_sm

[![Windows build](https://github.com/sunfengsheng/esm/actions/workflows/windows-build.yml/badge.svg)](https://github.com/sunfengsheng/esm/actions/workflows/windows-build.yml)

A clean-room Windows file search engine targeting functional and performance parity with Everything.

> Status: multi-volume NTFS discovery, persistent MFT snapshots, NTFS indexing, continuous USN following, checksummed append-only WAL recovery, memory-mapped v2 compact snapshots, local IPC, a Windows SCM service, a first native desktop GUI, a Windows fallback realtime watcher, and an NSIS installer with a native fallback launcher are operational. The project can recursively scan, reconcile watched directory trees after `ReadDirectoryChangesW` notifications, enumerate a live NTFS MFT, reconstruct paths, replay create/delete/rename updates, recover complete WAL transactions after a crash, truncate torn WAL tails, stream compact catalog snapshots without a temporary `vector<FileRecord>`, and serve bounded searches to CLI and GUI clients. Full Everything query/NTFS parity, a generalized delta database, provider-specific FAT/exFAT/network semantics, per-user authorization, code signing/automatic upgrades, and Xapian content indexing remain.

## Principles

- Clean-room implementation using documented Windows APIs.
- Correctness and measurable performance before UI polish.
- Every feature has a compatibility test; every hot path has a benchmark.
- Filename metadata and document-content indexes remain separate subsystems.

## Build

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

## Install on Windows

Builds are packaged as a 64-bit NSIS installer:

```text
dist\everything_sm-0.1.0-setup.exe
```

1. Run the installer and approve the Windows UAC prompt.
2. Keep **Install multi-volume NTFS indexing service** enabled. The SCM service automatically discovers drive-letter NTFS volumes such as `C:`, `D:`, and `E:` and exposes one combined search index.
3. The service stores its checksummed atomic base snapshot at `C:\ProgramData\everything_sm\indexes\mft-index.snapshot`. On restart it loads that local snapshot first, then immediately reconciles all mounted NTFS volumes in the background.
4. Keep the compatibility scan directory configured as a fallback. If the service is unavailable, the launcher starts a hidden per-session recursive scanner and watcher for that directory.
5. Start `everything_sm` from the desktop or Start menu shortcut. Type a filename, path fragment, extension filter, size/date predicate, or other supported query. Double-click a result to open it; `Ctrl+Alt+Space` shows the window globally.

The launcher always uses Pipe `everything_sm_service`. The multi-volume service reconciles the MFT every minute and refreshes its snapshot every five minutes. Uninstall from **Windows Settings > Apps** or from the Start menu; the uninstaller can also remove the machine-level snapshot and current-user GUI settings.

To rebuild the installer from source:

```powershell
.\packaging\build-installer.ps1
```

The script performs an isolated Release build, bootstraps the pinned NSIS 3.12 toolchain when needed, treats NSIS warnings as errors, and writes the setup executable plus its SHA-256 hash to `dist`.
## Run

```powershell
.\build\esm_cli.exe scan D:\ "readme ext:md"
.\build\esm_benchmark.exe 1000000

# Terminal 1: build an index and serve local queries
.\build\esm_server.exe scan D:\work\everything_sm everything_sm

# Terminal 2: query the server without elevation
.\build\esm_cli.exe query everything_sm "readme ext:md"

# Or launch the native desktop client against that Pipe
.\build\esm_gui.exe everything_sm

# Elevated Terminal 1: live MFT + USN service for D:
$checkpoint = "$env:LOCALAPPDATA\everything_sm\D-live.checkpoint"
.\build\esm_server.exe live D: $checkpoint everything_sm_live

# Non-elevated Terminal 2: query the live elevated server
.\build\esm_cli.exe query everything_sm_live "readme"

# Elevated shell: install the default persistent multi-volume MFT service
.\build\esm_service.exe install-mft-auto "C:\ProgramData\everything_sm\indexes" everything_sm_service
.\build\esm_service.exe start

# Optional compatibility command: install a fast single-volume MFT service
.\build\esm_service.exe install-mft C: everything_sm_service

# Advanced live MFT + USN service still requires its checkpoint on another volume
$checkpoint = "C:\ProgramData\everything_sm\D-service.checkpoint"
.\build\esm_service.exe install D: $checkpoint everything_sm_service

# A normal shell can query status and search the protected local Pipe
.\build\esm_service.exe status
.\build\esm_cli.exe query everything_sm_service "readme"

# Elevated shell: stop and remove the service
.\build\esm_service.exe stop
.\build\esm_service.exe uninstall
```

The default `install-mft-auto` service discovers all mounted fixed NTFS drive-letter volumes, namespaces each MFT file reference with the stable volume identity to prevent cross-volume ID collisions, and combines the records in one `MetadataIndex`. A failed or incomplete volume reconciliation does not replace the previous complete index. The base snapshot is checksummed and replaced atomically; it is loaded before the first background reconciliation so queries can become available without waiting for a fresh full-volume enumeration. Removable, FAT/exFAT, network, and cloud providers remain separate future work.

`esm_server scan` now keeps a non-elevated recursive `ReadDirectoryChangesW` watch active unless `--once` is supplied. Any notification batch or overflow marks the root dirty; a 150 ms quiet-period debounce then performs an authoritative full recursive rescan and atomically replaces the searchable index. Fallback records use a fixed FNV-1a synthetic identity over a normalized case-folded absolute path, populate synthetic parent IDs plus Win32 size/attribute/`FILETIME` metadata, and index directory reparse-point entries without traversing through them. This is deliberately a correctness-first coarse reconciliation provider: rename changes its path-derived identity, and it is not yet a provider-specific incremental FAT/exFAT, network-share, cloud-drive, Linux, or macOS implementation.

`live` and `esm_service` share `LiveIndexSession`. On first start, or when persistence is unusable, the session captures an exact pre-MFT USN boundary, builds the catalog and search index, replays changes that occurred during enumeration, and then follows the Journal. On later starts it validates `<checkpoint>.metadata` against its checksum, volume identity, Journal ID and embedded exact `NextUsn`, attaches the mapped v2 node/name arenas, streams complete transactions from `<checkpoint>.wal`, and finally catches up the current Journal tail. Legacy version 1 snapshots remain readable and are immediately rewritten as v2. Invalid, truncated, expired, wrong-volume, or wrong-Journal snapshots automatically fall back to a full MFT rebuild and are rewritten.

Each applied Journal batch is appended as a checksummed WAL transaction before its external checkpoint is advanced. Replay reads one bounded transaction at a time instead of loading the whole log. A partial final header or payload is treated as a torn tail and physically truncated to the last valid transaction; checksum damage in a complete transaction fails recovery with `ERROR_CRC`. Publishing a consolidated snapshot before clearing the old WAL is restart-safe because replay skips transactions already covered by the snapshot cursor. Runtime polling automatically consolidates a WAL at 64 MiB, while the SCM service also refreshes after five minutes or 100,000 changes and on clean shutdown. This is a functional WAL/checkpoint layer, not yet a general-purpose incremental metadata database.

`NtfsCatalog` keeps its stable state in an ID-sorted compact node table with all base names in one contiguous `wchar_t` arena. That base can be owned heap storage or read-only ranges backed directly by a v2 snapshot mapping, avoiding an `unordered_map` entry and independent `std::wstring` allocation for every stable node. Journal mutations are held in a small hash overlay plus base tombstones; reads merge overlay/base state, and manual or automatic compaction (100,000 pending deltas by default) rebuilds an owned immutable base and releases any mapping. Paths remain derived from parent IDs rather than stored in the catalog base. In the 1,000,000-record benchmark the compact catalog reduced catalog-only working set from 288.06 MB to 98.64 MB; the mapped-v2 benchmark attached 1,000,000 nodes in 13.026 ms without rebuilding node/name vectors.

The v2 save path now writes the compact node table and name arena directly under a shared catalog read view, eliminating the former million-record temporary `vector<FileRecord>` from snapshot creation.

Current query support includes quoted/scoped/excluded terms, `name:`, `path:`, `ext:`, `file:`, `folder:`, wildcard matching, basic Boolean expressions, size/date/attribute predicates, regular-expression terms, case/whole-word request flags, and server-side sorting. It is still a compatibility subset, not the complete Everything grammar.
Name gram signatures contain both raw and accent-folded grams, so the default diacritic-insensitive search mode stays on the indexed candidate path instead of normalizing every catalog entry at query time.

`esm_gui` provides an Everything-style seven-menu desktop layout (`File`, `Edit`, `View`, `Search`, `Bookmarks`, `Tools`, `Help`). Every visible menu command is connected to real behavior and is enabled or disabled from the current selection, clipboard, file-list, and service state. The GUI includes a history-enabled search combo, category filter selector, keyboard-first navigation, a virtual result list, Shell file icons, configurable columns, all Everything-style metadata sort choices, ascending/descending order, window and font sizing, topmost modes, and persisted view/search settings.

File operations include open, open location, cut, copy, Shell file clipboard paste, copy/move to a selected folder, advanced copy/move with collision policy, in-place rename, recycle-bin delete, properties, multi-file drag, result text/path/name/parent copying, and a selection-aware context menu. Export supports EFU, UTF-8 CSV, and UTF-8 plain path lists. The file-list editor can open an existing EFU/CSV list or create a new list, add/rescan directories, remove exact paths or directory subtrees, de-duplicate case-insensitively, overwrite or save as EFU, and immediately load the saved list into the current search window.

Search-menu behavior includes case, whole-word, path, diacritic, and regular-expression switches; advanced search; built-in and user-defined filters; and persisted filter management. Bookmarks store the query and its search flags. Tools provide live search-service connection state, connect/disconnect commands, the file-list editor, and options. Help commands expose local search syntax, regular-expression guidance, command-line help, the local project documentation, update-source status, and About information. The project intentionally labels its Named Pipe connection as a search server rather than claiming Everything ETP compatibility, and does not fabricate a donation destination or online update feed.

The repository includes menu regression scripts. Launch the GUI, then run:

```powershell
.\tools\gui-menu-smoke.ps1 -BuildDir .\build
.\tools\gui-modal-menu-smoke.ps1 -BuildDir .\build
```

These scripts verify menu check/radio transitions, dynamic filter state, and the modal commands for advanced search, filters, bookmarks, service connection, file-list editing, options, syntax help, update status, and About. Native Shell context-menu integration, Windows preview handlers, DPI/dark-mode polish, complete Everything query compatibility, and performance parity remain future work.

The IPC layer uses a versioned binary frame, strict UTF-8 conversion, a 4 MiB payload ceiling, a 1,000-result ceiling, exact reads/writes, client timeout/retry, `PIPE_REJECT_REMOTE_CLIENTS`, and an explicit protected DACL. SYSTEM/administrators have full control; authenticated local users receive pipe read/write access for queries. Both the foreground server and SCM service use four concurrent Pipe instances and a stoppable worker-pool lifecycle. Per-user authorization is not yet implemented.

See `docs/ROADMAP.md`, `docs/ARCHITECTURE.md`, and `docs/PERFORMANCE.md`.

Live modes reject a checkpoint located on the indexed volume. The metadata snapshot is stored beside it as `<checkpoint>.metadata`, so both files must remain outside the indexed NTFS volume. Otherwise checkpoint or snapshot writes would themselves generate USN records and can create a self-sustaining update loop. Snapshot replacement is atomic (`.tmp` plus write-through rename). Before replacing a currently mapped v2 file, the catalog compacts pending deltas or materializes the immutable base into owned vectors so no live mapping holds the destination open. The service refreshes persistence periodically and saves one final snapshot on a clean stop.

Fast NTFS diagnostics (run from an elevated shell):

```powershell
.\build\esm_cli.exe mft D: "readme"
.\build\esm_cli.exe journal D:
.\build\esm_cli.exe journal D: .\build\D.checkpoint
$checkpoint = "$env:LOCALAPPDATA\everything_sm\D-live.checkpoint"
.\build\esm_cli.exe live D: $checkpoint "readme"
```

Current measurements are recorded in `docs/BASELINE_2026-07-23.md`.


