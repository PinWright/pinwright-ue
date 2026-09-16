# material.compile-state

Whether a material's SHADER compiles, reported by every material verb under one `shaderCompile` field. A material that can never compile writes a valid `.uasset`, saves, reads back correctly and renders the engine Default Material, so no other field in any response separates it from one that works.

## A graph write is not a shader compile

`material.compile_mgir` returns `blocksCompiled` and `expressionsCreated`. `material.graph.*` returns `nodeId` and `createdNodes`. `material.authoring.connect_nodes` returns `"Nodes connected."`. All of those describe **expressions placed and wired**. None of them is the platform shader compiler running on the HLSL the graph translates to, and the two outcomes are independent: a Custom node holding malformed HLSL, or a texture sampled with the wrong `SamplerType`, produces byte-identical success payloads.

The failure mode this closes is not subtle once it is named, and it is expensive: two UI materials shipped through a full authoring, saving, disk-verification and PIE capture cycle before anyone asked the engine the question. Both were broken. The engine knew from the first write.

## The `shaderCompile` block

Every material write verb and the material read verbs (`material.authoring.get_material_info`, `get_material_instance_info`) publish it. `material.authoring.compile_material` publishes it alongside its own long-standing `compileStatus` / `compileErrors` fields, built from the same measurement so the two cannot disagree.

| field | meaning |
| --- | --- |
| `status` | the single field to branch on — see the five spellings below |
| `succeeded` | `true` only for `completed`. An empty error list from a compile that never ran is not a pass |
| `failed` | the PERMANENT case. Kept as its own boolean because the recurring mis-read is treating it as a transient "still warming up" |
| `errors[]` | failed-permutation HLSL errors verbatim, naming file, line, shader type and permutation |
| `errorCount` | length of `errors[]` |
| `rendersDefaultMaterial` | the renderer substitutes the engine Default Material for **the measured resource**: either the parent chain resolves to it, or that resource has no complete shader map. Read it with `measuredSubject` and `declaredUsages`, never alone — see below |
| `rendersDefaultMaterialScope` | **always present.** One sentence stating what the boolean was measured on and what it structurally cannot see. The flag is never published without it |
| `measuredSubject` | which resource the whole block describes: `baseMaterial`, `instanceStaticPermutation`, or `parentInherited` (an instance with no static permutation, whose fields are its parent's). Omitted on a multi-material fold — `materials[]` carries one per row instead |
| `measuredMaterialPath` | the asset that owns that resource — the parent's path for `parentInherited`. Omitted when there is none (a parentless instance) rather than emitted empty |
| `declaredUsages[]` | the `EMaterialUsage` names this material declares. A consumer whose usage is absent draws the engine Default Material and `rendersDefaultMaterial` cannot see it. Omitted on a multi-material fold; see `materials[]` |
| `waited` / `waitedMs` | whether this call blocked on the compiler, and for how long |
| `hint` | what to do next. Absent for `completed` |
| `materials[]` | per-material `{assetPath, status, measuredSubject, declaredUsages[]}` rows, on verbs that finalise several materials in one call (`material.compile_mgir` over a multi-entry document). The top-level flag is those materials OR-ed together, so read the rows to find which one is substituted |

## The five statuses

- **`completed`** — a compile ran and the engine installed a complete shader map. The only clean pass.
- **`failed`** — a compile ran and at least one permutation failed. Permanent: retrying produces the identical broken result forever, and a capture of this material is evidence of nothing.
- **`outstanding`** — a compile is in flight. The verdict is not knowable yet; re-read, or block on it.
- **`timedOut`** — a bounded wait expired with the compile still running. The compile is **not** abandoned.
- **`notCompiled`** — no compile has run. **This is the common one on a write verb, and it is not a pass.**

`notCompiled` deserves the emphasis. `PostEditChange` caches the shader map with `EMaterialShaderPrecompileMode::None`: it translates the material to HLSL and does **not** submit the permutation compile jobs, which are deferred until the material is first *drawn*. In a headless editor nothing is ever drawn, so a material that can never compile probes as `notCompiled` indefinitely. An empty `errors[]` beside `notCompiled` means "nobody asked", not "nothing was wrong".

## A compile verdict is not a render verdict: usage flags

`rendersDefaultMaterial:false` is a statement about **one shader map**, and a shader map is complete without the permutations of a usage the material does not declare — `GPUSkinVertexFactory::ShouldCompilePermutation` returns false unless `bIsUsedWithSkeletalMesh`, so a material missing that flag has a complete map that contains no skinned permutation at all. The renderer then substitutes the engine Default Material on the skeletal mesh while every compile field reads healthy. The same holds for `bUsedWithInstancedStaticMeshes` on an ISM/HISM layer and for `bUsedWithNiagaraSprites` / `bUsedWithNiagaraMeshParticles` on particle renderers.

The editor hides it by repairing it: `UMaterial::SetMaterialUsage` sets the missing flag at draw time when `bAutomaticallySetUsageInEditor`, recompiles, and dirties the package. Unsaved, that repair is re-paid every launch and a packaged build ships the substitution — so "the material looks right in the editor" is the signature of this defect, not evidence against it.

What to read:

- `shaderCompile.declaredUsages[]` — on every material response, the short list of what the material is allowed on.
- `material.authoring.get_material_info` → `usage` — the whole set: `declared[]`, `flags[]` (one row per usage with `declared` and the `property` behind it), and `autoSetInEditor`.
- `material.authoring.get_material_instance_info` → `usage` — the same, plus `parentDeclared[]` and a per-row `overridden`, because an instance inherits the parent's set and may override single bits.

There is deliberately **no usage-setter verb**. Write the flag with `property.set` on the *base* material using the `property` name from the `flags[]` row, then call `compile_material`; `property.set` writes the member directly and does not run `SetMaterialUsage`'s paired recompile.

## Getting a real verdict

Two ways, both cheap next to the alternative:

- `call("material.authoring.compile_material", {assetPath})` — always blocks. Forces every permutation through the platform compiler, drains the result under a 90 s ceiling, and reports the outcome. 3–10 seconds, no world, no lock, no PIE. It accepts a `UMaterialInstanceConstant` as well as a `UMaterial`; see the instance bullet below for which resource it compiles.
- `waitForShaderCompile: true` on the batch write verbs that accept it — `material.compile_mgir` and `material.graph.create_nodes`. Same measurement, folded into the write that prompted it, so an authoring batch ends with an answer instead of a follow-up call.

Every other write verb publishes the free non-blocking probe and, when the status is not `completed`, a `hint` naming the verb above. That is deliberate: probing costs a pointer walk and can be done unconditionally, while forcing a synchronous compile on every `add_node` would make graph authoring unusable.

## Known capture fallback is fail-closed

`asset.generate_thumbnail` and `render.capture_asset_preview` probe every directly inspectable material after the final render or retry and publish `materialReadiness`. They pump already-submitted compile work for one bounded wait, then read the actual interface resource, including a material instance's own static permutation. The top level reports `measured`, `compiled`, `compiling`, `failed`, `usingDefaultMaterial`, `fallbackOccurred`, `fallbackPossible`, `subjectMaterialsRendered`, and `subjects[]`. `reason` names a known fallback; `possibleReason:"shaderMapIncomplete"` names uncertainty after the wait. Thumbnail mesh rows use `meshUsagePolicy:"thumbnailLod0Sections"`: the engine's Static Mesh and Skeletal Mesh thumbnail renderers disable the LOD show flag, which selects LOD0. Asset-preview rows use `meshUsagePolicy:"capturedComponentSections"`; `renderedLodIndex` and `scope` say whether LOD came from the forced/predicted component state or the Static Mesh LOD0 fallback, and editor preview/hidden-section filters keep inactive sections out of fallback policy. Null included slots are reported as unassigned Default Material substitutions instead of disappearing from the report.

A known Default Material substitution is not visual evidence. A failed shader map with compile errors or an unassigned rendered slot therefore returns `success:false` with error code `MATERIAL_FALLBACK` by default, retaining `materialReadiness` in the error details. Pass `allowFallback:true` only when that known-fallback image is wanted; the normal image result is then retained, while `fallbackOccurred:true` and a reason-aware top-level `warnings[]` entry remain. A final `notCompiled`, `outstanding`, or `timedOut` status is different: it proves no final shader verdict, not a permanent failure or what was in earlier pixels. Those captures remain successful without `allowFallback`, report `fallbackPossible:true`, keep the exact status in `subjects[]`, and add a warning. Selecting the engine Default Material intentionally reports `usingDefaultMaterial:true` but both fallback flags false. Preview debug modes that replace subject materials report `subjectMaterialsRendered:false` and do not attribute those debug pixels to a broken subject material.

## What it does not tell you

- **Nothing about whether the edit reached the screen.** A material can compile perfectly while a landscape keeps drawing the previous shader, because its per-component material instances cache derived data. That is `consumerRefresh`, a separate measured block — see [Limitations and reliability notes](material.authoring.md#limitations-and-reliability-notes).
- **Nothing about material functions.** A `UMaterialFunction` has no shader map of its own; verbs that write one publish no `shaderCompile` block, and its errors surface on the materials that call it.
- **Instance state is resource-specific, and the block says which.** An instance without a static permutation (`bHasStaticPermutationResource` false) inherits its parent's resource and verdict: `measuredSubject:"parentInherited"`, `measuredMaterialPath` is the parent, and there is no instance-level compile to obtain — set a static parameter first if you need one. An instance with static-switch or base-property overrides owns a permutation resource, so its compile state and errors are measured separately (`measuredSubject:"instanceStaticPermutation"`) even though the authored HLSL still comes from its material chain. `compile_material` follows the same split: it compiles the instance's own permutation when there is one, and otherwise compiles and reports the parent's resource — without dirtying, refreshing consumers of, or saving that parent, and with a `warnings[]` entry naming it. `consumerRefresh` is emitted for a `UMaterial` only; an instance is nobody's master.

- **Nothing about the usage flags.** See the section above; that gap is what `declaredUsages[]` and the `usage` block on the read verbs close.

## See also

- [`material.authoring`](material.authoring.md) — `compile_material` is the verb that blocks and measures.
- [`material.graph`](material.graph.md) — why nothing in that namespace reaches the screen on its own.
- [`material.mgir`](material.mgir.md) — MGIR compiles the graph; this page is the other half.
- [`visual-review`](visual-review.md) — compile before you capture.
