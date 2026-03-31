#!/usr/bin/env python3
"""
Cross-Link Test Harness and Report Generator

Runs all cross-link test cases (tc16-tc26) and generates:
1. Markdown test report
2. Summary table (CSV)
3. Bug list (JSON)

Usage:
    python run_cross_link_tests.py [--output-dir OUTPUT_DIR]
"""

import argparse
import json
import os
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

# Test case definitions
TEST_CASES = [
    {"id": "tc16", "dir": "tc16_cross_link_backend_sanity", "name": "Backend Sanity", "phase": 1, "signal": "top.mem0_rd_bw_mon.clk", "time": 399970000},
    {"id": "tc17", "dir": "tc17_cross_link_smoke", "name": "Cross-Link Smoke", "phase": 2, "signal": "top.mem0_rd_bw_mon.ready_in", "time": 399970000},
    {"id": "tc18", "dir": "tc18_cross_link_edge_correctness", "name": "Edge Correctness", "phase": 3, "signal": "top.mem0_rd_bw_mon.clk", "time": 399970000},
    {"id": "tc19", "dir": "tc19_cross_link_direction_ranking", "name": "Direction Ranking", "phase": 4, "signal": "top.mem0_rd_bw_mon.clk", "time": 399970000},
    {"id": "tc20", "dir": "tc20_cross_link_closeness_ranking", "name": "Closeness Ranking", "phase": 5, "signal": "top.mem0_rd_bw_mon.clk", "time": 399970000},
    {"id": "tc21", "dir": "tc21_cross_link_stuck_classification", "name": "Stuck Classification", "phase": 6, "signal": "top.mem0_rd_bw_mon.ready_in", "time": 399970000},
    {"id": "tc22", "dir": "tc22_cross_link_snapshot_sampling", "name": "Snapshot Sampling", "phase": 7, "signal": "top.mem0_rd_bw_mon.clk", "time": 399970000},
    {"id": "tc23", "dir": "tc23_cross_link_mapping_robustness", "name": "Mapping Robustness", "phase": 8, "signal": "top.mem0_rd_bw_mon.clk", "time": 399970000},
    {"id": "tc24", "dir": "tc24_cross_link_unmapped_handling", "name": "Unmapped Handling", "phase": 9, "signal": "top.mem0_rd_bw_mon.ready_in", "time": 399970000},
    {"id": "tc25", "dir": "tc25_cross_link_performance", "name": "Performance", "phase": 10, "signal": "top.mem0_rd_bw_mon.ready_in", "time": 399970000},
    {"id": "tc26", "dir": "tc26_cross_link_non_clock_active", "name": "Non-Clock Active", "phase": 11, "signal": "varies", "time": "varies"},
    {"id": "tc27", "dir": "tc27_history_failure_regression", "name": "History Failure Regression", "phase": 12, "signal": "CQ signals", "time": "784000000-800000000"},
]


class TestResult:
    def __init__(self, tc_id: str, name: str, phase: int, signal: str, time_val: str):
        self.tc_id = tc_id
        self.name = name
        self.phase = phase
        self.signal = signal
        self.time_val = str(time_val)
        self.cold_time: Optional[float] = None
        self.hot_time: Optional[float] = None
        self.json_size: Optional[int] = None
        self.result: str = "PENDING"
        self.tests_run: int = 0
        self.tests_passed: int = 0
        self.tests_failed: int = 0
        self.warnings: List[str] = []
        self.errors: List[str] = []
        self.duration: float = 0.0

    def to_dict(self) -> Dict[str, Any]:
        return {
            "tc_id": self.tc_id,
            "name": self.name,
            "phase": self.phase,
            "signal": self.signal,
            "time": self.time_val,
            "cold_time": self.cold_time,
            "hot_time": self.hot_time,
            "json_size": self.json_size,
            "result": self.result,
            "tests_run": self.tests_run,
            "tests_passed": self.tests_passed,
            "tests_failed": self.tests_failed,
            "warnings": self.warnings,
            "errors": self.errors,
            "duration": self.duration,
        }


def run_test_case(tc_dir: Path) -> Tuple[bool, str, float]:
    """Run a single test case and return (success, output, duration)."""
    run_script = tc_dir / "run_test.sh"
    if not run_script.exists():
        return False, f"run_test.sh not found in {tc_dir}", 0.0

    start_time = time.time()
    try:
        result = subprocess.run(
            [str(run_script)],
            capture_output=True,
            text=True,
            timeout=300,
            cwd=str(tc_dir),
        )
        duration = time.time() - start_time
        output = result.stdout + result.stderr
        return result.returncode == 0, output, duration
    except subprocess.TimeoutExpired:
        return False, "Test timed out after 300 seconds", time.time() - start_time
    except Exception as e:
        return False, str(e), time.time() - start_time


def parse_test_output(output: str, tc_id: str) -> Dict[str, Any]:
    """Parse test output to extract results."""
    info = {
        "tests_run": 0,
        "tests_passed": 0,
        "tests_failed": 0,
        "cold_time": None,
        "hot_time": None,
        "json_size": None,
        "warnings": [],
        "errors": [],
    }

    # Parse "Ran X tests in" line
    # Check for OK or FAILED (unittest output format)
    # Look for the summary line "Ran X tests in Ys" and the result line after it
    output_lines = output.split("\n")
    found_ran = False
    tests_passed_line = False
    
    for i, line in enumerate(output_lines):
        if "Ran" in line and "tests in" in line:
            found_ran = True
            parts = line.split()
            for j, p in enumerate(parts):
                if p == "Ran":
                    try:
                        info["tests_run"] = int(parts[j + 1])
                    except (ValueError, IndexError):
                        pass
            # Check next few lines for OK or FAILED
            for next_line in output_lines[i+1:i+4]:
                if next_line.strip() == "OK":
                    tests_passed_line = True
                    break
                elif next_line.startswith("FAILED ("):
                    # Parse "FAILED (failures=1, errors=0)" format
                    import re
                    match = re.search(r'FAILED \(failures=(\d+), errors=(\d+)\)', next_line)
                    if match:
                        info["tests_failed"] = int(match.group(1)) + int(match.group(2))
                    break
    
    if found_ran and tests_passed_line:
        info["tests_passed"] = info["tests_run"]

    # Parse warnings
    for line in output.split("\n"):
        if "[WARN]" in line:
            info["warnings"].append(line.strip())

    # Parse performance data for tc25
    if tc_id == "tc25":
        for line in output.split("\n"):
            if "Cold:" in line:
                parts = line.split()
                for i, p in enumerate(parts):
                    if p == "Cold:":
                        try:
                            info["cold_time"] = float(parts[i + 1].rstrip("s,"))
                        except (ValueError, IndexError):
                            pass
            if "Hot:" in line:
                parts = line.split()
                for i, p in enumerate(parts):
                    if p == "Hot:":
                        try:
                            info["hot_time"] = float(parts[i + 1].rstrip("s,"))
                        except (ValueError, IndexError):
                            pass

    return info


def generate_markdown_report(results: List[TestResult], output_path: Path) -> None:
    """Generate markdown test report."""
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    md = f"""# Cross-Link Test Report

Generated: {timestamp}

## Executive Summary

| Metric | Value |
|--------|-------|
| Total Test Cases | {len(results)} |
| Passed | {sum(1 for r in results if r.result == 'PASS')} |
| Warning | {sum(1 for r in results if r.result == 'WARN')} |
| Failed | {sum(1 for r in results if r.result in ('FAIL', 'PENDING'))} |
| Total Duration | {sum(r.duration for r in results):.1f}s |

---

## Test Results by Phase

"""

    for phase in range(1, 13):
        phase_results = [r for r in results if r.phase == phase]
        if not phase_results:
            continue

        phase_name = phase_results[0].name.split(":")[0] if phase_results else f"Phase {phase}"
        md += f"### Phase {phase}: {phase_name}\n\n"

        for r in phase_results:
            status_icon = {"PASS": "✓", "WARN": "⚠", "FAIL": "✗", "PENDING": "?"}.get(r.result, "?")
            md += f"""#### {r.tc_id}: {r.name}

- **Result:** {status_icon} {r.result}
- **Signal:** {r.signal}
- **Time:** {r.time_val}
- **Duration:** {r.duration:.1f}s
- **Tests:** {r.tests_passed}/{r.tests_run} passed

"""
            if r.warnings:
                md += "**Warnings:**\n"
                for w in r.warnings[:5]:
                    md += f"- {w}\n"
                md += "\n"

            if r.errors:
                md += "**Errors:**\n"
                for e in r.errors[:5]:
                    md += f"- {e}\n"
                md += "\n"

            md += "---\n\n"

    md += """## Appendix: Test Case Details

| ID | Name | Phase | Signal | Time | Result | Duration |
|----|------|-------|--------|------|--------|----------|
"""
    for r in results:
        md += f"| {r.tc_id} | {r.name} | {r.phase} | {r.signal} | {r.time_val} | {r.result} | {r.duration:.1f}s |\n"

    with open(output_path, "w") as f:
        f.write(md)

    print(f"Markdown report written to: {output_path}")


def generate_summary_csv(results: List[TestResult], output_path: Path) -> None:
    """Generate summary table CSV."""
    with open(output_path, "w") as f:
        f.write("test_case,signal,time,cold_time,hot_time,json_size,result,duration,tests_passed,tests_run\n")
        for r in results:
            cold = f"{r.cold_time:.3f}" if r.cold_time else "N/A"
            hot = f"{r.hot_time:.4f}" if r.hot_time else "N/A"
            size = str(r.json_size) if r.json_size else "N/A"
            f.write(f"{r.tc_id},{r.signal},{r.time_val},{cold},{hot},{size},{r.result},{r.duration:.1f},{r.tests_passed},{r.tests_run}\n")

    print(f"Summary CSV written to: {output_path}")


def generate_bug_list(results: List[TestResult], output_path: Path) -> None:
    """Generate bug list JSON."""
    bugs = []

    for r in results:
        if r.result in ("FAIL", "PENDING"):
            for error in r.errors:
                bugs.append({
                    "test_case": r.tc_id,
                    "title": f"{r.name}: {error[:100]}",
                    "reproduction": {
                        "signal": r.signal,
                        "time": r.time_val,
                        "phase": r.phase,
                    },
                    "observed": error,
                    "expected": "Test should pass",
                    "suspected_layer": "unknown",
                })

    with open(output_path, "w") as f:
        json.dump(bugs, f, indent=2)

    print(f"Bug list written to: {output_path}")


def main():
    parser = argparse.ArgumentParser(description="Cross-Link Test Harness")
    parser.add_argument("--output-dir", type=str, default=None,
                       help="Output directory for reports (default: test_cases dir)")
    args = parser.parse_args()

    # Determine paths
    test_cases_dir = Path(__file__).resolve().parent
    output_dir = Path(args.output_dir) if args.output_dir else test_cases_dir
    output_dir.mkdir(parents=True, exist_ok=True)

    print("=" * 60)
    print("  Cross-Link Test Harness")
    print("=" * 60)
    print(f"Test cases directory: {test_cases_dir}")
    print(f"Output directory: {output_dir}")
    print()

    results: List[TestResult] = []

    for tc in TEST_CASES:
        tc_id = tc["id"]
        tc_dir = test_cases_dir / tc["dir"]
        print(f"\n[{tc['phase']}/{len(TEST_CASES)}] Running {tc_id}: {tc['name']}...")

        if not tc_dir.exists():
            print(f"  SKIP: Directory not found: {tc_dir}")
            result = TestResult(tc_id, tc["name"], tc["phase"], tc["signal"], tc["time"])
            result.result = "SKIP"
            results.append(result)
            continue

        success, output, duration = run_test_case(tc_dir)

        result = TestResult(tc_id, tc["name"], tc["phase"], tc["signal"], tc["time"])
        result.duration = duration
        parsed = parse_test_output(output, tc_id)
        result.tests_run = parsed["tests_run"]
        result.tests_passed = parsed["tests_passed"]
        result.tests_failed = parsed["tests_failed"]
        result.warnings = parsed["warnings"]
        result.errors = parsed["errors"]
        result.cold_time = parsed["cold_time"]
        result.hot_time = parsed["hot_time"]

        if success:
            result.result = "WARN" if result.warnings else "PASS"
            print(f"  PASS (duration: {duration:.1f}s)")
        else:
            result.result = "FAIL"
            print(f"  FAIL: {output[:200]}")

        results.append(result)

    # Generate reports
    print("\n" + "=" * 60)
    print("Generating reports...")
    print("=" * 60)

    generate_markdown_report(results, output_dir / "cross_link_test_report.md")
    generate_summary_csv(results, output_dir / "cross_link_test_summary.csv")
    generate_bug_list(results, output_dir / "cross_link_bug_list.json")

    # Print summary
    print("\n" + "=" * 60)
    print("  Test Summary")
    print("=" * 60)
    print(f"Total:  {len(results)}")
    print(f"Passed: {sum(1 for r in results if r.result == 'PASS')}")
    print(f"Warn:   {sum(1 for r in results if r.result == 'WARN')}")
    print(f"Failed: {sum(1 for r in results if r.result in ('FAIL', 'PENDING'))}")
    print(f"Total Duration: {sum(r.duration for r in results):.1f}s")
    print("=" * 60)

    return 0 if all(r.result in ("PASS", "WARN", "SKIP") for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
