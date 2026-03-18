import json
import subprocess
import os
import sys
from typing import List, Optional
from mcp.server.fastmcp import FastMCP

# Initialize FastMCP server
mcp = FastMCP("Waveform Explorer")

# Path to the C++ executable
CLI_PATH = os.path.join(os.path.dirname(__file__), "build", "wave_agent_cli")

def run_cli(vcd_path: str, cmd: str, args: dict = None):
    """Helper to run the C++ CLI tool."""
    if not os.path.exists(CLI_PATH):
        return {"status": "error", "message": f"CLI tool not found at {CLI_PATH}. Please build it first."}
    
    if not os.path.exists(vcd_path):
        return {"status": "error", "message": f"VCD file not found: {vcd_path}"}

    query = {"cmd": cmd, "args": args or {}}
    query_str = json.dumps(query)
    
    try:
        result = subprocess.run(
            [CLI_PATH, vcd_path, query_str],
            capture_output=True,
            text=True,
            check=True
        )
        return json.loads(result.stdout)
    except subprocess.CalledProcessError as e:
        return {"status": "error", "message": f"CLI execution failed: {e.stderr or e.stdout}"}
    except json.JSONDecodeError:
        return {"status": "error", "message": "Failed to parse CLI output as JSON"}
    except Exception as e:
        return {"status": "error", "message": str(e)}

@mcp.tool()
def list_signals(vcd_path: str):
    """List all hierarchical signal paths found in the VCD file."""
    return run_cli(vcd_path, "list_signals")

@mcp.tool()
def get_signal_info(vcd_path: str, path: str):
    """Get metadata for a specific signal (width, type, etc.)."""
    return run_cli(vcd_path, "get_signal_info", {"path": path})

@mcp.tool()
def get_snapshot(vcd_path: str, signals: List[str], time: int):
    """Get the values of multiple signals at a specific timestamp."""
    return run_cli(vcd_path, "get_snapshot", {"signals": signals, "time": time})

@mcp.tool()
def get_value_at_time(vcd_path: str, path: str, time: int):
    """Get the value of a single signal at a specific timestamp."""
    return run_cli(vcd_path, "get_value_at_time", {"path": path, "time": time})

@mcp.tool()
def find_edge(vcd_path: str, path: str, edge_type: str, start_time: int, direction: str = "forward"):
    """
    Search for the next/previous transition edge of a signal.
    edge_type: 'posedge', 'negedge', or 'anyedge'
    direction: 'forward' or 'backward'
    """
    return run_cli(vcd_path, "find_edge", {
        "path": path, 
        "edge_type": edge_type, 
        "start_time": start_time, 
        "direction": direction
    })

@mcp.tool()
def find_condition(vcd_path: str, expression: str, start_time: int, direction: str = "forward"):
    """
    Find the first timestamp where a logical condition is met.
    expression format: 'PATH == VALUE' (e.g., 'TOP.timer_tb.count == b00000001')
    """
    return run_cli(vcd_path, "find_condition", {
        "expression": expression, 
        "start_time": start_time, 
        "direction": direction
    })

@mcp.tool()
def get_transitions(vcd_path: str, path: str, start_time: int, end_time: int, max_limit: int = 50):
    """Get a compressed history of signal transitions within a time window."""
    return run_cli(vcd_path, "get_transitions", {
        "path": path, 
        "start_time": start_time, 
        "end_time": end_time, 
        "max_limit": max_limit
    })

@mcp.tool()
def analyze_pattern(vcd_path: str, path: str, start_time: int, end_time: int):
    """Analyze a signal's behavior (e.g., identify clocks, static signals)."""
    return run_cli(vcd_path, "analyze_pattern", {
        "path": path, 
        "start_time": start_time, 
        "end_time": end_time
    })

if __name__ == "__main__":
    mcp.run()
