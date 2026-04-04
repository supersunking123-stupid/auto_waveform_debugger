"""All @mcp.tool handler functions."""

import uuid
from typing import Any, Dict, List, Optional

from .expression_evaluator import LogicValue
from .server import mcp

from .clients import (
    RtlTraceServeSession,
    WaveformDaemon,
    _get_batch_snapshot,
    _rtl_trace_json,
    _run_cmd,
    _sample_signal_times,
    _cycle_sample_times,
    _wave_query,
    _extract_db_path_from_serve_args,
    rtl_serve_sessions,
    rtl_session_ids_by_key,
    wave_daemons,
    runtime_state_lock,
)
from .mapping import (
    _map_signal_to_waveform,
    _normalize_edge_type,
    wave_signal_cache,
    wave_signal_resolution_cache,
    wave_prefix_page_cache,
    _resolve_bin,
)
from .models import (
    DEFAULT_EXPLAIN_WINDOW_AFTER,
    DEFAULT_RANK_WINDOW_AFTER,
    DEFAULT_RANK_WINDOW_BEFORE,
    DEFAULT_RTL_TRACE_BIN,
    DEFAULT_SESSION_NAME,
    Direction,
    EdgeType,
    EdgeTypeAlias,
    ResolutionReference,
    TimeReference,
    TraceMode,
    TraceOptions,
)
from .ranking import (
    _build_explanations,
    _build_signal_summaries,
    _collect_cone_signals,
    _rank_signal_summaries,
    _window_bounds,
)
from .virtual_signals import vs_service
from .sessions import (
    _default_session_payload,
    _expand_signal_groups,
    _get_active_session_ref,
    _get_or_create_session,
    _list_session_payloads,
    _load_session_payload,
    _resolve_session,
    _resolve_time_range_reference,
    _resolve_time_reference,
    _require_waveform_path_from_session,
    _save_session_payload,
    _session_file_path,
    _session_summary,
    _set_active_session_ref,
    _time_resolution_info,
    _validate_named_entity,
    _with_session_metadata,
)


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


# ---------------------------------------------------------------------------
# RTL trace tools
# ---------------------------------------------------------------------------

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
    db_path = _extract_db_path_from_serve_args(args)
    with runtime_state_lock:
        if db_path is not None:
            key = (bin_path, db_path)
            sid = rtl_session_ids_by_key.get(key)
            if sid:
                sess = rtl_serve_sessions.get(sid)
                if sess and sess.process.poll() is None:
                    return {"status": "success", "session_id": sid, "startup": sess.startup}
                rtl_session_ids_by_key.pop(key, None)
                rtl_serve_sessions.pop(sid, None)
        sid = str(uuid.uuid4())
        try:
            session = RtlTraceServeSession(bin_path, args)
            rtl_serve_sessions[sid] = session
            if db_path is not None:
                rtl_session_ids_by_key[(bin_path, db_path)] = sid
            return {"status": "success", "session_id": sid, "startup": session.startup}
        except Exception as e:
            return {"status": "error", "message": str(e)}


@mcp.tool()
def rtl_trace_serve_query(session_id: str, command_line: str):
    """
    Send one line command to an existing rtl_trace serve session.
    """
    with runtime_state_lock:
        sess = rtl_serve_sessions.get(session_id)
    if not sess:
        return {"status": "error", "message": f"session not found: {session_id}"}
    return sess.query(command_line)


@mcp.tool()
def rtl_trace_serve_stop(session_id: str):
    """
    Stop a running rtl_trace serve session.
    """
    with runtime_state_lock:
        sess = rtl_serve_sessions.pop(session_id, None)
        if sess:
            for key, sid in list(rtl_session_ids_by_key.items()):
                if sid == session_id:
                    rtl_session_ids_by_key.pop(key, None)
    if not sess:
        return {"status": "error", "message": f"session not found: {session_id}"}
    return sess.stop()


# ---------------------------------------------------------------------------
# Waveform passthrough tools
# ---------------------------------------------------------------------------

@mcp.tool()
def wave_agent_query(vcd_path: str, cmd: str, args: Optional[Dict[str, Any]] = None, wave_cli_bin: Optional[str] = None):
    """
    Generic wave_agent_cli command wrapper.
    Keeps original waveform command model: <cmd> + <args>.
    """
    try:
        return _wave_query(vcd_path, cmd, args=args, wave_cli_bin=wave_cli_bin)
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def list_signals(
    vcd_path: Optional[str] = None,
    pattern: str = "",
    types: Optional[List[str]] = None,
    session_name: Optional[str] = None,
):
    """List waveform signal paths.

    If vcd_path is omitted, the active Session selects the waveform file.
    Default behavior lists only signals declared in the waveform's top module.
    Pass `pattern="*"` to enumerate the full namespace, or a narrower
    wildcard such as `top.nvdla_top.nvdla_core2cvsram_ar_*`.
    `types` may include any combination of `input`, `output`, `inout`,
    `net`, and `register`.
    """
    try:
        resolved_waveform, session = _require_waveform_path_from_session(vcd_path, session_name)
        # Use late import to go through the wrapper module so mock.patch works
        import agent_debug_automation.agent_debug_automation_mcp as _wrapper
        result = _wrapper.wave_agent_query(
            resolved_waveform,
            "list_signals",
            {"pattern": pattern, "types": list(types or [])},
        )
        return _with_session_metadata(result, session)
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def get_signal_info(vcd_path: Optional[str] = None, path: str = "", session_name: Optional[str] = None):
    """Get metadata for one signal in the active or selected Session waveform."""
    try:
        resolved_waveform, session = _require_waveform_path_from_session(vcd_path, session_name)
        import agent_debug_automation.agent_debug_automation_mcp as _wrapper
        result = _wrapper.wave_agent_query(resolved_waveform, "get_signal_info", {"path": path})
        return _with_session_metadata(result, session)
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def get_snapshot(
    vcd_path: Optional[str] = None,
    signals: Optional[List[str]] = None,
    time: TimeReference = 0,
    radix: str = "hex",
    signals_are_groups: bool = False,
    session_name: Optional[str] = None,
):
    """Get values for multiple signals at one time.

    For session-aware use:
    - time may be an integer, "Cursor", or "BM_<bookmark_name>"
    - signals may be Signal Group names when signals_are_groups=True
    - if vcd_path is omitted, the active Session selects the waveform
    """
    try:
        expanded_signals, signal_info, signal_session = _expand_signal_groups(
            signals or [],
            signals_are_groups=signals_are_groups,
            waveform_path=vcd_path,
            session_name=session_name,
        )
        resolved_time, time_info, time_session = _resolve_time_reference(
            time,
            waveform_path=signal_session["waveform_path"] if signal_session else vcd_path,
            session_name=signal_session["session_name"] if signal_session else session_name,
        )
        session = time_session if signal_session is None else signal_session
        import agent_debug_automation.agent_debug_automation_mcp as _wrapper

        # Partition signals into real vs. virtual so each subset is handled correctly.
        real_signals = [s for s in expanded_signals if not vs_service.is_virtual(s, session)]
        virtual_signals_list = [s for s in expanded_signals if vs_service.is_virtual(s, session)]

        # Fetch real signals from the C++ backend.
        if real_signals:
            result = _wrapper.wave_agent_query(
                session["waveform_path"],
                "get_snapshot",
                {"signals": real_signals, "time": resolved_time, "radix": radix},
            )
        else:
            result = {"status": "success", "data": {}}

        if result.get("status") != "success":
            return _with_session_metadata(result, session, time_info, signal_info)

        # Evaluate each virtual signal and merge into the snapshot data.
        snapshot_data = result.get("data", {})
        for vsig in virtual_signals_list:
            snapshot_data[vsig] = _format_virtual_value_at_time(vsig, session, resolved_time, radix)
        result["data"] = snapshot_data

        return _with_session_metadata(result, session, time_info, signal_info)
    except Exception as e:
        return {"status": "error", "message": str(e)}


def _normalize_logic_char(ch: str) -> str:
    lowered = ch.lower()
    if lowered in ("0", "1"):
        return lowered
    if lowered in ("x", "u", "?"):
        return "x"
    if lowered == "z":
        return "z"
    return "x"


def _simplify_scalar_value(value: str) -> str:
    if value in ("0", "1", "x", "z"):
        return value
    if value in ("U", "u", "?"):
        return "x"
    if not value:
        return "x"

    ch = value[0].lower()
    if ch in ("0", "1", "x", "z"):
        return ch
    if ch in ("u", "?"):
        return "x"
    return "x"


def _normalize_radix(radix: str) -> str:
    normalized = radix.lower()
    if normalized in ("bin", "binary", "2"):
        return "bin"
    if normalized in ("dec", "decimal", "10"):
        return "dec"
    return "hex"


def _normalize_multibit_bits(value: str, width: int) -> str:
    if not value:
        return ""

    hex_digits = max(1, (width + 3) // 4)
    bit_digits = max(1, width)
    first = value[0]

    if first in ("h", "H"):
        hex_map = {
            "0": "0000", "1": "0001", "2": "0010", "3": "0011",
            "4": "0100", "5": "0101", "6": "0110", "7": "0111",
            "8": "1000", "9": "1001", "a": "1010", "b": "1011",
            "c": "1100", "d": "1101", "e": "1110", "f": "1111",
            "z": "zzzz",
        }
        bits = "".join(hex_map.get(ch.lower(), "xxxx") for ch in value[1:])
        if len(bits) < hex_digits * 4:
            bits = "0" * (hex_digits * 4 - len(bits)) + bits
        if len(bits) > bit_digits:
            bits = bits[-bit_digits:]
        return bits

    if first in ("b", "B"):
        bits = "".join(_normalize_logic_char(ch) for ch in value[1:])
        if not bits:
            return ""
        if len(bits) < bit_digits:
            bits = "0" * (bit_digits - len(bits)) + bits
        elif len(bits) > bit_digits:
            bits = bits[-bit_digits:]
        return bits

    return value.lower()


def _binary_bits_to_decimal(bits: str) -> str:
    decimal = "0"
    for bit in bits:
        carry = 1 if bit == "1" else 0
        digits = list(decimal)
        for idx in range(len(digits) - 1, -1, -1):
            digit = (ord(digits[idx]) - ord("0")) * 2 + carry
            digits[idx] = chr(ord("0") + (digit % 10))
            carry = digit // 10
        if carry > 0:
            digits.insert(0, chr(ord("0") + carry))
        decimal = "".join(digits)
    return decimal


def _decimal_string_to_bits(decimal: str) -> str:
    if not decimal:
        return ""
    if any(not ch.isdigit() for ch in decimal):
        return ""

    decimal = decimal.lstrip("0") or "0"
    if decimal == "0":
        return "0"

    bits: List[str] = []
    while decimal and decimal != "0":
        carry = 0
        quotient: List[str] = []
        for ch in decimal:
            cur = carry * 10 + (ord(ch) - ord("0"))
            q = cur // 2
            carry = cur % 2
            if quotient or q != 0:
                quotient.append(chr(ord("0") + q))
        bits.append(chr(ord("0") + carry))
        decimal = "".join(quotient) if quotient else "0"

    bits.reverse()
    return "".join(bits)


def _normalize_query_value_to_bits(value: str, width: int, radix: str) -> str:
    if not value:
        return ""

    bit_digits = max(1, width)
    prefix = value[0].lower()
    normalized_radix = _normalize_radix(radix)

    if width <= 1:
        if value in ("0", "1", "x", "z"):
            return value
        if prefix in ("b", "d", "h"):
            nested_radix = "bin" if prefix == "b" else ("dec" if prefix == "d" else "hex")
            return _normalize_query_value_to_bits(value[1:], width, nested_radix)
        return _simplify_scalar_value(value)

    if prefix == "b":
        bits = "".join(_normalize_logic_char(ch) for ch in value[1:])
        if not bits:
            return ""
        if len(bits) < bit_digits:
            bits = "0" * (bit_digits - len(bits)) + bits
        elif len(bits) > bit_digits:
            bits = bits[-bit_digits:]
        return bits

    if prefix == "h":
        return _normalize_multibit_bits(value, width)

    if prefix == "d" or normalized_radix == "dec":
        decimal_digits = value[1:] if prefix == "d" else value
        bits = _decimal_string_to_bits(decimal_digits)
        if not bits:
            return ""
        if len(bits) < bit_digits:
            bits = "0" * (bit_digits - len(bits)) + bits
        elif len(bits) > bit_digits:
            bits = bits[-bit_digits:]
        return bits

    if normalized_radix == "bin":
        bits = "".join(_normalize_logic_char(ch) for ch in value)
        if not bits:
            return ""
        if len(bits) < bit_digits:
            bits = "0" * (bit_digits - len(bits)) + bits
        elif len(bits) > bit_digits:
            bits = bits[-bit_digits:]
        return bits

    return _normalize_multibit_bits("h" + value, width)


def _raw_value_matches_query(raw_value: str, width: int, query_bits: str) -> bool:
    if not query_bits:
        return False
    if width <= 1:
        return _simplify_scalar_value(raw_value) == query_bits
    return _normalize_multibit_bits(raw_value, width) == query_bits


def _format_multibit_value(value: str, width: int, radix: str) -> str:
    hex_digits = max(1, (width + 3) // 4)
    bit_digits = max(1, width)
    normalized_radix = _normalize_radix(radix)
    normalized = _normalize_multibit_bits(value, width)
    if not normalized:
        if normalized_radix == "dec":
            return "dx"
        if normalized_radix == "bin":
            return "bx"
        return "hx"

    if normalized[0] not in ("0", "1", "x", "z"):
        return normalized

    if normalized_radix == "bin":
        bits = normalized
        if len(bits) < bit_digits:
            bits = "0" * (bit_digits - len(bits)) + bits
        return "b" + bits

    if normalized_radix == "dec":
        if any(bit not in ("0", "1") for bit in normalized):
            return "b" + normalized
        return "d" + _binary_bits_to_decimal(normalized)

    bits = normalized
    if not bits:
        return "hx"
    if len(bits) < hex_digits * 4:
        bits = "0" * (hex_digits * 4 - len(bits)) + bits

    out = ["h"]
    for idx in range(0, len(bits), 4):
        nibble = bits[idx:idx + 4]
        if all(bit == "z" for bit in nibble):
            out.append("z")
            continue
        if any(bit in ("x", "z") for bit in nibble):
            out.append("x")
            continue
        out.append(format(int(nibble, 2), "x"))
    return "".join(out)


def _virtual_logic_values_at_time(path: str, session: Dict[str, Any], time: int) -> tuple[LogicValue, Optional[LogicValue]]:
    current = vs_service.eval_logic_at_time(path, session, time)
    if time <= 0:
        return current, None
    previous = vs_service.eval_logic_at_time(path, session, time - 1)
    return current, previous


def _format_virtual_logic_value(current: LogicValue, previous: Optional[LogicValue], radix: str) -> str:
    current_raw = current.to_string()
    previous_raw = "U" if previous is None else previous.to_string()
    width = max(current.width, previous.width if previous is not None else current.width)

    if width <= 1:
        if previous_raw == "0" and current_raw == "1":
            return "rising"
        if previous_raw == "1" and current_raw == "0":
            return "falling"
        return _simplify_scalar_value(current_raw)

    if previous_raw != current_raw:
        return "changing"
    return _format_multibit_value(current_raw, width, radix)


def _format_virtual_value_at_time(path: str, session: Dict[str, Any], time: int, radix: str) -> str:
    current, previous = _virtual_logic_values_at_time(path, session, time)
    return _format_virtual_logic_value(current, previous, radix)


def _virtual_edge_matches(previous_value: str, current_value: str, edge_type: str) -> bool:
    if edge_type == "posedge":
        return previous_value == "0" and current_value == "1"
    if edge_type == "negedge":
        return previous_value == "1" and current_value == "0"
    return previous_value != current_value


@mcp.tool()
def get_value_at_time(
    vcd_path: Optional[str] = None,
    path: str = "",
    time: TimeReference = 0,
    radix: str = "hex",
    session_name: Optional[str] = None,
):
    """Get one signal value at one time.

    For session-aware use, time may be an integer, "Cursor", or
    "BM_<bookmark_name>".
    """
    try:
        resolved_time, time_info, session = _resolve_time_reference(time, waveform_path=vcd_path, session_name=session_name)
        if vs_service.is_virtual(path, session):
            result = {
                "status": "success",
                "data": _format_virtual_value_at_time(path, session, resolved_time, radix),
            }
            return _with_session_metadata(result, session, time_info)
        import agent_debug_automation.agent_debug_automation_mcp as _wrapper
        result = _wrapper.wave_agent_query(
            session["waveform_path"],
            "get_value_at_time",
            {"path": path, "time": resolved_time, "radix": radix},
        )
        return _with_session_metadata(result, session, time_info)
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def find_edge(
    vcd_path: Optional[str] = None,
    path: str = "",
    edge_type: EdgeType | EdgeTypeAlias = "anyedge",
    start_time: TimeReference = 0,
    direction: Direction = "forward",
    session_name: Optional[str] = None,
):
    """Search for the next or previous signal edge from a time reference."""
    try:
        edge_type = _normalize_edge_type(edge_type)
        resolved_time, time_info, session = _resolve_time_reference(start_time, waveform_path=vcd_path, session_name=session_name)
        if vs_service.is_virtual(path, session):
            CHUNK_SIZE = 1000000
            if direction == "forward":
                max_time = vs_service.max_leaf_transition_time(path, session)
                if max_time <= resolved_time:
                    result = {"status": "success", "data": -1}
                    return _with_session_metadata(result, session, time_info)

                curr_start = resolved_time
                while curr_start <= max_time:
                    curr_end = min(max_time, curr_start + CHUNK_SIZE - 1)
                    transitions = list(vs_service.iter_transitions(path, session, curr_start, curr_end))
                    prev = None
                    for tr in transitions:
                        val = tr["v"]
                        if prev is not None and _virtual_edge_matches(prev, val, edge_type) and tr["t"] > resolved_time:
                            result = {"status": "success", "data": tr["t"]}
                            return _with_session_metadata(result, session, time_info)
                        prev = val
                    if curr_end >= max_time:
                        break
                    curr_start = curr_end + 1
            elif direction == "backward":
                curr_end = resolved_time
                while True:
                    curr_start = max(0, curr_end - CHUNK_SIZE + 1)
                    transitions = list(vs_service.iter_transitions(path, session, curr_start, curr_end))
                    prev = None
                    last_match = None
                    for tr in transitions:
                        val = tr["v"]
                        if prev is not None and _virtual_edge_matches(prev, val, edge_type) and curr_start <= tr["t"] <= resolved_time:
                            last_match = tr["t"]
                        prev = val

                    if last_match is not None:
                        result = {"status": "success", "data": last_match}
                        return _with_session_metadata(result, session, time_info)

                    if curr_start == 0:
                        break

                    curr_end = curr_start - 1

            result = {"status": "success", "data": -1}
            return _with_session_metadata(result, session, time_info)
        import agent_debug_automation.agent_debug_automation_mcp as _wrapper
        result = _wrapper.wave_agent_query(
            session["waveform_path"],
            "find_edge",
            {
                "path": path,
                "edge_type": edge_type,
                "start_time": resolved_time,
                "direction": direction,
            },
        )
        return _with_session_metadata(result, session, time_info)
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def find_value_intervals(
    vcd_path: Optional[str] = None,
    path: str = "",
    value: str = "",
    start_time: TimeReference = 0,
    end_time: TimeReference = 0,
    radix: str = "hex",
    session_name: Optional[str] = None,
):
    """Find all matching value intervals in a session-aware time range."""
    try:
        resolved_start, resolved_end, start_info, end_info, session = _resolve_time_range_reference(
            start_time,
            end_time,
            waveform_path=vcd_path,
            session_name=session_name,
        )
        if vs_service.is_virtual(path, session):
            current_logic, _ = _virtual_logic_values_at_time(path, session, resolved_start)
            width = current_logic.width
            query_bits = _normalize_query_value_to_bits(value, width, radix)
            if not query_bits:
                return {"status": "error", "message": "failed to parse query value"}

            transitions = list(vs_service.iter_transitions(
                path, session, resolved_start, resolved_end,
            ))
            intervals = []
            current_raw = transitions[0]["v"] if transitions else current_logic.to_string()
            interval_start: Optional[int] = None
            if _raw_value_matches_query(current_raw, width, query_bits):
                interval_start = resolved_start

            for tr in transitions[1:]:
                next_matches = _raw_value_matches_query(tr["v"], width, query_bits)
                if interval_start is not None and not next_matches:
                    intervals.append({"start": interval_start, "end": tr["t"] - 1})
                    interval_start = None
                elif interval_start is None and next_matches:
                    interval_start = tr["t"]

            if interval_start is not None:
                intervals.append({"start": interval_start, "end": resolved_end})
            result = {
                "status": "success",
                "data": intervals,
            }
            result["resolved_time_range"] = {"start": start_info, "end": end_info}
            return _with_session_metadata(result, session)
        import agent_debug_automation.agent_debug_automation_mcp as _wrapper
        result = _wrapper.wave_agent_query(
            session["waveform_path"],
            "find_value_intervals",
            {
                "path": path,
                "value": value,
                "start_time": resolved_start,
                "end_time": resolved_end,
                "radix": radix,
            },
        )
        result["resolved_time_range"] = {"start": start_info, "end": end_info}
        return _with_session_metadata(result, session)
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def find_condition(
    vcd_path: Optional[str] = None,
    expression: str = "",
    start_time: TimeReference = 0,
    direction: Direction = "forward",
    session_name: Optional[str] = None,
):
    """Find the first timestamp where a simple condition is met."""
    try:
        resolved_time, time_info, session = _resolve_time_reference(start_time, waveform_path=vcd_path, session_name=session_name)
        import agent_debug_automation.agent_debug_automation_mcp as _wrapper
        result = _wrapper.wave_agent_query(
            session["waveform_path"],
            "find_condition",
            {
                "expression": expression,
                "start_time": resolved_time,
                "direction": direction,
            },
        )
        return _with_session_metadata(result, session, time_info)
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def get_transitions(
    vcd_path: Optional[str] = None,
    path: str = "",
    start_time: TimeReference = 0,
    end_time: TimeReference = 0,
    max_limit: int = 50,
    session_name: Optional[str] = None,
):
    """Get a compressed transition history in a session-aware time window."""
    try:
        resolved_start, resolved_end, start_info, end_info, session = _resolve_time_range_reference(
            start_time,
            end_time,
            waveform_path=vcd_path,
            session_name=session_name,
        )
        if vs_service.is_virtual(path, session):
            transitions = list(vs_service.iter_transitions(
                path, session, resolved_start, resolved_end,
            ))
            truncated = len(transitions) > max_limit
            result = {"status": "success", "data": transitions[:max_limit], "truncated": truncated}
            result["resolved_time_range"] = {"start": start_info, "end": end_info}
            return _with_session_metadata(result, session)
        import agent_debug_automation.agent_debug_automation_mcp as _wrapper
        result = _wrapper.wave_agent_query(
            session["waveform_path"],
            "get_transitions",
            {
                "path": path,
                "start_time": resolved_start,
                "end_time": resolved_end,
                "max_limit": max_limit,
            },
        )
        result["resolved_time_range"] = {"start": start_info, "end": end_info}
        return _with_session_metadata(result, session)
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def count_transitions(
    vcd_path: Optional[str] = None,
    path: str = "",
    start_time: TimeReference = 0,
    end_time: TimeReference = 0,
    edge_type: EdgeType | EdgeTypeAlias = "anyedge",
    session_name: Optional[str] = None,
):
    """Count edges or toggles for one signal in a session-aware time window."""
    try:
        edge_type = _normalize_edge_type(edge_type)
        resolved_start, resolved_end, start_info, end_info, session = _resolve_time_range_reference(
            start_time,
            end_time,
            waveform_path=vcd_path,
            session_name=session_name,
        )
        if vs_service.is_virtual(path, session):
            # Count transitions in the virtual signal's computed timeline.
            transitions = list(vs_service.iter_transitions(
                path, session, resolved_start, resolved_end,
            ))
            count = 0
            prev_v: Optional[str] = None
            for tr in transitions:
                v = tr["v"]
                if prev_v is not None and v != prev_v:
                    if edge_type == "anyedge":
                        count += 1
                    elif edge_type == "posedge" and prev_v == "0" and v == "1":
                        count += 1
                    elif edge_type == "negedge" and prev_v == "1" and v == "0":
                        count += 1
                prev_v = v
            result = {
                "status": "success",
                "data": {"count": count, "signal": path,
                         "requested_edge_type": edge_type,
                         "start_time": resolved_start, "end_time": resolved_end},
            }
            result["resolved_time_range"] = {"start": start_info, "end": end_info}
            return _with_session_metadata(result, session)
        import agent_debug_automation.agent_debug_automation_mcp as _wrapper
        result = _wrapper.wave_agent_query(
            session["waveform_path"],
            "count_transitions",
            {
                "path": path,
                "start_time": resolved_start,
                "end_time": resolved_end,
                "edge_type": edge_type,
            },
        )
        result["resolved_time_range"] = {"start": start_info, "end": end_info}
        return _with_session_metadata(result, session)
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def dump_waveform_data(
    vcd_path: Optional[str] = None,
    signals: Optional[List[str]] = None,
    start_time: TimeReference = 0,
    end_time: TimeReference = 0,
    output_path: str = "",
    mode: str = "transitions",
    sample_period: Optional[int] = None,
    radix: str = "hex",
    overwrite: bool = False,
    signals_are_groups: bool = False,
    session_name: Optional[str] = None,
):
    """Dump waveform data to a local JSONL file for offline processing."""
    try:
        expanded_signals, signal_info, signal_session = _expand_signal_groups(
            signals or [],
            signals_are_groups=signals_are_groups,
            waveform_path=vcd_path,
            session_name=session_name,
        )
        resolved_start, resolved_end, start_info, end_info, time_session = _resolve_time_range_reference(
            start_time,
            end_time,
            waveform_path=signal_session["waveform_path"] if signal_session else vcd_path,
            session_name=signal_session["session_name"] if signal_session else session_name,
        )
        session = time_session if signal_session is None else signal_session
        import agent_debug_automation.agent_debug_automation_mcp as _wrapper
        payload = {
            "signals": expanded_signals,
            "start_time": resolved_start,
            "end_time": resolved_end,
            "output_path": output_path,
            "mode": mode,
            "radix": radix,
            "overwrite": overwrite,
        }
        if sample_period is not None:
            payload["sample_period"] = sample_period
        result = _wrapper.wave_agent_query(
            session["waveform_path"],
            "dump_waveform_data",
            payload,
        )
        result["resolved_time_range"] = {"start": start_info, "end": end_info}
        return _with_session_metadata(result, session, None, signal_info)
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def get_signal_overview(
    vcd_path: Optional[str] = None,
    path: str = "",
    start_time: TimeReference = 0,
    end_time: TimeReference = 0,
    resolution: ResolutionReference = "auto",
    radix: str = "hex",
    session_name: Optional[str] = None,
):
    """Summarize one signal over a time window using a resolution-aware overview.

    For session-aware use:
    - start_time and end_time may be integers, "Cursor", or "BM_<bookmark_name>"
    - if vcd_path is omitted, the active Session selects the waveform
    - resolution may be an integer or "auto"
    """
    try:
        resolved_start, resolved_end, start_info, end_info, session = _resolve_time_range_reference(
            start_time,
            end_time,
            waveform_path=vcd_path,
            session_name=session_name,
        )
        if vs_service.is_virtual(path, session):
            # Evaluate using iter_transitions to get full timeline
            transitions = list(vs_service.iter_transitions(path, session, resolved_start, resolved_end))
            num_transitions = max(0, len(transitions) - 1)
            # Create a simple preview of the last few transitions
            preview = []
            for t in transitions[-5:]:
                preview.append({
                    "t": t["t"],
                    "v": _format_virtual_value_at_time(path, session, t["t"], radix),
                })
                
            result = {
                "status": "success",
                "data": {
                    "signal": path,
                    "type": "virtual",
                    "width": "unknown",
                    "transitions_in_window": num_transitions,
                    "start_time": resolved_start,
                    "end_time": resolved_end,
                    "preview": preview,
                }
            }
            result["resolved_time_range"] = {"start": start_info, "end": end_info}
            return _with_session_metadata(result, session)
            
        import agent_debug_automation.agent_debug_automation_mcp as _wrapper
        result = _wrapper.wave_agent_query(
            session["waveform_path"],
            "get_signal_overview",
            {
                "path": path,
                "start_time": resolved_start,
                "end_time": resolved_end,
                "resolution": resolution,
                "radix": radix,
            },
        )
        result["resolved_time_range"] = {"start": start_info, "end": end_info}
        return _with_session_metadata(result, session)
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def analyze_pattern(
    vcd_path: Optional[str] = None,
    path: str = "",
    start_time: TimeReference = 0,
    end_time: TimeReference = 0,
    session_name: Optional[str] = None,
):
    """Analyze a signal's behavior (e.g., identify clocks, static signals)."""
    try:
        resolved_start, resolved_end, start_info, end_info, session = _resolve_time_range_reference(
            start_time,
            end_time,
            waveform_path=vcd_path,
            session_name=session_name,
        )
        if vs_service.is_virtual(path, session):
            transitions = list(vs_service.iter_transitions(path, session, resolved_start, resolved_end))
            num_transitions = max(0, len(transitions) - 1)
            
            # Simple pattern analysis for virtual signals
            result_data = {
                "signal": path,
                "is_static": num_transitions == 0,
                "transitions": num_transitions,
                "time_range": {"start": resolved_start, "end": resolved_end}
            }
            if num_transitions > 1:
                intervals = [transitions[i]["t"] - transitions[i-1]["t"] for i in range(1, len(transitions))]
                min_int = min(intervals)
                max_int = max(intervals)
                result_data["min_interval"] = min_int
                result_data["max_interval"] = max_int
                result_data["is_periodic"] = min_int == max_int
            
            result = {"status": "success", "data": result_data}
            result["resolved_time_range"] = {"start": start_info, "end": end_info}
            return _with_session_metadata(result, session)

        import agent_debug_automation.agent_debug_automation_mcp as _wrapper
        result = _wrapper.wave_agent_query(
            session["waveform_path"],
            "analyze_pattern",
            {
                "path": path,
                "start_time": resolved_start,
                "end_time": resolved_end,
            },
        )
        result["resolved_time_range"] = {"start": start_info, "end": end_info}
        return _with_session_metadata(result, session)
    except Exception as e:
        return {"status": "error", "message": str(e)}


# ---------------------------------------------------------------------------
# Session CRUD tools
# ---------------------------------------------------------------------------

@mcp.tool()
def create_session(waveform_path: str, session_name: str, description: str = ""):
    """Create a waveform-bound Session.

    A Session is a saved waveform view for one waveform file. It stores:
    - the current Cursor time
    - named Bookmarks, which are saved times referenced as BM_<name>
    - named Signal Groups, which are saved signal lists

    The active Session is used when session-aware waveform tools omit vcd_path.
    """
    try:
        payload = _load_session_payload(waveform_path, session_name)
        if payload is not None:
            return {"status": "error", "message": f"session already exists: {session_name}"}
        payload = _save_session_payload(_default_session_payload(waveform_path, session_name=session_name, description=description))
        return _with_session_metadata({"status": "success", "data": _session_summary(payload)}, payload)
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def list_sessions(waveform_path: Optional[str] = None):
    """List saved Sessions, optionally filtered by waveform path."""
    try:
        invalid_sessions: List[Dict[str, str]] = []
        sessions = [_session_summary(item) for item in _list_session_payloads(waveform_path, diagnostics=invalid_sessions)]
        invalid_sessions.sort(key=lambda item: item["path"])
        active = _get_active_session_ref()
        return {"status": "success", "data": sessions, "active_session": active, "invalid_sessions": invalid_sessions}
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def get_session(session_name: Optional[str] = None, waveform_path: Optional[str] = None):
    """Get one Session or resolve the active/default Session for a waveform."""
    try:
        payload = _resolve_session(waveform_path=waveform_path, session_name=session_name)
        result = dict(payload)
        result["bookmarks"] = dict(payload["bookmarks"])
        result["signal_groups"] = dict(payload["signal_groups"])
        return _with_session_metadata({"status": "success", "data": result}, payload)
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def switch_session(session_name: str, waveform_path: Optional[str] = None):
    """Switch the active Session used by session-aware waveform tools."""
    try:
        payload = _resolve_session(waveform_path=waveform_path, session_name=session_name, create_default=False)
        active = _set_active_session_ref(payload["waveform_path"], payload["session_name"])
        return _with_session_metadata({"status": "success", "active_session": active}, payload)
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def delete_session(session_name: str, waveform_path: Optional[str] = None):
    """Delete a Session. Default_Session is retained for each waveform."""
    if session_name == DEFAULT_SESSION_NAME:
        return {"status": "error", "message": "cannot delete Default_Session"}
    try:
        payload = _resolve_session(waveform_path=waveform_path, session_name=session_name, create_default=False)
        if payload["session_name"] == DEFAULT_SESSION_NAME:
            return {"status": "error", "message": "cannot delete Default_Session"}
        path = _session_file_path(payload["waveform_path"], payload["session_name"])
        if path.exists():
            path.unlink()
        active = _get_active_session_ref()
        if active and active["waveform_path"] == payload["waveform_path"] and active["session_name"] == payload["session_name"]:
            default_session = _get_or_create_session(payload["waveform_path"], DEFAULT_SESSION_NAME)
            active = _set_active_session_ref(default_session["waveform_path"], default_session["session_name"])
        return {"status": "success", "deleted_session": _session_summary(payload), "active_session": active}
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def set_cursor(time: TimeReference, waveform_path: Optional[str] = None, session_name: Optional[str] = None):
    """Set the Session Cursor.

    The Cursor is the current focus time for a Session. Session-aware tools can
    refer to it by passing time="Cursor" instead of an integer timestamp.
    """
    try:
        resolved_time, time_info, session = _resolve_time_reference(time, waveform_path=waveform_path, session_name=session_name)
        session["cursor_time"] = max(0, int(resolved_time))
        session = _save_session_payload(session)
        return _with_session_metadata({"status": "success", "data": {"cursor_time": session["cursor_time"]}}, session, time_info)
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def move_cursor(delta: int, waveform_path: Optional[str] = None, session_name: Optional[str] = None):
    """Move the Session Cursor by a signed time delta."""
    try:
        session = _resolve_session(waveform_path=waveform_path, session_name=session_name)
        session["cursor_time"] = max(0, int(session["cursor_time"]) + int(delta))
        session = _save_session_payload(session)
        return _with_session_metadata(
            {"status": "success", "data": {"cursor_time": session["cursor_time"], "delta": int(delta)}},
            session,
            _time_resolution_info(f"cursor+={int(delta)}", session["cursor_time"], session),
        )
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def get_cursor(waveform_path: Optional[str] = None, session_name: Optional[str] = None):
    """Get the current Session Cursor time."""
    try:
        session = _resolve_session(waveform_path=waveform_path, session_name=session_name)
        return _with_session_metadata(
            {"status": "success", "data": {"cursor_time": int(session["cursor_time"])}},
            session,
            _time_resolution_info("Cursor", int(session["cursor_time"]), session),
        )
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def create_bookmark(
    bookmark_name: str,
    time: TimeReference,
    description: str = "",
    waveform_path: Optional[str] = None,
    session_name: Optional[str] = None,
):
    """Create or replace a Bookmark in the selected Session.

    A Bookmark is a named saved time. Session-aware tools can refer to it by
    passing time="BM_<bookmark_name>" instead of an integer timestamp.
    """
    try:
        resolved_time, time_info, session = _resolve_time_reference(time, waveform_path=waveform_path, session_name=session_name)
        key = _validate_named_entity(bookmark_name, "bookmark_name")
        session["bookmarks"][key] = {"time": int(resolved_time), "description": description}
        session = _save_session_payload(session)
        return _with_session_metadata({"status": "success", "data": {"bookmark_name": key, **session["bookmarks"][key]}}, session, time_info)
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def delete_bookmark(bookmark_name: str, waveform_path: Optional[str] = None, session_name: Optional[str] = None):
    """Delete a Bookmark from the selected Session."""
    try:
        session = _resolve_session(waveform_path=waveform_path, session_name=session_name)
        key = _validate_named_entity(bookmark_name, "bookmark_name")
        if key not in session["bookmarks"]:
            return {"status": "error", "message": f"bookmark not found: {key}"}
        deleted = session["bookmarks"].pop(key)
        session = _save_session_payload(session)
        return _with_session_metadata({"status": "success", "data": {"bookmark_name": key, **deleted}}, session)
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def list_bookmarks(waveform_path: Optional[str] = None, session_name: Optional[str] = None):
    """List Bookmarks for the selected Session."""
    try:
        session = _resolve_session(waveform_path=waveform_path, session_name=session_name)
        bookmarks = [
            {"bookmark_name": name, "time": int(item["time"]), "description": item.get("description", "")}
            for name, item in sorted(session["bookmarks"].items())
        ]
        return _with_session_metadata({"status": "success", "data": bookmarks}, session)
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def create_signal_group(
    group_name: str,
    signals: List[str],
    description: str = "",
    waveform_path: Optional[str] = None,
    session_name: Optional[str] = None,
):
    """Create or replace a Signal Group in the selected Session.

    A Signal Group is a named saved list of signals. Session-aware snapshot
    tools can expand groups by passing signals_are_groups=True.
    """
    try:
        session = _resolve_session(waveform_path=waveform_path, session_name=session_name)
        key = _validate_named_entity(group_name, "group_name")
        ordered = list(dict.fromkeys(signals))
        session["signal_groups"][key] = {"signals": ordered, "description": description}
        session = _save_session_payload(session)
        return _with_session_metadata({"status": "success", "data": {"group_name": key, **session["signal_groups"][key]}}, session)
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def update_signal_group(
    group_name: str,
    signals: Optional[List[str]] = None,
    description: Optional[str] = None,
    waveform_path: Optional[str] = None,
    session_name: Optional[str] = None,
):
    """Update a named Signal Group in the selected Session."""
    try:
        session = _resolve_session(waveform_path=waveform_path, session_name=session_name)
        key = _validate_named_entity(group_name, "group_name")
        group = session["signal_groups"].get(key)
        if group is None:
            return {"status": "error", "message": f"signal group not found: {key}"}
        if signals is not None:
            group["signals"] = list(dict.fromkeys(signals))
        if description is not None:
            group["description"] = description
        session["signal_groups"][key] = group
        session = _save_session_payload(session)
        return _with_session_metadata({"status": "success", "data": {"group_name": key, **group}}, session)
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def delete_signal_group(group_name: str, waveform_path: Optional[str] = None, session_name: Optional[str] = None):
    """Delete a named Signal Group from the selected Session."""
    try:
        session = _resolve_session(waveform_path=waveform_path, session_name=session_name)
        key = _validate_named_entity(group_name, "group_name")
        if key not in session["signal_groups"]:
            return {"status": "error", "message": f"signal group not found: {key}"}
        deleted = session["signal_groups"].pop(key)
        session = _save_session_payload(session)
        return _with_session_metadata({"status": "success", "data": {"group_name": key, **deleted}}, session)
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def list_signal_groups(waveform_path: Optional[str] = None, session_name: Optional[str] = None):
    """List Signal Groups for the selected Session."""
    try:
        session = _resolve_session(waveform_path=waveform_path, session_name=session_name)
        groups = [
            {"group_name": name, "signals": list(item.get("signals", [])), "description": item.get("description", "")}
            for name, item in sorted(session["signal_groups"].items())
        ]
        return _with_session_metadata({"status": "success", "data": groups}, session)
    except Exception as e:
        return {"status": "error", "message": str(e)}


# ---------------------------------------------------------------------------
# Virtual signal expression tools
# ---------------------------------------------------------------------------

@mcp.tool()
def create_signal_expression(
    signal_name: str,
    expression: str,
    description: str = "",
    waveform_path: Optional[str] = None,
    session_name: Optional[str] = None,
):
    """Create a virtual signal from a Verilog-like expression.

    The virtual signal can be used with all session-aware waveform tools
    (get_transitions, get_value_at_time, get_snapshot, find_edge, etc.).

    Supported operators (standard Verilog):
      Unary: ~ ! & | ^ ~& ~| ~^ ^~ (reduction when unary prefix)
      Binary: & | ^ ~& ~| ~^ && || == != < > <= >= << >> <<< >>> + - * / % **
      Ternary: ? :
      Custom extensions: binary ~& ~| ~^ (e.g., a ~& b = ~(a & b)),
                         = inside ternary (a ? b = c : d treated as a ? c : d)

    Example:
      create_signal_expression("aw_sent", "awvalid & awready")
      create_signal_expression("bus_sum", "bus_a + bus_b")
    """
    try:
        session = _resolve_session(waveform_path=waveform_path, session_name=session_name)
        session = vs_service.create(session, signal_name, expression, description)
        info = vs_service.get_info(signal_name, session)
        return _with_session_metadata({"status": "success", "data": info}, session)
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def update_signal_expression(
    signal_name: str,
    expression: Optional[str] = None,
    description: Optional[str] = None,
    waveform_path: Optional[str] = None,
    session_name: Optional[str] = None,
):
    """Update the expression or description of an existing virtual signal."""
    try:
        session = _resolve_session(waveform_path=waveform_path, session_name=session_name)
        session = vs_service.update(session, signal_name, expression=expression, description=description)
        info = vs_service.get_info(signal_name, session)
        return _with_session_metadata({"status": "success", "data": info}, session)
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def delete_signal_expression(
    signal_name: str,
    waveform_path: Optional[str] = None,
    session_name: Optional[str] = None,
):
    """Delete a virtual signal from the session."""
    try:
        session = _resolve_session(waveform_path=waveform_path, session_name=session_name)
        session = vs_service.delete(session, signal_name)
        return _with_session_metadata({"status": "success", "data": {"deleted": signal_name}}, session)
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def list_signal_expressions(
    waveform_path: Optional[str] = None,
    session_name: Optional[str] = None,
):
    """List all virtual signals defined in the session."""
    try:
        session = _resolve_session(waveform_path=waveform_path, session_name=session_name)
        virtuals = vs_service.list(session)
        return _with_session_metadata({"status": "success", "data": virtuals}, session)
    except Exception as e:
        return {"status": "error", "message": str(e)}


# ---------------------------------------------------------------------------
# Cross-link analysis tools
# ---------------------------------------------------------------------------

@mcp.tool()
def trace_with_snapshot(
    db_path: str,
    waveform_path: Optional[str] = None,
    signal: str = "",
    time: TimeReference = 0,
    mode: TraceMode = "drivers",
    trace_options: Optional[TraceOptions] = None,
    sample_offsets: Optional[List[int]] = None,
    clock_path: Optional[str] = None,
    cycle_offsets: Optional[List[int]] = None,
    rank_window_before: Optional[int] = None,
    rank_window_after: Optional[int] = None,
    rtl_trace_bin: Optional[str] = None,
    wave_cli_bin: Optional[str] = None,
    session_name: Optional[str] = None,
):
    """Trace a signal structurally and return cone values at time T plus optional T+/-offset samples."""
    try:
        resolved_time, time_info, session = _resolve_time_reference(
            time,
            waveform_path=waveform_path,
            session_name=session_name,
        )
        context = _build_common_context(
            db_path=db_path,
            waveform_path=session["waveform_path"],
            signal=signal,
            time=resolved_time,
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
        extra_times = [resolved_time + int(offset) for offset in (sample_offsets or [])]
        absolute_samples, sample_unmapped = _sample_signal_times(
            session["waveform_path"],
            cone_signals,
            extra_times,
            wave_cli_bin=wave_cli_bin,
        )
        cycle_data: Dict[str, Any] = {}
        if clock_path and cycle_offsets:
            resolved_clock = _map_signal_to_waveform(session["waveform_path"], clock_path, wave_cli_bin=wave_cli_bin)
            if resolved_clock is None:
                context["warnings"].append(f"clock path not found in waveform: {clock_path}")
            else:
                cycle_times, cycle_warnings = _cycle_sample_times(
                    session["waveform_path"],
                    resolved_time,
                    resolved_clock,
                    [int(offset) for offset in cycle_offsets],
                    wave_cli_bin=wave_cli_bin,
                )
                context["warnings"].extend(cycle_warnings)
                valid_times = [t for t in cycle_times.values() if t is not None]
                cycle_samples, cycle_unmapped = _sample_signal_times(
                    session["waveform_path"],
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
        return _with_session_metadata(context, session, time_info)
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def explain_signal_at_time(
    db_path: str,
    waveform_path: Optional[str] = None,
    signal: str = "",
    time: TimeReference = 0,
    mode: TraceMode = "drivers",
    trace_options: Optional[TraceOptions] = None,
    rank_window_before: Optional[int] = None,
    rank_window_after: Optional[int] = None,
    rtl_trace_bin: Optional[str] = None,
    wave_cli_bin: Optional[str] = None,
    session_name: Optional[str] = None,
):
    """Explain which structural drivers are most correlated with the signal state at time T."""
    try:
        resolved_time, time_info, session = _resolve_time_reference(
            time,
            waveform_path=waveform_path,
            session_name=session_name,
        )
        context = _build_common_context(
            db_path=db_path,
            waveform_path=session["waveform_path"],
            signal=signal,
            time=resolved_time,
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
        return _with_session_metadata(context, session, time_info)
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def rank_cone_by_time(
    db_path: str,
    waveform_path: Optional[str] = None,
    signal: str = "",
    time: TimeReference = 0,
    mode: TraceMode = "drivers",
    trace_options: Optional[TraceOptions] = None,
    window_start: Optional[int] = None,
    window_end: Optional[int] = None,
    rtl_trace_bin: Optional[str] = None,
    wave_cli_bin: Optional[str] = None,
    session_name: Optional[str] = None,
):
    """Rank cone signals by recent activity and stuckness near time T."""
    try:
        resolved_time, time_info, session = _resolve_time_reference(
            time,
            waveform_path=waveform_path,
            session_name=session_name,
        )
        if window_start is None:
            window_start = max(0, resolved_time - DEFAULT_RANK_WINDOW_BEFORE)
        if window_end is None:
            window_end = resolved_time + DEFAULT_RANK_WINDOW_AFTER
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
            session["waveform_path"],
            cone_signals,
            mode=mode,
            focus_time=resolved_time,
            start_time=window_start,
            end_time=window_end,
            wave_cli_bin=wave_cli_bin,
        )
        return _with_session_metadata({
            "status": "success",
            "target": signal,
            "time_context": {
                "focus_time": resolved_time,
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
        }, session, time_info)
    except Exception as e:
        return {"status": "error", "message": str(e)}


@mcp.tool()
def explain_edge_cause(
    db_path: str,
    waveform_path: Optional[str] = None,
    signal: str = "",
    time: TimeReference = 0,
    edge_type: EdgeType | EdgeTypeAlias = "anyedge",
    direction: Direction = "backward",
    mode: TraceMode = "drivers",
    trace_options: Optional[TraceOptions] = None,
    rank_window_before: Optional[int] = None,
    rank_window_after: Optional[int] = None,
    rtl_trace_bin: Optional[str] = None,
    wave_cli_bin: Optional[str] = None,
    session_name: Optional[str] = None,
):
    """Resolve the relevant edge for a signal, then explain the upstream cause chain at that edge."""
    try:
        edge_type = _normalize_edge_type(edge_type)
        resolved_time, time_info, session = _resolve_time_reference(
            time,
            waveform_path=waveform_path,
            session_name=session_name,
        )
        mapped_signal = _map_signal_to_waveform(session["waveform_path"], signal, wave_cli_bin=wave_cli_bin)
        if mapped_signal is None:
            return {"status": "error", "message": f"signal not found in waveform: {signal}"}
        edge_result = _wave_query(
            session["waveform_path"],
            "find_edge",
            {
                "path": mapped_signal,
                "edge_type": edge_type,
                "start_time": int(resolved_time),
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
                "message": f"no {edge_type} edge found for {signal} near {resolved_time}",
            }
        edge_time = int(edge_time)
        # Use late import to go through the wrapper module so mock.patch works
        import agent_debug_automation.agent_debug_automation_mcp as _wrapper
        context = _wrapper.explain_signal_at_time(
            db_path=db_path,
            waveform_path=session["waveform_path"],
            signal=signal,
            time=edge_time,
            mode=mode,
            trace_options=trace_options,
            rank_window_before=rank_window_before,
            rank_window_after=rank_window_after,
            rtl_trace_bin=rtl_trace_bin,
            wave_cli_bin=wave_cli_bin,
            session_name=session["session_name"],
        )
        if context.get("status") != "success":
            return context

        before_time = max(0, edge_time - 1)
        before_after = _get_batch_snapshot(
            session["waveform_path"],
            [mapped_signal],
            edge_time,
            wave_cli_bin=wave_cli_bin,
        )
        before_snapshot = _get_batch_snapshot(
            session["waveform_path"],
            [mapped_signal],
            before_time,
            wave_cli_bin=wave_cli_bin,
        )
        context["time_context"]["requested_time"] = resolved_time
        context["time_context"]["resolved_edge_time"] = edge_time
        context["time_context"]["edge_type"] = edge_type
        context["waveform"]["edge_context"] = {
            "signal": signal,
            "mapped_signal": mapped_signal,
            "value_before_edge": before_snapshot.get(mapped_signal),
            "value_at_edge": before_after.get(mapped_signal),
        }
        return _with_session_metadata(context, session, time_info)
    except Exception as e:
        return {"status": "error", "message": str(e)}
