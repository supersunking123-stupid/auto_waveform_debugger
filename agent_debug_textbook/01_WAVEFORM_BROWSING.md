# Playbook 01 — Waveform Browsing

**Role:** You are an observer. Your job is to read waveform data — values, transitions, patterns, intervals — without reasoning about RTL structure.

**When to use:** You need to answer questions like "what is happening in the waveform?" rather than "why is it happening?"

---

## Tools available in this playbook

| Tool | Purpose |
|---|---|
| `list_signals` | Discover what signals exist in the waveform |
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
│   └─ list_signals(vcd_path=...)
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

1. `list_signals(vcd_path="wave.fsdb")` — see what's available.
2. Pick signals of interest.
3. `get_signal_info(path="top.interesting_sig")` — check width/type.
4. `get_signal_overview(path="top.interesting_sig", start_time=0, end_time=100000, resolution="auto")` — get the big picture.
5. Zoom into interesting regions with `get_transitions(...)` or `get_snapshot(...)`.

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

- Use `radix="hex"` for buses, `radix="bin"` for control signals where individual bits matter, `radix="dec"` for counters or arithmetic.
- `get_snapshot` with a signal group is efficient: create a group via Session Management, then call `get_snapshot(signals=["MyGroup"], signals_are_groups=True)`.
- `find_condition` accepts compound Boolean expressions — use it instead of chaining multiple `find_edge` calls.
- `max_limit` in `get_transitions` defaults to 50. Increase it only if you truly need more; large results are harder to reason about.
- Prefer `get_signal_overview` over `get_transitions` when you need a big-picture view of a long time range.
