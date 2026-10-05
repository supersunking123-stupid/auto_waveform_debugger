"""Used ports must keep shared clock/reset/data fanout compact."""
import argparse
import json
import struct
import tempfile
from pathlib import Path
from endpoint_dedup_regression import read_db, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rtl-trace', type=Path, required=True)
    parser.add_argument('--source-dir', type=Path, required=True)
    args = parser.parse_args()
    source = args.source_dir.resolve() / 'tests/fixtures/global_port_compaction.sv'
    binary = args.rtl_trace.resolve()
    with tempfile.TemporaryDirectory(prefix='rtl_compact_ports_') as directory:
        root = Path(directory)
        db = root/'canonical.db'
        compile_args = ['compile', '--db', db, '--single-unit', source, '--top', 'gnet_top']
        run(binary, compile_args)
        with db.open('rb') as stream:
            header = struct.unpack('<16sII15Q', stream.read(144))
        # Check counts before decoding: a broken broad upward traversal emits
        # about 9.7 million endpoints and would make a Python object heap huge.
        # Exact fixed-array parent routes restore the original connected lists.
        # Active leaf ports and global sinks stay compact.
        assert header[6] == 26421 and header[7] == 31928, header
        assert db.stat().st_size < 4*1024*1024, db.stat().st_size
        decoded = read_db(db)
        for owner in ('gnet_top.g[0].u', 'gnet_top.g[1099].u'):
            for field, mode in [('d', 0), ('q', 1), ('clk', 0)]:
                entries = decoded['lists'][owner+'.'+field][mode]
                assert len(entries) == 1 and entries[0][7] == 1, (owner, field, entries)
        assert len(decoded['globals']) >= 3, decoded['globals'].keys()
        direct = json.loads(run(binary, ['trace', '--db', db, '--signal', 'gnet_top.gclk',
                                         '--mode', 'loads', '--format', 'json']).stdout)
        alias = json.loads(run(binary, ['trace', '--db', db, '--signal', 'gnet_top.u_pass.clk_out',
                                        '--mode', 'loads', '--format', 'json']).stdout)
        assert len(direct['endpoints']) == 1100 and not alias['stops'], alias
        assert alias['endpoints'] == direct['endpoints'], 'whole clock alias lost compact global sinks'
        print('PASS: active-port lists retain compact markers; 26,421 endpoints, 31,928 refs, DB below 4 MiB')
        off = root/'baseline.db'
        run(binary, ['compile', '--db', off, '--single-unit', source, '--top', 'gnet_top'],
            {'RTL_TRACE_CANONICAL_BODIES': '0'})
        assert off.read_bytes() == db.read_bytes(), 'canonical/baseline bytes differ'
        verified = run(binary, ['compile', '--db', root/'verify.db', '--single-unit', source,
                               '--top', 'gnet_top'], {'RTL_TRACE_CANONICAL_VERIFY': '1'})
        assert 'mismatched_lists=0' in verified.stdout, verified.stdout
        print('PASS: high-fanout global fixture canonical on/off bytes and VERIFY match')

        driver_source = args.source_dir.resolve() / 'tests/fixtures/global_driver_compaction.sv'
        driver_db = root/'drivers.db'
        run(binary, ['compile', '--db', driver_db, '--single-unit', driver_source,
                     '--top', 'global_driver_top'])
        driver_records = read_db(driver_db)
        fields = ('first_vf_offset', 'tx_eq_rx_preset_hint', 'pclk', 'aclk',
                  'rst_n', 'rstn', 'resetn', 'core_rst_n_int')
        source_lines = driver_source.read_text().splitlines()
        for source_name in ('global_driver_top.clk', 'global_driver_top.core_rst_n'):
            assert source_name in driver_records['globals'], driver_records['globals'].keys()
            sinks = driver_records['globals'][source_name][1]
            assert len(sinks) >= 1024, (source_name, len(sinks))
            for field in fields:
                assert 'global_driver_top.g[0].u.'+field in sinks, (source_name, field)
        for index in (0, 1099):
            for field in fields:
                signal = f'global_driver_top.g[{index}].u.{field}'
                answer = json.loads(run(binary, ['trace', '--db', driver_db, '--signal', signal,
                                                 '--mode', 'drivers', '--cone-level', '1',
                                                 '--format', 'json']).stdout)
                expected = {(line, text.strip().rstrip(';')) for line, text in enumerate(source_lines, 1)
                            if text.strip().startswith(field+' <=')}
                actual = {(e['line'], e['assignment']) for e in answer['endpoints']}
                assert actual == expected and len(answer['endpoints']) == 2, (signal, answer)
                assert not answer['diagnostics'], (signal, answer)
                assert all(s['reason'] == 'cone_limit' for s in answer['stops']), (signal, answer)
                assert all(e['path'] == signal and not e['bit_map_approximate']
                           for e in answer['endpoints']), (signal, answer)
        print('PASS: 16 compact-global register driver queries retain exactly 32 assignment endpoints')

        vector_source = args.source_dir.resolve() / 'tests/fixtures/global_clock_vector.sv'
        vector_db = root/'clock_vector.db'
        run(binary, ['compile', '--db', vector_db, '--single-unit', vector_source,
                     '--top', 'gclk'])
        expected_sinks = {f'gclk.g[{index}].u.q' for index in range(1200)}
        for target in ('gclk.clk_v', 'gclk.clk_v[0]', 'gclk.clk_v[1]'):
            answer = json.loads(run(binary, ['trace', '--db', vector_db, '--signal', target,
                                             '--mode', 'loads', '--format', 'json']).stdout)
            assert not answer['diagnostics'], (target, answer)
            selected = target != 'gclk.clk_v'
            endpoints = answer['endpoints']
            assert len(endpoints) == 1200 and {e['path'] for e in endpoints} == expected_sinks, answer
            assert all(e['assignment'] == 'global-clock-sink' and
                       e['bit_map_approximate'] == selected for e in endpoints), answer
            if selected:
                assert len(answer['stops']) == 1, answer
                stop = answer['stops'][0]
                assert stop['reason'] == 'unresolved_connection_mapping' and \
                    'compact-global-net-has-no-source-bit-mapping' in stop['detail'], answer
            else:
                assert not answer['stops'], answer
        print('PASS: whole clock vector keeps 1,200 exact sinks; two bit queries mark '
              'the whole-net fallback approximate and explain its missing mapping')


if __name__ == '__main__':
    main()
