# PinWright

An in-editor MCP server for Unreal Engine: RPC verbs that read and author project content, plus the
IR and file formats those verbs compile from.

## Language

### PinWright Model

**Model**:
One `.pwmodel` source file and the single `UStaticMesh` it compiles to. The file is the durable
source; the asset is derived.
_Avoid_: mesh document, model file, recipe

**Part**:
A named sub-region of a Model's output mesh, carrying its own ops, transform and material
assignment. Parts merge; a Part is never a separate asset.
_Avoid_: sub-mesh, section, component, group

**Op**:
One geometry operation inside a Part — a line of the source and one entry in the parser's op table.
The Op vocabulary is the extracted `geometry.*` verb set minus the namespace.
_Avoid_: command, instruction, node, feature

**Hull**:
A convex collision element whose input is hulled on the way in, so concavity in that input is
discarded. Named for what happens to the input rather than for `FKConvexElem`, which is named for
what it stores.
_Avoid_: convex, convex hull element

**Slot**:
A material slot on the output asset, identified by its **name**. Two Parts tagging the same name
share one Slot.
_Avoid_: material index, material ID, material channel

**Model-level**:
Declared once per Model, outside any Part: `materials`, `collision`, `lightmap`. These map to
per-asset state and have no per-Part meaning.

**Part-level**:
Declared inside a Part: every geometry Op, including `uv`.

**Part-local space**:
The coordinate space a Part's Ops work in. Op `at`/`rotate`/`scale` and the `transform` Op are
expressed here.

**Mesh space**:
The coordinate space of the merged output mesh. A Part's own `at`/`rotate`/`scale` maps Part-local to
Mesh space, and collision element `at`/`rotate` is expressed here.
_Avoid_: model space, asset space, local space

**Op transform**:
`at`/`rotate`/`scale` on a single Op, composed into that primitive's append call.

**Part transform**:
`at`/`rotate`/`scale` in a Part header, applied once after the Part's Ops have run.

**Collision transform**:
`at`/`rotate` on a collision element, applied in Mesh space at collision build.

**Provenance stamp**:
The record a generated asset carries naming the Model source it was compiled from.
_Avoid_: ownership marker, generated flag
