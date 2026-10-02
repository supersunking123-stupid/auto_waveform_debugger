"""Used ports must keep shared clock/reset/data fanout compact."""
import argparse
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
        print('PASS: active-port lists retain compact markers; 26,421 endpoints, 31,928 refs, DB below 4 MiB')
        off = root/'baseline.db'
        run(binary, ['compile', '--db', off, '--single-unit', source, '--top', 'gnet_top'],
            {'RTL_TRACE_CANONICAL_BODIES': '0'})
        assert off.read_bytes() == db.read_bytes(), 'canonical/baseline bytes differ'
        verified = run(binary, ['compile', '--db', root/'verify.db', '--single-unit', source,
                               '--top', 'gnet_top'], {'RTL_TRACE_CANONICAL_VERIFY': '1'})
        assert 'mismatched_lists=0' in verified.stdout, verified.stdout
        print('PASS: high-fanout global fixture canonical on/off bytes and VERIFY match')


if __name__ == '__main__':
    main()
