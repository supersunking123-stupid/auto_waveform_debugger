#!/usr/bin/env python3
"""Logical multidimensional coordinate checks for the E4b USER DECISION prototype."""
import argparse
import json
import os
import subprocess
import tempfile
from pathlib import Path


def run(binary, arguments, expected=0, env=None):
    process = subprocess.run([str(binary), *map(str, arguments)], text=True, capture_output=True,
                             timeout=30, env=None if env is None else {**os.environ, **env})
    assert process.returncode == expected, (arguments, process.returncode, process.stdout, process.stderr)
    return process


def compile_db(binary, source, db, incremental=False, env=None):
    arguments = ['compile', '--db', db, '--single-unit', source, '--top', 'multidim_axes']
    if incremental:
        arguments.append('--incremental')
    return run(binary, arguments, env=env)


def trace(binary, db, signal, mode='loads', expected=0):
    process = run(binary, ['trace', '--db', db, '--mode', mode, '--signal', signal, '--format', 'json'],
                  expected=expected)
    return json.loads(process.stdout) if expected == 0 else process


def selected_labels(payload, labels):
    return {labels[e['line']] for e in payload['endpoints'] if e['line'] in labels}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--rtl-trace', type=Path, required=True)
    parser.add_argument('--source-dir', type=Path, required=True)
    parser.add_argument('--parent-bin', type=Path)
    args = parser.parse_args()
    binary = args.rtl_trace.resolve()
    parent = args.parent_bin.resolve() if args.parent_bin else None
    fixture = args.source_dir.resolve() / 'tests/fixtures/multidim_axes.sv'
    labels = {i: line.split('// CASE ', 1)[1].strip() for i, line in enumerate(fixture.read_text().splitlines(), 1)
              if '// CASE ' in line}
    with tempfile.TemporaryDirectory(prefix='rtl_trace_axes_') as directory:
        root = Path(directory)
        db = root / 'axes.db'
        compile_db(binary, fixture, db)
        cases = [
            ('p', 'p10', [(1, 1), (0, 0)], '[1][0]'),
            ('p', 'p11', [(1, 1), (1, 1)], '[1][1]'),
            ('p', 'p20', [(2, 2), (0, 0)], '[2][0]'),
            ('p', 'p21', [(2, 2), (1, 1)], '[2][1]'),
            ('p', 'row1', [(1, 1)], '[1]'),
            ('p', 'range321', [(3, 1)], '[3:1]'),
            ('p', 'range1121', [(1, 1), (2, 1)], '[1][2:1]'),
            ('p', 'parameter21', [(2, 2), (1, 1)], '[2][1]'),
            ('p', 'merged_rows12', [(2, 1)], '[2:1]'),
            ('p', 'nested_dynamic_outer0', [None, (0, 0)], None),
            ('p', 'dynamic_outer0', [None, (0, 0)], None),
            ('p', 'dynamic_inner1', [(1, 1), None], None),
            ('p', 'unknown_outer0', [None, (0, 0)], None),
            ('ascending_outer', 'ascending10', [(1, 1), (0, 0)], '[1][0]'),
            ('ascending_outer', 'ascending20', [(2, 2), (0, 0)], '[2][0]'),
            ('shifted', 'shifted64', [(6, 6), (4, 4)], '[6][4]'),
            ('shifted', 'shifted75', [(7, 7), (5, 5)], '[7][5]'),
            ('signed_outer', 'signed20', [(-2, -2), (0, 0)], '[-2][0]'),
            ('signed_outer', 'signed31', [(-3, -3), (1, 1)], '[-3][1]'),
            ('ascending_inner', 'indexed_up', [(1, 1), (1, 2)], '[1][1:2]'),
            ('ascending_inner', 'indexed_down', [(2, 2), (1, 2)], '[2][1:2]'),
            ('unpacked_desc', 'unpacked_desc14', [(1, 1), (4, 4)], '[1][4]'),
            ('unpacked_desc', 'unpacked_desc25', [(2, 2), (5, 5)], '[2][5]'),
            ('unpacked_asc', 'unpacked_asc14', [(1, 1), (4, 4)], '[1][4]'),
            ('unpacked_asc', 'unpacked_asc25', [(2, 2), (5, 5)], '[2][5]'),
            ('three_axes', 'three210', [(2, 2), (1, 1), (0, 0)], '[2][1][0]'),
            ('three_axes', 'three103', [(1, 1), (0, 0), (3, 3)], '[1][0][3]'),
            ('dynamic_values', 'dynamic_array0', [None, (0, 0)], None),
            ('queue_values', 'queue_last0', [None, (0, 0)], None),
        ]
        grouped = {}
        for signal, label, axes, bitmap in cases:
            grouped.setdefault(signal, []).append((label, axes, bitmap))
        for signal, members in grouped.items():
            payload = trace(binary, db, 'multidim_axes.' + signal)
            by_label = {labels[e['line']]: e for e in payload['endpoints'] if e['line'] in labels}
            for label, axes, bitmap in members:
                assert label in by_label, (signal, label, payload)
                endpoint = by_label[label]
                if bitmap is not None:
                    assert endpoint['bit_map'] == bitmap and not endpoint['bit_map_approximate'], endpoint
                else:
                    assert endpoint['bit_map_approximate'], endpoint
        queries = {
            'p': [[(1, 1), (0, 0)], [(1, 1), (1, 1)], [(2, 2), (0, 0)],
                  [(2, 2), (1, 1)], [(1, 2), (0, 0)]],
            'ascending_outer': [[(1, 1), (0, 0)], [(2, 2), (0, 0)]],
            'shifted': [[(6, 6), (4, 4)], [(7, 7), (5, 5)], [(6, 7), (4, 5)]],
            'signed_outer': [[(-2, -2), (0, 0)], [(-3, -3), (1, 1)], [(-3, -2), (0, 1)]],
            'ascending_inner': [[(1, 1), (1, 1)], [(1, 1), (2, 2)], [(1, 1), (3, 3)], [(2, 2), (2, 2)]],
            'unpacked_desc': [[(1, 1), (4, 4)], [(2, 2), (5, 5)]],
            'unpacked_asc': [[(1, 1), (4, 4)], [(2, 2), (5, 5)]],
            'three_axes': [[(2, 2), (1, 1), (0, 0)], [(1, 1), (0, 0), (3, 3)],
                           [(1, 2), (0, 1), (0, 0)], [(2, 2), (1, 1)]],
            'dynamic_values': [[(0, 0), (0, 0)], [(2, 2), (1, 1)]],
            'queue_values': [[(0, 0), (0, 0)], [(2, 2), (1, 1)]],
        }
        checked = 0
        for signal, selections in queries.items():
            own_labels = {label for label, _, _ in grouped[signal]}
            for selection in selections:
                suffix = ''.join(f'[{left}]' if left == right else f'[{left}:{right}]'
                                 for left, right in selection)
                expected = set()
                for label, coordinate, _ in grouped[signal]:
                    if all(endpoint is None or max(min(endpoint), min(query)) <= min(max(endpoint), max(query))
                           for endpoint, query in zip(coordinate, selection)):
                        expected.add(label)
                payload = trace(binary, db, 'multidim_axes.' + signal + suffix)
                got = selected_labels(payload, labels) & own_labels
                assert got == expected, (signal, selection, got, expected, payload)
                checked += 1
        for instance in (1, 2):
            payload = trace(binary, db, f'multidim_axes.u[{instance}].m[1][0]')
            assert 'instance_p10' in selected_labels(payload, labels), payload
        print(f'PASS: {checked} signed/ascending/packed/unpacked/range/dynamic axis selections and indexed instances')
        for suffix in ('[2147483648][0]', '[-2147483649][0]', '[1junk][0]', '[1][0]junk',
                       '[1][0', '[1][]', '[1:2:3][0]'):
            rejected = trace(binary, db, 'multidim_axes.p' + suffix, expected=1)
            assert 'Invalid --signal syntax:' in rejected.stderr and not rejected.stdout
        for signal in ('packet.member[1][0]', 'whole_packet.member[1][0]', 'associative_values[0][0]'):
            rejected = trace(binary, db, 'multidim_axes.' + signal, expected=1)
            assert 'remain unsupported' in rejected.stderr and json.loads(rejected.stdout)['diagnostics'][-1]['severity'] == 'error'
        print('PASS: overflow/malformed selects and unsupported struct/associative encodings reject clearly')
        names = json.loads(run(binary, ['find', '--db', db, '--query', 'bus[foo]', '--limit', '20', '--format', 'json']).stdout)['matches']
        assert len(names) == 2, names
        for name in names:
            for signal in (name, name + '[2]'):
                trace(binary, db, signal, mode='drivers')
        print('PASS: escaped SV identifiers resolve before bracket syntax validation')
        compact_source = root / 'compact_axes.sv'
        compact_source.write_text("""
module compact_leaf(input logic[3:0][3:0] global_clk, output logic y);
  assign y = global_clk[1][0];
endmodule
module multidim_axes(input logic[3:0][3:0] global_clk, output logic y[0:1024]);
  for(genvar i=0;i<1025;i++) begin:g
    compact_leaf u(.global_clk(global_clk),.y(y[i]));
  end
endmodule
""")
        compact_db = root / 'compact_axes.db'
        compile_db(binary, compact_source, compact_db)
        rejected = trace(binary, compact_db, 'multidim_axes.global_clk[1][0]', expected=1)
        assert 'compact global nets remain unsupported' in rejected.stderr and json.loads(rejected.stdout)['diagnostics'][-1]['code'] == 'unsupported_compact_global_axes'
        print('PASS: compact global multi-axis queries reject before fast-path output')

        # Slang rejects selectors chained after a range before tracing begins. Keep
        # that diagnostic contract, rather than weakening production elaboration.
        invalid = root / 'range_chain.sv'
        invalid.write_text('module multidim_axes(input logic[3:0][3:0] p, output logic y); assign y=p[3:1][2][0]; endmodule\n')
        failure = run(binary, ['compile', '--db', root / 'range_chain.db', '--single-unit', invalid, '--top', 'multidim_axes'], expected=1)
        assert 'cannot chain select expressions after a range select' in failure.stderr
        invalid.write_text("module multidim_axes(input logic[3:0][3:0] p, output logic y); assign y=p[2'bxx][0]; endmodule\n")
        failure = run(binary, ['compile', '--db', root / 'unknown_literal.db', '--single-unit', invalid, '--top', 'multidim_axes'], expected=1)
        assert 'index-oob' in failure.stderr
        print('PASS: range-chain and literal-X compiler diagnostics remain unchanged')

        off_db = root / 'canonical_off.db'
        compile_db(binary, fixture, off_db, env={'RTL_TRACE_CANONICAL_BODIES': '0'})
        assert db.read_bytes() == off_db.read_bytes()
        assert Path(str(db) + '.meta').read_bytes() == Path(str(off_db) + '.meta').read_bytes()
        verified = compile_db(binary, fixture, root / 'verified.db',
                              env={'RTL_TRACE_CANONICAL_VERIFY': '1'})
        assert 'mismatched_lists=0' in verified.stdout, verified.stdout
        print('PASS: canonical on/off DB identity and VERIFY zero mismatches')
        if parent:
            from coordinate_diagnostics_regression import footer, original_fields
            import struct
            old_db = root / 'old.db'
            compile_db(parent, fixture, old_db)
            for signal in ('multidim_axes.p', 'multidim_axes.shifted', 'multidim_axes.unpacked_asc'):
                old = run(parent, ['trace', '--db', old_db, '--mode', 'loads', '--signal', signal, '--format', 'json'])
                candidate = run(binary, ['trace', '--db', old_db, '--mode', 'loads', '--signal', signal, '--format', 'json'])
                assert json.loads(old.stdout) == original_fields(json.loads(candidate.stdout)), signal
                assert old.stderr == candidate.stderr
            for mode in ('drivers', 'loads'):
                for suffix in ('[0]', '[1][0]'):
                    rejected = trace(binary, old_db, 'multidim_axes.p'+suffix, mode=mode, expected=1)
                    assert 'Rebuild the DB' in rejected.stderr
                    assert json.loads(rejected.stdout)['diagnostics'][-1]['code'] == 'legacy_multidimensional_select'
            rebuilt = compile_db(binary, fixture, old_db, incremental=True)
            assert 'cache hit' not in (rebuilt.stdout + rebuilt.stderr).lower()
            assert old_db.read_bytes() == db.read_bytes()
            hit = compile_db(binary, fixture, old_db, incremental=True)
            assert 'cache hit' in (hit.stdout + hit.stderr).lower()
            print('PASS: real legacy multidimensional single/multiple selects refuse; parent incremental rebuild then fresh hit')
            scalar_source = root / 'vector_only.sv'
            scalar_source.write_text('module vector_only(input logic[7:0] a, output logic[3:0] y); assign y={a[0],a[1],a[2],a[3]}; endmodule\n')
            vector_dbs = []
            for label, compiler in (('parent', parent), ('candidate', binary)):
                vector_db = root / (label + '_vector.db')
                run(compiler, ['compile', '--db', vector_db, '--single-unit', scalar_source, '--top', 'vector_only'])
                vector_dbs.append(vector_db)
            # Version6 is an intentional format change, even for vectors. The
            # prior body is identical in the epoch6 B-only feature snapshot.
            current_meta=Path(str(vector_dbs[1])+'.meta').read_text()
            if 'SEMANTICS_EPOCH:6\n' in current_meta:
                prefix=footer(vector_dbs[1].read_bytes())[0]
                old_layout=bytearray(vector_dbs[1].read_bytes()[:prefix])
                struct.pack_into('<II',old_layout,16,5,0)
                assert bytes(old_layout)==vector_dbs[0].read_bytes()
            for query in ('vector_only.a', 'vector_only.a[2]'):
                old=run(parent,['trace','--db',vector_dbs[0],'--mode','loads','--signal',query,'--format','json'])
                compatible=run(binary,['trace','--db',vector_dbs[0],'--mode','loads','--signal',query,'--format','json'])
                assert json.loads(old.stdout)==original_fields(json.loads(compatible.stdout))
                if '[' in query: assert 'legacy_dimensions_unverified' in compatible.stderr
            print('PASS: one-dimensional legacy query fields unchanged; coordinate metadata and warning explicitly checked')


if __name__ == '__main__':
    main()
