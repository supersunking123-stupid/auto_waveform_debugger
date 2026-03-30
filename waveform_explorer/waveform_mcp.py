import json
import os
import atexit
import threading
from typing import List, Optional, Dict, Union
from mcp.server.fastmcp import FastMCP
from daemon_client import WaveformDaemon

# Initialize FastMCP server
mcp = FastMCP("Waveform Explorer")

CLI_PATH = os.path.join(os.path.dirname(__file__), "build", "wave_agent_cli")

# Cache for daemons: vcd_path -> WaveformDaemon
daemons: Dict[str, WaveformDaemon] = {}
_daemons_lock = threading.RLock()


def _close_all_daemons() -> None:
    with _daemons_lock:
        current = list(daemons.values())
        daemons.clear()
    for daemon in current:
        daemon.close()


atexit.register(_close_all_daemons)

def get_daemon(vcd_path: str) -> WaveformDaemon:
    vcd_path = os.path.abspath(vcd_path)
    with _daemons_lock:
        daemon = daemons.get(vcd_path)
        if daemon is not None and daemon.process.poll() is None:
            return daemon
        if daemon is not None:
            daemon.close()
        if not os.path.exists(vcd_path):
            raise FileNotFoundError(f"VCD file not found: {vcd_path}")
        daemons[vcd_path] = WaveformDaemon(CLI_PATH, vcd_path)
        return daemons[vcd_path]

@mcp.tool()
def list_signals(vcd_path: str, pattern: str = "", types: Optional[List[str]] = None):
    """List waveform signal paths for one concrete waveform file.

    This standalone MCP is stateless. It does not support Sessions, Cursor,
    Bookmarks, or Signal Groups.

    Default behavior lists only signals declared in the top module.
    Pass `pattern="*"` to enumerate the full waveform namespace, or use a
    hierarchy-aware wildcard such as `top.u_axi.ar_*` to narrow the result.
    `types` may include any combination of `input`, `output`, `inout`,
    `net`, and `register`.
    """
    try:
        return get_daemon(vcd_path).query("list_signals", {
            "pattern": pattern,
            "types": list(types or []),
        })
    except Exception as e:
        return {"status": "error", "message": str(e)}

@mcp.tool()
def get_signal_info(vcd_path: str, path: str):
    """Get metadata for one signal in one concrete waveform file."""
    try:
        return get_daemon(vcd_path).query("get_signal_info", {"path": path})
    except Exception as e:
        return {"status": "error", "message": str(e)}

@mcp.tool()
def get_snapshot(vcd_path: str, signals: List[str], time: int, radix: str = "hex"):
    """Get values for multiple signals at one integer timestamp.

    Unlike the merged MCP, this standalone server does not resolve session
    aliases such as "Cursor" or "BM_<name>" and does not expand Signal Groups.
    """
    try:
        return get_daemon(vcd_path).query("get_snapshot", {"signals": signals, "time": time, "radix": radix})
    except Exception as e:
        return {"status": "error", "message": str(e)}

@mcp.tool()
def get_value_at_time(vcd_path: str, path: str, time: int, radix: str = "hex"):
    """Get one signal value at one integer timestamp."""
    try:
        return get_daemon(vcd_path).query("get_value_at_time", {"path": path, "time": time, "radix": radix})
    except Exception as e:
        return {"status": "error", "message": str(e)}

@mcp.tool()
def find_edge(vcd_path: str, path: str, edge_type: str, start_time: int, direction: str = "forward"):
    """
    Search for the next or previous transition edge from one integer start time.

    This standalone server does not resolve session time aliases.
    edge_type: 'posedge', 'negedge', or 'anyedge'
    direction: 'forward' or 'backward'
    """
    try:
        return get_daemon(vcd_path).query("find_edge", {
            "path": path, 
            "edge_type": edge_type, 
            "start_time": start_time, 
            "direction": direction
        })
    except Exception as e:
        return {"status": "error", "message": str(e)}

@mcp.tool()
def find_value_intervals(vcd_path: str, path: str, value: str, start_time: int, end_time: int, radix: str = "hex"):
    """Find all matching value intervals in one explicit integer time range."""
    try:
        return get_daemon(vcd_path).query("find_value_intervals", {
            "path": path,
            "value": value,
            "start_time": start_time,
            "end_time": end_time,
            "radix": radix,
        })
    except Exception as e:
        return {"status": "error", "message": str(e)}

@mcp.tool()
def find_condition(vcd_path: str, expression: str, start_time: int, direction: str = "forward"):
    """
    Find the first timestamp where a logical condition is met from one integer start time.
    expression format: 'PATH == VALUE' (e.g., 'TOP.timer_tb.count == b00000001')
    """
    try:
        return get_daemon(vcd_path).query("find_condition", {
            "expression": expression, 
            "start_time": start_time, 
            "direction": direction
        })
    except Exception as e:
        return {"status": "error", "message": str(e)}

@mcp.tool()
def get_transitions(vcd_path: str, path: str, start_time: int, end_time: int, max_limit: int = 50):
    """Get a compressed transition history in one explicit integer time window."""
    try:
        return get_daemon(vcd_path).query("get_transitions", {
            "path": path, 
            "start_time": start_time, 
            "end_time": end_time, 
            "max_limit": max_limit
        })
    except Exception as e:
        return {"status": "error", "message": str(e)}

@mcp.tool()
def get_signal_overview(
    vcd_path: str,
    path: str,
    start_time: int,
    end_time: int,
    resolution: Union[int, str],
    radix: str = "hex",
):
    """Summarize one signal over a time window using a resolution-aware overview.

    Inputs are explicit and stateless:
    - `vcd_path` selects the waveform file
    - `start_time` and `end_time` are integer timestamps
    - `resolution` may be an integer or `"auto"`

    This standalone server does not resolve Sessions, Cursor, Bookmarks, or
    Signal Groups.
    """
    try:
        return get_daemon(vcd_path).query("get_signal_overview", {
            "path": path,
            "start_time": start_time,
            "end_time": end_time,
            "resolution": resolution,
            "radix": radix,
        })
    except Exception as e:
        return {"status": "error", "message": str(e)}

@mcp.tool()
def analyze_pattern(vcd_path: str, path: str, start_time: int, end_time: int):
    """Analyze one signal over one explicit integer time window."""
    try:
        return get_daemon(vcd_path).query("analyze_pattern", {
            "path": path, 
            "start_time": start_time, 
            "end_time": end_time
        })
    except Exception as e:
        return {"status": "error", "message": str(e)}

if __name__ == "__main__":
    mcp.run()
