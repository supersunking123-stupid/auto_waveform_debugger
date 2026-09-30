#!/usr/bin/env python3
"""E4d experiment: exact endpoint removal, ref preservation, and stop changes."""
import argparse
import collections
import contextlib
import hashlib
import json
import os
import re
import struct
import subprocess
import tempfile
from pathlib import Path

HEADER = struct.Struct('<16sII15Q')
SIGNAL = struct.Struct('<8I')
ENDPOINT = struct.Struct('<11I4B')
NONE = 0xffffffff


def run(binary, args, env=None):
    process = subprocess.run([str(binary), *map(str, args)], capture_output=True, text=True,
                             timeout=45, env={**os.environ, **(env or {})})
    assert process.returncode == 0, (args, process.returncode, process.stdout, process.stderr)
    return process


def compile_db(binary, source, db, top='endpoint_duplicates', env=None, incremental=False):
    args = ['compile', '--db', db, '--single-unit', source, '--top', top]
    if incremental:
        args.append('--incremental')
    return run(binary, args, env)


def trace(binary, db, signal, mode='loads', extra=(), fmt='json'):
    return run(binary, ['trace', '--db', db, '--mode', mode, '--signal', signal,
                        '--format', fmt, *extra]).stdout


def read_db(path):
    """Decode v5 lists/ref sets without discarding ref order or endpoint flags."""
    data = path.read_bytes()
    header = HEADER.unpack_from(data)
    assert header[1] == 5
    counts = header[3:]
    offset = HEADER.size
    string_offsets = struct.unpack_from('<' + 'I' * (counts[0] + 1), data, offset)
    offset += 4 * (counts[0] + 1)
    strings = [data[offset + string_offsets[i]:offset + string_offsets[i + 1]].decode()
               for i in range(counts[0])]
    offset += counts[1]
    signal_start = offset

    def records(fmt, count):
        nonlocal offset
        shape = struct.Struct(fmt)
        result = [shape.unpack_from(data, offset + i * shape.size) for i in range(count)]
        offset += shape.size * count
        return result

    signals = records('<8I', counts[2])
    endpoints = records('<11I4B', counts[3])
    refs = [r[0] for r in records('<I', counts[4])]
    after_refs = offset
    reverse = []
    reverse_ordered = []
    for range_count, flat_count in ((counts[5], counts[6]), (counts[7], counts[8]),
                                  (counts[9], counts[10])):
        ranges = records('<3I', range_count)
        flat = [r[0] for r in records('<I', flat_count)]
        reverse.append({(strings[path_id], strings[signals[sig_id][0]])
                        for path_id, begin, count in ranges for sig_id in flat[begin:begin + count]})
        reverse_ordered.append({strings[path_id]: [strings[signals[i][0]] for i in flat[begin:begin + count]]
                                for path_id, begin, count in ranges})
    hierarchy = records('<6I', counts[11])
    children = records('<I', counts[12])
    # GraphGlobalNetRecord contains four uint32 fields (source/category/begin/count).
    globals_raw = records('<4I', counts[13])
    sinks = [r[0] for r in records('<I', counts[14])]
    globals_ = {strings[source]: (strings[category], tuple(strings[i] for i in sinks[begin:begin + count]))
                for source, category, begin, count in globals_raw}
    suffix = data[offset:]  # unchanged hierarchy-parameter sections

    def key(e):
        # Ref offsets differ after emission. Preserve their exact ordered slices.
        return (*e[:7], *e[11:], tuple(refs[e[7]:e[7] + e[8]]), tuple(refs[e[9]:e[9] + e[10]]))

    lists = {strings[s[0]]: ([key(e) for e in endpoints[s[1]:s[1] + s[2]]],
                            [key(e) for e in endpoints[s[3]:s[3] + s[4]]]) for s in signals}
    return {'header': header, 'strings': strings, 'signals': signals, 'endpoints': endpoints,
            'lists': lists, 'reverse': reverse, 'globals': globals_, 'hierarchy': hierarchy,
            'children': children, 'suffix': suffix, 'size': len(data), 'raw': data,
            'keys': [key(e) for e in endpoints], 'signal_start': signal_start, 'after_refs': after_refs,
            'reverse_ordered': reverse_ordered}


def assert_only_duplicates_removed(parent, candidate):
    assert parent['strings'] == candidate['strings'], 'unexpected string ID/order change'
    assert parent['lists'].keys() == candidate['lists'].keys()
    assert parent['reverse'] == candidate['reverse'], 'reverse-ref sets changed'
    for field in ('globals', 'hierarchy', 'children', 'suffix'):
        assert parent[field] == candidate[field], field
    removed = [0, 0]
    kept = [False] * len(parent['endpoints'])
    owners = [0] * len(kept)
    for old_signal, new_signal in zip(parent['signals'], candidate['signals']):
        assert (old_signal[0], old_signal[5:]) == (new_signal[0], new_signal[5:]), 'signal metadata changed'
        for direction, begin_field in enumerate((1, 3)):
            old_begin, old_count = old_signal[begin_field:begin_field + 2]
            new_begin, new_count = new_signal[begin_field:begin_field + 2]
            old = parent['keys'][old_begin:old_begin + old_count]
            new = candidate['keys'][new_begin:new_begin + new_count]
            signal = parent['strings'][old_signal[0]]
            # In-memory equality is stricter than serialization (IDs vs text refs
            # are distinct before emission). Permit kept serialized duplicates;
            # every removed record must equal a previously retained record.
            retained = set()
            pos = 0
            for local_index, entry in enumerate(old):
                index = old_begin + local_index
                owners[index] += 1
                if pos < len(new) and entry == new[pos]:
                    retained.add(entry)
                    kept[index] = True
                    pos += 1
                else:
                    assert entry in retained, (signal, direction, 'lost first/unique endpoint', entry)
                    removed[direction] += 1
            assert pos == len(new), (signal, direction, 'new/reordered endpoint')
    assert all(count == 1 for count in owners), 'endpoint ownership/layout changed'
    prefix = [0]
    for keep in kept:
        prefix.append(prefix[-1] + int(keep))
    expected_signals = []
    for record in parent['signals']:
        s = list(record)
        for field in (1, 3):
            begin, count = record[field:field + 2]
            s[field] = prefix[begin]
            s[field + 1] = prefix[begin + count] - prefix[begin]
        expected_signals.append(s)
    expected_endpoints = []
    expected_refs = []
    for index, keep in enumerate(kept):
        if not keep:
            continue
        e = list(parent['endpoints'][index])
        lhs, rhs = parent['keys'][index][-2:]
        e[7] = len(expected_refs)
        expected_refs.extend(lhs)
        e[9] = len(expected_refs)
        expected_refs.extend(rhs)
        expected_endpoints.append(e)
    header = list(parent['header'])
    header[6] = len(expected_endpoints)
    header[7] = len(expected_refs)
    expected = HEADER.pack(*header) + parent['raw'][HEADER.size:parent['signal_start']]
    expected += b''.join(SIGNAL.pack(*s) for s in expected_signals)
    expected += b''.join(ENDPOINT.pack(*e) for e in expected_endpoints)
    expected += struct.pack('<' + 'I' * len(expected_refs), *expected_refs)
    expected += parent['raw'][parent['after_refs']:]
    assert expected == candidate['raw'], 'unexplained bytes beyond duplicate records/ref slices/ranges/counts'
    return removed


def write_synthetic(db, duplicates):
    """Small valid v5 graph: repeated logic/port routes with no frontend ambiguity."""
    names = ['dedup.root', 'dedup.b', 'dedup.c', 'dedup.d', 'dedup.port', 'dedup.child']
    strings = names + ['fixture.sv', 'input']
    refs = []
    endpoints = []
    signal_records = []
    route = {'dedup.root': [('dedup.root', 'dedup.b', 10, 0),
                            ('dedup.root', 'dedup.c', 10, 0)],
             'dedup.c': [('dedup.c', 'dedup.d', 22, 0)],
             'dedup.port': [('dedup.child', None, 30, 1)],
             'dedup.child': [('dedup.child', 'dedup.d', 31, 0)]}
    if duplicates:
        route['dedup.root'].insert(1, route['dedup.root'][0])
        route['dedup.port'].append(route['dedup.port'][0])
    for name in names:
        starts = []
        for _ in ('drivers', 'loads'):
            begin = len(endpoints)
            for path, target, line, kind in route.get(name, []):
                lhs = len(refs)
                if target is not None:
                    refs.append(strings.index(target))
                rhs = len(refs)
                if target is not None:
                    refs.append(strings.index(target))
                count = int(target is not None)
                endpoints.append((strings.index(path), 6, 7 if kind else NONE, NONE,
                                  line, 0, 0, lhs, count, rhs, count, kind, 0, 0, 0))
            starts += [begin, len(endpoints) - begin]
        signal_records.append((strings.index(name), *starts, NONE, 0, 0))
    encoded = [s.encode() for s in strings]
    offsets = [0]
    for item in encoded:
        offsets.append(offsets[-1] + len(item))
    counts = [len(strings), offsets[-1], len(names), len(endpoints), len(refs)] + [0] * 10
    data = HEADER.pack(b'RTL_TRACE_GDB_1\0', 5, 0, *counts)
    data += struct.pack('<' + 'I' * len(offsets), *offsets) + b''.join(encoded)
    data += b''.join(SIGNAL.pack(*s) for s in signal_records)
    data += b''.join(ENDPOINT.pack(*e) for e in endpoints)
    data += struct.pack('<' + 'I' * len(refs), *refs) + struct.pack('<QQ', 0, 0)
    db.write_bytes(data)


def stop_keys(payload):
    return [(s['signal'], s['reason'], s['detail'], s['depth']) for s in payload['stops']]


def ordered_subsequence(after, before):
    iterator = iter(before)
    return all(any(item == wanted for item in iterator) for wanted in after)


def replay_unselected_load_routes(db, root_signal, extra):
    """Independent ordered route proof for this fixture's unselected load query.

    No selector, global-net, fallback-driver or output-clipping policy is inferred.
    Record every attempted edge and first stop key, including suppressed depths.
    """
    def option(name, default):
        return extra[extra.index(name) + 1] if name in extra else default
    cone = int(option('--cone-level', 1))
    depth_limit = int(option('--depth', 8))
    max_nodes = int(option('--max-nodes', 5000))
    include = option('--include', None)
    exclude = option('--exclude', None)
    stop_at = option('--stop-at', None)
    by_name = {db['strings'][s[0]]: s for s in db['signals']}
    visited, seen_stops, stops, events = set(), set(), [], []
    cap_hit = False

    def stop(signal, reason, detail, depth):
        key = (signal, reason, detail)  # Actual stop_once omits depth.
        recorded = key not in seen_stops
        events.append({'event': 'stop', 'key': key, 'depth': depth, 'recorded': recorded})
        if recorded:
            seen_stops.add(key)
            stops.append((*key, depth))

    def walk(signal, depth, cone_depth):
        nonlocal cap_hit
        events.append({'event': 'attempt', 'signal': signal, 'depth': depth, 'cone_depth': cone_depth})
        if depth > depth_limit:
            stop(signal, 'depth_limit', 'max-depth-reached', depth); return
        if cap_hit:
            return
        if len(visited) >= max_nodes:
            cap_hit = True
            stop(signal, 'node_limit', 'max-nodes-reached', depth); return
        if signal in visited:
            stop(signal, 'cycle', 'already-visited', depth); return
        visited.add(signal)
        if stop_at is not None and re.search(stop_at, signal):
            stop(signal, 'stop_at', 'matched-stop-at-regex', depth); return
        s = by_name[signal]
        for index in range(s[3], s[3] + s[4]):
            e = db['endpoints'][index]
            path = db['strings'][e[0]]
            allowed = (include is None or re.search(include, path)) and not (exclude and re.search(exclude, path))
            lhs = [db['strings'][i] for i in db['keys'][index][-2]]
            rhs = db['keys'][index][-1]
            events.append({'event': 'edge', 'from': signal, 'path': path, 'line': e[4], 'kind': e[11],
                           'depth': depth, 'cone_depth': cone_depth, 'lhs': lhs,
                           'full_serialized_key_sha256': hashlib.sha256(repr(db['keys'][index]).encode()).hexdigest()})
            if e[11] != 1:
                if not allowed:
                    stop(path, 'filtered', 'expr-filtered', depth); continue
                if cone_depth + 1 < cone:
                    if lhs:
                        for target in lhs:
                            if target != signal:
                                walk(target, depth + 1, cone_depth + 1)
                    else:
                        expanded = False
                        if '--prefer-port-hop' in extra:
                            if path != signal and path in by_name:
                                walk(path, depth + 1, cone_depth + 1); expanded = True
                            for target in db['reverse_ordered'][1].get(path, []):
                                if target != signal:
                                    walk(target, depth + 1, cone_depth + 1); expanded = True
                        if not expanded:
                            stop(path, 'cone_limit', 'no-expandable-assignment-context', depth)
                elif lhs or rhs:
                    stop(path, 'cone_limit', 'max-cone-level-reached', depth)
            else:
                expanded = False
                if path != signal and path in by_name:
                    walk(path, depth + 1, cone_depth); expanded = True
                for target in db['reverse_ordered'][1].get(path, []):
                    if target != signal:
                        walk(target, depth + 1, cone_depth); expanded = True
                if not expanded and not allowed:
                    stop(path, 'filtered', 'port-filtered', depth)
    walk(root_signal, 0, 0)
    return {'stops': stops, 'visited': len(visited), 'events': events}


def synthetic_stop_checks(binary, root, evidence):
    before = root / 'synthetic_duplicates.db'
    after = root / 'synthetic_unique.db'
    write_synthetic(before, True)
    write_synthetic(after, False)
    differences = []
    variants = [(), ('--cone-level', '2'), ('--cone-level', '3')]
    variants += [('--cone-level', '3', '--depth', str(n)) for n in (0, 1, 2)]
    variants += [('--cone-level', '3', '--max-nodes', str(n)) for n in (1, 2, 3)]
    variants += [('--cone-level', '3', flag, pattern) for flag, pattern in (
        ('--include', 'dedup'), ('--exclude', 'dedup.root'), ('--stop-at', 'dedup.b'))]
    variants.append(('--cone-level', '3', '--prefer-port-hop'))
    for signal in ('dedup.root', 'dedup.port'):
        for mode in ('drivers', 'loads'):
            for extra in variants:
                old = json.loads(trace(binary, before, signal, mode, extra))
                new = json.loads(trace(binary, after, signal, mode, extra))
                assert old['endpoints'] == new['endpoints'], (signal, mode, extra, old, new)
                assert old['summary']['visited'] == new['summary']['visited'], (signal, mode, extra)
                bounded = '--max-nodes' in extra
                if not bounded:
                    old_stops = collections.Counter(stop_keys(old))
                    new_stops = collections.Counter(stop_keys(new))
                    assert not (new_stops - old_stops), (signal, mode, extra, old, new)
                    assert ordered_subsequence(stop_keys(new), stop_keys(old)), (signal, mode, extra)
                    assert all(reason in {'cycle', 'depth_limit', 'cone_limit', 'filtered'}
                               for _, reason, _, _ in (old_stops - new_stops)), extra
                if old != new:
                    differences.append({'signal': signal, 'mode': mode, 'args': list(extra),
                                        'classification': 'BOUNDED_STOP_CHANGE' if bounded
                                        else 'REDUNDANT_TRAVERSAL_REMOVED', 'before': old, 'after': new})
    # Ordered routes prove the bound behavior: root -> b consumes node2;
    # a repeated b attempt fires the parent node cap before its visited check.
    node_routes = {
        ('dedup.root', 1): ([('dedup.b', 'node_limit')], [('dedup.b', 'node_limit')]),
        ('dedup.root', 2): ([('dedup.b', 'node_limit')], [('dedup.c', 'node_limit')]),
        ('dedup.root', 3): ([('dedup.b', 'cycle'), ('dedup.d', 'node_limit')],
                          [('dedup.d', 'node_limit')]),
        ('dedup.port', 1): ([('dedup.child', 'node_limit')], [('dedup.child', 'node_limit')]),
        ('dedup.port', 2): ([('dedup.d', 'node_limit')], [('dedup.d', 'node_limit')]),
        ('dedup.port', 3): ([('dedup.child', 'node_limit')], []),
    }
    for (signal, cap), expected in node_routes.items():
        for mode in ('drivers', 'loads'):
            extra = ('--cone-level', '3', '--max-nodes', str(cap))
            old = json.loads(trace(binary, before, signal, mode, extra))
            new = json.loads(trace(binary, after, signal, mode, extra))
            assert [(s['signal'], s['reason']) for s in old['stops']] == expected[0], (signal, cap, old)
            assert [(s['signal'], s['reason']) for s in new['stops']] == expected[1], (signal, cap, new)
    old = json.loads(trace(binary, before, 'dedup.port', extra=('--cone-level', '3')))
    new = json.loads(trace(binary, after, 'dedup.port', extra=('--cone-level', '3')))
    assert any(s['reason'] == 'cycle' for s in old['stops']) and not new['stops']
    assert differences, 'synthetic cases did not expose repeated traversal differences'
    (evidence / 'synthetic_query_differences.json').write_text(json.dumps(differences, indent=2) + '\n')


def real_queries():
    roots = [('loads', 'endpoint_duplicates.a'), ('loads', 'endpoint_duplicates.u0.a'),
             ('loads', 'endpoint_duplicates.u0.vec[1]'),
             ('loads', 'endpoint_duplicates.u0.matrix[1][0]'),
             ('drivers', 'endpoint_duplicates.tail0'), ('drivers', 'endpoint_duplicates.tail1')]
    extras = [(), ('--cone-level', '2'), ('--cone-level', '3'),
              ('--cone-level', '3', '--depth', '1'),
              ('--cone-level', '3', '--max-nodes', '2'),
              ('--cone-level', '3', '--prefer-port-hop'),
              ('--cone-level', '3', '--include', 'endpoint_duplicates'),
              ('--cone-level', '3', '--exclude', 'u0'),
              ('--cone-level', '3', '--stop-at', 'u0')]
    return [(mode, signal, extra) for mode, signal in roots for extra in extras]


def parent_checks(binary, parent, source, root, evidence, candidate_db):
    old_db = root / 'parent.db'
    compile_db(parent, source, old_db)
    old = read_db(old_db)
    new = read_db(candidate_db)
    removed = assert_only_duplicates_removed(old, new)
    assert sum(removed) > 0, 'duplicate RTL fixture removed no endpoints'
    labels = {line.split('// CASE ', 1)[1].strip(): n
              for n, line in enumerate(source.read_text().splitlines(), 1) if '// CASE ' in line}
    for label in ('repeated_plain', 'repeated_symbolic', 'repeated_multidim'):
        before_count = sum(e[4] == labels[label] for e in old['endpoints'])
        after_count = sum(e[4] == labels[label] for e in new['endpoints'])
        assert before_count > after_count > 0, (label, before_count, after_count)
    diffs = []
    for mode, signal, extra in real_queries():
        for fmt in ('json', 'text'):
            old_output = trace(parent, old_db, signal, mode, extra, fmt)
            assert trace(binary, old_db, signal, mode, extra, fmt) == old_output, 'old DB output changed'
        before = json.loads(trace(parent, old_db, signal, mode, extra))
        after = json.loads(trace(binary, candidate_db, signal, mode, extra))
        # Stable first occurrence keeps unique routes. On this bounded fixture,
        # only stop occurrences/first node-cap target may differ.
        assert before['endpoints'] == after['endpoints'], (mode, signal, extra, before, after)
        assert before['summary']['visited'] == after['summary']['visited']
        bounded = '--max-nodes' in extra
        shifted_cycle = False
        if not bounded:
            added = collections.Counter(stop_keys(after)) - collections.Counter(stop_keys(before))
            if added:
                shifted_cycle = True
                # Removing the earliest redundant u2 connection route changes
                # where the same first-recorded cycle key gets its depth.
                assert mode == 'loads' and signal == 'endpoint_duplicates.a'
                assert '--cone-level' in extra and extra[extra.index('--cone-level') + 1] == '3'
                assert list(added) == [('endpoint_duplicates.u2.q', 'cycle', 'already-visited', 2)]
                assert ('endpoint_duplicates.u2.q', 'cycle', 'already-visited', 1) in stop_keys(before)
                routes = [replay_unselected_load_routes(sections, signal, extra) for sections in (old, new)]
                for route, payload in zip(routes, (before, after)):
                    assert route['stops'] == stop_keys(payload), (mode, signal, extra, route, payload)
                    assert route['visited'] == payload['summary']['visited']
                key = 'route_' + hashlib.sha256(repr((mode, signal, extra)).encode()).hexdigest()[:16]
                (evidence / (key + '.json')).write_text(json.dumps({'query': [mode, signal, *extra],
                    'parent_db_sha256': hashlib.sha256(old['raw']).hexdigest(),
                    'candidate_db_sha256': hashlib.sha256(new['raw']).hexdigest(),
                    'before': routes[0], 'after': routes[1]}, indent=2) + '\n')
            else:
                assert ordered_subsequence(stop_keys(after), stop_keys(before)), (mode, signal, extra)
        diffs.append({'mode': mode, 'signal': signal, 'args': list(extra),
                      'classification': 'IDENTICAL' if before == after else
                      'FIRST_CYCLE_DEPTH_CHANGED' if shifted_cycle else
                      'BOUNDED_STOP_CHANGE' if bounded else 'REDUNDANT_TRAVERSAL_REMOVED',
                      'before': before, 'after': after})
    (evidence / 'rtl_query_differences.json').write_text(json.dumps(diffs, indent=2) + '\n')
    for top in ('endpoint_duplicates', 'endpoint_duplicate_global'):
        inherited = root / (top + '_epoch.db')
        clean = root / (top + '_clean.db')
        compile_db(parent, source, inherited, top)
        before_meta = Path(str(inherited) + '.meta').read_text()
        assert 'SEMANTICS_EPOCH:4\n' in before_meta
        old_sections = read_db(inherited)
        rebuilt = compile_db(binary, source, inherited, top, incremental=True)
        assert 'incremental-cache-hit' not in rebuilt.stdout
        assert 'incremental cache hit' not in rebuilt.stderr
        compile_db(binary, source, clean, top)
        before_hit = tuple(Path(str(inherited) + suffix).read_bytes() for suffix in ('', '.meta'))
        assert before_hit == tuple(Path(str(clean) + suffix).read_bytes() for suffix in ('', '.meta'))
        assert before_meta.replace('SEMANTICS_EPOCH:4', 'SEMANTICS_EPOCH:5') == before_hit[1].decode()
        assert_only_duplicates_removed(old_sections, read_db(inherited))
        hit = compile_db(binary, source, inherited, top, incremental=True)
        assert 'signals: incremental-cache-hit' in hit.stdout
        assert before_hit == tuple(Path(str(inherited) + suffix).read_bytes() for suffix in ('', '.meta'))
    report = {'removed_drivers': removed[0], 'removed_loads': removed[1],
              'parent_bytes': old['size'], 'candidate_bytes': new['size'],
              'reverse_sets_equal': True, 'strings_equal': True, 'epoch_4_to_5_rebuild_then_hit': True}
    (evidence / 'parent_comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS parent full-key removal/ref sets, old DB exact queries, epoch4 -> 5 rebuild/clean/hit', report)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--rtl-trace', type=Path, required=True)
    parser.add_argument('--source-dir', type=Path, required=True)
    parser.add_argument('--parent-bin', type=Path)
    parser.add_argument('--output-dir', type=Path)
    args = parser.parse_args()
    binary = args.rtl_trace.resolve()
    source = args.source_dir.resolve() / 'tests/fixtures/endpoint_duplicates.sv'
    manager = contextlib.nullcontext(str(args.output_dir)) if args.output_dir else tempfile.TemporaryDirectory(prefix='rtl_trace_dedup_')
    with manager as directory:
        root = Path(directory)
        root.mkdir(parents=True, exist_ok=True)
        db = root / 'candidate.db'
        proc = compile_db(binary, source, db, env={'RTL_TRACE_SAVE_GRAPH_PROFILE': '1'})
        removed = re.findall(r'dedup_(?:drivers|loads)_removed=(\d+)', proc.stderr)
        assert len(removed) == 2 and sum(map(int, removed)) > 0, proc.stderr
        off = root / 'off.db'
        compile_db(binary, source, off, env={'RTL_TRACE_CANONICAL_BODIES': '0'})
        for suffix in ('', '.meta'):
            assert Path(str(db) + suffix).read_bytes() == Path(str(off) + suffix).read_bytes()
        verified = compile_db(binary, source, root / 'verified.db', env={'RTL_TRACE_CANONICAL_VERIFY': '1'})
        assert 'mismatched_lists=0' in verified.stdout
        global_db = root / 'global.db'
        compile_db(binary, source, global_db, 'endpoint_duplicate_global')
        global_sections = read_db(global_db)
        assert 'endpoint_duplicate_global.clk' in global_sections['globals'], 'compaction moved after dedup'
        category, sinks = global_sections['globals']['endpoint_duplicate_global.clk']
        assert category == 'clock' and len(sinks) == 512
        synthetic_stop_checks(binary, root, root)
        print('PASS positive RTL dedup, canonical on/off, VERIFY, raw global threshold, cycle/node/depth/filter tests')
        if args.parent_bin:
            parent_checks(binary, args.parent_bin.resolve(), source, root, root, db)


if __name__ == '__main__':
    main()
