"""Public query identity keeps ordered context refs, including timing accesses."""
import argparse
import json
import struct
import tempfile
from pathlib import Path
from endpoint_dedup_regression import run
from graph_db_mapping_regression import layout


def check(binary, fixtures, root):
    source = fixtures/'shared_clock_refs.sv'
    queries = 0
    original = None
    for top, expected in [('shared_clock_top', 6), ('compact_shared_clock_top', 1500)]:
        dbs = []
        for canonical in ('0', '1'):
            db = root/(top+canonical+'.db'); dbs.append(db)
            compile_result = run(binary, ['compile', '--db', db, '--single-unit', source, '--top', top],
                                 {'RTL_TRACE_CANONICAL_BODIES': canonical, 'RTL_TRACE_CANONICAL_VERIFY': '1'})
            if canonical == '1': assert 'mismatched_lists=0' in compile_result.stdout
            if top == 'shared_clock_top':
                selections = [owner+'.'+signal+suffix for owner in (top, top+'.u')
                              for signal in ('clk', 'rst_n') for suffix in ('', '[0]', '[0:0]')]
            else:
                selections = [top+'.clk', top+'.rst_n']
            for signal in selections:
                body = json.loads(run(binary, ['trace', '--db', db, '--signal', signal,
                                              '--mode', 'loads', '--format', 'json']).stdout)
                endpoints = body['endpoints']
                targets = {v for e in endpoints for v in e['lhs']}
                targets.update(e['path'] for e in endpoints if e['assignment'].startswith('global-') and e['assignment'].endswith('-sink'))
                assert len(targets) == expected, body
                if top == 'shared_clock_top':
                    assert {top+'.u.gen_sync['+str(i)+'].chain' for i in range(5)} <= targets, body
                    assert len(endpoints) == (10 if signal.split('.')[-1].startswith('rst_n') else 5), body
                else:
                    assert len(endpoints) == expected and not body['stops'], body
                queries += 1
        assert dbs[0].read_bytes() == dbs[1].read_bytes(), top
        if top == 'shared_clock_top': original = dbs[0].read_bytes()

    # Mutate only valid public reference vectors. The native timing key remains
    # identical. These fixtures exercise the actual query dedup, not a Python key.
    sections = layout(original)
    off, n, _ = sections['offsets']; blob, _, _ = sections['blob']
    offsets = list(struct.unpack_from('<'+'I'*n, original, off))
    strings = [original[blob+offsets[i]:blob+offsets[i+1]] for i in range(n-1)]
    sb, count, _ = sections['signals']; eb, _, _ = sections['endpoints']; rb, _, _ = sections['signal_refs']
    owner = b'shared_clock_top.u.clk'
    row = next(struct.unpack_from('<8I', original, sb+32*i) for i in range(count)
               if strings[struct.unpack_from('<I', original, sb+32*i)[0]] == owner)
    assert row[4] == 5
    e0 = struct.unpack_from('<11I4B', original, eb+48*row[3])
    e1pos = eb+48*(row[3]+1); e1 = struct.unpack_from('<11I4B', original, e1pos)
    assert e0[8] == e1[8] == 2
    refs0 = struct.unpack_from('<2I', original, rb+4*e0[7])
    refs1 = struct.unpack_from('<2I', original, rb+4*e1[7])
    chain0, meta = refs0 if b'.chain' in strings[refs0[0]] else tuple(reversed(refs0))
    chain1 = next(i for i in refs1 if b'.chain' in strings[i])
    mutations = []
    duplicate = bytearray(original); struct.pack_into('<II', duplicate, e1pos+28, e0[7], 2)
    mutations.append(('same_complete_refs', duplicate, 4))
    order = bytearray(original); struct.pack_into('<2I', order, rb+4*e1[7], *reversed(refs0))
    mutations.append(('ordered_refs', order, 5))
    multiplicity = bytearray(original)
    struct.pack_into('<I', multiplicity, eb+48*row[3]+32, 1)
    struct.pack_into('<I', multiplicity, rb+4*e0[7], meta)
    struct.pack_into('<2I', multiplicity, rb+4*e1[7], meta, meta)
    mutations.append(('reference_count_and_multiplicity', multiplicity, 5))
    side = bytearray(original); struct.pack_into('<4I', side, e1pos+28, 0, 0, e0[7], 2)
    mutations.append(('reference_side', side, 5))
    framing = bytearray(original)
    e2 = struct.unpack_from('<11I4B', original, eb+48*(row[3]+2))
    chain2 = next(i for i in struct.unpack_from('<2I', original, rb+4*e2[7]) if b'.chain' in strings[i])
    struct.pack_into('<2I', framing, rb+4*e0[7], chain0, meta)
    struct.pack_into('<2I', framing, rb+4*e1[7], chain1, chain2)
    replacement = {chain0: b'a\tb', meta: b'c', chain1: b'a', chain2: b'b\tc'}
    new_strings = [replacement.get(i, value) for i, value in enumerate(strings)]
    new_blob = b''.join(new_strings); new_offsets = [0]
    for value in new_strings: new_offsets.append(new_offsets[-1]+len(value))
    framing = framing[:blob]+new_blob+framing[blob+offsets[-1]:]
    struct.pack_into('<Q', framing, 32, len(new_blob))
    struct.pack_into('<'+'I'*n, framing, off, *new_offsets)
    mutations.append(('length_framed_reference_names', framing, 5))
    for label, data, expected in mutations:
        db = root/(label+'.db'); db.write_bytes(data)
        body = json.loads(run(binary, ['trace', '--db', db, '--signal', owner.decode(),
                                      '--mode', 'loads', '--format', 'json']).stdout)
        assert len(body['endpoints']) == expected, (label, body)
        queries += 1
    print('PASS: shared clock/async reset contexts, compact threshold, exact duplicate/order/count/multiplicity/side/length framing')
    return queries


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rtl-trace', type=Path, required=True)
    parser.add_argument('--source-dir', type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='rtl_query_refs_') as directory:
        check(args.rtl_trace.resolve(), args.source_dir.resolve()/'tests/fixtures/port_connection_mapping', Path(directory))
