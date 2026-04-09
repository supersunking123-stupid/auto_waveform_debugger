# Playbook 07 — Virtual Signals

**Role:** You are a signal synthesizer. Your job is to derive new observable signals — compound conditions, reassembled buses, filtered handshakes — from existing waveform signals, so that browsing and investigation tools can operate on them directly.

**When to use:** You need to observe or search on a signal that does not exist in the waveform directly. The target is a logical combination of real signals (e.g., a handshake, an error condition, a bus slice), or you need to view bus bits in a different arrangement.

---

## Tools available in this playbook

### Expression-based creation

| Tool | Purpose |
|---|---|
| `create_signal_expression` | Create a virtual signal by a Verilog-like expression over real or virtual signals |
| `update_signal_expression` | Change the expression or description of an existing virtual signal |
| `delete_signal_expression` | Remove a virtual signal |
| `list_signal_expressions` | List all virtual signals in the active session |

### Bus construction helpers

| Tool | Purpose |
|---|---|
| `create_bus_concat` | Concatenate signals MSB-first (first in list = MSB) |
| `create_bus_slice` | Extract a named `[msb:lsb]` sub-range from a signal (inclusive) |
| `create_bus_slices` | Slice a bus into equal-width pieces; produces MSB-first range-suffixed names |
| `create_reversed_bus` | Reverse bit order without changing width |

### All browsing tools (work transparently on virtual signals)

Once created, a virtual signal behaves like a real signal in:
`get_transitions`, `get_value_at_time`, `find_edge`, `get_snapshot`, `find_value_intervals`, `count_transitions`, `get_signal_overview`, `analyze_pattern`

---

## Key concepts

**Virtual signals are session-persistent.**  They are stored in the active session and survive across tool calls in the same conversation. Listing them with `list_signal_expressions` shows all created signals, including bus-construction results.

**Evaluation is event-driven and cached.**  A virtual signal is computed only at timestamps where at least one of its operands changes. The result is cached; subsequent queries for the same virtual signal reuse the cache. Cache is automatically invalidated when the expression or any dependency is updated or deleted.

**Chaining is supported.**  A virtual signal may depend on another virtual signal, up to 16 levels deep. Circular dependencies are rejected at creation time.

**Structural trace tools do not support virtual signal names.**  `trace_with_snapshot`, `explain_signal_at_time`, `rank_cone_by_time`, and `explain_edge_cause` require a real signal path. Do not pass a virtual signal name to these tools. The correct workflow is: use virtual signals for *observation*, then trace the *real* signals that contribute to the condition.

**In Root-Cause Analysis, virtual signals are the default optimization for repeated protocol events.**  If you will search the same handshake, error predicate, or burst-complete condition more than once, create it once here instead of repeating the same `find_condition` expression throughout Playbook 04.

---

## Expression operator reference

`create_signal_expression` supports standard Verilog operators and literal formats:

| Category | Operators | Notes |
|----------|-----------|-------|
| Unary bitwise | `~` `!` | `!` treats non-zero as true |
| Unary reduction | `&` `\|` `^` `~&` `~\|` `~^` `^~` | Standard Verilog reduction (prefix on a bus) |
| Bitwise binary | `&` `\|` `^` | Standard Verilog |
| Custom binary | `~&` `~\|` `~^` | Extension: `a ~& b` = `~(a & b)` |
| Logical | `&&` `\|\|` | Vector truthiness: non-zero = true |
| Equality | `==` `!=` `===` `!==` | `===`/`!==` are 4-state identity (x === x is true) |
| Relational | `<` `>` `<=` `>=` | Returns 1-bit |
| Shift | `<<` `>>` `<<<` `>>>` | `>>>` uses signedness of left operand |
| Arithmetic | `+` `-` `*` `/` `%` `**` | Division by zero returns x |
| Ternary | `? :` | X-merge per IEEE 1364 when condition is x/z |
| Assignment in ternary | `=` | Extension: `cond ? a = b : c` treated as `cond ? b : c` |

**Literal formats:** `0`, `1`, `4'b1010`, `32'hFF`, `8'd255`, `'x`, `'z`.

**Examples beyond simple Boolean:**

```python
# Arithmetic: compute a derived address
create_signal_expression(signal_name="word_addr", expression="top.byte_addr >> 2")

# Ternary: select between two data sources
create_signal_expression(signal_name="selected_data",
                         expression="(top.sel == 1) ? top.data_a : top.data_b")

# Reduction: check if any bit in a status bus is set
create_signal_expression(signal_name="any_error", expression="|top.error_bits")

# Comparison with constant
create_signal_expression(signal_name="is_write_to_target",
                         expression="cmd_opcode == 4'd5 && cmd_addr >= 32'h80000000")
```

---

## Decision tree

```
What are you trying to create?
│
├─ "I want to observe when a multi-signal condition is true"
│   (e.g., a handshake, an error flag, a protocol event)
│   └─ create_signal_expression(signal_name="...", expression="sig_a & sig_b")
│
├─ "I want to extract a sub-range of bits from a bus"
│   └─ create_bus_slice(signal_name="...", source_signal="top.bus", msb=7, lsb=4)
│
├─ "I want to slice a wide bus into equal-width chunks for parallel inspection"
│   └─ create_bus_slices(signal_name_prefix="data", source_signal="top.data", slice_width=32)
│       → produces data_127_96, data_95_64, data_63_32, data_31_0
│
├─ "I want to assemble a bus from multiple signals (Verilog concat order)"
│   └─ create_bus_concat(signal_name="...", source_signals=["msb_sig", "lsb_sig"])
│       → first entry becomes the MSB side
│
├─ "I want to view a bus with reversed bit order"
│   └─ create_reversed_bus(signal_name="...", source_signal="top.bus")
│
└─ "I want a derived signal that depends on another virtual signal"
    └─ create_signal_expression(signal_name="v2", expression="v1 | top.c")
        (where v1 is also a virtual signal — chaining is allowed)
```

---

## Common sequences

### Sequence A — Create a handshake signal and find its edges

This is the most common use case. AXI, AHB, and most on-chip protocols use a valid/ready (or req/ack) handshake. Creating a virtual signal for the handshake lets you use `find_edge` and `get_transitions` on a single signal instead of chaining multiple conditional queries.

```python
# Step 1 — create the handshake signal
create_signal_expression(
    signal_name="aw_sent",
    expression="top.awvalid & top.awready",
    description="AW channel handshake pulse"
)

# Step 2 — find the first handshake after time 0
find_edge(path="aw_sent", edge_type="posedge", start_time=0, direction="forward")

# Step 3 — count handshakes in a window
count_transitions(path="aw_sent", start_time=0, end_time=500000, edge_type="posedge")

# Step 4 — list all handshake moments
get_transitions(path="aw_sent", start_time=0, end_time=500000)
```

Compare: without the virtual signal, the same search requires `find_condition(expression="top.awvalid & top.awready == 1", ...)` repeated for each occurrence — and `find_condition` has no `count` equivalent.

---

### Sequence B — Create an error condition signal, then search and snapshot it

When a bug only occurs when several conditions are simultaneously true, a virtual signal reduces repetitive expressions throughout the investigation.

```python
# Step 1 — define the error condition
create_signal_expression(
    signal_name="fifo_write_while_full",
    expression="top.u_fifo.wr_en & top.u_fifo.full",
    description="Illegal write to full FIFO"
)

# Step 2 — scan for the first occurrence
find_edge(path="fifo_write_while_full", edge_type="posedge", start_time=0, direction="forward")
# → first_occurrence_time = 312000

# Step 3 — bookmark and snapshot
create_bookmark(bookmark_name="first_illegal_write", time=312000,
                description="First fifo_write_while_full pulse")
get_snapshot(
    signals=["top.u_fifo.wr_en", "top.u_fifo.full", "top.u_fifo.count",
             "top.u_fifo.wr_ptr", "fifo_write_while_full"],
    time=312000
)

# Step 4 — see how often this condition occurred
count_transitions(path="fifo_write_while_full", start_time=0, end_time=1000000,
                  edge_type="posedge")

# Step 5 — look at the pattern over time
get_signal_overview(path="fifo_write_while_full", start_time=0, end_time=1000000,
                    resolution="auto")
```

---

### Sequence C — Bus manipulation for display and comparison

Use bus helpers when the waveform contains signals that need to be rearranged before they can be compared to a spec or log. This is common with byte-swapped buses, packed structs, and multi-source concatenations.

```python
# Extract the address field from a packed command bus [63:32]
create_bus_slice(
    signal_name="cmd_addr",
    source_signal="top.cmd_bus",
    msb=63,
    lsb=32,
    description="Address field of cmd_bus[63:32]"
)

# Extract the opcode field [31:28]
create_bus_slice(
    signal_name="cmd_opcode",
    source_signal="top.cmd_bus",
    msb=31,
    lsb=28,
    description="Opcode field of cmd_bus[31:28]"
)

# Now query them independently
get_value_at_time(path="cmd_addr", time="Cursor", radix="hex")
get_value_at_time(path="cmd_opcode", time="Cursor", radix="hex")
find_value_intervals(path="cmd_opcode", value="5", start_time=0, end_time=1000000, radix="dec")
```

For a wide bus that needs to be split into equal-width lanes:

```python
# Slice a 128-bit data bus into four 32-bit lanes
create_bus_slices(
    signal_name_prefix="data",
    source_signal="top.wdata",
    slice_width=32,
    description="32-bit lanes of wdata[127:0]"
)
# Creates: data_127_96, data_95_64, data_63_32, data_31_0

get_snapshot(
    signals=["data_127_96", "data_95_64", "data_63_32", "data_31_0"],
    time="Cursor",
    radix="hex"
)
```

For a byte-swapped bus:

```python
# The waveform logs bytes in reversed order; reverse to match the spec
create_reversed_bus(
    signal_name="wdata_byteswap",
    source_signal="top.wdata",
    description="Byte-reversed view of wdata for spec comparison"
)
get_value_at_time(path="wdata_byteswap", time="Cursor", radix="hex")
```

---

### Sequence D — Chained virtual signals

Use chaining when a derived condition depends on another derived condition you already defined.

```python
# Level 1: individual channel handshakes
create_signal_expression(signal_name="aw_sent",
                         expression="top.awvalid & top.awready")
create_signal_expression(signal_name="ar_sent",
                         expression="top.arvalid & top.arready")

# Level 2: either channel fired (depends on two level-1 signals)
create_signal_expression(
    signal_name="any_addr_sent",
    expression="aw_sent | ar_sent",
    description="Any AXI address-channel handshake"
)

# Use level-2 signal directly in browsing tools
count_transitions(path="any_addr_sent", start_time=0, end_time=1000000,
                  edge_type="posedge")
```

Maximum chain depth is 16 levels. Circular dependencies (e.g., `v1 = v1 | top.c`) are rejected at creation time.

---

### Sequence E — Lifecycle management

```python
# See all virtual signals in the active session
list_signal_expressions()

# Fix a typo in an expression
update_signal_expression(
    signal_name="aw_sent",
    expression="top.u_axi.awvalid & top.u_axi.awready"
)

# Remove a signal that is no longer needed
# (also invalidates any virtual signal that depends on it)
delete_signal_expression(signal_name="aw_sent")
```

---

## When NOT to use virtual signals

| Situation | Better tool |
|---|---|
| One-off condition search with no repeated use | `find_condition(expression="...", ...)` directly |
| You need structural trace on the resulting condition | Trace the *real* signals that feed the condition; virtual signal names are not accepted by `trace_with_snapshot`, `explain_signal_at_time`, `rank_cone_by_time`, or `explain_edge_cause` |
| Viewing a sub-range of bits just once | Use `radix="bin"` and count bits manually, or `get_value_at_time` with `radix="hex"` and mask by hand |

---

## Tips

- **Name virtual signals concisely and descriptively.** They appear in snapshot outputs alongside real signal paths. `aw_sent` is better than `virtual_awvalid_and_awready_handshake_signal`.
- **Add descriptions at creation time.** `list_signal_expressions()` returns them; a one-line description prevents confusion after hours of debugging.
- **Prefer `create_signal_expression` over chaining `find_condition`.** For any condition you will query more than twice, create a virtual signal — it is faster (cached), composable (chainable), and usable with a wider set of tools.
- **Bus slices can themselves be combined in expressions.** For example: after creating `cmd_addr` and `cmd_opcode`, you can write `create_signal_expression(signal_name="is_write_to_target", expression="cmd_opcode == 5 && cmd_addr >= 32'h80000000")`.
- **When a virtual signal depends on a real signal path you are not sure is correct**, check the path first with `list_signals(pattern="...")` before creating the expression — a typo in a signal path will cause the expression to evaluate as `x` everywhere.
