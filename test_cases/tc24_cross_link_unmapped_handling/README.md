# Test Case 24: Cross-Link Unmapped Signal Handling

## Phase 9: Unmapped Signal Handling

Verifies unmapped signals are reported, not silently discarded.

## Test Configuration

- Run `trace_with_snapshot` and `explain_signal_at_time`
- Choose signal whose structural cone likely includes non-directly-visible waveform names

## Test 9.1: Unmapped Signal Reporting

Expected:
- `unmapped_signals` exists
- Each unmapped entry contains:
  - `signal`
  - `reason`
- Mapped signals still produce usable output
- Tool returns `status=success`

Use WARN if chosen cone maps everything cleanly.
