import time
import json
import subprocess
import os

# Paths
CLI_PATH = os.path.join(os.path.dirname(__file__), "build", "wave_agent_cli")
VCD_PATH = os.path.join(os.path.dirname(__file__), "timer_tb.vcd")
LARGE_VCD_PATH = "/home/qsun/AI_PROJ/auto_waveform_debugger/Cores-VeeR-EH1/sim.vcd"

def run_cli(vcd_path: str, cmd: str, args: dict = None):
    query = {"cmd": cmd, "args": args or {}}
    query_str = json.dumps(query)
    start = time.perf_counter()
    result = subprocess.run(
        [CLI_PATH, vcd_path, query_str],
        capture_output=True,
        text=True,
        check=True
    )
    end = time.perf_counter()
    return end - start

def test_performance(vcd_path):
    print(f"--- Benchmarking VCD: {vcd_path} ---")
    if not os.path.exists(vcd_path):
        print("VCD file not found, skipping.")
        return

    # 1. Warm-up
    run_cli(vcd_path, "list_signals")

    # 2. Measure multiple commands
    cmds = [
        ("list_signals", {}),
        ("get_snapshot", {"signals": ["TOP.timer_tb.clk"], "time": 10000}),
        ("analyze_pattern", {"path": "TOP.timer_tb.clk", "start_time": 0, "end_time": 100000}),
        ("get_transitions", {"path": "TOP.timer_tb.clk", "start_time": 0, "end_time": 50000})
    ]

    total_time = 0
    for cmd, args in cmds:
        latency = run_cli(vcd_path, cmd, args)
        print(f"Command '{cmd}' took: {latency:.4f}s")
        total_time += latency

    print(f"Total time for 4 commands: {total_time:.4f}s")
    print(f"Average latency per command: {total_time / len(cmds):.4f}s\n")

if __name__ == "__main__":
    if not os.path.exists(CLI_PATH):
        print(f"Error: {CLI_PATH} not found. Please build the C++ tool first.")
        exit(1)

    test_performance(VCD_PATH)
    test_performance(LARGE_VCD_PATH)
