# Test Case 22: Cross-Link Snapshot and Cycle Sampling

## Phase 7: Snapshot And Cycle Sampling

Verifies absolute and cycle-relative sampling in `trace_with_snapshot`.

## Test Configuration

- **Signal:** `top.mem0_rd_bw_mon.clk`
- **Time:** `399970000`
- **Mode:** `drivers`
- **Clock:** `top.mem0_rd_bw_mon.clk`
- **Sample Offsets:** `[-1000, 0, 1000]`
- **Cycle Offsets:** `[-1, 0, 1]`

## Test 7.1: Snapshot and Cycle Sampling

Tool: `trace_with_snapshot(...)`

Expected:
- `waveform.absolute_offset_samples` exists
- `waveform.cycle_offset_samples.clock_path` exists
- `waveform.cycle_offset_samples.resolved_clock_path` exists
- `waveform.cycle_offset_samples.cycle_times` includes `-1`, `0`, `1`
- `waveform.cycle_offset_samples.samples` exists for valid cycle times

Clock-specific:
- Cycle `0` corresponds to `399970000`
- Cycle `-1` is previous posedge
- Cycle `1` is next posedge
