#!/usr/bin/env python3
"""Version6 declaration and selector diagnostics, including old-reader rejection."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile


def run(binary, args, expected=0, env=None):
    p = subprocess.run([str(binary), *map(str, args)], text=True, capture_output=True, timeout=40,
                       env={**os.environ, **(env or {})})
    assert p.returncode == expected, (args, p.returncode, p.stdout, p.stderr)
    return p


def compile_db(binary, source, db, incremental=False, env=None):
    return run(binary, ['compile', '--db', db, '--single-unit', source, '--top', 'coordinate_diagnostics',
                        '-Wno-index-oob', '-Wno-range-oob', *(['--incremental'] if incremental else [])], env=env)


def trace(binary, db, signal, mode='loads', expected=0, text=False):
    return run(binary, ['trace', '--db', db, '--mode', mode, '--signal', signal,
                       '--format', 'text' if text else 'json'], expected)


def footer(data):
    h = struct.unpack_from('<16sII15Q', data)
    assert h[1] == 6 and h[2] in (1, 3, 7, 15), h[:3]
    pos = 144 + 4 * (h[3] + 1) + h[4]
    pos += sum(n * size for n, size in zip(h[5:], (32, 48, 4, 12, 4, 12, 4, 12, 4, 24, 4, 16, 4)))
    count = struct.unpack_from('<Q', data, pos)[0]
    pos += 8 + count * 12
    count = struct.unpack_from('<Q', data, pos)[0]
    pos += 8 + count * 12
    rows_count = struct.unpack_from('<Q', data, pos)[0]
    rows_start = pos + 8
    axes_count_pos = rows_start + rows_count * 16
    axes_count = struct.unpack_from('<Q', data, axes_count_pos)[0]
    assert axes_count_pos + 8 + axes_count * 12 == len(data)
    return pos, rows_start, rows_count, axes_count_pos


def original_fields(payload):
    assert payload['coordinate_encoding'] in ('flat_bits', 'declared_axes', 'legacy_unverified', 'parent_struct_bits')
    result = {k: v for k, v in payload.items() if k not in ('coordinate_encoding', 'declared_axes', 'diagnostics')}
    result['endpoints'] = [{k: v for k, v in e.items() if k != 'bit_map_encoding'} for e in result['endpoints']]
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--rtl-trace', type=Path, required=True)
    parser.add_argument('--source-dir', type=Path, required=True)
    parser.add_argument('--parent-bin', type=Path)
    parser.add_argument('--evidence', type=Path)
    args = parser.parse_args()
    binary = args.rtl_trace.resolve()
    source = args.source_dir.resolve() / 'tests/fixtures/coordinate_diagnostics.sv'
    records = []
    with tempfile.TemporaryDirectory(prefix='rtl_coordinate_diagnostics_') as directory:
        root = Path(directory)
        db = root / 'fresh.db'
        compile_db(binary, source, db)
        data = db.read_bytes()
        start, rows_start, rows_count, axes_count_pos = footer(data)
        assert rows_count >= 4
        for mode in ('drivers', 'loads'):
            for signal, code in [('p[1]', 'ambiguous_single_axis'), ('unused[1]', 'ambiguous_single_axis'),
                                 ('p[5]', 'axis_out_of_bounds'), ('p[1][5]', 'axis_out_of_bounds'),
                                 ('unused[5]', 'axis_out_of_bounds'), ('p[1][0][0]', 'too_many_axes'),
                                 ('aggregates[1][0]', 'unsupported_coordinate_type')]:
                name = 'coordinate_diagnostics.' + signal
                p = trace(binary, db, name, mode, expected=1)
                body = json.loads(p.stdout)
                assert body['diagnostics'][-1]['code'] == code and body['diagnostics'][-1]['severity'] == 'error', body
                assert not body['endpoints'] and body['coordinate_encoding'] == 'declared_axes'
                assert 'rebuild' not in p.stderr.lower(), p.stderr
                text = trace(binary, db, name, mode, expected=1, text=True)
                assert not text.stderr and code in text.stdout, text.stdout
                records.append({'signal': name, 'mode': mode, 'code': code, 'json': body})
        p_whole = json.loads(trace(binary, db, 'coordinate_diagnostics.p').stdout)
        maps = {e['bit_map'] for e in p_whole['endpoints']}
        assert '[!oob:5][0]' in maps and '[1][!oob:4:2]' in maps and '[?idx][0]' in maps, maps
        assert "[!constant-xz:2'bxx][0]" in maps and "[!constant-overflow:64'h100000000][0]" in maps, maps
        for e in p_whole['endpoints']:
            assert e['bit_map_encoding'] == 'declared_axes', e
        selected = json.loads(trace(binary, db, 'coordinate_diagnostics.p[1][0]').stdout)
        assert any(e['bit_map'] == '[?idx][0]' for e in selected['endpoints']), selected
        enums = json.loads(trace(binary, db, 'coordinate_diagnostics.enums[1][0]').stdout)
        assert enums['coordinate_encoding'] == 'declared_axes' and not enums['diagnostics']
        assert any(e['bit_map'] == '[1][0]' for e in enums['endpoints']), enums
        unused = json.loads(trace(binary, db, 'coordinate_diagnostics.unused').stdout)
        assert unused['declared_axes'] == [{'left': 3, 'right': 0, 'fixed': True, 'packed': True},
                                          {'left': 1, 'right': 0, 'fixed': True, 'packed': True}], unused
        print('PASS: declaration bounds/packed ambiguity in both modes, enum axes, aggregate unsupported, exact OOB vs dynamic markers')
        for mode in ('drivers', 'loads'):
            ambiguous = json.loads(trace(binary, db, 'coordinate_diagnostics.p[2]', mode, expected=1).stdout)
            message = ambiguous['diagnostics'][-1]['message']
            assert 'Declared row 2 → coordinate_diagnostics.p[2][1:0]' in message, message
            assert 'Flattened bit 2 → coordinate_diagnostics.p[1][0]' in message, message
            flat = json.loads(trace(binary, db, 'coordinate_diagnostics.p[5]', mode, expected=1).stdout)
            assert 'Flattened bit 5 → coordinate_diagnostics.p[2][1]' in flat['diagnostics'][-1]['message'], flat
            rows = json.loads(trace(binary, db, 'coordinate_diagnostics.p[3:2]', mode, expected=1).stdout)
            assert 'Declared row range [3:2] → coordinate_diagnostics.p[3:2][1:0]' in rows['diagnostics'][-1]['message'], rows
            aggregate = json.loads(trace(binary, db, 'coordinate_diagnostics.aggregates[1]', mode, expected=1).stdout)
            assert 'packed struct or packed union array' in aggregate['diagnostics'][-1]['message']
            assert 'whole signal still works' in aggregate['diagnostics'][-1]['message']
            trace(binary, db, 'coordinate_diagnostics.aggregates', mode)
            member = json.loads(trace(binary, db, 'coordinate_diagnostics.member_packet.matrix[2]', mode, expected=1).stdout)
            assert member['diagnostics'][-1]['code'] == 'ambiguous_single_axis'
            multi = json.loads(trace(binary, db, 'coordinate_diagnostics.member_packet.matrix[2][1]', mode, expected=1).stdout)
            assert multi['diagnostics'][-1]['code'] == 'unsupported_struct_member_axes'
            trace(binary, db, 'coordinate_diagnostics.member_packet.matrix', mode)
            trace(binary, db, 'coordinate_diagnostics.member_packet.vector[2]', mode)
        assert struct.unpack_from('<I', data, 20)[0] == 15
        round2 = root / 'round2_features.db'
        round2_data = bytearray(data); struct.pack_into('<I', round2_data, 20, 1)
        round2.write_bytes(round2_data)
        selected_member = json.loads(trace(binary, round2, 'coordinate_diagnostics.member_packet.matrix[2]', expected=1).stdout)
        assert selected_member['diagnostics'][-1]['code'] == 'legacy_multidimensional_select'
        for mode in ('drivers', 'loads'):
            scalar = json.loads(trace(binary, round2, 'coordinate_diagnostics.member_packet.vector[2]', mode).stdout)
            assert scalar['diagnostics'][0]['code'] == 'legacy_dimensions_unverified', scalar
        trace(binary, round2, 'coordinate_diagnostics.member_packet.matrix')
        print('PASS: rewritten declared/flat queries, specific aggregate type, member ambiguity and fresh scalar-member selection')

        # Remove the footer and feature fields to create a declaration-free v5
        # reader fixture. New coordinate records must not be inferred as declarations.
        legacy = root / 'legacy.db'
        legacy_data = bytearray(data[:start])
        struct.pack_into('<II', legacy_data, 16, 5, 0)
        legacy.write_bytes(legacy_data)
        for mode in ('drivers', 'loads'):
            error = trace(binary, legacy, 'coordinate_diagnostics.p[1]', mode, expected=1)
            assert json.loads(error.stdout)['diagnostics'][-1]['code'] == 'legacy_multidimensional_select'
            warning = trace(binary, legacy, 'coordinate_diagnostics.unused[1]', mode)
            assert json.loads(warning.stdout)['diagnostics'][0]['code'] == 'legacy_dimensions_unverified'
            assert 'legacy_dimensions_unverified' in warning.stderr
            scalar = trace(binary, legacy, 'coordinate_diagnostics.vector_only[5]', mode)
            fresh = trace(binary, db, 'coordinate_diagnostics.vector_only[5]', mode)
            assert original_fields(json.loads(scalar.stdout)) == original_fields(json.loads(fresh.stdout))
            assert json.loads(scalar.stdout)['diagnostics'][0]['code'] == 'legacy_dimensions_unverified'
            member = trace(binary, legacy, 'coordinate_diagnostics.member_packet.vector[2]', mode)
            fresh_member = trace(binary, db, 'coordinate_diagnostics.member_packet.vector[2]', mode)
            assert original_fields(json.loads(member.stdout)) == original_fields(json.loads(fresh_member.stdout))
            assert json.loads(member.stdout)['diagnostics'][0]['code'] == 'legacy_dimensions_unverified'
        print('PASS: legacy single-axis guards inspect both lists, endpoint-free warning and unchanged scalar results')

        malformed = []
        for name, offset, fmt, value in [('feature', 20, '<I', 0), ('huge_rows', start, '<Q', 2**64-1),
                                         ('id', rows_start, '<I', struct.unpack_from('<Q', data, 40)[0]),
                                         ('overlap', rows_start+4, '<I', 1), ('unknown_flags', rows_start+12, '<I', 64),
                                         ('duplicate_id', rows_start+16, '<I', struct.unpack_from('<I', data, rows_start)[0]),
                                         ('huge_axes', axes_count_pos, '<Q', 2**64-1),
                                         ('axis_flags', axes_count_pos+8+8, '<I', 4)]:
            changed = bytearray(data); struct.pack_into(fmt, changed, offset, value); malformed.append((name, changed))
        malformed += [('truncated', data[:-1]), ('trailing', data+b'x')]
        for name, changed in malformed:
            bad = root / (name+'.db'); bad.write_bytes(changed)
            failed = run(binary, ['find', '--db', bad, '--query', 'p'], expected=1)
            assert ('unsupported v6 DB feature flags' if name == 'feature' else 'Failed to read DB') in failed.stderr, failed.stderr
        print('PASS: sparse row IDs/flags/coverage, bounded allocations, truncation and trailing bytes reject')
        for version in (0, 7, 999):
            bad = root / ('version_'+str(version)+'.db')
            changed = bytearray(data); struct.pack_into('<I', changed, 16, version); bad.write_bytes(changed)
            commands = [['trace', '--mode', 'loads', '--signal', 'coordinate_diagnostics.p'],
                        ['find', '--query', 'p'], ['hier'],
                        ['whereis-instance', '--instance', 'coordinate_diagnostics'], ['serve']]
            for command in commands:
                # Serve reports startup errors within its response loop and exits normally.
                failed = run(binary, [command[0], '--db', bad, *command[1:]], expected=0 if command[0] == 'serve' else 1)
                assert f'unsupported DB version {version} (this binary reads 1-6); recompile' in failed.stderr, failed.stderr
                assert 'Failed to read DB' not in failed.stderr
        print('PASS: trace/find/hier/whereis/serve report specific unsupported DB versions')

        # Serve errors retain the response boundary and the next query succeeds.
        serve = subprocess.run(
            [str(binary), 'serve', '--db', str(db)], input='trace --mode loads --signal coordinate_diagnostics.p[5] --format json\ntrace --mode loads --signal coordinate_diagnostics.enums[1][0] --format json\nquit\n',
            capture_output=True, text=True, timeout=40)
        parts = serve.stdout.split('<<END>>\n')
        assert serve.returncode == 0 and len(parts) == 5, serve.stdout
        assert json.loads(parts[1])['diagnostics'][-1]['code'] == 'axis_out_of_bounds'
        assert json.loads(parts[2])['endpoints'] and parts[3] == 'bye\n'
        print('PASS: structured error leaves serve response framing and next query intact')

        meta_path = Path(str(db)+'.meta'); current_meta = meta_path.read_text()
        epoch_line = next(line for line in current_meta.splitlines() if line.startswith('SEMANTICS_EPOCH:'))
        assert int(epoch_line.split(':')[1]) >= 6
        for previous in (3, 4, 5):
            meta_path.write_text(current_meta.replace(epoch_line, 'SEMANTICS_EPOCH:'+str(previous)))
            rebuilt = compile_db(binary, source, db, incremental=True)
            assert 'cache hit' not in (rebuilt.stdout+rebuilt.stderr).lower()
            assert db.read_bytes() == data and meta_path.read_text() == current_meta
        hit = compile_db(binary, source, db, incremental=True)
        assert 'cache hit' in (hit.stdout+hit.stderr).lower()
        # A round-2 DB has epoch 8 too, but lacks member declaration coverage.
        assert 'MEMBER_DECLARED_AXES:1\n' in current_meta
        meta_path.write_text(current_meta.replace('MEMBER_DECLARED_AXES:1\n', ''))
        rebuilt = compile_db(binary, source, db, incremental=True)
        assert 'cache hit' not in (rebuilt.stdout+rebuilt.stderr).lower()
        assert db.read_bytes() == data and meta_path.read_text() == current_meta
        if args.parent_bin:
            parent = args.parent_bin.resolve()
            old = root / 'actual_parent.db'
            compile_db(parent, source, old)
            assert struct.unpack_from('<I', old.read_bytes(), 16)[0] == 5
            old_error = trace(binary, old, 'coordinate_diagnostics.p[1]', expected=1)
            assert json.loads(old_error.stdout)['diagnostics'][-1]['code'] == 'legacy_multidimensional_select'
            refused = run(parent, ['find', '--db', db, '--query', 'p'], expected=1)
            assert 'Failed to read DB' in refused.stderr
            rebuilt = compile_db(binary, source, old, incremental=True)
            assert 'cache hit' not in (rebuilt.stdout+rebuilt.stderr).lower() and old.read_bytes() == data
            records.append({'old_reader_rejects_v6': True, 'actual_parent_incremental_rebuild': True})
            print('PASS: actual-parent/old-reader compatibility')
        else:
            print('SKIP: actual-parent/old-reader compatibility (--parent-bin not supplied)')
        print('PASS: epochs 3/4/5 and missing member-coverage feature force rebuild; fresh hit')
        off = root / 'canonical_off.db'
        compile_db(binary, source, off, env={'RTL_TRACE_CANONICAL_BODIES': '0'})
        assert off.read_bytes() == data
        verify = compile_db(binary, source, root/'verify.db', env={'RTL_TRACE_CANONICAL_VERIFY': '1'})
        assert 'mismatched_lists=0' in verify.stdout
        if args.evidence:
            args.evidence.parent.mkdir(parents=True, exist_ok=True)
            args.evidence.write_text(json.dumps({'db_sha256': hashlib.sha256(data).hexdigest(), 'epoch': epoch_line,
                'source': str(source), 'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(), 'reviews': records}, indent=2)+'\n')
        print('PASS: canonical on/off identical declaration footer and VERIFY exact endpoint lists')


if __name__ == '__main__':
    main()
