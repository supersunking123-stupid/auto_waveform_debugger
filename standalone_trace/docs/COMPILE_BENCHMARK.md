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

## Lumion vb2b_dbPCIe__ips (2026-09-30) — current reference design

The NVDLA sources used above no longer exist on this machine; this design
(`func_ver/sanity/vb2b/vb2b_dbPCIe__ips`, testbench + PCIe IP, top `vb2b_test`) is the new
benchmark. It is much larger than NVDLA: 3.8M signals, 706,788 hierarchy nodes, 21.6M endpoints,
2.74 GB DB. See `BENCHMARK_HOWTO.md` for how to reproduce.

Machine: 24 cores, 61 GB RAM, other jobs may share it. Binary: `standalone_trace/build/rtl_trace`
at commit `8ca9d96` plus no compiler changes (instrumentation-only variants excluded). All runs
under `systemd-run --user --scope -p MemoryMax=40G` (guard only) and `/usr/bin/time -v`.

| Config | Wall | User | Sys | Peak RSS | Build Graph |
| --- | --- | --- | --- | --- | --- |
| default (run 1) | 3:43.07 | 214.0s | 8.3s | 27.77 GiB | 200.7s |
| default (run 2) | 3:44.67 | 216.0s | 6.0s | 27.91 GiB | 201.3s |
| default (run 3) | 3:35.23 | 208.5s | 5.7s | 27.81 GiB | 193.5s |
| `--low-mem` | 13:43.99 | 814.1s | 8.9s | 27.47 GiB | 803.0s |
| `--partition-budget 100000` | 3:45.83 | 217.0s | 7.6s | 27.82 GiB | 198.3s |
| `--partition-budget 25000` | 4:02.46 | 236.1s | 5.8s | 27.56 GiB | 201.6s |

Run-to-run variance for the default config is small (wall 3:35–3:45, RSS 27.8–27.9 GiB).
`--low-mem` is 3.7x slower for a 1% RSS saving; `--partition-budget` changes nothing useful.

### Phase profile (default config, instrumented build)

| Phase | Wall | RSS after |
| --- | --- | --- |
| elaboration | ~5s | 4.6 GB |
| collect traceable symbols (+struct decomposition) | ~6s | 10.1 GB |
| collect instance hierarchy | ~1s | 11.3 GB |
| SaveGraphDb pre-intern | ~1s | 13.1 GB |
| **SaveGraphDb main build loop** | **~185s (signal_record) + 7s merge** | **28.1 GB (peak 28.6 GB)** |
| finalize refs / hierarchy / string offsets | ~2.5s | 28.0 GB |
| write DB | 1-13s (page cache dependent) | 28.0 GB |

- Growth in the build loop is steady and proportional to endpoints emitted (~700 B/endpoint),
  it is not caused by transient cache spikes (trace cache stays at 256 bodies; the worst signal
  has only 44,800 loads and adds ~250 MB).
- The identifiable accumulators (endpoints, signal_refs, strings, ref tables) explain only
  ~5-6 GB of the ~15 GB growth; mimalloc reports 10.7 GB committed against 28 GB OS RSS.
  Attribution work is tracked in `PLAN.md`/follow-up sections.
- The last doubling of `graph.endpoints` (19.0M -> 38.07M capacity) adds ~0.5 GB to the peak.

### Query behaviour on the Lumion DB (baseline DB)

| Operation | Cost |
| --- | --- |
| one-shot CLI query (DB load) | ~3.3 s warm cache (~15 s cold), 5.6 GB RSS per process |
| `serve` startup + load | ~2.2 s |
| `find --regex` in `serve` | ~13 s (single-threaded `std::regex_search` over all 3.8M names) |
| `trace` drivers/loads (cone level 1-3) in `serve` | < 1 ms |
| `hier --depth 3 --format json` in `serve` | ~0.44 s |

Spot checks of `trace` results against the original sources (`apb4_master_bfm.v`,
`dbPCIe__ips.vp`, `apb_parity_generator.v`) matched.

## Compile diagnostics environment variables

All are off by default and do not change the DB unless noted.

| Variable | Effect |
| --- | --- |
| `RTL_TRACE_SAVE_GRAPH_PROFILE=1` | `save_graph_db` time breakdown (`build_graph phases`, counts, `finalize phases`) plus a `signal_record breakdown` line: `index_build_s` (per-body trace index builds, includes first-touch AST binding), `bodies_built/bodies_distinct/bodies_rebuilt` (rebuilds = index evicted from the body cache and built again), `resolve_s`, `symbol_path_less_s`. These overlap (nested inside `signal_record_s`). |
| `RTL_TRACE_MEM_PROGRESS=1` | Periodic lines from the main per-signal loop (every 100k signals and for any signal with >100k loads): `[Memory] progress ...` (RSS, container sizes/capacities, cache sizes), `[Memory] acct ...` (byte estimate of every accumulator vs RSS = "unaccounted", i.e. mostly slang AST), `[Memory] proc ...` (`mallinfo2` glibc arena and `/proc/self/smaps_rollup`, with mimalloc-region vs other anonymous mappings), plus a final `peak_loads_signal=` line with the RSS delta around that signal. Tunable with `RTL_TRACE_MEM_PROGRESS_EVERY=<n>` / `RTL_TRACE_MEM_PROGRESS_BIG_LOADS=<n>`. Costs a `/proc` read per signal (a few percent wall). |
| `RTL_TRACE_MI_STATS=1` | Adds `[Memory] mi <phase> ...` lines (mimalloc `mi_process_info`, then `mi_collect(true)` and re-read) after each `SaveGraphDb` phase, and `[Memory] micensus ...` lines: a `mi_heap_visit_blocks` census of live vs page-committed bytes per block size class (the 4096-byte class is slang's `BumpAllocator` segments, i.e. the AST). `RTL_TRACE_MI_STATS=2` additionally prints the full `mi_stats`. Set `MIMALLOC_VISIT_ABANDONED=1` in the environment to also census abandoned pages. Note `mi_process_info` "rss/commit" is a commit counter, not RSS; trust `micensus` and `[Memory] proc`. Requires a mimalloc build. |
| `RTL_TRACE_BODY_CACHE=<n>` | Number of per-body trace indexes kept by the build loop (default 4096; 256 before; `--low-mem` always 4). Pure time/memory trade, never changes the DB. |
| `RTL_TRACE_ENDPOINTS_PER_SIGNAL=<n>` | Untouched-virtual reservation of `graph.endpoints` / `signal_refs` / ref-pair vectors as a multiple of the signal count (default 8) so they do not double-and-copy. Never changes the DB. |
| `RTL_TRACE_KEEP_SIGNAL_PATHS=1` | Keep the `SignalCompileItem::path` strings during the build loop (default: freed after they were interned; the loop reads paths from `graph.strings`). A/B measurement only. |
| `RTL_TRACE_SIGNALS_RESERVE=<n>` | Initial capacity of the compile-time signal vector (default 2000000). A tiny value forces reallocation, for sanitizer runs. |
| `RTL_TRACE_FIX_ENDPOINT_MERGE=1` | Uses `MergeEndpointBitRangesInPlaceStable`. **Changes DB output**: endpoints that share path/file/line/text/lhs/rhs and have adjacent or overlapping bit ranges now merge (default merging is defeated by a moved-from grouping key). |
