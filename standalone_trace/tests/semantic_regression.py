#!/usr/bin/env python3
import argparse
import json
import shutil
import subprocess
import tempfile
from pathlib import Path


def run_cmd(cmd, cwd=None, expect=0):
    proc = subprocess.run(cmd, cwd=cwd, text=True, capture_output=True)
    if proc.returncode != expect:
        raise AssertionError(
            "Command failed\n"
            f"cmd: {' '.join(cmd)}\n"
            f"cwd: {cwd}\n"
            f"expected_rc: {expect}, actual_rc: {proc.returncode}\n"
            f"stdout:\n{proc.stdout}\n"
            f"stderr:\n{proc.stderr}\n"
        )
    return proc


def run_trace_json(rtl_trace, db_path, mode, signal, extra=None):
    cmd = [
        str(rtl_trace),
        "trace",
        "--db",
        str(db_path),
        "--mode",
        mode,
        "--signal",
        signal,
        "--format",
        "json",
    ]
    if extra:
        cmd.extend(extra)
    proc = run_cmd(cmd)
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        raise AssertionError(f"Invalid JSON output for signal {signal}: {proc.stdout}") from exc


def assert_any(endpoints, pred, msg):
    if not any(pred(e) for e in endpoints):
        raise AssertionError(msg)


def assert_invalid_arg(rtl_trace, args, expected_fragment):
    proc = run_cmd([str(rtl_trace)] + args, expect=1)
    if expected_fragment not in proc.stderr:
        raise AssertionError(
            f"expected error fragment {expected_fragment!r} in stderr for {' '.join(args)}\n"
            f"stderr:\n{proc.stderr}"
        )


def run_json_cmd(cmd, cwd=None, expect=0):
    proc = run_cmd(cmd, cwd=cwd, expect=expect)
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        raise AssertionError(f"Invalid JSON output for command {' '.join(cmd)}: {proc.stdout}") from exc


def normalize_reported_path(path_str):
    return str(Path(path_str).resolve())


def main():
    ap = argparse.ArgumentParser(description="Standalone trace semantic regression suite")
    ap.add_argument("--rtl-trace", required=True, help="Path to rtl_trace binary")
    ap.add_argument("--source-dir", required=True, help="Path to standalone_trace source dir")
    args = ap.parse_args()

    rtl_trace = Path(args.rtl_trace).resolve()
    src_dir = Path(args.source_dir).resolve()
    fixture = src_dir / "tests" / "fixtures" / "semantic_top.sv"
    if not rtl_trace.exists():
        raise SystemExit(f"rtl_trace not found: {rtl_trace}")
    if not fixture.exists():
        raise SystemExit(f"fixture not found: {fixture}")

    tmpdir = Path(tempfile.mkdtemp(prefix="rtl_trace_semreg_"))
    try:
        db = tmpdir / "semantic.db"
        inc_db = tmpdir / "semantic_inc.db"

        # 1) compile smoke
        c = run_cmd(
            [
                str(rtl_trace),
                "compile",
                "--db",
                str(db),
                "--single-unit",
                str(fixture),
                "--top",
                "semantic_top",
            ]
        )
        if "signals:" not in c.stdout:
            raise AssertionError(f"compile output missing signals summary: {c.stdout}")

        # 2) drivers can cross submodule input port and reach producer assignments
        drivers = run_trace_json(rtl_trace, db, "drivers", "semantic_top.u_cons.in_bus")
        if drivers.get("summary", {}).get("count") != 2:
            raise AssertionError(f"unexpected drivers count: {drivers}")
        endpoints = drivers.get("endpoints", [])
        assert_any(
            endpoints,
            lambda e: e.get("assignment") == "data <= 8'h00" and e.get("path") == "semantic_top.u_prod.data",
            "missing reset driver assignment after cross-port traversal",
        )
        assert_any(
            endpoints,
            lambda e: e.get("assignment") == "data <= data + 8'h01" and e.get("path") == "semantic_top.u_prod.data",
            "missing increment driver assignment after cross-port traversal",
        )

        # 3) better loads assignment context: condition expression should carry LHS path
        loads = run_trace_json(rtl_trace, db, "loads", "semantic_top.data")
        l_endpoints = loads.get("endpoints", [])
        assert_any(
            l_endpoints,
            lambda e: e.get("assignment") == "" and e.get("bit_map") == "[0]" and "semantic_top.flag" in e.get("lhs", []),
            "missing loads condition-context LHS for semantic_top.data[0]",
        )
        hit_loads = run_trace_json(rtl_trace, db, "loads", "semantic_top.hit")
        assert_any(
            hit_loads.get("endpoints", []),
            lambda e: e.get("assignment") == "flag <= hit" and "semantic_top.flag" in e.get("lhs", []),
            "missing nonblocking assignment LHS for semantic_top.hit loads",
        )

        # 4) bit-level precision and bit-select query filtering
        bit_q = run_trace_json(rtl_trace, db, "loads", "semantic_top.u_cons.in_bus[3]")
        if bit_q.get("summary", {}).get("count") != 1:
            raise AssertionError(f"unexpected bit-query count: {bit_q}")
        b0 = bit_q.get("endpoints", [])[0]
        if b0.get("bit_map") != "[3]" or b0.get("bit_map_approximate"):
            raise AssertionError(f"unexpected bit_map for [3] query: {b0}")
        if not any(s.get("reason") == "bit_filter" for s in bit_q.get("stops", [])):
            raise AssertionError(f"expected bit_filter stop in [3] query: {bit_q}")

        range_q = run_trace_json(rtl_trace, db, "loads", "semantic_top.u_cons.in_bus[7:4]")
        if range_q.get("summary", {}).get("count") != 1:
            raise AssertionError(f"unexpected range-query count: {range_q}")
        r0 = range_q.get("endpoints", [])[0]
        if r0.get("bit_map") != "[7:4]" or r0.get("bit_map_approximate"):
            raise AssertionError(f"unexpected bit_map for [7:4] query: {r0}")

        # 5) traversal controls: depth / node limits should emit stops
        depth_limit = run_trace_json(
            rtl_trace,
            db,
            "drivers",
            "semantic_top.u_cons.in_bus",
            extra=["--depth", "0"],
        )
        if not any(s.get("reason") == "depth_limit" for s in depth_limit.get("stops", [])):
            raise AssertionError(f"expected depth_limit stop: {depth_limit}")

        node_limit = run_trace_json(
            rtl_trace,
            db,
            "drivers",
            "semantic_top.u_cons.in_bus",
            extra=["--max-nodes", "1"],
        )
        if not any(s.get("reason") == "node_limit" for s in node_limit.get("stops", [])):
            raise AssertionError(f"expected node_limit stop: {node_limit}")

        # 6) find typo suggestions
        find_proc = run_cmd(
            [
                str(rtl_trace),
                "find",
                "--db",
                str(db),
                "--query",
                "semantic_top.u_cons.in_buz",
                "--limit",
                "3",
            ],
            expect=2,
        )
        if "suggestions:" not in find_proc.stdout or "semantic_top.u_cons.in_bus" not in find_proc.stdout:
            raise AssertionError(f"find typo suggestions missing expected candidate:\n{find_proc.stdout}")

        # 7) incremental compile cache hit
        run_cmd(
            [
                str(rtl_trace),
                "compile",
                "--db",
                str(inc_db),
                "--incremental",
                "--single-unit",
                str(fixture),
                "--top",
                "semantic_top",
            ]
        )
        inc2 = run_cmd(
            [
                str(rtl_trace),
                "compile",
                "--db",
                str(inc_db),
                "--incremental",
                "--single-unit",
                str(fixture),
                "--top",
                "semantic_top",
            ]
        )
        if "signals: incremental-cache-hit" not in inc2.stdout:
            raise AssertionError(f"incremental cache hit missing:\n{inc2.stdout}")

        # 8) invalid numeric args should return a parse error instead of terminating
        assert_invalid_arg(
            rtl_trace,
            ["trace", "--db", str(db), "--mode", "drivers", "--signal", "semantic_top.hit", "--depth", "abc"],
            "Invalid --depth: abc",
        )
        assert_invalid_arg(
            rtl_trace,
            ["hier", "--db", str(db), "--max-nodes", "oops"],
            "Invalid --max-nodes: oops",
        )
        assert_invalid_arg(
            rtl_trace,
            ["find", "--db", str(db), "--query", "semantic_top.hit", "--limit", "bad"],
            "Invalid --limit: bad",
        )

        # 9) hierarchy query can optionally expose definition source locations
        hier_with_source = run_json_cmd(
            [
                str(rtl_trace),
                "hier",
                "--db",
                str(db),
                "--root",
                "semantic_top.u_cons",
                "--depth",
                "0",
                "--format",
                "json",
                "--show-source",
            ]
        )
        tree = hier_with_source.get("tree", {})
        if tree.get("module") != "consumer":
            raise AssertionError(f"unexpected hier module info: {hier_with_source}")
        source = tree.get("source")
        if (
            source is None
            or normalize_reported_path(source.get("file", "")) != str(fixture)
            or source.get("line") != 15
        ):
            raise AssertionError(f"unexpected hier source info: {hier_with_source}")

        hier_without_source = run_json_cmd(
            [
                str(rtl_trace),
                "hier",
                "--db",
                str(db),
                "--root",
                "semantic_top.u_cons",
                "--depth",
                "0",
                "--format",
                "json",
            ]
        )
        if "source" in hier_without_source.get("tree", {}):
            raise AssertionError(f"hier should omit source unless requested: {hier_without_source}")

        # 10) whereis-instance provides quick module/source lookup for one instance
        whereis = run_json_cmd(
            [
                str(rtl_trace),
                "whereis-instance",
                "--db",
                str(db),
                "--instance",
                "semantic_top.u_cons",
                "--format",
                "json",
            ]
        )
        if whereis.get("module") != "consumer":
            raise AssertionError(f"unexpected whereis module info: {whereis}")
        whereis_source = whereis.get("source")
        if (
            whereis_source is None
            or normalize_reported_path(whereis_source.get("file", "")) != str(fixture)
            or whereis_source.get("line") != 15
        ):
            raise AssertionError(f"unexpected whereis source info: {whereis}")

        print("semantic_regression: PASS")
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


if __name__ == "__main__":
    main()
