"""Signal path normalization, FSDB prefix lookup, and waveform signal caching."""

import threading
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from .models import (
    DEFAULT_WAVE_CLI,
    EdgeType,
    SESSION_STORE_DIR,
)


def _normalize_waveform_path(path: str) -> str:
    return str(Path(path).expanduser().resolve())


def _normalize_top_variants(path: str) -> List[str]:
    variants: List[str] = []
    for candidate in (path, path[4:] if path.startswith("TOP.") else f"TOP.{path}"):
        if candidate and candidate not in variants:
            variants.append(candidate)
    return variants


def _normalize_edge_type(edge_type: str) -> EdgeType:
    normalized = str(edge_type).strip().lower()
    alias_map: Dict[str, EdgeType] = {
        "rise": "posedge",
        "rising": "posedge",
        "risingedge": "posedge",
        "posedge": "posedge",
        "fall": "negedge",
        "falling": "negedge",
        "fallingedge": "negedge",
        "negedge": "negedge",
        "edge": "anyedge",
        "any": "anyedge",
        "anyedge": "anyedge",
    }
    return alias_map.get(normalized, "anyedge")


def _strip_bit_suffix(path: str) -> str:
    bracket = path.find("[")
    if bracket == -1:
        return path
    return path[:bracket]


def _signal_parent_prefixes(signal: str) -> List[str]:
    prefixes: List[str] = []
    base = _strip_bit_suffix(signal)
    parts = base.split(".")
    for i in range(len(parts) - 1, 0, -1):
        prefix = ".".join(parts[:i]) + "."
        if prefix not in prefixes:
            prefixes.append(prefix)
    return prefixes


def _resolve_bin(override: Optional[str], default_path: Path, fallback_name: str) -> str:
    if override:
        return str(Path(override).expanduser().resolve())
    if default_path.exists():
        return str(default_path)
    return fallback_name


# ---------------------------------------------------------------------------
# Signal caches (module-level, shared with clients/tools)
# ---------------------------------------------------------------------------

runtime_state_lock = threading.RLock()

wave_signal_cache: Dict[str, List[str]] = {}
wave_signal_resolution_cache: Dict[Tuple[str, str], Optional[str]] = {}
wave_prefix_page_cache: Dict[Tuple[str, str], List[str]] = {}


# _wave_query is wired by clients.py after import to avoid circular dependency.
# For test mock-patch compatibility, look it up through the wrapper module.
def _wave_query(vcd_path: str, cmd: str, args=None, wave_cli_bin=None):
    import agent_debug_automation.agent_debug_automation_mcp as _wrapper
    return _wrapper._wave_query(vcd_path, cmd, args=args, wave_cli_bin=wave_cli_bin)


def _get_wave_signals(vcd_path: str, wave_cli_bin: Optional[str] = None) -> List[str]:
    normalized = str(Path(vcd_path).expanduser().resolve())
    with runtime_state_lock:
        cached_signals = wave_signal_cache.get(normalized)
    if cached_signals is None:
        if normalized.lower().endswith(".fsdb"):
            signals: List[str] = []
            cursor = ""
            while True:
                result = _wave_query(
                    normalized,
                    "list_signals_page",
                    {"prefix": "", "cursor": cursor, "limit": 5000},
                    wave_cli_bin=wave_cli_bin,
                )
                if result.get("status") != "success":
                    raise RuntimeError(result.get("message", "failed to page waveform signals"))
                page = list(result.get("data", []))
                signals.extend(page)
                if not result.get("has_more"):
                    break
                cursor = result.get("next_cursor", "")
                if not cursor:
                    break
            with runtime_state_lock:
                wave_signal_cache[normalized] = signals
        else:
            result = _wave_query(normalized, "list_signals", {"pattern": "*"}, wave_cli_bin=wave_cli_bin)
            if result.get("status") != "success":
                raise RuntimeError(result.get("message", "failed to list waveform signals"))
            with runtime_state_lock:
                wave_signal_cache[normalized] = list(result.get("data", []))
    with runtime_state_lock:
        return list(wave_signal_cache[normalized])


def _wave_get_signal_info(vcd_path: str, path: str, wave_cli_bin: Optional[str] = None) -> Optional[Dict[str, Any]]:
    result = _wave_query(
        vcd_path,
        "get_signal_info",
        {"path": path},
        wave_cli_bin=wave_cli_bin,
    )
    if result.get("status") != "success":
        return None
    return result.get("data")


def _get_fsdb_signals_by_prefix(vcd_path: str, prefix: str, wave_cli_bin: Optional[str] = None) -> List[str]:
    normalized = str(Path(vcd_path).expanduser().resolve())
    cache_key = (normalized, prefix)
    with runtime_state_lock:
        cached_page = wave_prefix_page_cache.get(cache_key)
    if cached_page is not None:
        return list(cached_page)

    signals: List[str] = []
    cursor = ""
    while True:
        result = _wave_query(
            normalized,
            "list_signals_page",
            {"prefix": prefix, "cursor": cursor, "limit": 5000},
            wave_cli_bin=wave_cli_bin,
        )
        if result.get("status") != "success":
            raise RuntimeError(result.get("message", f"failed to page FSDB signals for prefix {prefix}"))
        page = list(result.get("data", []))
        signals.extend(page)
        if not result.get("has_more"):
            break
        cursor = result.get("next_cursor", "")
        if not cursor:
            break
    with runtime_state_lock:
        wave_prefix_page_cache[cache_key] = signals
        return list(signals)


def _map_signal_to_waveform(vcd_path: str, signal: str, wave_cli_bin: Optional[str] = None) -> Optional[str]:
    normalized_wave = str(Path(vcd_path).expanduser().resolve())
    cache_key = (normalized_wave, signal)
    with runtime_state_lock:
        if cache_key in wave_signal_resolution_cache:
            return wave_signal_resolution_cache[cache_key]

    if normalized_wave.lower().endswith(".fsdb"):
        for candidate in _normalize_top_variants(signal):
            info = _wave_get_signal_info(normalized_wave, candidate, wave_cli_bin=wave_cli_bin)
            if info is not None:
                resolved = info.get("path", candidate)
                with runtime_state_lock:
                    wave_signal_resolution_cache[cache_key] = resolved
                return resolved

        candidate_variants = _normalize_top_variants(signal)
        base_variants = {_strip_bit_suffix(candidate) for candidate in candidate_variants}
        for candidate in candidate_variants:
            for prefix in _signal_parent_prefixes(candidate):
                scoped_signals = _get_fsdb_signals_by_prefix(normalized_wave, prefix, wave_cli_bin=wave_cli_bin)
                exact_set = set(scoped_signals)
                for variant in candidate_variants:
                    if variant in exact_set:
                        with runtime_state_lock:
                            wave_signal_resolution_cache[cache_key] = variant
                        return variant
                for item in scoped_signals:
                    if _strip_bit_suffix(item) in base_variants:
                        with runtime_state_lock:
                            wave_signal_resolution_cache[cache_key] = item
                        return item

        with runtime_state_lock:
            wave_signal_resolution_cache[cache_key] = None
        return None

    known = set(_get_wave_signals(normalized_wave, wave_cli_bin=wave_cli_bin))
    for candidate in _normalize_top_variants(signal):
        if candidate in known:
            with runtime_state_lock:
                wave_signal_resolution_cache[cache_key] = candidate
            return candidate
    with runtime_state_lock:
        wave_signal_resolution_cache[cache_key] = None
    return None
