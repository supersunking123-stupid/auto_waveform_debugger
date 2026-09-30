# Compile Benchmark Commands

## Current reference design: Lumion vb2b_dbPCIe__ips

The Lumion repo has `make rtl_trace_db` in
`func_ver/sanity/vb2b/vb2b_dbPCIe__ips/Makefile` (copies sources to `rtl_trace_work/src`, applies
`rtl_trace/patch_sources.sh` for slang compatibility, runs `rtl_trace compile` with the VCS
defines/filelists). A ready worktree is at `local_test_design/lumion` (gitignored, branch
`rtl-trace-bench`, created with `git worktree add -b rtl-trace-bench <dir>` from the Lumion repo
`/home/qsun/Lumion_proj/lumionchip_shanghai_n1_PCIE_HPA_EVAL_batch_quick_20260929_120154`).

```bash
cd local_test_design/lumion/func_ver/sanity/vb2b/vb2b_dbPCIe__ips
free -g    # need >= 34 GB MemAvailable: the compile peaks at ~28 GB RSS
make rtl_trace_clean
systemd-run --user --scope -p MemoryMax=40G -p MemorySwapMax=0 \
  /usr/bin/time -v make rtl_trace_db RTL_TRACE=<repo root>/standalone_trace/build/rtl_trace \
  TRACE_ARGS="--low-mem"      # optional extra rtl_trace compile args
```

- Do not cap below ~30 GB: a 10 GB cap kills the compile during hierarchy collection.
- Results: `Elapsed (wall clock)` and `Maximum resident set size` in the `/usr/bin/time -v` output;
  `rtl_trace_work/rtl_trace_compile.log` has the `[Memory]` lines and `save_graph_db: ...` timing.
- Reference numbers: `COMPILE_BENCHMARK.md` ("Lumion vb2b_dbPCIe__ips" section).
- Offline builds of `rtl_trace` itself: slang's CMake fetches mimalloc from GitHub at configure
  time; pass `-DFETCHCONTENT_SOURCE_DIR_MIMALLOC=<path to an existing mimalloc source tree>` (e.g.
  `standalone_trace/build/_deps/mimalloc-src`) to avoid the download.

## Historical: NVDLA (design no longer available)

All NVDLA benchmarks were run from `/home/qsun/DVT/nvdla/hw/verif/sim_vip`.

## Prerequisites

```bash
# Flush swap and page cache for clean measurements
sudo swapoff -a && sudo swapon -a
sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
free -h   # confirm swap is 0, sufficient free RAM
```

## Binary path

```bash
RTL_TRACE=<repo root>/standalone_trace/build/rtl_trace
```

## Without --low-mem

```bash
cd /home/qsun/DVT/nvdla/hw/verif/sim_vip
rm -f /tmp/bench.db
/usr/bin/time -v $RTL_TRACE compile \
  --db /tmp/bench.db \
  --top top --relax-defparam --mfcu -Wempty-body \
  +define+NO_PERFMON_HISTOGRAM \
  -y /home/qsun/DVT/dw_ver \
  -f ../dut/dut.f \
  +incdir+$(pwd)/rtl_trace_svt_axi \
  +define+DESIGNWARE_INCDIR=$HOME/synopsys_apps/dw \
  -y ../synth_tb -y ../../outdir/nv_full/vmod/vlibs \
  +incdir+../synth_tb +incdir+../dut \
  +incdir+../../outdir/nv_full/vmod/vlibs \
  +incdir+../../outdir/nv_full/vmod/include \
  +incdir+../../outdir/nv_full/vmod/vlibs \
  +incdir+.. \
  ../synth_tb/tb_top.v ../synth_tb/csb_master.v \
  ../synth_tb/csb_master_seq.v ../synth_tb/axi_slave.v \
  ../synth_tb/id_fifo.v ../synth_tb/memory.v \
  ../synth_tb/memresp_fifo.v ../synth_tb/raddr_fifo.v \
  ../synth_tb/slave_mem_wrap.v ../synth_tb/waddr_fifo.v \
  ../synth_tb/wdata_fifo.v ../synth_tb/wstrb_fifo.v \
  ../synth_tb/clk_divider.v ../synth_tb/slave2mem_rd.v \
  ../synth_tb/slave2mem_wr.v \
  ../../outdir/nv_full/vmod/vlibs/NV_DW02_tree.v \
  ../../outdir/nv_full/vmod/vlibs/NV_DW_lsd.v \
  ../../outdir/nv_full/vmod/vlibs/NV_DW_minmax.v \
  $(pwd)/rtl_trace_svt_axi/nvdla_cvsram_axi_svt_bind.sv
```

## With --low-mem

Same command, add `--low-mem` after `--db /tmp/bench.db`.

## What to look for in output

- `save_graph_db: build_graph done elapsed_s=` — build_graph phase time
- `save_graph_db: write_file done elapsed_s=` — write phase time
- `Elapsed (wall clock) time` from `/usr/bin/time -v` — total wall time
- `User time (seconds)` — CPU user time
- `System time (seconds)` — CPU system time (high values indicate memory pressure)
- `Maximum resident set size (kbytes)` — peak RSS

## Design stats

- Signals: 1,004,882
- Endpoints: 4,316,478
- Hierarchy nodes: 131,735
