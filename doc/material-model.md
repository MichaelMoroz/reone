# Material model: specular-weighted, energy-coupled metallic-roughness

The shading model the PBR raster mode and the path tracer share, why the model
it replaced could not express a matte surface, and what of the design is still
open. Implemented in `slang/lib/brdf.slang` and `slang/lib/surface_model.slang`.

## The problem with the previous model

Shading before this model:

```
Rf0      = lerp(lerp(F0, albedo, metallic), 1, s_env)
F(μ)     = Rf0 + (1 - Rf0) (1 - μ)^5
specular = D G F / (4 n·v)                       on every surface
diffuse  = (1 - F(n·v)) (1 - metallic) albedo / π
p_s      = luminance(F(n·v))                     specular sampling probability
```

Three defects, none of which the `f0` dial can reach:

1. **Schlick has no weight.** `F → 1` at grazing for every `F0`, including 0.
   Every oblique wall and floor gets a sky-coloured lobe added on top of full
   diffuse, and the sampler follows it (`p_s → 1`). This is the white sheen.
2. **`s_env` is a hidden metalness.** Env-map presence pushes `Rf0` toward 1
   inside the shading equation. Rough versus shiny was decided by the env map's
   alpha, not by any material parameter.
3. **Diffuse and specular are not coupled.** Diffuse is weighted by the
   single-direction `1 - F(n·v)`; specular is not normalized at all. Grazing
   angles double-count, rough normal incidence loses energy (single-scatter
   GGX drops ~60% at roughness 1).

## The model

Parameters per material, all overridable per category:

| parameter | range | meaning |
|---|---|---|
| `baseColor` | rgb | albedo for dielectrics, F0 tint for metals |
| `metallic` | 0..1 | dielectric ↔ conductor |
| `roughness` | 0..1 | `α = roughness²` |
| `specular` | 0..1 | weight on the whole dielectric Fresnel curve. `0` removes the lobe entirely, including at grazing. Metals ignore it |
| `f0` | 0..1 | dielectric reflectance at normal incidence; 0.04 is IOR 1.5, 1 a mirror |
| `emission` | intensity, gamma | as before |

On a reflective (env-mapped) surface `roughness`, `specular` and `f0` are not
single values but each a pair, lerped by the texel's alpha; see *The authored
mask* below.

Not implemented, add only when an asset needs them: `specularTint` (dielectric
F0 toward `baseColor`), `sheen` (Burley's `(1 - cos θ_d)^5` cloth backscatter),
clearcoat, transmission.

### Evaluation

```
f0      = lerp(f0, baseColor, metallic)             SpecularLayer.f0
w       = lerp(specular, 1, metallic)               SpecularLayer.weight
F(μ)    = w · (f0 + (1 - f0)(1 - μ)^5)              specularFresnel

spec_ss = D G F / (4 n·l n·v)                       unchanged GGX

E_ss    = A(μ_v, r) + B(μ_v, r)                     split-sum fit with F0 = 1
k_ms    = w · f0 · (1 - E_ss) / E_ss                Turquin 2019, eq. 16
spec    = spec_ss · (1 + k_ms)

E_spec  = w · (f0 · A + B) · (1 + k_ms)             compensated directional albedo
diffuse = (1 - metallic) · (1 - E_spec) · baseColor / π
```

`(1 - E_spec)` is the energy the specular layer actually took at this view
angle over the whole hemisphere; it replaces the single-direction `1 - F(n·v)`
and removes the grazing double-count.

`A`, `B` come from `nrdEnvironmentTermRtg`, the analytic split-sum fit that
already produces the denoiser's demodulation factors: it is linear in F0, so
its F0 = 1 value is Turquin's `E_ss`. Using the same fit for the model and for
the factors keeps them equal by construction, and the tracer needs no new
binding. The raster's ambient specular keeps `sBRDFLUT` for the prefiltered
environment integral, scaled by `w` and `(1 + k_ms)` like every other lobe.

### The authored mask

Odyssey blends its (always sharp) environment map by `1 - alpha` of the
diffuse texel: KotOR.js `ShaderOdysseyModel.ts` has
`outgoingLight += envColor * (1.0 - diffuseColor.a)`. The channel is a painted
reflectivity mask and carries no roughness; treating low alpha as low
roughness is our reading, justified only because the original could not blur.

So the reflective class defines what each end of the mask means, and the
texel lerps between them:

```
roughness = lerp(roughness[0], roughness[1], alpha)
specular  = lerp(specular[0],  specular[1],  alpha)
f0        = lerp(f0[0],        f0[1],        alpha)
```

Defaults `roughness {0.2, 1}`, `specular {1, 0.3}`, `f0 {1, 0.04}`: alpha 0 is
the painted mirror (sharp, F0 → 1), alpha 1 a matte dielectric. Pinning a pair
to one value ignores the mask for that property. The rough class (no env map)
has no mask and stores its single values in both ends. Metalness is a single
value per class, negative for curated. The global roughness floor is gone: the
alpha-0 roughness is the floor, per class.

Env-map presence classifies a material on the CPU; the shader receives PBR
quantities and the mask, nothing else.

### Multiple scattering (Turquin 2019)

*Practical multiple scattering compensation for microfacet models*, ILM.

- Single-scatter GGX loses the energy of rays that hit a second microfacet.
  The fix is a scale on the existing lobe keyed on the outgoing direction only:
  `ρ = (1 + k_ms(ω_o)) ρ_ss`, `k_ms = (1 - E_ss) / E_ss` for `F = 1` (eq. 9–10).
- Fresnel absorption per bounce is folded in by multiplying `k_ms` by an
  average Fresnel term. The paper tests `E_avg / (1 - F_ss (1 - E_avg))`,
  `F_ss^(α√α)`, `F_ss` and plain `F0`; all are visually equivalent, and `F0`
  (eq. 15–16) is the production choice.
- For dielectrics with transmission, reflection and refraction lobes are
  normalized together by `E_ss^R + E_ss^T` (eq. 18). Not needed until glass.
- Because the result is a pure scaling of `ρ_ss`, **the sampling routine and
  PDF do not change** (§4). Kulla–Conty's alternative adds a separate lobe that
  cannot be importance-sampled analytically. Turquin's form is not reciprocal,
  which does not matter for a unidirectional tracer.
- Compensation fixes energy *loss* at high roughness (rough conductors going
  dark and desaturated). It does not address the grazing sheen; that comes from
  the `specular` weight and the `E_spec` coupling above.

### Sampling

- `p_s = luminance(E_spec)`, the compensated albedo of the specular layer, not
  `luminance(F(n·v))`. With `specular = 0` no specular path is ever traced.
- Specular branch: VNDF sampling; weight `F(v·h) · G1(n·l) · (1 + k_ms) / p_s`.
- Diffuse branch: cosine sampling; weight `(1 - E_spec)(1 - metallic) baseColor / (1 - p_s)`,
  which is the primary's `diffFactor` exactly, so the demodulated weight is
  `1 / (1 - p_s)`.

### Coverage

| surface | specular | metallic | roughness |
|---|---|---|---|
| plaster, dirt, grass ground | 0–0.3 | 0 | 0.8–1 |
| painted, plastic | 0.5 | 0 | 0.3–0.6 |
| glossy floor | 0.5–1 (tinted) | 0 | 0.1–0.3 |
| brushed metal | – | 1 | 0.4–0.7 |
| polished metal | – | 1 | < 0.2 |

KotOR's authored data supplies two signals, env-map presence and
self-illumination; they map to category defaults for `specular` / `metallic`
and to `emission`. Everything else is the category override.

### Cost

Two evaluations of the split-sum polynomial per shaded hit and per light draw;
no fetches.

### Migration

- Rough class: `roughness`, `metallic`, `specular {0.3}`, `f0 {0.04}`.
  Reflective class: `roughness0/1`, `specular0/1`, `f00/1`, `metallic`
  (negative: curated). Retired keys `cat{i}reflroughness` (maps to both ends),
  `reflroughnessscale`, `reflmetallicscale`, `reflf0`, `ptroughnessfloor` are
  read and ignored.
- `InstanceMaterial.overrideParams = (r0, r1, s0, s1)`, `overrideF0 = (f0_0,
  f0_1)`, `overrideMetallic`; 320 bytes.
- Verification: retro untouched (no retro shader imports the changed modules);
  PBR and traced galleries flipped against the previous build. Still to do: a
  white-furnace check of `E_spec + diffuse albedo ≤ 1` per roughness in the
  testbed.
