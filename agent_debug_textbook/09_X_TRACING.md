# Playbook 09 — X Tracing

**Role:** You are an `X`-origin debugger. Your job is to locate the **first harmful `X`**: the earliest instance and earliest activation interval where an `X` becomes active on a path that can affect architecturally relevant behavior.

**When to use:** A failure involves unknown values such as `X` on data, control, protocol payloads, corrupted writes/reads, or explicit simulator/VIP messages showing unknown bits.

**Core idea:** Do not start by deep-tracing the nearest internal net with an `X`. First move at **higher hierarchy boundaries** to determine where the harmful `X` first appears. Stay at the **highest meaningful mapped subsystem boundary** you can justify, and only descend into subsystem internals after a subsystem-level ingress/egress check says that subsystem is the likely creator region. Only after isolating the likely creator instance should you switch back to ordinary driver tracing.

**Finish condition:** "Find the exact RTL statement" means **find the exact RTL statement in the first creator block**. It does **not** mean "quote the first exact source line you can find on the bad cone." A pipe register, FIFO storage element, concat, or mux statement is not the root cause if harmful `X` inputs are already present at that block boundary.

---

## What counts as a harmful `X`

An `X` is **harmful** only if all of these are true:

- It appears on an active path, not an idle or don't-care path.
- The corresponding `valid`, `ready`, enable, write-enable, select, or state context indicates the value matters at that time.
- The `X` can influence architecturally visible behavior: a committed write, accepted read data, protocol response, state transition, assertion condition, or user-visible output.

An `X` is **not yet evidence** if:

- It is on a data bus while the relevant `valid` / enable / select is inactive.
- It is in an unselected mux input.
- It is in a later pipeline register after the failure path is already idle.

The goal is to find the **first harmful `X`**, not the loudest `X`.

---

## Working harmful interval

Do **not** reduce harmful-`X` tracing to one scalar timestamp. A harmful boundary often becomes active at one cycle edge and only shows a bad egress `X` one or more cycles later.

For the **current boundary frontier**, maintain a **working harmful interval**:

- start from the control edge that makes the path active, often found by `find_edge` on the relevant `valid` / enable / select signal
- extend through the earliest interval where the boundary output is harmfully `X`
- widen the interval enough to cover any plausible local latency between ingress and egress on that boundary
- represent that interval explicitly as `start_time` + `end_time` or as a start/end bookmark pair; never compress it to one bookmark name

Use `get_transitions`, `find_edge`, and `find_value_intervals` over that interval. Use `get_snapshot` / `get_value_at_time` only to spot-check specific points inside the interval.

When the frontier moves to a different owner or different boundary, **re-seek the harmful interval for that new boundary**. Do **not** blindly reuse the old boundary's timestamp or interval, because the upstream `X` may have arrived earlier than the downstream harmful observation.

---

## What counts as a mapped subsystem boundary

A **mapped subsystem boundary** is any hierarchy boundary that the current architecture context identifies as a meaningful debug stop, such as:

- a wrapper, subsystem, partition, cluster, engine, or other major child block called out by the architecture doc or crawler output
- the current debug-scope boundary chosen during orientation
- the highest boundary where the current failure can still be explained in terms of interfaces, peers, clock/reset domains, or major control/dataflow landmarks

This definition is intentionally generic. Do **not** depend on a design-specific instance name. Depend on whether the current architecture context says: "this boundary is a meaningful place to decide whether the harmful `X` is entering from outside or being created inside."

If a boundary meets that definition, it is a subsystem-boundary stop for harmful `X` tracing.

---

## Tool priorities for X tracing

Prefer these in order:

- `get_transitions`, `find_edge`, or `find_value_intervals` for the boundary signal plus its `valid` / enable context, especially when locating when the harmful `X` first becomes active and when comparing ingress-to-egress delay across a boundary interval
- `get_snapshot` / `get_value_at_time` at **instance boundary ports** for endpoint spot-checks inside the current harmful interval
- `rtl_trace hier` to move between parent and sibling instances
- `rtl_trace trace` when you must confirm whether a boundary net is owned by a child instance, sibling instance, parent-level glue, or local logic
- `rtl_trace whereis-instance` only after you already know the instance path and need module/source lookup
- `trace_with_snapshot` / `explain_signal_at_time` only after the likely creator instance is isolated

Do **not** start by tracing deep internal combinational cones unless the hierarchy-first steps have already isolated the creator block.

Before any deep local tracing or source-file inspection, complete the **creator-block gate**:

- harmful `X` outputs present at the current block boundary: yes / no
- harmful `X` inputs present at the current block boundary: yes / no
- if harmful `X` inputs are present, continue upward or sideways to the structural owner
- if harmful `X` outputs are present and harmful `X` inputs are absent, this is only a creator candidate; prove whether the owner is a child instance, local logic, parent-glue, or top-level before descending
- if a child instance owns the harmful boundary, move the frontier into that child instead of stopping at the parent wrapper, unless the boundary has crossed into a mapped subsystem and the subsystem-boundary gate below applies first
- if ownership resolves to a child instance, **reset the frontier to that child's ingress boundary first**; do not continue from the child's bad output bit or child output bus yet
- if the boundary driver is not a sibling instance, use `rtl_trace trace` to determine whether the owner is a child instance, parent-level glue, top-level connectivity, or local logic before descending
- if the harmful boundary crosses a **mapped subsystem boundary** (for example a major child block from the architecture doc such as a top-level partition), stop at that subsystem instance first; do not jump directly to a deep leaf returned by structural trace
- at a subsystem boundary, produce a **subsystem-boundary evidence table** before descending inside the subsystem
- the subsystem-boundary evidence table must include:
  - audit interval and activation edge used for the audit
  - harmful egress signal(s) checked and the active-path proof for each one
  - relevant ingress data groups
  - matching `valid` / ready / enable / select groups
  - active mode / config / state groups that control the harmful path
  - per-group classification: `harmful X` / `clean` / `gated off`
  - reason for every `gated off` classification
  - decision: keep tracing at the higher layer or descend into subsystem internals
- the subsystem-boundary stop overrides the generic child-owner shortcut for harmful `X` tracing: even if structural trace already names a deeper child owner, and even if the subsystem wrapper appears to be simple pass-through wiring on this path, you must complete the subsystem-boundary audit and evidence table first
- once a mapped subsystem boundary has already been audited for the same harmful interval, harmful path, and controlling context, reuse that evidence table instead of bouncing back to the subsystem wrapper again
- descend into subsystem internals only if the subsystem egress is harmfully `X` while the relevant subsystem ingress groups are `clean` or `gated off`; if subsystem ingress is already harmful, keep tracing at the higher hierarchy layer instead of diving into the subsystem
- if the subsystem-boundary evidence table does not yet exist for the current harmful interval, harmful path, and controlling context, descent below that boundary is **forbidden**

---

## The X-tracing workflow

### Phase 1 — Confirm the harmful X at the symptom

1. Identify the exact time window where the visible failure happens.
2. Verify the waveform precision before using any integer time.
3. If the observed harmful `X` is on an internal net rather than a meaningful boundary, **lift the frontier outward first**:
   - move to the nearest meaningful egress boundary of the current instance or mapped subsystem
   - if multiple candidate boundaries exist, choose the highest one that still explains the failure in terms of interfaces and control/dataflow context
   - do **not** begin the hierarchy climb from an arbitrary deep internal net when a boundary-level symptom is available
4. Sample the failing or lifted-boundary signal and its immediate control context:
   - data
   - `valid` / `ready`
   - write enable / read enable
   - select / mode / state
5. Record why the observed `X` is harmful rather than inactive.
6. Search **backward in time** on that same boundary signal and its activity context to determine the **working harmful interval** for the current frontier.
   - Use `find_edge` on the activating `valid` / enable / select signal when there is a clear edge that makes the path live.
   - Use `get_transitions` or `find_value_intervals` on both the boundary data signal and the activity context to bracket the interval from activation through the earliest harmful `X` observation.
   - If ingress-to-egress latency is plausible on this boundary, widen the interval enough to cover that latency instead of collapsing to one sample.
   - The working X-tracing state is the **current boundary plus its harmful interval**, not a single timestamp.

If you cannot yet prove the `X` is on an active path, do not begin X tracing. First resolve the protocol or timing context.

### Phase 2 — Move upward through parent boundaries

Starting from the instance where the harmful `X` is observed at the current harmful interval:

1. Move to the **father** instance.
2. Check the father's **input ports** for suspicious `X` values over the current harmful interval, not just at one sample.
3. For every suspicious data input, also check the corresponding activity context over that interval or over the causally earlier portion of that interval:
   - `valid` / `ready`
   - command-valid
   - write/read enable
   - mux select
   - state / mode gating
4. Ignore inactive `X` inputs. Keep only harmful suspicious inputs, and note whether each one appears before, at, or after the harmful egress activity inside the interval.
5. Repeat this father/grandfather search upward until one of these happens:
   - you find an ancestor with **no harmful suspicious `X` inputs**
   - you reach the top module

This phase answers: **how high in the hierarchy can the harmful `X` be observed at active input boundaries?**

### Phase 3 — Choose the next owner of the harmful X

There are two valid outcomes from Phase 2:

1. **The current ancestor has harmful `X` outputs but no harmful `X` inputs.**
   - Treat that ancestor as the current **creator candidate**, not automatically as the final creator block.
   - Do not force a sibling search. The boundary may instead be driven by a child instance, parent-level glue, top-level IO, constants, or logic local to that ancestor.
   - Use `rtl_trace trace` on the ancestor boundary net to determine ownership before descending.
   - If a **child instance** owns the harmful boundary output, move the X-tracing frontier into that child and continue the hierarchy-first walk there.
   - After moving into that child, first re-seek the child's own harmful interval, then restart the creator-block gate on the child's **ingress input groups** before tracing any child output bit or internal net.
   - If that child is a **mapped subsystem wrapper or major subsystem instance**, stop at the subsystem boundary first.
     - Produce the subsystem-boundary evidence table over the subsystem's current harmful interval.
     - If subsystem ingress is already harmful, do **not** descend into subsystem internals yet; keep tracing across the higher-level subsystem boundary or peer/top-level glue.
     - Descend inside the subsystem only when the subsystem egress is harmful and the relevant subsystem ingress groups are `clean` or `gated off`.
     - Do this even if structural tracing already identifies a deeper child owner or the subsystem wrapper looks like pass-through wiring on the current path.
     - If that same subsystem boundary was already audited earlier for the same harmful interval, harmful path, and controlling context, reuse the existing subsystem evidence table and continue to the deeper child instead of restarting the subsystem audit.
     - If the subsystem-boundary evidence table is missing or incomplete, the next step is **not** "pick a deeper child." The next step is "finish the subsystem-boundary audit."
   - If **local logic** owns the harmful boundary output, this ancestor is the likely creator block and you may descend locally.
   - If **parent-level glue or top-level connectivity** owns the boundary, keep the frontier at that external owner rather than reporting the ancestor wrapper as the creator.

2. **The last ancestor that still had harmful suspicious `X` inputs** is still upstream-dependent.
   - Go back to that ancestor.
   - Identify which child input port into that ancestor carries the harmful `X`.
   - Find the structural owner of that bad input.
     - Prefer a **driving sibling instance** when one exists at the same hierarchical level.
     - If no sibling instance owns the driver, allow for **parent-level glue or top-level connectivity** instead of assuming every bad input comes from a sibling block.
   - Do not immediately dive inside the ancestor's internal nets if an external structural owner can be identified first.

This phase answers: **which block or boundary owner injects the harmful `X` into the current frontier?**

### Phase 4 — Repeat on the next structural owner

If Phase 3 already isolated a likely creator block, skip to Phase 5.

Otherwise continue on the structural owner identified in Phase 3:

1. If the owner is a **child instance** or **driving sibling instance**:
   - Re-seek the owner's own harmful interval before auditing its ingress boundary. Do not assume the downstream boundary interval is still the correct one.
   - Check its **input ports** for harmful suspicious `X` values over that interval.
   - For each suspicious data input, verify the matching `valid` / enable / select context and any active mode / config / state context over that interval.
   - If the owner is a **child instance**, treat this as an ingress reset:
     - start from the child's input groups, not the child's output bus
     - do not trace a representative bad child output bit yet
     - for generated / HLS blocks, child-output bit tracing is forbidden until the ingress groups are classified
   - If the owner is a **mapped subsystem instance** or a child inside a mapped subsystem:
     - prefer the subsystem boundary over the deeper leaf boundary
     - produce the subsystem-boundary evidence table before descending to internal child blocks
     - if subsystem ingress is already harmful, keep the frontier at the higher subsystem layer and continue tracing across subsystem interfaces
     - if that subsystem boundary was already audited for the same harmful interval, harmful path, and controlling context, reuse the existing subsystem evidence table and continue downward instead of returning to the subsystem wrapper
     - if the subsystem-boundary evidence table is missing or incomplete, any deeper internal descent is invalid
2. If the owner is **parent-level glue** or **top-level connectivity**:
   - Stay at that owner level and use `rtl_trace trace` plus waveform checks on the owning net or top-level port.
   - Follow the same harmful-`X` / active-path test until you either re-enter a concrete instance, prove local glue owns the bad value, or prove the source is outside the traced design boundary.
   - Do **not** invent instance input ports for glue logic or top-level wiring that do not exist.
3. Repeat this owner-by-owner process until you find the first place where:
   - suspicious harmful `X` outputs are present
   - harmful suspicious `X` inputs are absent
   - and ownership is resolved to local logic or to a source outside the traced design boundary

That result gives you either the **likely X creator block** or proof that the source is outside the traced design boundary.

### Phase 5 — Switch to ordinary signal tracing inside the creator block

Only after isolating the likely creator block should you descend into internal logic.

Now:

1. If the current creator candidate sits below a **mapped subsystem boundary** that has not yet been audited for the same harmful interval, harmful path, and controlling context, stop and return to the highest unaudited mapped subsystem boundary first.
2. Produce any missing subsystem-boundary evidence table before local tracing continues.
3. If the subsystem-boundary evidence table remains missing or incomplete, local descent is forbidden.
4. Complete the **creator-block check** and keep it in context:
   - current block
   - current harmful interval and activation edge
   - harmful `X` outputs present: yes / no
   - harmful `X` inputs present: yes / no
   - driver ownership: child-instance / sibling / parent-glue / local logic / top-level
   - decision: continue hierarchy walk or descend locally
5. Before descending, write an explicit **boundary evidence table**:
   - audit interval and activation edge used for the audit
   - harmful output signal(s) checked
   - active handshake / enable context for those outputs
   - each relevant input group checked over the current harmful interval or a causally earlier interval on that boundary
   - matching `valid` / ready / enable / select groups
   - active mode / config / state groups that control the harmful path
   - classification for each input group: `harmful X` / `clean` / `gated off`
   - reason for every `gated off` classification
6. If harmful `X` inputs are still present, **do not** descend. Continue the hierarchy-first walk.
7. If the boundary owner is a **child instance**, move into that child and continue the hierarchy-first walk.
8. After moving into a child instance, restart from the child's ingress boundary:
   - re-seek the child's harmful interval first
   - classify the child's data ingress groups
   - classify the relevant `valid` / ready / enable / select context
   - classify active config / mode groups that can influence the harmful path
   - only after these ingress groups are explicitly classified may you consider local tracing inside the child
9. If the child ingress groups are not yet classified, tracing a representative bad child output bit or child internal cone is **not allowed**.
10. If harmful `X` outputs are present and harmful `X` inputs are absent, and ownership is **local logic**, descend into the block's internal logic.
11. Pick the suspicious output that first becomes harmful within the current creator-block harmful interval.
12. Trace its drivers using the regular root-cause workflow.
13. Keep tracing until you can name a specific buggy statement, wrong gating condition, bad bit select, stale register capture, or other concrete cause.
14. The final "exact RTL statement" must come from this creator block, not from an upstream or downstream transport/storage block.

### RCA handoff rule

When Playbook 09 isolates a likely creator block, hand off to Playbook 04 with this packet:

- harmful interval and activation edge for the current creator boundary
- current creator candidate / creator block
- current mapped subsystem boundary, if one was audited
- completed boundary evidence table for the creator-block gate
- completed subsystem-boundary evidence table, if one was required

Playbook 04 should **reuse** these tables instead of restarting the same creator-block or subsystem-boundary audit. Re-open the audit only if the harmful interval, active path, controlling context, creator candidate, or audited subsystem boundary changed, or if the earlier table was incomplete.

### Boundary-proof rule

The creator-block gate is satisfied only by **explicit boundary evidence**, not by a narrative impression.

Do **not** claim "the inputs are clean or gated off" unless you can name the input groups you checked and classify each one.

For harmful `X` tracing, "input groups" means the ingress-side groups that can actually feed the harmful path:

- data ingress groups
- matching `valid` / ready / enable / select groups
- active config / mode / state groups

For **mapped subsystem boundaries**, apply the same rule at subsystem scope:

- audit interval and activation edge for that subsystem boundary
- relevant subsystem ingress data groups
- matching subsystem ingress `valid` / ready / enable / select groups
- active subsystem mode / config / state groups that control the harmful path
- harmful subsystem egress signal(s) and their active-path proof
- the explicit decision: keep tracing at the higher layer or descend internally

The subsystem-boundary evidence table is mandatory whenever the harmful frontier crosses a mapped subsystem boundary. Without that table, the proof is incomplete and descent below that boundary is forbidden.

Do not skip the subsystem boundary just because structural tracing names a deeper internal child as the owner of the current harmful output.

Do not re-open an already completed subsystem-boundary audit at the same harmful interval unless the harmful path or controlling context changed.

Do not replace ingress classification with output-side bit tracing.

For every relevant boundary input group, record one of:

- `harmful X` — the signal is `X` on an active path and can influence the bad output during the audited interval
- `clean` — the signal is not `X` during the audited interval where it could influence the bad output
- `gated off` — the signal may be `X`, but its corresponding valid / enable / select / load condition is inactive during the audited interval; record the gating reason

If you cannot produce this table, the boundary proof is incomplete and you must keep climbing, classify the boundary yourself in bounded batches, or delegate the classification if delegation is explicitly authorized.

Missing-table rule:

- If the next intended move would cross below a mapped subsystem boundary and the subsystem-boundary evidence table for the current harmful interval, harmful path, and controlling context is missing or incomplete, that move is invalid.
- The correct next action is to return to that boundary and finish the audit, not to choose a deeper child or representative output bit.

### Generated-block rule

For large generated / HLS / repeated blocks, a partial snapshot or a quick source skim is **not** enough to prove the block has no harmful `X` inputs.

In those cases:

- prefer a subagent boundary-port classifier **only if delegation is explicitly authorized**, or
- do the same boundary classification yourself in bounded batches, or
- keep the hierarchy-first walk going until the owner is clearer

Do **not** descend into a large generated block on the basis of:

- control ports looking clean while data-side inputs were not classified
- one or two representative inputs only
- top-of-file source reading without an explicit boundary evidence table
- a partial snapshot that does not cover the relevant input groups
- output ownership alone, without a fresh ingress-side boundary check for the owned child
- a representative bad output bit chosen before the child ingress groups were classified
- a deep internal owner returned by structural trace before the enclosing subsystem boundary was checked

---

## Subagent rule for large port lists

When checking a father/grandfather instance or a driving sibling, the input-port list can be too large for the main agent to inspect comfortably.

In that situation, the main agent may **delegate the large port scan only if delegation is explicitly authorized in the current session**.

If delegation is authorized:

- Spawn a subagent **without full-history fork**. Pass only a short summary of:
  - the current instance path
  - the current harmful interval, expressed as explicit `start_time` / `end_time` values or a start-bookmark / end-bookmark pair, and, when known, the activation edge that makes the path live
  - which boundary output is harmful
  - which `valid` / ready / enable / select signals matter
  - which mode / config / state signals matter for gating on this path
- Do **not** override model / agent settings on a forked child. Use a minimal-context helper whose only job is port classification.
- Ask the subagent to analyze one instance's input ports over that interval and return a compact boundary-audit table containing:
  - audit interval and activation edge used
  - harmful boundary output(s) checked and active-path proof summary
  - harmful suspicious `X` inputs
  - gated-off relevant inputs, with the gating reason for each one
  - clean relevant inputs
- The subagent should return a short summary table, not a raw dump of every port, and should omit irrelevant ports that cannot feed the harmful path.
- The main agent should keep ownership of:
  - the causal narrative
  - which instance to inspect next
  - when to switch from hierarchy-first X tracing to ordinary driver tracing

If delegation is **not** authorized:

- Do the same classification yourself in bounded batches of ports.
- Keep only the compact summary table in context:
  - audit interval and activation edge used
  - harmful boundary output(s) checked and active-path proof summary
  - harmful suspicious `X` inputs
  - gated-off relevant inputs, with the gating reason for each one
  - clean relevant inputs
- Continue the hierarchy-first walk once the compact table is complete.

Use delegation or the single-agent bounded-batch fallback when any of these are true:

- the instance has many input ports and the raw list would bloat context
- several buses must each be paired with `valid` / enable / select signals
- the boundary classification depends on mode / config / state gating in addition to handshake signals
- the agent is checking repeated or wide interface boundaries and only needs the suspicious subset

The subagent is a **filter**, not the decision-maker. The main agent still decides whether an `X` is harmful and where the X-tracing frontier moves next.

---

## Stop conditions

Stop the hierarchy-first X-tracing phase only when one of these is true:

- You have proved that the first boundary with harmful `X` outputs and no harmful `X` inputs is owned by **local logic** in the current block.
- You have traced ownership through parent-level glue or top-level connectivity and proved the source is **outside the traced design boundary**.
- You proved the visible `X` is inactive and therefore not the causal path.

Do **not** stop merely because the current block has no harmful `X` inputs. That only makes it a creator candidate; you must still resolve ownership as child instance, local logic, parent-level glue, or outside the traced design.

---

## Anti-patterns

Do not do these:

- Treat every visible `X` as causal evidence without checking `valid` / enable context.
- Dive into internal nets before checking parent and sibling instance boundaries.
- Trace stage-by-stage through long repeated pipelines when the first harmful `X` might already be visible at a higher boundary.
- Dump every input port of a large instance into the main context instead of filtering for suspicious harmful ports.
- Declare victory at the first explicit RTL `X` assignment if harmful suspicious `X` inputs are still present upstream.
- After resolving ownership to a child instance, continue from the child's bad output instead of resetting to the child's ingress boundary.
- Skip a mapped subsystem boundary and jump directly into a deep internal child just because structural tracing identified that child as an owner.
- Treat a subsystem wrapper as "transparent" and use that as a reason to skip subsystem ingress classification.
- Take any deeper step below a mapped subsystem boundary before the subsystem-boundary evidence table is complete for the same harmful interval, harmful path, and controlling context.
- Reuse a downstream harmful timestamp on a new owner boundary without re-checking whether the harmful interval moved earlier because of latency or storage.
- In a generated / HLS child, start representative-bit tracing before ingress input groups were classified.
- Report a pipe/skid register assignment that merely latched an already-bad payload as the root cause.
- Report a FIFO RAM entry or storage flop write as the root cause when the incoming data is already harmful `X`.
- Report a concat or pack statement as the root cause when one of its active inputs is already harmful `X`.
- Report a mux/default-to-`X` branch as the root cause when the selected live input is already harmful `X`.

---

## Minimal working summary to keep

After each hierarchy step, summarize only:

- current instance
- current mapped subsystem boundary, if any
- current harmful interval and activation edge, if known
- subsystem-boundary evidence table complete or incomplete
- harmful suspicious `X` outputs observed
- harmful suspicious `X` inputs observed or not observed
- relevant `valid` / enable / mode context
- creator-block decision: keep climbing or descend locally
- next boundary to inspect

This is enough to preserve the causal thread without carrying giant port lists in context.
