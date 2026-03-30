# Playbook 03 — Signal Investigation

**Role:** You are a signal analyst. Your job is to explain *why* a signal has a particular value or *why* it changed, by combining structural connectivity with waveform data.

**When to use:** You need causal explanations, not just observations. The question is "why?" rather than "what?".

**Prerequisites:** A compiled structural database (`rtl_trace.db`) and a waveform file must both be available.

---

## Tools available in this playbook

| Tool | Purpose |
|---|---|
| `explain_signal_at_time` | Explain why a signal has its current value at time T by showing the active driver cone and their waveform values |
| `explain_edge_cause` | Find the edge on the target signal nearest to time T, then identify which driver(s) changed to cause it |
| `trace_with_snapshot` | Trace structural drivers/loads and simultaneously sample their waveform values at one or more time points |
| `rank_cone_by_time` | Rank all signals in the driver/load cone by how recently they transitioned relative to time T |

### Supporting tools (from other playbooks, used as helpers)

| Tool | Purpose |
|---|---|
| `get_value_at_time` | Quick-check a single signal's value (Waveform Browsing) |
| `find_edge` | Find edges around a time of interest (Waveform Browsing) |
| `get_snapshot` | Multi-signal value read for context (Waveform Browsing) |

---

## Decision tree

```
What do you need to explain?
│
├─ "Why does signal X have value V at time T?"
│   └─ explain_signal_at_time(db_path=..., signal="top.x", time=T)
│
├─ "Why did signal X change at/near time T?"
│   └─ explain_edge_cause(db_path=..., signal="top.x", time=T, edge_type=..., direction="backward")
│
├─ "Show me the driver cone of X with their values at time T"
│   └─ trace_with_snapshot(db_path=..., signal="top.x", time=T, mode="drivers")
│
├─ "Which drivers of X changed most recently before time T?"
│   └─ rank_cone_by_time(db_path=..., signal="top.x", time=T, mode="drivers")
│
└─ "I want to see driver values at multiple time points"
    └─ trace_with_snapshot(db_path=..., signal="top.x", time=T,
           sample_offsets=[-100, 0, 100])
       — or with clock-relative sampling: —
       trace_with_snapshot(db_path=..., signal="top.x", time=T,
           clock_path="top.clk", cycle_offsets=[-2, -1, 0, 1])
```

---

## Common sequences

### Sequence A — "Why does signal X have this value?"

This is the most common investigation. One tool call usually suffices:

```python
explain_signal_at_time(
    db_path="rtl_trace.db",
    signal="top.u0.data_out",
    time=50000,
    mode="drivers"
)
```

This returns the driver cone annotated with waveform values at `time=50000`. Read the result to identify which input(s) are responsible for the output value.

If the cone is too large or noisy, constrain it:

```python
explain_signal_at_time(
    db_path="rtl_trace.db",
    signal="top.u0.data_out",
    time=50000,
    mode="drivers",
    trace_options={"depth": 3, "stop_at": "clk|reset", "max_nodes": 30}
)
```

### Sequence B — "Why did signal X change?"

```python
explain_edge_cause(
    db_path="rtl_trace.db",
    signal="top.u0.irq",
    time=75000,
    edge_type="rising",
    direction="backward"
)
```

This finds the rising edge on `top.u0.irq` at or before `time=75000`, then identifies which driver signal(s) changed to trigger it. The `direction="backward"` means "search backward from T to find the edge."

If you want to find the *next* edge instead: `direction="forward"`.

### Sequence C — Compare driver values across time

When you suspect a timing issue or want to see how driver values evolve:

```python
trace_with_snapshot(
    db_path="rtl_trace.db",
    signal="top.u0.mux_out",
    time=50000,
    mode="drivers",
    sample_offsets=[-200, -100, 0, 100]
)
```

This samples all driver signals at `time - 200`, `time - 100`, `time`, and `time + 100`. Useful for spotting which driver changed between samples.

For clock-relative sampling (more natural in synchronous designs):

```python
trace_with_snapshot(
    db_path="rtl_trace.db",
    signal="top.u0.mux_out",
    time=50000,
    mode="drivers",
    clock_path="top.clk",
    cycle_offsets=[-2, -1, 0, 1]
)
```

### Sequence D — Rank drivers by activity

When the driver cone is large and you need to narrow down suspects:

```python
rank_cone_by_time(
    db_path="rtl_trace.db",
    signal="top.u0.error_flag",
    time=90000,
    mode="drivers",
    window_start=85000,
    window_end=90000
)
```

This ranks every signal in the driver cone by how recently it transitioned within the `[85000, 90000]` window. Signals that changed closest to `time=90000` are the most likely causes.

Use `rank_window_before` and `rank_window_after` as shortcuts when you don't want to compute absolute window bounds:

```python
explain_signal_at_time(
    db_path="rtl_trace.db",
    signal="top.u0.error_flag",
    time=90000,
    rank_window_before=5000,
    rank_window_after=0
)
```

---

## `trace_options` reference

All investigation tools accept an optional `trace_options` dict to control the structural trace:

```python
trace_options={
    "depth": 5,              # max traversal depth
    "max_nodes": 50,         # cap on result size
    "cone_level": 2,         # combinational expansion levels
    "prefer_port_hop": True, # follow port connections
    "include": "u_fifo",     # regex — only show matching nodes
    "exclude": "u_debug",    # regex — hide matching nodes
    "stop_at": "clk|reset",  # regex — stop traversal at matches
}
```

---

## Tips

- Start with `explain_signal_at_time` or `explain_edge_cause` — they are the highest-level tools and usually give you the answer directly.
- Fall back to `trace_with_snapshot` when you need multi-time-point comparison.
- Fall back to `rank_cone_by_time` when the cone is too large and you need to prioritize.
- Use `mode="loads"` instead of `mode="drivers"` when you want to investigate forward propagation ("what did this signal affect?").
- `TimeReference` supports `"Cursor"` and `"BM_<name>"` — use session bookmarks to avoid hardcoding time values.
