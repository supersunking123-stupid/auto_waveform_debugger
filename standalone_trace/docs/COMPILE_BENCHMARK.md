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

### Smaller compile-memory items (TODO item 3, 2026-09-30)

Ref tables derived after the build loop, compact global-net sinks stored as string ids,
`SignalCompileItem::path` removed (see `TODO.md`, "Done / dropped"). Same session, same machine
load, both DBs and `.meta` `cmp`-identical to the reference DB:

| Binary | Wall | User | Sys | Max RSS |
| --- | --- | --- | --- | --- |
| HEAD (`490e5ec`) | 1:36.89 | 92.2s | 4.6s | 27,729,112 kB (26.44 GiB) |
| item 3 | 1:34.65 | 90.1s | 4.5s | 27,062,888 kB (25.81 GiB) |

Accumulators at the end of the build loop (`RTL_TRACE_MEM_PROGRESS=1`, `acct build_done`):

| Accumulator | HEAD | item 3 |
| --- | --- | --- |
| load/driver/assign-lhs ref pairs (37.6M pairs) | 349 MB reserved, 301 MB touched | 0 (derived at finalize) |
| compact global nets (514 nets, 1.67M sinks) | 536 MB | 18 MB (35,430 pooled paths) |
| compile-side signal table (`sizeof(SignalCompileItem)`) | 305 MB (80 B) | 153 MB (40 B) |
| accounted total | 5,213 MB | 4,193 MB |

In the instrumented runs the loop RSS was 0.3-0.8 GB lower through the first 3.2M signals; the peak
moved from a transient at ~3.2M signals (26,887 MB) to the end of the loop (26,377 MB). Finalize
got faster (`refs_s` 1.9 s -> 0.5 s, `global_nets_s` 0.27 s -> 0.02 s).

### Endpoint merge fix (TODO item 2, option A, 2026-09-30)

Measured on top of `490e5ec` (option B was an experiment only and is not in the tree).
One run each, all with `RTL_TRACE_SAVE_GRAPH_PROFILE=1`; see `TODO.md` item 2 for details.

| Build | Wall | Peak RSS | Endpoints | DB size | merge_s |
| --- | --- | --- | --- | --- | --- |
| HEAD `490e5ec` (current pass) | 1:41.9 | 26.43 GiB | 21,610,369 | 2.74 GB | 6.50 s |
| option B: pass removed (experiment) | 1:39.3 | 26.46 GiB | 21,631,237 | 2.74 GB | ~0.2 s |
| option A: candidate merge | 1:38.4 | 26.58 GiB | 19,659,527 | 2.63 GB | 0.30 s |

### Canonical-body tracer as default (TODO item 4, 2026-09-30) — current numbers

Signals of instance bodies that slang skipped are traced on their canonical body (no per-instance
body binding). Same session, `main` with TODO items 2, 3 and 4; both DBs `cmp`-identical to each
other and to the item-2 reference DB (19,659,527 endpoints, 2,625,464,530 bytes):

| Build | Wall | Max RSS | `build_graph` |
| --- | --- | --- | --- |
| default (canonical bodies) | 0:48.73 | 14,820,324 kB (14.13 GiB) | 29.3 s |
| `RTL_TRACE_CANONICAL_BODIES=0` (per-body binding) | 1:34.73 | 26,949,052 kB (25.70 GiB) | 75.0 s |

Index builds drop from 1.94M (707k distinct bodies) to 2,227; RSS growth in the build loop from
~15 GB to ~2.6 GB.

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
| `RTL_TRACE_MEM_PROGRESS=1` | Periodic lines from the main per-signal loop (every 100k signals and for any signal with >100k loads): `[Memory] progress ...` (RSS, container sizes/capacities, cache sizes, `inferred_lhs_refs`, compact global-net `compact_sinks` / `pooled_sinks`), `[Memory] acct ...` (byte estimate of every accumulator vs RSS = "unaccounted", i.e. mostly slang AST), `[Memory] proc ...` (`mallinfo2` glibc arena and `/proc/self/smaps_rollup`, with mimalloc-region vs other anonymous mappings), plus a final `peak_loads_signal=` line with the RSS delta around that signal. Tunable with `RTL_TRACE_MEM_PROGRESS_EVERY=<n>` / `RTL_TRACE_MEM_PROGRESS_BIG_LOADS=<n>`. Costs a `/proc` read per signal (a few percent wall). |
| `RTL_TRACE_MI_STATS=1` | Adds `[Memory] mi <phase> ...` lines (mimalloc `mi_process_info`, then `mi_collect(true)` and re-read) after each `SaveGraphDb` phase, and, **only together with `RTL_TRACE_MEM_PROGRESS=1`**, `[Memory] micensus ...` lines (before the build loop, every progress interval and after the loop): a `mi_heap_visit_blocks` census of live vs page-committed bytes per block size class (the 4096-byte class is slang's `BumpAllocator` segments, i.e. the AST). `RTL_TRACE_MI_STATS=2` (also requires `RTL_TRACE_MEM_PROGRESS=1`) additionally prints the full `mi_stats` as `[MiStats]` lines. Without `RTL_TRACE_MEM_PROGRESS=1`, `RTL_TRACE_MI_STATS` prints only the per-phase `[Memory] mi ...` lines. Set `MIMALLOC_VISIT_ABANDONED=1` in the environment to also census abandoned pages. Note `mi_process_info` "rss/commit" is a commit counter, not RSS; trust `micensus` and `[Memory] proc`. Requires a mimalloc build. |
| `RTL_TRACE_BODY_CACHE=<n>` | Number of per-body trace indexes kept by the build loop (default 4096; 256 before; `--low-mem` always 4). Pure time/memory trade, never changes the DB. |
| `RTL_TRACE_ENDPOINTS_PER_SIGNAL=<n>` | Untouched-virtual reservation of `graph.endpoints` / `signal_refs` / ref-pair vectors as a multiple of the signal count (default 8) so they do not double-and-copy. Never changes the DB. |
| `RTL_TRACE_KEEP_SIGNAL_PATHS=1` | Keep the compile-side signal path vector (parallel to `SignalCompileItem`, which has no path field) during the build loop (default: freed after the bucket sort; the loop reads paths from `graph.strings`). A/B measurement only. |
| `RTL_TRACE_SIGNALS_RESERVE=<n>` | Initial capacity of the compile-time signal vector (default 2000000). A tiny value forces reallocation, for sanitizer runs. |
| `RTL_TRACE_CANONICAL_BODIES=0` | Disables the default canonical-body tracer: every instance body that slang skipped (instance caching) is bound lazily and indexed, as before 2026-09-30. Same DB; much slower and larger on designs with many identical instances (Lumion 0:48.7 / 14.13 GiB default vs 1:34.7 / 25.70 GiB). Fallback and A/B checks. |
| `RTL_TRACE_CANONICAL_VERIFY=1` | With the canonical tracer: also builds the baseline record for every signal (binding every body, like `=0`) and reports mismatches. The DB is still written from the canonical record. Diagnostic only. |
| `RTL_TRACE_CANONICAL_STATS=1` | Prints `[Canon]` statistics: skipped/elaborated instance bodies, which call paths bind them, per-body index builds and redirected signals. |

### Canonical diagnostic switch recommendations (item 4 hardening)

Keep `RTL_TRACE_CANONICAL_VERIFY` and `RTL_TRACE_CANONICAL_STATS` opt-in and disabled by
default. VERIFY builds actual-body records as an independent comparison and remains the
strongest full-design oracle. It reports mismatches but still writes the canonical DB;
validation must require a positive verified-signal count and zero mismatched lists, as well
as successful process status. STATS helps diagnose skipped-body binding, but adds whole-design
classification, bookkeeping, RSS reads and timing. Neither belongs in default-path timing samples.

Keep `RTL_TRACE_CANONICAL_ALLOW_IFACE` only as an unsafe diagnostic while interface exclusions
remain. It bypasses unresolved, shape, conflict, alias split/merge and forwarding-interface
protection. Supported maps still apply; invalid partial maps are discarded. A successful compile
with this switch does not establish correctness. Do not enable it in production or acceptance
runs. The frozen pre-hardening fixtures did not produce DB differences with this switch: existing
collectors omit direct hierarchical interface references. That limitation is not proof that bypass
is safe for other constructs.

A future cleanup could move instrumentation out of the compile implementation. This task makes
no such move and removes no switches. Preserve `CanonBodyHasIfacePort`, a production helper
currently housed in `CanonicalBodiesStats.inc`, and the instance classification logic. Keep
`RTL_TRACE_CANONICAL_BODIES=0` as the independent baseline until broader design coverage exists.


### Item 4 interface hardening measurements (2026-09-30)

Release builds use GCC 11.5 (C++20), pinned slang/fmt, the existing mimalloc source and project
Python. Samples use normal OS caching with no cache flush. No other task-owned heavy jobs
run during timing. All DBs are byte-identical; newly generated metadata matches exactly.
Saved legacy Lumion metadata differs only by the required single `SEMANTICS_EPOCH:2` line.

Task A compares frozen `26d84df` with the corrected interface-mapping implementation:

| Pair | Baseline wall s | Candidate wall s | Baseline build_graph s | Candidate build_graph s | Baseline RSS KiB | Candidate RSS KiB |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 45.17 | 45.17 | 26.664 | 26.646 | 14824972 | 14833844 |
| 2 | 45.49 | 45.76 | 26.725 | 27.235 | 14824368 | 14843560 |
| 3 | 44.78 | 45.72 | 26.204 | 27.027 | 14842100 | 14837060 |
| Median | 45.17 | 45.72 | 26.664 | 27.027 | 14824972 | 14837060 |
| Range | 44.78–45.49 | 45.17–45.76 | 26.204–26.725 | 26.646–27.235 | 14824368–14842100 | 14833844–14843560 |

The median regressions, +0.55 s wall and +0.0115 GiB RSS, pass the 1 s/0.15 GiB limits.
No Lumion gain is expected because it has no interface crossings. VERIFY checks 3,798,275
signals with zero mismatched lists; the DB has 19,659,527 endpoints.

The initial extracted helper copied a destination string on each module rewrite. Two
exploratory pairs showed wall 44.91/45.18 s baseline versus 48.40/48.11 s candidate. That
implementation was discarded. The final helper uses a const string reference.
The original third A2 pair overlapped an unrelated eight-worker CPU job and was replaced;
its wall samples 52.50/49.56 s are excluded. Accepted labels are h4A2_*_1, *_2 and *_3q.
A separate single VCS/simulation job and desktop activity remained visible in host samples.
These are shared-host measurements; no task-owned builds/tests overlapped them.

The deterministic generator defaults to 4096 repeated modules, each with a scalar modport
and whole interface-array port. Three alternating baseline/A pairs gave:

| Metric | Baseline samples | A samples | Baseline median (range) | A median (range) |
|---|---|---|---|---|
| Wall s | 0.16,0.16,0.16 | 0.13,0.14,0.14 | 0.16 (0.16–0.16) | 0.14 (0.13–0.14) |
| build_graph s | 0.079,0.078,0.079 | 0.055,0.055,0.055 | 0.079 (0.078–0.079) | 0.055 (0.055–0.055) |
| RSS KiB | 172452,172264,172456 | 142608,142604,142600 | 172452 (172264–172456) | 142604 (142600–142608) |

Separate STATS runs show skipped-body index builds 7898→0 (4095 distinct first builds),
interface exclusion attempts 12285→0 and redirected signals 0→8190. All mapping anomalies
are zero. Every generated DB and metadata file matches exactly. Absolute timing differences
are small; elimination of binding is the main result.

Verification: CTest 4/4, test-case suite 27/27, canonical-vs-disabled and reference-vs-candidate
sweeps 246/246 each across six variants. All 48 new interface fixture/variant combinations
pass their diagnostic requirements. The frozen ALLOW_IFACE negative-control mismatch was
not reproduced because existing collectors omit direct hierarchical interface references.
The pure path test covers substitutions; end-to-end interface-reference coverage remains open.

Full evidence/report: `/tmp/auto_waveform_item4_codex_report_2026-09-30.md`.


### Task B symbol-ID hint experiment — discarded

The experiment passed a stack-local frame/representative-ID/actual-ID hint through endpoint
and reference translation. Only an exact frame and ID match used the direct ID. All other
paths used Task A translation. It added no persistent cache and preserved sorting/duplicates.

Three alternating pairs compare frozen Task A2 with the hint experiment:

| Pair | Baseline wall s | Candidate wall s | Baseline build_graph s | Candidate build_graph s | Baseline RSS KiB | Candidate RSS KiB |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 45.43 | 42.55 | 26.923 | 23.99 | 14852848 | 14834312 |
| 2 | 45.21 | 43.03 | 26.716 | 24.367 | 14831980 | 14822916 |
| 3 | 48.72 | 43.5 | 29.713 | 24.437 | 14840664 | 14835608 |
| Median | 45.43 | 43.03 | 26.923 | 24.367 | 14840664 | 14834312 |
| Range | 45.21–48.72 | 42.55–43.5 | 26.716–29.713 | 23.99–24.437 | 14831980–14852848 | 14822916–14835608 |

The median build_graph reduction is 2.556 s (9.5%). It exceeds 2% (0.53846 s), but does not
exceed the baseline range (2.997 s). RSS falls by only 6352 KiB, far below 2%. The required
retention rule therefore fails, and the hint code was discarded. Single CPU-bound jobs
from other projects were visible during this shared-host series, including the slower
third baseline. All three pairs are included in this decision.

The experiment passed CTest 4/4, test-case suite 27/27 and both six-variant sweeps 246/246.
All Lumion timing DBs and metadata pass exact comparisons. B VERIFY also checks 3,798,275
signals with zero mismatched lists. Restoring Task A leaves its
validated implementation intact. The experiment patch and measurements are retained in
`/tmp/item4_taskB_hint.patch` and `/tmp/item4_taskB_metrics.json`.


### Review fixes after Task A measurements

Claude's review found a self-reference regression not covered by the original fixtures:
interface-root substitution could rewrite a module-local interface onto an unrelated
external interface. The follow-up rejects that crossing unless its root substitution agrees
with module translation. Consistent self-references and external-to-local connections stay
supported. The branch now reports `iface_selfref` for the excluded case.

Nine new fixtures raise each six-variant sweep from 246 to 300 cases. All 300 canonical-vs-off
and 300 reference-vs-candidate comparisons pass exact DB and metadata checks. CTest passes
4/4, the project suite passes 27/27, and all 27 canonical fixtures have a separate default
VERIFY compile with positive signal counts and zero mismatched lists. The 30-case adversarial
set also passes exact DB/meta comparisons and VERIFY; all five formerly failing cases fall
back, while s1/s5 still redirect without interface exclusions. The diagnostic checker passes
108 interface fixture/variant combinations. See TODO for fallback reachability and source
citations. External interface references omitted by the collectors remain an oracle limit.

No new performance samples were taken. The agreed fix scope requires no Lumion timing rerun;
Lumion has no interface crossings. Historical measurements above describe the earlier binary.
Task B remains discarded. Its third baseline was contaminated, and sample exclusion was not
uniform between A and B. Thus the retention-rule result does not establish that the hint is
ineffective. A user-approved rerun would need a uniform predeclared contamination policy and
three quiet-host alternating pairs after this fix. No hint was reapplied or rerun.

Follow-up evidence: `/tmp/item4_fix_ctest.log`, `/tmp/item4_fix_testcases.log`,
`/tmp/item4_fix_canon/summary.txt`, `/tmp/item4_fix_ab/summary.txt`,
`/tmp/item4_fix_diagnostics.log`, and `/tmp/codex_item4_oracle_rerun/validation_summary.txt`.

### Item-5 experimental measurements (2026-10-01)

These are isolated prototypes, not the current implementation on `item5`.
The full report and raw medians/min-max are in
`/tmp/auto_waveform_item5_codex_report_20260930.md` and `/tmp/item5_logs/`.

E2's default1thread and8thread compile comparisons each obtained0quiet pairs in
6attempts. E3a's default compile comparison did likewise. All runs are preserved;
no compile gain or regression decision follows from their diagnostic numbers.
Default thread count remains1.

E3a paired warm query measurements,3alternating repetitions perquery:

| Operation | Reference median (range) | E3a median (range) |
|---|---|---|
| trace drivers wall |1.7963s (1.7962–1.8468)|0.8239s (0.8108–0.8269)|
| trace max RSS |5,523,320kB|3,405,920kB|
| literal find wall |1.8421s (1.8419–1.8539)|0.2283s (0.2283–0.2294)|
| literal find max RSS |5,522,936kB|2,230,832kB|
| serve startup |1.7046s (1.6936–1.7372)|1.0168s (1.0157–1.0450)|
| serve regex |0.7304s (0.7114–0.7340)|0.6786s (0.6761–0.6787)|

A clean warm confirmation measured trace1.786s (1.771–1.817) ->0.803s
(0.799–0.831), RSS5,523,320 ->3,406,104kB. Serve startup1.696 ->1.015s.
All48saved artifacts match, including every serve repetition/exchange.
The confirmation monitor saw only the persistent desktop renderer; no external
batch job. Query measurements were serialized with worker builds/tests held.

Full49query T0 baseline runs are diagnostic only because external waveform-decoder
activity was found in their host record. E3a's independent paired-query gain exceeds
30% for both trace and literal find. Its compile regression gate is inconclusive,
so the prototype is not retained. Warm trace<1.0s closes the E3b v6 gate.
