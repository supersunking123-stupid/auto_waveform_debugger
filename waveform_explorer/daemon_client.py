import json
import subprocess
import threading
from collections import deque
from typing import Deque, Dict, Optional


class WaveformDaemon:
    def __init__(self, cli_path: str, waveform_path: str):
        self._lock = threading.RLock()
        self._stderr_chunks: Deque[str] = deque(maxlen=256)
        self._closed = False
        self.process = subprocess.Popen(
            [cli_path, waveform_path],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self._stderr_thread = threading.Thread(target=self._drain_stderr, daemon=True)
        self._stderr_thread.start()

    def _drain_stderr(self) -> None:
        stderr = self.process.stderr
        if stderr is None:
            return
        try:
            while True:
                chunk = stderr.read(1024)
                if not chunk:
                    break
                with self._lock:
                    self._stderr_chunks.append(chunk)
        except Exception:
            # Best-effort drain only.
            return

    def _stderr_text(self) -> str:
        with self._lock:
            return "".join(self._stderr_chunks)

    def query(self, cmd: str, args: Optional[Dict[str, object]] = None) -> dict:
        query_obj = {"cmd": cmd, "args": args or {}}
        query_str = json.dumps(query_obj)

        with self._lock:
            if self._closed:
                return {"status": "error", "message": "daemon is closed"}
            if self.process.poll() is not None:
                return {
                    "status": "error",
                    "message": "Daemon exited",
                    "stderr": self._stderr_text(),
                }

            stdin = self.process.stdin
            stdout = self.process.stdout
            if stdin is None or stdout is None:
                return {"status": "error", "message": "daemon pipes are unavailable"}

            try:
                stdin.write(query_str + "\n")
                stdin.flush()

                line = stdout.readline()
                if not line:
                    return {
                        "status": "error",
                        "message": "Daemon crashed",
                        "stderr": self._stderr_text(),
                    }

                return json.loads(line)
            except Exception as e:
                return {"status": "error", "message": str(e)}

    def close(self) -> None:
        with self._lock:
            if self._closed:
                return
            self._closed = True

            if self.process.poll() is None:
                self.process.terminate()
                try:
                    self.process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    self.process.kill()

            for stream_name in ("stdin", "stdout", "stderr"):
                stream = getattr(self.process, stream_name, None)
                if stream:
                    try:
                        stream.close()
                    except Exception:
                        pass

        if self._stderr_thread.is_alive():
            self._stderr_thread.join(timeout=1)

    def terminate(self) -> None:
        self.close()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()

