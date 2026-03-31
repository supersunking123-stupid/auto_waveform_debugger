# EDA Tool Usage for Local Testing

**When to use this playbook:** You suspect a specific component is buggy and waveform debugging of the full-system simulation is too complex — the cone is too deep, the behavior is stateful, or the timing is too subtle to read from the system waveform. Spawn a minimal targeted simulation of that component and analyze the resulting waveform with the MCP tools. This is Rule 4 in `rtl_debug_guide.md`.

**Do not use a sandbox.** EDA tools require environment variables (`$VCS_HOME`, `$LM_LICENSE_FILE`, and others) sourced from `~/my_env/vcs.bash`. Sandbox shells do not inherit those variables, cannot reach the EDA tool installation paths, and produce waveform files that the MCP server cannot access. Run all simulation commands in a normal (non-sandboxed) shell.

---

## Step 0 — Set up the environment

Every shell that runs VCS or Verdi must source the environment first:

```bash
source ~/my_env/vcs.bash
```

Verify with `which vcs`. If the command is not found, the environment was not sourced correctly — stop and report rather than proceeding.

**Always pass `-full64` to `vcs` and `verdi` commands.** Only 64-bit mode is supported on this machine. A compile without `-full64` will fail or produce incorrect results.

---

## Step 1 — Create a scratch directory

Never run test simulations inside the project repo or the system-level design directory. Use a dedicated scratch root so that:
- Git sees no pollution
- The MCP server can always reach the output files (it runs under the same user)
- Multiple tests can run in parallel without conflict
- Files persist after the test so the agent can open the waveform in a named session

**Convention:**

```bash
SCRATCH=~/agent_scratch/$(date +%Y%m%d_%H%M%S)_<short_test_name>
mkdir -p "$SCRATCH"
cd "$SCRATCH"
```

Examples:
```
~/agent_scratch/20260330_143012_fifo_overflow/
~/agent_scratch/20260330_151800_arbiter_starvation/
~/agent_scratch/20260330_162030_cdc_sync/
```

Clean up when the investigation is complete:
```bash
rm -rf ~/agent_scratch/20260330_143012_fifo_overflow/
```

---

## Step 1.5 — Generate a filelist from the structural DB (if a DB exists)

If you have already compiled a structural DB for the design under investigation, use it to collect the DUT's source files automatically instead of hunting through the directory tree.

```python
# Dump the DUT subtree with source file annotations
rtl_trace(args=["hier", "--db", "rtl_trace.db",
                "--root", "top.u_dma",   # the instance you want to isolate
                "--depth", "10",          # deep enough to cover all sub-instances
                "--show-source", "--format", "json"])
```

Parse the `source_file` fields from the JSON, deduplicate, and write them to `$SCRATCH/files.f`. Add the testbench file at the end:

```
# files.f — auto-generated from hier --show-source
/path/to/rtl/dma_top.sv
/path/to/rtl/dma_engine.sv
/path/to/rtl/fifo.sv
/path/to/rtl/arbiter.sv
tb_top.sv
```

If you only need the source file for a single instance (e.g., to read the RTL before writing stimulus), use the faster point lookup:

```python
rtl_trace(args=["whereis-instance", "--db", "rtl_trace.db",
                "--instance", "top.u_dma.u_fifo", "--format", "json"])
# → returns module name, source file path, and definition line number
```

**If no DB exists yet:** skip this step and write the filelist manually based on your knowledge of the design, or compile the DB first (Step 4 below also compiles one for the scratch test).

---

## Step 2 — Write a minimal testbench

The goal is the smallest testbench that exercises the suspected corner case. Do not build a full UVM environment unless the DUT genuinely requires one (see Template C). A small directed testbench is faster to write, faster to debug, and produces a smaller waveform that is easier to analyze.

### Template A — Simple directed testbench (no UVM)

Use this when the DUT is a standalone module with no UVM dependencies.

```systemverilog
// tb_top.sv — minimal directed testbench
`timescale 1ns/1ps
module tb_top;

    // --- Clock and reset ---
    logic clk, rst_n;
    initial clk = 0;
    always #5 clk = ~clk;          // 100 MHz
    initial begin
        rst_n = 0;
        repeat(5) @(posedge clk);
        rst_n = 1;
    end

    // --- DUT signals ---
    logic        wr_en, rd_en;
    logic [7:0]  din, dout;
    logic        full, empty;

    // --- DUT instantiation ---
    my_fifo #(.DEPTH(8), .WIDTH(8)) dut (
        .clk(clk), .rst_n(rst_n),
        .wr_en(wr_en), .rd_en(rd_en),
        .din(din), .dout(dout),
        .full(full), .empty(empty)
    );

    // --- Waveform dump ---
    initial begin
        $dumpfile("wave.vcd");
        $dumpvars(0, tb_top);
    end

    // --- Stimulus: exercise the suspected corner case ---
    initial begin
        wr_en = 0; rd_en = 0; din = 0;
        @(posedge rst_n);

        // Fill FIFO completely, then try one more write
        repeat(8) begin
            @(posedge clk);
            wr_en = 1; din = $urandom;
        end
        @(posedge clk); wr_en = 1;   // write when full — suspected bug site

        @(posedge clk); wr_en = 0;
        repeat(20) @(posedge clk);
        $finish;
    end

endmodule
```

### Template B — Testbench with VCD+ dump (generates `.vpd`)

Replace the `$dumpfile` block with:

```systemverilog
initial begin
    $vcdpluson;    // VCD+ format, output: vcdplus.vpd
end
```

VCD+ files are larger and require DVE/Verdi to view interactively, but the MCP tools support them. Use plain VCD (`.vcd`) when you want a simple file with no extra dependencies.

### Template C — Minimal UVM testbench

Use this only when the DUT requires UVM agents or the protocol is too complex to drive with raw `initial` blocks.

```systemverilog
// tb_top.sv — minimal UVM testbench skeleton
`timescale 1ns/1ps
module tb_top;
    import uvm_pkg::*;
    `include "uvm_macros.svh"
    import my_tb_pkg::*;

    logic clk, rst_n;
    initial clk = 0;
    always #5 clk = ~clk;
    initial begin rst_n = 0; #20ns; rst_n = 1; end

    my_if m_if(clk, rst_n);

    my_dut dut (.clk(clk), .rst_n(rst_n), .port(m_if));

    initial $vcdpluson;

    initial begin
        uvm_config_db#(virtual my_if)::set(null, "uvm_test_top.*", "vif", m_if);
        run_test("my_directed_test");
    end
endmodule
```

Compile with `-ntb_opts uvm` and pass `+UVM_TESTNAME=my_directed_test` at runtime (see Step 3).

---

## Step 3 — Compile and simulate

### Non-UVM

```bash
source ~/my_env/vcs.bash

# Compile
vcs -full64 -sverilog -timescale=1ns/1ps \
    +incdir+. \
    tb_top.sv dut.sv \
    -l comp.log -o simv

# Check for errors before simulating
grep -i "^Error" comp.log

# Simulate
./simv -l sim.log
```

### UVM

```bash
source ~/my_env/vcs.bash

# Compile — include UVM runtime, enable full debug access for waveform probing
vcs -full64 -sverilog -ntb_opts uvm -debug_access+all \
    -timescale=1ns/1ps \
    +incdir+. +incdir+./dv +incdir+./rtl \
    tb_top.sv dut.sv \
    -l comp.log -o simv

grep -i "^Error" comp.log

# Simulate with a specific test
./simv +UVM_TESTNAME=my_directed_test +UVM_VERBOSITY=UVM_HIGH -l sim.log
```

### Using a file list

For more than 3–4 source files, create a `files.f`:

```
+incdir+.
+incdir+./rtl
rtl/my_dut.sv
rtl/sub_module.sv
tb_top.sv
```

Then compile with `-f files.f` instead of listing files individually.

---

## Step 4 — Compile the structural DB (optional but recommended)

If you want to use cross-link tools (`explain_signal_at_time`, `rank_cone_by_time`, etc.) on the isolated component, also build a structural DB from the same sources:

```bash
rtl_trace compile \
    --db rtl_trace.db \
    --top tb_top \
    -f files.f
```

This gives you full structural + waveform analysis on the isolated component, not just waveform browsing.

---

## Step 5 — Hand off the waveform to the MCP session tools

After simulation completes, load the waveform into a named session so you can query it with all MCP tools. Use the full absolute path.

```python
create_session(
    waveform_path="/home/qsun/agent_scratch/20260330_143012_fifo_overflow/wave.vcd",
    session_name="fifo_overflow_isolated",
    description="Targeted test: FIFO overflow corner case"
)

set_cursor(time=<failure_time>)
create_bookmark(bookmark_name="overflow", time=<failure_time>)

get_snapshot(
    signals=["tb_top.dut.wr_en", "tb_top.dut.full", "tb_top.dut.count"],
    time="BM_overflow"
)

# If you also built a structural DB, use cross-link tools
explain_signal_at_time(
    db_path="/home/qsun/agent_scratch/20260330_143012_fifo_overflow/rtl_trace.db",
    waveform_path="/home/qsun/agent_scratch/20260330_143012_fifo_overflow/wave.vcd",
    signal="tb_top.dut.full",
    time="BM_overflow"
)
```

---

## Waveform format reference

| Format | How to generate | MCP support | Notes |
|---|---|---|---|
| `.vcd` | `$dumpfile("wave.vcd"); $dumpvars(0, top);` | Full | Best default; portable, no extra setup |
| `.vpd` | `$vcdpluson;` | Full | VCD+ format; requires VCS installation |
| `.fsdb` | `$fsdbDumpfile("wave.fsdb"); $fsdbDumpvars(0, top);` | Full | Requires Verdi PLI (`-P $VERDI_HOME/...`); best for large designs |

For targeted local tests, use `.vcd`. It requires no additional tool setup and the MCP handles it natively.

---

## Common compile errors

| Error | Likely cause | Fix |
|---|---|---|
| `vcs: command not found` | Environment not sourced | `source ~/my_env/vcs.bash` |
| `Error: only 64-bit mode supported` | Missing `-full64` | Add `-full64` to the `vcs` command |
| `License checkout failed` | License server unreachable | Check VPN / network; verify `$LM_LICENSE_FILE` |
| `Unresolved module 'X'` | Missing source file or `+incdir` | Add the file or include directory |
| `timescale not set` | Missing `-timescale` flag | Add `-timescale=1ns/1ps` |
| `uvm_pkg not found` | Missing `-ntb_opts uvm` | Add `-ntb_opts uvm` for UVM testbenches |

If compile fails, read `comp.log` fully before retrying. Report the first `Error` line verbatim. Do not attempt more than two compile retries without understanding the error.

---

## When NOT to write a local test

- The failing behavior only manifests with full system stimulus (requires a specific multi-block interaction). Continue debugging the system waveform instead.
- The component has deep UVM dependencies that would take longer to replicate than to debug directly. Read the UVM log and system waveform instead.
- You already have a clear causal chain and know exactly which RTL line is wrong. Fix first, then verify with the existing regression — do not write a new test before the fix.
