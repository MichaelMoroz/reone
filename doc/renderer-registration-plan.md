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
