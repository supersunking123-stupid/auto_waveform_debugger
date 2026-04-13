# Level 2: Full Struct Member Decomposition (TODO)

## Goal

Make individual struct members discoverable and queryable as first-class signals in rtl_trace. Users should be able to `find axi_llc_mst_req.aw.addr` and `trace --signal top.req.aw.valid` directly.

## Current State (after Level 1)

- Expression tracing resolves `MemberAccessExpression` to parent struct + bit offset
- Endpoints show member paths (e.g., `top.req.aw.valid`) with bit ranges
- But `find`, `hier`, and signal lookup only know about whole-struct signals
- Struct members are NOT in the signal index

## Changes Needed

### 1. Signal Collection — `CollectTraceableSymbols()` (GraphDb.cc:1274)

For each collected `SignalCompileItem`, check if its type is a packed struct (after `getCanonicalType()`). If so, enumerate `FieldSymbol` members via the Scope API and emit additional `SignalCompileItem` entries for each member. Recurse for nested struct members.

Path naming: `top.u1.my_struct.aw.valid`
Bit offset/width: from `FieldSymbol::bitOffset` and `getType().getCanonicalType().getBitWidth()`

### 2. Compile-Time Data — `CompileData.h`

Extend `SignalCompileItem`:
```cpp
uint64_t member_bit_offset = 0;   // 0 for top-level signals
uint64_t member_bit_width = 0;    // 0 = use symbol type width
uint32_t parent_signal_idx = UINT32_MAX;  // index into signals vector
```

### 3. DB Schema — `GraphDbTypes.h`

Bump version to 5. Add fields to `GraphSignalRecord`:
```cpp
uint32_t parent_signal_id = UINT32_MAX;  // reference to parent struct signal
uint64_t member_bit_offset = 0;
uint64_t member_bit_width = 0;
```

These are backward-compatible: version 4 readers ignore the extra fields.

### 4. Signal Index — Runtime Query

`BuildSessionSignalIndex()` already builds `signal_name_to_id` from all `GraphSignalRecord` entries. With member signals added, `find axi_llc_mst_req.aw` and direct `trace --signal top.req.aw.valid` will work automatically.

### 5. Hierarchy Display — `hier` Command

Extend `hier` output to show struct members under their parent signal. Add `--show-members` flag or group members as children in JSON output.

### 6. Waveform Integration — MCP Layer

Struct members in waveforms (VCD/FST) are stored as flat bit vectors. The MCP tools (`create_bus_slice`) should auto-map struct member queries to the correct bit range on the parent waveform signal. This requires the DB to carry the bit offset/width metadata.

### 7. Unpacked Struct Support

Level 1 only handles packed structs. Unpacked struct members have different offset semantics (byte offsets, not bit offsets). Extend `ResolveStructMemberAccess` to handle `UnpackedStructType` and `UnpackedUnionType`.

### 8. Nested Typedef Resolution

Handle chains like `typedef struct packed { ... } aw_t; typedef struct packed { aw_t aw; ... } req_t;` where the member type is itself a typedef. `getCanonicalType()` handles this, but the displayed type name should be the typedef name, not the anonymous struct.

## Estimated Effort

Large — touches compilation, DB format, query layer, and MCP integration. Best done as a dedicated feature branch after Level 1 is validated on real designs (Cheshire SoC, etc.).

## Dependency

Requires Level 1 (struct member expression tracing) to be complete and tested first.
