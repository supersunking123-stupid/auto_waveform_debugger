# Playbook 08 — Design Mapping

**Role:** You are a design mapper. Your job is to establish the architectural context for debugging by reading existing architecture docs when they are sufficient, or by generating them with the `rtl-crawler-multi-agent` skill when they are not.

**When to use:** Use this playbook before active debugging when the design or subsystem is unfamiliar, when no architecture document has been provided, or when tracing inside a subsystem has become circular and unproductive.

**Prerequisites:** A usable structural database path must be known. If no compiled DB exists yet, ask the user for the exact project-specific compile flow rather than guessing the filelist or top-module recipe.

**Important capability boundary:** The current `rtl-crawler-multi-agent` skill is rooted at the design `top_module`. It builds a full-design map and then emits per-subsystem docs from that top-level crawl. It does **not** take a subsystem-root parameter. When you need local subsystem context, rerun or reuse the full-design crawl and then focus on the generated doc for the subsystem you care about.

---

## What counts as a sufficient architecture document?

A document is sufficient only if it covers the **relevant debug scope** and answers all of these:

1. **Hierarchy boundaries** — major child blocks and wrappers in the relevant design or subsystem
2. **Interfaces and peers** — what external buses or protocol connections exist, and which neighboring blocks they connect to
3. **Clock/reset structure** — the important clock domains, resets, or domain boundaries
4. **Control/dataflow landmarks** — the main controller, datapath, FIFO, arbiter, bridge, or pipeline landmarks that let you navigate the subsystem during debug

Treat any of the following as potentially sufficient if they meet the rubric above:
- User-provided architecture/design docs
- Prior crawler-generated docs such as `design_index.md` and subsystem architecture files
- Project docs that clearly describe the same subsystem you are debugging

If the doc only covers the top level but not the current subsystem, it is **not** sufficient for subsystem-level debug. Refresh or reuse the full-design crawl, then read the generated doc for the current subsystem.

---

## Decision tree

```
Do you already have architecture docs for the relevant scope?
│
├─ Yes, and they satisfy the sufficiency rubric
│   └─ Read them, summarize the boundaries/interfaces/clocks/resets, then return to the debug playbook
│
├─ No top-level architecture map exists
│   └─ Run a full-design crawl with `rtl-crawler-multi-agent`
│
├─ A top-level map exists, but the current subsystem is undocumented or still opaque
│   └─ Refresh the full-design crawl, then focus on the generated subsystem doc
│
└─ You are looping inside the same subsystem during debug
    └─ Pause tracing, refresh or reuse the full-design crawl, then read the relevant subsystem doc before continuing
```

---

## Mandatory rules

- **Use the `rtl-crawler-multi-agent` skill.** Do not try to recreate the crawler manually with ad hoc `hier` and `trace` calls across the entire design.
- **Map before you guess.** If the subsystem architecture is unclear, get the map before doing more driver-cone tracing.
- **Pick the smallest supported action that resolves the uncertainty.**
  - No top-level map or unclear failure ownership → full-design crawl
  - Localized debug inside one opaque subsystem → reuse or refresh the full-design crawl, then read that subsystem's generated doc
- **After the crawl, read the output before resuming debug.** A generated document is only useful if you summarize and apply it.

---

## Common sequences

### Sequence A — Reuse an existing architecture document

1. Read the existing doc(s) covering the current design or subsystem.
2. Check the sufficiency rubric.
3. Write a short working summary for yourself:
   - relevant subsystem boundary
   - major child blocks
   - external interfaces and peer blocks
   - clock/reset domains
   - the most likely control/dataflow landmarks for the failing path
4. Return to the active debug playbook.

### Sequence B — Full-design crawl before debugging

Use this when you do not have a trustworthy top-level architecture map.

1. Confirm `db_path` and `top_module`.
2. Invoke the `rtl-crawler-multi-agent` skill.
3. Provide:
   - `db_path`
   - `top_module`
   - `output_dir`
   - `max_depth`
4. Read the generated `design_index.md` plus the subsystem doc most relevant to the failure.
5. Summarize the ownership of the failing path before moving to waveform-based debug.

### Sequence C — Refresh the design map when tracing is looping inside one subsystem

Use this when you are stuck inside one subsystem and repeated traces are not shrinking the suspect set.

1. Define the subsystem boundary you are currently inside.
2. Invoke `rtl-crawler-multi-agent` using the design `top_module`, or reuse an existing full-design crawl if it is still current.
3. Read the resulting subsystem architecture doc for the boundary from step 1.
4. Restart the debug from the subsystem boundary:
   - use documented child blocks as branch points
   - use documented interfaces as search anchors
   - use documented clocks/resets to avoid mixing domains

---

## Stuck triggers that require refreshing the design map and focusing on the subsystem doc

Pause signal tracing and run this playbook when any of these occur:

- You have spent **8 or more investigation-oriented tool calls** inside one subsystem without shrinking the suspect set or moving the causal frontier deeper
- You revisit the **same suspect cone or same 2–3 signals twice** without a new explanation
- The logic is clearly stateful or hierarchical enough that local driver/load traces no longer tell you where to branch next
- You cannot state the subsystem's major child blocks, interfaces, and clock/reset boundaries from memory

After the crawl, make one bounded post-crawl debug pass. If the root cause is still opaque and you have crossed the existing local-investigation threshold, escalate to a targeted local testbench.

---

## Output contract

Before returning to another playbook, produce a short working summary for yourself containing:

- Which doc satisfied the need, or which crawl output you generated
- The subsystem boundary you will debug inside
- The major child blocks or wrappers relevant to the failure
- The external interfaces or peer blocks relevant to the failure
- The clock/reset domains that matter
- The next debug branch you will investigate based on the map

If you cannot generate or find a sufficient architecture document, report that explicitly instead of pretending the map exists.

---

## Relationship to other playbooks

- **Before `04_ROOT_CAUSE_ANALYSIS.md`:** use this playbook when Phase 0 shows the design context is missing
- **During `04_ROOT_CAUSE_ANALYSIS.md`:** switch here when the investigation becomes circular inside one subsystem
- **Not a replacement for `02_STRUCTURAL_EXPLORATION.md`:** use `02` for targeted hierarchy/find/trace questions; use `08` when you need an architecture map and persistent docs
