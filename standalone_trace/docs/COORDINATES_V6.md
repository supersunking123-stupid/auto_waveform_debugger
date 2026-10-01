# Declared coordinates in graph DB version 6

Version 6 stores declarations for signals with two or more numeric axes. This
includes signals with no endpoints and compact global roots. A query can check
declared bounds without inferring a declaration from its accesses.

Packed multidimensional signals require multiple axes for a selected query.
A single selector can mean a flattened waveform bit or an outer array index.
The query reports `ambiguous_single_axis` instead of choosing one interpretation.
An index outside a declared axis reports `axis_out_of_bounds`. Unpacked outer
selectors remain valid. Missing trailing axes select the whole remaining
subarray. Whole-signal queries remain valid.

Enum arrays have a terminal bit axis. Packed struct or union arrays and
multidimensional struct members remain unsupported. Their diagnostics describe
the unsupported type. They do not ask users to rebuild a fresh DB.

## File layout

The 144-byte header retains the existing magic and counts. Its version is 6 and
its reserved feature field is 1. Readers built for versions 1–5 reject version 6.
The new reader accepts versions 1–6. The body through instance parameters keeps
the version 5 layout. Two sections follow:

| Section | Count | Record |
| --- | --- | --- |
| Signal coordinates | uint64 | uint32 signal_id, axis_begin, axis_count, flags |
| Declared axes | uint64 | int32 left, right; uint32 flags |

Coordinate records are 16 bytes. Axis records are 12 bytes. Coordinate rows are
sorted by unique signal ID. Each row has at least two axes. Its axis range starts
immediately after the preceding row. All axis records belong to one row.

Coordinate flags are packed outer axis `0x01`, unsupported coordinate type
`0x02`, enum terminal `0x04`, and packed aggregate terminal `0x08`. Aggregate
terminal requires the unsupported flag. Enum and aggregate terminal are mutually
exclusive. Axis flags are fixed range `0x01` and packed `0x02`. A packed axis must
have a fixed range. A nonfixed axis stores zero bounds.

The loader checks IDs, flags, count arithmetic, axis coverage, section sizes,
and end of file. Truncated sections, unknown flags and trailing bytes fail the
load. Footer allocations are bounded by bytes remaining in the file.

## Query output and legacy files

Trace JSON adds `coordinate_encoding`, `declared_axes`, and `diagnostics`.
Each endpoint adds `bit_map_encoding`. Encodings are `flat_bits`,
`declared_axes`, `parent_struct_bits`, and `legacy_unverified`; endpoint encodings
also use `legacy_flattened` and `unrestricted`. A diagnostic contains `code`,
`message`, and `severity`. Selector errors return nonzero status and a JSON
result with empty endpoints. Text errors appear on stdout and stderr. Serve
response boundaries remain intact after errors.

Legacy files have no declaration table. Every selected legacy query carries
`legacy_dimensions_unverified`. Known multidimensional access evidence in
either drivers or loads, or a query with multiple selectors, reports
`legacy_multidimensional_select` and asks users to rebuild. Access text is
evidence of an access, not proof of declared bounds. Legacy single-axis scalar
results retain their original endpoints and add the warning.

Logical access text distinguishes conservative unknowns. `[?expression]` is a
runtime selector. `[!oob:expression]` is a known constant outside the declared
range. `[!constant-xz:expression]` and `[!constant-overflow:expression]` describe
constants that cannot become an int32 coordinate. `[!invalid-range:expression]`
describes an unsupported range. These axes remain approximate. Other exact axes
still participate in overlap filtering.

## Follow-up experiment snapshots

Epoch 6 measures these coordinate changes. Ordinary range merging remains
enabled. Merged-range display provenance and full-key endpoint dedup are
disabled for this snapshot; their feature tests are explicitly disabled.
Epoch 7 reenables provenance only for merges of different coordinates.
Epoch 8 reenables full-key dedup with cheap scratch reuse. Each epoch forces an
incremental rebuild of older semantics, including epochs 3, 4 and 5.
