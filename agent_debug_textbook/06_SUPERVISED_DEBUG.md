# Playbook 06 — Supervised Debug (Two-Agent Architecture)

**Role:** Two agents collaborate: a **Debugger** that executes the debug workflow, and a **Supervisor** that enforces playbook compliance, catches errors, and challenges conclusions. This architecture compensates for model weaknesses — carelessness, memory loss, hallucination, and poor reasoning over long data sequences.

**When to use:** The model executing the debug is prone to drifting from playbooks, making tool calls that don't match stated intent, or drawing conclusions from unverified assumptions. If a single-agent debug session has already failed, or the debug is especially high-risk/ambiguous, retry with this two-agent setup. Do not use supervised mode by default for every short, routine debug.

**Prerequisites:** Same as Playbook 04 — a compiled structural database, a waveform file, and a sufficient architecture document for the relevant design or subsystem. If that document does not already exist, Phase 0 must route through `08_DESIGN_MAPPING.md` before active debugging. If the failure involves architecturally relevant unknown (`X`) values, supervised debug must route through `09_X_TRACING.md` before ordinary Playbook 04 phase work begins. The Supervisor does not call MCP tools directly; it operates by reviewing the Debugger's actions and steering via structured feedback.

---

## The two roles

### Debugger

The Debugger follows Playbooks 01–09 and `rtl_debug_guide.md` exactly as written. It:
- Executes all MCP tool calls
- Records observations and summaries after each phase
- Presents compact batches of planned tool calls, then reports results and conclusions to the Supervisor before moving to the next phase
- Generates Python scripts when instructed by the Supervisor (or proactively for long data sequences)

### Supervisor

The Supervisor does **not** call MCP tools. It:
- Holds the playbook rules in active context and enforces them
- Reviews each proposed phase batch, plus any high-risk individual call: checks that the arguments match the Debugger's stated intent
- Reviews every summary the Debugger produces: checks that conclusions follow from the actual tool results, not from assumptions
- Challenges the Debugger with adversarial questions when a conclusion seems unsupported
- Reviews Python scripts before the Debugger executes them
- Decides when to backtrack to the error-scenario anchor session
- Keeps a running log of verified facts vs. unverified assumptions

---

## Communication protocol

The agents alternate in compact batches. Each phase of the debug workflow follows this loop. For ordinary bugs, use Phase 0–4 from Playbook 04. For harmful-`X` bugs, run Playbook 09 first; once Playbook 09 has produced the creator-block / subsystem-boundary evidence table, transition into Playbook 04 inside that isolated creator region while reusing that evidence instead of repeating the same audit.

```
┌──────────────────────────────────────────────────────┐
│                    Phase N loop                       │
│                                                       │
│  1. Debugger states the phase goal and proposes the   │
│     next batch of 1–3 related tool calls              │
│  2. Supervisor reviews the batch against the          │
│     playbook → approve, correct, or reject            │
│  3. Debugger executes the approved batch              │
│  4. Debugger summarizes verified results only         │
│  5. Supervisor reviews the summary:                   │
│     → Do the conclusions follow from the results?     │
│     → Were any values hallucinated or assumed?        │
│     → Does the stated intent match the actual calls?  │
│  6. Supervisor either approves another batch in the   │
│     same phase, approves the phase transition, or     │
│     blocks and redirects                              │
│                                                       │
└──────────────────────────────────────────────────────┘
```

**Critical rule:** The Debugger must **never advance to the next phase** without Supervisor approval. Per-call approval is only required in high-risk states (timescale uncertainty, path ambiguity, contradictory results, or golden-boundary ambiguity). Routine cursor/bookmark updates and tightly related read batches should be reviewed at the batch level, not one call at a time.

**Phase 0 gate:** The Supervisor must block Phase 1 if the Debugger has not checked architecture-document sufficiency. If no sufficient doc exists and the Debugger did not route through `08_DESIGN_MAPPING.md`, stop the debug and require that mapping first.

**Harmful-`X` gate:** If the failure includes architecturally relevant unknown (`X`) data or control, the Supervisor must block ordinary Playbook 04 tracing until the Debugger has completed Playbook 09 far enough to produce a creator-block / subsystem-boundary evidence table or to prove the source is outside the traced design boundary.

## Efficiency rules for supervised mode

- Prefer one high-information call over several narrow probes. `trace_with_snapshot`, `get_snapshot`, and `rank_cone_by_time` are better than long sequences of point queries.
- Use `rtl_trace_serve_start/query/stop` when the phase will need 3+ structural queries on the same DB.
- Use `create_signal_expression` for reusable handshakes or protocol events instead of repeating the same condition search.
- Use `dump_waveform_data` plus a short script for long transition streams instead of pasting raw JSON into prompts.

---

## The Supervisor's post-phase checklist

After each phase, the Supervisor runs these checks. Each check is mechanical — it does not require deep EDA knowledge, only careful comparison of "what was said" vs. "what was done."

### Intent-vs-action check

> "The Debugger said it would trace the **drivers** of signal X. Did the tool call use `mode='drivers'`? Or did it accidentally use `mode='loads'`?"

Compare the Debugger's stated goal with the actual tool call arguments. Common carelessness errors:
- Saying "trace drivers" but calling with `mode="loads"` (or vice versa)
- Saying "check signal X" but querying a different signal path
- Saying "at time T" but passing the wrong time value (especially timescale errors)
- Saying "search backward" but using `direction="forward"`

### Fact-vs-assumption check

> "The Debugger said signal X is 1 at time T. Was this returned by a tool call, or is it an assumption?"

For every factual claim in the summary, the Supervisor verifies it was returned by a tool call in the current or a previous phase. If the Debugger states a value without a corresponding tool result, the Supervisor flags it:

> "You stated that `rd_adr = 2` at time 412370000. Which tool call returned this value? If none, you must query it before using it in your reasoning."

### Conclusion-validity check

> "You observed X, Y, Z. You concluded W. Does W follow from those observations, or does it require an unstated assumption?"

The Supervisor checks whether the Debugger's conclusion is a logical consequence of the observed evidence. If the conclusion requires an assumption that was not verified, the Supervisor challenges:

> "You concluded that the FIFO is desynchronized because `rd_adr` skipped entries. But you haven't checked what `wr_adr` was at the same time. Could the write pointer also have wrapped early? Verify before concluding."

### Alternative-path check

> "You found signal B is wrong and started tracing B's drivers. But did you check all other RHS drivers of the original signal A? Could a different driver be the actual cause?"

This check enforces Rule 11 (layer-by-layer tracing). If the Debugger descends one branch of the driver cone without checking the others, the Supervisor blocks:

> "Signal A = X & Y. You found Y is wrong and moved on. But you didn't verify X. Check X first — if X is also wrong, the root cause may be elsewhere."

### Design-map sufficiency check

> "You are debugging inside `top.u_dma`, but what are its major child blocks, interfaces, and clock/reset boundaries? If you cannot answer that from an existing doc, why did you skip Phase 0 mapping?"

The Supervisor checks whether the Debugger has enough architectural context for the current scope. If the Debugger is inside an opaque subsystem and no sufficient doc exists, the Supervisor redirects to `08_DESIGN_MAPPING.md` instead of approving more cone tracing.

### Golden-boundary check

> "You are questioning whether the vendor VIP is correct. Stop. The VIP is trusted. Trace the immediate driver of the interface signal instead; the source may be DUT RTL or home-grown testbench RTL."

See Rule 14. If the Debugger hypothesizes that an EDA-vendor VIP or protocol checker is wrong, the Supervisor rejects the hypothesis unless the user has explicitly stated that the VIP configuration or connection is suspect.

---

## Golden boundary rule (Rule 14)

Only **EDA-vendor protocol VIP monitors/checkers** are golden by default. Do not extend that trust automatically to the rest of the testbench.

| Component | Trust level | Supervisor stance |
|---|---|---|
| **EDA-vendor VIP monitors / protocol checkers** | Golden | Trust the checker; trace the flagged interface signal's immediate driver |
| **Home-grown memory models / BFMs / wrappers** | Not golden | May be the real source of the bad value; investigate them if they drive it |
| **Home-grown assertions / scoreboards / reference models** | Evidence, not proof | Cross-check with waveform + structural trace before treating them as authoritative |
| **Stimulus generators** | Inputs, not golden | They may be wrong; require evidence before concluding that they are |

**How to apply:**

- **Never hypothesize** that an EDA-vendor VIP checker is buggy unless the user says to check the VIP configuration or connection.
- **Trace the immediate driver of the flagged interface signal first.** Do not assume the source is the DUT until the driver trace proves it.
- When a **home-grown assertion/checker** fires, use it as a clue to choose what to observe next. Do not treat it as proof until the waveform supports it.
- If the Debugger starts treating a home-grown memory model or BFM as "golden," the Supervisor must redirect: "The VIP may be golden, but this model is not. Trace the model's driver chain."

---

## Error-scenario anchor session

The first session the Debugger creates should capture the complete error scenario. This session serves as the **home base** for the entire debug. Every causal chain must ultimately explain the signals in this snapshot. When a dead end is reached, the Debugger returns here to try a different path.

### Setup (Phase 1, step 1.2 after Phase 0 mapping)

```python
# 1. Create the anchor session
create_session(waveform_path="wave.fsdb", session_name="error_scenario")
switch_session(session_name="error_scenario", waveform_path="wave.fsdb")

# 2. Bookmark the failure time
set_cursor(time=T_fail)
create_bookmark(bookmark_name="error_point", time=T_fail,
                description="<failure description + time precision note>")

# 3. Snapshot all signals mentioned in the error message
get_snapshot(signals=[...all signals from the error report...],
             time="BM_error_point", radix="hex")

# 4. Create a signal group for the error interface
create_signal_group(group_name="error_interface",
                    signals=[...the signals just snapshotted...])
```

### Using the anchor

- **At any point during debugging**, the Debugger can call `switch_session(session_name="error_scenario")` to return to the error context.
- **Before concluding**, the Debugger must verify: "Does my root cause explanation account for every signal value in the error-scenario snapshot?" If not, the explanation is incomplete.
- **On dead end**, the Supervisor instructs the Debugger: "Return to the error-scenario session. You've exhausted the `<signal_name>` branch. Pick a different signal from the error interface and trace that instead."

### Backtracking protocol

When the Debugger hits a dead end (two consecutive phases with no progress, a branch that terminates at a correct signal, or repeated looping inside the same subsystem):

```
1. Supervisor: "Return to error_scenario session."
2. Debugger: switch_session(session_name="error_scenario")
3. Supervisor: "Is this a branch dead end, or are you missing subsystem context?"
4. If subsystem context is missing:
   - Supervisor: "Pause tracing. Run Playbook 08 using the design top module, then read the doc for the current subsystem."
   - Debugger: generates or reads the subsystem doc, summarizes it, then returns.
5. If subsystem context is already sufficient:
   - Supervisor: "Which signals in the error-interface group have you NOT yet traced?"
   - Debugger: lists untraced signals.
   - Supervisor: "Trace <next_signal>. Start from Phase 2."
6. Debugger: proceeds with the approved branch.
```

This prevents the Debugger from wandering into unrelated parts of the design. Every investigation branch originates from the error scenario and must reconnect to it.

---

## Python script generation for waveform data

When waveform data involves long sequences (>20 transitions), the Debugger should write a Python script to process the data rather than attempting to reason over raw JSON mentally. This prevents:
- Miscounting transitions in a long list
- Confusing values at different time points
- Missing a pattern that is obvious in code but hard to see in raw data

### When to generate a script

| Situation | Action |
|---|---|
| Checking pointer values across >20 time points | Script: iterate transitions, find first anomaly |
| Comparing two signals for desynchronization | Script: zip their transitions, flag mismatches |
| Computing expected vs. actual values | Script: implement the expected logic, compare against waveform |
| Searching for a pattern in long transition data | Script: regex or condition-based filter |
| Decoding packed bus values | Script: bit-extract and format |

### Script structure

```python
#!/usr/bin/env python3
"""
Purpose: <one-line description of what this script checks>
Signals: <list of signals being analyzed>
Time range: <start> to <end>
Expected result: <what the output should show if the hypothesis is correct>
"""

import json

# Raw data from MCP tool call (paste the JSON result here)
transitions = [
    # ... from get_transitions() result
]

# Analysis
for i, t in enumerate(transitions):
    # ... processing logic
    pass

# Summary
print(f"Result: ...")
```

### Review protocol

1. **Debugger** writes the script and presents it to the Supervisor.
2. **Supervisor** reviews:
   - Does the script logic match the stated purpose?
   - Are the raw data values correctly copied from the tool result?
   - Is the comparison/condition correct (e.g., `>=` vs `>`, 0-based vs 1-based)?
   - Will the output actually answer the question?
3. **Supervisor** approves or requests changes.
4. **Debugger** executes the script and reports the result.

### Example: detecting pointer wrap anomaly

```python
#!/usr/bin/env python3
"""
Purpose: Find the first time rd_adr wraps to 0 and check what value it wraps FROM.
         Expected: wrap from 447 (FIFO depth - 1). Bug if it wraps from any other value.
Signals: top.nvdla_top...id_fifo.rd_adr
Time range: 0 to 412370000
"""

import json

# From get_transitions(path="...rd_adr", start_time=0, end_time=412370000, max_limit=1000)
transitions = [...]  # paste tool result

for i in range(1, len(transitions)):
    prev_val = int(transitions[i-1]["value"])
    curr_val = int(transitions[i]["value"])
    curr_time = transitions[i]["time"]

    # Detect wrap: value goes to 0 from a non-zero predecessor
    if curr_val == 0 and prev_val > 0:
        if prev_val != 447:
            print(f"ANOMALY at time {curr_time}: rd_adr wrapped from {prev_val} to 0 (expected wrap from 447)")
        else:
            print(f"OK at time {curr_time}: rd_adr wrapped from {prev_val} to 0")
```

---

## Supervisor decision table

Quick reference for the Supervisor's most common interventions:

| Observation | Supervisor action |
|---|---|
| Debugger's tool call arguments don't match stated intent | Block the call, point out the mismatch, ask for correction |
| Debugger's summary contains a value not returned by any tool | Flag as hallucination, require the Debugger to query the signal |
| Debugger concludes root cause but hasn't checked all RHS drivers | Block conclusion, list unchecked drivers, require verification |
| Debugger questions an EDA-vendor VIP, or treats a home-grown model as golden | Redirect: "Trust the vendor VIP. Trace the immediate driver of the flagged signal; the source may be DUT RTL or home-grown testbench RTL." |
| Debugger is processing >20 transitions by inspection | Instruct: "Write a Python script to process this data. I will review it." |
| Debugger skipped Phase 0 design-map check or is looping in an undocumented subsystem | Block and redirect: "Run Playbook 08 using the design top module, then read the relevant subsystem doc before more tracing." |
| Debugger is stuck after two attempts on the same branch | Instruct: "Return to error_scenario session. Pick a different signal to trace." |
| Debugger's conclusion doesn't explain the error-scenario snapshot | Block conclusion: "Your root cause must explain why <signal> was <value> at the error point. It currently doesn't." |
| Debugger skips a playbook phase | Block: "You skipped Phase N. Go back and complete it." |
| Two consecutive tool results are empty or contradictory | Invoke Rule 13: "Stop. Diagnose the empty results before continuing." |

---

## Implementation with Claude Code

In Claude Code, this two-agent architecture maps to the **Agent tool**:

### Option A — Supervisor as outer agent (recommended)

The user's session acts as the Supervisor. It spawns the Debugger as a subagent:

```
Agent(
    prompt="""You are the Debugger agent. Follow the debug workflow in
    agent_debug_textbook/04_ROOT_CAUSE_ANALYSIS.md exactly. If the failure
    involves architecturally relevant unknown (`X`) values, run
    agent_debug_textbook/09_X_TRACING.md first and carry its completed
    creator-block / subsystem-boundary evidence table into Playbook 04 instead
    of repeating the same boundary audit. Complete Phase 0 before Phase 1.
    After each phase,
    report each phase or compact batch of tool calls, results, and conclusions
    back to me for review. Do not advance to the next phase until I approve.

    The failure is: <error message>
    Waveform: <path>
    Structural DB: <path>
    Existing architecture docs: <paths or "none">
    Design top module for crawler (required if docs are missing): <top_module>
    Crawl output dir (required if docs are missing): <output_dir>
    Crawl depth (optional): <max_depth or default 4>
    Current subsystem of interest, if already known: <instance path or "unknown">
    """,
    description="RTL debug agent"
)
```

The Supervisor (outer agent) reviews each batch/phase response and sends corrections or approval via `SendMessage`. The prompt must provide enough Phase 0 context for the Debugger to either reuse existing docs or invoke Playbook 08 cleanly. At minimum, that means either doc paths, or `top_module` plus crawl output settings.

### Option B — Debugger spawns Supervisor for review

The Debugger is the outer agent. After each phase, it spawns a Supervisor subagent to review its work:

```
Agent(
    prompt="""You are the Supervisor. Review the following debug phase output.
    Check: (1) do tool call arguments match stated intent? (2) are all claimed
    values backed by tool results? (3) does the conclusion follow logically?
    (4) is a vendor VIP being questioned, or is a home-grown model being treated
    as golden? (5) were all RHS drivers checked? (6) if Phase 0 required
    design mapping, did the Debugger have either existing doc paths or crawler
    inputs (`top_module`, `output_dir`, optional `max_depth`)? (7) if the
    failure involves architecturally relevant unknown (`X`) values, did the
    Debugger complete Playbook 09 far enough to produce and reuse a valid
    creator-block / subsystem-boundary evidence table before ordinary RCA?

    Playbook rules: <paste relevant rules>

    Phase output to review:
    <paste debugger's phase summary>
    """,
    description="Debug supervisor review"
)
```

### Option C — Single agent with self-review checkpoints

When spawning two agents is impractical, a single agent can adopt a self-review discipline by pausing after each phase to run the Supervisor's checklist on its own output. This is less reliable than a true two-agent split (the same model that made the error is checking for it), but still catches the most obvious mistakes.

Insert this block after each phase:

```
SELF-REVIEW CHECKPOINT — Phase N complete
□ Do my tool call arguments match what I said I would do?
□ Did I complete the Phase 0 architecture-document sufficiency check? If not, stop.
□ Is every factual claim backed by a tool result I can point to?
□ Does my conclusion follow from the observations without unstated assumptions?
□ Did I check all RHS drivers, not just the first suspicious one?
□ Am I questioning a vendor VIP, or treating a home-grown model as golden? If so, stop.
□ Can I explain the error-scenario snapshot with my current theory?
```

---

## Tips

- **Use supervised mode selectively.** It reduces false starts, but it adds review latency. Prefer it after a failed single-agent pass or when the session is large, ambiguous, or high-risk.
- **Keep Supervisor prompts short and rule-based.** The Supervisor's effectiveness comes from mechanical checks, not from being a better debugger. A checklist is more reliable than open-ended reasoning.
- **Phase 0 is mandatory here too.** Supervised mode does not excuse missing design context. If the subsystem is undocumented, map it before approving more tracing.
- **The error-scenario session is non-negotiable.** Every investigation branch must start from and reconnect to the error scenario. If the Debugger cannot explain the error snapshot, the investigation is incomplete.
- **Python scripts are for data processing, not for tool calls.** The script processes data already returned by MCP tools. It does not replace the tools — it supplements them for tasks where humans would use a spreadsheet or a short script.
- **Backtracking is not failure.** In a complex design, the first branch often leads to a correct signal (dead end). Returning to the error scenario and trying the next suspect is the designed workflow, not an admission of failure.
