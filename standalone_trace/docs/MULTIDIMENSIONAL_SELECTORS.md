# E4b logical multidimensional selectors (prototype)

This experiment changes DB bytes and trace output. It requires a user decision.
The DB version stays at 5. Compile semantics epoch 3 forces an incremental rebuild.
Endpoint reserved bit `0x02` marks logical coordinates. Bit `0x01` remains available
for the merged-range experiment.

Ordinary multidimensional packed and unpacked arrays use declared-axis coordinates
in outer-to-inner order. A row-only access is marked too. For example, `m[1][0]`
writes `[1][0]`, and `m[1]` writes `[1]`. Signed indexes and declaration endianness
are preserved. Indexed part-selects use the declared axis direction to expand
`+:` and `-:` into logical bounds.

A range keeps the current axis. An element select consumes it. The encoder
can compose repeated constraints on one axis. The current slang frontend rejects
selectors chained after a range, such as `m[3:1][2][0]`, before tracing begins.
Those source forms remain unsupported; production diagnostics are unchanged. Unknown selectors remain symbolic, prefixed with `?`, and mark
the endpoint approximate. Each known axis can still exclude a disjoint query.
Unknown preceding slices keep subsequent selections on that axis conservative.

Queries accept a sequence of signed decimal indexes or ranges after the signal
name. They preserve indexes before the final dot in an instance path. The longest
exact stored signal name takes precedence over interpreting a trailing suffix.
Matching uses numeric intersection for each known axis. Missing trailing axes
mean the whole remaining subarray. Port endpoints keep conservative matching.
Filtering still applies at the queried root; port traversal does not translate
selected coordinates into connected signals.

Unmarked nonempty endpoint bitmaps cannot support exact multiple-axis queries.
Such queries stop before output and explain that the DB must be rebuilt. Existing
plain and one-axis queries on old DBs retain the prior matching rule.
Single-dimensional DB encodings and output remain unchanged for existing syntax.

Packed struct member arrays keep the existing absolute-bit representation. Their
multidimensional endpoint ranges are marked approximate, so derived member
filtering remains conservative. Exact multiple-axis struct-member queries are
unsupported and fail clearly, including whole-aggregate and empty-access cases.
Multiple-axis queries of compact global nets are unsupported because compaction
omits coordinate metadata. Associative-array encodings are also unsupported. This experiment does not add a separate absolute-plus-logical
representation for struct members or runtime array bounds.

The encoding flag is carried through DB materialization, endpoint equality,
canonical VERIFY, and endpoint merge grouping. Whole-row ranges can merge on
their shared logical outer axis. Multiple-axis bitmaps are not flattened or merged.
