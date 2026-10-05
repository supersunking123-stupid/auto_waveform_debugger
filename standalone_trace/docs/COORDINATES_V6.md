# Declared coordinates in graph DB version 6

Version 6 initially stored declarations for signals with two or more numeric axes. This
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
its reserved field selects features. Supported values are 1, 3 and 15.
Epoch 18 writes 15. Readers built for versions 1–5 reject version 6.
The new reader accepts versions 1–6. The body through instance parameters keeps
the version 5 layout. Two sections follow:

| Section | Count | Record |
| --- | --- | --- |
| Signal coordinates | uint64 | uint32 signal_id, axis_begin, axis_count, flags |
| Declared axes | uint64 | int32 left, right; uint32 flags |

Coordinate records are 16 bytes. Axis records are 12 bytes. Coordinate rows are
sorted by unique signal ID. Initial feature-1 rows have at least two axes.
Mapped-owner coverage adds verified sparse rows with one axis. These rows
require coordinate flag `0x10` and port-coverage feature `0x04`. Genuine
feature-3 member rows still have at least two axes. Each row's axis range starts
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
selection keeps its existing coordinate behavior and adds the warning. The
query-reference identity repair can restore distinct contexts that the old key
suppressed; it applies to legacy formats too.

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
Collapsing exact copies of `[7:0]` leaves its original flag unset. A selected
query keeps that complete range, as it does for a single `[7:0]` access. When
different coordinates merge, display narrowing applies only to the root's
output copy. Traversal, cached records, member coordinates and logical axes
keep their original bitmaps.
Epoch 8 reenables full-key dedup with cheap scratch reuse. Each epoch forces an
incremental rebuild of older semantics, including epochs 3, 4 and 5.

Dedup returns before touching scratch for empty and singleton lists. Active
lists use an open-address table with original-record indices and cached hashes.
Only occupied slots are cleared before moving survivors. Full-field equality
and hashing still include strings, ordered references and coordinate flags.
Survivors keep their first occurrence and source order. Compaction runs first,
so duplicate removal does not change the compact-global threshold.

Epoch 18 uses port query coverage (header feature bit `0x04`) and exact compact
port routes (`0x08`). New DBs use header feature field 15. Endpoint source bitmaps retain their native
coordinate space. Their stored `Q1;...|...` envelope carries separate owner-domain
intervals: physical packed offsets, or flattened ordinals for fixed unpacked
owners. Fixed unpacked mapped owners
use sparse full-owner ordinals derived from declared logical prefixes and
terminal packed strides. Query output strips that
envelope. Constants and unsupported connection domains have explicit stop
markers; they do not become neighboring source references.

Mapped owner and crossed source declaration rows use flag `0x10` and may carry one declared packed
axis, including ascending or nonzero ranges. Member rows whose owner offsets
are not verified also use flag `0x20`; exact filtering stops conservatively for
those rows. Exact fixed unpacked source selectors remain logical indices. Only the separate
mapped owner coverage uses the full-owner ordinal. Owner query conversion uses
the same checked declaration order, directions and strides. Old readers reject the new feature bit.

Feature field 7 is rejected. Rebuild old experimental caches with the current
compiler. Epoch 18 invalidates older mapping semantics without changing the
v6 record layout.

Exact standalone compact ports carry an `R1` route envelope and endpoint flag
`0x20` in addition to coverage flag `0x04`. Each sparse piece records child low
and high owner ordinals, a target root path string ID, and the target low
ordinal. The target range has the same width. Fixed unpacked indices flatten
only in this hidden route domain. Exact public native source indices remain logical.
Verified source-member offsets stay relative to their root. Canonical targets
use the actual parent frame; the port identity uses the child frame.

Approximate legacy hop answers preserve A's public native bitmap and encoding.
For a fixed one-axis array of scalar elements, the checked legacy conversion
preserves `flat_bits` storage offsets. This exception does not infer member offsets
or multidimensional layouts.

R1 pieces cover the complete formal and are exact. Each original bound piece
fits its selected packed row. After conversion to full-owner ordinals, adjacent
pieces with the same target and translation may merge across contiguous rows.
The runtime validates that full-root range; it does not treat it as one row. Constant, unsupported and
one-bit dependency maps use existing Q1 mapped traversal instead. Already
projected exact compact branches remain Q1 terminal ports. Unproven mappings
retain the legacy hop answer marked approximate and report the actual parent
connection text and source location. Unknown layouts and bounded-map refusals
stop visibly. Route fields take part in compiler dedup and VERIFY.
Native output dedup still runs after owner-domain filtering.

Query deduplication includes exact ordered public LHS and RHS names, with
separate vector counts and length-framed fields. Shared source locations do not
make generated timing contexts identical. This query-only repair applies to
legacy formats too; it does not change DB bytes or native coordinates.

Root input queries can retain an active declaration beside a mapped writer.
For an unused plain input, the exact connection and selected owner coverage
control this compatibility record. An ordinary ANSI packed-struct input uses
a complete checked formal map with exact signal or constant pieces. Its
constant-connected bits keep the active declaration and the constant stop.
The retained port keeps its original public native bitmap. Q1 coverage filters
the queried owner domain separately. A connected writer alone does not prove
that an active port declaration can be removed.
