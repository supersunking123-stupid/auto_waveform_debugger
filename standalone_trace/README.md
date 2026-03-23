# standalone_trace

`rtl_trace` 是从 simview 中抽取出来的精简版 RTL trace 工具，只保留：

- 读取并 elaboration Verilog/SystemVerilog 设计（基于 slang）
- `compile` 阶段按实例 body 建立 drivers / loads 索引并复用缓存，避免对每个信号重复做整棵 AST 遍历
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
build/rtl_trace compile --db rtl_trace.db --top <top_module> [--incremental] [--relax-defparam] [--mfcu] [--partition-budget N] [--compile-log <file>] [slang source args...]
build/rtl_trace trace --db rtl_trace.db --mode drivers --signal top.u0.sig[3] \
  [--cone-level N] [--prefer-port-hop] [--depth N] [--max-nodes N] [--include RE] [--exclude RE] [--stop-at RE] [--format text|json]
build/rtl_trace trace --db rtl_trace.db --mode loads --signal top.u0.sig[7:4] \
  [--cone-level N] [--prefer-port-hop] [--depth N] [--max-nodes N] [--include RE] [--exclude RE] [--stop-at RE] [--format text|json]
build/rtl_trace hier --db rtl_trace.db [--root top.u0] [--depth N] [--max-nodes N] [--format text|json]
build/rtl_trace find --db rtl_trace.db --query "foo.bar" [--regex] [--limit N] [--format text|json]
build/rtl_trace serve [--db rtl_trace.db]
```

说明：
- `compile` 阶段会解析/展开 RTL 并生成二进制 graph DB（默认 `rtl_trace.db`）。
- `--top <top_module>` 为必选参数；缺失时工具会报错并退出。
- `compile --incremental` 会基于输入指纹复用已有 DB（命中时跳过重编译）。
- `compile --relax-defparam` 会放宽 defparam 相关报错（例如跨层级 defparam 暂未解析时），便于先生成可用 DB。
- `compile --mfcu` 使用“分组 MFCU”：每个 `-f` 文件列表内的源文件合并为一个 compilation unit，命令行直接传入的源文件合并为另一个 compilation unit（而不是把所有输入全并成一个 unit）。
- `compile --partition-budget N` 会按实例树做预算切分并分区生成 DB（日志会显示切分结果和每个分区执行进度）。
- `compile --compile-log <file>` 会把编译阶段关键步骤与分区信息同步写入日志文件（同时仍打印到屏幕）；对于 DB 生成阶段，还会额外记录 `save_graph_db` 子步骤用时（如 `build_graph`、`write_file`）。
- `trace` 阶段只查询数据库，不会再次解析 RTL。
- 数据库当前格式为二进制 graph DB。
- 已不再支持旧的 V7/V8 文本 DB；需要重新 `compile` 生成当前 graph DB。
- `serve` 适合大设计交互式调试：DB 只加载一次，后续 `find` / `trace` / `hier` 会复用常驻会话。
- 对高扇出时钟/复位网络，`compile` 会把其 fanout 压缩到专用紧凑表中，减少普通 `loads` 明细存储；`trace` 查询这类网络时会优先走该紧凑表。
- `compile` 生成 DB 时会缓存每个信号的解析结果，并在字符串收集与最终写出两个阶段之间复用，避免重复解析同一信号。
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
- `hier`：输出实例层级树（支持 `--root`、`--depth`、`--max-nodes`、`--format json`）。
- `--cone-level N`：逻辑锥自动展开级数（默认 1；`drivers` 沿 RHS 回溯，`loads` 沿 LHS 前推）。
- `--prefer-port-hop`：当命中端口绑定表达式且无可展开 RHS/LHS 时，优先尝试沿端口桥接继续追踪。
- `serve`：启动交互式后端，命令以行为单位输入，每次响应后输出一行 `<<END>>`。

## TODO

- 支持完整的 cross-hier `defparam` 语义（当前仅提供 `--relax-defparam` 作为兼容放宽模式）。

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
  --top timer_tb \
  -f simview.f

/path/to/standalone_trace/build/rtl_trace trace \
  --db trace.db \
  --mode drivers \
  --signal timer_tb.timeout
```
