# The task list

One place for everything outstanding. Compiled 2026-08-05 from nine planning
documents by two independent passes — an extraction pass over what the documents
say, and a staleness pass (codex, `gpt-5.6-terra`) over what the code shows.
Where those two disagreed, the code won and the disagreement is recorded.

| File | What it is |
|---|---|
| [MASTER.md](MASTER.md) | Every open item, one row each, with a stable ID and its provenance. |
| [GLOSSARY.md](GLOSSARY.md) | What the vocabulary means. Read this before MASTER if any term is unfamiliar. |
| [DECISIONS.md](DECISIONS.md) | Contradictions between documents that a person has to settle, and items with no owner. |
| [_generated/doc-staleness-report.md](../_generated/doc-staleness-report.md) | The raw staleness audit. Regenerate rather than edit. |

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

Every row carries where it came from. The source documents stay in the tree as
history — they hold reasoning, measurements and postmortems that no task list
should try to absorb. What they no longer hold is authority over what to do next.

| Document | Standing after this consolidation |
|---|---|
| `backlog.md` | Superseded by MASTER. Keep for its postmortems and design prose (8.9's BLAS analysis, 1.9's light calibration, 3.7's transparency design). |
| `phase-f.md` | **Still authoritative for the raster track's design.** MASTER points at it; it is not replaced. |
| `retro-rendering-differences.md` | **Still authoritative for fidelity.** MASTER carries its rows by reference. |
| `renderer-redesign-plan.md` | **Still authoritative for the structural track.** |
| `renderer-registration-plan.md` | Historical. Its live residue is in MASTER; the rest is a design record. |
| `cleanup-plan.md` | Historical. Same treatment. Its `F0…F10` numbering is dead — see the glossary. |
| `vulkan-rt-backend.md` | **Largely obsolete.** Its status block, architecture section and per-mesh BLAS strategy describe a tree that no longer exists. Its `§11.2` hybrid decision and `§15`/`§16` conventions are the parts still worth reading. |
| `vulkan-remaining-plan.md` | **Obsolete.** Four of its six items are done; item 5 still claims retro is OpenGL-only. |
| `vulkan-opengl-remaining-difference.md` | **Obsolete.** Its comparison target was deleted with OpenGL; its remaining claims are not parity facts. |

## Adding an item

Give it the next free number in its area, a state, and a provenance — a
`file:line`, a commit, or "observed in play, <date>". An item with no evidence
and no owner is how the last nine documents got to where they are.
