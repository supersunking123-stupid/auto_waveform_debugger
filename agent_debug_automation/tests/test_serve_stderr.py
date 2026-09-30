"""RtlTraceServeSession stderr handling against a real `rtl_trace serve` process.

serve writes per-query diagnostics to stderr before it writes <<END>> to stdout. The session
must drain stderr while a query runs (a diagnostic bigger than the pipe capacity used to block
serve until the backend read timed out) and attribute each diagnostic to its own response.
"""

import errno
import fcntl
import os
import resource
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from typing import Optional
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from agent_debug_automation import clients


FIXTURE_SV = ROOT / "standalone_trace" / "tests" / "fixtures" / "semantic_top.sv"
BIG_BAD_REGEX = "[" + "a" * 70000
SHORT_BAD_REGEX = "zz("
VALID_QUERY = "find --query semantic_top.clk --limit 1"
# Short enough that a blocked serve fails the test quickly, long enough for a loaded machine.
TEST_READ_TIMEOUT_SEC = 10


def _rtl_trace_bin() -> Path:
    override = os.environ.get("RTL_TRACE_BIN")
    if override:
        return Path(override).expanduser().resolve()
    return ROOT / "standalone_trace" / "build" / "rtl_trace"


def _bad_regex_query(pattern: str) -> str:
    return f"find --query {pattern} --regex"


class _SessionTestBase(unittest.TestCase):
    bin_path: Path
    tmpdir: str
    db_path: Path

    @classmethod
    def setUpClass(cls) -> None:
        cls.bin_path = _rtl_trace_bin()
        if not cls.bin_path.exists():
            raise unittest.SkipTest(f"rtl_trace binary not found: {cls.bin_path}")
        if not FIXTURE_SV.exists():
            raise unittest.SkipTest(f"fixture not found: {FIXTURE_SV}")
        cls.tmpdir = tempfile.mkdtemp(prefix="serve_stderr_")
        cls.db_path = Path(cls.tmpdir) / "s.db"
        proc = subprocess.run(
            [str(cls.bin_path), "compile", "--db", str(cls.db_path), "--top", "semantic_top", str(FIXTURE_SV)],
            text=True,
            capture_output=True,
            timeout=120,
            check=False,
        )
        if proc.returncode != 0:
            shutil.rmtree(cls.tmpdir, ignore_errors=True)
            raise RuntimeError(f"rtl_trace compile failed: {proc.stderr}")

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.tmpdir, ignore_errors=True)

    def setUp(self) -> None:
        patcher = mock.patch.object(clients, "DEFAULT_BACKEND_READ_TIMEOUT_SEC", TEST_READ_TIMEOUT_SEC)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.session: Optional[clients.RtlTraceServeSession] = None

    def tearDown(self) -> None:
        if self.session is not None:
            self.session.stop()

    def _start(self, serve_args: Optional[list] = None, bin_path: Optional[str] = None) -> clients.RtlTraceServeSession:
        args = serve_args if serve_args is not None else ["--db", str(self.db_path)]
        self.session = clients.RtlTraceServeSession(bin_path or str(self.bin_path), args)
        return self.session

    def _assert_bad_regex(self, result: dict, pattern: str) -> None:
        self.assertEqual(result.get("status"), "success", result.get("message"))
        self.assertEqual(result.get("stdout"), "")
        self.assertIn("Invalid regex", result.get("stderr", ""))
        self.assertIn(pattern, result.get("stderr", ""))

    def _assert_valid(self, result: dict) -> None:
        self.assertEqual(result.get("status"), "success", result.get("message"))
        self.assertIn("signal semantic_top.clk", result.get("stdout", ""))
        self.assertNotIn("stderr", result)


class ServeStderrTest(_SessionTestBase):
    def test_short_invalid_regex_reports_stderr(self) -> None:
        session = self._start()
        self.assertEqual(session.startup.get("status"), "success")
        self._assert_bad_regex(session.query(_bad_regex_query(SHORT_BAD_REGEX)), SHORT_BAD_REGEX)

    def test_big_invalid_regex_does_not_block_serve(self) -> None:
        session = self._start()
        assert session.process.stderr is not None
        capacity = fcntl.fcntl(session.process.stderr.fileno(), fcntl.F_GETPIPE_SZ)
        self.assertGreater(len(BIG_BAD_REGEX), capacity)

        started = time.monotonic()
        result = session.query(_bad_regex_query(BIG_BAD_REGEX))
        self.assertLess(time.monotonic() - started, TEST_READ_TIMEOUT_SEC)
        self._assert_bad_regex(result, BIG_BAD_REGEX)
        self.assertGreater(len(result["stderr"]), 65536)
        self.assertIsNone(session.process.poll())

        # Nothing from the big diagnostic may leak into the next response.
        self._assert_valid(session.query(VALID_QUERY))

    def test_alternating_queries_keep_per_response_stderr(self) -> None:
        session = self._start()
        for round_idx in range(3):
            for pattern in (SHORT_BAD_REGEX, BIG_BAD_REGEX):
                with self.subTest(round=round_idx, pattern_len=len(pattern)):
                    bad = session.query(_bad_regex_query(pattern))
                    self._assert_bad_regex(bad, pattern)
                    # Exactly one diagnostic line per bad query.
                    self.assertEqual(bad["stderr"].count("Invalid regex"), 1)
                    self._assert_valid(session.query(VALID_QUERY))
        self.assertIsNone(session.process.poll())

    def test_startup_diagnostic_goes_to_startup_result(self) -> None:
        missing_db = Path(self.tmpdir) / "does_not_exist.db"
        session = self._start(["--db", str(missing_db)])
        self.assertEqual(session.startup.get("status"), "success")
        self.assertIn("Failed to read DB", session.startup.get("stderr", ""))
        # The startup diagnostic must not be attributed to the first query.
        status = session.query("help")
        self.assertEqual(status.get("status"), "success")
        self.assertNotIn("stderr", status)

    def test_startup_exit_reports_stderr(self) -> None:
        session = self._start(["--bogus-option"])
        self.assertEqual(session.startup.get("status"), "error")
        self.assertIn("Unknown option", session.startup.get("stderr", ""))
        self.assertIsNotNone(session.process.poll())

    def test_stop_joins_pump_before_closing_pipes(self) -> None:
        session = self._start()
        self._assert_bad_regex(session.query(_bad_regex_query(BIG_BAD_REGEX)), BIG_BAD_REGEX)
        pump = session._stderr_pump
        assert pump is not None
        self.assertTrue(pump.is_alive())

        real_close = clients._close_process_pipes
        pump_alive_at_close = []

        def _checked_close(process: subprocess.Popen) -> None:
            pump_alive_at_close.append(pump.is_alive())
            real_close(process)

        started = time.monotonic()
        with mock.patch.object(clients, "_close_process_pipes", _checked_close):
            self.assertEqual(session.stop(), {"status": "success"})
        self.assertLess(time.monotonic() - started, 3.0)
        self.assertEqual(pump_alive_at_close, [False])
        self.assertFalse(pump.is_alive())
        assert session.process.stderr is not None
        self.assertTrue(session.process.stderr.closed)
        self.assertIsNotNone(session.process.poll())

        # A query on a stopped session reports the exit instead of touching the closed pipes.
        after = session.query(VALID_QUERY)
        self.assertEqual(after.get("status"), "error")
        self.assertEqual(after.get("message"), "rtl_trace serve exited")
        # A second stop is harmless.
        self.assertEqual(session.stop(), {"status": "success"})

    def test_pump_read_failure_is_reported_not_eof(self) -> None:
        def _failing_read(pump: clients._StderrPump) -> bytes:
            raise OSError(errno.EIO, "injected read failure")

        with mock.patch.object(clients._StderrPump, "_read_chunk", _failing_read):
            session = self._start()
            self.assertEqual(session.startup.get("status"), "success")
            bad = session.query(_bad_regex_query(SHORT_BAD_REGEX))
            self.assertEqual(bad.get("status"), "success", bad.get("message"))
            self.assertIn("stderr capture failed", bad.get("stderr", ""))
            self.assertIn("injected read failure", bad["stderr"])
            pump = session._stderr_pump
            assert pump is not None
            self.assertFalse(pump.is_alive())
            # Later responses keep saying so instead of looking clean.
            later = session.query(VALID_QUERY)
            self.assertIn("signal semantic_top.clk", later.get("stdout", ""))
            self.assertIn("stderr capture failed", later.get("stderr", ""))


class ServeEofStderrTest(_SessionTestBase):
    """serve dying mid-session: the terminated/exited results carry its stderr.

    The real serve has no command that exits with a diagnostic, so a stub that speaks the
    serve protocol stands in for it.
    """

    def _write_stub(self, stderr_bytes: int) -> str:
        stub = Path(self.tmpdir) / f"fake_serve_{stderr_bytes}.sh"
        stub.write_text(
            "#!/bin/sh\n"
            "echo 'startup ok'\n"
            "echo '<<END>>'\n"
            "read line\n"
            f"head -c {stderr_bytes} /dev/zero | tr '\\0' x >&2\n"
            "echo \"fatal: died on $line\" >&2\n"
            "echo 'partial output'\n"
            "exit 3\n"
        )
        stub.chmod(stub.stat().st_mode | stat.S_IXUSR)
        return str(stub)

    def test_exit_mid_query_reports_stderr(self) -> None:
        for stderr_bytes in (0, 200000):
            with self.subTest(stderr_bytes=stderr_bytes):
                session = self._start([], bin_path=self._write_stub(stderr_bytes))
                self.assertEqual(session.startup, {"status": "success", "stdout": "startup ok\n"})

                result = session.query("boom")
                self.assertEqual(result.get("status"), "error")
                self.assertEqual(result.get("message"), "rtl_trace serve terminated while waiting for response")
                self.assertEqual(result.get("stdout"), "partial output\n")
                self.assertTrue(result["stderr"].endswith("fatal: died on boom\n"))
                self.assertEqual(len(result["stderr"]), stderr_bytes + len("fatal: died on boom\n"))

                session.process.wait(timeout=5)
                exited = session.query("again")
                self.assertEqual(exited.get("status"), "error")
                self.assertEqual(exited.get("message"), "rtl_trace serve exited")
                self.assertEqual(exited.get("stderr"), "")
                session.stop()
                self.session = None


# select() rejects descriptors >= FD_SETSIZE; the session's pipes must work above it.
FD_SETSIZE = 1024
HIGH_FD_FLOOR = 1100


class _HighFdMixin:
    """Occupies every descriptor below HIGH_FD_FLOOR so the session's pipes land above FD_SETSIZE."""

    def setUp(self) -> None:
        super().setUp()  # type: ignore[misc]
        soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        want = HIGH_FD_FLOOR + 100
        if soft != resource.RLIM_INFINITY and soft < want:
            if hard != resource.RLIM_INFINITY and hard < want:
                self.skipTest(f"RLIMIT_NOFILE hard limit {hard} < {want}: cannot place pipes above fd {FD_SETSIZE}")
            resource.setrlimit(resource.RLIMIT_NOFILE, (want, hard))
            self.addCleanup(resource.setrlimit, resource.RLIMIT_NOFILE, (soft, hard))
        fillers: list = []
        # Registered after the rlimit restore, so it runs first (cleanups are LIFO).
        self.addCleanup(lambda: [os.close(fd) for fd in fillers])
        while not fillers or fillers[-1] < HIGH_FD_FLOOR:
            fillers.append(os.open(os.devnull, os.O_RDONLY))

    def _start(self, *args, **kwargs) -> clients.RtlTraceServeSession:
        session = super()._start(*args, **kwargs)  # type: ignore[misc]
        assert session.process.stderr is not None and session.process.stdout is not None
        self.assertGreaterEqual(session.process.stderr.fileno(), FD_SETSIZE)
        self.assertGreaterEqual(session.process.stdout.fileno(), FD_SETSIZE)
        return session


class HighFdServeStderrTest(_HighFdMixin, ServeStderrTest):
    """Every ServeStderrTest case (startup errors, 70 KB diagnostic, alternating queries, stop,
    pump failure) with the session's pipes above FD_SETSIZE."""


class HighFdServeEofStderrTest(_HighFdMixin, ServeEofStderrTest):
    """serve exiting mid-query with the session's pipes above FD_SETSIZE."""


if __name__ == "__main__":
    unittest.main()
