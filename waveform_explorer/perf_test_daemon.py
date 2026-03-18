import time
import json
import subprocess
import os

# Paths
CLI_PATH = os.path.join(os.path.dirname(__file__), "build", "wave_agent_cli")
VCD_PATH = os.path.join(os.path.dirname(__file__), "timer_tb.vcd")
LARGE_VCD_PATH = "/home/qsun/AI_PROJ/auto_waveform_debugger/Cores-VeeR-EH1/sim.vcd"

class WaveformDaemon:
    def __init__(self, vcd_path: str):
        self.vcd_path = vcd_path
        self.process = subprocess.Popen(
            [CLI_PATH, vcd_path],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1
        )
    
    def query(self, cmd: str, args: dict = None) -> dict:
        query_obj = {"cmd": cmd, "args": args or {}}
        query_str = json.dumps(query_obj)
        self.process.stdin.write(query_str + "\n")
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        return json.loads(line)

    def terminate(self):
        self.process.terminate()

def test_performance(vcd_path):
    print(f"--- Benchmarking VCD (Daemon Mode): {vcd_path} ---")
    if not os.path.exists(vcd_path):
        print("VCD file not found, skipping.")
        return

    # 1. Start Daemon (Includes initial load time)
    start_load = time.perf_counter()
    daemon = WaveformDaemon(vcd_path)
    # Trigger first query to ensure it's loaded
    daemon.query("list_signals")
    end_load = time.perf_counter()
    print(f"Initial Load + First Query: {end_load - start_load:.4f}s")

    # 2. Measure subsequent commands (Pure query latency)
    cmds = [
        ("list_signals", {}),
        ("get_snapshot", {"signals": ["TOP.core_clk" if "sim.vcd" in vcd_path else "TOP.timer_tb.clk"], "time": 10000}),
        ("analyze_pattern", {"path": "TOP.core_clk" if "sim.vcd" in vcd_path else "TOP.timer_tb.clk", "start_time": 0, "end_time": 100000}),
        ("get_transitions", {"path": "TOP.core_clk" if "sim.vcd" in vcd_path else "TOP.timer_tb.clk", "start_time": 0, "end_time": 50000})
    ]

    total_query_time = 0
    for cmd, args in cmds:
        start = time.perf_counter()
        daemon.query(cmd, args)
        latency = time.perf_counter() - start
        print(f"Query '{cmd}' took: {latency:.4f}s")
        total_query_time += latency

    print(f"Total time for 4 subsequent queries: {total_query_time:.4f}s")
    print(f"Average query latency: {total_query_time / len(cmds):.4f}s\n")
    daemon.terminate()

if __name__ == "__main__":
    test_performance(VCD_PATH)
    test_performance(LARGE_VCD_PATH)
