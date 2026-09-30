"""Shared constants and helpers for the cross-link tests (tc16-tc27).

The tests run against a small, regenerable fixture instead of a large external
design. `make_fixture.sh` builds it from `cross_link_fixture/src`:

    test_cases/rtl_trace.db   structural DB (rtl_trace compile, top module `top`)
    test_cases/wave.fsdb      FSDB from VCS + Verdi PLI

Fixture timeline (timescale 1ps in the FSDB; clk period 10 ns, posedges at
5 ns + k * 10 ns):

    0     - 33 ns   reset
    33    - 2000 ns normal traffic, `ready_in` toggles (FIFO fills 1 of ~3 cycles)
    2005  - 3005 ns sink stalled: FIFO full, `ready_in` stuck at 0
    3005  - 4000 ns normal traffic

Hierarchy: top (testbench) -> dut (xl_dut: u_src, u_fifo, u_sink), hs_mon.
"""

import subprocess
import sys
import unittest
from pathlib import Path

TEST_CASES_DIR = Path(__file__).resolve().parent
ROOT_DIR = TEST_CASES_DIR.parent

DB_PATH = str(TEST_CASES_DIR / "rtl_trace.db")
WAVE_PATH = str(TEST_CASES_DIR / "wave.fsdb")
RTL_TRACE_BIN = str(ROOT_DIR / "standalone_trace" / "build" / "rtl_trace")
WAVE_CLI_BIN = str(ROOT_DIR / "waveform_explorer" / "build" / "wave_agent_cli")
MAKE_FIXTURE = TEST_CASES_DIR / "make_fixture.sh"

# ---- Signals (hierarchical paths as seen in both the RTL DB and the FSDB) ----
TB = "top"
MON = "top.hs_mon"
CLK = "top.hs_mon.clk"            # clock seen by the monitor (driven by tb clock generator)
READY = "top.hs_mon.ready_in"     # handshake ready (driven by FIFO wr_ready)
VALID = "top.hs_mon.valid_in"     # handshake valid (driven by source)
LAST = "top.hs_mon.last_in"       # burst last flag
ID = "top.hs_mon.id_in"           # 8-bit burst id
FIFO = "top.dut.u_fifo"
FIFO_COUNT = "top.dut.u_fifo.count"          # [2:0]
FIFO_WR_PTR = "top.dut.u_fifo.wr_ptr"        # [1:0]
SRC_ID = "top.dut.u_src.id"                  # [7:0]
SRC_DATA = "top.dut.u_src.data"              # [15:0]
SINK_CHECKSUM = "top.dut.u_sink.checksum"    # [15:0]
STALL_REQ = "top.stall_req"
FIFO_RD_DATA = "top.dut.u_fifo.rd_data"      # cone contains the (not dumped) memory array

# ---- Times (ps) ----
# Clock posedge at which ready_in, valid_in and last_in all change (1 -> 0).
T_REF = 175000
# Posedge in the middle of the stall window (ready_in stuck at 0 from 2005000
# to 3005000).
T_STUCK = 2505000
STALL_START = 2005000
STALL_END = 3005000


def ensure_fixture():
    """Make sure rtl_trace.db / wave.fsdb exist and are current.

    Runs ./make_fixture.sh (a no-op when up to date). Raises FileNotFoundError
    with the exact command to run if the fixture cannot be produced.
    """
    cmd = [str(MAKE_FIXTURE)]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
        rc, out = proc.returncode, proc.stdout + proc.stderr
    except Exception as exc:  # pragma: no cover - environment problem
        rc, out = 1, str(exc)
    if rc != 0:
        raise FileNotFoundError(
            f"cross-link fixture unavailable (rc={rc}). Regenerate with: {MAKE_FIXTURE} --force\n{out[-2000:]}"
        )
    for path, name in [
        (DB_PATH, "rtl_trace.db"),
        (WAVE_PATH, "wave.fsdb"),
        (RTL_TRACE_BIN, "rtl_trace"),
        (WAVE_CLI_BIN, "wave_agent_cli"),
    ]:
        if not Path(path).exists():
            raise FileNotFoundError(f"{name} not found: {path} (fixture: run {MAKE_FIXTURE} --force)")
