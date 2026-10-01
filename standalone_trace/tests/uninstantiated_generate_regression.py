"""Inactive generate endpoint/reference removal and actual-instance audit witnesses."""
import argparse
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
from endpoint_dedup_regression import read_db


def run(binary, args, env=None, expected=0):
    p = subprocess.run([str(binary), *map(str, args)], env={**os.environ, **(env or {})},
                       capture_output=True, text=True, timeout=60)
    assert p.returncode == expected, (args, p.returncode, p.stdout, p.stderr)
    return p


def compile_db(binary, source, db, env=None, incremental=False):
    return run(binary, ['compile', '--db', db, '--single-unit', source, '--top', 'inactive_top',
                        *(['--incremental'] if incremental else [])], env)


def trace(binary, db, signal, mode):
    return json.loads(run(binary, ['trace', '--db', db, '--mode', mode,
                                  '--signal', signal, '--format', 'json']).stdout)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rtl-trace', type=Path, required=True)
    ap.add_argument('--source-dir', type=Path, required=True)
    ap.add_argument('--parent-bin', type=Path)
    args = ap.parse_args()
    binary = args.rtl_trace.resolve()
    source = args.source_dir.resolve() / 'tests/fixtures/canonical_bodies/uninstantiated_generate.sv'
    labels = {line.split('// CASE ', 1)[1].strip(): i for i, line in enumerate(source.read_text().splitlines(), 1)
              if '// CASE ' in line}
    owners = ['inactive_top.u_on0', 'inactive_top.u_on1', 'inactive_top.u_off0', 'inactive_top.u_off1']
    with tempfile.TemporaryDirectory(prefix='rtl_inactive_generate_') as directory:
        root = Path(directory)
        db = root / 'on.db'
        manifest = root / 'scopes.jsonl'
        result = compile_db(binary, source, db, {'RTL_TRACE_CANONICAL_STATS': '1',
                                                'RTL_TRACE_INACTIVE_SCOPES': str(manifest)})
        redirect = re.search(r'\[Canon\] tracer signals .*redirected=(\d+)', result.stdout)
        assert redirect and int(redirect.group(1)) > 0, result.stdout
        on = read_db(db)
        records = [json.loads(line) for line in manifest.read_text().splitlines()]
        assert records[0]['schema'] == 1 and records[0]['semantics_epoch'] == 9
        assert records[-1]['write_ok'] is True and records[-1]['visited_actual_instances'] == 7
        scopes = [r for r in records if r['record'] == 'inactive_scope']
        assert len(scopes) == records[-1]['inactive_scopes']
        assert {r['owner_instance'] for r in scopes} == set(owners)
        for owner in owners:
            selected = [r for r in scopes if r['owner_instance'] == owner]
            assert any(r['scope_path'].endswith('.g_same_line') for r in selected)
            assert any(r['scope_path'].endswith('.g_always_off') for r in selected)
            assert not any('.g_always_off.g_inner_looks_active' in r['scope_path'] for r in selected)
            for r in selected:
                assert r['scope_path'].startswith(owner+'.')
                assert r['same_original_buffer'] and r['original_span']['namespace_unambiguous'], r
                assert r['original_span']['file'] == str(source)
            same = next(r for r in selected if r['scope_path'].endswith('.g_same_line'))
            assert same['logical_range_exclusive'] is False
            span = same['original_span']
            text = source.read_text()[span['begin']:span['end']]
            assert 'q[1] = a[1]' in text and 'q[0] = a[0]' not in text and 'q[2] = a[2]' not in text, text

        for owner in owners:
            enabled = '.u_on' in owner
            absent = ['case_default', 'nested_inner_off', 'nested_outer_off',
                      'case_zero' if enabled else 'case_one',
                      'disabled_class_active' if enabled else 'outer_on',
                      'outer_off' if enabled else 'enabled_child']
            for signal, mode in ((owner+'.q', 'drivers'), (owner+'.a', 'loads')):
                endpoints = trace(binary, db, signal, mode)['endpoints']
                lines = {e['line'] for e in endpoints}
                assert not lines.intersection(labels[label] for label in absent), (signal, mode, endpoints)
                assert labels['procedural_dead_arm_retained'] in lines
                same_line = [e for e in endpoints if e['line'] == labels['same_line']]
                assert {e['bit_map'] for e in same_line} == {'[0]', '[2]'}, same_line
                loop = [e for e in endpoints if e['line'] == labels['loop']]
                assert {e['bit_map'] for e in loop} == ({'[6:5]'} if enabled else set()), (signal, loop)
                active = ['case_one', 'outer_on'] if enabled else ['case_zero', 'outer_off', 'disabled_class_active']
                assert all(labels[label] in lines for label in active), (signal, mode, endpoints)
        for name in on['strings']:
            assert not any(part in name for part in ('.g_always_off.', '.g_same_line.')), name
        for owner in owners[2:]:
            assert not any(name.startswith(owner+'.g_enabled.') for name in on['lists']), owner
        for refs in on['reverse']:
            assert not any('.g_always_off.' in a or '.g_always_off.' in b for a, b in refs)
        print('PASS: disabled if/case/zero-trip/nested/unnamed branches drop both endpoint directions; active siblings/procedural arms survive')

        off = root/'off.db'
        compile_db(binary, source, off, {'RTL_TRACE_CANONICAL_BODIES':'0'})
        assert off.read_bytes() == db.read_bytes()
        assert Path(str(off)+'.meta').read_bytes() == Path(str(db)+'.meta').read_bytes()
        verified = compile_db(binary, source, root/'verify.db', {'RTL_TRACE_CANONICAL_VERIFY':'1'})
        assert 'mismatched_lists=0' in verified.stdout
        assert 'SEMANTICS_EPOCH:9\n' in Path(str(db)+'.meta').read_text()
        print('PASS: two copies of each parameter class redirect canonically; canonical on/off bytes and VERIFY lists match')

        if args.parent_bin:
            inherited = root/'parent.db'
            compile_db(args.parent_bin.resolve(), source, inherited)
            parent = read_db(inherited)
            assert 'SEMANTICS_EPOCH:8\n' in Path(str(inherited)+'.meta').read_text()
            assert len(parent['endpoints']) > len(on['endpoints'])
            assert len(parent['raw']) > len(on['raw'])
            rebuilt = compile_db(binary, source, inherited, incremental=True)
            assert 'incremental-cache-hit' not in rebuilt.stdout
            assert inherited.read_bytes() == db.read_bytes()
            hit = compile_db(binary, source, inherited, incremental=True)
            assert 'incremental-cache-hit' in hit.stdout
            print('PASS: real epoch 8 parent has phantom endpoints; epoch 9 forces rebuild then cache hit')
        else:
            print('SKIP: actual epoch 8 parent rebuild comparison (--parent-bin missing)')
        manifest.unlink()
        audited = compile_db(binary, source, db, {'RTL_TRACE_INACTIVE_SCOPES':str(manifest)}, incremental=True)
        assert 'incremental-cache-hit' not in audited.stdout and manifest.exists()
        assert db.read_bytes() == on['raw']
        bad = root/'missing'/'audit.jsonl'
        run(binary, ['compile', '--db', root/'failed.db', '--single-unit', source, '--top', 'inactive_top'],
            {'RTL_TRACE_INACTIVE_SCOPES':str(bad)}, expected=1)
        assert not (root/'failed.db').exists()
        assert not list(root.glob('.rtl_trace_stage_*'))
        print('PASS: audit binds actual owners/original offset namespaces, rejects same-line range claims, bypasses cache and fails visibly on write error')
        alias_include = root/'alias_include.sv'
        alias_include.write_text('`line 10 "shared_alias.vp" 0\nmodule unused_alias; endmodule\n')
        aliased = root/'aliased.sv'
        aliased.write_text('`include "'+str(alias_include)+'"\n`line 10 "shared_alias.vp" 0\n'
                           'module inactive_top(input logic a, output logic q);\n'
                           'if (0) begin:g_off\nassign q=a;\nend\nendmodule\n')
        alias_manifest = root/'alias.jsonl'
        compile_db(binary, aliased, root/'alias.db', {'RTL_TRACE_INACTIVE_SCOPES':str(alias_manifest)})
        alias_rows = [json.loads(line) for line in alias_manifest.read_text().splitlines()]
        row = next(r for r in alias_rows if r['record']=='inactive_scope')
        assert row['original_span']['namespace_unambiguous'] is False
        assert row['logical_range_exclusive'] is False
        macro = root/'macro.sv'
        macro.write_text('`define OFF_BLOCK begin:g_macro assign q=a; end\n'
                         'module inactive_top(input logic a, output logic q);\n'
                         'if (0) `OFF_BLOCK\nendmodule\n')
        macro_manifest = root/'macro.jsonl'
        compile_db(binary, macro, root/'macro.db', {'RTL_TRACE_INACTIVE_SCOPES':str(macro_manifest)})
        macro_rows = [json.loads(line) for line in macro_manifest.read_text().splitlines()]
        row = next(r for r in macro_rows if r['record']=='inactive_scope')
        assert row['macro_boundary'] is True and row['original_span'] is None
        assert row['logical_range_exclusive'] is False
        print('PASS: aliased logical filenames and macro invocation boundaries cannot claim an offset/line witness')
        fifo = root/'audit_fifo'
        os.mkfifo(fifo)
        run(binary, ['compile', '--db', root/'fifo.db', '--single-unit', source, '--top', 'inactive_top'],
            {'RTL_TRACE_INACTIVE_SCOPES':str(fifo)}, expected=1)
        assert not (root/'fifo.db').exists()
        print('PASS: requested audit FIFO fails fast instead of blocking')
        interior = root/'interior_macro.sv'
        interior.write_text('module inactive_top(input logic a, output logic q);\n'
                            'if (0) begin:g_off\n'
                            '`define A_ASSIGN assign q = a;\n'
                            '`A_ASSIGN\nend\n'
                            '`line 4 "'+str(interior)+'" 0\n'
                            '`A_ASSIGN\nendmodule\n')
        interior_manifest = root/'interior_macro.jsonl'
        interior_db = root/'interior_macro.db'
        compile_db(binary, interior, interior_db, {'RTL_TRACE_INACTIVE_SCOPES':str(interior_manifest)})
        rows = [json.loads(line) for line in interior_manifest.read_text().splitlines()]
        row = next(r for r in rows if r['record']=='inactive_scope')
        assert row['contains_macro_definition'] is True and row['macro_boundary'] is False
        assert row['original_span'] is None and row['logical_range_exclusive'] is False
        for signal, mode in [('inactive_top.q','drivers'),('inactive_top.a','loads')]:
            payload = trace(binary, interior_db, signal, mode)
            assert any(e['kind']=='expr' and e['assignment']=='q = a' for e in payload['endpoints']), payload
        # A repeated line alias without macros also cannot claim exclusive lines.
        repeated = root/'repeated_alias.sv'
        repeated.write_text('`line 10 "repeated.vp" 0\n'
                            'module inactive_top(input logic a, output logic q);\n'
                            'if (0) begin:g_off\nassign q=a;\nend\n'
                            '`line 12 "repeated.vp" 0\nassign q=a;\nendmodule\n')
        repeated_manifest = root/'repeated.jsonl'
        compile_db(binary, repeated, root/'repeated.db', {'RTL_TRACE_INACTIVE_SCOPES':str(repeated_manifest)})
        rows = [json.loads(line) for line in repeated_manifest.read_text().splitlines()]
        row = next(r for r in rows if r['record']=='inactive_scope')
        assert row['original_span'] is not None and row['logical_range_exclusive'] is False
        print('PASS: active macro invocation survives; interior definitions and repeated line aliases cannot claim inactive witnesses')





if __name__ == '__main__':
    main()
