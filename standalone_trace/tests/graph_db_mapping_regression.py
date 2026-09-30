#!/usr/bin/env python3
"""Exercise immutable DB snapshots, compatibility, and bounds before mapped access."""
import argparse
import os
import select
import struct
import subprocess
import tempfile
import time
from pathlib import Path

HEADER = struct.Struct('<16sII15Q')
SOURCE = '''
module mapped_leaf #(parameter W = 8)(input logic clk, input logic [W-1:0] i,
                                      output logic [W-1:0] o);
  always_ff @(posedge clk) o <= i;
endmodule
module mapped_top(input logic clk, input logic [7:0] a, output logic [7:0] y);
  logic [7:0] mid;
  assign mid = a;
  mapped_leaf u_leaf(.clk(clk), .i(mid), .o(y));
endmodule
'''
QUERIES = [
    ['find', '--query', 'mapped_top', '--limit', '100', '--format', 'json'],
    ['find', '--query', r'mapped_top.*\.i$', '--regex', '--format', 'json'],
    ['trace', '--mode', 'drivers', '--signal', 'mapped_top.y', '--format', 'json'],
    ['trace', '--mode', 'loads', '--signal', 'mapped_top.a', '--cone-level', '3', '--format', 'json'],
    ['hier', '--root', 'mapped_top', '--format', 'json'],
    ['whereis-instance', '--instance', 'mapped_top.u_leaf', '--show-params', '--format', 'json'],
]


def run(binary, args, expected=0):
    result = subprocess.run([str(binary), *map(str, args)], capture_output=True, timeout=20)
    assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
    return result


def query(binary, db, args):
    return run(binary, [args[0], '--db', db, *args[1:]])


def compile_db(binary, db, source):
    run(binary, ['compile', '--db', db, '--single-unit', source, '--top', 'mapped_top'])


def layout(data):
    header = HEADER.unpack_from(data)
    counts = header[3:]
    position = HEADER.size
    sections = {}
    def take(name, count, width):
        nonlocal position
        sections[name] = (position, count, width)
        position += count * width
    take('offsets', counts[0] + 1, 4)
    take('blob', counts[1], 1)
    for name, count, width in zip(
        ('signals', 'endpoints', 'signal_refs', 'load_ranges', 'load_ids', 'driver_ranges',
         'driver_ids', 'assignment_ranges', 'assignment_ids', 'hierarchy', 'children', 'globals', 'sinks'),
        counts[2:], (32, 48, 4, 12, 4, 12, 4, 12, 4, 24, 4, 16, 4),
    ):
        take(name, count, width)
    range_count = struct.unpack_from('<Q', data, position)[0]
    take('param_range_count', 1, 8)
    take('param_ranges', range_count, 12)
    param_count = struct.unpack_from('<Q', data, position)[0]
    take('param_count', 1, 8)
    take('params', param_count, 12)
    assert position == len(data), (position, len(data))
    return sections


def legacy_bytes(data, sections, version):
    output = bytearray(data[:HEADER.size])
    struct.pack_into('<I', output, 16, version)
    for name, (offset, count, width) in sections.items():
        part = data[offset:offset + count * width]
        if name == 'signals':
            part = b''.join(part[i:i + 20] for i in range(0, len(part), 32))
        elif name == 'hierarchy' and version <= 2:
            part = b''.join(part[i:i + 8] + part[i + 16:i + 24] for i in range(0, len(part), 24))
        elif name in ('assignment_ranges', 'assignment_ids') and version == 1:
            continue
        elif name.startswith('param') and version < 4:
            continue
        output.extend(part)
    return output


class Serve:
    def __init__(self, binary, db):
        self.process = subprocess.Popen([str(binary), 'serve', '--db', str(db)],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE)
        self.pending = b''
        self.response()

    def response(self):
        deadline = time.monotonic() + 15
        while b'<<END>>\n' not in self.pending:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([self.process.stdout], [], [], remaining)[0]:
                raise AssertionError('serve response deadline exceeded')
            chunk = os.read(self.process.stdout.fileno(), 65536)
            assert chunk, ('serve exited before response', self.process.poll(), self.pending)
            self.pending += chunk
        answer, self.pending = self.pending.split(b'<<END>>\n', 1)
        return answer

    def query(self, command):
        self.process.stdin.write(command.encode() + b'\n')
        self.process.stdin.flush()
        return self.response()

    def close(self):
        if self.process.poll() is None:
            try:
                self.query('quit')
            finally:
                self.process.stdin.close()
                try:
                    self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait()
        err = self.process.stderr.read()
        self.process.stdout.close()
        self.process.stderr.close()
        assert self.process.returncode == 0 and not err, (self.process.returncode, err)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--rtl-trace', required=True, type=Path)
    parser.add_argument('--reference-bin', type=Path)
    args = parser.parse_args()
    binary = args.rtl_trace.resolve()
    reference = args.reference_bin.resolve() if args.reference_bin else None
    with tempfile.TemporaryDirectory(prefix='rtl_trace_mapping_') as directory:
        root = Path(directory)
        source = root / 'mapped.sv'
        source.write_text(SOURCE)
        db = root / 'mapped.db'
        compile_db(binary, db, source)
        original = db.read_bytes()
        sections = layout(original)
        for command in QUERIES:
            got = query(binary, db, command)
            if reference:
                want = query(reference, db, command)
                assert (got.returncode, got.stdout, got.stderr) == (want.returncode, want.stdout, want.stderr)
        for version in (1, 2, 3, 4):
            compat = root / f'compat_v{version}.db'
            compat.write_bytes(legacy_bytes(original, sections, version))
            for command in QUERIES:
                got = query(binary, compat, command)
                if reference:
                    want = query(reference, compat, command)
                    assert (got.stdout, got.stderr) == (want.stdout, want.stderr), (version, command)
                elif command[0] in ('find', 'trace', 'hier'):
                    assert got.stdout == query(binary, db, command).stdout, (version, command)
        print('PASS: v1-v5 queries and unaligned mapped sections')
        writers = [subprocess.Popen([str(binary), 'compile', '--db', str(db), '--single-unit',
                                     str(source), '--top', 'mapped_top'],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                   for _ in range(2)]
        for writer in writers:
            stdout, stderr = writer.communicate(timeout=20)
            assert writer.returncode == 0, (writer.returncode, stdout, stderr)
        assert db.read_bytes() == original and not list(root.glob('*.tmp.*'))
        print('PASS: concurrent same-source writers publish complete identical snapshots')

        # Readers do not create sidecars, including when the DB directory is read-only.
        readonly = root / 'readonly'
        readonly.mkdir()
        readonly_db = readonly / 'snapshot.db'
        readonly_db.write_bytes(original)
        readonly_db.chmod(0o444)
        readonly.chmod(0o555)
        try:
            for command in QUERIES:
                query(binary, readonly_db, command)
            assert list(readonly.iterdir()) == [readonly_db]
        finally:
            readonly.chmod(0o755)
        print('PASS: read-only DB queries require no sidecar')

        mutations = [('short_header', original[:HEADER.size - 1])]
        def mutate(name, offset, fmt, value):
            payload = bytearray(original)
            struct.pack_into(fmt, payload, offset, value)
            mutations.append((name, payload))
        mutate('bad_magic', 0, '<I', 0)
        mutate('bad_version', 16, '<I', 99)
        offset_base, count, _ = sections['offsets']
        mutate('offset_out_of_blob', offset_base + (count - 1) * 4, '<I', len(original))
        mutate('offset_decrease', offset_base + 4, '<I', 0xFFFFFFFF)
        for name, (offset, count, width) in sections.items():
            if count:
                mutations.append((f'truncated_{name}', original[:offset + count * width - 1]))
        for name, field in (('signals', 8), ('endpoints', 28), ('load_ranges', 4),
                            ('driver_ranges', 4), ('assignment_ranges', 4),
                            ('hierarchy', 16), ('globals', 8), ('param_ranges', 4)):
            offset, count, _ = sections[name]
            if count:
                mutate(f'bad_range_{name}', offset + field, '<I', 0xFFFFFFFF)
        for name in ('load_ids', 'driver_ids', 'assignment_ids'):
            offset, count, _ = sections[name]
            if count:
                mutate(f'bad_signal_{name}', offset, '<I', 0xFFFFFFFF)
        corrupt = root / 'corrupt.db'
        for name, payload in mutations:
            corrupt.write_bytes(payload)
            for command in QUERIES:
                got = run(binary, [command[0], '--db', corrupt, *command[1:]], expected=1)
                assert not got.stdout and b'Failed to read DB:' in got.stderr, name
                if reference:
                    want = run(reference, [command[0], '--db', corrupt, *command[1:]], expected=1)
                    assert got.stderr == want.stderr, (name, command)
        # Huge counts must fail before allocation. Do not feed these to an eager reference
        # loader that can abort or attempt a huge allocation.
        for count_offset in (24, 32, 40, 48, 56, 64, 80, 96, 112, 128):
            payload = bytearray(original)
            struct.pack_into('<Q', payload, count_offset, 0xFFFFFFFFFFFFFFFF)
            corrupt.write_bytes(payload)
            run(binary, ['find', '--db', corrupt, '--query', 'mapped_top'], expected=1)
        print(f'PASS: {len(mutations)} corruption cases and 10 allocation bounds')

        # Same-source recompile keeps bytes but replaces the inode. Serve still has the
        # preceding snapshot open. A changed source is visible only after reload.
        db.chmod(0o640)
        client = Serve(binary, db)
        try:
            command = 'find --query mapped_top --limit 100 --format json'
            before = client.query(command)
            inode = db.stat().st_ino
            compile_db(binary, db, source)
            assert db.stat().st_ino != inode and db.read_bytes() == original
            assert db.stat().st_mode & 0o777 == 0o640
            assert client.query(command) == before
            source.write_text(SOURCE.replace('logic [7:0] mid;', 'logic [7:0] mid; logic new_marker;'))
            compile_db(binary, db, source)
            assert client.query(command) == before
            assert b'new_marker' not in before
            for _ in range(3):
                client.query('reload')
                assert b'new_marker' in client.query(command)
        finally:
            client.close()
        assert not list(root.glob('*.tmp.*'))
        print('PASS: compile while serve is mapped, mode preservation, reload lifetimes')

        # Atomic output follows the symlink target, matching the preceding writer.
        link = root / 'link.db'
        link.symlink_to(db.name)
        compile_db(binary, link, source)
        assert link.is_symlink() and link.read_bytes() == db.read_bytes()
        dangling = root / 'dangling.db'
        dangling_target = root / 'created_target.db'
        dangling.symlink_to(dangling_target.name)
        compile_db(binary, dangling, source)
        assert dangling.is_symlink() and dangling_target.read_bytes() == db.read_bytes()
        print('PASS: existing and dangling DB output symlinks retain their targets')
        # Failed publication must not leave partially written output or temporary files.
        destination = root / 'cannot_replace.db'
        destination.mkdir()
        (destination / 'keep').write_text('preserved')
        result = subprocess.run([str(binary), 'compile', '--db', str(destination), '--single-unit',
                                 str(source), '--top', 'mapped_top'], capture_output=True, timeout=20)
        assert result.returncode != 0 and (destination / 'keep').read_text() == 'preserved'
        assert not list(root.glob('*.tmp.*'))
        print('PASS: failed publication cleans its temporary file')


if __name__ == '__main__':
    main()
