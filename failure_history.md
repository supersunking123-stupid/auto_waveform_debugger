# Failure History

This file records previously failing waveform queries that should remain covered by regression tests.

Waveform used for reproduction:

```bash
/home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/wave.fsdb
```

Binary used for reproduction:

```bash
/home/qsun/AI_PROJ/auto_waveform_debugger/waveform_explorer/build/wave_agent_cli
```

## Regression Commands

These commands intentionally use the original bare structural names without packed range suffixes. They previously failed because the FSDB stored the signals as packed-vector paths such as `cq_rd_count9[8:0]`, while direct lookup only accepted exact names.

### 1. CQ count transitions

```bash
waveform_explorer/build/wave_agent_cli /home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/wave.fsdb '{"cmd":"get_transitions","args":{"path":"top.nvdla_top.u_partition_o.u_NV_NVDLA_cvif.u_read.u_cq.cq_rd_count9","start_time":784000000,"end_time":800000000,"max_limit":5}}'
```

Expected: `status":"success"` and transition values beginning with:

```text
b000111011
b000111010
b000111011
```

### 2. CQ credits transitions

```bash
waveform_explorer/build/wave_agent_cli /home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/wave.fsdb '{"cmd":"get_transitions","args":{"path":"top.nvdla_top.u_partition_o.u_NV_NVDLA_cvif.u_read.u_cq.cq_rd9_credits","start_time":784000000,"end_time":800000000,"max_limit":5}}'
```

Expected: `status":"success"` and transition values beginning with:

```text
b000111011
b000111010
b000111011
```

### 3. CQ take-thread-id transitions

```bash
waveform_explorer/build/wave_agent_cli /home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/wave.fsdb '{"cmd":"get_transitions","args":{"path":"top.nvdla_top.u_partition_o.u_NV_NVDLA_cvif.u_read.u_cq.cq_rd_take_thread_id","start_time":784000000,"end_time":800000000,"max_limit":5}}'
```

Expected: `status":"success"` and transition values beginning with:

```text
b1001
b0000
b1001
```

### 4. CQ snapshot at 784530000

```bash
waveform_explorer/build/wave_agent_cli /home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/wave.fsdb '{"cmd":"get_snapshot","args":{"time":784530000,"signals":["top.nvdla_top.u_partition_o.u_NV_NVDLA_cvif.u_read.u_cq.cq_rd_count9","top.nvdla_top.u_partition_o.u_NV_NVDLA_cvif.u_read.u_cq.update_head_next"]}}'
```

Expected:

```text
top.nvdla_top.u_partition_o.u_NV_NVDLA_cvif.u_read.u_cq.cq_rd_count9 = b000111011
top.nvdla_top.u_partition_o.u_NV_NVDLA_cvif.u_read.u_cq.update_head_next = b0000000000
```

### 5. CQ snapshot at 799290000

```bash
waveform_explorer/build/wave_agent_cli /home/qsun/AI_PROJ/auto_waveform_debugger/test_cases/wave.fsdb '{"cmd":"get_snapshot","args":{"time":799290000,"signals":["top.nvdla_top.u_partition_o.u_NV_NVDLA_cvif.u_read.u_cq.cq_rd_count9","top.nvdla_top.u_partition_o.u_NV_NVDLA_cvif.u_read.u_cq.update_head_next"]}}'
```

Expected:

```text
top.nvdla_top.u_partition_o.u_NV_NVDLA_cvif.u_read.u_cq.cq_rd_count9 = b000000000
top.nvdla_top.u_partition_o.u_NV_NVDLA_cvif.u_read.u_cq.update_head_next = b0000000000
```

## Original Failure Mode

Before the fix, these bare-name queries failed because `waveform_explorer` only matched exact FSDB paths, while the dumped signals appeared with packed suffixes:

- `...cq_rd_count9[8:0]`
- `...cq_rd9_credits[8:0]`
- `...cq_rd_take_thread_id[3:0]`
- `...update_head_next[9:0]`

The resolver now falls back from a bare hierarchical base path to the uniquely matching packed-vector FSDB signal.
