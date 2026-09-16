# model.vertex-color

How a `.pwmodel` carries colour, and why a file full of `color=` can still compile to a uniformly grey asset. Read [`model.authoring`](model.authoring.md) first for the `materials { }` block and tagging rules; this page covers material **slots**, vertex **colour**, the material needed to see it, and the ops `color=` cannot reach.

The example corpus ships six ready instances over two masters under `/Game/PinWrightExamples/Materials/` (see the table below); twelve of thirteen models bind them, making it a worked reference as well as a trap illustration. A third master, `M_PwModelExample_WeatheredWood`, has no instance family: its defaults are driftwood's and `driftwood.pwmodel` binds both slots (`Bark`, `Heartwood`) directly to the master, with per-wood recipes in its MGIR header.

## Slot for shading behaviour, vertex colour for tint

A slot boundary costs a draw call and exists to separate **surfaces that shade differently** — iron against gasket, clay against wick, glazing against stone. Different roughness, different metallic, different blend mode.

`color=` on a generator costs nothing and separates **tint within one surface** — twelve crystals of the same quartz, five driftwood limbs of the same bleached wood, a warm tread against a cool stringer.

The test is whether the two regions want different *shading*, not different *colour*. Twelve tinted crystals in one `Quartz` slot is right; twelve slots named after the tints is wrong, and the compiler will not stop you.

## Vertex colour names the region; a graph carries everything finer

The ceiling is structural, not a budget: the overlay stores one value per triangle **corner**, so vertex colour is flat per face and can only say which *region* a face belongs to. Grain, fibre, weathering and wear are sub-face detail and need a material graph.

**Tint the procedural result by vertex colour; never replace it.** `Examples/mgir/M_PwModelExample_WeatheredWood.mgir` keeps `MaterialExpressionVertexColor` as the tone the whole surface is built on and multiplies an anisotropic grain, a sharpened check noise and a weathering mask over it, so a model's `color=` ops keep working and its other slots' bindings are unaffected. A graph that wires its own tint into Base Color instead discards every op this page is about. Sixteen parameters in groups `Grain` / `Checks` / `Weathering` / `Surface`, a tangent-space Normal from a finite difference of the grain one step along U, and no texture asset at all — `NOISEFUNCTION_GradientALU` is pure ALU. The header carries the rationale, the UV-axis derivation and the wood table; read it before copying a number.

Measured on `driftwood`: bound to the flat matte dielectric it read as painted clay. Same mesh, same vertex colours, under the procedural master it reads as sea-bleached wood.

## Saturation makes vertex colour carry weathering state

`Bleach` is scaled per pixel by `1 - saturate(vertexColourSaturation * FreshSensitivity)`. A weathered body tint is near-neutral and a fresh-break tint is not, so colours already in the mesh say **how weathered** a face is as well as what tone it is — no extra channel, no second slot, no second shading behaviour to justify.

| `driftwood` region | vertex colour | saturation | result |
|---|---|---|---|
| trunk body | (0.30, 0.29, 0.265) | 0.12 | bleached |
| fissure tools | (0.19, 0.17, 0.135) | 0.29 | part bleached |
| break planes | (0.35, 0.30, 0.245) | 0.30 | part bleached |
| heartwood tool | (0.66, 0.53, 0.34) | 0.48 | left warm |

Two escape hatches: `FreshSensitivity: 0` pins the mask at 1 and bleaches everything uniformly; `Bleach: 0` skips the axis entirely, leaving a grain-and-roughness pass over the model's own colours — that is what an oak or boxwood consumer sets.

## Nothing in engine content samples vertex colour

**This is the trap.** `color=` lands in a per-corner colour overlay that the compiler carries into the asset's `FColorVertexBuffer`, so the data is on disk and `static_mesh.describe` will not tell you anything is wrong. But `BasicShapeMaterial`, `WorldGridMaterial`, `M_Grid` and `DefaultMaterial` all ignore vertex colour, so a model bound to any of them renders every op the same flat grey. A write that never happened looks identical to one that did.

Two ways to see it:

- `/Engine/EngineDebugMaterials/VertexColorViewMode_ColorOnly` — a debug material that shows the raw channel, for checking the data landed.
- A material with a `MaterialExpressionVertexColor` node wired into Base Color — for shipping.

Vertex colour also needs **light**. A face the level does not light renders near-black whatever its tint, so judge colour on a surface you can see is lit, from more than one angle. In a level with no ambient — `ExampleProjectWelcome` sits at mean luminance 0.00015 — every downward-facing face is unlit and the material reads as broken when it is not.

## The material, as MGIR source

`Examples/mgir/M_PwModelExample_VertexColor.mgir` in the plugin folder is that material as text. No `.uasset` ships with PinWright; compile the text into your own project:

```
call("material.compile_mgir", { text: <file contents>, save: true })
call("material.authoring.compile_material",
     { assetPath: "/Game/PinWrightExamples/Materials/M_PwModelExample_VertexColor" })
```

The second call is not a formality: `material.compile_mgir` does **not** block on shader compilation and returns no compile errors, so a broken graph surfaces only in `material.authoring.compile_material`'s `compileSucceeded`, never in the first response's `blocksCompiled`.

The graph is four nodes: `MaterialExpressionVertexColor` → Base Color, and scalar parameters `Roughness` / `Metallic` / `Specular` → their own inputs. One graph covers every surface family because the family lives in a material instance, not in a second graph.

`Examples/mgir/M_PwModelExample_VertexColorTranslucent.mgir` is the glass variant: same graph plus `Opacity = VertexColor.A * OpacityFloor`. It is a **separate master** rather than another instance because blend mode, two-sided and translucency lighting mode are material-level properties — no `material.authoring` verb overrides them on a `UMaterialInstanceConstant`, and `set_blend_mode` on an instance fails `ASSET_NOT_FOUND: Could not load Material`.

**MGIR carries material-level properties, and the round trip reproduces them.** Nine travel as `property Name: Value` lines inside the `entry material` block — `MaterialDomain`, `BlendMode`, `ShadingModel`, `TwoSided`, `bIsThinSurface`, `TranslucencyLightingMode`, `bUseMaterialAttributes`, `OpacityMaskClipValue`, `NumCustomizedUVs`. Measured this session on `M_PwModelExample_WeatheredWood`, a decompile → compile round trip returned all nine, plus every parameter default and the `%sym.r` / `%sym.g` channel swizzles, exactly. Full table in [`material.mgir`](material.mgir.md).

**A document pinned before the `property` opcode shipped carries no property lines**, and recompiling it therefore leaves the target at whatever it already was. On a fresh material that is the opaque default: an *opaque* asset with an Opacity pin the renderer silently ignores, every response reporting success — measured, `blendMode: "Opaque"`, `twoSided: false` against the original's `Translucent` / `true`. `M_PwModelExample_VertexColorTranslucent.mgir` is exactly such a document, so the four follow-up calls its header lists are still what that file needs until it is re-decompiled.

## Instances are not MGIR either

`material.decompile_mgir` refuses a `UMaterialInstanceConstant` outright (`ASSET_NOT_FOUND`), so the family table ships as a call sequence. Each is `material.authoring.create_material_instance` with `parentMaterial` set to one of the two masters:

| Instance | Parent | Roughness | Metallic | Specular | For |
|---|---|---|---|---|---|
| `MI_PwModelExample_MatteDielectric` | opaque | 0.85 | 0 | 0.35 | stone, slate, terracotta, unfinished wood, host rock |
| `MI_PwModelExample_SatinDielectric` | opaque | 0.30 | 0 | 0.60 | glaze, varnish, polished boxwood |
| `MI_PwModelExample_PolishedMetal` | opaque | 0.15 | 1 | 0.50 | brass, bronze |
| `MI_PwModelExample_RoughMetal` | opaque | 0.55 | 1 | 0.50 | cast iron, steel, tread plate |
| `MI_PwModelExample_Cloth` | opaque | 0.95 | 0 | 0.10 | felt, wick fibre |
| `MI_PwModelExample_Glass` | translucent | 0.05 | 0 | 1.00 | glazing, quartz (`OpacityFloor` 0.35) |

`create_material_instance` takes the whole set inline as `parameters: { scalar: { … } }`, one recompile for the batch.

**The two metal instances need something to reflect.** `Metallic: 1` means no diffuse response at all, so in a level with only point lights and no sky light or reflection capture the metal reads near-black except where a specular highlight lands — measured, the same seven tinted boxes that render as saturated diffuse at `Metallic: 0` render as three dim tinted sheens and four black ones at `Metallic: 1`. That is correct PBR, not a broken binding. Add a sky light before judging a metal family.

## An unresolved binding is safe

Binding a slot to a material the consumer has not created yet degrades cleanly: the compile still succeeds, the asset is still written, and the slot falls back to the default material with

```
PWMODEL_STAGE_WARNING: Material slot 'Shell' is bound to '/Game/…/M_NotThere',
which could not be loaded; using the default material
```

That is why an example may name a path that does not exist in every project. **`model.validate` reports it too**, in the same sentence — it loads every binding whose slot the geometry actually tags, so the check no longer waits for a compile. Its warning is anchored on the `materials { }` line the binding is written on, where the compile path resolves bindings at asset creation and has no position to report (line -1). A binding for a slot nothing tags is not resolved at all: it already carries `PWMODEL_UNUSED_MATERIAL` from the parser and is dropped before the creator sees it.

## What vertex colour cannot do

- **8 bits per channel.** The value round-trips through an `SRGBToLinear` / `LinearToSRGB` pair that cancel approximately, not exactly, with the error worst in the darks. Fine for tint and for masking; wrong for carrying precise data into a shader.
- **No gradients across a flat face.** A one-quad face has four corner samples and shades flat. Gradients need the face subdivided — use `segments=` on the generator before writing colours.
- **A tint and a mask CAN now share the four channels** — this used to be the entry that said they could not. `set_vertex_color` takes a `channels=` mask on both the RPC and the `.pwmodel` op, so an alpha write leaves a per-part RGB ladder alone; see [`model.authoring`](model.authoring.md). The first producer of a second signal to put there is the ambient-occlusion bake — `bake_ao` inside a model source, `geometry.bake_ambient_occlusion` on the RPC surface — which is what finally makes a contact shadow at a part junction authorable.
- **Modifiers do not tag. `set_vertex_color set_all=true` used not to close the gap; it now does, and the table below has not yet been re-measured.** `set_all` no longer paints only existing colour elements: it creates seam-preserving elements for triangles carrying none before painting, and reports `colorElementsCreated` when needed. Every **no** in the table is therefore OLD behaviour, kept because the renders were real and a re-measurement needs a baseline; treat it as history until a fresh probe replaces it. What has *not* changed: `color=` is still generator-only, the compiler's scratch-to-target append still has no colour reconciliation (the UV side's `ReconcileUVChannelsForAppend` still has no colour counterpart), and no `PWMODEL_*` diagnostic flags colourless vertices. Write `color=` on every generator as the reliable rule.

- **Old measurements (superseded above).** `color=` is a parameter on primitive generators only; `sweep` and `extrude_along_spline` take none, while `append_buffers` carries its own `colors=` buffer. `set_vertex_color set_all=true` is the only op that *could* paint the rest, and measured, it does not:

  | geometry | reached by `set_all`? |
  |---|---|
  | vertices a generator made | yes |
  | `extrude_along_spline` output | **no** |
  | `bevel` chamfer faces | **no** |
  | `shell` inner wall | **no** |
  | geometry a boolean tool contributed | **unreliable** — reached in one probe, missed in another |

  Probe: two parts, both slots on the same matte instance so the result is a colour fact and not a shading one. A `box` with `color=red` renders red, a `sphere` with `color=blue` renders blue, and a rod built by `extrude_along_spline` in the same part with `set_vertex_color set_all=true color=blue` after it renders **white**. Nothing reports it — no diagnostic, no count, no `health` field; the compile is clean and the geometry is white. Position does not help: the op was tried immediately after the booleans and as the part's last op, with the same result.

  Re-probed 2026-08-21 on a four-cube row, one matte slot, all four cubes `color=(0.85, 0.12, 0.08)`: **plain** renders red on every face; **bevelled** renders red with white chamfer strips; **bevelled then `set_vertex_color set_all=true`** renders *identically* to the bevelled one, so the blanket recolour is a measured no-op on chamfers; and a cube with an **uncoloured `subtract` tool** taken out of its top renders the crater interior **white**. Put `color=` on that same tool and the crater comes back red. So the tool's own colour is the working control, and the "unreliable" row above is about what happens with **no** colour on the tool: `ships_wheel`'s hub socket was measured taking the nave's tint unasked (adding `color=` left the render byte-identical and the vertex count at 11,840), while `chess_rook`'s dished top and crenellation slots came out white.

  Six of the eleven reviewed examples were carrying uncoloured boolean tools when this was checked. `chess_rook` was the visible one — a white ring at each crown rim from `bevel`, a white saucer where the dish was subtracted, and six white slot interiors, all on an otherwise boxwood piece, and all camera-stable across a 110° orbit, which is what separates them from a specular highlight. `amphora`'s neck bore, `pipe_junction`'s eight bolt holes and both limb bores, `spur_gear`'s tooth flanks / pockets / bores and `spiral_stair`'s tread undersides were the rest.

  **The practical rule is to write `color=` on every generator, including the ones inside `union { }` blocks** — a tool inside a union does not inherit the tint of the op it is unioned onto — and to treat anything a modifier creates as uncolourable today. Three examples carry the consequence: `spiral_stair`'s swept handrail is left the default white because it cannot be tinted at all, `oil_lamp` dropped a collar `bevel` that was rendering as a white ring on an otherwise terracotta lamp, and `chess_rook` dropped both of its — the rim `bevel` for the same white ring, and the crenellation tool's `bevel`, whose rounded slot corners came back white while the flat walls beside them took the tool's tint.

- **Sweep output no longer takes model slot 0 — it inherits, and it takes `material=`.** Separate from colour, and it used to bite the same ops. `sweep` and `extrude_along_spline` output landed on the **model-wide** slot table's first entry, material ID 0, rather than on the enclosing part's slot. Measured on the same probe: part `a` tags `Alpha`, part `b` tags only `Beta` and then sweeps, and the swept rod came back on **`Alpha`** — one part rendering in two materials, slot count unchanged, no diagnostic. `spiral_stair` orders `part handrail` first for exactly that reason; written last, its oak rail shipped bound to the treads' rough-metal slot. Both ops now carry `material=`, and untagged they take the slot of the geometry they append onto; where that geometry carries more than one slot the op takes the majority and warns `PWMODEL_MODIFIER_MATERIAL_AMBIGUOUS`. The part-ordering workaround is no longer needed. `color=` is still not on either op — `set_vertex_color set_all=true` remains the only route to a vertex colour there.

## See also

- [`model.authoring`](model.authoring.md) — the `materials { }` block, slot allocation order, and the per-op tagging rules.
- [`material.mgir`](material.mgir.md) — MGIR syntax, `Append` versus `Extend`, and what the round trip does and does not preserve.
- [`material.authoring`](material.authoring.md) — the instance creators and the scalar/vector parameter setters.
- [`visual-review`](visual-review.md) — capture surfaces, and the one-scene rule for comparing two variants.
- [`visual-review.model-rig`](visual-review.model-rig.md) — the two review materials that sit in this same folder: `M_PwReview_Neutral`, a flat untextured grey to judge geometry under, and `M_PwReview_UVChecker`, which samples `TextureCoordinate` directly and is the only way a `uv` op becomes visible at all. Both ship as MGIR beside the two vertex-colour masters and `M_PwModelExample_WeatheredWood` — the procedural wood-grain master, whose header holds its own per-wood parameter table — in `Examples/mgir/`.
