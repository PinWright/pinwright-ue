# bpir.examples.construction-script

## Construction Script

Entry point for the Blueprint construction script, executed in-editor on placement/modification. See `call("bpir.entry-points")` §1 for the `entry construction` declaration.

```
entry construction ConstructionScript() {
    %mesh = call AddStaticMeshComponent(MeshAsset: "/Game/Meshes/SM_Cube.SM_Cube")
    call SetRelativeLocation(Target: %mesh, NewLocation: FVector(0.0, 0.0, 100.0))
    call SetWorldScale3D(Target: %mesh, NewScale: FVector(2.0, 2.0, 2.0))
}
```

> **Tests:** Parser: `ParseConstructionEntry` | Compiler: `ConstructionEntry` | Decompiler: _no dedicated test_ | Round-trip: _no dedicated test_

_See also: call("bpir.examples") for the full index._
