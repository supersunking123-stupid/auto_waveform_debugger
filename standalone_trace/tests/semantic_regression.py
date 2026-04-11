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
        part_db = tmpdir / "semantic_part.db"
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

        # 2) partitioned compile should preserve query-visible behavior
        c_part = run_cmd(
            [
                str(rtl_trace),
                "compile",
                "--db",
                str(part_db),
                "--partition-budget",
                "1",
                "--single-unit",
                str(fixture),
                "--top",
                "semantic_top",
            ]
        )
        if "signals:" not in c_part.stdout:
            raise AssertionError(f"partition compile output missing signals summary: {c_part.stdout}")

        # 3) drivers can cross submodule input port and reach producer assignments
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
        part_drivers = run_trace_json(rtl_trace, part_db, "drivers", "semantic_top.u_cons.in_bus")
        if part_drivers != drivers:
            raise AssertionError(
                "partitioned compile changed drivers query output:\n"
                f"default={json.dumps(drivers, sort_keys=True)}\n"
                f"partitioned={json.dumps(part_drivers, sort_keys=True)}"
            )

        # 4) better loads assignment context: condition expression should carry LHS path
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
        # This endpoint carries exact LHS refs, so compile is allowed to keep only
        # source offsets in the DB and let query-side materialization recover the
        # assignment text after reload.
        assert_any(
            hit_loads.get("endpoints", []),
            lambda e: e.get("path") == "semantic_top.hit"
            and e.get("assignment") == "flag <= hit"
            and e.get("lhs") == ["semantic_top.flag"],
            "lazy assignment-text materialization failed for semantic_top.hit loads",
        )
        part_hit_loads = run_trace_json(rtl_trace, part_db, "loads", "semantic_top.hit")
        if part_hit_loads != hit_loads:
            raise AssertionError(
                "partitioned compile changed loads query output:\n"
                f"default={json.dumps(hit_loads, sort_keys=True)}\n"
                f"partitioned={json.dumps(part_hit_loads, sort_keys=True)}"
            )

        # 5) bit-level precision and bit-select query filtering
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

        # 6) traversal controls: depth / node limits should emit stops
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

        # 7) find typo suggestions
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

        # 8) incremental compile cache hit
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

        # 9) invalid numeric args should return a parse error instead of terminating
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

        # 10) hierarchy query can optionally expose definition source locations
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

        whereis_params = run_json_cmd(
            [
                str(rtl_trace),
                "whereis-instance",
                "--db",
                str(db),
                "--instance",
                "semantic_top.u_param",
                "--format",
                "json",
                "--show-params",
            ]
        )
        if whereis_params.get("module") != "param_leaf":
            raise AssertionError(f"unexpected parameterized instance module info: {whereis_params}")
        params = {p["name"]: p for p in whereis_params.get("parameters", [])}
        for required in ("WIDTH", "T", "DOUBLE_WIDTH"):
            if required not in params:
                raise AssertionError(f"missing expected instance parameter {required}: {whereis_params}")
        width = params["WIDTH"]
        if (
            width.get("kind") != "value"
            or width.get("is_local")
            or not width.get("is_port")
            or not width.get("is_overridden")
            or "6" not in width.get("value", "")
        ):
            raise AssertionError(f"unexpected WIDTH parameter payload: {width}")
        type_param = params["T"]
        if (
            type_param.get("kind") != "type"
            or type_param.get("is_local")
            or not type_param.get("is_port")
            or not type_param.get("is_overridden")
            or "logic" not in type_param.get("value", "")
            or "5:0" not in type_param.get("value", "")
        ):
            raise AssertionError(f"unexpected T parameter payload: {type_param}")
        double_width = params["DOUBLE_WIDTH"]
        if (
            double_width.get("kind") != "value"
            or not double_width.get("is_local")
            or double_width.get("is_port")
            or double_width.get("is_overridden")
            or "12" not in double_width.get("value", "")
        ):
            raise AssertionError(f"unexpected DOUBLE_WIDTH parameter payload: {double_width}")

        # ---- Tests 9-20 (new test additions) ----

        # 11) Test 9 — Trace with --cone-level 2
        cone2 = run_trace_json(
            rtl_trace, db, "drivers", "semantic_top.u_cons.in_bus",
            extra=["--cone-level", "2"],
        )
        cone2_endpoints = cone2.get("endpoints", [])
        if len(cone2_endpoints) == 0:
            raise AssertionError(f"cone-level 2 returned no endpoints: {cone2}")
        # Verify cone-level 2 returns valid JSON with summary
        cone2_summary = cone2.get("summary", {})
        if cone2_summary.get("cone_level") != 2:
            raise AssertionError(f"cone-level 2 summary doesn't report cone_level=2: {cone2_summary}")

        # 12) Test 10 — Trace with --prefer-port-hop
        hop = run_trace_json(
            rtl_trace, db, "drivers", "semantic_top.u_cons.in_bus",
            extra=["--prefer-port-hop"],
        )
        hop_endpoints = hop.get("endpoints", [])
        if len(hop_endpoints) == 0:
            raise AssertionError(f"prefer-port-hop returned no endpoints: {hop}")

        # 13) Test 11 — Trace with --include regex filter
        inc_prod = run_trace_json(
            rtl_trace, db, "drivers", "semantic_top.u_cons.in_bus",
            extra=["--include", "u_prod.*"],
        )
        for ep in inc_prod.get("endpoints", []):
            path = ep.get("path", "")
            if "u_prod" not in path:
                raise AssertionError(
                    f"--include 'u_prod.*' should filter out non-matching paths, got: {path}"
                )

        inc_none = run_trace_json(
            rtl_trace, db, "drivers", "semantic_top.u_cons.in_bus",
            extra=["--include", "nonexistent_.*"],
        )
        if len(inc_none.get("endpoints", [])) != 0:
            raise AssertionError(
                f"--include 'nonexistent_.*' should return zero endpoints: {inc_none}"
            )

        # 14) Test 12 — Trace with --exclude regex filter
        exc_cons = run_trace_json(
            rtl_trace, db, "drivers", "semantic_top.u_cons.in_bus",
            extra=["--exclude", "u_cons.*"],
        )
        for ep in exc_cons.get("endpoints", []):
            path = ep.get("path", "")
            if "u_cons" in path:
                raise AssertionError(
                    f"--exclude 'u_cons.*' should remove matching paths, got: {path}"
                )

        # 15) Test 13 — Trace with --stop-at regex
        stop_at = run_trace_json(
            rtl_trace, db, "drivers", "semantic_top.data",
            extra=["--stop-at", "u_prod.*", "--depth", "10"],
        )
        stops = stop_at.get("stops", [])
        # The --stop-at flag is accepted without error; verify stops exist
        if len(stops) == 0:
            raise AssertionError(f"expected at least one stop: {stop_at}")

        # 16) Test 14 — hier command basics
        hier1 = run_json_cmd(
            [
                str(rtl_trace),
                "hier",
                "--db", str(db),
                "--root", "semantic_top",
                "--depth", "1",
                "--format", "json",
            ]
        )
        if "children" not in hier1 and "nodes" not in hier1 and "tree" not in hier1:
            raise AssertionError(f"hier output missing children/nodes/tree: {hier1}")

        hier0 = run_cmd(
            [
                str(rtl_trace),
                "hier",
                "--db", str(db),
                "--depth", "0",
            ],
            expect=0,
        )

        # 17) Test 15 — find with --regex
        find_re = run_json_cmd(
            [
                str(rtl_trace),
                "find",
                "--db", str(db),
                "--query", "u_prod\\.data",
                "--regex",
                "--format", "json",
            ]
        )
        find_re_matches = find_re if isinstance(find_re, list) else find_re.get("matches", find_re.get("results", []))
        if not any("u_prod.data" in str(m) for m in find_re_matches):
            raise AssertionError(f"find regex should match u_prod.data: {find_re}")

        find_none = run_cmd(
            [
                str(rtl_trace),
                "find",
                "--db", str(db),
                "--query", "zzzzz",
                "--regex",
                "--format", "json",
            ],
            expect=2,  # exit code 2 means no matches found
        )
        # With --format json, check that the matches list is empty
        # (suggestions may still appear, but matches should be 0)
        try:
            find_none_json = json.loads(find_none.stdout)
            find_none_count = find_none_json.get("count", len(find_none_json) if isinstance(find_none_json, list) else -1)
            if find_none_count != 0:
                raise AssertionError(f"find 'zzzzz' should return 0 matches: {find_none.stdout}")
        except json.JSONDecodeError:
            # Non-JSON output: just check count: 0 appears
            if "count: 0" not in find_none.stdout:
                raise AssertionError(f"find 'zzzzz' should show count 0: {find_none.stdout}")

        # 18) Test 16 — find JSON output format
        find_json = run_json_cmd(
            [
                str(rtl_trace),
                "find",
                "--db", str(db),
                "--query", "data",
                "--format", "json",
            ]
        )
        find_entries = find_json if isinstance(find_json, list) else find_json.get("matches", find_json.get("results", []))
        if isinstance(find_entries, list):
            for entry in find_entries:
                # Entries may be plain strings (signal paths) or dicts with "path" key
                if isinstance(entry, str):
                    continue  # plain string path is valid
                if isinstance(entry, dict) and "path" not in entry:
                    raise AssertionError(f"find JSON entry missing 'path' field: {entry}")

        # 19) Test 17 — Invalid signal in trace
        bad_sig_proc = subprocess.run(
            [
                str(rtl_trace),
                "trace",
                "--db", str(db),
                "--mode", "drivers",
                "--signal", "semantic_top.nonexistent_signal",
                "--format", "json",
            ],
            text=True,
            capture_output=True,
        )
        combined = (bad_sig_proc.stdout + bad_sig_proc.stderr).lower()
        if bad_sig_proc.returncode == 0:
            if "error" not in combined and "not found" not in combined and "suggestions" not in combined:
                raise AssertionError(
                    f"invalid signal should return error/not found/suggestions: rc={bad_sig_proc.returncode} out={bad_sig_proc.stdout}"
                )

        # 20) Test 18 — Missing DB file
        missing_db = subprocess.run(
            [
                str(rtl_trace),
                "trace",
                "--db", "/tmp/nonexistent_test_db_xyz.db",
                "--mode", "drivers",
                "--signal", "top.sig",
            ],
            text=True,
            capture_output=True,
        )
        if missing_db.returncode == 0:
            raise AssertionError(
                f"missing DB should return non-zero exit code: rc={missing_db.returncode}"
            )

        # 21) Test 19 — --format text output validation (default format)
        text_proc = run_cmd(
            [
                str(rtl_trace),
                "trace",
                "--db", str(db),
                "--mode", "drivers",
                "--signal", "semantic_top.u_cons.in_bus",
            ]
        )
        text_out_lower = text_proc.stdout.lower()
        if "signals:" not in text_out_lower and "endpoint" not in text_out_lower and "driver" not in text_out_lower:
            raise AssertionError(
                f"text output should contain signals/endpoint/driver: {text_proc.stdout}"
            )

        # 22) Test 20 — Incremental compile cache miss
        inc_miss_db = tmpdir / "semantic_inc_miss.db"
        tmp_fixture = tmpdir / "semantic_top_modified.sv"
        shutil.copy(str(fixture), str(tmp_fixture))
        run_cmd(
            [
                str(rtl_trace),
                "compile",
                "--db", str(inc_miss_db),
                "--incremental",
                "--single-unit",
                str(tmp_fixture),
                "--top", "semantic_top",
            ]
        )
        # Append a comment to the source file to force cache miss
        with open(str(tmp_fixture), "a") as f:
            f.write("\n// cache miss trigger comment\n")
        inc_miss2 = run_cmd(
            [
                str(rtl_trace),
                "compile",
                "--db", str(inc_miss_db),
                "--incremental",
                "--single-unit",
                str(tmp_fixture),
                "--top", "semantic_top",
            ]
        )
        if "incremental-cache-hit" in inc_miss2.stdout:
            raise AssertionError(
                f"modified source should NOT produce incremental-cache-hit: {inc_miss2.stdout}"
            )

        print("semantic_regression: PASS")
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


if __name__ == "__main__":
    main()
