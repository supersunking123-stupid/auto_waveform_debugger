# RTL Debug Guide

This document teaches you how to debug an RTL design efficiently. It covers the mindset, the process, and the common pitfalls. Read this before starting any debug task. For tool usage details, consult the playbook system (`00_ROUTER.md`).

---

## Part 1 — The Debug Process

Debugging is not random exploration. Follow this phased approach.

### Phase 0 — Orient yourself

Before touching any waveform, understand what you are looking at.

1. **Read the failure description.** Extract the failing signal, the time of failure, the test name, and any error or assertion message. If the description is vague ("test hangs", "output mismatch"), your first job is to make it concrete.
   - When reading a simulation log, do **not** dump the whole file into context. Start with targeted extraction such as `grep` for `UVM_ERROR` / `UVM_FATAL` / assertion text, plus `head` and `tail` for setup/final status. Expand only around the relevant matches.
2. **Check whether a sufficient architecture document already exists for the relevant design or subsystem.** A sufficient doc may be user-provided or crawler-generated, but it must identify the major hierarchy boundaries, interfaces/peer blocks, clock/reset domains, and the main control/dataflow landmarks relevant to the current failure.
   - If no sufficient doc exists, use `rtl-crawler-multi-agent` before active debugging.
   - If the failure ownership is unclear, crawl the full design.
   - If the failure is already localized but the subsystem is still opaque, rerun or reuse the full-design crawl and then read the generated doc for that subsystem.
3. **Understand the design context.** Use the architecture doc first, then targeted structural exploration (`hier`, `find`) to understand the module hierarchy around the failure point. You do not need to understand the entire chip — just the neighborhood of the symptom.
4. **Identify the clock domain(s).** Determine which clock drives the failing logic. If multiple clock domains are nearby, note the boundaries immediately — they will matter later.
5. **Determine the waveform time precision. Do this before passing any time value to any tool.** Call `get_signal_info` on any signal in the waveform and read the timescale it reports. The waveform timestamps are in units of the *precision* — the second number in the `` `timescale `` directive. See Rule 12 for the full explanation and conversion procedure.
6. **Set up your workspace.** Create a session, bookmark the failure time, and create a signal group for the signals mentioned in the failure. This is your anchor; you will always be able to return here.

**Do not start tracing signals until Phase 0 is complete.** Agents that skip orientation or design mapping waste time chasing irrelevant signals.

### Phase 1 — Observe the symptom

Read the waveform around the failure point. Your goal is to answer: **"What exactly went wrong, and what does the local context look like?"**

- Take a snapshot of the failing signal and its immediate neighbors (same module, same interface).
- Check the transitions on the failing signal leading up to the failure. Did it change unexpectedly, or did it *fail to change* when it should have?
- Look at the relevant control signals (enables, valids, readys, state machines) at the same time. Are they in expected states?

Record your observations in a summary. Attach bookmarks to any interesting moments you find.

### Phase 2 — Form hypotheses

Based on what you observed, list every reasonable explanation for the failure (see Rule 2 below). Then prioritize:

- **Most likely first.** If a control signal is clearly wrong, that is a better starting point than a speculative timing race.
- **Cheapest to check first.** If one hypothesis can be confirmed or eliminated with a single `get_value_at_time` call, do that before launching a deep cone trace.
- **Closest to the symptom first.** Start with the immediate driver of the failing signal and work outward. Do not jump to a distant module unless you have a specific reason.

### Phase 3 — Trace causation

Now trace backward from the symptom to find the cause. Use the Signal Investigation playbook (`03_SIGNAL_INVESTIGATION.md`).

The core loop is:

```
1. Pick the current signal of interest (starts as the failing signal).
2. Ask: "Why does this signal have this value at this time?"
   → Use explain_signal_at_time or explain_edge_cause.
3. The answer will point to one or more driver signals.
4. Check whether those driver values are correct or unexpected.
   - If correct (the driver is doing what it should, but the result is still wrong):
     the bug is in the logic *between* the driver and the current signal,
     or the design intent is wrong. Investigate the RTL.
   - If unexpected: the driver is the new signal of interest. Go to step 1.
5. Repeat until you reach a root cause (see stop criteria below).
```

**Direction of tracing:**
- **Backward (upstream)** is the default. "Why does signal X have this value?" → trace its drivers.
- **Forward (downstream)** is useful when you find an anomaly and want to know its impact. "Signal Y glitched — did it corrupt anything downstream?" → trace its loads.

### Phase 4 — Confirm the root cause

You have found the root cause when you reach one of these:

- **A primary input or testbench stimulus** that is incorrect.
- **A register** whose stored value is wrong because it was loaded incorrectly in a previous cycle (trace back to that cycle).
- **A design bug** — the RTL logic itself is wrong (missing case, wrong bit-select, inverted polarity, missing reset, etc.).
- **A timing/CDC issue** — a signal crossed a clock domain without proper synchronization, or a synchronizer's latency was not accounted for.

Confirm by checking: **"If this root cause were fixed, would the symptom disappear?"** If you can verify this by checking alternate scenarios in the waveform (e.g., the same logic works correctly at other times), your confidence increases. If not, consider spawning a local test (Rule 4).

### Phase 5 — Document

Record your findings:
- Bookmark the root cause location and the symptom location.
- Write a concise causal chain: "Signal A was wrong at time T1 because signal B was wrong at time T2 because [root cause]."
- Note which hypotheses were eliminated and why.

---

## Part 2 — The Rules

### Rule 1 — Summarize regularly to manage your context

The waveform and design hierarchy can be very complex, and your context window fills quickly. Just like human engineers do not memorize everything visible in their waveform viewer, you should not try to hold all details in context. Instead:

- **Summarize after every 3–5 tool calls.** Write a brief note to yourself: "What have I learned? What is my current theory? What should I check next?"
- **Anchor summaries to session state.** Attach bookmarks and signal groups to your summaries so you can reconstruct the context later without re-querying.
- **Discard raw data after summarizing.** You do not need to remember the exact value of every signal at every time point. Remember conclusions, not data.
- **Treat simulation logs the same way.** Do not paste full `test.log` / `sim.log` files into context. Use `grep`, `head`, and `tail` to extract the failing error, nearby context, and end-of-run status; then summarize.

### Rule 2 — Maintain a hypothesis checklist

Never jump to a conclusion. After observing the symptom, list every reasonable explanation:

**How to generate hypotheses:**
- What could cause this signal to have this value? (structural — what drives it?)
- What could cause this signal to change at this time? (temporal — what happened just before?)
- Could the data be correct but arriving at the wrong time? (timing/protocol issue)
- Could the control path be wrong while the data path is fine, or vice versa?
- Is there a reset or initialization issue?
- Is there a clock domain crossing near here?

**How to manage the checklist:**
- Mark each hypothesis as *open*, *eliminated*, or *confirmed*.
- When you eliminate a hypothesis, record *why* (one sentence) — this prevents you from accidentally re-investigating it later.
- When new evidence suggests a new theory, add it to the list immediately.
- If all hypotheses are eliminated and you are stuck, revisit your assumptions (see "When you are stuck" below).

### Rule 3 — Be suspicious of FIFOs, CDC synchronizers, and arbitration logic

These components share a dangerous property: they work correctly 99% of the time, and their failures are delayed and non-local.

**FIFOs:**
- Pointer wrap-around bugs only trigger at specific counts.
- Almost-full/almost-empty thresholds can be off by one.
- Asymmetric read/write widths can miscalculate occupancy.
- When a FIFO is near the failure, always check: pointer values, full/empty flags, and whether the occupancy count is consistent with the pointers.

**CDC synchronizers:**
- A missing or incorrect synchronizer may not cause failures in most simulations because the timing happens to be safe. But under corner-case timing (which the failing test may exercise), the unsynchronized signal is sampled at the wrong time.
- Check: is the signal properly synchronized (multi-flop chain)? Is the synchronized version used consistently on the receiving side? Could the source signal change during the synchronizer's sampling window?

**Arbitration and shared-resource logic:**
- Priority inversion, starvation, and deadlock are common under heavy load.
- When an arbiter is near the failure, check: are all requestors getting grants eventually? Is the grant signal consistent with the priority scheme? Is there a cycle where no grant is issued despite pending requests?

### Rule 3A — Build a design map before deep-tracing a complex subsystem

Signal tracing explains **local causation**. It does **not** give you a stable architectural map of a complex subsystem. In deeply hierarchical designs, those are different needs. If you keep tracing signals in circles because you do not know the subsystem's major blocks, interfaces, or control/dataflow shape, stop and build the map first.

Use `rtl-crawler-multi-agent` when any of these are true:

- No sufficient architecture doc exists for the current design or subsystem.
- You cannot name the subsystem's major child blocks, interfaces, and clock/reset boundaries from memory.
- You have made repeated driver/load traces inside one subsystem, but the next branch point is still unclear.
- You revisit the same suspect cone or same 2–3 signals twice without a new explanation.

The current crawler flow is rooted at the design `top_module`; it does not take a subsystem-root parameter. When you need local subsystem context, rerun or reuse the full-design crawl and then focus on the generated subsystem doc.

After the crawl:

1. Read `design_index.md` plus the relevant subsystem doc.
2. Summarize the subsystem boundary, major child blocks, interfaces/peers, and clocks/resets.
3. Restart the debug from that mapped boundary instead of from the last leaf signal you touched.

### Rule 4 — Escalate in two steps: crawl first, simulate second

If a component is suspicious and waveform debugging is proving difficult, do **not** jump directly from passive tracing to local simulation. First build the subsystem map; then, if the bug is still opaque, isolate the block in a local testbench.

**Step 1 — Crawler escalation:** If you have made about **8 investigation-oriented tool calls** focused on one subsystem without shrinking the suspect set or moving the causal frontier deeper, stop passive tracing and run `rtl-crawler-multi-agent` on the full design (or reuse a current full-design crawl), then read the generated doc for that subsystem. If ownership is still unclear, use the top-level map first.

**Step 2 — Local-test escalation:** After reading the generated map and making one bounded post-crawl debug pass, if you still have made more than about **10 tool calls** focused on a single block's internal signals without identifying a concrete root cause, spawn a subagent to build a standalone testbench for that component. More queries on the same block are unlikely to converge faster than targeted simulation would.

- The crawler run should produce the architectural landmarks you were missing: child blocks, interfaces, clock/reset boundaries, and likely control/dataflow branch points.
- The subagent should then create a minimal testbench exercising the suspected corner case.
- Consult `EDA_USE.md` for simulator setup on this machine.
- This is effective for any component where targeted stimulus reveals internal behavior more directly than passive observation: counters and pointers (exercise boundary conditions), arbiters (drive concurrent requests), state machines (force edge-case transitions), CDC logic (vary sampling edge timing), FIFOs (fill-drain across the wrap boundary).
- The resulting waveform can be loaded into an MCP session and analyzed with the same playbooks as a full-system waveform.

### Rule 5 — Distinguish the symptom from the cause

The signal that fails is almost never the signal that is buggy. Train yourself to think in causal chains:

- "Signal X is wrong" → symptom.
- "Signal X is wrong *because* signal Y is wrong" → one step closer.
- "Signal Y is wrong *because* the state machine entered state STALL when it should not have" → approaching root cause.
- "The state machine entered STALL because the FIFO full flag was asserted one cycle early due to a pointer comparison bug" → root cause.

Each step in the chain should be supported by waveform evidence. If you cannot explain a link in the chain, that link is your next investigation target.

### Rule 6 — Check the temporal neighborhood, not just the failure instant

Bugs rarely manifest at a single time point. Always check:

- **Before the failure:** What changed in the 5–10 clock cycles leading up to the failure? Use `get_transitions` or `trace_with_snapshot` with `cycle_offsets`.
- **After the failure:** Does the design recover, or does the error cascade? This tells you whether the issue is transient or persistent.
- **Earlier occurrences:** Use `find_condition` to check whether the same failure condition occurred earlier in the simulation. If it did not, the failing test is exercising a specific scenario — identify what makes it different.

### Rule 7 — Follow the data, not your assumptions

When the waveform contradicts your mental model of the design, **trust the waveform**. Common traps:

- "This signal should be high here" — check it, do not assume it.
- "This module is simple, it cannot be the problem" — simple modules can still be mis-connected or have wrong bit-widths.
- "This worked in the last simulation" — the current test may exercise a different path.

If you find yourself rationalizing why the waveform "must be wrong," you are probably on the wrong track. Re-examine your assumptions.

### Rule 8 — Limit your trace depth to stay effective — but not your investigation depth

Deep cone traces produce overwhelming results. Use constraints to keep individual trace calls actionable. **But do not confuse limiting a single trace call with stopping the investigation early.** Limiting trace depth controls how much structural output you receive per call; it does not mean you stop probing once you find the first unexpected value. After receiving a shallow trace result, you are expected to follow each suspicious driver further until you reach the true root cause.

See also Rule 11 — the two rules work together: Rule 8 keeps individual calls manageable; Rule 11 ensures you keep going layer by layer.

Deep cone traces produce overwhelming results. Use constraints to keep traces actionable:

- Start with `depth=3` and increase only if needed.
- Use `stop_at="clk|reset"` to prevent tracing through clock tree and reset logic (these are almost never the root cause in functional bugs).
- Use `include` and `exclude` regex filters to focus on the relevant module.
- If `rank_cone_by_time` returns more than ~15 signals, narrow the window or increase the depth constraint before analyzing the results.

**Important:** `depth` limits a single trace call, not the investigation. After you consume a shallow result, you call the tool again on the next suspect. Keep calling until Rule 11's stop criteria are met.

### Rule 9 — Normalize values when cross-referencing between sources

Waveform signals, simulation logs, VIP messages, and specification documents frequently use **different conventions to represent the same quantity**. If you do not normalize before comparing, you will see phantom mismatches that waste your time or, worse, cause you to dismiss a real clue as a representation issue.

**Common normalization traps:**

- **0-based vs. 1-based counts.** The AXI protocol defines `ARLEN`/`AWLEN` as 0-based (a value of 0 means 1 beat). However, many verification IP components (e.g., Synopsys SVT AXI VIP) report burst length as 1-based in their transaction logs. An `AWLEN=7` on the waveform corresponds to `length=8` in the VIP message. If you compare them directly, every transaction appears off by one.
- **Byte addresses vs. word addresses.** Logs may report byte addresses while the waveform shows word-aligned addresses (shifted by 2 or 3 bits), or vice versa.
- **Radix differences.** A log may print an ID as decimal `216` while the waveform shows `0xd8`. These are the same value, but a direct string comparison will miss the match.
- **Signal name mapping.** VIP logs may use protocol-level names (`AWADDR`, `BRESP`) while the waveform hierarchy uses design-specific names (`u_axi_if.aw_addr`, `u_axi_if.b_resp`). Do not assume they will not match just because the names differ.
- **Bit ordering and packing.** Multi-field buses (e.g., a packed struct on a data bus) may be displayed MSB-first in the waveform but unpacked field-by-field in the log.
- **Enumeration encoding.** A state machine may show `3'b010` on the waveform while the log prints `STATE_WRITE`. Consult the RTL source for the encoding.

**What to do:**
1. When you first cross-reference a log entry against a waveform value, explicitly check whether the source uses a different base, offset, or convention. Do this once and record the mapping.
2. If a value appears to be "off by one" between two sources, suspect a 0-based/1-based convention mismatch before suspecting a real bug.
3. When using `find_value_intervals` or `find_condition` to search for a value mentioned in a log, apply the normalization first. For example, if the log says `length=8` and you know the VIP is 1-based, search for `awlen == 7` on the waveform, not `awlen == 8`.

### Rule 10 — Report MCP tool failures — never silently fall back to source reading

When an MCP tool call returns an error or an unexpected empty result, **stop and report the failure immediately**. Do not silently switch to reading source files as a substitute. The MCP tools are the primary debugging interface — they exist because raw source reading does not scale to real designs and lacks the temporal evidence that waveform tools provide.

**What to do when a tool fails:**

1. Read the error message carefully. Many failures are fixable in one attempt:
   - `DB not found` or `compile error` → the structural DB has not been compiled yet; run `rtl_trace compile`.
   - `Signal not found in waveform` → the path format differs between the structural DB and the waveform; use `rtl_trace find` to locate the signal's exact DB path, and `list_signals` to check the waveform namespace.
   - `Waveform file not found` → verify the path and extension (`.vcd`, `.fsdb`, `.fst`).
   - `Timeout` → try `rtl_trace_serve_start/query/stop` instead of the one-shot wrapper, which has a default timeout.
2. Make at most **one or two targeted recovery attempts**. If the tool still fails, do not continue guessing.
3. **Report the failure explicitly**: state the tool you called, the exact arguments, and the full error message. This feedback directly improves the toolkit.
4. Continue the investigation using other available MCP tools — structural-only tools if waveform tools fail, or waveform-only tools if the DB is unavailable. Do not abandon the MCP layer entirely.

**What never to do:**
- **NEVER open a `.v` or `.sv` source file to trace a signal or understand connectivity.** Source reading is not a fallback for a failed tool call — it is a different activity (code review) that bypasses all simulation evidence. The correct fallback for a failed `explain_signal_at_time` is to report the failure and try `trace_with_snapshot` or `rtl_trace trace`, not to open the file. Source reading is only permitted after MCP tools have identified the guilty instance and you need to read its logic to confirm the bug.
- Do not suppress or paraphrase error messages — report them verbatim.
- Do not retry the identical failing call more than twice without changing something.

---

### Rule 11 — Trace the driver cone layer by layer — do not stop at the first wrong signal

Finding a signal with an unexpected value is not the root cause. It is the next signal to investigate. Every driver of that signal must be checked, and if any driver is also wrong, its drivers must be checked in turn. **The investigation continues until you reach a signal whose wrong value can be fully explained by a concrete design flaw, wrong primary input, or a register that was incorrectly loaded in a prior cycle.**

**The correct loop:**

```
1. Signal X has an unexpected value at time T.
2. Call explain_signal_at_time (or trace_with_snapshot) on X.
3. Inspect every RHS term in X's driver cone:
   a. Check the waveform value of each driver at time T.
   b. For each driver whose value is also unexpected → it becomes the new X. Go to step 2.
   c. For each driver whose value is correct → the bug is in the logic connecting
      that driver to X, not in the driver itself.
4. Repeat until every branch of the cone terminates at a root cause (see stop criteria).
```

**Stop criteria — you have reached the root cause when:**
- A driver is a primary input or testbench stimulus that is provably wrong.
- A driver is a register whose stored value was loaded incorrectly in a *previous* cycle (go to that cycle and repeat from step 1).
- A driver is driven by correct inputs but the combinational logic that connects them to the output is wrong by design (wrong operator, wrong bit-select, inverted condition, etc.).
- A driver is properly connected to a clock-domain crossing but the synchronizer is missing or broken.

**Common mistake to avoid:**

> Signal Y is 0 when it should be 1. Y is assigned `Y = A & B`. A is 1, B is 0.
> ❌ Wrong: "B is the problem — fix whatever drives B."
> ✓ Right: "B is 0 unexpectedly. Now trace B's drivers. Is B's value wrong because of its own driver, or because of an earlier register?"

Do not patch Y's driver to work around a wrong B. If B has its own wrong driver, that driver has its own wrong driver, and so on. Only fixing the actual root leaves the design correct everywhere, not just at the symptom site.

**Practical guidance:**
- Call `explain_signal_at_time` once per cone level, using the previous result to identify which driver to investigate next.
- Use `trace_with_snapshot` with `clock_path` and `cycle_offsets=[-3,-2,-1,0]` to observe how values evolve across multiple cycles — this often reveals exactly which cycle and which layer the error first appeared.
- Use `rank_cone_by_time` to prioritize which branch to descend first when a cone has many drivers.
- A shallow `depth=3` trace on each layer is cleaner than one overwhelming deep trace. Call multiple times rather than widening depth until the output is unreadable.

---

### Rule 12 — Determine the waveform time precision before using any time argument

Waveform timestamps are integers, but their unit depends on the `` `timescale `` directive used during simulation — specifically the **precision** (the second number). If you pass a time value without knowing the precision, you will silently query the wrong time and get results that look plausible but are completely wrong.

**How `` `timescale `` works:**

```
`timescale <time_unit> / <time_precision>
```

- `time_unit` — the unit for delay statements in RTL (e.g., `#10` means 10 ns).
- `time_precision` — the smallest representable time step. **This is the unit of every timestamp in the waveform.**

| `` `timescale `` | Time precision | To express 100 ns, pass… |
|---|---|---|
| `1ns/1ns` | 1 ns | `100` |
| `1ns/1ps` | 1 ps | `100000` |
| `1ns/100fs` | 100 fs | `1000000` |
| `1ps/1ps` | 1 ps | `100000` |

**How to determine the precision:**

Call `get_signal_info` on any signal before making your first time-based query:

```python
get_signal_info(path="top.clk")
```

Read the `timescale` or `time_unit` field in the result. That field tells you what one waveform timestamp integer represents.

You can also verify by calling `analyze_pattern` on the clock and comparing the reported period against the known clock frequency:

```python
analyze_pattern(path="top.clk", start_time=0, end_time=10000000)
# If the reported period is 10000 and you know the clock is 100 MHz (10 ns period),
# then 10000 units = 10 ns → 1 unit = 1 ps → precision is 1 ps.
```

**Rules:**

1. **Always call `get_signal_info` on a signal before your first time-based query in any new waveform.** Do this even if you think you know the timescale.
2. **Convert all time values before passing them.** If a failure report says "error at 150 ns" and precision is 1 ps, pass `150000`, not `150`.
3. **Do not assume 1 ns precision.** The most common simulation setup is `` `timescale 1ns/1ps ``, which means precision is 1 ps. Assuming 1 ns will put you off by 1000×.
4. **Record the precision in your first session bookmark description** so you do not need to re-check it later. Example: `create_bookmark(bookmark_name="failure", time=150000, description="150 ns — precision 1ps")`.

---

### Rule 13 — Stop and report when results are untrustworthy; never guess forward

When tool results are empty, contradictory, or cannot be explained, the correct response is to **stop, diagnose the failure, and report**. It is never correct to fill the gap with assumptions and continue as if the results were valid.

**Why this matters:** A single wrong assumption, propagated through five subsequent tool calls, creates a false narrative that is harder to undo than the original problem. The agent appears to be making progress while actually drifting further from the truth. Stopping early costs a few tokens; guessing forward wastes the entire investigation.

#### Stop triggers — halt and diagnose when you observe any of these

| Observation | What it likely means |
|---|---|
| Two or more consecutive waveform queries return empty results or zero transitions | Wrong time range — likely a timescale unit error or querying before simulation start |
| Signal values are all `0`, `x`, or constant across a range where activity is expected | Wrong signal path, wrong time range, or the signal was never driven |
| Results contradict each other (e.g., a signal is 1 and 0 at the same time from two different calls) | Path mismatch — two calls may be referencing different signals |
| You have made three or more consecutive tool calls based on an assumption you have not verified | The assumption may be wrong; stop and verify it before continuing |
| A conclusion requires more than one unverified assumption to hold simultaneously | The chain is speculative; verify each link independently before proceeding |

#### What to do when a stop trigger fires

1. **Stop the current line of investigation immediately.** Do not make another tool call in the same direction.
2. **Diagnose the source of the bad results.** Common causes:
   - Wrong time unit (Rule 12) — re-check timescale with `get_signal_info`.
   - Wrong signal path — use `list_signals` or `rtl_trace find` to verify the path exists.
   - Time range outside simulation bounds — call `get_signal_overview` with a very wide range to confirm where the simulation actually has data.
   - Stale or missing DB — the structural DB may not match the waveform.
3. **Write an explicit status summary** before doing anything else: what you know from verified evidence, what assumptions you made, which of those assumptions may be wrong, and what you need to verify next.
4. **Report the situation.** If you cannot diagnose the source of the bad results within two recovery attempts, report clearly: the tool called, the arguments, the result, and why it is suspicious. Do not continue debugging on top of unresolved uncertainty.

#### What never to do

- Do not state a conclusion as fact if it is based on an assumption rather than a tool result.
- Do not use phrases like "this signal is probably X" or "the value is likely Y" as the basis for the next tool call. Verify first, then call.
- Do not continue after two consecutive empty or nonsensical results without first confirming the time range and signal paths are correct.

---

### Rule 14 — Respect the golden boundary: vendor VIP protocol checkers are golden; home-grown models are not

Do not invent a giant "golden" boundary around the entire testbench. In this project, the default golden boundary is **EDA-vendor protocol VIP monitors/checkers**. If one of those fires, trust the checker. But do **not** extend that trust automatically to home-grown memory models, BFMs, scoreboards, reference models, monitors, or assertions. Those components may themselves be buggy and may be the source of the bad interface value that the VIP observed.

**Trust levels:**

| Component | Trust level | How to treat it |
|---|---|---|
| **EDA-vendor VIP monitors / protocol checkers** | Golden | Trust the checker; trace the interface signal that reached it |
| **Home-grown memory models / BFMs / monitor wrappers** | Not golden | If they drive the failing signal, debug them directly |
| **Home-grown assertions / scoreboards / reference models** | Evidence, not proof | Cross-check against waveform + driver trace before treating them as authoritative |
| **Testbench stimulus generators** | Inputs, not golden | They may be wrong; prove it from waveform evidence before concluding |

**How to apply:**

1. **Never hypothesize that an EDA-vendor VIP checker is buggy** unless the user explicitly says to check the VIP configuration or connection.
2. **Start from the interface signal the VIP/checker flagged, then trace its immediate driver.** Do not assume the source is the DUT until the driver trace shows that.
3. If the driver is **DUT RTL**: proceed into the DUT.
4. If the driver is **home-grown testbench RTL** (memory model, BFM, wrapper, monitor logic): the investigation shifts to that testbench logic. This is not questioning the VIP; it is identifying the real producer of the wrong value.
5. If a **home-grown assertion/checker/scoreboard** fires, treat it as a strong clue, not an unchallengeable fact. Cross-check the waveform and structural trace before declaring it correct.
6. **If you find yourself explaining why a vendor VIP "shouldn't have fired"**, stop. The VIP did fire. The question is which RTL block produced the wrong value that reached it.

---

## Part 3 — Common Bug Patterns

When forming hypotheses (Rule 2), check whether the symptom matches any of these common patterns. This can accelerate your investigation.

### Missing or incorrect reset
**Symptom:** A register or state machine has an unexpected value at the beginning of operation (but not necessarily at time 0 — the reset may be released late or re-asserted).
**Check:** Trace the reset signal to the failing register. Is it asserted when expected? Is the register in the reset sensitivity list? Is the reset polarity correct?

### Off-by-one in counters or pointers
**Symptom:** A counter, pointer, or address is one value too high or too low, causing overflow, underflow, or out-of-range access.
**Check:** Compare the counter value to its expected range. Check boundary conditions (is the comparison `>=` vs `>`, `<` vs `<=`?).

### Handshake / protocol violation
**Symptom:** A valid/ready (or req/ack) interface drops a transaction, duplicates one, or deadlocks.
**Check:** Look at the handshake signals in the 5–10 cycles around the failure. Was `valid` asserted without `ready`? Was `ready` asserted without `valid`? Did both sides follow the protocol (valid must hold until ready, etc.)?

### Bus width or bit-select mismatch
**Symptom:** Data is partially correct but has wrong bits in certain positions, or upper/lower bits are zero/garbage.
**Check:** Use structural trace to verify that the bit-widths match at every connection point. Check for truncation, zero-extension, or sign-extension mismatches.

### Stale data from pipeline or register
**Symptom:** The output data is correct but corresponds to a *previous* transaction, not the current one.
**Check:** Look at the pipeline stage enables and valid qualifiers. Is the data being forwarded correctly? Is there a bypass path that should be active but is not?

### Priority inversion or starvation
**Symptom:** One requestor never gets a grant, or gets a grant too late, causing a timeout or overflow elsewhere.
**Check:** Watch the arbiter for 20–50 cycles. Does every requestor eventually get served? Compare the grant pattern to the priority scheme.

### False mismatch from normalization differences (not a real bug)
**Symptom:** A log or VIP message reports a value that appears to disagree with the waveform, but the design otherwise behaves correctly. Typically the discrepancy is exactly 1, or the values look unrelated until you convert radix.
**Check:** Before concluding there is a real mismatch, apply Rule 9. Convert both values to the same radix and the same convention (0-based vs. 1-based, byte vs. word address). If they match after normalization, this is not a bug — move on. If you spend time investigating a "mismatch" that turns out to be a representation difference, you have wasted debug cycles on a false lead.

---

## Part 4 — When You Are Stuck

If you have exhausted your hypothesis checklist and still cannot find the root cause:

1. **Map the current subsystem before more tracing.** If you do not already have a sufficient architecture doc for the block you are stuck in, use or refresh the full-design `rtl-crawler-multi-agent` output now. Read the generated subsystem doc, identify the major child blocks and interfaces, then restart from the subsystem boundary.

2. **Widen the observation window.** You may be looking at the wrong time range. Search for the *first* occurrence of the anomaly — the failure you are seeing may be a secondary effect of an earlier bug.

3. **Switch direction.** If you have been tracing drivers (backward), try tracing loads (forward) from a known-good signal to see where it goes wrong. The corruption point may be easier to find from the other side.

4. **Check a different clock domain.** If the failure is near a CDC boundary, the root cause may be in the other clock domain entirely. Switch your investigation to that side.

5. **Compare with a passing test.** If you have a waveform from a passing test, compare the signal values at the same logical point (same transaction number, same state machine state, not necessarily the same absolute time). The difference often reveals the bug.

6. **Question the testbench (with caution — see Rule 14).** The testbench itself may be incorrect — wrong expected values, wrong timing constraints, or wrong stimulus. Rule 14 still applies: trust EDA-vendor VIP protocol checkers, but do not automatically treat home-grown models, assertions, or scoreboards as golden. Use the driver trace and waveform to decide whether the source is DUT RTL or testbench RTL.

7. **Spawn a subagent for local verification (Rule 4).** Isolate the suspicious block and test it independently. Use this after the crawler pass if the system-level waveform is still too complex to debug efficiently.

8. **Ask for help.** If you have spent significant effort and are not making progress, summarize your findings and present them. A clear summary of what you have checked and eliminated is valuable even without a final answer.

---

**If none of the above is working — stop.**

Continuing to generate tool calls when results are consistently empty or nonsensical is not debugging — it is token consumption with no expected benefit. The threshold is low: if you have attempted two recovery strategies and results are still untrustworthy, **stop and report** rather than trying a third. Apply Rule 13: write a status summary of what is known, what is assumed, and what is blocking you, then present it. An honest "I am blocked and here is why" is more useful than a fabricated causal chain built on unverified assumptions.
