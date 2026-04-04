"""Central virtual signal service — dependency resolution, caching, backend integration.

All tools delegate here for virtual signal operations instead of scattering
``if virtual`` branches across tools.py.
"""

from __future__ import annotations

import hashlib
from typing import Any, Dict, Iterator, List, Optional, Sequence, Tuple

from .expression_evaluator import LogicValue, evaluate_expression, iter_virtual_transitions
from .expression_parser import ParseError, collect_signal_refs, parse_expression
from .models import MAX_VIRTUAL_SIGNAL_DEPTH


# ---------------------------------------------------------------------------
# In-memory transition cache
# ---------------------------------------------------------------------------

# Key: (waveform_path, session_name, signal_name, expression_hash, start_time, end_time)
_virtual_cache: Dict[Tuple[str, str, str, str, int, int], List[Dict[str, Any]]] = {}

# Maximum cache entries before eviction
_MAX_CACHE_SIZE = 256


def _cache_key(
    waveform_path: str,
    session_name: str,
    signal_name: str,
    expression: str,
    start_time: int,
    end_time: int,
) -> Tuple[str, str, str, str, int, int]:
    expr_hash = hashlib.sha256(expression.encode("utf-8")).hexdigest()[:16]
    return (waveform_path, session_name, signal_name, expr_hash, start_time, end_time)


def _cache_get(
    waveform_path: str,
    session_name: str,
    signal_name: str,
    expression: str,
    start_time: int,
    end_time: int,
) -> Optional[List[Dict[str, Any]]]:
    return _virtual_cache.get(_cache_key(waveform_path, session_name, signal_name, expression, start_time, end_time))


def _cache_put(
    waveform_path: str,
    session_name: str,
    signal_name: str,
    expression: str,
    start_time: int,
    end_time: int,
    transitions: List[Dict[str, Any]],
) -> None:
    if len(_virtual_cache) >= _MAX_CACHE_SIZE:
        oldest = next(iter(_virtual_cache))
        del _virtual_cache[oldest]
    _virtual_cache[_cache_key(waveform_path, session_name, signal_name, expression, start_time, end_time)] = transitions


def _cache_invalidate(waveform_path: str, session_name: str, signal_name: str) -> None:
    keys_to_delete = [
        key for key in _virtual_cache
        if key[0] == waveform_path and key[1] == session_name and key[2] == signal_name
    ]
    for key in keys_to_delete:
        _virtual_cache.pop(key, None)


def _cache_invalidate_all(signal_name: str, session: Dict[str, Any]) -> None:
    """Invalidate cache for *signal_name* and all transitive dependents (BFS)."""
    waveform = session["waveform_path"]
    session_name = session["session_name"]
    created = session.get("created_signals", {})

    # BFS: collect the full set of signals to invalidate
    to_invalidate: set = set()
    frontier = [signal_name]
    while frontier:
        current = frontier.pop()
        if current in to_invalidate:
            continue
        to_invalidate.add(current)
        # Find all signals that directly depend on 'current'
        for name, info in created.items():
            if name not in to_invalidate and current in info.get("dependencies", []):
                frontier.append(name)

    # Invalidate cache for every collected signal
    for name in to_invalidate:
        if name in created:
            _cache_invalidate(waveform, session_name, name)


def clear_virtual_cache() -> None:
    """Clear all cached transitions. Used in tests and cleanup."""
    _virtual_cache.clear()


# ---------------------------------------------------------------------------
# Dependency resolution
# ---------------------------------------------------------------------------

def _topological_sort(
    signal_name: str,
    created_signals: Dict[str, Dict[str, Any]],
) -> List[List[str]]:
    """Return evaluation layers (leaves first) for *signal_name*.

    Each layer is a list of signal names that can be evaluated in parallel.
    Raises ValueError on circular dependencies or depth overflow.
    """
    visited: set = set()
    path: set = set()
    layers: List[List[str]] = []

    def _visit(name: str, depth: int) -> None:
        if depth > MAX_VIRTUAL_SIGNAL_DEPTH:
            raise ValueError(f"dependency depth exceeds {MAX_VIRTUAL_SIGNAL_DEPTH} for signal '{name}'")
        if name in path:
            raise ValueError(f"circular dependency detected: {' -> '.join(path)} -> {name}")
        if name in visited:
            return
        path.add(name)
        info = created_signals.get(name)
        if info is None:
            # Leaf signal (real waveform signal) — no further dependencies
            path.discard(name)
            visited.add(name)
            return
        for dep in info.get("dependencies", []):
            _visit(dep, depth + 1)
        path.discard(name)
        visited.add(name)
        # Place in appropriate layer
        layer_idx = 0
        if info.get("dependencies"):
            for dep in info["dependencies"]:
                for i, layer in enumerate(layers):
                    if dep in layer:
                        layer_idx = max(layer_idx, i + 1)
                        break
        while len(layers) <= layer_idx:
            layers.append([])
        layers[layer_idx].append(name)

    _visit(signal_name, 0)
    return layers


# ---------------------------------------------------------------------------
# VirtualSignalService
# ---------------------------------------------------------------------------

class VirtualSignalService:
    """Central service for virtual signal operations.

    All MCP tools delegate here instead of implementing virtual-signal logic
    directly.
    """

    def is_virtual(self, signal_name: str, session: Dict[str, Any]) -> bool:
        return signal_name in session.get("created_signals", {})

    @staticmethod
    def _logic_value_for_raw(raw_value: str, width_hint: Optional[int] = None) -> LogicValue:
        """Parse a raw waveform value while preserving a known signal width."""
        value = LogicValue.from_string(raw_value)
        if width_hint is None or value.width == width_hint:
            return value

        lowered = raw_value.strip().lower()
        if lowered in ("x", "u", "?"):
            return LogicValue.x_val(width_hint)
        if lowered == "z":
            return LogicValue.z_val(width_hint)
        return value.with_width(width_hint)

    def create(
        self,
        session: Dict[str, Any],
        signal_name: str,
        expression: str,
        description: str = "",
    ) -> Dict[str, Any]:
        """Parse expression, store in session. Returns updated session."""
        from .sessions import _save_session_payload, _validate_named_entity

        signal_name = _validate_named_entity(signal_name, "signal_name")
        created = session.setdefault("created_signals", {})

        if signal_name in created:
            raise ValueError(f"virtual signal already exists: {signal_name}")

        # Parse expression
        try:
            ast = parse_expression(expression)
        except ParseError as e:
            raise ValueError(f"expression parse error: {e}") from e

        deps = collect_signal_refs(ast)

        # Check for cycles
        temp_created = dict(created)
        temp_created[signal_name] = {"expression": expression, "dependencies": deps}
        try:
            _topological_sort(signal_name, temp_created)
        except ValueError as e:
            raise ValueError(str(e)) from e

        created[signal_name] = {
            "expression": expression,
            "ast": ast,
            "dependencies": deps,
            "description": description,
        }
        return _save_session_payload(session)

    def update(
        self,
        session: Dict[str, Any],
        signal_name: str,
        expression: Optional[str] = None,
        description: Optional[str] = None,
    ) -> Dict[str, Any]:
        """Update an existing virtual signal. Validates cycles/depth, invalidates cache."""
        from .sessions import _save_session_payload

        created = session.get("created_signals", {})
        if signal_name not in created:
            raise ValueError(f"virtual signal not found: {signal_name}")

        info = created[signal_name]

        if expression is not None and expression != info["expression"]:
            try:
                ast = parse_expression(expression)
            except ParseError as e:
                raise ValueError(f"expression parse error: {e}") from e
            deps = collect_signal_refs(ast)

            # Validate cycles and depth with the new expression before committing
            temp_created = dict(created)
            temp_created[signal_name] = dict(info)
            temp_created[signal_name]["expression"] = expression
            temp_created[signal_name]["dependencies"] = deps
            try:
                _topological_sort(signal_name, temp_created)
            except ValueError as e:
                raise ValueError(str(e)) from e

            # Invalidate cache before committing new deps
            _cache_invalidate_all(signal_name, session)
            info["expression"] = expression
            info["ast"] = ast
            info["dependencies"] = deps

        if description is not None:
            info["description"] = description

        return _save_session_payload(session)

    def delete(
        self,
        session: Dict[str, Any],
        signal_name: str,
    ) -> Dict[str, Any]:
        """Delete a virtual signal. Invalidates cache for dependents."""
        from .sessions import _save_session_payload

        created = session.get("created_signals", {})
        if signal_name not in created:
            raise ValueError(f"virtual signal not found: {signal_name}")

        # Invalidate cache before removing
        _cache_invalidate_all(signal_name, session)
        del created[signal_name]
        return _save_session_payload(session)

    def list(self, session: Dict[str, Any]) -> List[Dict[str, Any]]:
        """List all virtual signals with metadata."""
        created = session.get("created_signals", {})
        result = []
        for name, info in sorted(created.items()):
            result.append({
                "signal_name": name,
                "expression": info.get("expression", ""),
                "dependencies": info.get("dependencies", []),
                "description": info.get("description", ""),
            })
        return result

    def get_info(self, signal_name: str, session: Dict[str, Any]) -> Dict[str, Any]:
        """Get metadata for a virtual signal."""
        created = session.get("created_signals", {})
        if signal_name not in created:
            raise ValueError(f"virtual signal not found: {signal_name}")
        info = created[signal_name]
        return {
            "signal_name": signal_name,
            "expression": info.get("expression", ""),
            "dependencies": info.get("dependencies", []),
            "description": info.get("description", ""),
        }

    def eval_logic_at_time(
        self,
        signal_name: str,
        session: Dict[str, Any],
        time: int,
        wave_cli_bin: Optional[str] = None,
    ) -> LogicValue:
        """Evaluate virtual signal at a single time point and return LogicValue."""
        ast, operand_transitions, signal_widths = self._resolve_deps(
            signal_name, session, time, time, wave_cli_bin=wave_cli_bin
        )

        values: Dict[str, LogicValue] = {}
        for path, trans in operand_transitions.items():
            width_hint = signal_widths.get(path)
            if trans:
                values[path] = self._logic_value_for_raw(trans[-1]["v"], width_hint)
            else:
                values[path] = LogicValue.x_val(width_hint or 1)
        return evaluate_expression(ast, values)

    def eval_at_time(
        self,
        signal_name: str,
        session: Dict[str, Any],
        time: int,
        wave_cli_bin: Optional[str] = None,
    ) -> str:
        """Evaluate virtual signal at a single time point. Returns value string."""
        return self.eval_logic_at_time(
            signal_name, session, time, wave_cli_bin=wave_cli_bin
        ).to_string()

    def max_leaf_transition_time(
        self,
        signal_name: str,
        session: Dict[str, Any],
        wave_cli_bin: Optional[str] = None,
    ) -> int:
        """Return the maximum transition timestamp reachable from any real leaf."""
        created = session.get("created_signals", {})
        waveform_path = session["waveform_path"]
        memo: Dict[str, int] = {}

        def _max_for_ref(ref: str) -> int:
            if ref in memo:
                return memo[ref]

            if ref not in created:
                value = self._fetch_last_transition_time(ref, waveform_path, wave_cli_bin)
                memo[ref] = value
                return value

            deps = created[ref].get("dependencies", [])
            if not deps:
                memo[ref] = -1
                return -1

            value = max((_max_for_ref(dep) for dep in deps), default=-1)
            memo[ref] = value
            return value

        return _max_for_ref(signal_name)

    def iter_transitions(
        self,
        signal_name: str,
        session: Dict[str, Any],
        start_time: int,
        end_time: int,
        wave_cli_bin: Optional[str] = None,
    ) -> Iterator[Dict[str, Any]]:
        """Lazy transition generator for a virtual signal."""
        # Check cache first
        created = session.get("created_signals", {})
        info = created.get(signal_name)
        if info is None:
            raise ValueError(f"virtual signal not found: {signal_name}")

        expression = info.get("expression", "")
        waveform = session["waveform_path"]
        session_name_val = session.get("session_name", "")
        cached = _cache_get(
            waveform,
            session_name_val,
            signal_name,
            expression,
            start_time,
            end_time,
        )
        if cached is not None:
            # Slice from cache
            for tr in cached:
                if start_time <= tr["t"] <= end_time:
                    yield tr
            return

        # Resolve dependencies and compute
        ast, operand_transitions, signal_widths = self._resolve_deps(
            signal_name, session, start_time, end_time, wave_cli_bin=wave_cli_bin
        )

        transitions = list(iter_virtual_transitions(
            ast, operand_transitions, start_time, end_time, signal_widths=signal_widths
        ))

        # Cache the result
        _cache_put(
            waveform,
            session_name_val,
            signal_name,
            expression,
            start_time,
            end_time,
            transitions,
        )

        for tr in transitions:
            yield tr

    def _resolve_deps(
        self,
        signal_name: str,
        session: Dict[str, Any],
        start_time: int,
        end_time: int,
        wave_cli_bin: Optional[str] = None,
    ) -> Tuple[Dict[str, Any], Dict[str, List[Dict[str, Any]]], Dict[str, int]]:
        """Resolve dependency DAG, fetch leaf transitions.

        Returns (ast, operand_transitions, signal_widths).
        """
        created = session.get("created_signals", {})
        info = created[signal_name]
        ast = info.get("ast")
        if ast is None:
            ast = parse_expression(info["expression"])

        leaf_refs = collect_signal_refs(ast)
        # Separate real leaves from virtual leaves
        operand_transitions: Dict[str, List[Dict[str, Any]]] = {}
        signal_widths: Dict[str, int] = {}

        for ref in leaf_refs:
            if ref in created:
                # Virtual leaf — recursively compute
                for tr in self.iter_transitions(ref, session, start_time, end_time, wave_cli_bin=wave_cli_bin):
                    operand_transitions.setdefault(ref, []).append(tr)
                # Width from first transition
                if ref in operand_transitions and operand_transitions[ref]:
                    val = operand_transitions[ref][0]["v"]
                    lv = LogicValue.from_string(val)
                    signal_widths[ref] = lv.width
            else:
                # Real leaf — fetch from backend
                trans = self._fetch_real_transitions(
                    ref, session["waveform_path"], start_time, end_time, wave_cli_bin
                )
                operand_transitions[ref] = trans
                # Get width from signal info
                w = self._fetch_signal_width(ref, session["waveform_path"], wave_cli_bin)
                if w is not None:
                    signal_widths[ref] = w

        return ast, operand_transitions, signal_widths

    def _fetch_real_transitions(
        self,
        signal_path: str,
        waveform_path: str,
        start_time: int,
        end_time: int,
        wave_cli_bin: Optional[str] = None,
    ) -> List[Dict[str, Any]]:
        """Fetch transitions for a real signal from the C++ backend.

        To guarantee that the evaluator's `bisect_right` has a proper seed value for
        signals that are stable across the window start time, we explicitly fetch
        the raw value exactly at `start_time - 1` and inject it as a pseudo-event at
        the start of the list.

        Raises ValueError if the backend truncates the result.
        """
        from .clients import _wave_query

        # 1. Fetch exactly the state immediately before the window
        seed_time = max(0, int(start_time) - 1)
        seed_res = _wave_query(
            waveform_path,
            "get_raw_value_at_time",
            {"path": signal_path, "time": seed_time},
            wave_cli_bin=wave_cli_bin,
        )
        history = []
        if seed_res.get("status") == "success":
            val = seed_res.get("data", "x")
            history.append({"t": seed_time, "v": val, "glitch": False})

        # 2. Fetch all transitions in the window [start_time, end_time]
        result = _wave_query(
            waveform_path,
            "get_transitions",
            {
                "path": signal_path,
                "start_time": int(start_time),
                "end_time": int(end_time),
                "max_limit": 10000,
            },
            wave_cli_bin=wave_cli_bin,
        )
        if result.get("status") != "success":
            return history

        if result.get("truncated", False):
            raise ValueError(
                f"virtual signal operand '{signal_path}' has more than 10,000 transitions "
                f"in the requested window [{start_time}, {end_time}]; "
                "narrow the query window or simplify the expression to avoid silent data loss."
            )

        history.extend(result.get("data", []))
        return history

    def _fetch_last_transition_time(
        self,
        signal_path: str,
        waveform_path: str,
        wave_cli_bin: Optional[str] = None,
    ) -> int:
        """Fetch the last transition timestamp for a real signal."""
        from .clients import _wave_query

        result = _wave_query(
            waveform_path,
            "get_last_transition_time",
            {"path": signal_path},
            wave_cli_bin=wave_cli_bin,
        )
        if result.get("status") != "success":
            return -1
        try:
            return int(result.get("data", -1))
        except (TypeError, ValueError):
            return -1

    def _fetch_signal_width(
        self,
        signal_path: str,
        waveform_path: str,
        wave_cli_bin: Optional[str] = None,
    ) -> Optional[int]:
        """Fetch signal width from the C++ backend."""
        from .clients import _wave_query

        result = _wave_query(
            waveform_path,
            "get_signal_info",
            {"path": signal_path},
            wave_cli_bin=wave_cli_bin,
        )
        if result.get("status") != "success":
            return None
        return result.get("data", {}).get("width")


# ---------------------------------------------------------------------------
# Module-level singleton
# ---------------------------------------------------------------------------

vs_service = VirtualSignalService()
