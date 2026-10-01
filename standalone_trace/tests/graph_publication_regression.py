"""Publication boundaries, guardian death handling and supported legacy leases."""
import argparse
import fcntl
import os
import signal
import subprocess
import tempfile
import time
from pathlib import Path
from graph_db_mapping_regression import SOURCE, Serve, compile_db, query, run


def wait_for(predicate, timeout=10):
    deadline = time.monotonic() + timeout
    while not predicate():
        assert time.monotonic() < deadline, 'condition deadline exceeded'
        time.sleep(0.01)


def stopped(process):
    status = Path(f'/proc/{process.pid}/status').read_text()
    return '\nState:\tT' in status


def children(process):
    return [int(p) for p in Path(f'/proc/{process.pid}/task/{process.pid}/children').read_text().split()]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--rtl-trace', type=Path, required=True)
    parser.add_argument('--reference-bin', type=Path)
    args = parser.parse_args()
    binary = args.rtl_trace.resolve()
    with tempfile.TemporaryDirectory(prefix='rtl_trace_publication_') as name:
        root = Path(name)
        a, b = root/'a.sv', root/'b.sv'
        a.write_text(SOURCE)
        b.write_text(SOURCE.replace('logic [7:0] mid;', 'logic [7:0] mid; logic changed_marker;'))
        oracle = {}
        for source in (a, b):
            db = root/(source.stem+'.db')
            compile_db(binary, db, source)
            oracle[source] = (db.read_bytes(), Path(str(db)+'.meta').read_bytes())
        destination = root/'target.db'
        metadata = Path(str(destination)+'.meta')

        compile_db(binary, destination, a)
        Path(str(destination)+'.lock').unlink()
        upgraded = run(binary, ['compile', '--db', destination, '--single-unit', a,
                               '--top', 'mapped_top', '--incremental'])
        assert b'incremental-cache-hit' not in upgraded.stdout
        assert Path(str(destination)+'.lock').exists()
        assert (destination.read_bytes(), metadata.read_bytes()) == oracle[a]
        print('PASS: legacy DB without sidecar upgrades under an exclusive lock instead of an unlocked cache hit')

        def publish(source, stage=None, group=False):
            env = dict(os.environ)
            if stage: env['RTL_TRACE_TEST_PUBLISH_STOP'] = stage
            return subprocess.Popen([str(binary), 'compile', '--db', str(destination),
                                     '--single-unit', str(source), '--top', 'mapped_top'],
                                    env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    start_new_session=group)

        def finish(process, expected=0):
            out, err = process.communicate(timeout=20)
            assert process.returncode == expected, (process.returncode, out, err)

        for stage in ('prepared', 'db_closed', 'meta_closed', 'meta_invalidated', 'db_published', 'meta_published'):
            compile_db(binary, destination, a)
            process = publish(b, stage)
            wait_for(lambda: stopped(process))
            assert len(children(process)) == 1
            process.kill(); finish(process, -signal.SIGKILL)
            # No new operation is launched before this cleanup assertion.
            wait_for(lambda: not list(root.glob('.rtl_trace_stage_*')))
            expected_source = b if stage in ('db_published', 'meta_published') else a
            assert destination.read_bytes() == oracle[expected_source][0]
            if stage in ('meta_invalidated', 'db_published'):
                assert not metadata.exists(), stage
            else:
                assert metadata.read_bytes() == oracle[expected_source][1], stage
            rebuilt = run(binary, ['compile', '--db', destination, '--single-unit', b,
                                   '--top', 'mapped_top', '--incremental'])
            assert (destination.read_bytes(), metadata.read_bytes()) == oracle[b]
            if stage != 'meta_published': assert b'incremental-cache-hit' not in rebuilt.stdout
        print('PASS: compiler SIGKILL at six stages cleans temps immediately; DB complete and metadata never stale')

        compile_db(binary, destination, a)
        process = publish(b, 'db_published')
        wait_for(lambda: stopped(process))
        assert destination.read_bytes() == oracle[b][0] and not metadata.exists()
        incremental = subprocess.Popen([str(binary), 'compile', '--db', str(destination),
                                        '--single-unit', str(b), '--top', 'mapped_top', '--incremental'],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        reader = subprocess.Popen([str(binary), 'find', '--db', str(destination), '--query', 'changed_marker'],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        time.sleep(0.15)
        assert incremental.poll() is None
        finish(reader)
        process.send_signal(signal.SIGCONT); finish(process)
        stdout, stderr = incremental.communicate(timeout=20)
        assert incremental.returncode == 0 and b'incremental-cache-hit' in stdout, (incremental.returncode, stdout, stderr)
        print('PASS: cache check waits for metadata-last publication; readers finish while sidecar stays exclusively locked')

        # Guardian death must produce an ordinary failure, not SIGPIPE termination.
        process = publish(a, 'prepared')
        wait_for(lambda: stopped(process))
        guardian = children(process)[0]
        os.kill(guardian, signal.SIGKILL)
        process.send_signal(signal.SIGCONT); finish(process)
        assert not list(root.glob('.rtl_trace_stage_*'))
        assert (destination.read_bytes(), metadata.read_bytes()) == oracle[a]
        print('PASS: dead guardian after successful publish warns without failing, SIGPIPE, leaked lock or temporary file')

        for stage in ('prepared', 'db_closed', 'meta_closed'):
            compile_db(binary, destination, a)
            process = publish(b, stage, group=True)
            wait_for(lambda: stopped(process))
            os.killpg(process.pid, signal.SIGINT)
            process.send_signal(signal.SIGCONT)
            finish(process, -signal.SIGINT)
            wait_for(lambda: not list(root.glob('.rtl_trace_stage_*')))
            assert (destination.read_bytes(), metadata.read_bytes()) == oracle[a]
        print('PASS: process-group SIGINT at prepared/write stages kills compiler but guardian cleans all staging')
        env = {**os.environ, 'RTL_TRACE_TEST_GUARDIAN_FORK_FAILURE': '1'}
        fallback = subprocess.run([str(binary), 'compile', '--db', str(destination), '--single-unit', str(a), '--top', 'mapped_top'], env=env, capture_output=True, timeout=20)
        assert fallback.returncode == 0 and b'continuing without it' in fallback.stderr
        assert not list(root.glob('.rtl_trace_stage_*'))
        print('PASS: injected guardian fork failure continues successfully with local cleanup')

        # Killing the entire group defeats the guardian. Recovery belongs to the
        # next writable compile; never claim immediate cleanup for this case.
        process = publish(b, 'meta_closed', group=True)
        wait_for(lambda: stopped(process))
        guardian = children(process)[0]
        os.kill(guardian, signal.SIGSTOP)
        wait_for(lambda: '\nState:\tT' in Path(f'/proc/{guardian}/status').read_text())
        os.killpg(process.pid, signal.SIGKILL); finish(process, -signal.SIGKILL)
        assert list(root.glob('.rtl_trace_stage_*'))
        compile_db(binary, destination, b)
        assert not list(root.glob('.rtl_trace_stage_*'))
        assert (destination.read_bytes(), metadata.read_bytes()) == oracle[b]
        print('PASS: documented whole-group kill fallback recovers staging under the next writer lock')

        lock = Path(str(destination)+'.lock')
        lock.chmod(0o444)
        with lock.open('rb') as lease:
            fcntl.flock(lease, fcntl.LOCK_EX)
            reader = subprocess.Popen([str(binary), 'find', '--db', str(destination), '--query', 'mapped_top'],
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            finish(reader)
            fcntl.flock(lease, fcntl.LOCK_UN)
        compile_db(binary, destination, a)  # advisory EX flock also accepts readonly existing lock fd
        lock.chmod(0o000)
        query(binary, destination, ['find', '--query', 'mapped_top'])
        lock.chmod(0o644)
        print('PASS: readers ignore exclusively held and unreadable sidecars; writer supports readonly lock')

        masked = root/'masked.db'
        process = subprocess.Popen([str(binary), 'compile', '--db', str(masked),
                                    '--single-unit', str(a), '--top', 'mapped_top'],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, umask=0o027)
        finish(process)
        assert masked.stat().st_mode & 0o777 == 0o640
        assert Path(str(masked)+'.meta').stat().st_mode & 0o777 == 0o640
        assert Path(str(masked)+'.lock').stat().st_mode & 0o777 == 0o640
        destination.chmod(0o2640)
        metadata.chmod(0o2640)
        mode, group = destination.stat().st_mode & 0o7777, destination.stat().st_gid
        compile_db(binary, destination, b)
        assert destination.stat().st_mode & 0o7777 == mode and destination.stat().st_gid == group
        assert metadata.stat().st_mode & 0o7777 == mode and metadata.stat().st_gid == group
        destination.chmod(0o444)
        before = (destination.read_bytes(), metadata.read_bytes())
        process = publish(a); finish(process, 1)
        assert (destination.read_bytes(), metadata.read_bytes()) == before
        destination.chmod(mode)
        assert not list(root.glob('.rtl_trace_stage_*'))
        print('PASS: mode/group preserved and readonly destination cannot be silently replaced')

        alias = root/'alias.db'; alias.symlink_to(destination.name)
        compile_db(binary, alias, a)
        assert not Path(str(alias)+'.meta').exists()
        assert metadata.read_bytes() == oracle[a][1]
        hit = run(binary, ['compile', '--db', alias, '--single-unit', a, '--top', 'mapped_top', '--incremental'])
        assert b'incremental-cache-hit' in hit.stdout
        print('PASS: symlink aliases bind DB, metadata and lock to the canonical target')

        # A header/size envelope gives cheap corruption detection, not a checksum.
        clean_db, clean_meta = oracle[a]
        import struct
        mutations = [('truncated_db', clean_db[:-1], clean_meta),
                     ('short_header', clean_db[:100], clean_meta),
                     ('old_meta', clean_db, clean_meta.split(b'RTL_TRACE_CACHE_ENVELOPE:')[0]),
                     ('bad_meta', clean_db, b'invalid'),
                     ('large_meta', clean_db, clean_meta+b'x'*4096)]
        for label, offset, fmt, value in [('magic', 0, '<I', 0), ('version', 16, '<I', 7),
                                          ('header_count', 40, '<Q', 2**64-1)]:
            changed = bytearray(clean_db); struct.pack_into(fmt, changed, offset, value)
            mutations.append((label, changed, clean_meta))
        for label, damaged_db, damaged_meta in mutations:
            destination.write_bytes(damaged_db); metadata.write_bytes(damaged_meta)
            rebuilt = run(binary, ['compile', '--db', destination, '--single-unit', a, '--top', 'mapped_top', '--incremental'])
            assert b'incremental-cache-hit' not in rebuilt.stdout, label
            assert (destination.read_bytes(), metadata.read_bytes()) == oracle[a], label
        print('PASS: truncation, bad/incomplete header, old/malformed/oversized metadata force rebuild')

        if args.reference_bin:
            reference_db = root/'reference.db'
            compile_db(args.reference_bin, reference_db, a)
            reference_a = reference_db.read_bytes()
            compile_db(binary, destination, a)
            client = Serve(binary, destination)
            old = subprocess.Popen([str(args.reference_bin), 'compile', '--db', str(destination),
                                    '--single-unit', str(b), '--top', 'mapped_top'],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                wait_for(lambda: 'locks' in Path(f'/proc/{old.pid}/wchan').read_text())
                assert old.poll() is None
                assert b'changed_marker' not in client.query('find --query mapped_top --format json')
            finally: client.close()
            finish(old)
            assert b'changed_marker' in query(binary, destination, ['find', '--query', 'changed_marker']).stdout
            print('PASS: actual old writer blocks on the unchanged mapped inode until serve closes')

            # Reproduce the unsupported legacy lock-old-inode/reopen-new-path
            # race deterministically, with no query executing during truncation.
            compile_db(binary, destination, a)
            old_snapshot = Serve(binary, destination)
            legacy = subprocess.Popen([str(args.reference_bin), 'compile', '--db', str(destination),
                                       '--single-unit', str(a), '--top', 'mapped_top'],
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            wait_for(lambda: 'locks' in Path(f'/proc/{legacy.pid}/wchan').read_text())
            compile_db(binary, destination, b)
            new_snapshot = Serve(binary, destination)
            assert b'changed_marker' in new_snapshot.query('find --query changed_marker')
            old_snapshot.close(); finish(legacy)
            assert destination.read_bytes() == reference_a
            assert len(oracle[a][0]) != len(oracle[b][0])
            assert new_snapshot.query('find --query mapped_top') == b''
            new_snapshot.query('quit'); new_snapshot.process.stdin.close(); new_snapshot.process.wait(timeout=5)
            assert new_snapshot.process.returncode == 0
            assert b'mapped file size changed' in new_snapshot.process.stderr.read()
            print('PASS: adversarial old-waiter rename race reproduced; completed rewrite fails closed, concurrent rewrite remains unsupported')

        # A malicious writer that ignores locks is unsupported. Detect a size
        # change already completed before the next command and drop the session.
        compile_db(binary, destination, a)
        client = Serve(binary, destination)
        with destination.open('wb') as truncated: truncated.write(b'bad')
        assert client.query('find --query mapped_top') == b''
        client.query('quit'); client.process.stdin.close(); client.process.wait(timeout=5)
        assert client.process.returncode == 0
        assert b'mapped file size changed' in client.process.stderr.read()
        print('PASS: completed uncooperative truncation fails closed at the command boundary (mid-query race unsupported)')


if __name__ == '__main__': main()
