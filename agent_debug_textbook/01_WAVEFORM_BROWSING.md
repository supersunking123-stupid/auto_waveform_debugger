# Playbook 01 — Waveform Browsing

**Role:** You are an observer. Your job is to read waveform data — values, transitions, patterns, intervals — without reasoning about RTL structure.

**When to use:** You need to answer questions like "what is happening in the waveform?" rather than "why is it happening?"

---

## Tools available in this playbook

| Tool | Purpose |
|---|---|
| `list_signals` | Discover what signals exist in the waveform; filter by hierarchy pattern and/or signal type to avoid context explosion on large designs |
| `get_signal_info` | Get metadata about a signal (width, type, scope) |
| `get_snapshot` | Read multiple signal values at one point in time |
| `get_value_at_time` | Read a single signal's value at one point in time |
| `find_edge` | Find the next/previous edge on a signal |
| `find_value_intervals` | Find all time intervals where a signal holds a specific value |
| `find_condition` | Find the next/previous time a Boolean expression is true |
| `get_transitions` | List all value changes on a signal in a time range |
| `get_signal_overview` | Get a downsampled summary of a signal across a time range |
| `analyze_pattern` | Detect repeating patterns (e.g., clock periods, duty cycles) |

---

## Decision tree

```
What do you need to know?
│
├─ "What signals exist?"
│   ├─ Top-level only (safe default)
│   │   └─ list_signals()                              ← pattern="" returns top module only
│   ├─ Narrow by hierarchy
│   │   └─ list_signals(pattern="top.u_fifo.*")        ← glob prefix
│   │   └─ list_signals(pattern="regex:.*_valid$")     ← regex
│   ├─ Narrow by signal type
│   │   └─ list_signals(pattern="top.u_axi.*", types=["input","output"])
│   └─ Full namespace (⚠ large designs: do NOT use without a pattern)
│       └─ list_signals(pattern="*")
│
├─ "What is signal X? (width, type)"
│   └─ get_signal_info(path="top.x")
│
├─ "What are the values right now / at time T?"
│   ├─ One signal   → get_value_at_time(path="top.x", time=T)
│   └─ Many signals → get_snapshot(signals=["top.a","top.b"], time=T)
│
├─ "When does signal X change?"
│   ├─ Next/previous edge → find_edge(path="top.x", edge_type=..., start_time=T, direction=...)
│   └─ All changes in range → get_transitions(path="top.x", start_time=T1, end_time=T2)
│
├─ "When does signal X equal value V?"
│   └─ find_value_intervals(path="top.x", value="V", start_time=T1, end_time=T2)
│
├─ "When is this condition true?"
│   └─ find_condition(expression="top.a == 1 && top.b == 0", start_time=T)
│
├─ "Give me a high-level view of signal X over a range"
│   └─ get_signal_overview(path="top.x", start_time=T1, end_time=T2, resolution="auto")
│
└─ "Is signal X periodic? What is its frequency?"
    └─ analyze_pattern(path="top.x", start_time=T1, end_time=T2)
```

---

## Common sequences

### Sequence A — Explore an unfamiliar waveform

1. `list_signals()` — see the top-level signals (safe starting point; does not dump the full namespace).
2. **Check the time precision before using any time argument** — call `get_signal_info` on any signal and read the reported timescale. Every time value you pass to any tool must be in units of the precision (the second number of the `` `timescale `` directive). See Rule 12 in `rtl_debug_guide.md` for the full conversion table.
   ```python
   get_signal_info(path="top.clk")   # read the timescale field from the result
   ```
3. Drill into the module of interest: `list_signals(pattern="top.u_interesting.*")` — see only that subtree.
4. Narrow further if still too many results: add `types=["input","output"]` to see only interface signals.
5. `get_signal_info(path="top.u_interesting.sig")` — check width/type of a specific signal.
6. `get_signal_overview(path="top.u_interesting.sig", start_time=0, end_time=100000, resolution="auto")` — get the big picture. (Use time values already converted to the correct precision.)
7. Zoom into interesting regions with `get_transitions(...)` or `get_snapshot(...)`.

### Sequence B — Check a bus at a specific time

1. `get_snapshot(signals=["top.addr","top.data","top.wen","top.ren"], time=50000, radix="hex")` — see all related signals at once.
2. If something looks wrong, use `find_edge(path="top.wen", edge_type="rising", start_time=50000, direction="backward")` to find when the write enable last asserted.

### Sequence C — Find when a condition occurs

1. `find_condition(expression="top.state == 3 && top.valid == 1", start_time=0, direction="forward")` — find the first occurrence.
2. `get_snapshot(signals=["top.state","top.valid","top.data"], time=<result>)` — see the full picture at that moment.
3. Repeat `find_condition(start_time=<result+1>)` to find subsequent occurrences.

### Sequence D — Characterize a clock or periodic signal

1. `analyze_pattern(path="top.clk", start_time=0, end_time=100000)` — detect period, duty cycle.
2. If you need exact edges: `get_transitions(path="top.clk", start_time=T1, end_time=T2, max_limit=50)`.

---

## Tips

- **Always check the waveform time precision with `get_signal_info` before your first time-based query.** Waveform timestamps are in units of the `` `timescale `` precision (the second number). The most common setup is `` `timescale 1ns/1ps ``, which means precision is 1 ps — passing `100` when you mean 100 ns will query time 100 ps instead, 1000× too early. See Rule 12 in `rtl_debug_guide.md`.
- Use `radix="hex"` for buses, `radix="bin"` for control signals where individual bits matter, `radix="dec"` for counters or arithmetic.
- `get_snapshot` with a signal group is efficient: create a group via Session Management, then call `get_snapshot(signals=["MyGroup"], signals_are_groups=True)`.
- `find_condition` accepts compound Boolean expressions — use it instead of chaining multiple `find_edge` calls.
- `max_limit` in `get_transitions` defaults to 50. Increase it only if you truly need more; large results are harder to reason about.
- Prefer `get_signal_overview` over `get_transitions` when you need a big-picture view of a long time range.

### `list_signals` — avoiding context explosion

On real chip waveforms (FSDB from gate-level or large RTL sims), the full signal namespace can contain tens of thousands of entries. Dumping it unfiltered will fill your context window and leave no room for analysis.

**Rules:**
- **Never call `list_signals(pattern="*")` without a narrowing `pattern` on a large or unknown design.** If you don't yet know the size, start with the default (`pattern=""`) and judge the result count before widening.
- **Use `pattern` to scope to a module subtree** before listing. Example: `list_signals(pattern="top.u_dma.*")` returns only signals under `u_dma`.
- **Use `types` to filter to interface signals** when you only need to know what a module exposes. Example: `types=["input","output"]` shows only ports, skipping all internal nets and registers.
- **Combine both** for maximum precision: `list_signals(pattern="top.u_axi.*", types=["output","net"])`.
- `pattern="regex:<expr>"` accepts a raw regular expression for cases where glob prefix is too coarse. Example: `pattern="regex:.*_valid$"` finds all `*_valid` signals anywhere in the hierarchy.
