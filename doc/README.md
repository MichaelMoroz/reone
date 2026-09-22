# reone documentation

Nine planning documents were consolidated into `doc/tasks/` on 2026-08-05; that folder
was flattened back into `doc/` on 2026-09-22, when everything it recorded as built was
removed. What remains is meant to be read, not archived.

| File | What it is |
|---|---|
| [TASKS.md](TASKS.md) | Every open item, one row each, with a stable ID — plus the decisions nobody has settled. **What to do.** |
| [DESIGN.md](DESIGN.md) | The shape of each unbuilt step and its acceptance criteria. **How, and why this way.** |
| [RENDERER.md](RENDERER.md) | How the renderer is put together today. **What exists.** |
| [CONVENTIONS.md](CONVENTIONS.md) | Conventions you would violate by accident, traps already paid for, and the capture and measurement rules. |
| [FIDELITY.md](FIDELITY.md) | Where retro still differs from the original, with evidence and the proof each row needs. |
| [GLOSSARY.md](GLOSSARY.md) | What the vocabulary means, including terms that are now dead. Read this first if a term is unfamiliar. |
| [LESSONS.md](LESSONS.md) | Postmortems and rejected approaches whose reasoning still binds. **What has already been paid for once.** |
| [material-model.md](material-model.md) | The shading model PBR and the tracer share. |
| [ray-culling.md](ray-culling.md) | Keeping grass out of the light path, and the harness that measured it. |
| [game-systems.md](game-systems.md) | Identity, save and runtime lifecycle. |
| [gui.md](gui.md) | GUI scaling and presentation, and how to capture visual proof. |

## IDs, states, priorities

`AREA-nnn`, assigned once and never reused. A dropped item keeps its number and a
reason — **a missing number is a question nobody can answer later.** Prefixes: `RAS`
raster · `STR` scene and structural · `TRC` path tracing · `FID` deviations from the
original renderer · `SYS` game systems · `TOOL` build, tooling, diagnostics, process.

States are `open` · `blocked` (needs another item first) · `decided` (settled, not yet
built) · `unproven`. **`unproven` is not a weak `open`:** the evidence bar has not been
met, and the next action is to gather evidence, not to write code. Several items are
mechanically certain from reading the source and still sit there, because **this
project has repeatedly been wrong about what a source difference does on screen.**

Priority: `P0` wrong or unverified now, and believing otherwise costs time later · `P1`
blocks other work or is a user-visible bug · `P2` real improvement, nothing blocked ·
`P3` worth doing eventually.

## The standing rule for fidelity questions

**Anything that brings the picture closer to the original goes into every render mode,
unless it obviously regresses the advanced lighting or material features of PBR and
path tracing.** Fidelity work is not retro's alone: retro is where a difference from
the original is *measured*, not where the correction belongs. Where a fix corrects how
authored data is read — a cutout threshold the format supplies, an animated texture the
TXI declares — it corrects all three modes, because all three read the same data and
none benefits from reading it wrongly.

The exception is narrow and has to be argued, not assumed: **a change regresses the other
modes when it would undo something they do *better*, not merely differently.** Retro's
non-inverse-square falloff and radius-squared cull are the model of a difference that stays
retro-only, because the corrected model is the point of the other two.

## Adding an item

Give it the next free number in its area, a state, and a provenance — a `file:line`, a
commit, or "observed in play, &lt;date&gt;". **An item with no evidence and no owner is how
the previous nine documents got to where they are.** And **do not record completion
here**: a built item leaves this folder, and git history is where it went.
