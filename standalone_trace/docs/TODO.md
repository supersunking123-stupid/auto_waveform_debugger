# TODO

Reference design for all measurements: Lumion `vb2b_dbPCIe__ips` (3.8M signals, 21.6M endpoints).
Current numbers and how to reproduce them: `COMPILE_BENCHMARK.md` and `BENCHMARK_HOWTO.md`.
Any change to the compile path must produce a DB that is `cmp`-identical to the previous one
(or come with an explicit, documented DB change).

## 1. Make the endpoint-merge fix the default (changes DB output)

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

To do:
- Run Lumion with the fix on: endpoint count, DB size, wall time, peak RSS (the merge pass ran
  about 7 s with the bug present).
- Check that `trace` output is still correct and no less useful for agents (merged bit ranges
  such as `[3:0]` instead of four single-bit endpoints); review query code that assumes
  one endpoint per assignment.
- If accepted: make it the default, remove the env switch, bump the DB format version if
  readers need to tell the difference, update test 31 in `tests/semantic_regression.py`
  (it currently asserts both the unmerged default and the merged fixed output), and refresh
  the benchmark docs.

## 2. Index only canonical instance bodies (large redesign)

Measured on Lumion: about 19 of the ~28 GB peak RSS is slang's AST, and about 12 GB of that
is added during the `SaveGraphDb` build loop. slang elaborates one canonical body per
parameter-identical instance; every other instance body (707k on this design) is bound lazily
the first time the build loop touches it, and stays resident. That is about 555 of the ~700
bytes per endpoint of loop growth, and most of the per-signal tracing time.

Idea: build the per-body trace index from `getCanonicalBody()` once per distinct body and remap
symbols, instances and port connections back to each instance, instead of binding every body.

Risks and checks:
- Hierarchical path, source location and port-connection resolution must come out identical
  per instance; the DB must stay `cmp`-identical to the current one.
- Anything parameter-dependent that differs between bodies sharing a canonical body
  (generate branches, defparams) must not be shared.
- `--relax-defparam`, `--mfcu` and virtual-interface designs need their own test cases.
- Verify with `tests/semantic_regression.py`, `test_cases/run_all_tests.sh`, the small-design
  DB `cmp` sweep, and the Lumion DB `cmp`.
Expected: much lower peak RSS and further wall time reduction (currently 1:36 wall, 26.4 GiB).

## 3. `find` / `serve` abort on an invalid regex (robustness bug)

`rtl_trace find --regex` with a malformed pattern (for example `zz(`) throws an uncaught
`std::regex_error`: the process aborts (exit 134, core dump). In `serve` mode this kills the
whole server, so an agent's mistyped regex loses the resident session and forces a reload of
a multi-GB DB (~2 s to 15 s, 5.6 GB RSS on Lumion). Fix: catch `std::regex_error` in
`RunFindWithSession` (and any other `std::regex` use), print an error, return a non-zero code
without exiting; add a test in `tests/semantic_regression.py` (the `find_fastpath` block
currently asserts the abort behaviour and must be changed with the fix). Also consider a
top-level `try/catch` in the `serve` loop so no query can take the session down.

## 4. Smaller compile-memory items (each about 0.25–0.4 GB on Lumion)

- Derive the load / driver / assignment-lhs ref tables at finalize from `graph.endpoints` and
  `signal_refs` instead of building them in the loop (interacts with string-interning order).
- Store the compact global-net sinks as string ids instead of `std::string` copies.
- Drop the `path` field of `SignalCompileItem` entirely (it is already freed after interning).
- Intern the per-endpoint `file` string once per source buffer.

## 5. Later / ideas

- Parallelise the build loop (slang `freeze()`, per-thread caches, ordered commit of string ids
  to keep the DB byte-identical). Only worthwhile after items 1–2, since memory rises with
  thread count.
- Startup: one-shot CLI queries reload the whole DB each time (3.3 s warm, 15 s cold cache,
  5.6 GB RSS); prefer `serve`.
- Known small leftovers: `test_cases/tc01_*` and `tc02_*` still skip the VCS step when
  `~/my_env/vcs.bash` is missing even though `vcs` is on `PATH`; the `list_signals` docstring in
  `agent_debug_automation/tools.py` still shows an NVDLA wildcard example.

## Done / dropped

- ~~Zero-copy string-view trace cache~~ — dropped. `SymbolRefList` was already
  `vector<const Symbol*>`, and removing the cache limit would increase memory. The real cost
  centres were the path sort and the body-index cache size (both fixed, see
  `COMPILE_BENCHMARK.md`).
