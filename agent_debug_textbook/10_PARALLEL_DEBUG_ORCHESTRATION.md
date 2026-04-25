# Playbook 10 - Parallel Multi-Agent Debug Orchestration

**Role:** An Orchestrator launches several independent Debugger agents against the same RTL failure, waits until every agent reaches a terminal conclusion or a configured timeout expires, then reviews all completed conclusions and produces one debug report.

**When to use:** Use this flow when root-cause success rate matters more than minimizing compute, when the bug is ambiguous, or when prior single-agent attempts produced weak or conflicting conclusions. Do not use it for short value-lookups, simple waveform browsing, or cases where the user has not authorized parallel agent work.

**Default configuration:**

| Parameter | Default | Meaning |
|---|---|---|
| `agent_count` | `3` | Number of independent Debugger agents to launch |
| `timeout_minutes` | `20` | Wall-clock timeout for the whole parallel run |
| `session_prefix` | `<bug_id_or_timestamp>` | Unique prefix used to create per-agent session names |
| `share_intermediate_findings` | `false` | Debugger agents should not see each other's hypotheses before the Orchestrator review |

**Prerequisites:** Same entry gates as `04_ROOT_CAUSE_ANALYSIS.md`: a compiled structural DB if structural tracing is needed, a waveform file, confirmed waveform time precision before time-based calls, and sufficient architecture context for the active debug scope. If architecture context is missing, run `08_DESIGN_MAPPING.md` before launching the parallel agents. If the failure involves harmful `X` propagation, each Debugger agent must route through `09_X_TRACING.md` before ordinary root-cause tracing.

---

## Roles

### Orchestrator

The Orchestrator owns the run control, not the detailed signal tracing. It:

- Performs the pre-flight checks in `00_ROUTER.md`
- Selects the applicable playbook chain for the bug
- Launches `agent_count` independent Debugger agents with identical shared facts and distinct `session_name` values
- Monitors whether each agent has reached a terminal status
- Stops the collection phase when all agents finish or `timeout_minutes` expires
- Collects every available agent conclusion, including inconclusive, blocked, and timed-out outcomes
- Reviews the conclusions for consensus, disagreement, unsupported claims, and unique evidence
- Writes the final parallel debug report

The Orchestrator should avoid steering agents toward another agent's hypothesis during the run. Independence is the point of this workflow.

### Debugger Agents

Each Debugger agent follows `00_ROUTER.md`, the selected playbooks, and `rtl_debug_guide.md`. It:

- Uses its assigned session name, for example `bug123__agent_01`
- Passes explicit `waveform_path` / `vcd_path` and `session_name` on session-aware calls
- Does not rely on `switch_session`
- Does not inspect other agents' intermediate notes, sessions, or conclusions
- Records bookmarks, signal groups, virtual signals, and summaries in its own session unless the Orchestrator explicitly asks agents to share a session
- Returns the required conclusion schema before the deadline, even if the result is inconclusive

---

## Independence and shared-resource rules

- Agents may share the same read-only RTL DB and waveform file through one MCP service; the backend request paths are serialized by the automation layer.
- Independent branches should use distinct sessions: `<session_prefix>__agent_01`, `<session_prefix>__agent_02`, and so on. Include a timestamp or run ID in `session_prefix` unless the Orchestrator intentionally wants to reuse existing state.
- Agents may intentionally share one session only when the Orchestrator wants collaborative state. That is not the default for this playbook.
- The active Session pointer is global. In parallel mode, every session-aware tool call must pass explicit `waveform_path` or `vcd_path` and explicit `session_name`.
- Do not merge intermediate hypotheses while agents are still running. Cross-contamination reduces the value of independent attempts.
- A majority conclusion is not automatically correct. The Orchestrator must judge evidence quality, not just vote count.

---

## Phase 0 - Orchestrator pre-flight

Before launching agents, the Orchestrator completes this checklist:

1. Confirm the EDA MCP tool surface is available.
2. Confirm the structural DB path if structural tracing or RCA will be used.
3. Confirm the waveform path and expected failure time or failure interval.
4. Confirm waveform time precision with `get_signal_info` if the Orchestrator will provide numeric times to agents.
5. Confirm sufficient architecture documentation for the debug scope; otherwise run `08_DESIGN_MAPPING.md`.
6. Route harmful-`X` failures to `09_X_TRACING.md`; route ordinary failures to `04_ROOT_CAUSE_ANALYSIS.md`.
7. Choose `agent_count`, `timeout_minutes`, and `session_prefix`.
8. Decide whether the Orchestrator will pre-create per-agent sessions or each Debugger agent will create its own assigned session. If a per-agent session name already exists from an earlier run, either choose a new `session_prefix` or explicitly record that stale state is being reused.
9. Prepare one prompt per Debugger agent using the template below.

Do not launch agents before the architecture-doc gate is satisfied. Parallelizing poorly oriented debug attempts usually creates several wrong narratives instead of one.

---

## Phase 1 - Launch Debugger agents

Launch all Debugger agents as close together as possible so they explore independently. Each agent receives the same failure facts but a different `agent_id` and `session_name`.

### Debugger agent prompt template

```text
You are Debugger Agent <agent_id> of <agent_count> in a parallel RTL debug run.

Read agent_debug_textbook/00_ROUTER.md first. Follow the selected playbooks exactly:
- Primary playbook: <04_ROOT_CAUSE_ANALYSIS.md or 09_X_TRACING.md then 04_ROOT_CAUSE_ANALYSIS.md>
- Supporting playbooks: <01/02/03/05/07/08 as needed>
- General guide: agent_debug_textbook/rtl_debug_guide.md

Shared facts:
- Failure description: <failure_description>
- Waveform path: <waveform_path>
- Structural DB path: <rtl_trace_db_path>
- Failure time or interval: <time_or_interval_in_waveform_units>
- Time precision: <time_precision>
- Architecture docs to read: <doc_paths>
- Relevant logs or assertions: <log_summary>
- Deadline: <absolute_deadline_or_timeout_minutes_from_launch>

Your assigned session:
- session_name: <session_prefix>__agent_<NN>

Rules:
- First create your assigned session with create_session(waveform_path="<waveform_path>", session_name="<session_prefix>__agent_<NN>") unless the Orchestrator says it has already been created. If creation fails because the session already exists, report the collision and use get_session/list_sessions to verify whether the Orchestrator intended to reuse it.
- Use explicit waveform_path/vcd_path and session_name on every session-aware tool call.
- Do not rely on switch_session.
- Do not inspect or reuse other agents' intermediate notes or conclusions.
- Use MCP tools for structural and waveform evidence; do not substitute source reading for tracing.
- Return a conclusion before the deadline, even if it is inconclusive or blocked.

Required final schema:

DEBUG_CONCLUSION:
agent_id: <agent_id>
session_name: <session_name>
status: concluded | inconclusive | blocked
root_cause_summary: <one paragraph>
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

If an agent reaches the deadline without returning this schema, the Orchestrator records it as `timed_out` and includes whatever partial summary is available.

---

## Phase 2 - Monitor run status

The Orchestrator monitors each Debugger agent until one of the finish conditions is met.

Finish conditions:

- All agents return `DEBUG_CONCLUSION`
- `timeout_minutes` expires

Status values:

| Status | Meaning |
|---|---|
| `running` | Agent is still investigating |
| `concluded` | Agent reached a supported root-cause conclusion |
| `inconclusive` | Agent finished but could not identify a root cause |
| `blocked` | Agent could not proceed because a required tool, DB, waveform, or document was unavailable |
| `timed_out` | The Orchestrator deadline expired before the agent returned a conclusion |

Monitoring rules:

- Default timeout is 20 minutes from agent launch.
- Do not silently extend the timeout. If more time is needed, record the extension reason or ask the user.
- Do not stop other agents just because one agent has a plausible answer. Continue until all agents finish or the timeout expires.
- Preserve partial results from blocked or timed-out agents; they can still contain useful eliminations or evidence.

---

## Phase 3 - Collect conclusions

After the finish condition is met, the Orchestrator collects:

- Every `DEBUG_CONCLUSION` from finished agents
- Status and partial notes from blocked or timed-out agents
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

The Orchestrator may choose a "most supported root cause", but only if the evidence supports it. If all agents are inconclusive or conflicting, report that directly and recommend the next investigation step.

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
- Finish condition: all_agents_finished | timeout
- Playbooks used: <playbook list>

## Agent Conclusions
| Agent | Session | Status | Conclusion | Confidence | Key Evidence |
|---|---|---|---|---|---|
| agent_<NN> | <session> | <status> | <summary> | <confidence> | <evidence> |
| ...repeat one row for every launched agent... | <session> | <status> | <summary> | <confidence> | <evidence> |

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
- **Architecture-doc skip:** Agents start deep waveform tracing without knowing the relevant hierarchy boundaries. Run Playbook 08 first.
- **Vote over evidence:** The most common conclusion is not necessarily the best one. Prefer the conclusion with the clearest causal chain and strongest tool-backed evidence.

---

## Relation to supervised debug

Use `06_SUPERVISED_DEBUG.md` when one Debugger needs continuous review from one Supervisor. Use this playbook when you want multiple independent Debugger agents to explore the same bug in parallel and an Orchestrator to reconcile their conclusions afterward.

The two workflows can be combined for expensive bugs: each branch can be a supervised Debugger/Supervisor pair, and the Orchestrator can aggregate the branch-level reports. Use that only when the user explicitly authorizes the additional agents and runtime.
