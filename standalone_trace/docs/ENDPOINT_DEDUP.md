# Exact endpoint deduplication (E4d experiment)

This is a USER DECISION prototype. It can change trace stops and the warnings that agents derive from them. Performance and retention require separate experiment evidence.

The compiler removes only endpoints with equal values for every in-memory field. It preserves the first occurrence and the order of survivors. Drivers and loads are handled separately for each signal. IDs and string refs remain distinct, even if they describe the same path. Ref order and repeated refs remain unchanged.

The key includes kind, path/file text and IDs, line, direction, assignment text and range fields, bitmap text, approximate flag, logical-axis flag, merged-range flag, and all four ordered ref vectors. The query-visible endpoint key and range-merge key are less strict and are not used here. A hash collision still requires complete field equality.

Dedup runs after global-net compaction and range merging, before emission. Clock/reset compaction therefore uses the original load count and sink extraction. Existing endpoint flags and reverse-ref sets are preserved. No endpoint moves while the lookup table holds keys into it. All lookups finish and keys are cleared before stable compaction starts. Scratch is reused across lists and released before reverse-ref finalization.

Database v5 layout stays unchanged. Endpoint/ref section lengths and offsets can shrink. Compile semantics epoch 5 forces an incremental rebuild of an epoch-4 database. The parent-to-candidate regression requires that rebuild, equality with a clean candidate compile, and a second incremental cache hit.

Trace already hides repeated displayed endpoints, but it expands every stored endpoint. Removing a true duplicate can remove a repeated cycle, depth, cone, or filter stop. A node cap is checked before the visited check; deleting a repeated route can change which target first reports that cap. These changes are agent-visible. They are classified with before/after records and route evidence; no replacement stop or warning suppression is added. Old databases retain their stored endpoints and their existing query output.

The stop uniqueness key contains reason, signal, and detail, but excludes depth. In the repeated-connection fixture, removing the first redundant route moves the retained `u2.q` cycle from depth 1 to depth 2. Five exact query cases replay both ordered routes and require the complete observed stop lists and visited counts. This is not a general exemption for changed stop depths.

Tests include complete-field/ref/flag negative cases, forced hash collisions, stable first occurrence, scratch reuse, repeated scalar/symbolic/multiaxis RTL uses, a raw-load compaction threshold case, synthetic repeated logic/port routes, and node/depth/filter/cone boundaries. The optional `--parent-bin` test argument performs the real epoch-4 parent checks and saves every fixture query comparison when `--output-dir` is given.

`RTL_TRACE_SAVE_GRAPH_PROFILE` adds `dedup_s`, removed driver/load counts, and the largest post-merge list size to existing compile diagnostics. It does not add a command option or a trace JSON field. Final acceptance must report endpoint/ref counts, database size, compile wall/RSS, and every corpus difference.
