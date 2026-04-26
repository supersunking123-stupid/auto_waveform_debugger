# Playbook 10 - Parallel Multi-Agent Debug Orchestration

**Role:** An Orchestrator launches several independent Debugger agents against the same RTL failure, waits until every agent reaches a terminal conclusion or a configured timeout expires, then reviews all completed conclusions and produces one debug report.

**When to use:** Use this flow when root-cause success rate matters more than minimizing compute, when the bug is ambiguous, or when prior single-agent attempts produced weak or conflicting conclusions. Do not use it for short value-lookups, simple waveform browsing, or cases where the user has not authorized parallel agent work.

**Default configuration:**

| Parameter | Default | Meaning |
|---|---|---|
| `agent_count` | `3` | Number of independent Debugger agents to launch |
| `timeout_minutes` | `30` | Wall-clock timeout for the whole parallel run |
| `session_prefix` | `<bug_id_or_timestamp>` | Unique prefix used to create per-agent session names |
| `session_startup` | `orchestrator_precreate` | Orchestrator creates per-agent sessions sequentially before launch |
| `waveform_warmup` | `required` | Orchestrator opens/indexes the shared waveform once before launch |
| `share_intermediate_findings` | `false` | Debugger agents should not see each other's hypotheses before the Orchestrator review |

**Prerequisites:** Same entry gates as `04_ROOT_CAUSE_ANALYSIS.md`: a compiled structural DB if structural tracing is needed, a waveform file, confirmed waveform time precision before time-based calls, and sufficient architecture context for the active debug scope. If architecture context is missing, run `08_DESIGN_MAPPING.md` before launching the parallel agents. If the failure involves harmful `X` propagation, each Debugger agent must route through `09_X_TRACING.md` before ordinary root-cause tracing.

---

## Roles

### Orchestrator

The Orchestrator owns the run control, not the detailed signal tracing. It:

- Performs the pre-flight checks in `00_ROUTER.md`
- Selects the applicable playbook chain for the bug
- Creates per-agent sessions sequentially before launching Debugger agents
- Warms the shared waveform backend once so agents do not all block on first FSDB open/indexing
- Launches `agent_count` independent Debugger agents with a serial-mode debug task derived from the user request, identical supplemental shared facts, and distinct `session_name` values
- Monitors whether each agent has reached a terminal status
- Stops the collection phase when all agents finish or `timeout_minutes` expires
- Validates each agent output against the required `DEBUG_CONCLUSION` schema before treating it as finished
- Collects every available agent conclusion, including inconclusive, blocked, timed-out, and non-compliant outcomes
- Reviews the conclusions for consensus, disagreement, unsupported claims, and unique evidence
- Writes the final parallel debug report

The Orchestrator should avoid steering agents toward another agent's hypothesis during the run. Independence is the point of this workflow.

### Debugger Agents

Each Debugger agent follows `00_ROUTER.md`, the selected playbooks, and `rtl_debug_guide.md`. It:

- Uses its assigned session name, for example `bug123__agent_01`
- Passes explicit `waveform_path` / `vcd_path` and `session_name` on session-aware calls
- Does not rely on `switch_session`
- Does not call `create_session` during startup unless the Orchestrator explicitly chose leaf-created sessions
- Does not inspect other agents' intermediate notes, sessions, or conclusions
- Records bookmarks, signal groups, virtual signals, and summaries in its own session unless the Orchestrator explicitly asks agents to share a session
- Returns the required conclusion schema before the deadline, even if the result is inconclusive
- If it claims `status: concluded`, identifies the root-cause type and a precise root-cause location. For RTL or testbench code bugs, this should be a source file, line, and statement or expression. For stimulus, CDC/timing, configuration, or environment roots, this should be the exact interface, event, setting, or boundary that caused the failure.

---

## Independence and shared-resource rules

- Agents may share the same read-only RTL DB and waveform file through one MCP service; the backend request paths are serialized by the automation layer.
- Independent branches should use distinct sessions: `<session_prefix>__agent_01`, `<session_prefix>__agent_02`, and so on. Include a timestamp or run ID in `session_prefix` unless the Orchestrator intentionally wants to reuse existing state.
- Agents may intentionally share one session only when the Orchestrator wants collaborative state. That is not the default for this playbook.
- The active Session pointer is global. In parallel mode, every session-aware tool call must pass explicit `waveform_path` or `vcd_path` and explicit `session_name`.
- Do not let every Debugger agent perform first-touch waveform/session startup concurrently. Large FSDB open/indexing can become the bottleneck and make all agents appear idle.
- Do not merge intermediate hypotheses while agents are still running. Cross-contamination reduces the value of independent attempts.
- A majority conclusion is not automatically correct. The Orchestrator must judge evidence quality, not just vote count.

---

## Phase 0 - Orchestrator pre-flight

Before launching agents, the Orchestrator completes this checklist:

1. Confirm the EDA MCP tool surface is available.
2. Confirm the structural DB path if structural tracing or RCA will be used.
3. Confirm the waveform path and expected failure time or failure interval.
4. Confirm sufficient architecture documentation for the debug scope; otherwise run `08_DESIGN_MAPPING.md`.
5. Route harmful-`X` failures to `09_X_TRACING.md`; route ordinary failures to `04_ROOT_CAUSE_ANALYSIS.md`.
6. Choose `agent_count`, `timeout_minutes`, and `session_prefix`.
7. If any session name already exists from an earlier run, either choose a new `session_prefix` or explicitly choose `reuse_existing_sessions=true` and record that stale state is being reused.
8. If using fresh sessions, pre-create every per-agent session sequentially with `create_session(waveform_path="<waveform_path>", session_name="<session_prefix>__agent_<NN>")`. If session creation fails because the session already exists, choose a new prefix or switch explicitly to the reuse flow; other creation failures are startup blockers. If reusing existing sessions, verify every assigned session sequentially with `get_session(waveform_path="<waveform_path>", session_name="<session_prefix>__agent_<NN>")` instead of calling `create_session`.
9. Warm the shared waveform backend once before launch. Prefer `get_signal_info` on a known clock or failure signal; if no signal is known yet, run a narrow `list_signals` query. Record the time precision if numeric times will be given to agents. If warm-up fails or times out, stop before launch and report a waveform startup blocker.
10. Derive the serial-mode debug task from the user request. Preserve the actual debug request and failure facts, but remove orchestration-only directives before passing it to Debugger agents. Do not pass text such as "run parallel orchestration", "launch agents", "act as Orchestrator", "use Playbook 10", agent-count/timeout instructions, aggregation/reporting instructions for the Orchestrator, or other parallel-run control text.
11. If removing orchestration-only text would leave no concrete debug task, synthesize a short serial-mode task from the verified failure facts and record that the task was synthesized.
12. Prepare one prompt per Debugger agent using the template below.

Do not launch agents before the architecture-doc gate, per-agent session setup, and shared waveform warm-up are complete. Parallelizing poorly oriented debug attempts usually creates several wrong narratives instead of one; parallelizing first-touch FSDB open/indexing usually creates three blocked agents instead of three investigations.

---

## Phase 1 - Launch Debugger agents

Launch all Debugger agents as close together as possible so they explore independently. Each agent receives the same serial-mode debug task and the same supplemental failure facts, but a different `agent_id` and `session_name`.

The default startup model is a barrier:

1. Orchestrator creates or verifies all per-agent sessions sequentially.
2. Orchestrator warms the waveform backend once and records the result.
3. Orchestrator launches Debugger agents.

Use leaf-created sessions only for small waveforms or special experiments. For large FSDBs, leaf-created sessions are a known failure mode because all agents can block before making evidence queries.

### Debugger agent prompt template

```text
You are Debugger Agent <agent_id> in an isolated RTL debug investigation.

Read agent_debug_textbook/00_ROUTER.md first. Follow the selected playbooks exactly:
- Primary playbook: <04_ROOT_CAUSE_ANALYSIS.md or 09_X_TRACING.md then 04_ROOT_CAUSE_ANALYSIS.md>
- Supporting playbooks: <01/02/03/05/07/08 as needed>
- General guide: agent_debug_textbook/rtl_debug_guide.md

Serial-mode user debug task, with orchestration directives removed:
<SERIAL_DEBUG_TASK_START>
<debug_task_prompt_with_parallel_orchestration_text_removed>
<SERIAL_DEBUG_TASK_END>

Treat the serial-mode debug task above as your primary task, the same way you would in serial single-agent mode. The supplemental facts below provide verified execution context and your isolated session. If semantic debug intent conflicts with the supplemental facts, preserve the serial-mode task's intent. If execution details conflict, use the supplemental facts for `waveform_path`, `rtl_trace_db_path`, converted failure time or interval, time precision, architecture-doc paths, session assignment, deadline, and launch-wrapper safety constraints. Report any such conflict in `remaining_uncertainties` instead of silently ignoring it.

Supplemental shared facts:
- Failure description: <failure_description>
- Waveform path: <waveform_path>
- Structural DB path: <rtl_trace_db_path>
- Failure time or interval: <time_or_interval_in_waveform_units>
- Time precision: <time_precision>
- Waveform warm-up: <completed_or_failed_with_details>
- Architecture docs to read: <doc_paths>
- Relevant logs or assertions: <log_summary>
- Deadline: <absolute_deadline_or_timeout_minutes_from_launch>

Your assigned session:
- session_name: <session_prefix>__agent_<NN>

Rules:
- Do not call create_session during startup. Your assigned session should already have been created or verified before launch. If get_session fails for your assigned session, report `blocked` with the missing session details instead of starting a competing session-creation flow.
- Use explicit waveform_path/vcd_path and session_name on every session-aware tool call.
- Do not rely on switch_session.
- Do not inspect or reuse other agents' intermediate notes or conclusions.
- Do not launch, supervise, or manage other agents. If orchestration text such as "run parallel", "launch agents", or "aggregate agents" appears in your assigned debug task, treat that as a prompt-preparation error: ignore the orchestration directive, continue the serial debug task if one is still clear, and mention the issue in `remaining_uncertainties`. If no concrete serial debug task remains after ignoring orchestration text, return `status: blocked` with `root_cause_location: unresolved - prompt preparation error: no concrete serial debug task`.
- Use MCP tools for structural and waveform evidence; do not substitute source reading for tracing.
- Return the literal `DEBUG_CONCLUSION` block before the deadline, even if it is inconclusive or blocked. A markdown summary without this block does not count as a terminal conclusion.
- If `status: concluded`, `root_cause_type` and `root_cause_location` must identify what caused the test failure precisely enough that a human can act on it. For RTL or testbench code bugs, `root_cause_location` must contain the source file, line, and statement or expression. If only a region or signal boundary is known, use `status: inconclusive` and set `root_cause_location: unresolved - <reason>`.

Required final schema:

DEBUG_CONCLUSION:
agent_id: <agent_id>
session_name: <session_name>
status: concluded | inconclusive | blocked
root_cause_summary: <one paragraph>
root_cause_type: rtl_design_bug | testbench_stimulus | testbench_model_bug | cdc_timing | configuration | environment | unknown
root_cause_location: <file:line: statement/expression, interface event, CDC boundary, configuration item, environment dependency, or unresolved - reason>
confidence: high | medium | low
primary_evidence:
- <tool-backed evidence item>
causal_chain:
- <symptom -> cause step>
eliminated_hypotheses:
- <hypothesis and why eliminated>
bookmarks_or_artifacts:
- <bookmark, signal group, virtual signal, dump file, or script>
remaining_uncertainties:
- <uncertainty or "none">
recommended_next_steps:
- <next action>
```

If an agent returns text without this schema before the deadline, the Orchestrator must not mark it finished. If time remains, re-prompt the same agent to rewrite its conclusion in the required schema. If the deadline expires without a valid schema, record it as `non_compliant` or `timed_out` and include whatever partial summary is available.

---

## Phase 2 - Monitor run status

The Orchestrator monitors each Debugger agent until one of the finish conditions is met.

Finish conditions:

- All agents return valid `DEBUG_CONCLUSION` blocks
- `timeout_minutes` expires

Status values:

| Status | Meaning |
|---|---|
| `running` | Agent is still investigating |
| `concluded` | Agent reached a supported root-cause conclusion |
| `inconclusive` | Agent finished but could not identify a root cause |
| `blocked` | Agent could not proceed because a required tool, DB, waveform, or document was unavailable |
| `timed_out` | The Orchestrator deadline expired before the agent returned a conclusion |
| `non_compliant` | Agent returned output, but it did not contain a valid `DEBUG_CONCLUSION` schema before the deadline |

Monitoring rules:

- Default timeout is 30 minutes from agent launch. Orchestrator setup time, including session setup and waveform warm-up, is reported separately and does not count as agent investigation time.
- Do not silently extend the timeout. If more time is needed, record the extension reason or ask the user.
- Do not stop other agents just because one agent has a plausible answer. Continue until all agents finish or the timeout expires.
- Preserve partial results from blocked or timed-out agents; they can still contain useful eliminations or evidence.
- Treat a report as terminal only when it contains a parseable `DEBUG_CONCLUSION` block with all required fields. In particular, `root_cause_type` and `root_cause_location` are required even when the location is explicitly unresolved.
- If an agent omits the schema or a required field and time remains, send a schema-fix prompt to that agent. Do not let a non-compliant report satisfy the `all_agents_finished` condition.

---

## Phase 3 - Collect conclusions

After the finish condition is met, the Orchestrator collects:

- Every `DEBUG_CONCLUSION` from finished agents
- Status and partial notes from blocked, timed-out, or non-compliant agents
- Session names and important artifacts for each agent
- Tool failures or environment blockers that affected any agent

The final report must list every agent, including agents that did not reach a confident root cause.

---

## Phase 4 - Orchestrator review

The Orchestrator reviews all conclusions before writing the report.

Required checks:

1. **Evidence check:** Does each factual claim cite waveform, structural, log, bookmark, or script evidence?
2. **Causal-chain check:** Does each conclusion explain how the root cause creates the observed symptom at the failure time?
3. **Consensus check:** Which agents reached the same root cause, and did they use independent evidence?
4. **Conflict check:** Which conclusions disagree, and what evidence would distinguish them?
5. **Quality check:** Which claims depend on assumptions, failed tools, or missing architecture context?
6. **Coverage check:** Which hypotheses were eliminated by multiple agents, and which remain untested?
7. **Precise-location check:** For every `concluded` agent, does `root_cause_type` match the claimed cause, does `root_cause_location` identify the actionable source statement, stimulus event, CDC boundary, configuration item, or environment dependency, and does the causal chain prove why that location caused the observed failure?

The Orchestrator may choose a "most supported root cause", but only if the evidence supports it. If all agents are inconclusive or conflicting, report that directly and recommend the next investigation step.

If all completed agents are `inconclusive`, or if the strongest consensus only identifies a region or signal boundary rather than a precise root-cause location, the Orchestrator must use remaining time for a focused continuation before finalizing. The continuation may re-prompt one or more existing agents or launch a follow-up branch targeted at the best current frontier. Only finalize without a precise root-cause location when the Orchestrator deadline expires, a required artifact is missing, or the report explicitly records why the location cannot be proven.

---

## Final report template

```markdown
# Parallel Debug Report

## Run Configuration
- Failure: <failure summary>
- Waveform: <waveform_path>
- Structural DB: <rtl_trace_db_path>
- Agent count: <N>
- Timeout: <timeout_minutes> minutes
- Startup status: <sessions_created_and_waveform_warmed | sessions_verified_and_waveform_warmed | startup_blocked>
- Startup duration: <duration>
- Finish condition: all_agents_finished | timeout
- Playbooks used: <playbook list>

## Agent Conclusions
| Agent | Session | Status | Conclusion | Confidence | Key Evidence |
|---|---|---|---|---|---|
| agent_<NN> | <session> | <status> | <summary> | <confidence> | <evidence> |
| ...repeat one row for every launched agent... | <session> | <status> | <summary> | <confidence> | <evidence> |

## Root Cause Location
<Root-cause type and actionable location: file:line statement/expression, stimulus event, CDC boundary, configuration item, environment dependency, or `unresolved` with the specific blocker. Do not report a regional signal boundary as a precise root-cause location.>

## Orchestrator Review
<Analysis of evidence quality, unsupported claims, and whether conclusions follow from tool results.>

## Consensus and Disagreement
<Group equivalent conclusions together. List conflicts explicitly.>

## Most Supported Root Cause
<Root cause, confidence, and why this conclusion is better supported than alternatives. If none is supported, say so.>

## Evidence Table
| Evidence | Source Agent(s) | Supports | Notes |
|---|---|---|---|
| <evidence> | <agents> | <claim> | <notes> |

## Remaining Gaps
- <gap>

## Recommended Next Actions
- <action>

## Appendix - Per-Agent Raw Summaries
### agent_<NN>
<raw conclusion or partial summary>

Repeat one appendix section for every launched agent, including timed-out and blocked agents.
```

---

## Common failure modes

- **False consensus:** Several agents repeat the same weak assumption. Treat this as one unsupported conclusion, not as strong evidence.
- **Premature convergence:** The Orchestrator shares one agent's theory too early and collapses independent search diversity.
- **Session collision:** Agents rely on the global active Session and overwrite each other's cursor or bookmarks. Use explicit per-agent sessions.
- **Hidden timeout:** The report only lists successful agents and omits timed-out runs. Always list every agent.
- **Schema drift:** A useful markdown report lacks `DEBUG_CONCLUSION` or omits required fields. Re-prompt before the deadline; otherwise record it as `non_compliant` and do not count it as finished.
- **Regional stop:** Agents agree on a subsystem boundary but do not identify a precise root-cause location. Use remaining time for focused continuation instead of reporting regional localization as root cause.
- **Architecture-doc skip:** Agents start deep waveform tracing without knowing the relevant hierarchy boundaries. Run Playbook 08 first.
- **Vote over evidence:** The most common conclusion is not necessarily the best one. Prefer the conclusion with the clearest causal chain and strongest tool-backed evidence.

---

## Relation to supervised debug

Use `06_SUPERVISED_DEBUG.md` when one Debugger needs continuous review from one Supervisor. Use this playbook when you want multiple independent Debugger agents to explore the same bug in parallel and an Orchestrator to reconcile their conclusions afterward.

The two workflows can be combined for expensive bugs: each branch can be a supervised Debugger/Supervisor pair, and the Orchestrator can aggregate the branch-level reports. Use that only when the user explicitly authorizes the additional agents and runtime.
