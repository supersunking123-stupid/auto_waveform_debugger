import json
import subprocess
import os
import sys
from typing import List, Optional, Dict
from mcp.server.fastmcp import FastMCP

# Initialize FastMCP server
mcp = FastMCP("Waveform Explorer")

# Path to the C++ executable
CLI_PATH = os.path.join(os.path.dirname(__file__), "build", "wave_agent_cli")

class WaveformDaemon:
    def __init__(self, vcd_path: str):
        self.vcd_path = vcd_path
        self.process = subprocess.Popen(
            [CLI_PATH, vcd_path],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1  # Line buffered
        )
    
    def query(self, cmd: str, args: dict = None) -> dict:
        query_obj = {"cmd": cmd, "args": args or {}}
        query_str = json.dumps(query_obj)
        
        try:
            self.process.stdin.write(query_str + "\n")
            self.process.stdin.flush()
            
            line = self.process.stdout.readline()
            if not line:
                # Process might have crashed, check stderr
                stderr = self.process.stderr.read()
                return {"status": "error", "message": f"Daemon crashed. Stderr: {stderr}"}
                
            return json.loads(line)
        except Exception as e:
            return {"status": "error", "message": str(e)}

    def __del__(self):
        if hasattr(self, 'process'):
            self.process.terminate()

# Cache for daemons: vcd_path -> WaveformDaemon
daemons: Dict[str, WaveformDaemon] = {}

def get_daemon(vcd_path: str) -> WaveformDaemon:
    vcd_path = os.path.abspath(vcd_path)
    if vcd_path not in daemons:
        if not os.path.exists(vcd_path):
            raise FileNotFoundError(f"VCD file not found: {vcd_path}")
        daemons[vcd_path] = WaveformDaemon(vcd_path)
    return daemons[vcd_path]

@mcp.tool()
def list_signals(vcd_path: str):
    """List all hierarchical signal paths found in the VCD file."""
    try:
        return get_daemon(vcd_path).query("list_signals")
    except Exception as e:
        return {"status": "error", "message": str(e)}

@mcp.tool()
def get_signal_info(vcd_path: str, path: str):
    """Get metadata for a specific signal (width, type, etc.)."""
    try:
        return get_daemon(vcd_path).query("get_signal_info", {"path": path})
    except Exception as e:
        return {"status": "error", "message": str(e)}

@mcp.tool()
def get_snapshot(vcd_path: str, signals: List[str], time: int, radix: str = "hex"):
    """Get the values of multiple signals at a specific timestamp."""
    try:
        return get_daemon(vcd_path).query("get_snapshot", {"signals": signals, "time": time, "radix": radix})
    except Exception as e:
        return {"status": "error", "message": str(e)}

@mcp.tool()
def get_value_at_time(vcd_path: str, path: str, time: int, radix: str = "hex"):
    """Get the value of a single signal at a specific timestamp."""
    try:
        return get_daemon(vcd_path).query("get_value_at_time", {"path": path, "time": time, "radix": radix})
    except Exception as e:
        return {"status": "error", "message": str(e)}

@mcp.tool()
def find_edge(vcd_path: str, path: str, edge_type: str, start_time: int, direction: str = "forward"):
    """
    Search for the next/previous transition edge of a signal.
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
def find_condition(vcd_path: str, expression: str, start_time: int, direction: str = "forward"):
    """
    Find the first timestamp where a logical condition is met.
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
    """Get a compressed history of signal transitions within a time window."""
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
def analyze_pattern(vcd_path: str, path: str, start_time: int, end_time: int):
    """Analyze a signal's behavior (e.g., identify clocks, static signals)."""
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
