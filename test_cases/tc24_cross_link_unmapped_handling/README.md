# Test Case 24: Cross-Link Unmapped Signal Handling

## Phase 9: Unmapped Signal Handling

Verifies unmapped signals are reported, not silently discarded.

## Test Configuration

- Run `trace_with_snapshot` and `explain_signal_at_time`
- Signal: `top.dut.u_fifo.rd_data` at `175000`. It is read from the FIFO memory array
  `top.dut.u_fifo.mem`, which the fixture FSDB does not dump (unpacked arrays are
  not dumped without `+mda`), so the cone always contains an unmapped signal.

## Test 9.1: Unmapped Signal Reporting

Expected:
- `unmapped_signals` exists
- Each unmapped entry contains:
  - `signal`
  - `reason`
- Mapped signals still produce usable output
- Tool returns `status=success`

Expected unmapped entry: `top.dut.u_fifo.mem` with reason `waveform-path-not-found`.
