#!/usr/bin/env python3
"""E4c USER DECISION: merged root display, provenance, and stable traversal."""
import argparse
import copy
import json
import os
import re
import struct
import subprocess
import tempfile
from pathlib import Path

HEADER = struct.Struct('<16sII15Q')
ENDPOINT = struct.Struct('<11I4B')


def run(binary, arguments, env=None):
    proc = subprocess.run([str(binary), *map(str, arguments)], capture_output=True, text=True,
                          timeout=45, env=None if env is None else {**os.environ, **env})
    assert proc.returncode == 0, (arguments, proc.returncode, proc.stdout, proc.stderr)
    return proc


def compile_db(binary, source, db, env=None, incremental=False):
    args = ['compile', '--db', db, '--single-unit', source, '--top', 'merged_range_clip']
    if incremental:
        args.append('--incremental')
    return run(binary, args, env)


def trace_args(signal, extra=(), mode='drivers', fmt='json'):
    return ['trace', '--mode', mode, '--signal', signal, '--format', fmt, *extra]


def trace(binary, db, signal, extra=(), mode='drivers'):
    return json.loads(run(binary, ['trace', '--db', db, *trace_args(signal, extra, mode)[1:]]).stdout)


def layout(data):
    header = list(HEADER.unpack_from(data))
    assert header[1] == 6 and header[2] in (1, 3, 7)
    strings_count, blob_size, signals_count, endpoints_count = header[3:7]
    blob_start = HEADER.size + 4 * (strings_count + 1)
    offsets = struct.unpack_from('<' + 'I' * (strings_count + 1), data, HEADER.size)
    strings = [data[blob_start + offsets[i]:blob_start + offsets[i+1]].decode()
               for i in range(strings_count)]
    endpoints_start = blob_start + blob_size + 32 * signals_count
    endpoints = [ENDPOINT.unpack_from(data, endpoints_start + ENDPOINT.size * i)
                 for i in range(endpoints_count)]
    return header, strings, blob_start, endpoints_start, endpoints


def assert_flag_only_delta(parent_bytes, candidate_bytes):
    assert len(parent_bytes) == len(candidate_bytes)
    old = layout(parent_bytes)
    new = layout(candidate_bytes)
    assert old[:4] == new[:4]
    cleared = bytearray(candidate_bytes)
    merged = 0
    for index, (before, after) in enumerate(zip(old[4], new[4])):
        assert before[:-1] == after[:-1], (index, before, after)
        assert after[-1] & ~1 == before[-1], (index, before, after)
        if after[-1] & 1:
            merged += 1
        cleared[new[3] + index * ENDPOINT.size + 47] &= ~1
    assert merged > 0 and bytes(cleared) == parent_bytes
    return merged


def patch_records(db, predicate, bitmap=None, approximate=None, scalar_bitmaps=None):
    """Test-only v5 edits: provenance plus one chosen string, preserving opaque sections."""
    data = bytearray(db.read_bytes())
    header, strings, blob_start, endpoints_start, endpoints = layout(data)
    selected = []
    bitmap_ids = set()
    for index, record in enumerate(endpoints):
        if not predicate(record, strings):
            continue
        selected.append(index)
        data[endpoints_start + index * ENDPOINT.size + 47] |= 1
        if scalar_bitmaps is not None:
            # Two logical records naturally bypass the scalar merge. Test-only
            # conversion supplies overlapping scalar ranges without adding a writer hook.
            replacement = scalar_bitmaps[strings[record[3]]]
            struct.pack_into('<I', data, endpoints_start + index * ENDPOINT.size + 12,
                             strings.index(replacement))
            data[endpoints_start + index * ENDPOINT.size + 47] &= ~2
        if approximate is not None:
            data[endpoints_start + index * ENDPOINT.size + 45] = approximate
        bitmap_ids.add(record[3])
    assert selected
    if bitmap is not None:
        for string_id in bitmap_ids:
            strings[string_id] = bitmap
        encoded = [text.encode() for text in strings]
        offsets = [0]
        for text in encoded:
            offsets.append(offsets[-1] + len(text))
        header[4] = offsets[-1]
        old_suffix = blob_start + layout(db.read_bytes())[0][4]
        data = bytearray(HEADER.pack(*header) + struct.pack('<' + 'I' * len(offsets), *offsets)
                         + b''.join(encoded) + data[old_suffix:])
    db.write_bytes(data)
    return selected


def selected(payload, line):
    return [e for e in payload['endpoints'] if e['line'] == line]


def remove_coordinates(db, signal):
    """Test-only scalar owner for the synthetic original-key clipping check.

    The frontend fixture is multidimensional to keep distinct original bitmaps
    before its scalar merge pass. This synthetic test removes only that owner's
    declaration, alongside the explicit scalar bitmap conversion above.
    """
    from coordinate_diagnostics_regression import footer
    data = db.read_bytes()
    begin, rows_start, count, axes_count_pos = footer(data)
    header, strings, blob_start, _, _ = layout(data)
    signal_start = blob_start + header[4]
    owner = next(i for i in range(header[5])
                 if struct.unpack_from('<I', data, signal_start + 32*i)[0] == strings.index(signal))
    kept_rows = []
    kept_axes = bytearray()
    for i in range(count):
        signal_id, axis_begin, axis_count, flags = struct.unpack_from('<4I', data, rows_start + 16*i)
        if signal_id == owner:
            continue
        kept_rows.append((signal_id, len(kept_axes)//12, axis_count, flags))
        kept_axes.extend(data[axes_count_pos+8+12*axis_begin:axes_count_pos+8+12*(axis_begin+axis_count)])
    assert len(kept_rows) == count-1
    db.write_bytes(data[:begin] + struct.pack('<Q', len(kept_rows))
                   + b''.join(struct.pack('<4I', *row) for row in kept_rows)
                   + struct.pack('<Q', len(kept_axes)//12) + kept_axes)


def bitmap_range(bitmap):
    match = re.fullmatch(r'\[(-?\d+)(?::(-?\d+))?\]', bitmap)
    assert match, bitmap
    left = int(match[1])
    right = int(match[2]) if match[2] is not None else left
    return left, right


def clip_text(bitmap, query):
    left, right = bitmap_range(bitmap)
    lo, hi = max(min(left, right), min(query)), min(max(left, right), max(query))
    if lo > hi:
        return bitmap
    if lo == hi:
        return f'[{lo}]'
    return f'[{lo}:{hi}]' if left < right else f'[{hi}:{lo}]'


class Serve:
    def __init__(self, binary, db):
        self.proc = subprocess.Popen([str(binary), 'serve', '--db', str(db)], text=True,
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE)
        self.response()

    def response(self):
        result = []
        while True:
            line = self.proc.stdout.readline()
            assert line, (self.proc.poll(), result)
            if line.rstrip('\n') == '<<END>>':
                return ''.join(result)
            result.append(line)

    def query(self, args):
        self.proc.stdin.write(' '.join(args) + '\n')
        self.proc.stdin.flush()
        return self.response()

    def close(self):
        self.query(['quit'])
        self.proc.stdin.close()
        self.proc.wait(timeout=10)
        errors = self.proc.stderr.read()
        self.proc.stdout.close()
        self.proc.stderr.close()
        assert self.proc.returncode == 0 and not errors, errors


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--rtl-trace', type=Path, required=True)
    parser.add_argument('--source-dir', type=Path, required=True)
    parser.add_argument('--parent-bin', type=Path)
    args = parser.parse_args()
    binary = args.rtl_trace.resolve()
    parent = args.parent_bin.resolve() if args.parent_bin else None
    fixture = args.source_dir.resolve() / 'tests/fixtures/merged_range_clip.sv'
    lines = {line.split('// CASE ', 1)[1].strip(): i
             for i, line in enumerate(fixture.read_text().splitlines(), 1) if '// CASE ' in line}
    root_signal = 'merged_range_clip.x'
    with tempfile.TemporaryDirectory(prefix='rtl_trace_merged_') as temp:
        root = Path(temp)
        db = root / 'candidate.db'
        compile_db(binary, fixture, db)
        header, strings, _, _, records = layout(db.read_bytes())
        by_line = {}
        for record in records:
            by_line.setdefault(record[4], []).append(record)
        assert all(e[-1] == 1 for e in by_line[lines['root_merged']])
        assert all(e[-1] == 3 for e in by_line[lines['logical_rows']]
                   if strings[e[0]] == 'merged_range_clip.m')
        assert all(e[-1] == 2 for e in by_line[lines['logical_point']]
                   if strings[e[0]] == 'merged_range_clip.m')
        assert all(e[-1] == 0 for e in by_line[lines['unmerged']] + by_line[lines['symbolic']])
        plain = trace(binary, db, root_signal)
        assert {e['bit_map'] for e in selected(plain, lines['root_merged'])} == {'[7:0]'}
        for suffix, bitmap in (('[2]', '[2]'), ('[6:3]', '[6:3]'), ('[3:6]', '[6:3]')):
            result = trace(binary, db, root_signal + suffix)
            assert {e['bit_map'] for e in selected(result, lines['root_merged'])} == {bitmap}, result
            text = run(binary, ['trace', '--db', db, *trace_args(root_signal + suffix, fmt='text')[1:]]).stdout
            assert re.search(r'\bbits\s+' + re.escape(bitmap), text), text
        disjoint = trace(binary, db, root_signal + '[20]')
        assert not disjoint['endpoints'] and any(s['reason'] == 'bit_filter' for s in disjoint['stops'])
        cone = trace(binary, db, root_signal + '[2]', ('--cone-level', '3'))
        assert {e['bit_map'] for e in selected(cone, lines['root_merged'])} == {'[2]'}
        assert {e['bit_map'] for e in selected(cone, lines['deeper_merged'])} == {'[15:8]'}, cone
        port = trace(binary, db, 'merged_range_clip.port_output[2]', ('--cone-level', '3'))
        # Crossing a mapped identity connection retains per-source native
        # bit accesses and owning coverage; mapped entries are not range-merged.
        assert {e['bit_map'] for e in selected(port, lines['leaf_merged'])} == {'[2]'}, port
        member = trace(binary, db, 'merged_range_clip.packet.hi[2]')
        assert {e['bit_map'] for e in selected(member, lines['member_merged'])} == {'[7:0]'}, member
        logical = trace(binary, db, 'merged_range_clip.m[1][0]', mode='loads')
        assert {e['bit_map'] for e in selected(logical, lines['logical_rows'])} == {'[2:1]'}, logical
        unmerged = trace(binary, db, 'merged_range_clip.input_bits[2]', mode='loads')
        assert {e['bit_map'] for e in selected(unmerged, lines['unmerged'])} == {'[7:0]'}, unmerged
        assert all(e['bit_map_approximate'] for e in selected(unmerged, lines['symbolic']))
        for label, lhs in (('repeated_slice', 'repeated_slice'), ('single_slice', 'single_slice')):
            records_on_input = [e for e in by_line[lines[label]]
                                if strings[e[0]] == 'merged_range_clip.input_bits']
            assert len(records_on_input) == 1 and records_on_input[0][-1] == 0, records_on_input
            endpoint = selected(unmerged, lines[label])
            assert len(endpoint) == 1 and endpoint[0]['bit_map'] == '[7:0]', endpoint
            assert endpoint[0]['lhs'] == ['merged_range_clip.' + lhs]
            assert endpoint[0]['rhs'] == ['merged_range_clip.input_bits']
            assert 'input_bits[7:0]' in endpoint[0]['assignment'], endpoint
        print('PASS: repeated concat slice and single slice retain identical unmerged coordinates and full refs')
        print('PASS: merged 0x01, logical 0x02/0x03; root intersections; cone/port/member/logical/unmerged exclusions')

        # Synthetic exact scalar coordinates cover orientations and full int32 bounds
        # independently of the unchanged legacy frontend selector normalization.
        for label, original, suffix, expected in (
                ('ascending', '[0:7]', '[6:3]', '[3:6]'),
                ('signed', '[-1:-8]', '[-4:-2]', '[-2:-4]'),
                ('signed_point', '[-1:-8]', '[-3]', '[-3]'),
                ('min', '[-2147483648:-2147483646]', '[-2147483647]', '[-2147483647]'),
                ('max', '[2147483647:2147483645]', '[2147483646]', '[2147483646]')):
            patched = root / (label + '.db')
            patched.write_bytes(db.read_bytes())
            patch_records(patched, lambda e, _: e[4] == lines['root_merged'], bitmap=original)
            got = trace(binary, patched, root_signal + suffix)
            assert {e['bit_map'] for e in selected(got, lines['root_merged'])} == {expected}, got
        approximate = root / 'approximate.db'
        approximate.write_bytes(db.read_bytes())
        patch_records(approximate, lambda e, _: e[4] == lines['root_merged'], approximate=1)
        got = trace(binary, approximate, root_signal + '[2]')
        assert {e['bit_map'] for e in selected(got, lines['root_merged'])} == {'[7:0]'}
        multi = root / 'multiple_brackets.db'
        multi.write_bytes(db.read_bytes())
        patch_records(multi, lambda e, _: e[4] == lines['root_merged'], bitmap='[7:0][1]')
        got = trace(binary, multi, root_signal + '[2]')
        assert {e['bit_map'] for e in selected(got, lines['root_merged'])} == {'[7:0][1]'}
        print('PASS: ascending, signed, int32 limits, approximate and multi-bracket display')

        duplicate_db = root / 'dedup.db'
        compile_db(binary, fixture, duplicate_db)
        patch_records(duplicate_db, lambda e, _: e[4] in (
            lines['distinct_original_keys'], lines['identical_original_keys']) and e[3] != 0xffffffff,
            scalar_bitmaps={'[1][0]': '[3:0]', '[2][0]': '[7:0]'})
        remove_coordinates(duplicate_db, 'merged_range_clip.m')
        result = trace(binary, duplicate_db, 'merged_range_clip.m[2]', mode='loads')
        distinct = selected(result, lines['distinct_original_keys'])
        identical = selected(result, lines['identical_original_keys'])
        assert len(distinct) == 2 and all(e['bit_map'] == '[2]' for e in distinct), distinct
        assert len(identical) == 1 and identical[0]['bit_map'] == '[2]', identical
        print('PASS: original-key dedup precedes display clipping')

        queries = [trace_args(root_signal + suffix) for suffix in ('[2]', '[5]', '', '[2]')]
        queries += [trace_args('merged_range_clip.m[1][0]', mode='loads'),
                    trace_args('merged_range_clip.packet.hi[2]')]
        client = Serve(binary, db)
        try:
            for command in queries + list(reversed(queries)):
                expected = run(binary, ['trace', '--db', db, *command[1:]]).stdout
                assert client.query(command) == expected, command
        finally:
            client.close()
        assert trace(binary, db, root_signal) == plain
        print('PASS: repeated cached serve selections leave full records stable')

        off = root / 'off.db'
        compile_db(binary, fixture, off, env={'RTL_TRACE_CANONICAL_BODIES': '0'})
        assert db.read_bytes() == off.read_bytes()
        assert Path(str(db) + '.meta').read_bytes() == Path(str(off) + '.meta').read_bytes()
        verified = compile_db(binary, fixture, root / 'verified.db', env={'RTL_TRACE_CANONICAL_VERIFY': '1'})
        assert 'mismatched_lists=0' in verified.stdout
        print('PASS: canonical on/off DB identity and VERIFY zero mismatches')

        if parent:
            parent_db = root / 'parent.db'
            compile_db(parent, fixture, parent_db)
            merged_count = assert_flag_only_delta(parent_db.read_bytes(), db.read_bytes())
            before_meta = Path(str(parent_db) + '.meta').read_text()
            after_meta = Path(str(db) + '.meta').read_text()
            assert before_meta.replace('SEMANTICS_EPOCH:6', 'SEMANTICS_EPOCH:7') == after_meta
            for mode, signal in (('drivers', root_signal + '[2]'), ('drivers', root_signal),
                                 ('drivers', 'merged_range_clip.packet.hi[2]'),
                                 ('loads', 'merged_range_clip.m[1][0]')):
                for fmt in ('json', 'text'):
                    command = trace_args(signal, mode=mode, fmt=fmt)
                    before = run(parent, ['trace', '--db', parent_db, *command[1:]])
                    after = run(binary, ['trace', '--db', parent_db, *command[1:]])
                    assert (before.stdout, before.stderr) == (after.stdout, after.stderr)
            for extra in ((), ('--cone-level','2'), ('--cone-level','3'),
                          ('--cone-level','3','--depth','1'),
                          ('--cone-level','3','--max-nodes','2'),
                          ('--cone-level','3','--prefer-port-hop')):
                before = trace(parent, parent_db, root_signal + '[2]', extra)
                after = trace(binary, db, root_signal + '[2]', extra)
                restored = copy.deepcopy(after)
                for endpoint in selected(restored, lines['root_merged']):
                    endpoint['bit_map'] = '[7:0]'
                assert before == restored, (extra,before,restored)
            rebuilt = compile_db(binary, fixture, parent_db, incremental=True)
            assert 'cache hit' not in (rebuilt.stdout + rebuilt.stderr).lower()
            assert parent_db.read_bytes() == db.read_bytes()
            hit = compile_db(binary, fixture, parent_db, incremental=True)
            assert 'cache hit' in (hit.stdout + hit.stderr).lower()
            print(f'PASS: {merged_count} merged flags are the only DB-byte deltas; old DB outputs exact; traversal unchanged; epoch 6 -> 7 rebuild then hit')
        else:
            print('SKIP: epoch 6 parent comparison (compatible B-only snapshot not supplied)')


if __name__ == '__main__':
    main()
