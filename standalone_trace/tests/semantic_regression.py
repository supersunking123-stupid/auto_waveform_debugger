#!/usr/bin/env python3
import argparse
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path


def run_cmd(cmd, cwd=None, expect=0, env=None):
    run_cwd = None if cwd is None else str(cwd)
    run_env = None
    if env:
        run_env = dict(os.environ)
        run_env.update(env)
    proc = subprocess.run(cmd, cwd=run_cwd, text=True, capture_output=True, env=run_env)
    if proc.returncode != expect:
        raise AssertionError(
            "Command failed\n"
            f"cmd: {' '.join(cmd)}\n"
            f"cwd: {run_cwd}\n"
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


def assert_absolute_reported_path(path_str, context):
    if not Path(path_str).is_absolute():
        raise AssertionError(f"{context} should be an absolute path, got: {path_str}")


def assert_nonabsolute_reported_path(path_str, context):
    if Path(path_str).is_absolute():
        raise AssertionError(f"{context} should use the legacy logical path form, got: {path_str}")


def main():
    ap = argparse.ArgumentParser(description="Standalone trace semantic regression suite")
    ap.add_argument("--rtl-trace", required=True, help="Path to rtl_trace binary")
    ap.add_argument("--source-dir", required=True, help="Path to standalone_trace source dir")
    args = ap.parse_args()

    rtl_trace = Path(args.rtl_trace).resolve()
    src_dir = Path(args.source_dir).resolve()
    repo_root = src_dir.parent
    fixture = src_dir / "tests" / "fixtures" / "semantic_top.sv"
    logical_fixture = fixture.relative_to(repo_root)
    if not rtl_trace.exists():
        raise SystemExit(f"rtl_trace not found: {rtl_trace}")
    if not fixture.exists():
        raise SystemExit(f"fixture not found: {fixture}")

    tmpdir = Path(tempfile.mkdtemp(prefix="rtl_trace_semreg_"))
    try:
        db = tmpdir / "semantic.db"
        part_db = tmpdir / "semantic_part.db"
        low_mem_db = tmpdir / "semantic_low_mem.db"
        inc_db = tmpdir / "semantic_inc.db"
        v3_db = tmpdir / "semantic_v3.db"
        logical_db = tmpdir / "semantic_logical.db"
        physical_source_path_flag = "--physical-source-paths"

        # 1) default compile preserves legacy logical source paths
        run_cmd(
            [
                str(rtl_trace),
                "compile",
                "--db",
                str(logical_db),
                "--single-unit",
                str(logical_fixture),
                "--top",
                "semantic_top",
            ],
            cwd=repo_root,
        )
        logical_whereis = run_json_cmd(
            [
                str(rtl_trace),
                "whereis-instance",
                "--db",
                str(logical_db),
                "--instance",
                "semantic_top.u_cons",
                "--format",
                "json",
            ]
        )
        logical_source = logical_whereis.get("source")
        if (
            logical_source is None
            or normalize_reported_path(logical_source.get("file", "")) != str(fixture)
            or logical_source.get("line") != 15
        ):
            raise AssertionError(f"unexpected logical whereis source info: {logical_whereis}")
        assert_nonabsolute_reported_path(logical_source.get("file", ""), "default whereis source file")

        # 2) compile smoke with physical absolute source paths
        c = run_cmd(
            [
                str(rtl_trace),
                "compile",
                "--db",
                str(db),
                physical_source_path_flag,
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
                physical_source_path_flag,
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

        c_low_mem = run_cmd(
            [
                str(rtl_trace),
                "compile",
                "--db",
                str(low_mem_db),
                physical_source_path_flag,
                "--low-mem",
                "--single-unit",
                str(fixture),
                "--top",
                "semantic_top",
            ]
        )
        if "signals:" not in c_low_mem.stdout:
            raise AssertionError(f"low-mem compile output missing signals summary: {c_low_mem.stdout}")

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
        for ep in endpoints:
            if normalize_reported_path(ep.get("file", "")) != str(fixture):
                raise AssertionError(f"unexpected endpoint source info: {ep}")
            assert_absolute_reported_path(ep.get("file", ""), f"drivers endpoint {ep.get('path', '')}")
        part_drivers = run_trace_json(rtl_trace, part_db, "drivers", "semantic_top.u_cons.in_bus")
        if part_drivers != drivers:
            raise AssertionError(
                "partitioned compile changed drivers query output:\n"
                f"default={json.dumps(drivers, sort_keys=True)}\n"
                f"partitioned={json.dumps(part_drivers, sort_keys=True)}"
            )
        low_mem_drivers = run_trace_json(rtl_trace, low_mem_db, "drivers", "semantic_top.u_cons.in_bus")
        if low_mem_drivers != drivers:
            raise AssertionError(
                "low-mem compile changed drivers query output:\n"
                f"default={json.dumps(drivers, sort_keys=True)}\n"
                f"low_mem={json.dumps(low_mem_drivers, sort_keys=True)}"
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
        low_mem_hit_loads = run_trace_json(rtl_trace, low_mem_db, "loads", "semantic_top.hit")
        if low_mem_hit_loads != hit_loads:
            raise AssertionError(
                "low-mem compile changed loads query output:\n"
                f"default={json.dumps(hit_loads, sort_keys=True)}\n"
                f"low_mem={json.dumps(low_mem_hit_loads, sort_keys=True)}"
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

        # 6) traversal controls limit query-time logic-cone expansion. Port
        # connections are already resolved while building directional lists.
        depth_limit = run_trace_json(
            rtl_trace,
            db,
            "drivers",
            "semantic_top.hit",
            extra=["--cone-level", "2", "--depth", "0"],
        )
        if not any(s.get("reason") == "depth_limit" for s in depth_limit.get("stops", [])):
            raise AssertionError(f"expected depth_limit stop: {depth_limit}")

        node_limit = run_trace_json(
            rtl_trace,
            db,
            "drivers",
            "semantic_top.hit",
            extra=["--cone-level", "2", "--max-nodes", "1"],
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
                physical_source_path_flag,
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
                physical_source_path_flag,
                "--single-unit",
                str(fixture),
                "--top",
                "semantic_top",
            ]
        )
        if "signals: incremental-cache-hit" not in inc2.stdout:
            raise AssertionError(f"incremental cache hit missing:\n{inc2.stdout}")
        # 8b) a DB built with older compile semantics must not be reused: rewrite the .meta to the
        # epoch-1 form (no SEMANTICS_EPOCH line, as written before the endpoint-merge fix).
        inc_meta = Path(str(inc_db) + ".meta")
        current_meta = inc_meta.read_text()
        meta_lines = current_meta.splitlines(keepends=True)
        if not any(l.startswith("SEMANTICS_EPOCH:") for l in meta_lines):
            raise AssertionError(f"compile fingerprint lacks SEMANTICS_EPOCH:\n{current_meta}")
        inc_meta.write_text("".join(l for l in meta_lines if not l.startswith("SEMANTICS_EPOCH:")))
        inc3 = run_cmd(
            [
                str(rtl_trace),
                "compile",
                "--db",
                str(inc_db),
                "--incremental",
                physical_source_path_flag,
                "--single-unit",
                str(fixture),
                "--top",
                "semantic_top",
            ]
        )
        if "incremental-cache-hit" in inc3.stdout:
            raise AssertionError(f"prior-epoch .meta must force a rebuild:\n{inc3.stdout}")
        if inc_meta.read_text() != current_meta:
            raise AssertionError("rebuild after an epoch change must rewrite the current .meta")

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
        assert_absolute_reported_path(source.get("file", ""), "hier source file")

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
        assert_absolute_reported_path(whereis_source.get("file", ""), "whereis source file")

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

        # v3 DB backward compat: create a proper v4-format DB, then patch version to 3.
        # v5 has 32-byte signal records; we must strip the 3 extra uint32_t fields
        # per record so the v4 loader (20-byte records) can read it.
        # We also need to truncate the v4-only sections (hierarchy params) at the end.
        import struct as _struct
        _orig = open(db, "rb").read()
        _hdr_size = _struct.calcsize("16sII15Q")  # 16+4+4+120 = 144
        _hdr = _struct.unpack_from("16sII15Q", _orig, 0)
        _fields = _hdr[3:]
        _str_count = _fields[0]   # string_count
        _str_blob_sz = _fields[1] # string_blob_size
        _sig_count = _fields[2]   # signal_count
        _ep_count = _fields[3]    # endpoint_count
        _sr_count = _fields[4]    # signal_ref_count
        _lrr_count = _fields[5]   # load_ref_range_count
        _lr_count = _fields[6]    # load_ref_count
        _drr_count = _fields[7]   # driver_ref_range_count
        _dr_count = _fields[8]    # driver_ref_count
        _lhrr_count = _fields[9]  # assignment_lhs_ref_range_count
        _lhr_count = _fields[10]  # assignment_lhs_ref_count
        _hier_count = _fields[11] # hierarchy_count
        _hier_child_count = _fields[12] # hierarchy_child_count
        _gn_count = _fields[13]   # global_net_count
        _gs_count = _fields[14]   # global_sink_count

        # Walk file layout with correct struct sizes:
        # GraphPathRefRange = 12 bytes (3 uint32_t)
        # GraphEndpointRecord = 48 bytes
        # GraphHierarchyRecord = 24 bytes (v3+, 6 uint32_t)
        # GraphGlobalNetRecord = 12 bytes
        _str_off_end = _hdr_size + (_str_count + 1) * 4  # uint32_t offsets
        _sig_start = _str_off_end + _str_blob_sz
        _sig_end_v5 = _sig_start + _sig_count * 32

        # Compute v3 boundary (all sections before v4-only param data)
        _v3_end = _sig_end_v5
        _v3_end += _ep_count * 48           # endpoints
        _v3_end += _sr_count * 4            # signal_refs
        _v3_end += _lrr_count * 12          # load_ref_ranges
        _v3_end += _lr_count * 4            # load_ref_signal_ids
        _v3_end += _drr_count * 12          # driver_ref_ranges
        _v3_end += _dr_count * 4            # driver_ref_signal_ids
        _v3_end += _lhrr_count * 12         # assignment_lhs_ref_ranges
        _v3_end += _lhr_count * 4           # assignment_lhs_ref_signal_ids
        _v3_end += _hier_count * 24         # hierarchy
        _v3_end += _hier_child_count * 4    # hierarchy_children
        _v3_end += _gn_count * 12           # global_nets
        _v3_end += _gs_count * 4            # global_sinks

        # Build v3 file: strip v5 signal records to v4, truncate at v3 boundary
        _before_sigs = _orig[:_sig_start]
        _v5_sigs = _orig[_sig_start:_sig_end_v5]
        _v4_sigs = bytearray()
        for _i in range(_sig_count):
            _v4_sigs += _v5_sigs[_i*32:_i*32+20]  # keep first 5 uint32_t
        _post_sig = _orig[_sig_end_v5:_v3_end]
        _out = bytearray(_before_sigs) + _v4_sigs + bytearray(_post_sig)
        # Patch version to 3
        _struct.pack_into("<I", _out, 16, 3)
        with open(v3_db, "wb") as _f:
            _f.write(_out)

        whereis_params_v3 = run_json_cmd(
            [
                str(rtl_trace),
                "whereis-instance",
                "--db",
                str(v3_db),
                "--instance",
                "semantic_top.u_param",
                "--format",
                "json",
                "--show-params",
            ]
        )
        if whereis_params_v3.get("parameters") is not None:
            raise AssertionError(f"expected parameters to be unavailable for v3 DB: {whereis_params_v3}")
        reason = whereis_params_v3.get("parameters_unavailable_reason", "")
        if "db version 3 does not store instance parameter metadata" not in reason:
            raise AssertionError(f"unexpected v3 parameter diagnostic: {whereis_params_v3}")

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
                physical_source_path_flag,
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
                physical_source_path_flag,
                "--single-unit",
                str(tmp_fixture),
                "--top", "semantic_top",
            ]
        )
        if "incremental-cache-hit" in inc_miss2.stdout:
            raise AssertionError(
                f"modified source should NOT produce incremental-cache-hit: {inc_miss2.stdout}"
            )

        # 23) Test — severity remapping: --compat vcs downgrades IndexOOB from Error to Warning
        compat_fixture = src_dir / "tests" / "fixtures" / "compat_severity.sv"
        if not compat_fixture.exists():
            raise SystemExit(f"fixture not found: {compat_fixture}")
        compat_db = tmpdir / "compat_severity.db"
        compat_db_nocompat = tmpdir / "compat_severity_nocompat.db"

        # With --compat vcs: IndexOOB is Warning, compile should succeed
        c_compat = run_cmd(
            [
                str(rtl_trace),
                "compile",
                "--db", str(compat_db),
                physical_source_path_flag,
                "--single-unit",
                str(compat_fixture),
                "--top", "compat_severity_top",
                "--compat", "vcs",
            ]
        )
        if "signals:" not in c_compat.stdout:
            raise AssertionError(f"--compat vcs compile should succeed: {c_compat.stdout}")

        # Without --compat vcs: IndexOOB is Error, compile should fail
        run_cmd(
            [
                str(rtl_trace),
                "compile",
                "--db", str(compat_db_nocompat),
                physical_source_path_flag,
                "--single-unit",
                str(compat_fixture),
                "--top", "compat_severity_top",
            ],
            expect=1,
        )

        # 24) Struct member access — packed struct field drivers/loads
        struct_fixture = src_dir / "tests" / "fixtures" / "struct_top.sv"
        if not struct_fixture.exists():
            raise SystemExit(f"fixture not found: {struct_fixture}")
        struct_db = tmpdir / "struct.db"

        run_cmd(
            [
                str(rtl_trace), "compile",
                "--db", str(struct_db),
                physical_source_path_flag,
                "--single-unit", str(struct_fixture),
                "--top", "struct_top",
            ]
        )

        # Drivers of struct_prod.pkt should include pkt.valid assignment
        pkt_drivers = run_trace_json(rtl_trace, struct_db, "drivers", "struct_top.u_prod.pkt")
        assert_any(
            pkt_drivers.get("endpoints", []),
            lambda e: "pkt.valid" in e.get("path", ""),
            "missing struct field driver for pkt.valid",
        )

        # Loads of struct_cons.pkt should include pkt.valid read
        pkt_loads = run_trace_json(rtl_trace, struct_db, "loads", "struct_top.u_cons.pkt")
        assert_any(
            pkt_loads.get("endpoints", []),
            lambda e: "pkt.valid" in e.get("path", "") or "pkt.code" in e.get("path", ""),
            "missing struct field load for pkt.valid or pkt.code",
        )

        # Bit-map should encode correct absolute range for sub-selected member
        # pkt.data is at bits [3:0]; pkt.data[1:0] should produce [1:0], not [3:0]
        data_loads = run_trace_json(rtl_trace, struct_db, "loads", "struct_top.u_cons.pkt")
        assert_any(
            data_loads.get("endpoints", []),
            lambda e: "pkt.data" in e.get("path", "") and e.get("bit_map", "") == "[1:0]",
            f"pkt.data[1:0] should have bit_map '[1:0]', got: "
            + str([e.get("bit_map") for e in data_loads.get("endpoints", []) if "pkt.data" in e.get("path", "")]),
        )

        # 25) Multi-member port connection: {pkt.valid, pkt.code[0]} must
        #     produce two distinct member endpoints, not collapse to one.
        concat_fixture = src_dir / "tests" / "fixtures" / "concat_top.sv"
        if not concat_fixture.exists():
            raise SystemExit(f"fixture not found: {concat_fixture}")
        concat_db = tmpdir / "concat.db"

        run_cmd(
            [
                str(rtl_trace), "compile",
                "--db", str(concat_db),
                physical_source_path_flag,
                "--single-unit", str(concat_fixture),
                "--top", "concat_top",
            ]
        )

        concat_loads = run_trace_json(rtl_trace, concat_db, "loads", "concat_top.u_src.pkt")
        assert_any(
            concat_loads.get("endpoints", []),
            lambda e: "pkt.valid" in e.get("path", ""),
            "multi-member port: pkt.valid endpoint missing",
        )
        assert_any(
            concat_loads.get("endpoints", []),
            lambda e: "pkt.code" in e.get("path", ""),
            "multi-member port: pkt.code endpoint missing",
        )

        # 26) Level 2 — find discovers struct members as first-class signals
        find_valid = run_json_cmd(
            [
                str(rtl_trace), "find",
                "--db", str(struct_db),
                "--query", "pkt.valid",
                "--format", "json",
            ]
        )
        find_entries = find_valid if isinstance(find_valid, list) else find_valid.get("matches", find_valid.get("results", []))
        member_paths = [e if isinstance(e, str) else e.get("path", "") for e in find_entries]
        if not any("pkt.valid" in p for p in member_paths):
            raise AssertionError(f"find 'pkt.valid' should discover member signals: {find_valid}")

        # 27) Level 2 — trace member signal returns filtered endpoints
        valid_drivers = run_trace_json(rtl_trace, struct_db, "drivers", "struct_top.u_prod.pkt.valid")
        valid_eps = valid_drivers.get("endpoints", [])
        # Should only contain pkt.valid assignments, not pkt.code or pkt.data
        for ep in valid_eps:
            if "pkt.code" in ep.get("path", "") or "pkt.data" in ep.get("path", ""):
                raise AssertionError(
                    f"pkt.valid drivers should not include other members: {ep}"
                )
        if len(valid_eps) < 2:
            raise AssertionError(f"pkt.valid should have >= 2 drivers (reset+increment), got {len(valid_eps)}: {valid_drivers}")

        valid_loads = run_trace_json(rtl_trace, struct_db, "loads", "struct_top.u_cons.pkt.valid")
        valid_load_eps = valid_loads.get("endpoints", [])
        # Should only contain the hit = pkt.valid & ... assignment
        for ep in valid_load_eps:
            if "pkt.data" in ep.get("path", "") or "pkt.code" in ep.get("path", ""):
                raise AssertionError(
                    f"pkt.valid loads should not include other members: {ep}"
                )

        # 28) Level 2 — member signal RHS must not be corrupted by sibling members.
        #     The hit = pkt.valid & pkt.code[0] endpoint reads pkt (the parent struct),
        #     NOT pkt.data (which was the bug from reusing parent Symbol* in
        #     symbol_path_ids — last member won and rewrote all LHS/RHS refs).
        valid_loads_rh = run_trace_json(rtl_trace, struct_db, "loads", "struct_top.u_cons.pkt.valid")
        assert_any(
            valid_loads_rh.get("endpoints", []),
            lambda e: "pkt.valid" in e.get("path", "") and
                      any("pkt.data" not in r for r in e.get("rhs", [])),
            "pkt.valid loads: RHS should reference the parent struct, not pkt.data "
            "(symbol_path_ids corruption from member signal reuse)",
        )
        # Also check no endpoint path is rewritten to a wrong sibling
        for ep in valid_loads_rh.get("endpoints", []):
            for r in ep.get("rhs", []):
                if "pkt.data" in r:
                    raise AssertionError(
                        f"pkt.valid loads: rhs should not reference pkt.data, got rhs={ep['rhs']}"
                    )

        # 29) Level 2 — code member drivers must not expand into data at cone-level 1
        code_drivers = run_trace_json(rtl_trace, struct_db, "drivers", "struct_top.u_prod.pkt.code")
        for ep in code_drivers.get("endpoints", []):
            if "pkt.data" in ep.get("path", ""):
                raise AssertionError(
                    f"pkt.code drivers at cone-level 1 should not contain pkt.data: {ep}"
                )

        # 30) Signal vector reallocation during struct decomposition must not change the DB.
        # RTL_TRACE_SIGNALS_RESERVE=1 forces `signals` to grow while
        # DecomposePackedStructFields runs (regression for a dangling reference into
        # the vector; run under ASan to catch it deterministically).
        for realloc_fixture, realloc_top in (("struct_top.sv", "struct_top"),
                                             ("struct_nested.sv", "nested_top")):
            fx_path = src_dir / "tests" / "fixtures" / realloc_fixture
            db_default = tmpdir / f"{realloc_top}_default.db"
            db_realloc = tmpdir / f"{realloc_top}_realloc.db"
            for db_path, env in ((db_default, None), (db_realloc, {"RTL_TRACE_SIGNALS_RESERVE": "1"})):
                run_cmd(
                    [str(rtl_trace), "compile", "--db", str(db_path), physical_source_path_flag,
                     "--single-unit", str(fx_path), "--top", realloc_top],
                    env=env,
                )
            if db_default.read_bytes() != db_realloc.read_bytes():
                raise AssertionError(
                    f"{realloc_fixture}: DB differs when the signal vector is forced to reallocate"
                )

        # 31) Endpoint bit-range merging: endpoints of the same assignment with adjacent or
        # overlapping exact bit ranges collapse into one endpoint; multi-dimensional selects
        # preserve logical axis coordinates.
        merge_fixture = src_dir / "tests" / "fixtures" / "endpoint_merge.sv"
        merge_db = tmpdir / "endpoint_merge.db"
        run_cmd(
            [str(rtl_trace), "compile", "--db", str(merge_db), physical_source_path_flag,
             "--single-unit", str(merge_fixture), "--top", "endpoint_merge"],
        )

        def merge_bit_maps(mode, signal):
            payload = run_trace_json(rtl_trace, merge_db, mode, signal)
            return sorted((e.get("line"), e.get("bit_map")) for e in payload.get("endpoints", []))

        a_loads = merge_bit_maps("loads", "endpoint_merge.a")
        expected_a_loads = [(16, "[3:0]"), (17, "[7:4]"), (20, "[3:0]"), (22, "[4]")]
        if a_loads != expected_a_loads:
            raise AssertionError(f"endpoint merge (a loads): expected {expected_a_loads}, got {a_loads}")
        g_drivers = merge_bit_maps("drivers", "endpoint_merge.g")
        expected_g_drivers = [(20, "[3:0]"), (22, "[4]")]
        if g_drivers != expected_g_drivers:
            raise AssertionError(
                f"endpoint merge (generate-loop drivers): expected {expected_g_drivers}, got {g_drivers}"
            )
        m_loads = merge_bit_maps("loads", "endpoint_merge.m")
        # Logical declared-axis coordinates keep the source indexes and axis order.
        # Multidimensional selects still do not merge into a flattened bit range.
        expected_m_loads = [(18, "[1][0]"), (18, "[1][1]"), (18, "[2][0]"), (18, "[2][1]")]
        if m_loads != expected_m_loads:
            raise AssertionError(
                f"endpoint merge must preserve logical multi-dimensional selects: expected {expected_m_loads}, "
                f"got {m_loads}"
            )
        # A root bit query narrows each merged output copy to the selected bit.
        a2_loads = merge_bit_maps("loads", "endpoint_merge.a[2]")
        expected_selected = "[2]"
        if a2_loads != [(16, expected_selected), (20, expected_selected)]:
            raise AssertionError(f"endpoint merge (a[2] loads): got {a2_loads}")
        # ===== BEGIN find_fastpath tests (parallel top-k find / literal prefilter / suggestions) =====
        run_find_fastpath_tests(rtl_trace, db, tmpdir)
        # ===== END find_fastpath tests =====
        # ===== BEGIN invalid_regex tests =====
        run_invalid_regex_tests(rtl_trace, db)
        # ===== END invalid_regex tests =====
        # ===== BEGIN canonical_bodies tests (canonical-body tracer vs RTL_TRACE_CANONICAL_BODIES=0) =====
        run_canonical_bodies_tests(rtl_trace, src_dir, tmpdir, physical_source_path_flag)
        # ===== END canonical_bodies tests =====

        print("semantic_regression: PASS")
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


# ===== BEGIN invalid_regex tests =====
# A malformed user regex (find --regex, trace --include/--exclude/--stop-at) must be reported as
# an argument error: non-zero exit without a crash (no abort/core dump), a clear message naming
# the option and pattern, and nothing on stdout. `serve` must survive it and keep answering.

def _assert_regex_rejected(proc, option, pattern, label):
    if proc.returncode == 0:
        raise AssertionError(f"[{label}] invalid regex must fail, got rc=0\n{proc.stdout}")
    if proc.returncode < 0 or proc.returncode == 134 or proc.returncode > 128:
        raise AssertionError(f"[{label}] invalid regex crashed the process: rc={proc.returncode}\n{proc.stderr}")
    want = f"Invalid regex for {option}: '{pattern}'"
    if want not in proc.stderr or "regex_error" in proc.stderr or "terminate" in proc.stderr:
        raise AssertionError(f"[{label}] expected {want!r} in stderr, got:\n{proc.stderr}")
    if proc.stdout != "":
        raise AssertionError(f"[{label}] invalid regex must not print to stdout:\n{proc.stdout}")


class _ServeClient:
    """Minimal `rtl_trace serve` driver: one command line in, stdout up to <<END>> out."""

    def __init__(self, rtl_trace, db):
        self.proc = subprocess.Popen(
            [str(rtl_trace), "serve", "--db", str(db)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        self.read_response()  # startup banner

    def read_response(self):
        lines = []
        while True:
            line = self.proc.stdout.readline()
            if line == "":
                raise AssertionError(f"serve exited (rc={self.proc.poll()}) before <<END>>; got: {lines}")
            if line.rstrip("\n") == "<<END>>":
                return "".join(lines)
            lines.append(line)

    def query(self, cmd):
        self.proc.stdin.write(cmd + "\n")
        self.proc.stdin.flush()
        return self.read_response()

    def close(self):
        try:
            self.query("quit")
        except Exception:
            pass
        self.proc.stdin.close()
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        err = self.proc.stderr.read()
        self.proc.stdout.close()
        self.proc.stderr.close()
        return err


def run_invalid_regex_tests(rtl_trace, db):
    sig = "semantic_top.hit"
    base = [str(rtl_trace), "trace", "--db", str(db), "--mode", "drivers", "--signal", sig, "--format", "json"]
    good = subprocess.run(base, text=True, capture_output=True)
    if good.returncode != 0:
        raise AssertionError(f"invalid_regex: baseline trace failed:\n{good.stdout}{good.stderr}")

    # one-shot trace: every regex option is validated at argument-parse time
    for opt in ("--include", "--exclude", "--stop-at"):
        proc = subprocess.run(base + [opt, "zz("], text=True, capture_output=True)
        _assert_regex_rejected(proc, opt, "zz(", f"trace {opt}")
    # a valid pattern is still accepted for each option
    for opt in ("--include", "--exclude", "--stop-at"):
        proc = subprocess.run(base + [opt, "^zz_no_such$"], text=True, capture_output=True)
        if proc.returncode != 0:
            raise AssertionError(f"trace {opt} with a valid regex failed: rc={proc.returncode}\n{proc.stderr}")

    # serve: bad regex -> error on stderr, response still terminated, next queries answered
    client = _ServeClient(rtl_trace, db)
    try:
        trace_cmd = f"trace --mode drivers --signal {sig} --format json"
        want_trace = client.query(trace_cmd)
        if want_trace != good.stdout:
            raise AssertionError(f"serve trace differs from one-shot trace:\n{want_trace}\nvs\n{good.stdout}")
        want_find = client.query("find --query semantic_top.u_ --limit 3")
        if "signal semantic_top.u_cons.hi_nibble" not in want_find:
            raise AssertionError(f"serve baseline find unexpected:\n{want_find}")
        for bad_cmd in (
            "find --query zz( --regex",
            "find --query 'a[' --regex --format json",
            f"{trace_cmd} --include 'zz('",
            f"{trace_cmd} --exclude 'zz('",
            f"{trace_cmd} --stop-at 'zz('",
        ):
            out = client.query(bad_cmd)
            if out != "":
                raise AssertionError(f"serve: bad regex query {bad_cmd!r} produced stdout:\n{out}")
            if client.proc.poll() is not None:
                raise AssertionError(f"serve died on {bad_cmd!r}: rc={client.proc.returncode}")
            # session is still loaded and answers valid queries identically
            if client.query(trace_cmd) != want_trace:
                raise AssertionError(f"serve: trace answer changed after {bad_cmd!r}")
            if client.query("find --query semantic_top.u_ --limit 3") != want_find:
                raise AssertionError(f"serve: find answer changed after {bad_cmd!r}")
        if client.query("find --query 'semantic_top.u_.*\\.in_bus$' --regex --format json") == "":
            raise AssertionError("serve: valid regex find returned nothing after bad regex queries")
    finally:
        err = client.close()
    for want in ("Invalid regex for --query (--regex): 'zz('", "Invalid regex for --query (--regex): 'a['",
                 "Invalid regex for --include: 'zz('", "Invalid regex for --exclude: 'zz('",
                 "Invalid regex for --stop-at: 'zz('"):
        if want not in err:
            raise AssertionError(f"serve stderr missing {want!r}:\n{err}")
    if client.proc.returncode != 0:
        raise AssertionError(f"serve exit code after bad regexes: {client.proc.returncode}\n{err}")
# ===== END invalid_regex tests =====


# ===== BEGIN canonical_bodies tests =====
# By default rtl_trace traces signals of instances whose bodies slang skipped (instance caching) through the
# canonical body and translates paths back; RTL_TRACE_CANONICAL_BODIES=0 binds every body instead. The two DBs must
# be byte-identical. The fixtures
# under tests/fixtures/canonical_bodies/ cover the risky cases (defparam, parameter types, generate, up/down
# hierarchical refs, bind, interface ports/modports, virtual interfaces, multi-level sharing); each starts with
# `// top: <name>` and optionally `// args: <extra compile args>`.

def run_canonical_bodies_tests(rtl_trace, src_dir, tmpdir, physical_source_path_flag):
    cb_dir = src_dir / "tests" / "fixtures" / "canonical_bodies"
    fixtures = sorted(cb_dir.glob("*.sv"))
    if not fixtures:
        raise AssertionError(f"no canonical_bodies fixtures in {cb_dir}")
    total_redirected = 0
    for fx in fixtures:
        header = fx.read_text().splitlines()[:4]
        top = next((l.split(":", 1)[1].strip() for l in header if l.startswith("// top:")), None)
        extra = next((l.split(":", 1)[1].split() for l in header if l.startswith("// args:")), [])
        iface_case = next((l.split(":", 1)[1].strip() for l in header
                           if l.startswith("// canonical-iface:")), None)
        if top is None:
            raise AssertionError(f"{fx.name}: missing '// top:' header")
        for variant in ([], ["--low-mem"], ["--mfcu"]):
            dbs = {}
            for tag, mode in (("off", "0"), ("on", "1")):
                env = {"RTL_TRACE_CANONICAL_BODIES": mode,
                       "RTL_TRACE_CANONICAL_STATS": "1",
                       "RTL_TRACE_CANONICAL_VERIFY": "0",
                       "RTL_TRACE_CANONICAL_ALLOW_IFACE": "0"}
                dbs[tag] = tmpdir / f"cb_{fx.stem}_{tag}.db"
                proc = run_cmd(
                    [str(rtl_trace), "compile", "--db", str(dbs[tag]), physical_source_path_flag,
                     "--single-unit", str(fx), "--top", top, *extra, *variant],
                    env=env,
                )
                if tag == "on":
                    line = next((l for l in proc.stdout.splitlines() if "[Canon] tracer signals" in l), None)
                    if line is None:
                        raise AssertionError(f"{fx.name}: canonical tracer did not run")
                    stats = dict(kv.split("=", 1) for kv in line.split() if "=" in kv)
                    total_redirected += int(stats.get("redirected", "0"))
                    for counter in ("broken", "map_fail", "child_lookup_fail", "port_map_fail",
                                    "down_map_fail", "xlate_anomaly", "xlate_id_miss"):
                        if stats.get(counter) != "0":
                            raise AssertionError(f"{fx.name} {variant}: {counter}={stats.get(counter)}")
                    if iface_case:
                        skipped_lines = [l for l in proc.stdout.splitlines()
                                         if "[Canon] index_build class=skip" in l]
                        if len(skipped_lines) != 4:
                            raise AssertionError(f"{fx.name}: missing skipped-body build counters")
                        skipped_builds = sum(int(dict(kv.split("=", 1) for kv in l.split()
                                                      if "=" in kv)["builds"])
                                             for l in skipped_lines)
                        if iface_case == "supported":
                            if int(stats["redirected"]) <= 0:
                                raise AssertionError(f"{fx.name} {variant}: no redirected signals")
                            if stats.get("excluded_crossings(iface)") != "0" or skipped_builds != 0:
                                raise AssertionError(f"{fx.name} {variant}: interface fallback or skipped builds")
                        elif iface_case in ("alias-split", "alias-merge", "selfref",
                                            "unresolved", "shape", "conflict", "forwarding"):
                            if int(stats["excluded_crossings(iface)"]) <= 0 or skipped_builds <= 0:
                                raise AssertionError(f"{fx.name} {variant}: expected interface fallback")
                            reason = "iface_" + iface_case.replace("-", "_")
                            if int(stats.get(reason, "0")) <= 0:
                                raise AssertionError(f"{fx.name} {variant}: missing {reason} exclusion")
                        else:
                            raise AssertionError(f"{fx.name}: unknown canonical-iface case {iface_case!r}")
            for suffix in ("", ".meta"):
                a = Path(str(dbs["off"]) + suffix).read_bytes()
                b = Path(str(dbs["on"]) + suffix).read_bytes()
                if a != b:
                    raise AssertionError(
                        f"{fx.name} {variant}: canonical-body tracing changed the DB{suffix or ''} vs RTL_TRACE_CANONICAL_BODIES=0")
        # VERIFY binds actual bodies as its oracle. Keep it separate from the
        # normal variants above, whose counters must prove skipped-body avoidance.
        # Direct external interface field references are still absent from the
        # baseline collectors; this oracle checks the lists they currently emit.
        verify_db = tmpdir / f"cb_{fx.stem}_verify.db"
        verify_proc = run_cmd(
            [str(rtl_trace), "compile", "--db", str(verify_db), physical_source_path_flag,
             "--single-unit", str(fx), "--top", top, *extra],
            env={"RTL_TRACE_CANONICAL_BODIES": "1",
                 "RTL_TRACE_CANONICAL_STATS": "0",
                 "RTL_TRACE_CANONICAL_VERIFY": "1",
                 "RTL_TRACE_CANONICAL_ALLOW_IFACE": "0"},
        )
        verify_lines = [line for line in verify_proc.stdout.splitlines()
                        if line.startswith("[Canon] verify ")]
        if len(verify_lines) != 1:
            raise AssertionError(f"{fx.name}: expected one canonical VERIFY summary")
        verify_stats = dict(kv.split("=", 1) for kv in verify_lines[0].split() if "=" in kv)
        try:
            verified_signals = int(verify_stats["signals"])
            mismatched_lists = int(verify_stats["mismatched_lists"])
        except (KeyError, ValueError) as exc:
            raise AssertionError(f"{fx.name}: invalid VERIFY summary: {verify_lines[0]}") from exc
        if verified_signals <= 0 or mismatched_lists != 0:
            raise AssertionError(f"{fx.name}: canonical VERIFY failed: {verify_lines[0]}")
    if total_redirected == 0:
        raise AssertionError("canonical_bodies: no signal was traced through a canonical body")
# ===== END canonical_bodies tests =====


# ===== BEGIN find_fastpath tests =====
# `rtl_trace find` scans names in parallel (bounded top-k) and prefilters regexes with a required
# literal. These tests pin the observable behaviour: lexicographically sorted first --limit
# matches, count == min(matches, limit), exit code 2 on no match, suggestions ordered by
# (edit distance, name), and rejection of invalid regexes.

FIND_FASTPATH_BIG_SV = """
module ff_leaf(input logic clk, input logic [7:0] i_paddr, output logic [7:0] o_data);
  logic [7:0] r_a, r_b, paddr_q, wdata_i;
  always_ff @(posedge clk) begin r_a <= i_paddr; r_b <= r_a; paddr_q <= r_b; wdata_i <= paddr_q; end
  assign o_data = wdata_i;
endmodule
module ff_mid(input logic clk, input logic [7:0] i_paddr, output logic [7:0] o_data);
  logic [7:0] d [0:63];
  for (genvar k = 0; k < 64; k++) begin : g_leaf
    ff_leaf u_leaf(.clk(clk), .i_paddr(i_paddr), .o_data(d[k]));
  end
  assign o_data = d[0];
endmodule
module ff_top(input logic clk, input logic [7:0] i_paddr, output logic [7:0] o_data);
  logic [7:0] d [0:127];
  for (genvar m = 0; m < 128; m++) begin : g_mid
    ff_mid u_mid(.clk(clk), .i_paddr(i_paddr), .o_data(d[m]));
  end
  assign o_data = d[0];
endmodule
"""


def _find_raw(rtl_trace, db, query, regex=False, limit=None, fmt=None, expect=None):
    cmd = [str(rtl_trace), "find", "--db", str(db), "--query", query]
    if regex:
        cmd.append("--regex")
    if limit is not None:
        cmd += ["--limit", str(limit)]
    if fmt:
        cmd += ["--format", fmt]
    proc = subprocess.run(cmd, text=True, capture_output=True)
    if expect is not None and proc.returncode != expect:
        raise AssertionError(f"find rc {proc.returncode} != {expect}: {' '.join(cmd)}\n{proc.stdout}{proc.stderr}")
    return proc


def _find_text_names(proc):
    return [l[len("signal "):] for l in proc.stdout.splitlines() if l.startswith("signal ")]


def _find_check(rtl_trace, db, all_names, query, regex, limit, matcher, label):
    """Compare find (text and json) against a pure-Python reference for the same query."""
    import re as _re
    expected_all = sorted(n for n in all_names if matcher(n))
    expected = expected_all if limit is None else expected_all[:limit]
    want_rc = 0 if expected else 2
    txt = _find_raw(rtl_trace, db, query, regex, limit, None)
    if txt.returncode != want_rc:
        raise AssertionError(f"[{label}] text rc {txt.returncode} != {want_rc}\n{txt.stdout}{txt.stderr}")
    if _find_text_names(txt) != expected:
        raise AssertionError(f"[{label}] text matches differ: got {_find_text_names(txt)[:10]} want {expected[:10]}")
    if f"count: {len(expected)}\n" not in txt.stdout:
        raise AssertionError(f"[{label}] text count mismatch (want {len(expected)}):\n{txt.stdout[:300]}")
    js = _find_raw(rtl_trace, db, query, regex, limit, "json")
    if js.returncode != want_rc:
        raise AssertionError(f"[{label}] json rc {js.returncode} != {want_rc}")
    obj = json.loads(js.stdout)
    if obj["matches"] != expected or obj["count"] != len(expected) or obj["regex"] != regex or obj["query"] != query:
        raise AssertionError(f"[{label}] json mismatch: {js.stdout[:300]}")
    if expected and obj["suggestions"]:
        raise AssertionError(f"[{label}] suggestions must be empty when there are matches")


def run_find_fastpath_tests(rtl_trace, small_db, tmpdir):
    import re as _re

    # --- small design: exact expected output -------------------------------------------------
    proc = _find_raw(rtl_trace, small_db, "semantic_top.u_", limit=3, expect=0)
    want = (
        "query: semantic_top.u_\nregex: false\ncount: 3\n"
        "signal semantic_top.u_cons.hi_nibble\nsignal semantic_top.u_cons.hit\n"
        "signal semantic_top.u_cons.in_bus\n"
    )
    if proc.stdout != want:
        raise AssertionError(f"find_fastpath substring/limit text output changed:\n{proc.stdout}")

    proc = _find_raw(rtl_trace, small_db, "semantic_top.u_.*\\.in_bus$", regex=True, fmt="json", expect=0)
    want = (
        '{"query":"semantic_top.u_.*\\\\.in_bus$","regex":true,"count":2,'
        '"matches":["semantic_top.u_cons.in_bus","semantic_top.u_param.in_bus"],"suggestions":[]}\n'
    )
    if proc.stdout != want:
        raise AssertionError(f"find_fastpath regex json output changed:\n{proc.stdout}")

    # no match -> exit 2 with edit-distance ordered suggestions (ties broken by name)
    proc = _find_raw(rtl_trace, small_db, "semantic_top.hitt", limit=3, expect=2)
    want = (
        "query: semantic_top.hitt\nregex: false\ncount: 0\nsuggestions:\n"
        "  semantic_top.hit\n  semantic_top.data\n  semantic_top.clk\n"
    )
    if proc.stdout != want:
        raise AssertionError(f"find_fastpath suggestions output changed:\n{proc.stdout}")
    proc = _find_raw(rtl_trace, small_db, "zzz.*qqq", regex=True, limit=2, fmt="json", expect=2)
    obj = json.loads(proc.stdout)
    if obj["count"] != 0 or obj["matches"] != [] or len(obj["suggestions"]) != 2:
        raise AssertionError(f"find_fastpath regex no-match json unexpected: {proc.stdout}")

    # invalid regex: reported as an argument error (rc != 0, no abort/signal, nothing on stdout)
    for bad in ("(", "zz("):  # the second has a literal prefix that must not mask the error
        proc = _find_raw(rtl_trace, small_db, bad, regex=True)
        _assert_regex_rejected(proc, "--query (--regex)", bad, f"find --regex {bad!r}")
    # substring mode treats regex metacharacters literally
    _find_raw(rtl_trace, small_db, "(", regex=False, expect=2)

    # stacked quantifiers: not ECMAScript grammar, but libstdc++ accepts them and applies the later
    # quantifier to the earlier one (`q+*` == `(q+)*`). The literal prefilter must not treat the first
    # quantified atom as required; results must equal the explicitly grouped Python regex.
    small_names = _find_text_names(_find_raw(rtl_trace, small_db, ".", regex=True, limit=10**9, expect=0))
    for query, grouped in (
        ("q+*", r"(?:q+)*"),
        ("clk+*", r"cl(?:k+)*"),
        ("hit+?*", r"hi(?:t+?)*"),
        ("in_b+*us", r"in_(?:b+)*us"),
        ("zz+*top", r"z(?:z+)*top"),
    ):
        _find_check(rtl_trace, small_db, small_names, query, True, None,
                    lambda n, g=grouped: _re.search(g, n) is not None, f"stacked {query}")

    # --- large synthetic design (crosses the parallel-scan threshold) -----------------------
    big_sv = tmpdir / "find_fastpath_big.sv"
    big_sv.write_text(FIND_FASTPATH_BIG_SV)
    big_db = tmpdir / "find_fastpath_big.db"
    run_cmd([str(rtl_trace), "compile", "--db", str(big_db), "--single-unit", str(big_sv), "--top", "ff_top"])
    every = _find_raw(rtl_trace, big_db, ".", regex=True, limit=10**9, expect=0)
    all_names = _find_text_names(every)
    if len(all_names) < 40000:
        raise AssertionError(f"find_fastpath big design too small for parallel path: {len(all_names)}")
    if all_names != sorted(all_names):
        raise AssertionError("find_fastpath: find output must be sorted")

    cases = [
        # (label, query, regex, matcher)
        ("substr", "paddr", False, lambda n: "paddr" in n),
        ("substr-none", "no_such_sig", False, lambda n: "no_such_sig" in n),
        ("re-literal", "i_.*paddr", True, lambda n: _re.search("i_.*paddr", n)),
        ("re-literal-esc", "g_mid\\[1[0-9]\\]\\.u_mid.*paddr_q", True,
         lambda n: _re.search(r"g_mid\[1[0-9]\]\.u_mid.*paddr_q", n)),
        ("re-anchor-class", "^ff_top\\.g_mid\\[[0-3]\\]\\.u_mid\\.d\\[\\d+\\]$", True,
         lambda n: _re.search(r"^ff_top\.g_mid\[[0-3]\]\.u_mid\.d\[\d+\]$", n)),
        ("re-alt", "(r_a|r_b)$", True, lambda n: _re.search("(r_a|r_b)$", n)),
        ("re-alt-top", "wdata_i$|paddr_q$", True, lambda n: _re.search("wdata_i$|paddr_q$", n)),
        ("re-opt-literal", "r_ab?$", True, lambda n: _re.search("r_ab?$", n)),
        ("re-star-literal", "paddr_qx*$", True, lambda n: _re.search("paddr_qx*$", n)),
        ("re-plus-literal", "paddr_q+$", True, lambda n: _re.search("paddr_q+$", n)),
        ("re-brace0", "r_ab{0,1}$", True, lambda n: _re.search("r_ab{0,1}$", n)),
        ("re-dot-any", "d.ta", True, lambda n: _re.search("d.ta", n)),
        ("re-none", "zzzz.*qqqq", True, lambda n: _re.search("zzzz.*qqqq", n)),
        ("re-all", ".*", True, lambda n: True),
    ]
    for label, query, is_regex, matcher in cases:
        for limit in (1, 3, 1000, 10**9):
            _find_check(rtl_trace, big_db, all_names, query, is_regex, limit, matcher, f"{label}/limit={limit}")

    # suggestions on the big design: parallel bounded edit distance must equal a full sort
    def bounded_edit_distance(a, b, bound):
        if abs(len(a) - len(b)) > bound:
            return bound + 1
        prev = list(range(len(b) + 1))
        for i in range(1, len(a) + 1):
            cur = [i] + [0] * len(b)
            for j in range(1, len(b) + 1):
                cur[j] = min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] != b[j - 1]))
            if min(cur) > bound:
                return bound + 1
            prev = cur
        return prev[len(b)]

    needle = "ff_top.g_mid[3].u_mid.g_leaf[7].u_leaf.paddr_qq"
    proc = _find_raw(rtl_trace, big_db, needle, limit=5, expect=2)
    sugg = [l.strip() for l in proc.stdout.split("suggestions:\n", 1)[1].splitlines()]
    scored = sorted((d, n) for n in all_names for d in [bounded_edit_distance(n, needle, 3)] if d <= 3)
    if len(scored) < 5:
        raise AssertionError("find_fastpath: reference needs >= 5 names within distance 3")
    if [n for _, n in scored[:5]] != sugg:
        raise AssertionError(f"find_fastpath big suggestions differ: got {sugg}, want {[n for _, n in scored[:5]]}")
# ===== END find_fastpath tests =====


if __name__ == "__main__":
    main()
