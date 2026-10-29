# Identity, save, and runtime lifecycle

The contracts behind the E-series save/runtime implementation. **The code and its tests
are the authority for the mechanics; this records the laws they enforce and the
shortcuts that look convenient and are bugs.** L remains authoritative for Odyssey
resource selection, buckets, mount planning and source ownership.

The architecture targets K1/K2 observable behaviour and content compatibility. **It does
not reproduce Odyssey filesystem staging, unsafe pointer lifetimes, or destructive
failure behaviour merely for implementation fidelity.**

## The eight engine laws

1. Resources have explicit owners; source lifetime is independent of lookup priority.
2. Every serialized object reference has an explicit identity domain.
3. Logical roster identity is a PartyTable slot, explicitly bound to a runtime
   representation.
4. A creature may survive an Area, but its Area-runtime residency may not.
5. Runtime identity ends at semantic destruction, even if C++ storage remains.
6. A fallible object or owned graph becomes gameplay-live only after its own
   construction, identity and ownership are coherent.
7. Enough destination resources and structure are prepared to reject avoidable failure
   before sacrificing a good world.
8. Authored gameplay begins only after the irreversible publication boundary; it is
   retired coherently on failure, not transactionally rolled back.

**These laws are orthogonal.** A saved ID is not a runtime ID, a roster slot is not
either ID, an Area attachment is not object existence, and a `shared_ptr` is not gameplay
liveness.

## Serialized identity

An ObjectId-shaped field is meaningful only with a `SerializedIdentityContext`.

| Domain | Interpretation | Publication |
|---|---|---|
| `ModuleGraph` | IDs refer within one authoritative saved module graph, including structural Module slot `0` | The saved ID is registered to the independently allocated live object |
| `DetachedRecord` | Local IDs never silently bind through the active module graph | Policy may publish supported detached state; it does not invent a module identity |
| `Template` | ObjectId-shaped template data is not a saved module reference | A fresh object receives an independent runtime ID |

**A disk save session and a saved module world are independent facts.**
`GIT.UseTemplates` is the discriminator: `0` identifies an authoritative persisted graph;
a template GIT identifies a fresh installed world **even when the surrounding operation
restores a disk save session** — the normal retail transition-autosave topology, where
restoring the session must never promote template-local numbers into saved identities.

Module saved IDs are graph-local; saved and runtime IDs may differ arbitrarily, and
**numeric equality across domains proves nothing.** Inbound references are parsed first,
bound after materialization, published only after required bindings complete; export
translates live references **exactly once**; and the saved-graph generation scopes parsed
references, so retirement increments it and drops mappings. **Missing, deleted or
out-of-graph mandatory references fail closed**, and explicitly preserved unsupported
payloads remain shadows, never executable state. Detached records may contain real
effects and actions, and their parse context stays detached, **so coincident module
numbers cannot capture them.**
**World clock.** The canonical runtime clock is absolute simulation milliseconds; retail
decomposes it into `Mod_PauseDay`/`Mod_PauseTime`, and `Mod_MinPerHour` defines world-day
length but **does not accelerate elapsed time.** Snapshot output writes the retail pair
and removes the erroneous historical `Mod_CalendarDay`/`Mod_TimeOfDay`; template-world
autosaves seed from `AUTOSAVEPARAMS` instead. Saved events, delayed commands and effect
expiry use the reconstructed absolute clock, so their original schedule survives.

## Roster, residency, liveness

**The canonical companion identity is `(RosterKind, PartyTable slot)`**, with NPC and
Puppet as separate namespaces. **Availability, binding, membership and runtime existence
are separate authorities**, so removing availability neither destroys an independently
Area/GIT-owned creature nor finalizes a detached materialization. **Tag is an authored
lookup value, not roster identity** — an available NPC record, an available K2 puppet and
an ordinary GIT creature may all legally carry the same tag. GIT state is a module graph
and a detached roster snapshot is a save-wide record; **neither is opportunistically
merged into the other.** The canonical PC stays distinct from the currently controlled
creature.
**Area residency is distinct from both logical identity and object existence.** Full
departure ends the Area lifetime *while the Area, Rooms, Triggers and Pathfinder are still
alive*, retiring scene attachment, Room and Trigger tenancy, path handles, combat and
perception bindings, and live action objects — **after** the snapshot has captured
supported continuation — while **durable stats, HP, locals, inventory, roster state and
persistent mechanical effect semantics survive.** The order is fixed: `OnExit → snapshot →
full Area retirement → Module destruction`. **Same-Area reposition or control transfer
ends no Area lifetime**, and the separate APIs exist so module-boundary retirement is
unavailable as a convenient reposition helper.

**Every gameplay Object is `Constructing`, `Live`, `Retired`, or `Presentation`.** The
registry is the authoritative publication surface, **but non-owning references must also
prove they still address the same live incarnation** — `RuntimeObjectRef<T>` resolves
only if storage remains, the Object is `Live`, and the incarnation matches. **It never
resolves by numeric ID.** Within one session generation **every numeric ID that has named
a published incarnation stays reserved after that incarnation retires.** NWScript's ABI
is numeric, so immediate script arguments may expose an ID, but **a raw numeric value
alone never becomes persistence or logical identity.** Semantic destruction retires
children before their owner and removes the pointer-guarded registry entry — **registry
removal cannot unregister a newer object using the same number, and a retained strong
pointer cannot restore gameplay liveness.**

## Ownership and publication

**Every live Item has one stable authoritative disposition:** Area residency,
inventory/container ownership, one equipment slot, another owned graph, presentation-only
clone, or retirement. `Item::owner` is nested ownership, **not** Area residency, and
**`owner == 0` alone never makes an Item claimable** — moving a world Item into an owned
graph first proves exact active-Area residency, then releases Area indexes, tenancy and
scene attachment. **KOTOR shared-inventory selection is action/UI policy**, so generic
`Creature` equipment ownership does not consult Party policy.

Fallible construction uses **object-graph staging**: stage storage and identity,
deserialize and validate, establish the owner relationship *while still staged*, then a
no-throw publication callback, and only then mark candidates `Live`. **Staging is not
gameplay publication** — candidates are invisible to registry lookup until commit, and an
Area/GIT load uses one outer staged graph so nested factories do not publish
independently. Failure before object-level commit **leaves no semantically live orphan or
authoritative saved alias**; owned-graph replacement builds the new closure before a
`noexcept` owner swap, so failure before the swap leaves the prior graph live. **This
addresses ordinary construction failure, not catastrophic allocation failure.**

**Presentation isolation.** Previews use `Presentation` objects and presentation-only
IDs, and a character preview **clones** equipment rather than re-owning the player's Item.
**Retaining a node or preview after semantic destruction cannot make its Object
executable or resolvable.**

## Preparation, publication, and failure

The minimum useful guarantee:

> Enough destination resource and structural state is prepared and validated to reject
> avoidable structural failure before sacrificing the authoritative session.

**There is deliberately no private duplicate of the complete Game, Party, registry or
scene** — which is why some post-commit failures go to the Main Menu rather than
preserving the old world. **That is a smaller and more truthful guarantee.**

**Three failure scopes, and the distinction is the whole design.** Pre-commit failure
**preserves the old world**; post-commit failure **retires the partial destination**;
pre-object-commit failure **additionally guarantees the candidate never became
semantically live**. **No stable boundary intentionally depends on a later registry or
resource cleanup sweep.**

Orderings the pipelines must preserve, whatever their step lists look like:

- **The destination preflight runs before source `OnExit`**, and the source snapshot is
  adopted **only after** module resource publication succeeds. Preparation temporarily
  mounts the candidate plan and **excludes the outgoing module sources from candidate
  lookup**; an external resource changing between preparation and commit is a
  post-commit failure.
- **Re-entering the same module is the special case** — it must reprepare against the
  newly captured snapshot.
- **Live Party actions never cross the Area boundary.** Only save-facing records and
  already-bound runtime dependencies are reconstructed, the captured continuation is
  installed **exactly once**, and outgoing-Area-only targets fail closed rather than
  capturing a destination object through numeric equality.
- **Saved mechanical state is bound and published before entry hooks.** Shipped scripts
  query and clear those values, so publishing afterwards would let a hook inspect an
  empty object and then have the loader resurrect what the hook removed.
- **A template-world restore never reserves `ModuleGraph` IDs.** Installed objects get
  fresh runtime IDs, and full disk `CreateParty` semantics **clear detached follower
  action queues** where retail respawns them — intentionally different from ordinary
  travel.
- **Same-Area control transfer performs no snapshot, resource activation or
  retirement**; full session retirement is stronger than module retirement, which
  preserves save-wide and logical Party state.

**The loader is not a transaction over arbitrary NWScript.** Source `OnExit` is a known
weaker boundary — it runs against the authoritative source, and arbitrary script mutation
**is not rollbackable** if snapshot creation or same-module repreparation subsequently
fails. A generic rollback journal was rejected as disproportionate and semantically
misleading.

## Forbidden shortcuts

Inferring roster identity from a Tag or ResRef, or identity from equal saved, detached or
runtime ObjectIds · treating a template-world restore as an authoritative saved
`ModuleGraph` · treating roster availability change as semantic destruction · retaining
Room, Trigger, Pathfinder, combat or perception state across Area departure, or raw live
action pointers across a module boundary · treating `shared_ptr` storage as gameplay
liveness, or registry membership as persistence identity · registering a fallible object
before validation, or publishing candidates globally and relying on cleanup after
ordinary failure · one ambiguous "unload Party" operation for both departure and
repositioning · embedding KOTOR shared-inventory disposition inside generic `Creature`
equipment ownership · building rollback machinery for arbitrary authored scripts, or
shadow versions of the complete Party, registry, scene or engine.

## Roadmap boundaries

**Loader roadmap:** migrate remaining consumers of the legacy resource container to the
common loader contract, delete the legacy architecture after parity validation, then an
optional final cleanup. **These are backend-migration tasks and must not be absorbed into
the lifecycle work.**

Known and intentionally deferred: Area-runtime retirement still enumerates mixed Creature
fields, which a later component split can shrink without changing the lifetime law; retail
writer parity for `Mod_NextCharId`; some weather and transition-related saved GIT fields;
explicit registration for each new first-class GIT object type; and **`TemporaryDiscovery`
safely restores the current synchronous shared resource view, but true concurrent loading
requires isolation or synchronization.**
