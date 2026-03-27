# agent_debug_automation Tech Note

## Purpose

`agent_debug_automation` is the orchestration layer that combines:
- structural RTL tracing from `standalone_trace` / `rtl_trace`
- time-based waveform queries from `waveform_explorer` / `wave_agent_cli`
- MCP tool exposure through `FastMCP`

It does not replace either backend. It composes them.

Main file:
- `agent_debug_automation/agent_debug_automation_mcp.py`

## Design overview

The module has three layers:

1. Backend process/session wrappers
- `RtlTraceServeSession`
- `WaveformDaemon`

2. Internal orchestration helpers
- trace JSON retrieval
- waveform querying
- signal mapping
- cone extraction
- ranking and explanation assembly

3. MCP tool functions
- low-level passthrough tools
- high-level cross-link tools

The most important concept is reuse of long-lived backend processes. Without that, the cross-link tools would pay repeated startup cost.

## Important module-level state

### Binary resolution

- `ROOT_DIR`
- `DEFAULT_RTL_TRACE_BIN`
- `DEFAULT_WAVE_CLI`

These define the default backend binary locations.

### Default timing/ranking constants

- `DEFAULT_RANK_WINDOW_BEFORE`
- `DEFAULT_RANK_WINDOW_AFTER`
- `DEFAULT_EXPLAIN_WINDOW_AFTER`
- `DEFAULT_TRANSITION_LIMIT`

These shape the default behavior of the higher-level tools.

### Session and cache dictionaries

- `rtl_serve_sessions`
- `rtl_session_ids_by_key`
- `wave_daemons`
- `wave_signal_cache`
- `wave_signal_resolution_cache`
- `wave_prefix_page_cache`

These caches are critical to performance.

## Important classes

### `RtlTraceServeSession`

Purpose:
- spawn `rtl_trace serve`
- send line-oriented commands
- read responses until `<<END>>`

Important methods:
- `query(...)`
- `stop(...)`

Important detail:
- startup also consumes the initial serve banner/response correctly, so later requests stay aligned.

### `WaveformDaemon`

Purpose:
- spawn `wave_agent_cli <waveform>` once
- keep it alive as a JSON request daemon

Important methods:
- `query(...)`
- `stop(...)`

The protocol is one JSON command per line, one JSON response per line.

## Important helper functions

### Backend command helpers

- `_resolve_bin(...)`
  - selects explicit override, repo-local built binary, or fallback executable name

- `_run_cmd(...)`
  - one-shot subprocess wrapper for low-level passthrough tools

- `_get_rtl_serve_session(...)`
  - returns a cached `rtl_trace serve` session per `(bin_path, db_path)`

- `_get_wave_daemon(...)`
  - returns a cached waveform daemon per waveform path

- `_wave_query(...)`
  - thin JSON query helper over the daemon

### Structural trace helpers

- `_append_trace_options(...)`
  - converts Python dict options into `rtl_trace trace` flags

- `_rtl_trace_json(...)`
  - issues `trace --format json` through a persistent `rtl_trace serve` session
  - parses the returned JSON and attaches `raw_command`

### Waveform path mapping helpers

- `_normalize_top_variants(...)`
- `_strip_bit_suffix(...)`
- `_signal_parent_prefixes(...)`
- `_wave_get_signal_info(...)`
- `_get_fsdb_signals_by_prefix(...)`
- `_map_signal_to_waveform(...)`

This group is the key glue between structural paths and waveform paths.

Behavior:
- try exact path match and `TOP.` variants first
- for FSDB, if exact lookup fails, page only the relevant hierarchy prefixes using `list_signals_page`
- support bit-select to bus fallback by comparing base signal names without the bracket suffix
- for VCD/FST, fall back to the full `list_signals` path cache

This avoids enumerating the entire FSDB namespace for common cases.

### Cone and sampling helpers

- `_collect_cone_signals(...)`
  - collects the target, each endpoint path, and all endpoint `lhs`/`rhs` signals from `rtl_trace` JSON

- `_window_bounds(...)`
  - computes the local time window around `T`

- `_get_batch_snapshot(...)`
  - runs one waveform snapshot query for multiple mapped signals

- `_get_transitions_window(...)`
  - fetches local transition history around `T`

- `_sample_signal_times(...)`
  - samples a group of cone signals at arbitrary timestamps and records unmapped signals

### Clock-relative helpers

- `_step_clock_edge(...)`
- `_cycle_sample_times(...)`

These are used by `trace_with_snapshot(...)` when cycle-relative sampling is requested.

Behavior:
- resolve the nearest base posedge at or before the focus time
- step forward or backward one clock edge at a time for each cycle offset

### Ranking helpers

- `_summarize_signal(...)`
- `_build_signal_summaries(...)`
- `_rank_signal_summaries(...)`

These functions convert raw waveform evidence into ranked cone summaries.

Per-signal summary fields include:
- mapped path and sampled values
- `recent_toggle_count`
- `last_transition_time`
- `closest_transition_time`
- `closest_transition_distance`
- `closest_transition_direction`
- `preferred_transition_side`
- `changed_at_time`
- `is_constant_in_window`
- `stuck_class`
- `activity_score`
- `closeness_score`
- `stuck_score`
- `total_score`

Current ranking policy:
- closeness to `T` is the strongest term
- `drivers` prefers transitions at or just before `T`
- `loads` prefers transitions at or just after `T`
- stuck signals are classified as:
  - `stuck_to_1`
  - `stuck_to_0`
  - `stuck_other`
- `stuck_to_1` scores higher than `stuck_to_0`

This is heuristic ranking, not formal Boolean causality.

### Explanation helper

- `_build_explanations(...)`

Purpose:
- combine `rtl_trace` endpoint structure with waveform ranking evidence
- produce ranked candidate endpoint paths
- attach per-RHS-term evidence where waveform mapping exists

Important behavior:
- score each endpoint from endpoint-local waveform evidence plus the best RHS term score
- retain full candidate lists instead of collapsing to a single sentence
- lower confidence when assignment structure or waveform mapping is weak

## Common orchestration flow

The core shared pipeline is `_build_common_context(...)`.

Working flow:
- run structural trace JSON through `_rtl_trace_json(...)`
- derive a focus window around `T`
- extract cone signals with `_collect_cone_signals(...)`
- map those structural signals into waveform paths
- query snapshots and transitions in the local window
- build signal summaries and ranking views
- return a structured context object with:
  - `target`
  - `time_context`
  - `structure`
  - `waveform`
  - `ranking`
  - `unmapped_signals`
  - `warnings`

All higher-level cross-link tools build on this object.

## MCP-exposed tool groups

### Low-level passthrough tools

These preserve the original backend command models.

`rtl_trace` side:
- `rtl_trace(...)`
- `rtl_trace_serve_start(...)`
- `rtl_trace_serve_query(...)`
- `rtl_trace_serve_stop(...)`

`wave_agent_cli` side:
- `wave_agent_query(...)`
- `list_signals(...)`
- `get_signal_info(...)`
- `get_snapshot(...)`
- `get_value_at_time(...)`
- `find_edge(...)`
- `find_condition(...)`
- `get_transitions(...)`
- `analyze_pattern(...)`

`get_snapshot(...)` and `get_value_at_time(...)` accept an optional `radix` for multi-bit stable values:
- `hex` default
- `bin`
- `dec`

### High-level cross-link tools

#### `trace_with_snapshot(...)`

Purpose:
- structural trace plus sampled cone values in one call

Working flow:
- build common context
- optionally sample extra absolute offsets
- optionally resolve cycle-relative times and sample them
- return full structure plus waveform samples

#### `explain_signal_at_time(...)`

Purpose:
- explain current structural candidates for a signal state at time `T`

Working flow:
- build common context
- turn signal summaries into a ranking map
- build endpoint/RHS explanations
- attach `explanations` to the returned context

#### `rank_cone_by_time(...)`

Purpose:
- return a cone ranking view without the explanation layer

Working flow:
- run structural trace
- build waveform summaries over the requested window
- return ranking slices:
  - `all_signals`
  - `most_active_near_time`
  - `most_stuck_in_window`
  - `unchanged_candidates`

#### `explain_edge_cause(...)`

Purpose:
- resolve a relevant edge, then explain the upstream cause chain at that edge

Working flow:
- map the target signal into the waveform
- use waveform `find_edge` to resolve the actual edge time
- call `explain_signal_at_time(...)` at the resolved edge time
- add `edge_context` with the value before and at the edge

Important behavior:
- backward search relies on waveform-side exact-edge semantics, so an edge exactly at `T` is preserved

## Response shape

The high-level tools intentionally return structured evidence, not free-form prose.

Common top-level sections:
- `status`
- `target`
- `time_context`
- `structure`
- `waveform`
- `ranking`
- `explanations`
- `unmapped_signals`
- `warnings`

This response shape is designed so downstream agents can consume the data programmatically.

## Where to start if you need to change behavior

- session/process management:
  - `RtlTraceServeSession`
  - `WaveformDaemon`

- structural command generation:
  - `_append_trace_options(...)`
  - `_rtl_trace_json(...)`

- signal mapping between DB and waveform namespaces:
  - `_map_signal_to_waveform(...)`

- ranking behavior:
  - `_summarize_signal(...)`
  - `_rank_signal_summaries(...)`

- explanation behavior:
  - `_build_explanations(...)`

- cross-link response content:
  - `_build_common_context(...)`
  - the four high-level MCP tool functions

## Practical mental model

Think of `agent_debug_automation` as a coordinator with caches:
- `rtl_trace` supplies the static cone
- `wave_agent_cli` supplies the temporal evidence
- this module aligns names, samples the right times, ranks candidates, and returns one combined JSON object

It is not a new trace engine and not a new waveform engine. It is the composition layer.
