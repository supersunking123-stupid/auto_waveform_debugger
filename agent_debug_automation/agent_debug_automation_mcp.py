"""Thin re-export wrapper preserving the old monolith import path.

The actual implementation now lives in the submodules:
  models.py, mapping.py, clients.py, sessions.py, ranking.py, server.py, tools.py

This file re-exports every public name so that:
  from agent_debug_automation import agent_debug_automation_mcp as mcp_mod
continues to work with zero test changes.
"""

import os
import sys

# ---------------------------------------------------------------------------
# Submodule imports (dependency order)
# ---------------------------------------------------------------------------
from . import models  # noqa: F401
from . import mapping  # noqa: F401
from . import clients  # noqa: F401
from . import sessions  # noqa: F401
from . import ranking  # noqa: F401
from . import server  # noqa: F401
from . import tools  # noqa: F401

# ---------------------------------------------------------------------------
# Re-export all public names that tests access via mcp_mod.*
# ---------------------------------------------------------------------------

# --- constants ---
from .models import (  # noqa: F401
    DEFAULT_SESSION_NAME,
    SESSION_STORE_DIR,
    ROOT_DIR,
    DEFAULT_RTL_TRACE_BIN,
    DEFAULT_WAVE_CLI,
    DEFAULT_RANK_WINDOW_BEFORE,
    DEFAULT_RANK_WINDOW_AFTER,
    DEFAULT_EXPLAIN_WINDOW_AFTER,
    DEFAULT_TRANSITION_LIMIT,
    DEFAULT_BACKEND_READ_TIMEOUT_SEC,
    ACTIVE_SESSION_FILE,
)

# --- type aliases ---
from .models import (  # noqa: F401
    TraceOptions,
    EdgeType,
    EdgeTypeAlias,
    Direction,
    TraceMode,
    TimeReference,
    ResolutionReference,
)

# --- mapping ---
from .mapping import (  # noqa: F401
    _normalize_waveform_path,
    _normalize_top_variants,
    _normalize_edge_type,
    _strip_bit_suffix,
    _signal_parent_prefixes,
    _resolve_bin,
    runtime_state_lock,
    wave_signal_cache,
    wave_signal_resolution_cache,
    wave_prefix_page_cache,
    _get_wave_signals,
    _wave_get_signal_info,
    _get_fsdb_signals_by_prefix,
    _map_signal_to_waveform,
)

# --- clients ---
from .clients import (  # noqa: F401
    RtlTraceServeSession,
    WaveformDaemon,
    _run_cmd,
    _read_line_with_timeout,
    _stop_process,
    _shell_join,
    rtl_serve_sessions,
    rtl_session_ids_by_key,
    _extract_db_path_from_serve_args,
    _get_rtl_serve_session,
    _append_trace_options,
    _rtl_trace_json,
    wave_daemons,
    _get_wave_daemon,
    _wave_query,
    _get_batch_snapshot,
    _get_transitions_window,
    _sample_signal_times,
    _step_clock_edge,
    _cycle_sample_times,
    _cleanup_runtime_state,
)

# --- sessions ---
from .sessions import (  # noqa: F401
    _utc_now_iso,
    _ensure_session_store,
    _session_waveform_id,
    _safe_session_name,
    _session_file_path,
    _read_json_file,
    _write_json_file,
    _validate_session_name,
    _validate_named_entity,
    _default_session_payload,
    _normalize_session_payload,
    _save_session_payload,
    _load_session_payload,
    _list_session_payloads,
    _get_active_session_ref,
    _set_active_session_ref,
    _clear_active_session_ref,
    _get_or_create_session,
    _resolve_session,
    _session_summary,
    _time_resolution_info,
    _resolve_time_reference,
    _resolve_time_range_reference,
    _expand_signal_groups,
    _with_session_metadata,
    _require_waveform_path_from_session,
)

# --- ranking ---
from .ranking import (  # noqa: F401
    _as_int,
    _collect_cone_signals,
    _window_bounds,
    _summarize_signal,
    _build_signal_summaries,
    _rank_signal_summaries,
    _describe_top_candidate,
    _build_explanations,
)

# --- server ---
from .server import mcp  # noqa: F401

# --- tools ---
from .tools import (  # noqa: F401
    _build_common_context,
    rtl_trace,
    rtl_trace_serve_start,
    rtl_trace_serve_query,
    rtl_trace_serve_stop,
    wave_agent_query,
    list_signals,
    get_signal_info,
    get_snapshot,
    get_value_at_time,
    find_edge,
    find_value_intervals,
    find_condition,
    get_transitions,
    get_signal_overview,
    analyze_pattern,
    create_session,
    list_sessions,
    get_session,
    switch_session,
    delete_session,
    set_cursor,
    move_cursor,
    get_cursor,
    create_bookmark,
    delete_bookmark,
    list_bookmarks,
    create_signal_group,
    update_signal_group,
    delete_signal_group,
    list_signal_groups,
    trace_with_snapshot,
    explain_signal_at_time,
    rank_cone_by_time,
    explain_edge_cause,
)

# ---------------------------------------------------------------------------
# Mock-patch forwarding shim
#
# Tests patch names on *this* module via ``mock.patch.object(mcp_mod, ...)``.
# For those patches to reach actual call-sites inside submodules, we override
# ``__setattr__`` to propagate attribute sets to every submodule that defines
# the same name.
# ---------------------------------------------------------------------------

_fwd_targets: dict = {}
_pkg_name = __name__.rpartition(".")[0]  # "agent_debug_automation"
for _sub_name in ("models", "mapping", "clients", "sessions", "ranking", "tools"):
    _sub = sys.modules.get(f"{_pkg_name}.{_sub_name}")
    if _sub is None:
        continue
    for _attr in dir(_sub):
        if _attr.startswith("__"):
            continue
        _fwd_targets.setdefault(_attr, []).append(_sub)


class _PatchForwardingModule(type(sys)):
    """Module subclass that propagates setattr to defining submodules."""

    def __setattr__(self, name, value):
        super().__setattr__(name, value)
        for target in _fwd_targets.get(name, ()):
            try:
                setattr(target, name, value)
            except (AttributeError, TypeError):
                pass


sys.modules[__name__].__class__ = _PatchForwardingModule

if __name__ == "__main__":
    mcp.run()
