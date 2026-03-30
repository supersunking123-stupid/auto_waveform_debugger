# RTL Debug Guide

This document teaches you how to debug an RTL design efficiently. It covers the mindset, the process, and the common pitfalls. Read this before starting any debug task. For tool usage details, consult the playbook system (`00_ROUTER.md`).

---

## Part 1 — The Debug Process

Debugging is not random exploration. Follow this phased approach.

### Phase 0 — Orient yourself

Before touching any waveform, understand what you are looking at.

1. **Read the failure description.** Extract the failing signal, the time of failure, the test name, and any error or assertion message. If the description is vague ("test hangs", "output mismatch"), your first job is to make it concrete.
2. **Understand the design context.** Use structural exploration (`hier`, `find`) to understand the module hierarchy around the failure point. You do not need to understand the entire chip — just the neighborhood of the symptom.
3. **Identify the clock domain(s).** Determine which clock drives the failing logic. If multiple clock domains are nearby, note the boundaries immediately — they will matter later.
4. **Set up your workspace.** Create a session, bookmark the failure time, and create a signal group for the signals mentioned in the failure. This is your anchor; you will always be able to return here.

**Do not start tracing signals until Phase 0 is complete.** Agents that skip orientation waste time chasing irrelevant signals.

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

### Rule 4 — Spawn subagents for local verification

If a component is suspicious and waveform debugging is painful (the cone is too deep, the behavior is stateful and hard to trace, or the timing is too subtle to see in the waveform), spawn a subagent to build a standalone test for that component.

- The subagent should create a minimal testbench exercising the suspected corner case.
- Consult `EDA_USE.md` for simulator setup on this machine.
- This is especially effective for FIFOs, CDC logic, arbiters, and complex state machines — components where targeted stimulus is more revealing than passive waveform observation.

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

### Rule 8 — Limit your trace depth to stay effective

Deep cone traces produce overwhelming results. Use constraints to keep traces actionable:

- Start with `depth=3` and increase only if needed.
- Use `stop_at="clk|reset"` to prevent tracing through clock tree and reset logic (these are almost never the root cause in functional bugs).
- Use `include` and `exclude` regex filters to focus on the relevant module.
- If `rank_cone_by_time` returns more than ~15 signals, narrow the window or increase the depth constraint before analyzing the results.

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

1. **Widen the observation window.** You may be looking at the wrong time range. Search for the *first* occurrence of the anomaly — the failure you are seeing may be a secondary effect of an earlier bug.

2. **Switch direction.** If you have been tracing drivers (backward), try tracing loads (forward) from a known-good signal to see where it goes wrong. The corruption point may be easier to find from the other side.

3. **Check a different clock domain.** If the failure is near a CDC boundary, the root cause may be in the other clock domain entirely. Switch your investigation to that side.

4. **Compare with a passing test.** If you have a waveform from a passing test, compare the signal values at the same logical point (same transaction number, same state machine state, not necessarily the same absolute time). The difference often reveals the bug.

5. **Question the testbench.** The testbench itself may be incorrect — wrong expected values, wrong timing constraints, or wrong stimulus. Do not blindly trust the checker.

6. **Spawn a subagent (Rule 4).** Isolate the suspicious block and test it independently. Sometimes the system-level waveform is too complex to debug efficiently.

7. **Ask for help.** If you have spent significant effort and are not making progress, summarize your findings and present them. A clear summary of what you have checked and eliminated is valuable even without a final answer.
