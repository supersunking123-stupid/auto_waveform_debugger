# TODO

Reference design for all measurements: Lumion `vb2b_dbPCIe__ips` (3.8M signals, 19.7M endpoints).
Current numbers and how to reproduce them: `COMPILE_BENCHMARK.md` and `BENCHMARK_HOWTO.md`.
Any change to the compile path must produce a DB that is `cmp`-identical to the previous one
(or come with an explicit, documented DB change).

Suggested order: 4 (remaining hardening) → 5.

## 2. Endpoint merge — done, option A (see "Done / dropped")

## 3. Smaller compile-memory items — done (see "Done / dropped")

Three of the four sub-items are in; the per-endpoint `file` interning was dropped. Lumion peak RSS
27,729,112 kB -> 27,062,888 kB (-0.64 GiB), wall 1:36.9 -> 1:34.7, DB and `.meta` `cmp`-identical.

## 4. Index only canonical instance bodies — default since 2026-09-30; hardening left

Measured on Lumion: about 19 of the ~28 GB peak RSS is slang's AST, and about 12 GB of that
is added during the `SaveGraphDb` build loop. slang elaborates one canonical body per
parameter-identical instance; every other instance body (707k on this design) is bound lazily
the first time the build loop touches it, and stays resident. That is about 555 of the ~700
bytes per endpoint of loop growth, and most of the per-signal tracing time.

Idea: build the per-body trace index from `getCanonicalBody()` once per distinct body and remap
symbols, instances and port connections back to each instance, instead of binding every body.

Start with a spike, not a rewrite:
- Measure what the build loop actually reads from non-canonical bodies (which symbol kinds,
  which lookups) and how many of the 707k are bound only for data derivable from the canonical
  body.
- Check which cases slang already refuses to share a canonical body for; those need no
  handling here.
- Prototype behind an env flag and compare the DB with `cmp`.

Risks and checks:
- Hierarchical path, source location and port-connection resolution must come out identical
  per instance; the DB must stay `cmp`-identical to the current one (including ordering).
- Anything that can differ between bodies sharing a canonical body must not be shared:
  generate branches, defparams, `parameter type`, hierarchical (upward/downward) references
  inside the body, `bind` directives, interface ports / modports connected to different
  interface instances.
- `--relax-defparam`, `--mfcu` and virtual-interface designs need their own test cases.
- Verify with `tests/semantic_regression.py`, `test_cases/run_all_tests.sh`, the small-design
  DB `cmp` sweep, and the Lumion DB `cmp`.
Expected: much lower peak RSS and further wall time reduction (currently 1:36 wall, 26.4 GiB).

**Status (2026-09-30): the canonical tracer is the default; `RTL_TRACE_CANONICAL_BODIES=0` selects the
old per-body binding as a fallback (same DB).** Final Lumion numbers are at the end of this item.
The original plan and spike notes follow.
`RTL_TRACE_CANONICAL_BODIES=1` (`db/CanonicalBodies.inc`, a narrow hook in `SaveGraphDb`) produces
a Lumion DB that is `cmp`-identical to the default. Lumion, same binary, back to back:

| | flag off | flag on |
| --- | --- | --- |
| wall | 1:38 | 0:51 |
| max RSS | 26.7 GiB | 15.1 GiB |
| `build_graph` | 77.6 s | 30.8 s |
| RSS growth in the build loop | 12.6 → 27.3 GB | 12.6 → 15.2 GB |
| per-body index builds | 1.94M (707k distinct) | 2,227 |

Phase 1 findings (`RTL_TRACE_CANONICAL_STATS=1`, `db/CanonicalBodiesStats.inc`):
- Of the 706,788 instance bodies, slang elaborates 2,227. The other 704,561 are skipped:
  130,196 are the top of a skipped subtree and 574,365 lie inside one. They share only 691
  distinct canonical bodies.
- Every skipped body is bound first by the *port-down* follow: a signal in the parent traces into
  the child's port and needs the child's trace index. That first build binds the body. Own-body
  builds (tracing a signal of the skipped body itself) come later and are only rebuilds after
  cache eviction.
- The instrumented default run attributes about 16.6 GB of gross RSS delta to index builds of
  skipped bodies, and 60 s of the 92 s per-signal time to signals in skipped bodies.
- The build loop needs only three things from a skipped body: its trace index, its children's
  port-connection expressions, and what `ResolveTraceResult` reads (paths, source locations, bit
  maps, lhs/rhs refs). All of it can be derived from the canonical body plus the instance path.
  On Lumion, 3,563,951 signals were redirected, with 0 mapping failures and 0 translation
  anomalies. `RTL_TRACE_CANONICAL_VERIFY=1` also builds the baseline record for every signal
  and reported 0 mismatches over 3.8M signals.
- The upward port follow in `CollectPortConnectionResults` is dead code. `GetContainingInstanceSymbol`
  always returns null, because `getHierarchicalParent()` of an `InstanceBody` skips its instance
  (slang `Symbol.cpp:45-57`). As a result, traces only ever go down through ports. The prototype
  mirrors this; if it is ever fixed, the frame-aware upward path in `Escape()` must be tested.

When slang shares a body (cache in `ElabVisitors.h` `tryApplyFromCache`):
- The cache key is the definition, the parameter values, the type parameters, and the interface-port
  keys (interface instance key plus modport name, `InstanceCacheKey.cpp`). The key does not include
  *which* interface instance a port connects to.
- Never cached:
  - instances with a config, a hierarchy override (defparam or instance-bind targets and their
    ancestors), or `Uninstantiated|FromBind|ParentFromBind` (`InstanceCacheKey.cpp:18-21`,
    `InstanceSymbols.h:61`);
  - bodies that contain bind directives or extern interface methods (`Compilation.cpp:1099-1105`,
    `Scope.cpp:311-314`);
  - bodies with upward names (`Compilation.cpp:1314-1339`, checked at `ElabVisitors.h` ~575);
  - instances on hierarchical-assignment paths (un-cached in `finalize()`, `ElabVisitors.h` ~445-462);
  - everything when `DisableInstanceCaching` is set (`Compilation.h:109`).
- Interfaces keep a canonical pointer but are still elaborated.
- Handled by the prototype: generate branches (part of the parameter key) and virtual interfaces.
  slang's placeholder instances (`InstanceSymbol::createVirtual`, `InstanceSymbols.cpp:398-418`)
  are deduplicated globally, so their paths are not translated.

The prototype works as follows:
- A signal of a skipped body is traced on its image in the canonical body, inside a chain of
  frames (crossing instance, canonical body).
- Descending into a skipped child through a port enters a new frame.
- The endpoint paths and lhs/rhs refs are translated prefix by prefix and then re-sorted.
- Crossings into non-module definitions still bind the actual body. Interface connections now
  use cached per-instance root maps. Unresolved connections, incompatible shapes, conflicting
  roots, changed alias relationships, and connected interfaces that forward interface ports
  retain narrow fallbacks. Lumion has no interface crossings.

Verified:
- The small-design sweep (33 designs × default/`--low-mem`/`--partition-budget 2`/
  `--physical-source-paths`/`--mfcu`/`--relax-defparam`): flag on is `cmp`-identical to flag
  off, and flag off is identical to HEAD, in all 198 cases.
- The 10 risk fixtures in `tests/fixtures/canonical_bodies/` are in that sweep and in
  `semantic_regression.py`.
- Lumion: the flag-off and flag-on DBs are both `cmp`-identical to the reference DB.

Hardening on `item4-hardening` (2026-09-30), including Claude review fixes:
- Cached interface-root translation supports scalar/modports, reversed whole arrays, generic
  ports, nested module forwarding, virtual-interface assignment and repeated local wrappers.
  Supported fixtures require redirection with no interface exclusions, skipped-body index
  builds or mapping anomalies. Alias split/merge, conflicting nested roots and interfaces
  that forward interface ports have fixtures requiring their specific fallback reasons.
- Self-referencing ports can shadow the module rewrite for locally traced signals. If a
  source root lies under the representative module, its destination must equal the module
  rewrite; otherwise use the actual body and report `iface_selfref`. Keep consistent roots
  for alias checks. External sources targeting image-local interfaces remain supported.
  Five fallback fixtures and two supported fixtures cover these cases.
- Each of the 27 canonical fixtures now has a separate default VERIFY compile requiring
  positive verified-signal count and zero mismatched lists. Its actual-body work is excluded
  from normal skipped-build checks. The helper regression preserves valid external roots
  across unrelated outer frames and retains enclosing-module rewriting of local interfaces.
  A stricter provenance-aware anomaly diagnostic is optional future work.
- The planned frozen `ALLOW_IFACE=1` DB-mismatch oracle was not reproduced. Existing collectors
  omit direct `HierarchicalValueExpression` interface references. VERIFY catches the local-body
  self-reference corruption but does not validate omitted external references end to end.
  Do not change collectors or DB semantics to force this oracle.
- Keep VERIFY/STATS opt-in and off by default. Keep ALLOW_IFACE as an unsafe diagnostic while
  exclusions remain; it bypasses correctness protection. Any relocation is separate work and
  must preserve production helpers in the statistics include. No switches removed or moved.
- Keep the `=0` fallback until broader design coverage exists.
- The stack-local symbol-ID hint experiment was discarded: median build_graph improved
  9.5%, but its 2.556 s gain did not exceed the 2.997 s baseline range. No persistent cache
  or hint code remains. The contaminated third baseline and uneven sample exclusion policy
  limit this conclusion. A rerun needs user approval, a uniform predeclared contamination
  policy, and three quiet-host alternating pairs after the self-reference fix.
- Historical Task A measurements pass Lumion regression limits (+0.55 s wall, +0.0115 GiB RSS
  median). The generated 4096-module design eliminates all 7898 skipped-body index builds.
  Full samples are in `COMPILE_BENCHMARK.md`; no new timing is claimed for this follow-up.

Fallback coverage and defensive guards (pinned slang `50ce32a`; paths below are relative to
`standalone_trace/third_party/slang/`):
- `iface_conflict`: `iface_conflict.sv` maps a parent interface and its nested leaf to
  destinations that do not preserve their containment relationship.
- `iface_forwarding`: `iface_forwarding.sv` connects a module to an interface that itself has
  an interface port. Root substitution does not model that forwarded connection graph.
- `iface_unresolved`: no valid default-policy crossing fixture was found. Invalid/unconnected
  ports yield empty connections (`source/ast/symbols/PortSymbols.cpp:917–939,1223–1235`);
  forwarding resolution and empty-array/non-interface rejection occur in
  `source/ast/Expression.cpp:1373–1415`. Empty instance arrays arise from dimension errors or
  limits (`source/ast/symbols/InstanceSymbols.cpp:140–158`). These errors block graph building
  in `standalone_trace/compile/Compiler.cc:411–415`. The unconnected-port probe fails before
  producing a DB. Retain the guard for invalid or changed upstream states.
- `iface_shape`: no valid default-policy crossing fixture was found. The cache key requires
  matching definitions, parameters, recursive interface definitions and modports
  (`source/ast/InstanceCacheKey.cpp:71–108`). Interface names, not heterogeneous concatenations,
  are accepted (`source/ast/Expression.cpp:1321–1325`). Array connections are rewired to formal
  ranges and must have compatible dimensions (`source/ast/symbols/PortSymbols.cpp:1193–1216,
  1283–1321`). A dimension-mismatch probe fails before DB generation; differing generic
  interface definitions compile but use separate cache keys. Keep the defensive guard.
These statements are scoped to successful elaboration under the pinned default diagnostic
policy; they do not assert unreachability under suppressed errors or future slang changes.

Final Lumion numbers (main with items 2, 3 and 4, same session; both DBs `cmp`-identical to the
item-2 reference DB, 19,659,527 endpoints):

| | default (canonical) | `RTL_TRACE_CANONICAL_BODIES=0` |
| --- | --- | --- |
| wall | 0:48.7 | 1:34.7 |
| max RSS | 14,820,324 kB (14.13 GiB) | 26,949,052 kB (25.70 GiB) |
| `build_graph` | 29.3 s | 75.0 s |

Verified: small-design sweep (33 designs × 6 variants) default vs `=0` 198/198 identical;
`test_cases/run_all_tests.sh` 27/27; `semantic_regression.py` (canonical block now compares the
default against `=0`).

## 5. Item-5 experiments (2026-10-01)

Review: `/tmp/auto_waveform_item5_claude_review_2026-10-01.md`. Codex report and raw data:
`/tmp/auto_waveform_item5_codex_report_20260930.md`, `/tmp/item5_logs/`.

The first-round quiet-host rule (at most 0.5 cores of other load) could never be met: this host
always carries 1–4 cores of background load. So every compile comparison was reported
inconclusive. The figures below are medians of candidate minus base over all 6 alternating
Lumion pairs. Differences of a few seconds on a ~45 s compile are not treated as significant.

- **E1 merged:** all 27 test scripts use `PYTHON`, defaulting to the repository
  `.venv/bin/python3`, and fail clearly if it is missing. `run_all_tests.sh` passes 27/27.
- **E2 discarded (this design):** parallel tracing with an ordered commit keeps the DB
  byte-identical, but `-t 8` is +6.4 s wall and +1.8 GiB (6/6 pairs). Causes: ~8 s of serial
  prewarm, a commit that never overlaps the workers, a serial fallback for 301k mostly-heavy
  signals, per-worker cold caches (~2× CPU), static chunks plus a barrier. The serial
  restructure alone is ~1 s faster. Prototype kept on `item5-e2`. Default stays at 1 thread.
- **E3a pending fixes (`item5-e3a`):** mmap'd v5 DB with lazy, command-specific indexes. Output
  is identical on a 60-query Lumion corpus, in one-shot and serve modes. Warm trace
  1.68 → 0.78 s and 5.5 → 3.4 GB; find 1.69 → 0.21 s; serve startup 1.66 → 1.04 s. Compile
  cost +0.4 s. Before merging: fsync and atomic `.meta` under the publish lock; a read-only
  fallback for the `.lock` sidecar; GraphString bounds re-check in a long-lived serve; CTest
  coverage with `--reference-bin`.
- **E3b closed:** warm trace after E3a is below 1.0 s, so no v6 indexed format is needed.
- **E4a closed:** the concern was reversed. Merging uses 122 MiB *less* peak RSS than not
  merging (6/6 pairs).
- **E4b/E4c/E4d pending fixes (`item5-visible`):**
  - E4b: logical per-axis coordinates for multidimensional selects.
  - E4c: narrowed display of merged root ranges.
  - E4d: compile-time removal of 1.03M exact-duplicate endpoints (DB −2.2%).
  - Needed before merging:
    - E4b must warn when a single index on a multi-axis signal is reinterpreted or is out of
      range, and must detect old and new DBs in both directions.
    - E4c must narrow only when different coordinates were merged.
    - E4d costs about +5.5 s of build_graph, mostly from a per-call hash-set `clear()`; it
      should be made cheaper.
    - The accepted stack takes fresh semantics epochs (6 or higher), because experimental
      binaries already wrote epochs 3–5.

Open idea: an E2 redesign could plausibly save ~17 s of build time. It would use shared
immutable state, no fallback classes, a frozen path→id map, and dynamic scheduling with a
reorder buffer. Profile the threaded build first.

## Done / dropped

- ~~Zero-copy string-view trace cache~~ — dropped. `SymbolRefList` was already
  `vector<const Symbol*>`, and removing the cache limit would increase memory. The real cost
  centres were the path sort and the body-index cache size (both fixed, see
  `COMPILE_BENCHMARK.md`).
- Invalid regex no longer aborts `find`, `trace` or `serve` — `find --regex` and
  `trace --include/--exclude/--stop-at` validate the pattern at argument-parse time and fail with
  `Invalid regex for <option>: '<pattern>' (<reason>)` (rc 1, no core dump); the `serve` loop also
  wraps every command in a `try/catch`, so one bad query answers with an error and the session
  stays loaded. Tests: `run_invalid_regex_tests` and the `find_fastpath` block in
  `tests/semantic_regression.py`. The MCP serve wrapper now returns serve's stderr diagnostics.
- `test_cases/tc01_*` and `tc02_*` `run_test.sh` now source `~/my_env/vcs.bash` only if present and
  otherwise use `vcs` from `PATH`, skipping the VCS step only when `vcs` is unavailable.
- `list_signals` docstring in `agent_debug_automation/tools.py` uses a neutral wildcard example
  (`top.u_core.axi_ar_*`) instead of the NVDLA one.
- ~~Item 3, smaller compile-memory items~~ (Lumion, same session: HEAD 1:36.9 / 27,729,112 kB max RSS
  -> 1:34.7 / 27,062,888 kB, -0.64 GiB; DB and `.meta` `cmp`-identical; accumulator sizes from the
  `RTL_TRACE_MEM_PROGRESS` `acct build_done` line):
  - Done: load / driver / assignment-lhs ref tables are derived after the build loop from
    `graph.endpoints` + `graph.signal_refs` (counting sort over path ids, one table at a time)
    instead of 37.6M `(path, signal)` pairs collected in the loop: -301 MB touched (349 MB
    reserved) during the loop, finalize refs 1.9 s -> 0.5 s. Only refs inferred from assignment
    text are still collected in the loop, because they intern new strings (1,256 inferred loads on
    Lumion, 0 refs).
  - Done: compact global-net sinks are stored as graph string ids (1.67M sinks, 514 nets); the few
    paths not yet interned at that point (35,430 on Lumion) go to a deduplicated side pool and are
    interned at finalize in the original order: `compact_global` 536 MB -> 18 MB.
  - Done: `SignalCompileItem::path` removed; the paths live in a parallel `std::vector<std::string>`
    that `SaveGraphDb` frees as a whole after the bucket sort, and the struct was repacked
    (80 -> 40 bytes): compile-signal table 305 MB -> 153 MB. `RTL_TRACE_KEEP_SIGNAL_PATHS=1` now
    keeps that vector.
  - Dropped: interning the per-endpoint `file` string once per source buffer. `EndpointRecord`s
    only live for one signal (nothing caches them), so the saving is bounded by the largest single
    record (44,800 loads x ~100 B, about 5 MB), far under the 50 MB bar; it would also change the
    `file` field used by the endpoint-merge grouping key (item 2).
- ~~Item 2, endpoint merge~~ — option A (2026-09-30). The old pass was not a no-op: its key
  pointed into moved-from endpoints and had no `path_id`, so the same source line in different
  instances collapsed into one endpoint (3,951 Lumion lists lost real endpoints), and a prefix
  parse corrupted multi-dimensional bit_maps (`[3][3]` → `[3]`, 5,136 lists); the
  `RTL_TRACE_FIX_ENDPOINT_MERGE` variant shared both bugs. Now one in-place
  `MergeEndpointBitRangesInPlace`: key includes `path_id`/`file_id`, strict `[N]`/`[L:R]` parser,
  source order kept, env switch and Stable variant removed, no DB format bump. Lumion: endpoints
  21.61M → 19.66M, DB 2.74 → 2.63 GB, merge pass 6.5 s → 0.3 s. Verified against an independent
  Python re-implementation on all 7.6M Lumion lists and tc01–tc15. Test 31 and
  `tests/fixtures/endpoint_merge.sv` updated. Agent-visible effect: per-bit assignments and
  generate loops show as one range (`bits [7:0]` instead of eight endpoints).
- `--incremental` no longer reuses DBs built with older compile semantics: the compile fingerprint
  carries a `SEMANTICS_EPOCH` line (now 2, bumped for the endpoint-merge fix), and a `.meta` without
  it or with an older epoch triggers a full rebuild (f8cd024). Bump `kCompileSemanticsEpoch` in
  `db/GraphDb.cc` whenever the same sources and arguments start producing a different DB.
