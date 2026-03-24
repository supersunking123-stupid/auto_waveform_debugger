# Test Case 27: History Failure Regression

## Purpose

This test case verifies that previously failing waveform queries now work correctly after the signal path resolution fix.

## Background

These commands previously failed because the FSDB stored signals as packed-vector paths (e.g., `cq_rd_count9[8:0]`), while direct lookup only accepted exact bare names. The resolver now falls back from bare hierarchical names to uniquely matching packed-vector FSDB signals.

## Test Assets

- **Waveform:** `/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/wave.fsdb`
- **Binary:** `/home/qsun/AI_PROJ/auto_waveform_debugger/waveform_explorer/build/wave_agent_cli`

## Time Window

All tests use the time window from the original failure reproduction:
- **Start:** `784000000` ps (784000 ns)
- **End:** `800000000` ps (800000 ns)

This falls within the waveform's hot region (300000-540000 ns was old; actual waveform extends beyond).

## Signals Under Test

| Signal | Type | Packed Suffix |
|--------|------|---------------|
| `top.nvdla_top.u_partition_o.u_NV_NVDLA_cvif.u_read.u_cq.cq_rd_count9` | [8:0] | Yes |
| `top.nvdla_top.u_partition_o.u_NV_NVDLA_cvif.u_read.u_cq.cq_rd9_credits` | [8:0] | Yes |
| `top.nvdla_top.u_partition_o.u_NV_NVDLA_cvif.u_read.u_cq.cq_rd_take_thread_id` | [3:0] | Yes |
| `top.nvdla_top.u_partition_o.u_NV_NVDLA_cvif.u_read.u_cq.update_head_next` | [9:0] | Yes |

## Tests

### Test 1: CQ count transitions

**Command:**
```
get_transitions(path="top.nvdla_top.u_partition_o.u_NV_NVDLA_cvif.u_read.u_cq.cq_rd_count9", 
                start_time=784000000, end_time=800000000, max_limit=5)
```

**Expected:**
- `status` = `success`
- Transition values: `b000111011`, `b000111010`, `b000111011`, ...

### Test 2: CQ credits transitions

**Command:**
```
get_transitions(path="top.nvdla_top.u_partition_o.u_NV_NVDLA_cvif.u_read.u_cq.cq_rd9_credits", 
                start_time=784000000, end_time=800000000, max_limit=5)
```

**Expected:**
- `status` = `success`
- Transition values: `b000111011`, `b000111010`, `b000111011`, ...

### Test 3: CQ take-thread-id transitions

**Command:**
```
get_transitions(path="top.nvdla_top.u_partition_o.u_NV_NVDLA_cvif.u_read.u_cq.cq_rd_take_thread_id", 
                start_time=784000000, end_time=800000000, max_limit=5)
```

**Expected:**
- `status` = `success`
- Transition values: `b1001`, `b0000`, `b1001`, ...

### Test 4: CQ snapshot at 784530000

**Command:**
```
get_snapshot(time=784530000, 
             signals=["top.nvdla_top.u_partition_o.u_NV_NVDLA_cvif.u_read.u_cq.cq_rd_count9",
                      "top.nvdla_top.u_partition_o.u_NV_NVDLA_cvif.u_read.u_cq.update_head_next"])
```

**Expected:**
- `cq_rd_count9` = `b000111011`
- `update_head_next` = `b0000000000`

### Test 5: CQ snapshot at 799290000

**Command:**
```
get_snapshot(time=799290000, 
             signals=["top.nvdla_top.u_partition_o.u_NV_NVDLA_cvif.u_read.u_cq.cq_rd_count9",
                      "top.nvdla_top.u_partition_o.u_NV_NVDLA_cvif.u_read.u_cq.update_head_next"])
```

**Expected:**
- `cq_rd_count9` = `b000000000`
- `update_head_next` = `b0000000000`

## Success Criteria

All 5 tests must pass with:
- `status` = `success`
- Correct transition/snapshot values matching expected patterns

## Related

- Failure history: `failure_history.md`
- Signal resolution fix: `agent_debug_automation_mcp.py` (_map_signal_to_waveform function)
