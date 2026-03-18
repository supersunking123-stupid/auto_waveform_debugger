# Local Test on Cores-VeeR-EH1

This guide shows how to test `standalone_trace/rtl_trace` against the
`Cores-VeeR-EH1` design in the same workspace.

## Do I need to run Step 1 / Step 2 every trial?

Usually no.

- Step 1 (`cmake` + `ninja`) is only needed when `standalone_trace` source code changes.
- Step 2 (`make ... snapshots/default/simview.flist`) is only needed when
  `Cores-VeeR-EH1` RTL / config / filelist inputs change.
- For repeated trials with the same binary and RTL setup, rerun only:
  - Step 3 (`rtl_trace compile ...`, or `--incremental`)
  - Step 4 (`rtl_trace trace ...`)

## 1) Build `rtl_trace`

```bash
cd /home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace
cmake -B build -GNinja .
ninja -C build
```

## 2) Prepare `simview.flist` in Cores-VeeR-EH1

```bash
cd /home/qsun/AI_PROJ/auto_waveform_debugger/Cores-VeeR-EH1
RV_ROOT=$PWD make -f tools/Makefile snapshots/default/simview.flist
```

This generates:
- `snapshots/default/simview.flist`
- `snapshots/default/common_defines.vh` (via `defines.h` dependency)

## 3) Compile trace DB

```bash
cd /home/qsun/AI_PROJ/auto_waveform_debugger/Cores-VeeR-EH1
/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/build/rtl_trace compile \
  --db /tmp/veer_trace.db \
  --single-unit \
  --libraries-inherit-macros \
  -f snapshots/default/simview.flist \
  -Idesign/lib \
  -Idesign/include \
  -Isnapshots/default \
  --top tb_top
```

Expected summary:
- `db: /tmp/veer_trace.db`
- `signals: <N>`

## 4) Run trace queries

Drivers example:

```bash
/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/build/rtl_trace trace \
  --db /tmp/veer_trace.db \
  --mode drivers \
  --signal tb_top.bridge.clk
```

Loads example:

```bash
/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/build/rtl_trace trace \
  --db /tmp/veer_trace.db \
  --mode loads \
  --signal tb_top.bridge.m_awvalid
```

For `--mode loads`, expression endpoints can include:
- `lhs <hierarchical_path>`

JSON output example:

```bash
/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/build/rtl_trace trace \
  --db /tmp/veer_trace.db \
  --mode drivers \
  --signal tb_top.bridge.clk \
  --format json
```

Use traversal controls:

```bash
/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/build/rtl_trace trace \
  --db /tmp/veer_trace.db \
  --mode drivers \
  --signal tb_top.bridge.clk \
  --depth 8 \
  --max-nodes 5000 \
  --include "tb_top\\." \
  --exclude "debug|scan"
```

Signal search example:

```bash
/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/build/rtl_trace find \
  --db /tmp/veer_trace.db \
  --query "tb_top.bridge.m_awvalid"
```

## Notes

- Re-run `compile` after RTL / config changes.
- You can use `compile --incremental` for repeated runs with unchanged inputs.
- DB format is `RTL_TRACE_DB_V4` (tool can still read V1 / V2 / V3).
- For built-in automated semantic regression, run:
  - `ctest --test-dir /home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/build --output-on-failure`
