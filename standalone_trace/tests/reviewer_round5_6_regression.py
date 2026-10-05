"""Round-5/6 reviewer fixtures: exact writers and retained fallback boundaries."""
import argparse
import json
import re
import tempfile
from pathlib import Path

from endpoint_dedup_regression import run


# The original reviewer comments also contain queries outside item 6:
# ambiguous single-axis coordinates, missing member names, union writers,
# and inout drivers. Their existing behavior is not an item-6 contract.
EXCLUDED = {
    'adv': {'u4.d[1]', 'x[13]'},
    'misc': {'x[5]', 'io.b', 'tri_w[4]'},
    'sel1': {'m[2]', 'm[2][0]', 'nz[12]', 'asc[5]', 'asc[15]'},
    'sel2': {'u_un.d', 'x[0]', 's.i.x'},
    'st': {'u_un.d', 'u_un2.d', 'un'},
}

# Unsupported conversions keep coarse writers and a child-port boundary.
# These are literal expected fixture results, not a source-proof evaluator.
FALLBACK = {
    'misc': {'ua.d': [], 'ub.d': []},
    'sel2': {
        'u_rep.d[0]': ['x0', 'x1'], 'u_rep.d[7]': ['x0', 'x1'],
        'u_rep2.d[0]': ['x0', 'x1'], 'u_rep2.d[3]': ['x0', 'x1'],
        'u_sext.d[7]': ['sx'], 'u_sext2.d[7]': ['sx'],
        'u_cast.d': ['x0', 'x1'],
        'u_strm.d[0]': ['x0', 'x1'], 'u_strm.d[7]': ['x0', 'x1'],
        'u_strm4.d[0]': ['x0', 'x1'], 'u_strm4.d[7]': ['x0', 'x1'],
        'u_cond.d[0]': ['x0', 'x1'],
    },
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rtl-trace', required=True, type=Path)
    parser.add_argument('--source-dir', required=True, type=Path)
    args = parser.parse_args()
    binary = args.rtl_trace.resolve()
    fixtures = args.source_dir.resolve() / 'tests/fixtures/reviewer_round5_6'
    count = fallback_count = 0
    with tempfile.TemporaryDirectory(prefix='reviewer_round5_6_') as temporary:
        root = Path(temporary)
        for source in sorted(fixtures.glob('*.sv')):
            top = source.stem
            labels = {}
            checks = []
            for line, text in enumerate(source.read_text().splitlines(), 1):
                label = re.search(r'// L (\S+)', text)
                if label:
                    labels[line] = label.group(1)
                check = re.match(r'\s*// Q (drivers|loads) (\S+) =>\s*(.*)$', text)
                if check and check[2] not in EXCLUDED.get(top, set()):
                    checks.append((check[1], check[2], sorted(check[3].split())))
            assert checks, source
            dbs = []
            for canonical in ('0', '1'):
                db = root / f'{top}{canonical}.db'
                dbs.append(db)
                compiled = run(binary, ['compile', '--db', db, '--single-unit', source, '--top', top],
                               {'RTL_TRACE_CANONICAL_BODIES': canonical,
                                'RTL_TRACE_CANONICAL_VERIFY': '1', 'RTL_TRACE_BODY_CACHE': '1'})
                if canonical == '1':
                    assert 'mismatched_lists=0' in compiled.stdout, compiled.stdout
                for mode, target, expected in checks:
                    body = json.loads(run(binary, ['trace', '--db', db, '--mode', mode,
                                                   '--signal', top + '.' + target,
                                                   '--format', 'json']).stdout)
                    assert not body['diagnostics'], (top, target, body)
                    # The accepted unused-input terminal can accompany a writer.
                    # Compare the fixture's named assignment sites separately.
                    got = sorted(labels[e['line']] for e in body['endpoints']
                                 if e['kind'] == 'expr' and e['line'] in labels)
                    fallback = target in FALLBACK.get(top, {})
                    expected = FALLBACK[top][target] if fallback else expected
                    assert got == expected, (top, mode, target, expected, body)
                    unresolved = [s for s in body['stops']
                                  if s['reason'] == 'unresolved_connection_mapping']
                    if fallback:
                        assert unresolved, (top, target, body)
                        assert all('connection ' in s['detail'] and '.sv:' in s['detail']
                                   for s in unresolved), body
                        assert any(e['kind'] == 'port' and e['bit_map_approximate']
                                   for e in body['endpoints']), body
                        assert all(e['bit_map_approximate'] for e in body['endpoints']), body
                        fallback_count += 1
                    else:
                        assert not unresolved, (top, target, body)
                        assert all(not e['bit_map_approximate'] for e in body['endpoints']), body
                    count += 1
            assert dbs[0].read_bytes() == dbs[1].read_bytes(), top
            for db in dbs:
                db.unlink()
                Path(str(db) + '.meta').unlink()
    print(f'PASS: {count} reviewer fixture queries ({fallback_count} fallback); '
          '7 fixtures, canonical identity and VERIFY')


if __name__ == '__main__':
    main()
