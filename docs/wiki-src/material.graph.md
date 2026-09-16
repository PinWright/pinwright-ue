# material.graph

Low-level material expression-graph editing: add / connect / inspect / remove nodes plus expression-class discovery. Reach for `call("material.graph")` only when `call("material.authoring")` doesn't expose a convenience for the expression class you need; for parameters, blend mode, common math nodes, compile, and instances stay on the higher-signal authoring API.

## A graph write is not a shader compile

Separate from the screen problem below and more fundamental: `nodeId`, `createdNodes` and `successCount` describe expressions placed and wired, never whether the resulting HLSL builds. Every verb here publishes a measured `shaderCompile` block — branch on `shaderCompile.status`, where `notCompiled` means "no compile has run", not "clean". `create_nodes` accepts `waitForShaderCompile: true` to block on the real verdict at the end of a batch; everywhere else, [`material.authoring.compile_material`](material.authoring.compile_material.md) is the verb that measures. See [`material.compile-state`](material.compile-state.md).

## Nothing in this namespace reaches the screen

Every verb mutates the graph and stops there, **by design**; it does not rebuild component material instances per node. A landscape can therefore compile and save while rendering the old look. With exposure pinned, wiring pure magenta to `BaseColor` moved a fixed frame **0.04%** (noise) versus **94.12%** after `compile_material`. Finish with [`material.authoring.compile_material`](material.authoring.compile_material.md), check `consumerRefresh.complete`, or build the graph through [`material.compile_mgir`](material.compile_mgir.md). See [Limitations and reliability notes](material.authoring.md#limitations-and-reliability-notes) for the full list.

## Bulk vs single edits

`add_expression` accepts a case-insensitive `properties` object of `UMaterialExpression` property names, using the same JSON conversion as asset property editing (`ParameterName`, `DefaultValue`, `Texture`, vector/color structs, and so on). Unknown or incompatible values are rejected before insertion.

`create_nodes` reduces round-trips but is best for simple insertion; individual calls are safer for unattended graphs. Verify with `get_node_details` / `material.authoring.get_material_info`.

## Cross-cluster overlap

The node model matches `call("blueprint.graph")` and `call("niagara.graph")`: ids address nodes, names address pins, and connections are bidirectional. Pin names follow the expression class.

## Discovery

`list_expression_types` and `search_expression_types` walk loaded `UMaterialExpression` subclasses and return `{className, shortName, category, description, caption, keywords, inputPins[], outputPins[], isParameter}`. `shortName` omits `MaterialExpression`; the other text comes from the engine. Pin arrays are positional: `outputPins[i]` is `sourceOutputIndex: i`, and its name is accepted as `sourcePin`. `list_expression_types` filters by `category`, `domainFilter`, `includeAbstract`, `parameterOnly`; search adds ranked keywords and `limit`. Use the returned `className` with `add_node`.

Unlike `blueprint.graph`, the material graph is flat: `UMaterialExpressionMultiply` is the multiply, with no wrapper or `target=Class::Member` tier. `MaterialFunctionCall` and `TextureSample` take their `UMaterialFunction*` / `UTexture*` references as properties. See `call("blueprint.graph")` for the two-tier contrast.

## Pin names are derived, not stored

The engine usually stores no display name on expression pins: `FExpressionOutput::OutputName` is `NAME_None` (including `VertexColor`'s five, `Constant3Vector`'s four, and `FontSample`'s five), and `GetInputName` is `NAME_None` for some inputs (`Custom`, `FunctionOutput`, `GetMaterialAttributes`, `Switch`'s trailing input). Verbs derive names as follows:

- a set `OutputName` / `GetInputName` wins verbatim;
- otherwise a masked output is named for its channel bits in RGBA order — `R`, `G`, `B`, `A`, `RGB`, `RGBA`, and `RG` for `Constant2Vector`;
- otherwise the engine's own graph-pin fallback, `Output` / `Output2` / `Output3` and `Input` / `Input2` / `Input3`.

Thus `VertexColor` reports `outputPins: ["RGB","R","G","B","A"]`, not five `"None"` values. Derived names are accepted by `sourcePin`, `inputName`, `pinName`, and MGIR pin references, so discovery output can be passed back directly.

## Inspection (`get_node_details`)

With `nodeId`, the response includes connectivity, parameter name, `group` / `sortPriority`, non-default reflected `UPROPERTY` values, and texture fields (`texturePath`, `samplerType`). Each input keeps its display `name`; when the reflected `FExpressionInput` property has a different spelling, the entry also includes optional `internalName` (for example, `True`/`A` and `False`/`B` on a `StaticSwitchParameter`). Each output has `index` (`sourceOutputIndex`) and `name` (`sourcePin`). Without `nodeId`, it returns a compact summary. The detail shape is shared with `material.authoring.get_material_node_details`.

## An ambiguous `nodeId` is refused, not resolved to one node

`nodeId` accepts a GUID, an object name, a full path, or a **parameter name** — and a parameter name is legally shared by several nodes (one texture parameter sampled on two or three projection planes in a triplanar material). `connect_nodes`, `break_connections`, `remove_node` and `get_node_details` refuse such a needle with `AMBIGUOUS_NODE` rather than acting on whichever node iterated first; the payload carries `requestedNodeId`, `candidateCount`, and `candidates` (`nodeId`, `name`, `class`, `parameterName` each). Re-issue once per node with a GUID or the full object name. Same contract in `material.authoring` — see [Limitations and reliability notes](material.authoring.md#limitations-and-reliability-notes).

## Connections

`connect_nodes` accepts case-insensitive `sourcePin` names from discovery/inspection (see [Pin names are derived, not stored](material.graph.md#pin-names-are-derived-not-stored)), or integer `sourceOutputIndex` (default `0`). Material-function-call outputs use their raw declared names. `sourceMask` (R/G/B/A/RGB/RGBA) is not wired; choose an already-masked output by name or index.

`connect_nodes` and `break_connections` recognize the full UE 5.6 main-material surface: `BaseColor`, `Metallic`, `Specular`, `Roughness`, `EmissiveColor`, `Opacity`, `OpacityMask`, `Normal`, `WorldPositionOffset`, `Refraction`, `Anisotropy`, `Tangent`, `ClearCoat`, `ClearCoatRoughness`, `Displacement`, `PixelDepthOffset`, `SurfaceThickness`, `FrontMaterial`, `ShadingModelFromMaterialExpression`, `MaterialAttributes`, and `CustomizedUVs[0..7]`. An invalid name returns the valid list.

Against the main node, `break_connections` clears resolved input(s); against another expression it clears all inputs or the named `FindExpressionInputByName` input. It returns `pinsBroken: [string]`. Previously this path could report success without changing state.

Read wiring from `connectedNodeId` (empty when unwired), never from a pin name. `"None"` meant “unnamed”, not “unconnected”; it is no longer emitted, so testing `"None"` or `''` for connectivity silently matches nothing.

## Creation and deletion

`add_node` and `add_expression` accept a resolved/loadable concrete `UMaterialExpression` subclass by short or full class name. The factory still resolves flagged classes so discovery and reflection can describe them, but `CLASS_Abstract`, `CLASS_Deprecated`, and `CLASS_NewerVersionExists` are rejected with `CLASS_NOT_INSTANTIABLE` before reflected-property validation, property writes, allocation, or insertion. `create_nodes` uses the same factory; a rejected node is reported in `failCount`.

`remove_node` uses UE's native `UMaterialEditingLibrary` cleanup path for both materials and material functions. It clears surviving inbound expression links (and, for materials, main-material inputs and parameter bookkeeping), removes the expression from its owning collection, and marks it with the internal `EInternalObjectFlags::Garbage` flag before returning `removed: true`. That field means graph deletion only; it is not a shader-compilation result. The native path does not promise clearing the deleted node's own inputs or execution begin/end links.

### material.graph.connect_nodes

`connect_nodes` changes only the asset; a landscape can keep drawing the previous shader while it reports success. Finish with [`material.authoring.compile_material`](material.authoring.compile_material.md) and branch on `consumerRefresh.complete`; see [Nothing in this namespace reaches the screen](material.graph.md#nothing-in-this-namespace-reaches-the-screen).
