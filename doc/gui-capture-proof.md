# GUI capture proof

GUI layout, scaling, list-spacing, splash-screen and movie presentation changes
need visual evidence. A build and a green test run are necessary but do not show
what the screen looks like.

`scripts/capture-gui-proof.ps1` renders the proof matrix headlessly and writes
one PNG per state into a folder. It needs a Release build in `build/bin` and
`ffmpeg` on PATH.

```powershell
.\scripts\capture-gui-proof.ps1 `
    -Kotor1Dir "<kotor install>" `
    -Kotor2Dir "<kotor 2 install>"
```

Output goes to `build/gui-proof/` as `<game>-<state>-<width>x<height>.png`.
`-States inventory,equipment-items` and `-Widths 3440` narrow a rerun while
iterating.

The matrix covers both games at 1024x768 and 3440x1440: startup movie frames,
main menu, every character-generation window, the gameplay HUD and populated
combat action sequence, an area-transition banner, Swoop, the three Pazaak
screens, every in-game tab, a
wrapping bark bubble, an icon-bearing confirmation popup, dialogue and a
computer terminal. A combined-overlay state intentionally presents the combat
HUD, bark bubble and confirmation popup at once to catch layering or shared-state
regressions. Every capture uses the default GUI presentation scales, including
the 50% list-row density. The matrix does not add alternate scale variants; use
a focused manual run when testing a non-default option.

`-NoWorld` renders the matrix with 3D scene content switched off - the world and
every scene-backed GUI panel - leaving only the deterministic 2D interface. Use it
whenever two builds are being compared: scene content is not reproducible frame to
frame, so a comparison that includes it measures the renderer's noise rather than
the layout. Without the switch the matrix renders the world, which is the right
default for judging a screen but the wrong one for judging a difference.

The switch also reseeds the shared random generator before every state. A batch is
one engine process, so whatever randomness one state consumes shifts the next; left
alone, that drift accumulates until the same state is dealt different Pazaak cards
and stocked with different container items. Seeding once at the head of the batch
is not enough, and the symptom - two builds disagreeing on the contents of a list
while every frame, baseline and margin matches to the pixel - looks nothing like a
seeding problem.

Captures are driven by console commands, so any of them can also be run by hand:
`pause <frames>`, `capture <path> [frames]` and `quit` sequence a batch from a
`--commands-file`, while `skipmovie`, `openchargen`, `showhud`, `showbark`,
`showpopup`, `showtransition` and `showgallerymode` set up states that are
otherwise hard to reach deterministically.

When reviewing, zoom in rather than trusting a fitted thumbnail, and check that
movies stay inside the shorter viewport axis, menu and gameplay plates cover the
framebuffer, controls stay within the authored canvas, and list rows stay
registered with their icon and slot art from the first row to the last. A state
that fails to capture is a failed run, not a state to omit.
