# GUI presentation and visual proof

## Capturing visual proof

GUI layout, scaling, list-spacing, splash-screen and movie changes need visual
evidence. **A build and a green test run are necessary but do not show what the screen
looks like.**

`scripts/capture-game-proof.ps1` renders the proof matrix headlessly, one PNG per state
into `build/gui-proof/`. It needs a Release build in `build/bin` and `ffmpeg` on PATH;
`-States` and `-Widths` narrow a rerun.

Invoke it as `.\scripts\capture-game-proof.ps1 -Kotor1Dir "<k1>" -Kotor2Dir "<k2>"`.

**Before trusting a comparison between two builds, prove the matrix reproduces against
the same build** — add `-NoWorld -VerifyReproducibility`, which captures each batch
twice with identical commands and compares decoded RGBA pixels exactly, failing with
every affected state and the deltas.

**Pass `-NoWorld` when comparing builds:** it emits `graphics off` so world rendering
does not enter the comparison, and in that mode the harness emits `seed 1337` **before
every state**. Each batch runs in one process with one random generator, so seeding
once per run lets drift accumulate from earlier states — **the symptom is two builds
disagreeing on the contents of a list while every frame, baseline, margin and alignment
axis still matches to the pixel.** Reseeding per state prevents that exposure; it does
not identify the source of any irreproducibility.

**The matrix** covers both games at 1024×768 and 3440×1440: startup movie frames, main
menu, every chargen window, the gameplay HUD and a populated combat action sequence, an
area-transition banner, Swoop, the three Pazaak screens, every in-game tab, a wrapping
bark bubble, an icon-bearing confirmation popup, dialogue and a computer terminal. A
dialogue whose replies overflow shows the reply scroll bar, and a TSL party-selection
roster puts Handmaiden in the slot she shares with Disciple. **A combined-overlay state
deliberately presents the combat HUD, bark bubble and confirmation popup at once to
catch layering or shared-state regressions.** Every capture uses the default
presentation scales, including the 50% list-row density; **use a focused manual run for
a non-default option.**

Captures are driven by console commands, so any can be run by hand: `pause`, `capture`
and `quit` sequence a batch from a `--commands-file`, while `skipmovie`, `openchargen`,
`showhud`, `showbark`, `showpopup`, `showtransition` and `showgallerymode` reach states
that are otherwise hard to set up deterministically.

**When reviewing, zoom in rather than trusting a fitted thumbnail.** Check that movies
stay inside the shorter viewport axis, that menu and gameplay plates cover the
framebuffer, that controls stay within the authored canvas, and that list rows stay
registered with their icon and slot art from the first row to the last. **A state that
fails to capture is a failed run, not a state to omit.**

## The K2 main menu

K2 selects its menu scene from `GBL_MAIN_SITH_LORD`: 0 Sion, 1 alternate Traya (unused
by normal progression), 2 Nihilus, 3 Traya, 4 environment and current leader, anything
else falling back to Sion. K1 retains its unnumbered `mainmenu`, orthographic
projection and 1.4 framing scale.

**Traps this area has already produced.** `MainMenu::refreshScene()` replaces the whole
scene including its node arena, because **ordinary `SceneGraph::clear()` only removes
roots and cannot release the old menu's cameras, emitters and other allocated nodes** —
and replacing a scene drops the graphics context's framebuffer references before their
owners are destroyed, so **screenshot readback invalidates the cached read framebuffer
when binding the window; otherwise the next scene blit reuses that binding.** The camera
attaches to the `camerahook` **directly below the authored scene root**, which matters for
`mainmenu03`, since it also contains a second `camerahook` inside Nihilus that the general
duplicate-name lookup would select instead. Retail inserts `gui3d_room` as a separate
black enclosing shell, but **it does not parent or occlude the numbered model and supplies
no camera or character lighting** — the numbered model supplies the floor, backdrop,
lights, emitters and looping animation. The dynamic leader uses `mainmenu05`'s
`cutscenedummy` position, falling back to `(0, -1, 0)` without that hook, and unsupported
actors log a warning and request `pause1` rather than failing.

**Durable options.** Before resetting a playable session, `Game::openMainMenu()` copies
the selector and, for value 4, the leader's gender, appearance, body column and texture
variation into five `reone.cfg` keys. **Cold startup, chargen cancel and partial-load
recovery do not replace this snapshot with empty runtime state, and no save slot is
inspected for the cold-start presentation.** The writer replaces only those five keys.
A complete valid tuple is required to insert the creature; otherwise value 4 shows the
environment alone. The reconstructed actor has **no gameplay registration, Party
membership, saved identity or retained live leader reference**, and the tuple setter
rejects runtime creatures.

Regression checks: `test/game/mainmenu.cpp`
(`--gtest_filter='MenuPresentation.*:MainMenuTest.*'`). `openmenu main` exercises the
same capture/retirement/refresh boundary; headless runs advance at 60 simulation frames
per second, so `pause 1200` clears one 16-second animation cycle.

## Inventory and equipment presentation

`InventoryMenu` and `Equipment` use `PresentationGUI` and supplied backings whose data
contracts hold display values, textures and backing-local selection handles — **they do
not retain gameplay items or creatures**, and the copied views are **not an immutable
content database.** `PresentationGUI` borrows an explicit game identity, providers, mixer
and sounds, and **these dependencies must outlive the screens**; independent presentation
surfaces need independent GUI providers, because loaded GUIs contain mutable controls and
event listeners. `InGameMenuHost` owns registrations and routes input and rendering, but
**does not select a player, pause gameplay or control the world.**

The contract that makes stale input safe:

- **Each equipment read invalidates the previous selection handles.** A request carries
  its read revision, item handle and slot; the backing checks exact runtime incarnations,
  current subject and inventory, stack count and equipment freshness, and the gameplay
  operation also checks live registration and source ownership. **Tags, resource names and
  list positions are not item identity.**
- **Command delivery and completion are separate operations.** The presenter waits for
  the matching result before refreshing from authority, so **sending a request does not
  decrement a count or move an icon optimistically.**
- `applyEquipmentOperation` delegates the existing candidate evaluation, splitting,
  replacement and ownership primitives with no hidden party lookup. Rejection before
  mutation leaves disposition unchanged, and a recoverable failed equip returns the taken
  candidate to the supplied inventory — but exceptions from core split, transfer or effect
  operations propagate, so **this wrapper does not provide atomic rollback over them.**

Tests exercise the production presenters and host with real GUI controls and a recording
renderer **without creating Game, Party, Creature or Item**
(`sharedpresentation.cpp`), the Equipment button through its ordinary backing
(`spitempresentation.cpp`), instance identity and stale requests
(`itemmenubacking.cpp`), and authoritative operations without a GUI
(`equipmentoperation.cpp`). **Pixel comparisons and gameplay smoke checks remain separate
acceptance evidence.**
