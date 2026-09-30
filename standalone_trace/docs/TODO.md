# TODO

Reference design for all measurements: Lumion `vb2b_dbPCIe__ips` (3.8M signals, 21.6M endpoints).
Current numbers and how to reproduce them: `COMPILE_BENCHMARK.md` and `BENCHMARK_HOWTO.md`.
Any change to the compile path must produce a DB that is `cmp`-identical to the previous one
(or come with an explicit, documented DB change).

Suggested order: 2 (mostly a decision plus one Lumion run) → 3 → 4 (largest payoff, start with a
measured spike) → 5.

## 2. Endpoint merge: fix it or remove it (changes DB output)

`MergeEndpointBitRangesInPlace` (`db/GraphDb.cc`) builds its grouping key from views into an
endpoint that is then moved from, so endpoints never actually merge. The corrected version,
`MergeEndpointBitRangesInPlaceStable`, exists and is enabled by `RTL_TRACE_FIX_ENDPOINT_MERGE=1`.
It is off by default because it changes the DB: endpoints with the same
path/file/line/text/lhs/rhs and adjacent or overlapping bit ranges collapse into one
(per-bit assignments, generate loops). Endpoint counts on small designs, default vs fixed:

| Design | Default | Fixed |
| --- | --- | --- |
| tc07_mux_tree | 65 | 17 |
| tc05_multi_dim_arrays | 171 | 66 |
| tc12_multi_cone | 260 | 140 |
| tc01_generate_loop | 170 | 110 |
| tc11_deep_soc | 6201 | 6036 |

Today the default pass costs about 7 s on Lumion and merges almost nothing, so the real choice
is between two options:
- **A. Merge properly**: make the stable version the default.
- **B. Do not merge**: remove the pass. This is not automatically `cmp`-identical, because the
  current pass still reorders endpoints and reformats `bit_map`; decide whether to keep an
  equivalent no-merge ordering or accept a one-time DB change.

To do:
- Run Lumion with the fix on: endpoint count, DB size, wall time, peak RSS.
- Check that `trace` output is still correct and no less useful for agents (merged bit ranges
  such as `[3:0]` instead of four single-bit endpoints); review query code that assumes
  one endpoint per assignment. This decides A vs B.
- Output order: both versions emit merged groups in hash-map iteration order (deterministic,
  but not source order, and it changes if the hash or map type changes). If A is chosen, sort
  the output (for example by each group's first original index) so the DB is stable.
- Then: make the chosen behaviour the default, remove the env switch, bump the DB format version
  if readers need to tell the difference, update test 31 in `tests/semantic_regression.py`
  (it currently asserts both the unmerged default and the merged fixed output), and refresh
  the benchmark docs.

## 3. Smaller compile-memory items — done (see "Done / dropped")

Three of the four sub-items are in; the per-endpoint `file` interning was dropped. Lumion peak RSS
27,729,112 kB -> 27,062,888 kB (-0.64 GiB), wall 1:36.9 -> 1:34.7, DB and `.meta` `cmp`-identical.

## 4. Index only canonical instance bodies (large redesign)

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

**Status (2026-09-30): spike done, prototype works, not yet the default.**
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
- Crossings into non-module definitions, or into bodies that have interface ports, fall back to
  binding the actual body. Such a body can be shared although it is connected to different
  interface instances. Lumion has none.

Verified:
- The small-design sweep (33 designs × default/`--low-mem`/`--partition-budget 2`/
  `--physical-source-paths`/`--mfcu`/`--relax-defparam`): flag on is `cmp`-identical to flag
  off, and flag off is identical to HEAD, in all 198 cases.
- The 10 risk fixtures in `tests/fixtures/canonical_bodies/` are in that sweep and in
  `semantic_regression.py`.
- Lumion: the flag-off and flag-on DBs are both `cmp`-identical to the reference DB.

To productionize (about 3–5 days):
- Make the canonical tracer the default and keep the baseline only as a fallback.
- Drop or fold in the stats instrumentation.
- Handle shared bodies with interface ports (translate through the actual interface connection)
  or keep the fallback. This needs a design with many such instances to measure.
- Replace the string-level path translation with symbol-level translation where it is cheap.
- Refresh `COMPILE_BENCHMARK.md` once it is the default. (`test_cases/run_all_tests.sh` with the flag on:
  27/27 pass, checked after integration.)

After integration into main together with item 3 (same session, `cmp`-identical to the reference
DB both ways): flag off 1:38.8 / 27,063,192 kB max RSS; flag on 0:50.4 / 14,942,764 kB,
`build_graph` 79.0 s → 31.9 s.

## 5. Later / ideas

- Parallelise the build loop (slang `freeze()`, per-thread caches, ordered commit of string ids
  to keep the DB byte-identical). Only worthwhile after items 2 and 4, since memory rises with
  thread count.
- Faster one-shot queries: each CLI query reloads the whole DB (3.3 s warm, 15 s cold cache,
  5.6 GB RSS). Options: an mmap-able DB layout or lazy loading of DB sections. Until then,
  agents should use `serve`.
- Small leftover: 15 `test_cases/*` scripts call bare `python3`; the system Python fails on
  `str | List[str]` in `models.py`, so `run_all_tests.sh` only passes with `.venv/bin` first on
  `PATH`. Use the repo `.venv` interpreter explicitly (or a `PYTHON` variable defaulting to it).

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
