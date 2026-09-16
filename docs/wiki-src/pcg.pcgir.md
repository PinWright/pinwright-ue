# pcg.pcgir

PCGIR is the text IR for `UPCGGraph` assets, like MGIR, BPIR, and AGIR. It is **decompile-only** via `pcg.decompile`; compile is deferred. Use `call("pcg")` for imperative node authoring, while `pcgir.txt` from `asset.dump` shares the same decompiler path and keeps dump/live output in lockstep.

## Availability

Like the rest of `pcg.*`, the PCGIR decompile surface requires the **PCG** engine plugin. The integration auto-loads when that plugin is enabled in the target project; when it is disabled, `pcg.decompile` is unregistered and calling it returns `PLUGIN_DISABLED` (enable the PCG plugin and restart the editor).

## Cross-cluster overlap

`asset.dump` writes the same decompile output to `pcgir.txt`; `IrSidecarRegistry` registers PCGIR as a uniform text-IR sidecar. Either surface is mandatory — never ship one without the other.

`pcg.decompile` accepts:

- `assetPath` — the PCG graph asset path (required).
- `includeReferencedSubgraphs` — reserved for future use. Subgraphs are **always flattened** in the current decompile direction; if you pass `true`, the response surfaces a warning advising the flag is deferred.

Example output:

```text
entry pcg `/Game/PCG/G_FoliageScatter` {
    node N_Input = `/Script/PCG.PCGGraphInputOutputSettings` @(-400, 0)
    node N_Output = `/Script/PCG.PCGGraphInputOutputSettings` @(800, 0)
    node N1 = `/Script/PCG.PCGSurfaceSamplerSettings` @(0, 0) {
        PointsPerSquaredMeter = 2.5
    }
    node N2 = `/Script/PCG.PCGSubgraphSettings` @(400, 0) {
        subgraph = "/Game/PCG/SG_BoulderScatter"
    }
    connect N_Input.In -> N1.In
    connect N1.Out -> N2.In
    connect N2.Out -> N_Output.Out
}
```

PCGIR entry targets are name tokens (backtick-delimited when they contain `/` or `.`, as PCG asset paths always do). Per-node sequential names `N1`, `N2`, ... are assigned in `UPCGGraph::GetNodes()` order; the auto-created graph terminals get reserved names `N_Input` and `N_Output`. Node identifiers are stable across a single decompile call but are **not** asset-stable — re-decompiling after an edit can renumber.

## Flatten-on-decompile policy for subgraphs

Subgraph nodes (`UPCGSubgraphSettings` and any other `UPCGBaseSubgraphSettings` subclass) emit the referenced graph's path as a `subgraph = "..."` property on the parent node. The child graph is **not** inlined or recursed into. This mirrors the MGIR composite-flatten policy: callers that want the child's content decompile it separately by its asset path. If the subgraph reference is null, the parent node still emits flat with a warning in the response payload.

## PCG engine API gotchas

Two non-obvious UE 5.6 PCG engine specifics the decompiler must handle:

1. **Graph terminals live outside `GetNodes()`.** `UPCGGraph::GetInputNode()` and `GetOutputNode()` (`Engine/Plugins/PCG/Source/PCG/Public/PCGGraph.h:360,364`) return terminal nodes that are auto-created via `CreateDefaultSubobject` in the graph constructor (`PCGGraph.cpp:393,412`) and are **NOT** included in `GetNodes()`. They always exist on a fresh `NewObject<UPCGGraph>`, so the decompile walk must explicitly merge them with stable reserved names (`N_Input`, `N_Output` — see above) and dedup if a future engine bump moves them into `Nodes`. Tests that want to exercise the `# no nodes` empty-graph branch must reflectively null both `UPROPERTY`s, because `RemoveNode` does not touch the terminal slots (see `TestPCGIRDecompile_EmptyGraph.cpp`).

2. **`UPCGEdge` pin polarity inverts node-pin intuition.** `UPCGEdge::InputPin` is the edge's **upstream-side** pin (the source — the output pin feeding the edge) and `UPCGEdge::OutputPin` is the **downstream-side** pin (the dest — the input pin receiving the edge). Naming is from the edge's POV, not the node's (`PCGEdge.h:38,42`). Emitting `connect <Source>.<SrcLabel> -> <Dest>.<DstLabel>` therefore reads `Edge->InputPin` for the source and `Edge->OutputPin` for the destination. The walk must traverse output pins only (`Pin->IsOutputPin()` + `Pin->Edges`, `PCGPin.h:257,291`) to avoid double-emission, since each edge is referenced by both endpoint pins.

See `Source/PinWrightPCG/Private/PCGIR/PCGIRDecompiler.cpp` for the terminal-merge logic and the output-pin edge traversal.

## Round-trip is logical, not visual

PCGIR follows the same logical-equivalence contract as MGIR / BPIR / AGIR: the IR is intended to preserve graph **structure** (nodes, edges, settings property values, positions) — not visual organization. Comment boxes, node colors, node order in the editor list, and any in-editor UX state are out of scope and not emitted. There is no round-trip yet (compile direction is deferred), but this caveat applies to the emitter's position and layout handling pre-emptively so future compile work has a clean contract.

## See also

- [`pcg`](pcg.md) for imperative PCG graph/node/pin authoring RPCs and implicit endpoint handling.
- [`asset`](asset.md) for asset-dump sidecar registration and diff-baseline behavior.
- [`material.mgir`](material.mgir.md) for the closest prior art on grammar shape, decompile-only flattening, and round-trip semantics.
