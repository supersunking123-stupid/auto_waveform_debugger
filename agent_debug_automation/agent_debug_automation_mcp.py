import atexit
import json
import os
import queue
import shlex
import subprocess
import threading
import uuid
import hashlib
import re
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Literal, Optional, Sequence, Tuple, TypedDict

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
DEFAULT_BACKEND_READ_TIMEOUT_SEC = 300
SESSION_STORE_DIR = ROOT_DIR / "agent_debug_automation" / ".session_store"
ACTIVE_SESSION_FILE = SESSION_STORE_DIR / "active_session.json"
DEFAULT_SESSION_NAME = "Default_Session"
runtime_state_lock = threading.RLock()


class TraceOptions(TypedDict, total=False):
    cone_level: int
    depth: int
    max_nodes: int
    include: str | List[str]
    exclude: str | List[str]
    stop_at: str | List[str]
    prefer_port_hop: bool


EdgeType = Literal["posedge", "negedge", "anyedge"]
EdgeTypeAlias = Literal["rise", "rising", "risingedge", "fall", "falling", "fallingedge", "edge", "any"]
Direction = Literal["forward", "backward"]
TraceMode = Literal["drivers", "loads"]
TimeReference = int | str
ResolutionReference = int | str


def _utc_now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def _normalize_waveform_path(path: str) -> str:
    return str(Path(path).expanduser().resolve())


def _ensure_session_store() -> None:
    SESSION_STORE_DIR.mkdir(parents=True, exist_ok=True)


def _session_waveform_id(waveform_path: str) -> str:
    normalized = _normalize_waveform_path(waveform_path)
    return hashlib.sha1(normalized.encode("utf-8")).hexdigest()[:16]


def _safe_session_name(session_name: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_.-]+", "_", session_name.strip())
    return cleaned or "session"


def _session_file_path(waveform_path: str, session_name: str) -> Path:
    prefix = _session_waveform_id(waveform_path)
    return SESSION_STORE_DIR / f"{prefix}__{_safe_session_name(session_name)}.json"


def _read_json_file(path: Path) -> Dict[str, Any]:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def _write_json_file(path: Path, payload: Dict[str, Any]) -> None:
    tmp_path = path.with_suffix(path.suffix + ".tmp")
    with tmp_path.open("w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2, sort_keys=True, ensure_ascii=False)
        f.write("\n")
    tmp_path.replace(path)


def _validate_session_name(session_name: str) -> str:
    name = session_name.strip()
    if not name:
        raise ValueError("session_name must not be empty")
    return name


def _validate_named_entity(name: str, field_name: str) -> str:
    value = name.strip()
    if not value:
        raise ValueError(f"{field_name} must not be empty")
    if value == "Cursor":
        raise ValueError(f"{field_name} cannot be Cursor")
    return value


def _default_session_payload(waveform_path: str, session_name: str = DEFAULT_SESSION_NAME, description: str = "") -> Dict[str, Any]:
    normalized_waveform = _normalize_waveform_path(waveform_path)
    now = _utc_now_iso()
    return {
        "session_name": _validate_session_name(session_name),
        "description": description,
        "waveform_path": normalized_waveform,
        "cursor_time": 0,
        "bookmarks": {},
        "signal_groups": {},
        "created_at": now,
        "updated_at": now,
    }


def _normalize_session_payload(payload: Dict[str, Any]) -> Dict[str, Any]:
    normalized = dict(payload)
    normalized["session_name"] = _validate_session_name(str(normalized.get("session_name", "")))
    normalized["waveform_path"] = _normalize_waveform_path(str(normalized.get("waveform_path", "")))
    normalized["description"] = str(normalized.get("description", ""))
    normalized["cursor_time"] = int(normalized.get("cursor_time", 0))
    normalized["bookmarks"] = dict(normalized.get("bookmarks", {}))
    normalized["signal_groups"] = dict(normalized.get("signal_groups", {}))
    normalized["created_at"] = str(normalized.get("created_at", _utc_now_iso()))
    normalized["updated_at"] = str(normalized.get("updated_at", normalized["created_at"]))
    return normalized


def _save_session_payload(payload: Dict[str, Any]) -> Dict[str, Any]:
    _ensure_session_store()
    normalized = _normalize_session_payload(payload)
    normalized["updated_at"] = _utc_now_iso()
    path = _session_file_path(normalized["waveform_path"], normalized["session_name"])
    _write_json_file(path, normalized)
    return normalized


def _load_session_payload(waveform_path: str, session_name: str) -> Optional[Dict[str, Any]]:
    path = _session_file_path(waveform_path, session_name)
    if not path.exists():
        return None
    return _normalize_session_payload(_read_json_file(path))


def _list_session_payloads(
    waveform_path: Optional[str] = None,
    diagnostics: Optional[List[Dict[str, str]]] = None,
) -> List[Dict[str, Any]]:
    _ensure_session_store()
    normalized_waveform = None if waveform_path is None else _normalize_waveform_path(waveform_path)
    sessions: List[Dict[str, Any]] = []
    for path in SESSION_STORE_DIR.glob("*.json"):
        if path == ACTIVE_SESSION_FILE:
            continue
        try:
            payload = _normalize_session_payload(_read_json_file(path))
        except Exception as exc:
            if diagnostics is not None:
                diagnostics.append({"path": str(path), "message": str(exc)})
            continue
        if normalized_waveform is not None and payload["waveform_path"] != normalized_waveform:
            continue
        sessions.append(payload)
    sessions.sort(key=lambda item: (item["waveform_path"], item["session_name"]))
    return sessions


def _get_active_session_ref() -> Optional[Dict[str, str]]:
    if not ACTIVE_SESSION_FILE.exists():
        return None
    try:
        payload = _read_json_file(ACTIVE_SESSION_FILE)
    except Exception:
        return None
    session_name = str(payload.get("session_name", "")).strip()
    waveform_path = str(payload.get("waveform_path", "")).strip()
    if not session_name or not waveform_path:
        return None
    return {
        "session_name": session_name,
        "waveform_path": _normalize_waveform_path(waveform_path),
    }


def _set_active_session_ref(waveform_path: str, session_name: str) -> Dict[str, str]:
    _ensure_session_store()
    payload = {
        "session_name": _validate_session_name(session_name),
        "waveform_path": _normalize_waveform_path(waveform_path),
        "updated_at": _utc_now_iso(),
    }
    _write_json_file(ACTIVE_SESSION_FILE, payload)
    return payload


def _clear_active_session_ref() -> None:
    if ACTIVE_SESSION_FILE.exists():
        ACTIVE_SESSION_FILE.unlink()


def _get_or_create_session(waveform_path: str, session_name: str = DEFAULT_SESSION_NAME, description: str = "") -> Dict[str, Any]:
    normalized_waveform = _normalize_waveform_path(waveform_path)
    existing = _load_session_payload(normalized_waveform, session_name)
    if existing is not None:
        return existing
    return _save_session_payload(_default_session_payload(normalized_waveform, session_name=session_name, description=description))


def _resolve_session(
    waveform_path: Optional[str] = None,
    session_name: Optional[str] = None,
    create_default: bool = True,
    set_active_on_create: bool = True,
) -> Dict[str, Any]:
    active_ref = _get_active_session_ref()
    normalized_waveform = None if waveform_path is None else _normalize_waveform_path(waveform_path)

    if session_name:
        if normalized_waveform is None:
            if active_ref and active_ref["session_name"] == session_name:
                normalized_waveform = active_ref["waveform_path"]
            else:
                matches = [item for item in _list_session_payloads() if item["session_name"] == session_name]
                if not matches:
                    raise ValueError(f"session not found: {session_name}")
                if len(matches) > 1:
                    raise ValueError(f"session_name is ambiguous without waveform_path: {session_name}")
                normalized_waveform = matches[0]["waveform_path"]
        payload = _load_session_payload(normalized_waveform, session_name)
        if payload is None:
            raise ValueError(f"session not found: {session_name}")
        return payload

    if normalized_waveform is None:
        if not active_ref:
            raise ValueError("waveform_path or an active session is required")
        payload = _load_session_payload(active_ref["waveform_path"], active_ref["session_name"])
        if payload is None:
            raise ValueError("active session reference is stale")
        return payload

    if active_ref and active_ref["waveform_path"] == normalized_waveform:
        payload = _load_session_payload(normalized_waveform, active_ref["session_name"])
        if payload is not None:
            return payload

    if not create_default:
        raise ValueError(f"no active session for waveform: {normalized_waveform}")

    payload = _get_or_create_session(normalized_waveform, DEFAULT_SESSION_NAME)
    if set_active_on_create and not active_ref:
        _set_active_session_ref(normalized_waveform, payload["session_name"])
    return payload


def _session_summary(payload: Dict[str, Any]) -> Dict[str, Any]:
    return {
        "session_name": payload["session_name"],
        "description": payload["description"],
        "waveform_path": payload["waveform_path"],
        "cursor_time": payload["cursor_time"],
        "bookmark_names": sorted(payload["bookmarks"].keys()),
        "signal_group_names": sorted(payload["signal_groups"].keys()),
        "created_at": payload["created_at"],
        "updated_at": payload["updated_at"],
    }


def _time_resolution_info(input_value: TimeReference, resolved_time: int, session: Dict[str, Any]) -> Dict[str, Any]:
    return {
        "input": input_value,
        "resolved_time": resolved_time,
        "session_name": session["session_name"],
        "waveform_path": session["waveform_path"],
    }


def _resolve_time_reference(
    time_value: TimeReference,
    waveform_path: Optional[str] = None,
    session_name: Optional[str] = None,
) -> Tuple[int, Dict[str, Any], Dict[str, Any]]:
    session = _resolve_session(waveform_path=waveform_path, session_name=session_name)
    if isinstance(time_value, int):
        return int(time_value), _time_resolution_info(time_value, int(time_value), session), session

    raw = str(time_value).strip()
    if not raw:
        raise ValueError("time reference must not be empty")
    if raw == "Cursor":
        resolved = int(session["cursor_time"])
        return resolved, _time_resolution_info(raw, resolved, session), session
    if raw.startswith("BM_"):
        bookmark_name = raw[3:]
        bookmark = session["bookmarks"].get(bookmark_name)
        if bookmark is None:
            raise ValueError(f"bookmark not found: {bookmark_name}")
        resolved = int(bookmark["time"])
        return resolved, _time_resolution_info(raw, resolved, session), session
    try:
        resolved = int(raw)
    except ValueError as exc:
        raise ValueError(f"unsupported time reference: {time_value}") from exc
    return resolved, _time_resolution_info(raw, resolved, session), session


def _resolve_time_range_reference(
    start_time: TimeReference,
    end_time: TimeReference,
    waveform_path: Optional[str] = None,
    session_name: Optional[str] = None,
) -> Tuple[int, int, Dict[str, Any], Dict[str, Any], Dict[str, Any]]:
    start_resolved, start_info, session = _resolve_time_reference(
        start_time,
        waveform_path=waveform_path,
        session_name=session_name,
    )
    end_resolved, end_info, _ = _resolve_time_reference(
        end_time,
        waveform_path=session["waveform_path"],
        session_name=session["session_name"],
    )
    return start_resolved, end_resolved, start_info, end_info, session


def _expand_signal_groups(
    signals: Sequence[str],
    signals_are_groups: bool,
    waveform_path: Optional[str] = None,
    session_name: Optional[str] = None,
) -> Tuple[List[str], Dict[str, Any], Optional[Dict[str, Any]]]:
    if not signals_are_groups:
        return list(signals), {"signals_are_groups": False, "group_names": [], "expanded_signals": list(signals)}, None

    session = _resolve_session(waveform_path=waveform_path, session_name=session_name)
    expanded: List[str] = []
    seen = set()
    group_names: List[str] = []
    for group_name in signals:
        group_key = _validate_named_entity(group_name, "group_name")
        group = session["signal_groups"].get(group_key)
        if group is None:
            raise ValueError(f"signal group not found: {group_key}")
        group_names.append(group_key)
        for signal in group.get("signals", []):
            if signal not in seen:
                seen.add(signal)
                expanded.append(signal)
    return expanded, {
        "signals_are_groups": True,
        "group_names": group_names,
        "expanded_signals": expanded,
    }, session


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


def _read_line_with_timeout(stream: Any, timeout_sec: float) -> str:
    result_queue: queue.Queue = queue.Queue(maxsize=1)

    def _reader() -> None:
        try:
            result_queue.put((True, stream.readline()))
        except Exception as exc:
            result_queue.put((False, exc))

    reader = threading.Thread(target=_reader, daemon=True)
    reader.start()
    try:
        ok, payload = result_queue.get(timeout=timeout_sec)
    except queue.Empty as exc:
        raise TimeoutError(f"backend read timed out after {timeout_sec}s") from exc
    if ok:
        return payload
    raise payload


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


def _stop_process(process: subprocess.Popen[Any]) -> None:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pass
    for stream_name in ("stdin", "stdout", "stderr"):
        stream = getattr(process, stream_name, None)
        if stream:
            try:
                stream.close()
            except Exception:
                pass


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

        if self.process.stdout is None:
            return {"status": "error", "message": "rtl_trace serve stdout is unavailable"}
        out_lines: List[str] = []
        while True:
            try:
                s = _read_line_with_timeout(self.process.stdout, DEFAULT_BACKEND_READ_TIMEOUT_SEC)
            except TimeoutError as exc:
                self.stop()
                return {"status": "error", "message": str(exc), "stdout": "".join(out_lines)}
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
            if self.process.stdin is None or self.process.stdout is None:
                return {"status": "error", "message": "rtl_trace serve pipes are unavailable"}
            self.process.stdin.write(line + "\n")
            self.process.stdin.flush()
            return self._read_until_end()
        except TimeoutError as e:
            self.stop()
            return {"status": "error", "message": str(e)}
        except Exception as e:
            return {"status": "error", "message": str(e)}

    def stop(self) -> Dict[str, Any]:
        _stop_process(self.process)
        return {"status": "success"}


rtl_serve_sessions: Dict[str, RtlTraceServeSession] = {}
rtl_session_ids_by_key: Dict[Tuple[str, str], str] = {}


def _cleanup_runtime_state() -> None:
    with runtime_state_lock:
        sessions = list(rtl_serve_sessions.values())
        daemons = list(wave_daemons.values())
        rtl_serve_sessions.clear()
        rtl_session_ids_by_key.clear()
        wave_daemons.clear()
        wave_signal_cache.clear()
        wave_signal_resolution_cache.clear()
        wave_prefix_page_cache.clear()

    for session in sessions:
        try:
            session.stop()
        except Exception:
            pass

    for daemon in daemons:
        try:
            daemon.stop()
        except Exception:
            pass


atexit.register(_cleanup_runtime_state)


def _extract_db_path_from_serve_args(serve_args: Sequence[str]) -> Optional[str]:
    for i, arg in enumerate(serve_args):
        if arg == "--db" and i + 1 < len(serve_args):
            return str(Path(serve_args[i + 1]).expanduser().resolve())
        if arg.startswith("--db="):
            value = arg.split("=", 1)[1]
            if value:
                return str(Path(value).expanduser().resolve())
    return None


def _get_rtl_serve_session(db_path: str, rtl_trace_bin: Optional[str] = None) -> RtlTraceServeSession:
    resolved_db = str(Path(db_path).expanduser().resolve())
    bin_path = _resolve_bin(rtl_trace_bin, DEFAULT_RTL_TRACE_BIN, "rtl_trace")
    key = (bin_path, resolved_db)
    with runtime_state_lock:
        sid = rtl_session_ids_by_key.get(key)
        if sid:
            sess = rtl_serve_sessions.get(sid)
            if sess and sess.process.poll() is None:
                return sess
            rtl_session_ids_by_key.pop(key, None)
            if sid in rtl_serve_sessions:
                rtl_serve_sessions.pop(sid, None)
        session_id = str(uuid.uuid4())
        sess = RtlTraceServeSession(bin_path, ["--db", resolved_db])
        if sess.startup.get("status") != "success":
            raise RuntimeError(sess.startup.get("message", "failed to start rtl_trace serve"))
        rtl_serve_sessions[session_id] = sess
        rtl_session_ids_by_key[key] = session_id
        return sess


def _append_trace_options(args: List[str], trace_options: Optional[TraceOptions]) -> None:
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
    mode: TraceMode = "drivers",
    trace_options: Optional[TraceOptions] = None,
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
            if self.process.stdin is None or self.process.stdout is None:
                return {"status": "error", "message": "wave_agent_cli daemon pipes are unavailable"}
            self.process.stdin.write(query_str + "\n")
            self.process.stdin.flush()
            line = _read_line_with_timeout(self.process.stdout, DEFAULT_BACKEND_READ_TIMEOUT_SEC)
            if not line:
                err = self.process.stderr.read() if self.process.stderr else ""
                return {"status": "error", "message": f"wave_agent_cli daemon crashed: {err}"}
            return json.loads(line)
        except TimeoutError as e:
            self.stop()
            return {"status": "error", "message": str(e)}
        except Exception as e:
            return {"status": "error", "message": str(e)}

    def stop(self):
        _stop_process(self.process)


wave_daemons: Dict[str, WaveformDaemon] = {}
wave_signal_cache: Dict[str, List[str]] = {}
wave_signal_resolution_cache: Dict[Tuple[str, str], Optional[str]] = {}
wave_prefix_page_cache: Dict[Tuple[str, str], List[str]] = {}


def _get_wave_daemon(vcd_path: str, wave_cli: Optional[str] = None) -> WaveformDaemon:
    normalized = str(Path(vcd_path).expanduser().resolve())
    with runtime_state_lock:
        daemon = wave_daemons.get(normalized)
        if daemon is not None and daemon.process.poll() is None:
            return daemon
        if daemon is not None:
            daemon.stop()
            wave_daemons.pop(normalized, None)
        if not os.path.exists(normalized):
            raise FileNotFoundError(f"waveform file not found: {normalized}")
        cli_path = _resolve_bin(wave_cli, DEFAULT_WAVE_CLI, "wave_agent_cli")
        wave_daemons[normalized] = WaveformDaemon(cli_path, normalized)
        return wave_daemons[normalized]


def _wave_query(vcd_path: str, cmd: str, args: Optional[dict] = None, wave_cli_bin: Optional[str] = None) -> Dict[str, Any]:
    return _get_wave_daemon(vcd_path, wave_cli=wave_cli_bin).query(cmd, args or {})


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
    snapshot = dict(result.get("data", {}))
    values: Dict[str, Any] = {}
    for signal, sample in snapshot.items():
        if isinstance(sample, dict) and "value" in sample:
            values[signal] = sample.get("value")
        else:
            values[signal] = sample
    return values


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


def _with_session_metadata(
    payload: Dict[str, Any],
    session: Optional[Dict[str, Any]] = None,
    time_context: Optional[Dict[str, Any]] = None,
    signal_context: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    if session is not None:
        payload["session"] = _session_summary(session)
    if time_context is not None:
        payload["resolved_time"] = time_context
    if signal_context is not None:
        payload["resolved_signals"] = signal_context
    return payload


def _require_waveform_path_from_session(
    waveform_path: Optional[str] = None,
    session_name: Optional[str] = None,
) -> Tuple[str, Dict[str, Any]]:
    session = _resolve_session(waveform_path=waveform_path, session_name=session_name)
    return session["waveform_path"], session


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
        result = wave_agent_query(
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
        result = wave_agent_query(resolved_waveform, "get_signal_info", {"path": path})
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
        result = wave_agent_query(
            session["waveform_path"],
            "get_snapshot",
            {"signals": expanded_signals, "time": resolved_time, "radix": radix},
        )
        return _with_session_metadata(result, session, time_info, signal_info)
    except Exception as e:
        return {"status": "error", "message": str(e)}


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
        result = wave_agent_query(
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
        result = wave_agent_query(
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
        result = wave_agent_query(
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
        result = wave_agent_query(
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
        result = wave_agent_query(
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
        result = wave_agent_query(
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
        result = wave_agent_query(
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
        context = explain_signal_at_time(
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


if __name__ == "__main__":
    mcp.run()
