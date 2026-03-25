import json
import os
import shlex
import subprocess
import uuid
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

try:
    from mcp.server.fastmcp import FastMCP
except ImportError:
    class FastMCP:  # type: ignore[override]
        def __init__(self, name: str):
            self.name = name

        def tool(self):
            def decorator(func):
                return func

            return decorator

        def run(self):
            raise RuntimeError("mcp.server.fastmcp is not installed")


mcp = FastMCP("Agent Debug Automation")

ROOT_DIR = Path(__file__).resolve().parent.parent
DEFAULT_RTL_TRACE_BIN = ROOT_DIR / "standalone_trace" / "build" / "rtl_trace"
DEFAULT_WAVE_CLI = ROOT_DIR / "waveform_explorer" / "build" / "wave_agent_cli"
DEFAULT_RANK_WINDOW_BEFORE = 1000
DEFAULT_RANK_WINDOW_AFTER = 1000
DEFAULT_EXPLAIN_WINDOW_AFTER = 0
DEFAULT_TRANSITION_LIMIT = 256


def _resolve_bin(override: Optional[str], default_path: Path, fallback_name: str) -> str:
    if override:
        return str(Path(override).expanduser().resolve())
    if default_path.exists():
        return str(default_path)
    return fallback_name


def _run_cmd(args: List[str], timeout_sec: int = 300) -> Dict[str, Any]:
    try:
        proc = subprocess.run(
            args,
            text=True,
            capture_output=True,
            timeout=timeout_sec,
            check=False,
        )
        return {
            "status": "success" if proc.returncode == 0 else "error",
            "exit_code": proc.returncode,
            "cmd": " ".join(shlex.quote(a) for a in args),
            "stdout": proc.stdout,
            "stderr": proc.stderr,
        }
    except subprocess.TimeoutExpired as e:
        return {
            "status": "error",
            "message": f"Command timed out after {timeout_sec}s",
            "cmd": " ".join(shlex.quote(a) for a in args),
            "stdout": e.stdout or "",
            "stderr": e.stderr or "",
        }
    except Exception as e:
        return {"status": "error", "message": str(e)}


def _as_int(value: Optional[int], default: int) -> int:
    return default if value is None else int(value)


def _normalize_top_variants(path: str) -> List[str]:
    variants: List[str] = []
    for candidate in (path, path[4:] if path.startswith("TOP.") else f"TOP.{path}"):
        if candidate and candidate not in variants:
            variants.append(candidate)
    return variants


def _shell_join(args: Sequence[str]) -> str:
    return " ".join(shlex.quote(arg) for arg in args)


class RtlTraceServeSession:
    def __init__(self, bin_path: str, serve_args: List[str]):
        self.bin_path = bin_path
        self.serve_args = serve_args
        self.process = subprocess.Popen(
            [bin_path, "serve"] + serve_args,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self.startup = self._read_until_end()

    def _read_until_end(self) -> Dict[str, Any]:
        if self.process.poll() is not None:
            err = self.process.stderr.read() if self.process.stderr else ""
            return {"status": "error", "message": "rtl_trace serve exited", "stderr": err}

        assert self.process.stdout is not None
        out_lines: List[str] = []
        while True:
            s = self.process.stdout.readline()
            if not s:
                err = self.process.stderr.read() if self.process.stderr else ""
                return {
                    "status": "error",
                    "message": "rtl_trace serve terminated while waiting for response",
                    "stderr": err,
                    "stdout": "".join(out_lines),
                }
            if s.strip() == "<<END>>":
                break
            out_lines.append(s)
        return {"status": "success", "stdout": "".join(out_lines)}

    def query(self, line: str) -> Dict[str, Any]:
        if self.process.poll() is not None:
            err = self.process.stderr.read() if self.process.stderr else ""
            return {"status": "error", "message": "rtl_trace serve exited", "stderr": err}

        try:
            assert self.process.stdin is not None
            self.process.stdin.write(line + "\n")
            self.process.stdin.flush()
            return self._read_until_end()
        except Exception as e:
            return {"status": "error", "message": str(e)}

    def stop(self) -> Dict[str, Any]:
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
        return {"status": "success"}


rtl_serve_sessions: Dict[str, RtlTraceServeSession] = {}
rtl_session_ids_by_key: Dict[Tuple[str, str], str] = {}


def _get_rtl_serve_session(db_path: str, rtl_trace_bin: Optional[str] = None) -> RtlTraceServeSession:
    resolved_db = str(Path(db_path).expanduser().resolve())
    bin_path = _resolve_bin(rtl_trace_bin, DEFAULT_RTL_TRACE_BIN, "rtl_trace")
    key = (bin_path, resolved_db)
    sid = rtl_session_ids_by_key.get(key)
    if sid:
        sess = rtl_serve_sessions.get(sid)
        if sess and sess.process.poll() is None:
            return sess
    session_id = str(uuid.uuid4())
    sess = RtlTraceServeSession(bin_path, ["--db", resolved_db])
    if sess.startup.get("status") != "success":
        raise RuntimeError(sess.startup.get("message", "failed to start rtl_trace serve"))
    rtl_serve_sessions[session_id] = sess
    rtl_session_ids_by_key[key] = session_id
    return sess


def _append_trace_options(args: List[str], trace_options: Optional[Dict[str, Any]]) -> None:
    if not trace_options:
        return

    option_specs = (
        ("cone_level", "--cone-level"),
        ("depth", "--depth"),
        ("max_nodes", "--max-nodes"),
        ("include", "--include"),
        ("exclude", "--exclude"),
        ("stop_at", "--stop-at"),
    )
    for key, flag in option_specs:
        value = trace_options.get(key)
        if value is None:
            continue
        if isinstance(value, list):
            for item in value:
                args.extend([flag, str(item)])
        else:
            args.extend([flag, str(value)])

    if trace_options.get("prefer_port_hop"):
        args.append("--prefer-port-hop")


def _rtl_trace_json(
    db_path: str,
    signal: str,
    mode: str = "drivers",
    trace_options: Optional[Dict[str, Any]] = None,
    rtl_trace_bin: Optional[str] = None,
) -> Dict[str, Any]:
    session = _get_rtl_serve_session(db_path, rtl_trace_bin=rtl_trace_bin)
    args = ["trace", "--mode", mode, "--signal", signal, "--format", "json"]
    _append_trace_options(args, trace_options)
    result = session.query(_shell_join(args))
    if result.get("status") != "success":
        return result

    stdout = result.get("stdout", "").strip()
    if not stdout:
        return {"status": "error", "message": "rtl_trace returned empty output"}
    try:
        payload = json.loads(stdout)
    except json.JSONDecodeError as e:
        return {"status": "error", "message": f"invalid rtl_trace JSON: {e}", "stdout": stdout}

    payload["status"] = "success"
    payload["raw_command"] = _shell_join(args)
    return payload


@mcp.tool()
def rtl_trace(args: List[str], rtl_trace_bin: Optional[str] = None, timeout_sec: int = 300):
    """
    Run standalone_trace rtl_trace with original command/options unchanged.
    Example:
      args=["compile","--db","rtl_trace.db","--top","top","-f","files.f"]
      args=["trace","--db","rtl_trace.db","--mode","drivers","--signal","top.u0.sig"]
      args=["find","--db","rtl_trace.db","--query","foo.bar"]
      args=["hier","--db","rtl_trace.db","--root","top.u0"]
    For interactive serve mode, use rtl_trace_serve_start/query/stop tools.
    """
    if not args:
        return {"status": "error", "message": "args must not be empty"}
    if args[0] == "serve":
        return {
            "status": "error",
            "message": "Use rtl_trace_serve_start/query/stop tools for interactive serve mode.",
        }

    bin_path = _resolve_bin(rtl_trace_bin, DEFAULT_RTL_TRACE_BIN, "rtl_trace")
    return _run_cmd([bin_path] + args, timeout_sec=timeout_sec)


@mcp.tool()
def rtl_trace_serve_start(serve_args: Optional[List[str]] = None, rtl_trace_bin: Optional[str] = None):
    """
    Start interactive `rtl_trace serve [serve_args...]`.
    Example serve_args=["--db","rtl_trace.db"]
    """
    args = serve_args or []
    bin_path = _resolve_bin(rtl_trace_bin, DEFAULT_RTL_TRACE_BIN, "rtl_trace")
    sid = str(uuid.uuid4())
    try:
        session = RtlTraceServeSession(bin_path, args)
        rtl_serve_sessions[sid] = session
        return {"status": "success", "session_id": sid, "startup": session.startup}
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def rtl_trace_serve_query(session_id: str, command_line: str):
    """
    Send one line command to an existing rtl_trace serve session.
    """
    sess = rtl_serve_sessions.get(session_id)
    if not sess:
        return {"status": "error", "message": f"session not found: {session_id}"}
    return sess.query(command_line)


@mcp.tool()
def rtl_trace_serve_stop(session_id: str):
    """
    Stop a running rtl_trace serve session.
    """
    sess = rtl_serve_sessions.pop(session_id, None)
    if not sess:
        return {"status": "error", "message": f"session not found: {session_id}"}
    for key, sid in list(rtl_session_ids_by_key.items()):
        if sid == session_id:
            rtl_session_ids_by_key.pop(key, None)
    return sess.stop()


class WaveformDaemon:
    def __init__(self, wave_cli_path: str, waveform_path: str):
        self.waveform_path = waveform_path
        self.process = subprocess.Popen(
            [wave_cli_path, waveform_path],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )

    def query(self, cmd: str, args: Optional[dict] = None) -> dict:
        query_obj = {"cmd": cmd, "args": args or {}}
        query_str = json.dumps(query_obj)
        try:
            assert self.process.stdin is not None
            assert self.process.stdout is not None
            self.process.stdin.write(query_str + "\n")
            self.process.stdin.flush()
            line = self.process.stdout.readline()
            if not line:
                err = self.process.stderr.read() if self.process.stderr else ""
                return {"status": "error", "message": f"wave_agent_cli daemon crashed: {err}"}
            return json.loads(line)
        except Exception as e:
            return {"status": "error", "message": str(e)}

    def stop(self):
        if self.process.poll() is None:
            self.process.terminate()


wave_daemons: Dict[str, WaveformDaemon] = {}
wave_signal_cache: Dict[str, List[str]] = {}
wave_signal_resolution_cache: Dict[Tuple[str, str], Optional[str]] = {}
wave_prefix_page_cache: Dict[Tuple[str, str], List[str]] = {}


def _get_wave_daemon(vcd_path: str, wave_cli: Optional[str] = None) -> WaveformDaemon:
    normalized = str(Path(vcd_path).expanduser().resolve())
    if normalized not in wave_daemons:
        if not os.path.exists(normalized):
            raise FileNotFoundError(f"waveform file not found: {normalized}")
        cli_path = _resolve_bin(wave_cli, DEFAULT_WAVE_CLI, "wave_agent_cli")
        wave_daemons[normalized] = WaveformDaemon(cli_path, normalized)
    return wave_daemons[normalized]


def _wave_query(vcd_path: str, cmd: str, args: Optional[dict] = None, wave_cli_bin: Optional[str] = None) -> Dict[str, Any]:
    return _get_wave_daemon(vcd_path, wave_cli=wave_cli_bin).query(cmd, args or {})


def _get_wave_signals(vcd_path: str, wave_cli_bin: Optional[str] = None) -> List[str]:
    normalized = str(Path(vcd_path).expanduser().resolve())
    if normalized not in wave_signal_cache:
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
            wave_signal_cache[normalized] = signals
        else:
            result = _wave_query(normalized, "list_signals", wave_cli_bin=wave_cli_bin)
            if result.get("status") != "success":
                raise RuntimeError(result.get("message", "failed to list waveform signals"))
            wave_signal_cache[normalized] = list(result.get("data", []))
    return wave_signal_cache[normalized]


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
    if cache_key in wave_prefix_page_cache:
        return wave_prefix_page_cache[cache_key]

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
    wave_prefix_page_cache[cache_key] = signals
    return signals


def _map_signal_to_waveform(vcd_path: str, signal: str, wave_cli_bin: Optional[str] = None) -> Optional[str]:
    normalized_wave = str(Path(vcd_path).expanduser().resolve())
    cache_key = (normalized_wave, signal)
    if cache_key in wave_signal_resolution_cache:
        return wave_signal_resolution_cache[cache_key]

    if normalized_wave.lower().endswith(".fsdb"):
        for candidate in _normalize_top_variants(signal):
            info = _wave_get_signal_info(normalized_wave, candidate, wave_cli_bin=wave_cli_bin)
            if info is not None:
                resolved = info.get("path", candidate)
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
                        wave_signal_resolution_cache[cache_key] = variant
                        return variant
                for item in scoped_signals:
                    if _strip_bit_suffix(item) in base_variants:
                        wave_signal_resolution_cache[cache_key] = item
                        return item

        wave_signal_resolution_cache[cache_key] = None
        return None

    known = set(_get_wave_signals(normalized_wave, wave_cli_bin=wave_cli_bin))
    for candidate in _normalize_top_variants(signal):
        if candidate in known:
            wave_signal_resolution_cache[cache_key] = candidate
            return candidate
    wave_signal_resolution_cache[cache_key] = None
    return None


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


def _get_batch_snapshot(
    vcd_path: str,
    mapped_signals: Sequence[str],
    time: int,
    wave_cli_bin: Optional[str] = None,
) -> Dict[str, Any]:
    if not mapped_signals:
        return {}
    result = _wave_query(
        vcd_path,
        "get_snapshot",
        {"signals": list(mapped_signals), "time": int(time)},
        wave_cli_bin=wave_cli_bin,
    )
    if result.get("status") != "success":
        raise RuntimeError(result.get("message", "failed to get snapshot"))
    return dict(result.get("data", {}))


def _get_transitions_window(
    vcd_path: str,
    signal: str,
    start_time: int,
    end_time: int,
    wave_cli_bin: Optional[str] = None,
) -> Dict[str, Any]:
    result = _wave_query(
        vcd_path,
        "get_transitions",
        {
            "path": signal,
            "start_time": int(start_time),
            "end_time": int(end_time),
            "max_limit": DEFAULT_TRANSITION_LIMIT,
        },
        wave_cli_bin=wave_cli_bin,
    )
    if result.get("status") != "success":
        raise RuntimeError(result.get("message", "failed to get transitions"))
    return result


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


def _sample_signal_times(
    vcd_path: str,
    signals: Sequence[str],
    sample_times: Sequence[int],
    wave_cli_bin: Optional[str] = None,
) -> Tuple[Dict[str, Dict[str, Any]], List[Dict[str, str]]]:
    mapped: Dict[str, str] = {}
    unmapped: List[Dict[str, str]] = []
    for signal in signals:
        mapped_signal = _map_signal_to_waveform(vcd_path, signal, wave_cli_bin=wave_cli_bin)
        if mapped_signal is None:
            unmapped.append({"signal": signal, "reason": "waveform-path-not-found"})
        else:
            mapped[signal] = mapped_signal

    per_time: Dict[str, Dict[str, Any]] = {}
    for sample_time in sample_times:
        snapshot = _get_batch_snapshot(
            vcd_path,
            list(mapped.values()),
            sample_time,
            wave_cli_bin=wave_cli_bin,
        )
        values: Dict[str, Any] = {}
        for original, resolved in mapped.items():
            values[original] = {
                "mapped_signal": resolved,
                "value": snapshot.get(resolved),
            }
        per_time[str(sample_time)] = values
    return per_time, unmapped


def _step_clock_edge(
    vcd_path: str,
    clock_path: str,
    start_time: int,
    direction: str,
    wave_cli_bin: Optional[str] = None,
) -> Optional[int]:
    result = _wave_query(
        vcd_path,
        "find_edge",
        {
            "path": clock_path,
            "edge_type": "posedge",
            "start_time": int(start_time),
            "direction": direction,
        },
        wave_cli_bin=wave_cli_bin,
    )
    if result.get("status") != "success":
        raise RuntimeError(result.get("message", "failed to find edge"))
    edge_time = result.get("data")
    if edge_time in (-1, None):
        return None
    return int(edge_time)


def _cycle_sample_times(
    vcd_path: str,
    time: int,
    clock_path: str,
    cycle_offsets: Sequence[int],
    wave_cli_bin: Optional[str] = None,
) -> Tuple[Dict[int, Optional[int]], List[str]]:
    warnings: List[str] = []
    base_edge = _step_clock_edge(
        vcd_path,
        clock_path,
        time,
        "backward",
        wave_cli_bin=wave_cli_bin,
    )
    if base_edge is None:
        warnings.append(f"no clock edge found at or before {time} for {clock_path}")
        return {offset: None for offset in cycle_offsets}, warnings

    times: Dict[int, Optional[int]] = {}
    for offset in cycle_offsets:
        current = base_edge
        if offset > 0:
            for _ in range(offset):
                next_time = _step_clock_edge(
                    vcd_path,
                    clock_path,
                    current + 1,
                    "forward",
                    wave_cli_bin=wave_cli_bin,
                )
                if next_time is None:
                    warnings.append(f"missing forward clock edge for offset {offset}")
                    current = None
                    break
                current = next_time
        elif offset < 0:
            for _ in range(-offset):
                prev_time = _step_clock_edge(
                    vcd_path,
                    clock_path,
                    max(0, current - 1),
                    "backward",
                    wave_cli_bin=wave_cli_bin,
                )
                if prev_time is None:
                    warnings.append(f"missing backward clock edge for offset {offset}")
                    current = None
                    break
                current = prev_time
        times[offset] = current
    return times, warnings


def _build_common_context(
    db_path: str,
    waveform_path: str,
    signal: str,
    time: int,
    mode: str,
    trace_options: Optional[Dict[str, Any]],
    rtl_trace_bin: Optional[str],
    wave_cli_bin: Optional[str],
    window_before: Optional[int],
    window_after: Optional[int],
    default_after: int,
) -> Dict[str, Any]:
    trace_payload = _rtl_trace_json(
        db_path=db_path,
        signal=signal,
        mode=mode,
        trace_options=trace_options,
        rtl_trace_bin=rtl_trace_bin,
    )
    if trace_payload.get("status") != "success":
        return trace_payload

    start_time, end_time = _window_bounds(time, window_before, window_after, default_after)
    cone_signals = _collect_cone_signals(trace_payload)
    summaries, unmapped = _build_signal_summaries(
        waveform_path,
        cone_signals,
        mode=mode,
        focus_time=time,
        start_time=start_time,
        end_time=end_time,
        wave_cli_bin=wave_cli_bin,
    )
    focus_samples, snapshot_unmapped = _sample_signal_times(
        waveform_path,
        cone_signals,
        [time],
        wave_cli_bin=wave_cli_bin,
    )
    ranking = _rank_signal_summaries(summaries)
    warnings: List[str] = []
    if trace_payload.get("stops"):
        warnings.append("rtl_trace returned bounded traversal stops")
    return {
        "status": "success",
        "target": signal,
        "time_context": {
            "focus_time": time,
            "window_start": start_time,
            "window_end": end_time,
            "mode": mode,
        },
        "structure": {
            "trace": trace_payload,
            "cone_signals": cone_signals,
        },
        "waveform": {
            "focus_samples": focus_samples.get(str(time), {}),
            "signal_summaries": summaries,
        },
        "ranking": ranking,
        "unmapped_signals": sorted(
            unmapped + snapshot_unmapped,
            key=lambda item: item["signal"],
        ),
        "warnings": warnings,
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


@mcp.tool()
def wave_agent_query(vcd_path: str, cmd: str, args: Optional[dict] = None, wave_cli_bin: Optional[str] = None):
    """
    Generic wave_agent_cli command wrapper.
    Keeps original waveform command model: <cmd> + <args>.
    """
    try:
        return _wave_query(vcd_path, cmd, args=args, wave_cli_bin=wave_cli_bin)
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def list_signals(vcd_path: str):
    """List all hierarchical signal paths found in the VCD file."""
    return wave_agent_query(vcd_path, "list_signals")


@mcp.tool()
def get_signal_info(vcd_path: str, path: str):
    """Get metadata for a specific signal (width, type, etc.)."""
    return wave_agent_query(vcd_path, "get_signal_info", {"path": path})


@mcp.tool()
def get_snapshot(vcd_path: str, signals: List[str], time: int):
    """Get the values of multiple signals at a specific timestamp."""
    return wave_agent_query(vcd_path, "get_snapshot", {"signals": signals, "time": time})


@mcp.tool()
def get_value_at_time(vcd_path: str, path: str, time: int):
    """Get the value of a single signal at a specific timestamp."""
    return wave_agent_query(vcd_path, "get_value_at_time", {"path": path, "time": time})


@mcp.tool()
def find_edge(vcd_path: str, path: str, edge_type: str, start_time: int, direction: str = "forward"):
    """Search for the next/previous transition edge of a signal."""
    return wave_agent_query(
        vcd_path,
        "find_edge",
        {
            "path": path,
            "edge_type": edge_type,
            "start_time": start_time,
            "direction": direction,
        },
    )


@mcp.tool()
def find_condition(vcd_path: str, expression: str, start_time: int, direction: str = "forward"):
    """Find the first timestamp where a logical condition is met."""
    return wave_agent_query(
        vcd_path,
        "find_condition",
        {
            "expression": expression,
            "start_time": start_time,
            "direction": direction,
        },
    )


@mcp.tool()
def get_transitions(vcd_path: str, path: str, start_time: int, end_time: int, max_limit: int = 50):
    """Get a compressed history of signal transitions within a time window."""
    return wave_agent_query(
        vcd_path,
        "get_transitions",
        {
            "path": path,
            "start_time": start_time,
            "end_time": end_time,
            "max_limit": max_limit,
        },
    )


@mcp.tool()
def analyze_pattern(vcd_path: str, path: str, start_time: int, end_time: int):
    """Analyze a signal's behavior (e.g., identify clocks, static signals)."""
    return wave_agent_query(
        vcd_path,
        "analyze_pattern",
        {
            "path": path,
            "start_time": start_time,
            "end_time": end_time,
        },
    )


@mcp.tool()
def trace_with_snapshot(
    db_path: str,
    waveform_path: str,
    signal: str,
    time: int,
    mode: str = "drivers",
    trace_options: Optional[Dict[str, Any]] = None,
    sample_offsets: Optional[List[int]] = None,
    clock_path: Optional[str] = None,
    cycle_offsets: Optional[List[int]] = None,
    rank_window_before: Optional[int] = None,
    rank_window_after: Optional[int] = None,
    rtl_trace_bin: Optional[str] = None,
    wave_cli_bin: Optional[str] = None,
):
    """Trace a signal structurally and return cone values at time T plus optional T+/-offset samples."""
    try:
        context = _build_common_context(
            db_path=db_path,
            waveform_path=waveform_path,
            signal=signal,
            time=time,
            mode=mode,
            trace_options=trace_options,
            rtl_trace_bin=rtl_trace_bin,
            wave_cli_bin=wave_cli_bin,
            window_before=rank_window_before,
            window_after=rank_window_after,
            default_after=DEFAULT_EXPLAIN_WINDOW_AFTER,
        )
        if context.get("status") != "success":
            return context

        cone_signals = context["structure"]["cone_signals"]
        extra_times = [time + int(offset) for offset in (sample_offsets or [])]
        absolute_samples, sample_unmapped = _sample_signal_times(
            waveform_path,
            cone_signals,
            extra_times,
            wave_cli_bin=wave_cli_bin,
        )
        cycle_data: Dict[str, Any] = {}
        if clock_path and cycle_offsets:
            resolved_clock = _map_signal_to_waveform(waveform_path, clock_path, wave_cli_bin=wave_cli_bin)
            if resolved_clock is None:
                context["warnings"].append(f"clock path not found in waveform: {clock_path}")
            else:
                cycle_times, cycle_warnings = _cycle_sample_times(
                    waveform_path,
                    time,
                    resolved_clock,
                    [int(offset) for offset in cycle_offsets],
                    wave_cli_bin=wave_cli_bin,
                )
                context["warnings"].extend(cycle_warnings)
                valid_times = [t for t in cycle_times.values() if t is not None]
                cycle_samples, cycle_unmapped = _sample_signal_times(
                    waveform_path,
                    cone_signals,
                    valid_times,
                    wave_cli_bin=wave_cli_bin,
                )
                cycle_data = {
                    "clock_path": clock_path,
                    "resolved_clock_path": resolved_clock,
                    "cycle_times": cycle_times,
                    "samples": cycle_samples,
                }
                context["unmapped_signals"].extend(cycle_unmapped)
        context["waveform"]["absolute_offset_samples"] = absolute_samples
        context["waveform"]["cycle_offset_samples"] = cycle_data
        context["unmapped_signals"].extend(sample_unmapped)
        return context
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def explain_signal_at_time(
    db_path: str,
    waveform_path: str,
    signal: str,
    time: int,
    mode: str = "drivers",
    trace_options: Optional[Dict[str, Any]] = None,
    rank_window_before: Optional[int] = None,
    rank_window_after: Optional[int] = None,
    rtl_trace_bin: Optional[str] = None,
    wave_cli_bin: Optional[str] = None,
):
    """Explain which structural drivers are most correlated with the signal state at time T."""
    try:
        context = _build_common_context(
            db_path=db_path,
            waveform_path=waveform_path,
            signal=signal,
            time=time,
            mode=mode,
            trace_options=trace_options,
            rtl_trace_bin=rtl_trace_bin,
            wave_cli_bin=wave_cli_bin,
            window_before=rank_window_before,
            window_after=rank_window_after,
            default_after=DEFAULT_EXPLAIN_WINDOW_AFTER,
        )
        if context.get("status") != "success":
            return context

        ranking_map = {
            item["signal"]: item
            for item in context["waveform"]["signal_summaries"]
        }
        target_summary = ranking_map.get(signal)
        explanations = _build_explanations(target_summary, context["structure"]["trace"], ranking_map)
        context["explanations"] = explanations
        return context
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def rank_cone_by_time(
    db_path: str,
    waveform_path: str,
    signal: str,
    time: int,
    mode: str = "drivers",
    trace_options: Optional[Dict[str, Any]] = None,
    window_start: Optional[int] = None,
    window_end: Optional[int] = None,
    rtl_trace_bin: Optional[str] = None,
    wave_cli_bin: Optional[str] = None,
):
    """Rank cone signals by recent activity and stuckness near time T."""
    try:
        if window_start is None:
            window_start = max(0, time - DEFAULT_RANK_WINDOW_BEFORE)
        if window_end is None:
            window_end = time + DEFAULT_RANK_WINDOW_AFTER
        trace_payload = _rtl_trace_json(
            db_path=db_path,
            signal=signal,
            mode=mode,
            trace_options=trace_options,
            rtl_trace_bin=rtl_trace_bin,
        )
        if trace_payload.get("status") != "success":
            return trace_payload

        cone_signals = _collect_cone_signals(trace_payload)
        summaries, unmapped = _build_signal_summaries(
            waveform_path,
            cone_signals,
            mode=mode,
            focus_time=time,
            start_time=window_start,
            end_time=window_end,
            wave_cli_bin=wave_cli_bin,
        )
        return {
            "status": "success",
            "target": signal,
            "time_context": {
                "focus_time": time,
                "window_start": window_start,
                "window_end": window_end,
                "mode": mode,
            },
            "structure": {
                "trace": trace_payload,
                "cone_signals": cone_signals,
            },
            "waveform": {
                "signal_summaries": summaries,
            },
            "ranking": _rank_signal_summaries(summaries),
            "explanations": {},
            "unmapped_signals": unmapped,
            "warnings": [],
        }
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def explain_edge_cause(
    db_path: str,
    waveform_path: str,
    signal: str,
    time: int,
    edge_type: str = "anyedge",
    direction: str = "backward",
    mode: str = "drivers",
    trace_options: Optional[Dict[str, Any]] = None,
    rank_window_before: Optional[int] = None,
    rank_window_after: Optional[int] = None,
    rtl_trace_bin: Optional[str] = None,
    wave_cli_bin: Optional[str] = None,
):
    """Resolve the relevant edge for a signal, then explain the upstream cause chain at that edge."""
    try:
        mapped_signal = _map_signal_to_waveform(waveform_path, signal, wave_cli_bin=wave_cli_bin)
        if mapped_signal is None:
            return {"status": "error", "message": f"signal not found in waveform: {signal}"}
        edge_result = _wave_query(
            waveform_path,
            "find_edge",
            {
                "path": mapped_signal,
                "edge_type": edge_type,
                "start_time": int(time),
                "direction": direction,
            },
            wave_cli_bin=wave_cli_bin,
        )
        if edge_result.get("status") != "success":
            return edge_result
        edge_time = edge_result.get("data")
        if edge_time in (-1, None):
            return {
                "status": "error",
                "message": f"no {edge_type} edge found for {signal} near {time}",
            }
        edge_time = int(edge_time)
        context = explain_signal_at_time(
            db_path=db_path,
            waveform_path=waveform_path,
            signal=signal,
            time=edge_time,
            mode=mode,
            trace_options=trace_options,
            rank_window_before=rank_window_before,
            rank_window_after=rank_window_after,
            rtl_trace_bin=rtl_trace_bin,
            wave_cli_bin=wave_cli_bin,
        )
        if context.get("status") != "success":
            return context

        before_time = max(0, edge_time - 1)
        before_after = _get_batch_snapshot(
            waveform_path,
            [mapped_signal],
            edge_time,
            wave_cli_bin=wave_cli_bin,
        )
        before_snapshot = _get_batch_snapshot(
            waveform_path,
            [mapped_signal],
            before_time,
            wave_cli_bin=wave_cli_bin,
        )
        context["time_context"]["requested_time"] = time
        context["time_context"]["resolved_edge_time"] = edge_time
        context["time_context"]["edge_type"] = edge_type
        context["waveform"]["edge_context"] = {
            "signal": signal,
            "mapped_signal": mapped_signal,
            "value_before_edge": before_snapshot.get(mapped_signal),
            "value_at_edge": before_after.get(mapped_signal),
        }
        return context
    except Exception as e:
        return {"status": "error", "message": str(e)}


if __name__ == "__main__":
    mcp.run()
