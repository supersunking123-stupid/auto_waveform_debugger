# waveform_explorer Tech Note

## Purpose

`waveform_explorer` exposes waveform data as structured JSON queries instead of GUI interactions or raw dumps. The implementation is split into three layers:
- `WaveDatabase`: waveform loading and value storage/access
- `AgentAPI`: JSON-level query semantics
- `main.cpp`: CLI/daemon dispatch

Supported backends:
- VCD
- FST
- FSDB

The important design distinction is that FSDB is now handled lazily, while VCD and FST still preload transitions.

## Main source files

- `src/WaveDatabase.h`
- `src/WaveDatabase.cpp`
- `src/AgentAPI.h`
- `src/AgentAPI.cpp`
- `src/main.cpp`

## Important data types

### `Transition`

Represents one value change event.
- `timestamp`
- `value`
- `is_glitch`

This is the core time-series unit used by higher-level queries.

### `SignalInfo`

Metadata per signal path.
- `name`
- `path`
- `width`
- `type`
- `signal_id`

`signal_id` is backend-specific:
- VCD: identifier code from the VCD file
- FST: handle converted to string
- FSDB: Verdi idcode converted to string

### `WaveDatabase`

This is the main backend abstraction.

Core responsibilities:
- detect waveform type
- load metadata and transitions
- normalize/resolve signal paths
- provide signal info, transition history, and value-at-time lookup

Key internal state:
- `timescale`
- `backend_kind`
- `signal_info`
- `id_transitions`
- `sorted_signal_paths_cache`
- FSDB-only state:
  - `fsdb_obj`
  - `fsdb_loaded_signal_ids`

### `AgentAPI`

Thin JSON-oriented API layer over `WaveDatabase`.

It implements the query semantics for:
- `get_signal_info`
- `list_signals_page`
- `get_snapshot`
- `get_value_at_time`
- `find_edge`
- `find_condition`
- `get_transitions`
- `get_signal_overview`
- `analyze_pattern`

## Major working flows

### 1. Process startup and dispatch

Main entry: `src/main.cpp`

Working flow:
- Construct `StdoutLineFilter` to suppress noisy FSDB SDK banner lines.
- Load the waveform once into `WaveDatabase`.
- Construct `AgentAPI api(db)`.
- Enter either:
  - one-shot mode: `wave_agent_cli <waveform> '<json_query>'`
  - daemon mode: read one JSON query per stdin line
- Dispatch by `cmd` string and print one JSON response per request.

The process model is intentionally simple: one process owns one loaded waveform.

### 2. Waveform loading

Main entry: `WaveDatabase::load(...)`

Backend selection:
- `.fst` -> `load_fst(...)`
- `.fsdb` -> `load_fsdb(...)`
- otherwise -> `load_vcd(...)`

#### VCD path

`load_vcd(...)`:
- parse timescale, scope stack, and `$var` declarations
- build `SignalInfo` map keyed by normalized full path
- read every value change into `id_transitions`
- track same-timestamp updates as glitches

This is a full eager load.

#### FST path

`load_fst(...)`:
- iterate hierarchy to build signal metadata
- assign one `signal_id` per FST handle
- iterate blocks and push transitions into `id_transitions`

This is also a full eager load.

#### FSDB path

`load_fsdb(...)`:
- verify the file with `ffrObject::ffrIsFSDB(...)`
- open once with `ffrOpen3(...)`
- read timescale
- set the view window to the file's min/max timestamps
- traverse the FSDB scope/var tree via callback
- populate only `signal_info`
- keep the FSDB object alive

This is metadata-only at load time. Transitions are not loaded yet.

That FSDB design is the main scalability decision in the current code.

### 3. Path normalization and lookup

Relevant functions:
- `normalize_loaded_path(...)`
- `resolve_query_path(...)`
- `strip_top_prefix(...)`

Behavior:
- loaded paths are normalized by stripping a leading `TOP.`
- query-time lookup accepts both styles where possible
- for packed FSDB signals, a bracket-free base name can resolve to a uniquely matching dumped path
  - example: `top.u_cq.cq_rd_count9` -> `top.u_cq.cq_rd_count9[8:0]`
- this keeps client behavior tolerant across waveform sources

Implementation detail:
- `rebuild_base_signal_path_cache(...)` builds a base-path to packed-path map after load
- ambiguous base names are excluded from fallback resolution on purpose

### 4. Lazy transition loading for FSDB

Relevant functions:
- `ensure_signal_transitions_loaded(...)`
- `ensure_fsdb_signal_loaded(...)`

Working flow for FSDB queries:
- resolve the requested path to a `SignalInfo`
- check whether `signal_id` already has cached transitions
- if not, use Verdi FSDB APIs to:
  - reset signal list
  - add the one target signal idcode
  - load only that signal
  - traverse all value changes with a `ffrVCTrvsHdl`
  - decode values into strings
  - build `Transition` vector
  - unload the signal again
- cache transitions in `id_transitions`

Important implication:
- metadata queries are cheap
- first transition query on a signal pays the load cost once
- repeated queries on the same signal are fast
- packed-vector fallback resolution happens before this stage; lazy loading still works on the resolved signal id

### 5. Value-at-time lookup

Main function: `WaveDatabase::get_value_at_time(...)`

Working flow:
- resolve path
- ensure transitions are loaded
- use `std::upper_bound` over sorted transitions
- return the latest value at or before `time`

This is the base operation reused by snapshots and edge checks.

### 6. Paged signal listing for FSDB

Relevant functions:
- `WaveDatabase::list_signal_paths_page(...)`
- `AgentAPI::list_signals_page(...)`

Purpose:
- avoid emitting a huge one-shot JSON array for large FSDB designs
- support prefix-filtered iteration using sorted signal paths plus cursor pagination

Important constraint:
- `list_signals_page` is FSDB-only by design
- `list_signals` remains the old full dump path

### 7. JSON query semantics in `AgentAPI`

#### `get_signal_info`
- validates the path
- returns metadata plus waveform timescale

#### `get_snapshot`
- bulk point query over multiple paths at one timestamp
- scalar responses are normalized to `0`, `1`, `x`, `z`, `rising`, or `falling`
- stable multi-bit values are displayed in hex by default
- bus responses return `changing` when a transition occurs exactly at the queried timestamp
- optional `radix` overrides for multi-bit stable values: `hex`, `bin`, `dec`

#### `get_value_at_time`
- single-signal point query
- uses the same simplified output rules as `get_snapshot`

#### `get_transitions`
- bounded window query over transition history
- returns a truncated flag if `max_limit` is hit

#### `get_signal_overview`
- builds a resolution-aware summary of one signal over `[start_time, end_time]`
- uses raw transitions plus width metadata, not the point-sample display layer
- stable single-bit segments return direct logic states
- stable multi-bit segments return `state: "stable"` plus formatted `value`
- dense activity collapses to `state: "flipping"`
- multi-bit flipping segments also report `unique_values` and `transitions`
- `resolution="auto"` searches for the smallest resolution that keeps the result to about 20 segments

#### `find_edge`
- searches transition history forward or backward
- classifies `posedge`, `negedge`, or `anyedge`
- backward search now resolves the last matching edge at or before `start_time`, including an exact edge at `start_time`

#### `find_value_intervals`
- scans one signal over `[start_time, end_time]` and returns every matching `[start, end]` interval
- compares against the stable raw waveform value, not the display-only `changing` marker
- accepts prefixed query values such as `d8`, `h08`, `b00001000`
- if the query value is unprefixed, `radix` controls how it is interpreted

#### `find_condition`
- deliberately simple parser for expressions of the form `PATH == VALUE`
- searches timestamps where the signal already changes
- this is intentionally narrow, not a general expression engine

#### `analyze_pattern`
- heuristic classifier over a time window
- identifies static, dynamic, and clock-like behavior
- clock detection is based on roughly consistent transition intervals

## Important implementation ideas

### One process owns one waveform

The process model is not a general multi-waveform service. Each `wave_agent_cli` instance loads one waveform and answers repeated queries on it. This is what makes the daemon mode effective.

### FSDB is not treated like VCD/FST anymore

This is the main design change to understand.

Current behavior:
- VCD/FST: eager transition load
- FSDB: eager metadata load, lazy per-signal transition load

That split exists because large ASIC FSDBs are not practical under the old all-transitions preload model.

### Value representation is string-based

Every sampled value is stored as a string, for example:
- `0`
- `1`
- `b1010`
- `hdeadbeef`

That keeps JSON output simple, but it is not the most memory-efficient format.

## Where to start if you need to change behavior

- Backend loading or lazy-load behavior:
  - `WaveDatabase::load_*`
  - `ensure_signal_transitions_loaded(...)`
  - `ensure_fsdb_signal_loaded(...)`

- Query semantics:
  - `AgentAPI.cpp`

- CLI or daemon protocol:
  - `main.cpp`

- Path normalization behavior:
  - `normalize_loaded_path(...)`
  - `resolve_query_path(...)`
  - `rebuild_base_signal_path_cache(...)`

## Practical mental model

Think of `waveform_explorer` as:
- waveform metadata loader
- per-signal transition store or accessor
- thin JSON query layer
- simple long-lived daemon wrapper

The backend decides how data is loaded. `AgentAPI` decides how that data is exposed.

Regression history for the packed-vector lookup failure that motivated this behavior is recorded in:
- `/home/qsun/AI_PROJ/auto_waveform_debugger/failure_history.md`
