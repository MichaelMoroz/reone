# Moving the renderer from submission to registration

Why the current hand-off between the scene and the renderer does not survive
ray tracing, and what to replace it with. Written from the code as it stands;
every file:line below is a thing to read before changing it.

## What happens today

The 3D scene never reaches `IRenderer`. That interface
(`include/reone/graphics/renderer.h:39`) is about frames and presentation -
`beginFrame`, `drawSceneOutput`, `captureFrame`, `endFrame` - and the scene
arrives as a single finished texture.

The scene-shaped interface is `IRenderPass`
(`include/reone/scene/render/pass.h:69`), in the scene library. The flow is:

1. `SceneGraph` culls and sorts, then calls `pipeline.inRenderPass(name, cb)` -
   `src/libs/scene/graph.cpp:536-551`, once per pass.
2. The pipeline binds that pass's targets and invokes the callback with an
   `IRenderPass &`.
3. The callback issues per-object draws: `draw`, `drawSkinned`, `drawDangly`,
   `drawSaber`, `drawBillboard`, `drawParticles`, `drawGrass`, `drawAABB`.
4. `pipeline.render()` returns the composited `Texture &`.

Control is already inverted - the pipeline calls back into the scene rather
than receiving a list. That part is good and should survive.

## What does not survive

**Culling policy lives in the wrong library.** `SceneGraph` decides visibility
against the camera frustum and hands over survivors. That is a rasterisation
policy: rays hit geometry behind the camera, so a TLAS built from the visible
set has holes in every reflection and every shadow. The shadow passes already
show the strain, re-walking the graph to cull against a light frustum - the
same operation on the same set under a different policy.

**Nothing has identity between frames.** A TLAS instance index, temporal
accumulation, a per-node BLAS and a motion vector all need an object to still
be the same object next frame. `prevTransform` is threaded through every draw
call precisely because there is nowhere to keep it.

**Instanced geometry has no material.** `drawParticles` and `drawGrass` take a
bare `graphics::Texture &` while everything else takes a `Material &`. A hit
shader cannot have two ways of asking what it hit.

## The shape to move to

Registration, with the renderer owning culling:

- `registerObject` - one per `MeshSceneNode`. Carries mesh, material,
  transform, and for the deforming variants their bones or positions.
- `registerInstancedObject` - one per `GrassSceneNode` or emitter, not one per
  cluster or particle. Carries a material and the description needed to
  generate instances.
- `unregister`, plus update paths. **"Register once" is a lie for anything
  animated**: bones, dangly positions, transforms and UV scroll all change per
  frame. The invalidation contract is where retained-mode designs go wrong, and
  its failure mode - a stale value - is much harder to find than a missing draw
  call. `invalidateResources`/`invalidateTexture` on `IRenderer` are a preview
  of this.

Culling then becomes per-pass policy over one registry: camera frustum for the
G-buffer, light frustum for shadows, distance and relevance for the TLAS, and
nothing at all for a full-scene bake.

`Material::staticObject` stops being advisory and starts selecting BLAS build
flags - `PREFER_FAST_TRACE` plus compaction against `PREFER_FAST_BUILD`.

## Acceleration structures

The axis that matters is not "skinned or not" but **whether a BLAS can be
shared between instances**.

| category | BLAS | keyed on | per frame |
|---|---|---|---|
| rigid mesh | shared | `Mesh` | nothing |
| skinned / dangly / saber | one each | scene node | refit |
| grass / particles | one unit quad, static | - | TLAS instances only |

Rigid meshes have fixed object-space vertices, so one BLAS serves every
instance and the TLAS carries the transform. Deforming meshes differ per
*node*, not per mesh - two characters sharing a model are in different poses -
so each needs its own, refit each frame.

Only **skinned** meshes need a compute pass to produce vertices. Dangly
positions are already computed CPU-side
(`src/libs/scene/node/mesh.cpp:316-327`) and handed over as an array; saber is
a single displacement vector and is cheap to apply on the CPU rather than run a
pass for.

For grass and particles, prefer **one static unit-quad BLAS plus N TLAS
instances** over baking instances into a per-frame BLAS. The TLAS is rebuilt
every frame regardless, so this costs nothing extra and avoids rebuilding a
BLAS over hundreds of thousands of triangles. The cost is instance-buffer
bandwidth, 64 bytes each; distance culling that the renderer now owns cuts it.

## Two things that break under ray tracing regardless

**Camera-facing geometry.** `ParticleInstance` carries `right` and `up`
computed per frame from the camera. Correct for the primary view, meaningless
for a reflection or shadow ray, and edge-on such a quad is invisible. Options:
a spherical or cross-quad proxy in the AS, or keep particles rasterised and
composite them over the traced image. Choose deliberately rather than
discovering that smoke has vanished from a reflection.

**Grass placement is view-dependent.** See below.

## What grass actually is

Not an object with an extent and a region. `GrassSceneNode` holds a reference
to an `aabbNode` - a mesh from the room model - and at
`src/libs/scene/node/grass.cpp:47` keeps every face of it whose `face.material`
appears in `_properties.materials`. The grass region *is* the union of those
faces, fixed at init. Each cluster inherits the face's lightmap UV via
`tryFaceUV2`, so grass is lit by the room's lightmap.

`update()` is then pure view-dependent LOD: clusters beyond
`kMaxClusterDistance2` return to a fixed pool, faces coming into range draw
from it, count per face derived from area by `getNumClustersInFace`.

So "grass everywhere it can go" is already well posed - the tagged faces. What
is view-dependent is only how many are currently realised, which is exactly the
policy to move into the renderer. `radius` on `drawGrass`, `kMaxClusterDistance2`
and the cluster pool all leave the scene node.

**Check placement determinism first.** If cluster position and variant come
from a global RNG advanced in materialisation order, the same face yields
different grass depending on when the camera approached it. Invisible today
because there is one viewpoint; immediately visible under ray tracing when a
reflection shows a hillside populated differently from the direct view. Derive
position and variant from a hash of (face index, cluster index) instead. Worth
doing on its own - it also makes captures reproducible.

**Measure the total instance count** for a real outdoor area before assuming
"everywhere" is affordable: grass faces times clusters per face, summed, in
something like danm14ab.

## Materials

`Material` (`include/reone/graphics/material.h:38`) carries type, a
`TextureUnit -> Texture &` map, a `mat3x4` UV transform, colour, bump-map
frame, ambient/diffuse/self-illum colours, the static/shadows/fog flags, and
optional blend, cull and polygon-mode overrides. Everything else is derived by
`materialFeatureMask` in the same header.

Three changes:

- Add `Grass` and `Particle` to `MaterialType`, and route
  `drawParticles`/`drawGrass` through `Material` like everything else.
- Flatten `textures` from an `unordered_map` of reference wrappers into fixed
  slot indices into a bindless descriptor array. A hit shader must reach any
  material and any texture at intersection time; it cannot bind six units
  before a draw.
- Serialise the rest into a GPU material buffer indexed by TLAS instance custom
  index.

Note what is *not* there: no metalness, no roughness. Roughness is scavenged
from diffuse alpha, which since the environment-strength change also carries
reflection strength; the MDL reader reads specular and shininess and discards
them. A hit shader will be inferring PBR terms in exactly the place the deferred
resolve infers them today, which is the right place for that to stay.

## What this deletes

Several G-buffer packing tricks exist only because a deferred resolve cannot
reach the material:

- the environment-map derived layer index, smuggled through self-illum alpha as
  a byte;
- the geometry feature bits packed into lightmap alpha.

A hit shader indexes the material directly, so both stop being load-bearing -
and with them goes the class of bug where a stale layer index bleeds onto a
surface that never had an environment map.

## Objects have no identity yet

Retained identity needs a key, and the scene has none.

- `SceneNode` carries no id and no name. It has `IUser *_user`, but `IUser`
  (`include/reone/scene/user.h:24`) is an empty interface - a virtual
  destructor and nothing else - so it is a back-pointer tag with nothing
  readable on it.
- Game `Object::id()` is a real, stable `uint32_t`
  (`include/reone/game/object.h:85`), but reaching it means `SceneNode::user()`
  plus a downcast to `game::Object`, and the scene library does not depend on
  game. The registry cannot use it without inverting that dependency.
- `graphics::ModelNode::number()` is a `uint16_t` unique *within a model*
  (`include/reone/graphics/modelnode.h:179`). Useful as half a composite key,
  useless alone.
- Pointer identity is what is actually used - `VulkanResources` caches by
  `Mesh *`, and the registry carries a `ModelSceneNode *` for culling.

Pointer identity has a failure mode this codebase already knows about.
`IRenderer::invalidateResources` exists for it, and says so: a backend caching
by address "cannot otherwise tell that an address has been reused by a
different object, and would hand a new mesh the previous one's buffers". That
is a bulk sledgehammer swung on module transition. Adequate for a texture
cache; useless for a TLAS instance index, where what matters is knowing *which*
object went away.

**Give `SceneNode` a generation-stamped handle** - a `uint32_t` index and a
`uint32_t` generation, or one packed 64-bit value. The scene graph assigns on
creation and bumps the generation on destruction, so a reused slot yields a
handle that compares unequal to the stale one.

That buys three things at once: the registry gets a key that survives a frame,
a TLAS instance and a per-node BLAS get something to hang off, and
`invalidateResources` can stop being a sledgehammer because a stale handle
becomes detectable rather than merely suspected.

Cheap while the registry is being reshaped. Awkward once things are keyed on
pointers.

### The handle should carry meaning, not just be unique

A bare integer identifies an object and tells you nothing about it. Debugging
this renderer is mostly the question "what is that thing" - most of a night
went into a missing head that would have been half an hour if a pixel could
have named itself.

Nothing needs authoring; the names already exist and are simply not reachable
from the scene library:

- `graphics::Model::name()` is the MDL resref - `c_drdastro`, `m14aa_01a`
  (`include/reone/graphics/model.h:43`);
- `graphics::ModelNode::name()` is the node within it - `head_g`, `wall_01`
  (`include/reone/graphics/modelnode.h:180`);
- `game::ObjectType` gives Creature, Placeable, Door, Trigger and the rest
  (`include/reone/game/types.h:248`), where a game object is behind the node.

**Intern them; do not store strings.** A registry entry is copied for every
object every frame, and a `std::string` per entry would cost more than
everything else in it. Keep a string table on the scene graph and put a
`uint32_t` id in the handle. Copying stays a few words, and the text is
resolved only when something actually displays it.

So the handle is roughly an index, a generation, a type, and two interned
name ids - model and node. Small enough to sit in a registry entry, and
enough to answer "what is that" without a second lookup structure.

Where it pays:

- the render target viewer can name what is under the cursor;
- a hit record in a path tracer carries an instance index, which resolves to
  `c_drdastro / head_g` rather than to a number;
- "was this registered, and did it survive culling" becomes a question with a
  readable answer.

One constraint from the destination: a TLAS instance custom index is 24 bits,
so whatever part of the handle is used as one has to fit in that.

## Registry lifetime: rebuilt per frame, for now

The registry is currently cleared and refilled every frame. That is a stopgap
chosen because it is correct by construction - there is no invalidation
contract to get wrong - and not because it is the right end state.

Two reasons it does not survive contact with ray tracing.

**Cost.** A frame in danm14ab registers around 1,500 entries, each carrying an
owned `Material`, and 551 of them are dangly meshes each copying a
`std::vector<glm::vec4>` of per-vertex positions. Those positions are already
recomputed on the CPU every frame, so they are computed and then copied.
Measure this before defending the rebuild; if the dangly copies dominate, that
alone justifies moving earlier than the schedule below suggests.

**Identity.** A TLAS instance index, a cached BLAS and temporal accumulation
all key off an object still being the same object next frame. Nothing can be
stable if every entry is fresh. Retained identity is a prerequisite there, not
an optimisation.

The way out is not "register once and never touch it", which is the promise the
plan warns against elsewhere. Split by how often a field actually changes:

- **Identity is retained.** An object registers when it enters the scene and
  unregisters when it leaves. The TLAS instance index and the cached BLAS hang
  off this.
- **Volatile state is written every frame** - transform, previous transform,
  bones, dangly positions. These change every frame regardless, so there is no
  invalidation to get wrong; they are overwritten, not invalidated.
- **Only rare, static things need an invalidation hook** - a material change, a
  texture swap, visibility. `Material::staticObject` already marks most of what
  never needs one.

That confines the hard part to a small and infrequent set rather than to the
whole registry, which is what makes the contract tractable.

**When.** Keep the per-frame rebuild until BLAS work begins. That is the point
where identity stops being an efficiency question and becomes a correctness
one, and it is far enough out that the registry's shape will have settled.

## Sequencing

Doing this while both backends exist means implementing it twice and
destabilising the reference the parity work is measured against. Doing it after
OpenGL is deleted means one implementation.

There is a middle path that does not force the choice. The **Vulkan
`IRenderPass` implementation can record instead of draw** - same `SceneGraph`
traversal, same callbacks, but it populates a registry as a side effect. That
yields the retained structure and the TLAS instance list without touching the
scene library or OpenGL, and it answers the granularity and update-model
questions against real data. Flipping ownership afterwards, so the renderer
culls rather than receiving pre-culled survivors, is a much smaller change
against a design already validated.

Order:

1. Record-instead-of-draw in the Vulkan pass. Registry built, nothing else
   changes.
2. Material changes: `Grass`/`Particle` types, `Material` on the instanced
   entry points, bindless flattening.
3. Grass placement determinism, independent of everything else.
4. Skinning compute pass and per-node BLAS for the deforming variants.
5. BLAS/TLAS build and refit off the registry.
6. Move culling into the renderer; drop `radius` and the cluster pool.

`slang/pbr_resolve.slang` fills `SurfaceParams` from a G-buffer and notes that
a path tracer fills the same struct from a hit record, everything downstream
shared. That is the seam all of this hangs off; keep it.

## Open questions

- Granularity for emitters: one registered object per emitter, or per particle
  system when an emitter has several?
- Do walkmeshes and AABB debug geometry register at all, or stay immediate-mode
  alongside the 2D and GUI layers, which must keep explicit draw order?
- Does the render-target viewer's need for backend-neutral target handles fold
  into this, or stay separate? It is currently the reason the Vulkan editor has
  no preview pane.
