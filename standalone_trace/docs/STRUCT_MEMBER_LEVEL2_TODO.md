# Struct Member Tracing — Implementation Status

## Level 1: Member Access Expression Tracing (DONE)

Resolves `MemberAccessExpression` chains (e.g., `pkt.valid`, `req.aw.addr`) to parent struct signals with correct bit offsets in expression tracing. Endpoints show member paths with absolute bit ranges.

Commits: `a82503d`, `66cb3e5`, `f7118ce`

## Level 2: Struct Member Signal Decomposition (DONE)

Enumerates packed struct fields as first-class signals so `find` discovers members and `trace --signal` works on individual fields.

### Implementation Summary

| Component | Change |
|-----------|--------|
| `CompileData.h` | Added `parent_signal_idx`, `member_bit_offset`, `member_bit_width`, `struct_depth` to `SignalCompileItem` |
| `Compiler.cc` | Forward declaration + call to `DecomposeStructMembers(signals, 2)` after `CollectTraceableSymbols` |
| `GraphDbTypes.h` | Bumped DB version to 5. Added 3 fields to `GraphSignalRecord` (20→32 bytes) |
| `GraphDb.cc` | `DecomposePackedStructFields` / `DecomposeStructMembers` for field enumeration; v4/v5 backward-compatible load; member endpoint materialization by bit-range filtering parent endpoints |
| `semantic_regression.py` | Tests 26-29: find discovers members, trace filters correctly, RHS not corrupted by sibling members |

### Key Design Decisions

- **Member signals skip `BuildSignalRecord`** — they store 0 drivers/0 loads in the DB. Endpoints are materialized at query time by filtering the parent's endpoints via `EndpointMatchesSignalSelect()`.
- **Member signals excluded from `symbol_path_ids`** — they reuse the parent's `Symbol*`, so inserting them would corrupt LHS/RHS resolution (last-member-wins).
- **Max decomposition depth = 2** — top-level fields + one level of nested struct fields.
- **Cone-level > 1 expands at parent-struct granularity** — member filtering applies only at the first cone level. Deeper traversal naturally sees the full parent struct, which is correct structural behavior.

### Performance (Cheshire SoC)

- ~57k signals → ~105k with depth-2 decomposition
- Compile time increase < 5% (member signals skip expensive AST walk)
- DB size: +60% signal records (32 vs 20 bytes each, no extra endpoints)

Commits: `985bad4`, `2f6fb27`

## Remaining Work

### 3. Hierarchy Display — `hier` Command

Extend `hier` output to show struct members under their parent signal. Add `--show-members` flag or group members as children in JSON output.

### 4. Waveform Integration — MCP Layer

Struct members in waveforms (VCD/FST) are stored as flat bit vectors. The MCP tools (`create_bus_slice`) should auto-map struct member queries to the correct bit range on the parent waveform signal. This requires the DB to carry the bit offset/width metadata (already stored in Level 2).

### 5. Unpacked Struct Support

Level 1-2 only handle packed structs. Unpacked struct members have different offset semantics (byte offsets, not bit offsets). Extend `ResolveStructMemberAccess` to handle `UnpackedStructType` and `UnpackedUnionType`.

### 6. Nested Typedef Resolution

Handle chains like `typedef struct packed { ... } aw_t; typedef struct packed { aw_t aw; ... } req_t;` where the member type is itself a typedef. `getCanonicalType()` handles this, but the displayed type name should be the typedef name, not the anonymous struct.
