# Compile Performance Benchmarks

NVDLA design (1M signals, 4.3M endpoints), 15 GB RAM machine, swap flushed before each run.

## Reading Guide

- Compare rows only within the same section.
- `Baseline`, `c2435ac`, and `8b04bc0` are historical uncapped runs.
- `Current Working Tree` was measured under a hard 6 GB cap:
  `ulimit -v 6291456`.
- A follow-up uncapped rerun of the current tree is included below.
- Peak RSS comes from `/usr/bin/time -v`. `Build Graph` comes from the
  `save_graph_db: build_graph done` log line.

## Historical Reference

### Baseline

Commit `494b78b` — no mimalloc, no string_view interning, no symbol path cache.

| Config | Build Graph | Wall Time | User Time | System Time | Peak RSS |
|---|---|---|---|---|---|
| (no `--low-mem`) | 87.6s | 142s | 49s | 93s | 5.5 GB |

### c2435ac — mimalloc + string_view interning + symbol path cache

| Config | Build Graph | Wall Time | User Time | System Time | Peak RSS |
|---|---|---|---|---|---|
| **without `--low-mem`** | 29.2s | **44.1s** | 39.2s | 5.5s | 5.35 GB |
| with `--low-mem` | 58.8s | 1:16 | 66.5s | 10.3s | 4.35 GB |

Note: the commit message claimed 46s wall time with 4.6 GB peak RSS. That benchmark was run
without `--low-mem` (confirmed by back-to-back comparison). The commit message was misleading.

### 8b04bc0 — allocation elimination round

Changes on top of c2435ac: `from_chars` integer parsing, `string_view` SymbolPathLess,
`MergeEndpointBitRangesInPlace` fast path, clock/reset name detection without allocation,
sort+unique skip for small vectors, explicit move semantics.

| Config | Build Graph | Wall Time | User Time | System Time | Peak RSS |
|---|---|---|---|---|---|
| **without `--low-mem`** | **25.2s** | **39.4s** | 34.8s | 5.3s | 5.37 GB |
| with `--low-mem` | 51.4s | 1:06 | 61.1s | 5.6s | 4.35 GB |

## Current Working Tree — body-local caches + real partition execution

NVDLA benchmark rerun on 2026-04-10 with a hard 6 GB virtual-memory cap:
`ulimit -v 6291456` before `rtl_trace compile`.

These numbers are safe-system runs, so wall time is not directly comparable to the
older uncapped measurements above. The useful comparison here is row-to-row inside
this section.

### Direct Comparison Under the 6 GB Cap

| Config | Build Graph | Vs Capped Default | Wall Time | Vs Capped Default | Peak RSS | RSS Delta |
|---|---|---|---|---|---|---|
| default | 101.8s | baseline | 2:18.76 | baseline | 4.376 GB | baseline |
| `--low-mem` | 133.0s | slower by 31.2s | 2:29.21 | slower by 10.45s | 4.376 GB | same |
| partition `100000` | 91.6s | faster by 10.2s | 1:48.64 | faster by 30.12s | 4.376 GB | same |
| partition `25000` | 71.0s | faster by 30.8s | 1:27.69 | faster by 51.07s | 4.377 GB | same |

### Memory Shape During the Capped Runs

| Stage | Current RSS | Peak RSS | Read |
|---|---|---|---|
| `MemAfterElab` | 1613 MB | 2248 MB | elaboration is not the final peak |
| `MemBeforeSaveGraphDb` | 2276-2277 MB | 2281 MB | symbol collection + hierarchy stay below 2.3 GB |
| `MemAfterSaveGraphDb` | 3750-3751 MB | 4273-4274 MB | final graph materialization creates the peak |

### What Changed

- The body-local cache refactor reduced the default compile peak from about `5.37 GB`
  to about `4.38 GB` on this workload.
- `--low-mem` no longer buys a measurable RSS reduction on top of the new default path,
  but it is still slower.
- `--partition-budget` now changes execution materially, and smaller partitions improved
  wall time under the 6 GB cap.
- `--partition-budget` did not reduce peak RSS. The dominant peak is still in
  `save_graph_db` after elaboration, which points to the final graph materialization /
  reverse-ref / serialization working set as the remaining memory bottleneck.

### Current Tree Without the 6 GB Cap

These reruns answer the narrow question of whether `ulimit -v 6291456` was inflating
wall time on the current tree.

| Config | Build Graph | Vs Capped Run | Wall Time | Vs Capped Run | Peak RSS | RSS Delta |
|---|---|---|---|---|---|---|
| default | 87.2s | faster by 14.6s | 2:01.11 | faster by 17.65s | 4.376 GB | same |
| partition `25000` | 69.8s | faster by 1.2s | 1:25.98 | faster by 1.71s | 4.377 GB | same |

Read:

- Removing the VM cap helps wall time somewhat, but not dramatically.
- The cap is not the main reason current wall time is still far above the old `39.4s`
  historical result.
- Peak RSS is effectively unchanged with or without the cap.

### Current Tree After Cache Retune

The default per-body cache limit was increased from `16` to `256`. This was the
first direct attempt to recover the `save_graph_db` regression without giving back
the RSS improvement.

| Config | Build Graph | Wall Time | Peak RSS | Read |
|---|---|---|---|---|
| default, cache `16` | 87.2s | 2:01.11 | 4.376 GB | previous default after the memory refactor |
| default, cache `256` | 60.2s | 1:20.28 | 4.377 GB | large wall-time recovery at effectively unchanged RSS |

Read:

- The small `16`-entry cache was a major contributor to the wall-time regression.
- Increasing the default cache to `256` recovered about `40.8s` of wall time and
  about `27.0s` of `build_graph` time.
- Peak RSS stayed flat because the true peak is still dominated by final graph
  materialization, not the compile-time body cache.

### Current Tree After Locality Scheduling

The default build order was then changed to process signals grouped by containing
instance path instead of raw full-path order. This keeps per-body compile caches hot
without needing explicit partition mode.

Protected rerun on 2026-04-10 with `ulimit -v 6291456`:

| Config | Build Graph | Wall Time | Peak RSS | Read |
|---|---|---|---|---|
| default | 47.4s | 1:03.05 | 4.386 GB | current fastest protected config |
| partition `25000` | 49.5s | 1:04.37 | 4.385 GB | slightly slower than default |
| partition `100000` | 49.0s | 1:07.00 | 4.385 GB | slower than default |

Read:

- Locality scheduling recovered another large chunk of wall time.
- After this change, explicit partitioning no longer helps speed on the benchmark.
- Peak RSS stayed essentially flat at about `4.385-4.386 GB`.
- The remaining time is now inside the per-signal `save_graph_db` build work, not in
  cache thrash from traversal order.

### Current Tree After Compile-Time Signal-Ref IDs

The next successful round kept compile-time endpoint `lhs/rhs` references as signal
path IDs whenever possible and added a bulk-copy fast path for ID-only endpoints.
This preserved the serialized DB format while removing a large amount of temporary
string churn during `save_graph_db`.

Protected rerun on 2026-04-10 with `ulimit -v 6291456`:

| Config | Build Graph | Wall Time | Peak RSS | Read |
|---|---|---|---|---|
| default | 31.3s | 0:47.14 | 4.383 GB | current best protected config |

Read:

- This recovered another large chunk of wall time over the locality-scheduling build.
- Peak RSS improved slightly while staying in the same `~4.38 GB` band.
- The bulk-copy fast path was necessary. A first version without that fast path
  regressed badly and was intentionally not kept.
- Later follow-up experiments such as visited-set reuse and compile-time file-view
  storage were measured and then rejected because they were flat-to-worse.

## Condensed Summary

### Historical Uncapped Trend

| Version | Default Wall Time | Default Peak RSS | `--low-mem` Wall Time | `--low-mem` Peak RSS | Read |
|---|---|---|---|---|---|
| `494b78b` | 142s | 5.5 GB | n/a | n/a | old baseline |
| `c2435ac` | 44.1s | 5.35 GB | 1:16 | 4.35 GB | first major speedup |
| `8b04bc0` | 39.4s | 5.37 GB | 1:06 | 4.35 GB | best uncapped wall time before current work |

### Current Bottom Line

- Peak RSS is down from about `5.37 GB` to about `4.38 GB`.
- The remaining peak comes from `save_graph_db`, not elaboration.
- `--low-mem` is no longer useful on this workload.
- Partitioning no longer improves time after locality scheduling, and still does not
  improve RSS.
- Removing the 6 GB cap recovers some wall time, but not enough to explain the full
  regression versus the old uncapped historical build.
- Retuning the default body-cache limit from `16` to `256` materially recovers wall
  time while keeping peak RSS essentially unchanged.
- Grouping the default build order by containing instance path recovers additional
  wall time and makes plain default compile the fastest protected mode again.
- Keeping compile-time signal refs as IDs, with a fast path for ID-only endpoints,
  reduces temporary string churn and currently gives the best protected result:
  `0:47.14` wall time, `31.336s` `build_graph`, `4.383 GB` peak RSS.
