"""Raw child connections cannot certify a reverse bridge on a wrapper port."""
import argparse
import json
import re
import tempfile
from pathlib import Path
from endpoint_dedup_regression import read_db, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rtl-trace', type=Path, required=True)
    parser.add_argument('--source-dir', type=Path, required=True)
    args = parser.parse_args()
    source = args.source_dir.resolve() / 'tests/fixtures/canonical_bodies/forwarded_port_traversal.sv'
    binary = args.rtl_trace.resolve()
    labels = {line.split('// CASE ', 1)[1].strip(): number
              for number, line in enumerate(source.read_text().splitlines(), 1) if '// CASE ' in line}
    owners = [('u0', 'x0', 'r0', 'clk0'), ('u1', 'x1', 'r1', 'clk1'),
              ('u_nested', 'x0', 'r2', 'clk0'), ('u_nested.u_wrapper', 'x0', 'r2', 'clk0'),
              ('u_flopped', 'x1', 'r3', 'clk1')]
    with tempfile.TemporaryDirectory(prefix='rtl_forwarded_ports_') as directory:
        root = Path(directory)
        db = root/'canonical.db'
        compile_args = ['compile', '--db', db, '--single-unit', source, '--top', 'forwarded_port_top']
        compiled = run(binary, compile_args, {'RTL_TRACE_CANONICAL_STATS': '1'})
        redirect = re.search(r'\[Canon\] tracer signals .*redirected=(\d+)', compiled.stdout)
        assert redirect and int(redirect.group(1)) > 0, compiled.stdout
        decoded = read_db(db)
        for owner, driver, load, clock in owners:
            for suffix, mode, target in [('d', 'drivers', driver), ('q', 'loads', load),
                                         ('clk', 'drivers', clock)]:
                signal = 'forwarded_port_top.'+owner+'.'+suffix
                entries = decoded['lists'][signal][0 if mode == 'drivers' else 1]
                assert len(entries) == 1, (signal, entries)
                entry = entries[0]
                assert decoded['strings'][entry[0]] == 'forwarded_port_top.'+target, (signal, entry)
                assert entry[4] == labels[target] and entry[7] == 0 and entry[9] == 1, (signal, entry)
                payload = json.loads(run(binary, ['trace', '--db', db, '--signal', signal,
                    '--mode', mode, '--format', 'json']).stdout)
                assert len(payload['endpoints']) == 1 and payload['endpoints'][0]['line'] == labels[target], payload
            # Opposite lists are nonempty, yet all data endpoints name deeper
            # leaf signals. They cannot bridge the wrapper's own d/q path.
            for suffix, mode in [('d', 1), ('q', 0)]:
                signal = 'forwarded_port_top.'+owner+'.'+suffix
                entries = decoded['lists'][signal][mode]
                assert entries and all(decoded['strings'][entry[0]] != signal for entry in entries), (signal, entries)
        for suffix, mode, target in [('d', 'drivers', 'x1'), ('q', 'loads', 'r3'), ('clk', 'drivers', 'clk1')]:
            signal = 'forwarded_port_top.u_flopped.u_leaf.'+suffix
            entries = decoded['lists'][signal][0 if mode == 'drivers' else 1]
            assert len(entries) == 1 and entries[0][7] == 1, (signal, entries)
            payload = json.loads(run(binary, ['trace', '--db', db, '--signal', signal,
                '--mode', mode, '--format', 'json']).stdout)
            assert len(payload['endpoints']) == 1 and payload['endpoints'][0]['line'] == labels[target], payload
        print('PASS: forwarded-only wrapper inputs, outputs and nested conditional clocks store actual parent routes; real local-use leaf ports stay compact')

        off = root/'baseline.db'
        run(binary, ['compile', '--db', off, '--single-unit', source, '--top', 'forwarded_port_top'],
            {'RTL_TRACE_CANONICAL_BODIES': '0'})
        assert off.read_bytes() == db.read_bytes()
        verified = run(binary, ['compile', '--db', root/'verify.db', '--single-unit', source,
                               '--top', 'forwarded_port_top'], {'RTL_TRACE_CANONICAL_VERIFY': '1'})
        assert 'mismatched_lists=0' in verified.stdout, verified.stdout
        assert 'SEMANTICS_EPOCH:13\n' in Path(str(db)+'.meta').read_text()
        print('PASS: forwarded routes across distinct canonical parent connections; canonical on/off bytes, VERIFY and epoch13 match')


if __name__ == '__main__':
    main()
