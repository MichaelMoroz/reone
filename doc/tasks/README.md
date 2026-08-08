# The task list

One place for everything outstanding. Compiled 2026-08-05 from nine planning
documents by two independent passes — an extraction pass over what the documents
say, and a staleness pass (codex, `gpt-5.6-terra`) over what the code shows.
Where those two disagreed, the code won and the disagreement is recorded.

| File | What it is |
|---|---|
| [MASTER.md](MASTER.md) | Every open item, one row each, with a stable ID and its provenance. **What to do.** |
| [DESIGN.md](DESIGN.md) | The shape of each unbuilt step, its acceptance criteria, and the decisions already taken. **How, and why this way.** |
| [RECORD.md](RECORD.md) | Postmortems, measurements, and approaches already rejected with their reasoning. **What has already been paid for once.** |
| [CONVENTIONS.md](CONVENTIONS.md) | Conventions an implementer violates by accident, and traps with their symptoms. |
| [FIDELITY.md](FIDELITY.md) | The retro fidelity audit: 37 rows with evidence, status and required proof. |
| [GLOSSARY.md](GLOSSARY.md) | What the vocabulary means. Read this first if any term is unfamiliar. |
| [DECISIONS.md](DECISIONS.md) | Contradictions a person has to settle, and work with no owner. |
| [_generated/doc-staleness-report.md](../_generated/doc-staleness-report.md) | The raw 2026-08-05 audit this folder was compiled from. It reads the nine retired documents, so it is provenance, not a live check. |

## IDs

`AREA-nnn`, assigned once and never reused. If an item is dropped, its row stays
with state `closed` and a reason — a missing number is a question nobody can
answer later.

| Prefix | Area |
|---|---|
| `RAS` | Raster rendering (the G and R tracks) |
| `STR` | Scene and structural (the S track, GpuScene, admission) |
| `TRC` | Path tracing (the PT substage, denoising, acceleration structures) |
| `FID` | Retro fidelity (deviations from the original renderer) |
| `SYS` | Game systems (rules, serialisation, UI) |
| `TOOL` | Build, tooling, diagnostics, process |

## States

`open` · `blocked` (needs another item first) · `decided` (settled, not yet built)
· `done` · `closed` (will not be done — reason required) · `unproven` (a claimed
difference with no capture or scan behind it yet).

**`unproven` is not a weak `open`.** It means the evidence bar has not been met,
and the next action is to gather evidence, not to write code. Several items are
mechanically certain from reading the source and still sit here, because this
project has repeatedly been wrong about what a source difference does on screen.

## Priority and effort

`P0` wrong or unverified now, and believing otherwise costs time later · `P1`
blocks other work or is a user-visible bug · `P2` real improvement, nothing
blocked · `P3` worth doing eventually. Effort `S` under a session, `M` a session
or two, `L` multi-session.

## Provenance

Nine documents were consolidated into this folder and then **deleted**. They are
recoverable from git history at `63ea1c41^`, but do not go looking — everything
worth keeping was moved here first, and what was left behind was left behind on
purpose: status blocks describing a tree that no longer exists, dependency tables
that are now wrong, and per-item rows that MASTER supersedes.

| Retired document | Where its content went |
|---|---|
| `backlog.md` | Items → MASTER. Postmortems and design analyses → RECORD (the BLAS analysis, the light calibration, the transparency design, the sky-classifier rejection). |
| `phase-f.md` | Step designs, acceptance criteria and the reference-engine survey → DESIGN. Items → MASTER. |
| `renderer-redesign-plan.md` | S0–S6 designs and the seven decisions → DESIGN. Items → MASTER. |
| `retro-rendering-differences.md` | Moved whole, as FIDELITY.md. |
| `renderer-registration-plan.md` | Architecture boundaries, rejected approaches and the calibration programme → RECORD. |
| `cleanup-plan.md` | Phase history, measurements and standing rules → RECORD. |
| `vulkan-rt-backend.md` | Conventions, traps and the hybrid decision → CONVENTIONS and DESIGN. Its architecture sections were obsolete and are gone. |
| `vulkan-remaining-plan.md` | The runtime-tier idea → CONVENTIONS. Four of its six items were already done; those are in MASTER's Closed list. |
| `vulkan-opengl-remaining-difference.md` | Measurement rules and the ruled-out list → CONVENTIONS. Its parity numbers died with the OpenGL backend. |

Two kinds of citation appear in MASTER's provenance column. A pointer like
`DESIGN.md` resolves here. An identifier like `backlog 1.13`, `FIDELITY #17` or
`vulkan-remaining §2` names the item's **origin** in a retired document — it is
there so a claim can be traced, not so the document can be opened.

## Adding an item

Give it the next free number in its area, a state, and a provenance — a
`file:line`, a commit, or "observed in play, <date>". An item with no evidence
and no owner is how the last nine documents got to where they are.
