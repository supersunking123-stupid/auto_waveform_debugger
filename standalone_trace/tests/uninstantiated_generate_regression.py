"""Inactive generate endpoint/reference removal and incremental epoch checks."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from endpoint_dedup_regression import read_db


def run(binary, args, env=None, expected=0):
    p = subprocess.run([str(binary), *map(str, args)], env={**os.environ, **(env or {})},
                       capture_output=True, text=True, timeout=60)
    assert p.returncode == expected, (args, p.returncode, p.stdout, p.stderr)
    return p


def compile_db(binary, source, db, env=None, incremental=False):
    return run(binary, ['compile', '--db', db, '--single-unit', source, '--top', 'inactive_top',
                        *(['--incremental'] if incremental else [])], env)


def trace(binary, db, signal, mode):
    return json.loads(run(binary, ['trace', '--db', db, '--mode', mode,
                                  '--signal', signal, '--format', 'json']).stdout)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rtl-trace', type=Path, required=True)
    ap.add_argument('--source-dir', type=Path, required=True)
    ap.add_argument('--parent-bin', type=Path)
    args = ap.parse_args()
    binary = args.rtl_trace.resolve()
    source = args.source_dir.resolve() / 'tests/fixtures/canonical_bodies/uninstantiated_generate.sv'
    labels = {line.split('// CASE ', 1)[1].strip(): i for i, line in enumerate(source.read_text().splitlines(), 1)
              if '// CASE ' in line}
    owners = ['inactive_top.u_on0', 'inactive_top.u_on1', 'inactive_top.u_off0', 'inactive_top.u_off1']
    with tempfile.TemporaryDirectory(prefix='rtl_inactive_generate_') as directory:
        root = Path(directory)
        db = root / 'on.db'
        result = compile_db(binary, source, db, {'RTL_TRACE_CANONICAL_STATS': '1'})
        redirect = re.search(r'\[Canon\] tracer signals .*redirected=(\d+)', result.stdout)
        assert redirect and int(redirect.group(1)) > 0, result.stdout
        on = read_db(db)

        for owner in owners:
            enabled = '.u_on' in owner
            absent = ['case_default', 'nested_inner_off', 'nested_outer_off',
                      'case_zero' if enabled else 'case_one',
                      'disabled_class_active' if enabled else 'outer_on',
                      'outer_off' if enabled else 'enabled_child',
                      'unnamed_off' if enabled else 'unnamed_on']
            for signal, mode in ((owner+'.q', 'drivers'), (owner+'.a', 'loads')):
                endpoints = trace(binary, db, signal, mode)['endpoints']
                lines = {e['line'] for e in endpoints}
                assert not lines.intersection(labels[label] for label in absent), (signal, mode, endpoints)
                assert labels['procedural_dead_arm_retained'] in lines
                same_line = [e for e in endpoints if e['line'] == labels['same_line']]
                assert {e['bit_map'] for e in same_line} == {'[0]', '[2]'}, same_line
                loop = [e for e in endpoints if e['line'] == labels['loop']]
                assert {e['bit_map'] for e in loop} == ({'[6:5]'} if enabled else set()), (signal, loop)
                active = ['case_one', 'outer_on', 'unnamed_on'] if enabled else ['case_zero', 'outer_off', 'disabled_class_active', 'unnamed_off']
                assert all(labels[label] in lines for label in active), (signal, mode, endpoints)
        for name in on['strings']:
            assert not any(part in name for part in ('.g_always_off.', '.g_same_line.')), name
        for owner in owners[2:]:
            assert not any(name.startswith(owner+'.g_enabled.') for name in on['lists']), owner
        for refs in on['reverse']:
            assert not any('.g_always_off.' in a or '.g_always_off.' in b for a, b in refs)
        print('PASS: disabled if/case/zero-trip/nested/unnamed branches drop both endpoint directions; active siblings/procedural arms survive')

        off = root/'off.db'
        compile_db(binary, source, off, {'RTL_TRACE_CANONICAL_BODIES':'0'})
        assert off.read_bytes() == db.read_bytes()
        assert Path(str(off)+'.meta').read_bytes() == Path(str(db)+'.meta').read_bytes()
        verified = compile_db(binary, source, root/'verify.db', {'RTL_TRACE_CANONICAL_VERIFY':'1'})
        assert 'mismatched_lists=0' in verified.stdout
        assert 'SEMANTICS_EPOCH:12\n' in Path(str(db)+'.meta').read_text()
        print('PASS: two copies of each parameter class redirect canonically; canonical on/off bytes and VERIFY lists match')

        if args.parent_bin:
            inherited = root/'parent.db'
            compile_db(args.parent_bin.resolve(), source, inherited)
            old_epoch = int(re.search(r'SEMANTICS_EPOCH:(\d+)', Path(str(inherited)+'.meta').read_text()).group(1))
            assert old_epoch < 12, old_epoch
            rebuilt = compile_db(binary, source, inherited, incremental=True)
            assert 'incremental-cache-hit' not in rebuilt.stdout
            assert inherited.read_bytes() == db.read_bytes()
            hit = compile_db(binary, source, inherited, incremental=True)
            assert 'incremental-cache-hit' in hit.stdout
            print(f'PASS: real epoch {old_epoch} parent forces epoch 12 rebuild then cache hit')
        else:
            print('SKIP: actual pre-epoch-12 parent rebuild comparison (--parent-bin missing)')


if __name__ == '__main__':
    main()
