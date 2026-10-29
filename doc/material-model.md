# Material model

The shading model PBR and the tracer share, implemented in `slang/lib/brdf.slang` and
`slang/lib/surface_model.slang`. **The shaders are the authority for the arithmetic;
this records why the model it replaced could not express a matte surface, and what is
still open.**

## Why the previous model failed

Shading before this model was `Rf0 = lerp(lerp(F0, albedo, metallic), 1, s_env)`, a
Schlick Fresnel on top, unnormalised GGX specular on every surface, and diffuse weighted
by the single-direction `1 - F(n.v)`. Three defects, **none of which the `f0` dial could
reach**: **Schlick has no weight**, so `F -> 1` at grazing for every `F0` and every
oblique wall got a sky-coloured lobe on top of full diffuse with the sampler following it
— **this was the white sheen**; **`s_env` was a hidden metalness**, pushing `Rf0` toward 1
*inside* the shading equation so rough versus shiny was decided by the env map's alpha
rather than any material parameter; and **diffuse and specular were not coupled**, so
grazing angles double-counted while rough normal incidence lost energy, single-scatter GGX
dropping ~60% at roughness 1.

## Parameters

Per material, all overridable per category:

| parameter | range | meaning |
|---|---|---|
| `baseColor` | rgb | albedo for dielectrics, F0 tint for metals |
| `metallic` | 0..1 | dielectric ↔ conductor |
| `roughness` | 0..1 | `α = roughness²` |
| `specular` | 0..1 | weight on the whole dielectric Fresnel curve. **`0` removes the lobe entirely, including at grazing.** Metals ignore it |
| `f0` | 0..1 | dielectric reflectance at normal incidence; 0.04 is IOR 1.5, 1 a mirror |
| `emission` | intensity, gamma | |

**Not implemented, and to be added only when an asset needs them:** `specularTint`,
`sheen`, clearcoat, transmission.

The key coupling: `(1 - E_spec)` — the energy the specular layer actually took at this
view angle over the whole hemisphere — replaces the single-direction `1 - F(n·v)` on
diffuse, **which is what removes the grazing double-count.** `E_spec` comes from the
same analytic split-sum fit that produces the denoiser's demodulation factors, so **the
model and the factors stay equal by construction** and the tracer needs no new binding.

Multiple-scatter compensation follows Turquin 2019: a scale on the existing lobe keyed
on the outgoing direction, which means **the sampling routine and PDF do not change** —
Kulla–Conty's alternative adds a separate lobe that cannot be importance-sampled
analytically. Compensation fixes energy *loss* at high roughness; it does **not**
address the grazing sheen, which is what the `specular` weight and the `E_spec`
coupling are for. Sampling uses `p_s = luminance(E_spec)`, the compensated albedo of
the specular layer, **so with `specular = 0` no specular path is ever traced.**

## The authored mask

Odyssey blends its (always sharp) environment map by `1 - alpha` of the diffuse texel.
**That channel is a painted reflectivity mask and carries no roughness** — treating low
alpha as low roughness is our reading, justified only because the original could not
blur.

So on a reflective surface `roughness`, `specular` and `f0` are each a **pair**, lerped
by the texel's alpha, and the reflective class defines what each end means. Defaults
`roughness {0.2, 1}`, `specular {1, 0.3}`, `f0 {1, 0.04}`: alpha 0 is the painted
mirror, alpha 1 a matte dielectric. Pinning a pair to one value ignores the mask for
that property; the rough class has no mask and stores its single value in both ends.
**The global roughness floor is gone — the alpha-0 roughness is the floor, per class.**
Env-map presence classifies a material on the CPU, and **the shader receives PBR
quantities and the mask, nothing else.**

Coverage the parameters are meant to span: plaster, dirt and grass ground at specular
0–0.3 / roughness 0.8–1; painted and plastic at 0.5 / 0.3–0.6; glossy floors at 0.5–1
tinted / 0.1–0.3; brushed metal at metallic 1 / roughness 0.4–0.7; polished metal below
0.2. **KotOR's authored data supplies only two signals — env-map presence and
self-illumination — so everything else is the category override.**

## Still open

A **white-furnace check** of `E_spec + diffuse albedo <= 1` per roughness in the testbed
has not been run (RAS-040); retro is untouched by this model, and PBR and traced galleries
were flipped against the previous build when it landed. The retired dial keys
`cat{i}reflroughness`, `reflroughnessscale`, `reflmetallicscale`, `reflf0` and
`ptroughnessfloor` are **read and ignored**, so an old `reone.cfg` or command line does not
fail — it silently has no effect.
