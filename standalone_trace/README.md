# standalone_trace

`rtl_trace` 是从 simview 中抽取出来的精简版 RTL trace 工具，只保留：

- 读取并 elaboration Verilog/SystemVerilog 设计（基于 slang）
- 对指定层级信号进行 `drivers` / `loads` 追踪
- 支持跨实例端口递归继续追踪（input / output 端口都会继续展开）
- trace 结果优先返回逻辑表达式位置（assignment / always 中的表达式），避免停留在端口边界
- `--mode loads` 下，若命中逻辑表达式，会额外打印该表达式对应 assignment 的 LHS 层级路径
- 支持多跳 tracing 控制：`--depth`、`--max-nodes`、`--stop-at`
- 支持路径过滤：`--include`、`--exclude`
- 支持结构化输出：`--format json`
- 支持信号检索：`find --query ...`（含近似建议）
- 默认自动追加 `--timescale 1ns/1ps`（若命令行未显式提供 `--timescale`）

该目录已 vendored 必要依赖（`third_party/slang`、`third_party/fmt`），
只拷贝 `standalone_trace/` 整个目录即可独立构建，不依赖上层 simview 工程。

本地联调（基于 `Cores-VeeR-EH1`）请见：
- [LOCALTEST.md](./LOCALTEST.md)

## Build

这是独立工程，直接在 `standalone_trace` 目录构建即可：

```bash
cd standalone_trace
cmake -B build -GNinja .
ninja -C build
```

## Usage

```bash
build/rtl_trace compile --db rtl_trace.db [--incremental] [slang source args...]
build/rtl_trace trace --db rtl_trace.db --mode drivers --signal top.u0.sig[3] \
  [--depth N] [--max-nodes N] [--include RE] [--exclude RE] [--stop-at RE] [--format text|json]
build/rtl_trace trace --db rtl_trace.db --mode loads --signal top.u0.sig[7:4] \
  [--depth N] [--max-nodes N] [--include RE] [--exclude RE] [--stop-at RE] [--format text|json]
build/rtl_trace find --db rtl_trace.db --query "foo.bar" [--regex] [--limit N] [--format text|json]
```

说明：
- `compile` 阶段会解析/展开 RTL 并生成离线数据库（默认 `rtl_trace.db`）。
- `compile --incremental` 会基于输入指纹复用已有 DB（命中时跳过重编译）。
- `trace` 阶段只查询数据库，不会再次解析 RTL。
- 数据库当前版本为 `RTL_TRACE_DB_V4`（可读取旧版 V1/V2/V3）。
- `compile` 时如果你未传 `--timescale`，工具会自动使用 `1ns/1ps`，避免 mixed / missing timescale 报错。
- 若显式传了 `--timescale`，使用用户值。
- 若你希望看到最新的跨端口递归结果和 `loads` 的 `lhs` 信息，请重新执行一次 `compile` 生成新 DB。

`trace` 输出补充：
- `mode=drivers`：若表达式可解析到 assignment，会打印：
  - `assign <assignment_text>`
  - `rhs <hierarchical_path>`（RHS 信号列表）
- `mode=loads`：若表达式可关联到 assignment，会打印：
  - `lhs <hierarchical_path>`（LHS 信号列表）
- `--format json`：输出包含 `summary`、`endpoints`、`stops`，便于 agent / 脚本处理。
- 支持位选查询：`--signal top.sig[3]`、`--signal top.sig[7:4]`。

## Automated Semantic Regression

项目内置 CTest 语义回归用例（覆盖跨端口追踪、loads 上下文 LHS、bit 过滤、depth/node stop、find 建议、incremental cache hit）：

```bash
cd standalone_trace
cmake -B build -GNinja .
ninja -C build
ctest --test-dir build --output-on-failure
```

示例（在 `example/simview_trace` 目录）：

```bash
/path/to/standalone_trace/build/rtl_trace compile \
  --db trace.db \
  -f simview.f

/path/to/standalone_trace/build/rtl_trace trace \
  --db trace.db \
  --mode drivers \
  --signal timer_tb.timeout
```
