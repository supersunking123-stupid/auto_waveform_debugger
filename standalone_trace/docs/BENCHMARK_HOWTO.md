# Compile Benchmark Commands

All benchmarks were run from `/home/qsun/DVT/nvdla/hw/verif/sim_vip`.

## Prerequisites

```bash
# Flush swap and page cache for clean measurements
sudo swapoff -a && sudo swapon -a
sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
free -h   # confirm swap is 0, sufficient free RAM
```

## Binary path

```bash
RTL_TRACE=/home/qsun/AI_PROJ/auto_waveform_debugger/standalone_trace/build/rtl_trace
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
