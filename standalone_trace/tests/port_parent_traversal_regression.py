"""Actual parent traversal for input drivers and output loads without local uses."""
import argparse
import json
import re
import tempfile
from pathlib import Path
from endpoint_dedup_regression import read_db, run


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
                assert len(entries) == 1, (signal, mode, entries)
                entry = entries[0]
                assert decoded['strings'][entry[0]] == 'port_parent_top.'+path, (signal, entry)
                assert entry[4] == labels[label] and entry[7] == 0 and entry[9] == 1, (signal, entry)
                payload = json.loads(run(binary, ['trace', '--db', db, '--signal', signal,
                    '--mode', mode, '--format', 'json']).stdout)
                assert len(payload['endpoints']) == 1 and payload['endpoints'][0]['kind'] == 'expr', payload
                assert payload['endpoints'][0]['line'] == labels[label], payload
                assert payload['endpoints'][0]['assignment'], payload
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
        assert 'SEMANTICS_EPOCH:11\n' in meta.read_text()
        meta.write_text(meta.read_text().replace('SEMANTICS_EPOCH:11\n', 'SEMANTICS_EPOCH:10\n'))
        rebuilt = compile_db(binary, source, db, incremental=True)
        assert 'incremental-cache-hit' not in rebuilt.stdout
        assert db.read_bytes() == off.read_bytes()
        assert 'SEMANTICS_EPOCH:11\n' in meta.read_text()
        hit = compile_db(binary, source, db, incremental=True)
        assert 'incremental-cache-hit' in hit.stdout
        print('PASS: epoch 10 fingerprint forces epoch 11 rebuild, then cache hit')


if __name__ == '__main__':
    main()
