# Performance contract

Parity is measured against the selected Everything reference build on the same machine, volumes, corpus, power mode, antivirus configuration and cache state.

## Primary gates

| Scenario | Gate |
|---|---:|
| NTFS cold enumeration | <= 1.25x reference time |
| Warm service start | <= 1.25x reference time |
| Incremental journal catch-up | <= 1.25x reference time |
| Query first 100 results, P50 | <= 1.10x reference time |
| Query first 100 results, P95 | <= 1.25x reference time |
| Idle metadata memory per 1M entries | <= 1.25x reference memory |
| Idle CPU after catch-up | < 0.2% and comparable to reference |
| Lost or duplicate events after recovery | zero |

Absolute guardrails when the reference executable is unavailable:

- query P50 <= 30 ms on 1M synthetic entries;
- query P95 <= 80 ms on 1M synthetic entries;
- first result batch should fit one UI frame where possible;
- ordinary changes must never trigger a full rescan.

The Phase 1 linear-scan index is a correctness baseline, not the final memory or latency design.

## Corpus

- 1M, 10M and 100M synthetic metadata records;
- short, long and Unicode names;
- deep paths and Windows long paths;
- common prefixes and low-selectivity terms;
- hard links and reparse points;
- source trees, package caches and media libraries;
- rename storms and bulk content extraction.

## Reproduction

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
.\build\esm_benchmark.exe 1000000
.\build\esm_catalog_benchmark.exe 1000000
```

Reports include commit, compiler, CPU, RAM, storage, Windows build, corpus seed and reference version.
