#!/usr/bin/env python3
"""Member identity beats approximate sibling axes; selects stay in parent bits."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def run(binary, args, expected=0, env=None):
    result = subprocess.run([str(binary), *map(str, args)], capture_output=True,
                            text=True, timeout=40, env={**os.environ, **(env or {})})
    assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
    return result


def compile_db(binary, source, db, env=None):
    return run(binary, ['compile', '--db', db, '--single-unit', source, '--top', 'member_loads'], env=env)


def trace(binary, db, name, mode, expected=0):
    return json.loads(run(binary, ['trace', '--db', db, '--mode', mode, '--signal',
                                  'member_loads.' + name, '--format', 'json'], expected).stdout)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rtl-trace', type=Path, required=True)
    ap.add_argument('--source-dir', type=Path, required=True)
    ap.add_argument('--parent-bin', type=Path)
    ap.add_argument('--baseline-bin', type=Path)
    args = ap.parse_args()
    binary = args.rtl_trace.resolve()
    source = args.source_dir.resolve() / 'tests/fixtures/member_loads.sv'
    with tempfile.TemporaryDirectory(prefix='rtl_member_loads_') as directory:
        root = Path(directory)
        db = root/'fresh.db'
        compile_db(binary, source, db)
        original = db.read_bytes()
        for mode in ('drivers', 'loads'):
            for member in ('packet.vector', 'packet.vector[2]', 'packet[2]'):
                endpoints = trace(binary, db, member, mode)['endpoints']
                assert len(endpoints) == 1 and endpoints[0]['path'] == 'member_loads.packet.vector', (member, mode, endpoints)
            matrix = trace(binary, db, 'packet.matrix', mode)['endpoints']
            assert len(matrix) == 1 and matrix[0]['path'] == 'member_loads.packet.matrix', (mode, matrix)
            whole = trace(binary, db, 'packet', mode)['endpoints']
            assert {e['path'] for e in whole} == {'member_loads.packet.matrix', 'member_loads.packet.vector'}, (mode, whole)
            high = trace(binary, db, 'reversed_packet.vector[2]', mode)['endpoints']
            assert len(high) == 1 and high[0]['path'] == 'member_loads.reversed_packet.vector', (mode, high)
            for name in ('packet.vector[9]', 'reversed_packet.vector[9]'):
                body = trace(binary, db, name, mode, 1)
                assert not body['endpoints'] and body['diagnostics'][-1]['code'] == 'axis_out_of_bounds', body
            # Existing multidimensional member limit stays explicit.
            body = trace(binary, db, 'packet.matrix[2][1]', mode, 1)
            assert body['diagnostics'][-1]['code'] == 'unsupported_struct_member_axes', body
            assert not body['endpoints']
            trace(binary, db, 'unpacked_packet', mode)
            missing = run(binary, ['trace', '--db', db, '--mode', mode, '--signal',
                                  'member_loads.unpacked_packet.vector', '--format', 'json'], 2)
            assert 'Signal not found' in missing.stderr
            unpacked = trace(binary, db, 'unpacked_matrix[2][1]', mode)['endpoints']
            assert all(e['bit_map'] in ('[2][1]', '[?idx][0]') for e in unpacked), unpacked
            assert any(e['bit_map'] == '[2][1]' for e in unpacked), unpacked
            oob = trace(binary, db, 'unpacked_matrix[4][1]', mode, 1)
            assert oob['diagnostics'][-1]['code'] == 'axis_out_of_bounds', oob
            nested = trace(binary, db, 'nested_packet[13]', mode)['endpoints']
            assert len(nested) == 1 and nested[0]['path'] == 'member_loads.nested_packet.a.a', (mode, nested)
            for name in ('nested_packet[1]', 'nested_packet.b', 'nested_packet.b[1]'):
                endpoints = trace(binary, db, name, mode)['endpoints']
                assert len(endpoints) == 1 and endpoints[0]['path'] == 'member_loads.nested_packet.b', (name, mode, endpoints)
            for name, path in (('nested_packet.a', 'nested_packet.a.a'),
                               ('nested_packet.a.a', 'nested_packet.a.a'),
                               ('nested_packet.a.b[1]', 'nested_packet.a.b')):
                endpoints = trace(binary, db, name, mode)['endpoints']
                assert 'member_loads.'+path in {e['path'] for e in endpoints}, (name, mode, endpoints)
                assert all(e['path'] != 'member_loads.nested_packet.b' for e in endpoints), endpoints
            unsupported = trace(binary, db, 'nested_packet.a.a[0][1]', mode, 1)
            assert unsupported['diagnostics'][-1]['code'] == 'unsupported_struct_member_axes', unsupported
        # A whole-struct driver still reaches each member. Filtering only known
        # sibling member identity must not remove whole-parent assignments.
        driver = trace(binary, db, 'whole.vector[2]', 'drivers')['endpoints']
        assert len(driver) == 1 and driver[0]['path'] == 'member_loads.whole', driver
        loads = trace(binary, db, 'whole.vector', 'loads')['endpoints']
        assert len(loads) == 1 and loads[0]['path'] == 'member_loads.whole.vector', loads
        print('PASS: member loads/drivers isolate siblings, local selects translate offsets, OOB errors and whole-struct drivers survive')
        print('PASS: packed multidimensional member queries keep explicit limits; unpacked roots filter axes and unpacked struct members remain explicitly missing')
        print('PASS: nested reused field names retain root bit 13 and inner members; verified outer sibling bounds stay isolated')
        if args.baseline_bin:
            old = root/'baseline.db'
            compile_db(args.baseline_bin.resolve(), source, old)
            assert old.read_bytes() == original, 'query-only fix changed DB bytes'
            assert Path(str(old)+'.meta').read_bytes() == Path(str(db)+'.meta').read_bytes()
            assert len(trace(args.baseline_bin.resolve(), old, 'packet.vector', 'loads')['endpoints']) == 2
            for mode in ('drivers', 'loads'):
                for name in ('nested_packet[13]', 'nested_packet.a', 'nested_packet.a.a'):
                    baseline = trace(args.baseline_bin.resolve(), old, name, mode)['endpoints']
                    candidate = trace(binary, db, name, mode)['endpoints']
                    assert [e['path'] for e in candidate] == [e['path'] for e in baseline], (name, mode, baseline, candidate)
                baseline = trace(args.baseline_bin.resolve(), old, 'nested_packet.a.b[1]', mode)['endpoints']
                candidate = trace(binary, db, 'nested_packet.a.b[1]', mode)['endpoints']
                assert {e['path'] for e in baseline} <= {e['path'] for e in candidate}, (mode, baseline, candidate)
            old.unlink()
            print('PASS: frozen pre-fix reproduces leak; fresh DB and fingerprint bytes remain identical')
        if args.parent_bin:
            old = root/'parent.db'
            compile_db(args.parent_bin.resolve(), source, old)
            for mode in ('drivers', 'loads'):
                for name in ('packet.vector', 'packet.vector[2]', 'packet[2]', 'packet.matrix'):
                    parent = trace(args.parent_bin.resolve(), old, name, mode)['endpoints']
                    new = trace(binary, db, name, mode)['endpoints']
                    assert [e['path'] for e in parent] == [e['path'] for e in new], (name, mode, parent, new)
                for name, path in (('nested_packet[13]', 'nested_packet.a.a'),
                                   ('nested_packet.a.b[1]', 'nested_packet.a.b')):
                    parent = trace(args.parent_bin.resolve(), old, name, mode)['endpoints']
                    new = trace(binary, db, name, mode)['endpoints']
                    if name == 'nested_packet[13]':
                        assert 'member_loads.'+path in {e['path'] for e in parent}, (name, mode, parent)
                    # Main does not translate the inner scalar member select.
                    # Preserve its conservative endpoints and include the real
                    # selected field with the current local-offset translation.
                    assert {e['path'] for e in parent} <= {e['path'] for e in new}, (name, mode, parent, new)
                    assert 'member_loads.'+path in {e['path'] for e in new}, (name, mode, new)
            old.unlink()
            print('PASS: main member, sub-select and parent-struct select endpoints match in both modes')
        off = root/'off.db'
        compile_db(binary, source, off, {'RTL_TRACE_CANONICAL_BODIES': '0'})
        assert off.read_bytes() == original
        off.unlink()
        verify = compile_db(binary, source, root/'verify.db', {'RTL_TRACE_CANONICAL_VERIFY': '1'})
        assert 'mismatched_lists=0' in verify.stdout
        print('PASS: canonical on/off DB bytes and VERIFY lists match')


if __name__ == '__main__':
    main()
