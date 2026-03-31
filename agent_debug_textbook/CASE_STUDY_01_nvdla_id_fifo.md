# Case Study 01 — NVDLA ID FIFO Read Pointer Off-by-Two

**Design:** NVDLA (NVIDIA Deep Learning Accelerator), open-source RTL
**Test:** `cc_alexnet_conv5_relu5_int16_dtest_cvsram`
**Bug type:** Off-by-one (actually off-by-two) in a pointer wrap condition
**Difficulty:** High — the symptom is an AXI protocol violation at the interface, with the root cause buried 3 levels of causation deep in a FIFO implementation detail. The failure only triggers after the read pointer has cycled through 446 entries, making it rare in shorter tests.
**Significance:** A weak model following the playbooks found this bug. GPT-4.5 did not.

---

## The failure

Simulation error from `test.log`:

```
UVM_ERROR @ 412370.00ns: [AMBA:AXI3:rlast_asserted_for_last_read_data_beat]
  xact { TYPE(READ) LENGTH('d1) RLAST('b0) ID('h8) ADDR('h50025000)
          START_TIME(397090.00ns) }
  current_data_beat_num: 'd0
```

The AXI master VIP fired a protocol violation: a single-beat read burst (`LENGTH=1`, i.e., ARLEN=0) had `RLAST=0` on its only data beat. The slave generated two beats instead of one.

---

## Why this bug is hard to find

This is a deferred-effect bug. The wrong behavior — the read pointer wrapping two addresses early — occurs long before the symptom appears. By the time the VIP fires the error, the pointer has already wrapped and corrupted the FIFO's read order. The sequence is:

1. Bug site: read pointer wraps at address 445 instead of 447.
2. Effect: 2-entry desynchronization between the ID FIFO and the parallel memory response FIFO.
3. Symptom: the slave uses stale metadata (wrong burst length) to generate RLAST, violating AXI protocol.

There is no assertion at the bug site. The RTL compiles and simulates cleanly. The failure only appears when the read pointer reaches position 445, which happens after ~446 read transactions. In short tests this never occurs.

An investigator looking only at the VIP error message sees an RLAST problem. There is nothing in the error that points to a FIFO pointer. Without systematic backward tracing, the natural hypothesis is a bug in the burst-length generation logic — which is correct by design.

---

## The debug workflow (how the agent did it)

### Phase 0 — Orient

The agent read the failure message and correctly identified:
- Failing signal: `RLAST` on the AXI read data channel
- Failure time: `412370 ns`
- Time precision: checked via `get_signal_info` → `1ns/1ps` → all timestamps in picoseconds
- Relevant hierarchy: `top.nvdla_top.u_nvdla_cvsram_axi_svt_bind`

```python
get_signal_info(vcd_path="wave.fsdb", path="top.nvdla_top....")
# → timescale: 1ns/1ps → precision 1ps
# Failure time in waveform units: 412370 * 1000 = 412370000
```

Workspace setup:
```python
create_session(waveform_path="wave.fsdb", session_name="nvdla_rlast_bug")
set_cursor(time=412370000)
create_bookmark(bookmark_name="rlast_fail", time=412370000,
                description="412370ns — RLAST=0 on single-beat burst ID=8")
```

### Phase 1 — Observe the symptom

The agent took a snapshot of the AXI read data channel at the failure time:

```python
get_snapshot(signals=["...rlast", "...rready", "...rvalid", "...rid"],
             time="BM_rlast_fail")
```

Result: `RLAST=0`, `RVALID=1`, `RREADY=1`, `RID=8`. A data beat was accepted by the master with `RLAST` deasserted, meaning the slave signaled "more data coming" for a burst it should have terminated.

The agent noted: "The slave believes this is a multi-beat burst. Either it received the wrong burst length, or it is misreading the burst length for this transaction."

### Phase 2 — Identify suspects

The agent called `rank_cone_by_time` on the RLAST signal to find which upstream signals had changed most recently before the failure:

```python
rank_cone_by_time(
    db_path="rtl_trace.db",
    waveform_path="wave.fsdb",
    signal="top....rlast",
    time="BM_rlast_fail",
    window_start=412000000,   # 412000ns
    window_end=412370000      # 412370ns
)
```

Top-ranked signals: the burst-length counter and the ID FIFO read data bus (`rdid_fifo_rd_bus`). The agent focused on `rdid_fifo_rd_bus` because it carries per-transaction metadata including burst length — a natural intermediate in the causal chain.

```python
get_value_at_time(path="...rdid_fifo_rd_bus", time=412370000, radix="hex")
# → 0x01080000000050025380
# Decoded: len=1, id=8, addr=0x50025380
```

This was wrong. The failing transaction had `ARLEN=0` (1 beat) and `ADDR=0x50025000`. The FIFO was returning `len=1` (2 beats) and a different address — metadata belonging to a different transaction.

The agent bookmarked this:
```python
create_bookmark(bookmark_name="fifo_desync", time=412370000,
                description="ID FIFO returning wrong metadata: len=1 addr=0x50025380 instead of len=0 addr=0x50025000")
```

### Phase 3 — Explain causation layer by layer

**Layer 1: why is the FIFO returning wrong metadata?**

```python
explain_signal_at_time(
    db_path="rtl_trace.db",
    waveform_path="wave.fsdb",
    signal="...rdid_fifo_rd_bus",
    time="BM_fifo_desync"
)
```

Result: `rdid_fifo_rd_bus` is driven by the RAM read port, addressed by `rd_adr`. The value at `rd_adr` at failure time was `9'd2`, not `9'd0` as expected for the transaction at the head of the queue.

**Layer 2: why is `rd_adr` pointing to position 2 instead of 0?**

The agent traced back `rd_adr` in time using `get_signal_overview`:

```python
get_signal_overview(path="...rd_adr", start_time=410000000, end_time=412500000, resolution="auto")
```

This revealed a sudden jump: `rd_adr` went from `445` directly to `0` at around `411930 ns`, skipping two entries. The write pointer had already advanced past 445 to 447, so positions 446 and 447 contained valid entries that `rd_adr` would never read.

The agent bookmarked the wrap:
```python
create_bookmark(bookmark_name="ptr_wrap", time=411930000,
                description="rd_adr wrapped 445→0, skipping entries 446 and 447")
```

**Layer 3: why did `rd_adr` wrap at 445 instead of 447?**

```python
explain_edge_cause(
    db_path="rtl_trace.db",
    waveform_path="wave.fsdb",
    signal="...rd_adr",
    time="BM_ptr_wrap",
    edge_type="anyedge",
    direction="backward"
)
```

This returned the driving assignment: `rd_adr_next_popping`, governed by:

```verilog
wire [8:0] rd_adr_next_popping = (rd_adr == 9'd445) ? 9'd0 : (rd_adr + 1'd1);
```

The wrap condition is `445`. The write pointer (line 139 of the same file) wraps at `447`:

```verilog
wr_adr <= (wr_adr == 9'd447) ? 9'd0 : (wr_adr + 1'd1);
```

Root cause confirmed: the read and write pointers have different wrap values. The FIFO has 448 entries (addresses 0–447), but the read pointer wraps 2 addresses early, permanently skipping positions 446 and 447.

### Phase 4 — Verify

The agent confirmed the causal chain holds in both directions:

1. **Forward check:** after the wrap at `411930 ns`, the agent verified that `rd_adr` was at position 2 exactly when the failing transaction's metadata was needed. The 2-entry skip exactly accounts for the desynchronization.

2. **Scope check:** `whereis-instance` confirmed that both `wrid2mem_fifo` and `rdid2mem_fifo` in `axi_slave.v` instantiate the same `id_fifo` module, so both are affected by the same bug.

3. **Rarity check:** `find_value_intervals` confirmed the read pointer reaches position 445 only once in this simulation — explaining why shorter tests never trigger the bug.

---

## The bug

**File:** `hw/verif/synth_tb/id_fifo.v`, line 160

```verilog
// BUG: wrap at 445, but FIFO has 448 entries (0–447)
wire [8:0] rd_adr_next_popping = (rd_adr == 9'd445) ? 9'd0 : (rd_adr + 1'd1);
```

**Fix:**

```verilog
wire [8:0] rd_adr_next_popping = (rd_adr == 9'd447) ? 9'd0 : (rd_adr + 1'd1);
```

---

## What the playbooks contributed

| Playbook step | What it prevented |
|---|---|
| Phase 0: check time precision | Agent correctly converted 412370 ns → 412370000 ps before every query. Without this, all waveform queries would have landed 1000× too early and returned empty results. |
| Phase 1: observe the symptom fully before hypothesizing | Agent did not immediately assume "the RLAST logic is wrong." It first established that the slave was using wrong burst-length metadata, narrowing the search space before any structural trace. |
| Phase 2: `rank_cone_by_time` before deep tracing | Identified `rdid_fifo_rd_bus` as the highest-ranked suspect in ~1 tool call, avoiding a wide exploratory trace through the burst-length generation logic (which was correct). |
| Phase 3: layer-by-layer descent | The agent traced three levels deep — RLAST → FIFO output → read address → wrap condition — rather than stopping at the first anomaly (wrong FIFO output) and concluding "the FIFO is broken." |
| Rule 13: stop-trigger discipline | The agent got one unexpected value (wrong FIFO metadata) and immediately verified the time precision and signal path before continuing, rather than guessing past the anomaly. |

---

## Lessons for future debugging

1. **AXI protocol violations at the VIP are rarely caused by the VIP-facing logic.** The RLAST generation was correct given its inputs. The bug was in how those inputs (burst length from the ID FIFO) were being corrupted upstream.

2. **FIFO pointer bugs are deferred by design.** The mismatch between read and write wrap values did not cause an immediate failure — it caused a slow drift that only manifested after enough transactions had been processed. When a FIFO is in the causal neighborhood of a failure, always check both pointer wrap conditions explicitly.

3. **Off-by-two is harder to spot than off-by-one.** A 1-entry difference between read and write wrap values is immediately visible as a fencepost error. A 2-entry difference (`445` vs `447`) looks like a specific address choice, not a typo. The structural trace was necessary to surface the comparison at all.

4. **`get_signal_overview` over a wide range is the right tool for pointer drift.** Rather than calling `get_transitions` on `rd_adr` and getting hundreds of individual value changes, `get_signal_overview` revealed the abnormal wrap at a glance.
