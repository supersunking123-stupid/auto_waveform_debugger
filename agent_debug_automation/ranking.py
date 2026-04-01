"""Heuristic scoring functions for signal ranking."""

from typing import Any, Dict, List, Optional, Sequence, Tuple

from .models import (
    DEFAULT_EXPLAIN_WINDOW_AFTER,
    DEFAULT_RANK_WINDOW_AFTER,
    DEFAULT_RANK_WINDOW_BEFORE,
    DEFAULT_TRANSITION_LIMIT,
    TraceOptions,
)


def _as_int(value: Optional[int], default: int) -> int:
    return default if value is None else int(value)


def _collect_cone_signals(trace_payload: Dict[str, Any]) -> List[str]:
    signals = set()
    for endpoint in trace_payload.get("endpoints", []):
        path = endpoint.get("path")
        if path:
            signals.add(path)
        for group in ("lhs", "rhs"):
            for item in endpoint.get(group, []):
                if item:
                    signals.add(item)
    target = trace_payload.get("target")
    if target:
        signals.add(target)
    return sorted(signals)


def _window_bounds(
    time: int,
    before: Optional[int],
    after: Optional[int],
    default_after: int,
) -> Tuple[int, int]:
    start = max(0, time - _as_int(before, DEFAULT_RANK_WINDOW_BEFORE))
    end = time + _as_int(after, default_after)
    return start, end


def _summarize_signal(
    signal: str,
    mapped_signal: Optional[str],
    mode: str,
    focus_time: int,
    start_time: int,
    end_time: int,
    focus_value: Optional[str],
    start_value: Optional[str],
    end_value: Optional[str],
    transitions: List[Dict[str, Any]],
) -> Dict[str, Any]:
    event_transitions = [t for t in transitions if t.get("t") != start_time]
    recent_toggle_count = len(event_transitions)
    transition_times = [int(item.get("t", 0)) for item in event_transitions]
    last_transition_time = max(transition_times) if transition_times else None
    changed_at_time = any(int(item.get("t", -1)) == focus_time for item in transitions)
    window_span = max(1, end_time - start_time)

    if mode == "loads":
        preferred_transition_times = [t for t in transition_times if t >= focus_time]
        preferred_side = "at_or_after"
    else:
        preferred_transition_times = [t for t in transition_times if t <= focus_time]
        preferred_side = "at_or_before"

    if preferred_transition_times:
        candidate_times = preferred_transition_times
        used_preferred_side = True
    else:
        candidate_times = transition_times
        used_preferred_side = False

    closest_transition_time = None
    closest_transition_distance = None
    closest_transition_direction = None
    if candidate_times:
        closest_transition_time = min(
            candidate_times,
            key=lambda t: (abs(t - focus_time), 0 if t == focus_time else 1, t),
        )
        closest_transition_distance = abs(focus_time - closest_transition_time)
        if closest_transition_time == focus_time:
            closest_transition_direction = "exact"
        elif closest_transition_time < focus_time:
            closest_transition_direction = "before"
        else:
            closest_transition_direction = "after"

        closeness_score = max(
            0,
            1000 - int((closest_transition_distance * 1000) / window_span),
        )
        if closest_transition_time == focus_time:
            closeness_score += 200
    else:
        closeness_score = 0

    is_constant = recent_toggle_count == 0 and start_value == end_value
    activity_score = min(recent_toggle_count, 6) * 20
    if changed_at_time:
        activity_score += 40

    if is_constant and focus_value == "1":
        stuck_class = "stuck_to_1"
        stuck_score = 140
    elif is_constant and focus_value == "0":
        stuck_class = "stuck_to_0"
        stuck_score = 70
    elif is_constant:
        stuck_class = "stuck_other"
        stuck_score = 30
    else:
        stuck_class = "dynamic"
        stuck_score = 0

    total_score = activity_score + stuck_score
    total_score += closeness_score
    return {
        "signal": signal,
        "mapped_signal": mapped_signal,
        "value_at_time": focus_value,
        "value_at_window_start": start_value,
        "value_at_window_end": end_value,
        "recent_toggle_count": recent_toggle_count,
        "last_transition_time": last_transition_time,
        "closest_transition_time": closest_transition_time,
        "closest_transition_distance": closest_transition_distance,
        "closest_transition_direction": closest_transition_direction,
        "preferred_transition_side": preferred_side,
        "used_preferred_transition_side": used_preferred_side,
        "time_since_last_transition": None if last_transition_time is None else focus_time - last_transition_time,
        "changed_at_time": changed_at_time,
        "is_constant_in_window": is_constant,
        "stuck_value": focus_value if is_constant else None,
        "stuck_class": stuck_class,
        "activity_score": activity_score,
        "closeness_score": closeness_score,
        "stuck_score": stuck_score,
        "total_score": total_score,
        "transitions": transitions,
    }


def _build_signal_summaries(
    vcd_path: str,
    signals: Sequence[str],
    mode: str,
    focus_time: int,
    start_time: int,
    end_time: int,
    wave_cli_bin: Optional[str] = None,
) -> Tuple[List[Dict[str, Any]], List[Dict[str, str]]]:
    # Late imports to avoid circular dependency
    from .mapping import _map_signal_to_waveform
    from .clients import _get_batch_snapshot, _get_transitions_window

    mapped_pairs: List[Tuple[str, str]] = []
    unmapped: List[Dict[str, str]] = []
    for signal in signals:
        mapped = _map_signal_to_waveform(vcd_path, signal, wave_cli_bin=wave_cli_bin)
        if mapped is None:
            unmapped.append({"signal": signal, "reason": "waveform-path-not-found"})
        else:
            mapped_pairs.append((signal, mapped))

    mapped_signals = [mapped for _, mapped in mapped_pairs]
    focus_snapshot = _get_batch_snapshot(vcd_path, mapped_signals, focus_time, wave_cli_bin=wave_cli_bin)
    start_snapshot = _get_batch_snapshot(vcd_path, mapped_signals, start_time, wave_cli_bin=wave_cli_bin)
    end_snapshot = _get_batch_snapshot(vcd_path, mapped_signals, end_time, wave_cli_bin=wave_cli_bin)

    summaries: List[Dict[str, Any]] = []
    for signal, mapped in mapped_pairs:
        transitions_result = _get_transitions_window(
            vcd_path,
            mapped,
            start_time,
            end_time,
            wave_cli_bin=wave_cli_bin,
        )
        summaries.append(
            _summarize_signal(
                signal=signal,
                mapped_signal=mapped,
                mode=mode,
                focus_time=focus_time,
                start_time=start_time,
                end_time=end_time,
                focus_value=focus_snapshot.get(mapped),
                start_value=start_snapshot.get(mapped),
                end_value=end_snapshot.get(mapped),
                transitions=list(transitions_result.get("data", [])),
            )
        )
    return summaries, unmapped


def _rank_signal_summaries(summaries: Sequence[Dict[str, Any]]) -> Dict[str, List[Dict[str, Any]]]:
    all_ranked = sorted(summaries, key=lambda item: (-item["total_score"], item["signal"]))
    most_active = sorted(
        [item for item in summaries if item["closeness_score"] > 0 or item["activity_score"] > 0],
        key=lambda item: (-item["closeness_score"], -item["activity_score"], item["signal"]),
    )
    most_stuck = sorted(
        [item for item in summaries if item["is_constant_in_window"]],
        key=lambda item: (-item["stuck_score"], item["signal"]),
    )
    unchanged = sorted(
        [item for item in summaries if item["recent_toggle_count"] == 0],
        key=lambda item: (item["signal"]),
    )
    return {
        "all_signals": all_ranked,
        "most_active_near_time": most_active,
        "most_stuck_in_window": most_stuck,
        "unchanged_candidates": unchanged,
    }


def _describe_top_candidate(candidate: Dict[str, Any]) -> str:
    endpoint = candidate.get("endpoint_path") or "<unknown>"
    term = candidate.get("top_rhs_term")
    if term:
        return (
            f"Top candidate {endpoint} via {term['signal']}="
            f"{term.get('value_at_time')} score={candidate['score']}"
        )
    return f"Top candidate {endpoint} score={candidate['score']}"


def _build_explanations(
    target_summary: Optional[Dict[str, Any]],
    trace_payload: Dict[str, Any],
    ranking_map: Dict[str, Dict[str, Any]],
) -> Dict[str, Any]:
    candidates: List[Dict[str, Any]] = []
    for endpoint in trace_payload.get("endpoints", []):
        endpoint_signal = endpoint.get("path")
        endpoint_summary = ranking_map.get(endpoint_signal, {})
        rhs_terms: List[Dict[str, Any]] = []
        for rhs_signal in endpoint.get("rhs", []):
            rhs_summary = ranking_map.get(rhs_signal)
            if not rhs_summary:
                rhs_terms.append(
                    {
                        "signal": rhs_signal,
                        "mapped_signal": None,
                        "value_at_time": None,
                        "blocking_score": 0,
                        "blocking_confidence": "low",
                        "reason": "waveform-path-not-found",
                    }
                )
                continue
            blocking_score = rhs_summary["activity_score"] + rhs_summary["stuck_score"]
            rhs_terms.append(
                {
                    "signal": rhs_signal,
                    "mapped_signal": rhs_summary["mapped_signal"],
                    "value_at_time": rhs_summary["value_at_time"],
                    "recent_toggle_count": rhs_summary["recent_toggle_count"],
                    "is_constant_in_window": rhs_summary["is_constant_in_window"],
                    "blocking_score": blocking_score,
                    "blocking_confidence": "low" if not endpoint.get("assignment") else "medium",
                }
            )
        rhs_terms.sort(key=lambda item: (-item["blocking_score"], item["signal"]))
        endpoint_score = endpoint_summary.get("activity_score", 0) + endpoint_summary.get("stuck_score", 0)
        if rhs_terms:
            endpoint_score += rhs_terms[0]["blocking_score"]
        candidates.append(
            {
                "endpoint_path": endpoint_signal,
                "file": endpoint.get("file"),
                "line": endpoint.get("line"),
                "kind": endpoint.get("kind"),
                "lhs": endpoint.get("lhs", []),
                "rhs": endpoint.get("rhs", []),
                "assignment": endpoint.get("assignment"),
                "endpoint_value_at_time": endpoint_summary.get("value_at_time"),
                "target_value_at_time": None if target_summary is None else target_summary.get("value_at_time"),
                "endpoint_activity_score": endpoint_summary.get("activity_score", 0),
                "endpoint_stuck_score": endpoint_summary.get("stuck_score", 0),
                "score": endpoint_score,
                "confidence": "low" if not endpoint.get("assignment") else "medium",
                "rhs_terms": rhs_terms,
                "top_rhs_term": rhs_terms[0] if rhs_terms else None,
            }
        )
    candidates.sort(key=lambda item: (-item["score"], item["endpoint_path"] or ""))
    return {
        "candidate_paths": candidates,
        "top_candidate": candidates[0] if candidates else None,
        "top_summary": None if not candidates else _describe_top_candidate(candidates[0]),
    }
