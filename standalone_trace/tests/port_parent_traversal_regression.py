"""Actual parent traversal for input drivers and output loads without local uses."""
import argparse
import hashlib
import json
import re
import subprocess
import tempfile
from pathlib import Path
from endpoint_dedup_regression import read_db, run


def source_valid_input_boundary(source, owner, driver, signal, decoded, stored, payload):
    """Exact active declarations and literal eight-bit parent connections only."""
    known = {'u_direct0': 'x0', 'u_direct1': 'x1',
             'u_nested0.u_leaf': 'x0', 'u_nested1.u_leaf': 'x1'}
    assert known.get(owner) == driver, ('unknown Input addition', owner, driver)
    assert signal == 'port_parent_top.' + owner + '.d'
    assert hashlib.sha256(source.read_bytes()).hexdigest() == \
        '7f25f681bd34642f084a4668abfa12ff2d7422a1168ebdca6a3c295d244b47f6'
    # The pinned declaration at line 2 precedes if(EN). Each listed instance
    # uses default EN=0; that excludes the consumer, not the Input declaration.
    # Nested u_leaf.d -> wrapper.d -> x0/x1 are full-width identity connections.
    pairs = [(bit, ('port_parent_top.' + driver, bit)) for bit in range(8)]
    assert len(pairs) == 8 and {bit for bit, _ in pairs} == set(range(8))
    assert payload['declared_axes'] == [dict(left=7, right=0, fixed=True, packed=True)]
    assert len(stored) == 1, (signal, stored)
    entry = stored[0]
    assert decoded['strings'][entry[0]] == signal
    assert Path(decoded['strings'][entry[1]]).resolve() == source
    assert decoded['strings'][entry[2]] == 'input'
    assert decoded['strings'][entry[3]] == 'Q1;0:7|', (signal, decoded['strings'][entry[3]])
    assert entry[4:] == (2, 0, 0, 1, 0, 0, 4, (), ()), (signal, entry)
    ports = [endpoint for endpoint in payload['endpoints'] if endpoint['path'] == signal]
    assert len(ports) == 1, (signal, ports)
    expected = dict(kind='port', path=signal, file=ports[0]['file'], line=2,
                    direction='input', bit_map='', bit_map_encoding='unrestricted',
                    bit_map_approximate=False, assignment='', lhs=[], rhs=[])
    assert ports[0] == expected and Path(ports[0]['file']).resolve() == source
    return pairs, expected


def compile_db(binary, source, db, env=None, incremental=False):
    return run(binary, ['compile', '--db', db, '--single-unit', source,
                        '--top', 'port_parent_top', *(['--incremental'] if incremental else [])], env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rtl-trace', type=Path, required=True)
    parser.add_argument('--source-dir', type=Path, required=True)
    args = parser.parse_args()
    source = args.source_dir.resolve() / 'tests/fixtures/canonical_bodies/inactive_port_traversal.sv'
    binary = args.rtl_trace.resolve()
    labels = {line.split('// CASE ', 1)[1].strip(): number
              for number, line in enumerate(source.read_text().splitlines(), 1) if '// CASE ' in line}
    owners = [('u_direct0', 'x0', 'r0'), ('u_direct1', 'x1', 'r1'),
              ('u_nested0.u_leaf', 'x0', 'r2'), ('u_nested1.u_leaf', 'x1', 'r3')]
    with tempfile.TemporaryDirectory(prefix='rtl_parent_ports_') as directory:
        root = Path(directory)
        db = root/'canonical.db'
        result = compile_db(binary, source, db, {'RTL_TRACE_CANONICAL_STATS': '1'})
        redirected = re.search(r'\[Canon\] tracer signals .*redirected=(\d+)', result.stdout)
        assert redirected and int(redirected.group(1)) > 0, result.stdout
        decoded = read_db(db)
        for owner, driver, load in owners:
            for suffix, mode, path, label in [('d', 'drivers', driver, 'drive_'+driver),
                                               ('q', 'loads', load, 'use_'+load)]:
                signal = 'port_parent_top.'+owner+'.'+suffix
                # Check the stored directional list, not only a query that might
                # recover the route through reverse refs from another signal.
                entries = decoded['lists'][signal][0 if mode == 'drivers' else 1]
                boundary = [entry for entry in entries if decoded['strings'][entry[0]] == signal]
                writers = [entry for entry in entries if entry not in boundary]
                if mode != 'drivers':
                    assert not boundary, (signal, mode, boundary)
                assert len(writers) == 1, (signal, mode, entries)
                entry = writers[0]
                assert decoded['strings'][entry[0]] == 'port_parent_top.'+path, (signal, entry)
                assert entry[4] == labels[label] and entry[7] == 0 and entry[9] == 1, (signal, entry)
                payload = json.loads(run(binary, ['trace', '--db', db, '--signal', signal,
                    '--mode', mode, '--format', 'json']).stdout)
                public_writers = [e for e in payload['endpoints'] if e['path'] != signal]
                assert len(public_writers) == 1 and public_writers[0]['kind'] == 'expr', payload
                assert public_writers[0]['line'] == labels[label], payload
                assert public_writers[0]['assignment'], payload
                if mode == 'drivers':
                    pairs, expected = source_valid_input_boundary(
                        source, owner, driver, signal, decoded, boundary, payload)
                    for selected in (0, 7):
                        assert dict(pairs)[selected] == ('port_parent_top.' + driver, selected)
                        selected_payload = json.loads(run(binary, ['trace', '--db', db,
                            '--signal', signal + f'[{selected}]', '--mode', mode,
                            '--format', 'json']).stdout)
                        assert [e for e in selected_payload['endpoints'] if e['path'] == signal] == [expected]
                    outside = subprocess.run([str(binary), 'trace', '--db', str(db),
                        '--signal', signal + '[8]', '--mode', mode, '--format', 'json'],
                        capture_output=True, text=True, timeout=45)
                    assert outside.returncode == 1
                    outside_payload = json.loads(outside.stdout)
                    assert not outside_payload['endpoints'] and \
                        outside_payload['diagnostics'][0]['code'] == 'axis_out_of_bounds'
                assert not any(stop['reason'] in ('depth_limit', 'node_limit') for stop in payload['stops']), payload
        for field, mode, label in [('d', 'drivers', 'drive_x0'), ('q', 'loads', 'use_r4')]:
            signal = 'port_parent_top.u_active.'+field
            entries = decoded['lists'][signal][0 if mode == 'drivers' else 1]
            assert len(entries) == 1 and entries[0][7] == 1, (signal, entries)
            payload = json.loads(run(binary, ['trace', '--db', db, '--signal', signal,
                '--mode', mode, '--format', 'json']).stdout)
            assert len(payload['endpoints']) == 1 and payload['endpoints'][0]['line'] == labels[label], payload
        for owner in ('u_direct0', 'u_direct1', 'u_nested0.u_leaf', 'u_nested1.u_leaf'):
            for field, mode in [('d', 1), ('q', 0)]:
                assert not decoded['lists']['port_parent_top.'+owner+'.'+field][mode], owner
        for owner in ('u_open', 'u_constant'):
            for field, mode in [('d', 0), ('q', 1)]:
                entries = decoded['lists']['port_parent_top.'+owner+'.'+field][mode]
                assert len(entries) == 1 and entries[0][7] == 1, (owner, field, entries)
        print('PASS: stored direct/nested input drivers and output loads reach actual parents; inactive local traces stay empty')

        off = root/'baseline.db'
        compile_db(binary, source, off, {'RTL_TRACE_CANONICAL_BODIES': '0'})
        assert off.read_bytes() == db.read_bytes(), 'canonical/baseline bytes differ'
        assert Path(str(off)+'.meta').read_bytes() == Path(str(db)+'.meta').read_bytes()
        verified = compile_db(binary, source, root/'verify.db', {'RTL_TRACE_CANONICAL_VERIFY': '1'})
        assert 'mismatched_lists=0' in verified.stdout, verified.stdout
        print('PASS: distinct parent connections across canonical duplicate instances; canonical on/off bytes and VERIFY match')

        meta = Path(str(db)+'.meta')
        assert 'SEMANTICS_EPOCH:20\n' in meta.read_text()
        meta.write_text(meta.read_text().replace('SEMANTICS_EPOCH:20\n', 'SEMANTICS_EPOCH:19\n'))
        rebuilt = compile_db(binary, source, db, incremental=True)
        assert 'incremental-cache-hit' not in rebuilt.stdout
        assert db.read_bytes() == off.read_bytes()
        assert 'SEMANTICS_EPOCH:20\n' in meta.read_text()
        hit = compile_db(binary, source, db, incremental=True)
        assert 'incremental-cache-hit' in hit.stdout
        print('PASS: epoch 19 fingerprint forces epoch 20 rebuild, then cache hit')

        # Inout boundaries must reach both the child and parent writer.
        inout_source = root/'inout_parent.sv'
        inout_source.write_text('''module leafio(inout wire [3:0] b, input logic en, input logic [3:0] o);
  assign b = en ? o : 'z;
endmodule
module misc(input logic en, input logic [3:0] o, i0, output wire [3:0] io);
  leafio u_io(.b(io), .en(en), .o(o));
  assign io = !en ? i0 : 'z;
endmodule
''')
        inout_db = root/'inout.db'
        run(binary, ['compile', '--db', inout_db, '--single-unit', inout_source, '--top', 'misc'])
        for signal in ('misc.u_io.b', 'misc.u_io.b[1]', 'misc.io', 'misc.io[1]'):
            payload = json.loads(run(binary, ['trace', '--db', inout_db, '--signal', signal,
                '--mode', 'drivers', '--format', 'json']).stdout)
            endpoints = payload['endpoints']
            assert sorted(e['line'] for e in endpoints) == [2, 6], (signal, payload)
            assert all(e['kind'] == 'expr' and not e['bit_map_approximate'] for e in endpoints), (signal, payload)
            assert {e['assignment'].strip() for e in endpoints} == {
                "b = en ? o : 'z", "io = !en ? i0 : 'z"}, (signal, payload)
        inout_off = root/'inout_off.db'
        run(binary, ['compile', '--db', inout_off, '--single-unit', inout_source, '--top', 'misc'],
            {'RTL_TRACE_CANONICAL_BODIES': '0'})
        assert inout_off.read_bytes() == inout_db.read_bytes()
        inout_verify = run(binary, ['compile', '--db', root/'inout_verify.db', '--single-unit',
            inout_source, '--top', 'misc'], {'RTL_TRACE_CANONICAL_VERIFY': '1'})
        assert 'mismatched_lists=0' in inout_verify.stdout, inout_verify.stdout
        inout_verified = re.search(r'\[Canon\] verify signals=(\d+)', inout_verify.stdout)
        assert inout_verified and int(inout_verified.group(1)) > 0, inout_verify.stdout
        print('PASS: inout child and parent queries each return both exact writers once')


if __name__ == '__main__':
    main()
