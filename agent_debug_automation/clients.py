"""wave_agent_cli and rtl_trace subprocess wrappers."""

import atexit
import json
import os
import queue
import select
import shlex
import subprocess
import threading
import time
import uuid
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

from .mapping import runtime_state_lock, wave_signal_cache, wave_signal_resolution_cache, wave_prefix_page_cache
from .models import (
    DEFAULT_BACKEND_READ_TIMEOUT_SEC,
    DEFAULT_RTL_TRACE_BIN,
    DEFAULT_WAVE_CLI,
    TraceMode,
    TraceOptions,
)


# ---------------------------------------------------------------------------
# Generic helpers
# ---------------------------------------------------------------------------

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


def _terminate_process(process: subprocess.Popen) -> None:
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


def _close_process_pipes(process: subprocess.Popen) -> None:
    for stream_name in ("stdin", "stdout", "stderr"):
        stream = getattr(process, stream_name, None)
        if stream:
            try:
                stream.close()
            except Exception:
                pass


def _stop_process(process: subprocess.Popen) -> None:
    _terminate_process(process)
    _close_process_pipes(process)


class _StderrPump:
    """Continuously drains a child's stderr pipe into a buffer on a daemon thread.

    Without it a chatty child (e.g. serve echoing a 70 KB invalid regex) fills the pipe and
    blocks before it can write its stdout terminator. The pump reads the raw fd under
    ``_lock``, so a caller holding the lock that sees the pipe empty knows every byte written
    so far is in the buffer. The text-mode wrapper around the pipe must not be read directly.
    """

    _POLL_MS = 200
    _READ_SIZE = 65536

    def __init__(self, stream: Any):
        self._fd = stream.fileno()
        # The pump is the only reader; non-blocking so a read under the lock can never stall.
        os.set_blocking(self._fd, False)
        # poll(), not select(): select() rejects descriptors >= FD_SETSIZE (1024), which a
        # long-running server with many open files hands out. One poll object per thread,
        # since CPython refuses concurrent poll() calls on the same object.
        self._pump_poller = self._new_poller()
        self._settle_poller = self._new_poller()
        self._lock = threading.Lock()
        self._chunks: List[bytes] = []
        self._eof = False
        self._error: Optional[str] = None
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="rtl-trace-serve-stderr", daemon=True)
        self._thread.start()

    def _new_poller(self) -> Any:
        poller = select.poll()
        poller.register(self._fd, select.POLLIN)  # POLLHUP/POLLERR/POLLNVAL are always reported
        return poller

    def _read_chunk(self) -> bytes:
        return os.read(self._fd, self._READ_SIZE)

    def _run(self) -> None:
        try:
            while not self._stop.is_set():
                if not self._pump_poller.poll(self._POLL_MS):
                    continue
                with self._lock:
                    try:
                        data = self._read_chunk()
                    except BlockingIOError:
                        continue
                    if not data:
                        self._eof = True
                        return
                    self._chunks.append(data)
        except Exception as exc:
            # A real failure is not EOF: keep it visible in every later stderr result.
            with self._lock:
                self._error = f"{type(exc).__name__}: {exc}"

    def _take_locked(self) -> str:
        data = b"".join(self._chunks)
        self._chunks = []
        text = data.decode("utf-8", errors="replace")
        if self._error is not None:
            if text and not text.endswith("\n"):
                text += "\n"
            text += f"[rtl_trace serve stderr capture failed ({self._error}); later stderr is lost]\n"
        return text

    def take_settled(self, max_wait_sec: float = 5.0) -> str:
        """Return (and clear) everything written so far, once the pump has emptied the pipe.

        Called after the child wrote its stdout terminator: any stderr it wrote before that
        is then either in the pipe or in the buffer, so waiting for the pipe to go empty
        attributes all of it to the current response and nothing from a later one.
        """
        deadline = time.monotonic() + max_wait_sec
        while True:
            with self._lock:
                settled = self._eof or not self._thread.is_alive() or time.monotonic() >= deadline
                if not settled:
                    try:
                        settled = not self._settle_poller.poll(0)
                    except Exception as exc:
                        self._error = self._error or f"{type(exc).__name__}: {exc}"
                        settled = True
                if settled:
                    return self._take_locked()
            time.sleep(0.001)

    def take_all(self, join_timeout_sec: float = 1.0) -> str:
        """Wait briefly for the pump to reach EOF (child exited), then return the buffer."""
        self._thread.join(timeout=join_timeout_sec)
        with self._lock:
            return self._take_locked()

    def stop(self, join_timeout_sec: float = 2.0) -> None:
        """Stop and join the thread; call before the pipe is closed so the fd is never reused."""
        self._stop.set()
        self._thread.join(timeout=join_timeout_sec)

    def is_alive(self) -> bool:
        return self._thread.is_alive()


def _shell_join(args: Sequence[str]) -> str:
    return " ".join(shlex.quote(arg) for arg in args)


def _resolve_bin(override: Optional[str], default_path: Path, fallback_name: str) -> str:
    if override:
        return str(Path(override).expanduser().resolve())
    if default_path.exists():
        return str(default_path)
    return fallback_name


# ---------------------------------------------------------------------------
# RTL trace serve session
# ---------------------------------------------------------------------------

class RtlTraceServeSession:
    def __init__(self, bin_path: str, serve_args: List[str]):
        self.bin_path = bin_path
        self.serve_args = serve_args
        self._query_lock = threading.RLock()
        self.process = subprocess.Popen(
            [bin_path, "serve"] + serve_args,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        # serve writes diagnostics to stderr while a query runs; drain it continuously so a
        # large one cannot fill the pipe and block serve before it writes <<END>>.
        self._stderr_pump: Optional[_StderrPump] = (
            _StderrPump(self.process.stderr) if self.process.stderr is not None else None
        )
        self.startup = self._read_until_end()

    def _read_until_end(self) -> Dict[str, Any]:
        if self.process.poll() is not None:
            err = self._take_stderr_after_exit()
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
                return {
                    "status": "error",
                    "message": "rtl_trace serve terminated while waiting for response",
                    "stderr": self._take_stderr_after_exit(),
                    "stdout": "".join(out_lines),
                }
            if s.strip() == "<<END>>":
                break
            out_lines.append(s)
        result: Dict[str, Any] = {"status": "success", "stdout": "".join(out_lines)}
        # serve reports per-query errors (bad regex, unknown signal, ...) on stderr and still
        # terminates the response with <<END>>; surface them instead of returning empty stdout.
        err = self._take_response_stderr()
        if err:
            result["stderr"] = err
        return result

    def _take_response_stderr(self) -> str:
        """Stderr serve wrote for the response whose <<END>> was just read.

        serve writes a query's diagnostics to stderr before it flushes <<END>> to stdout, so
        once the pump has emptied the pipe all of them are buffered and none of the next one's.
        """
        return self._stderr_pump.take_settled() if self._stderr_pump else ""

    def _take_stderr_after_exit(self) -> str:
        """Everything left on stderr once serve has exited or closed stdout."""
        return self._stderr_pump.take_all() if self._stderr_pump else ""

    def query(self, line: str) -> Dict[str, Any]:
        with self._query_lock:
            if self.process.poll() is not None:
                err = self._take_stderr_after_exit()
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
        with self._query_lock:
            _terminate_process(self.process)
            # Join the pump before closing the pipes so it never selects/reads a closed
            # (and possibly reused) fd.
            if self._stderr_pump:
                self._stderr_pump.stop()
            _close_process_pipes(self.process)
        return {"status": "success"}


# Module-level state for rtl serve sessions
rtl_serve_sessions: Dict[str, RtlTraceServeSession] = {}
rtl_session_ids_by_key: Dict[Tuple[str, str], str] = {}


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
        detail = str(result.get("stderr", "")).strip()
        message = f"rtl_trace error: {detail}" if detail else "rtl_trace returned empty output"
        return {"status": "error", "message": message}
    try:
        payload = json.loads(stdout)
    except json.JSONDecodeError as e:
        return {"status": "error", "message": f"invalid rtl_trace JSON: {e}", "stdout": stdout}

    payload["status"] = "success"
    payload["raw_command"] = _shell_join(args)
    return payload


# ---------------------------------------------------------------------------
# Waveform daemon
# ---------------------------------------------------------------------------

class WaveformDaemon:
    def __init__(self, wave_cli_path: str, waveform_path: str):
        self.waveform_path = waveform_path
        self._query_lock = threading.RLock()
        self.process = subprocess.Popen(
            [wave_cli_path, waveform_path],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )

    def query(self, cmd: str, args: Optional[dict] = None) -> dict:
        with self._query_lock:
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
        with self._query_lock:
            _stop_process(self.process)


# Module-level state for wave daemons
wave_daemons: Dict[str, WaveformDaemon] = {}


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


# ---------------------------------------------------------------------------
# Cleanup at exit
# ---------------------------------------------------------------------------

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

# Wire mapping._wave_query to clients._wave_query (breaks circular dependency).
from . import mapping as _mapping  # noqa: E402
_mapping._wave_query = _wave_query


# ---------------------------------------------------------------------------
# Waveform data helpers (snapshots, transitions, clock stepping)
# ---------------------------------------------------------------------------

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
            "max_limit": 256,
        },
        wave_cli_bin=wave_cli_bin,
    )
    if result.get("status") != "success":
        raise RuntimeError(result.get("message", "failed to get transitions"))
    return result


def _sample_signal_times(
    vcd_path: str,
    signals: Sequence[str],
    sample_times: Sequence[int],
    wave_cli_bin: Optional[str] = None,
) -> Tuple[Dict[str, Dict[str, Any]], List[Dict[str, str]]]:
    from .mapping import _map_signal_to_waveform

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
