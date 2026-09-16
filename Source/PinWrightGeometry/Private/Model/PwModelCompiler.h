// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwModelCompiler.h - Source text -> one UStaticMesh or USkeletalMesh.
//
// The surface below is deliberately free of MCP and JSON: no FJsonObject, no
// FHandlerContext. ModelCompileHandler.cpp marshals this into a response, and keeping the
// marshalling on that side is what lets a later UPwModelLibrary Python wrapper (following
// UPinWrightPackageLibrary's precedent) be a thin shim rather than a refactor.
//
// ONE SOURCE FILE COMPILES TO EXACTLY ONE ASSET (docs/adr/0001-one-file-one-asset.md).
// That shows up here as a single AssetPath in the result and one asset-creation seam in the
// implementation. Parts are sub-regions of that one mesh; FPwModelPartInfo reports their
// counts for diagnostics and nothing else.
//
// Compile takes an FStringView rather than a file path so the whole pipeline is testable
// without touching the filesystem. Requiring a real file is the model.compile RPC verb's
// rule, not the compiler's - see the plan's Decisions section for why an asset compiled
// from an inline payload has no recoverable source.
#pragma once

#include "CoreMinimal.h"
#include "Containers/StringView.h"

#include "Model/PwModelDiagnostic.h"
#include "Handlers/Geometry/MeshAuditUtils.h"
#include "Utils/AssetSaveState.h"

struct FPwModelCompileOptions
{
    // A single /Game/... ASSET path, not a directory: one file yields one asset.
    FString OutputAssetPath;

    // Recorded in the asset's provenance stamp so a later compile of the same source can
    // overwrite without a flag. Empty for inline text, which is why model.compile refuses
    // inline text - every inline compile would stamp an empty path and therefore match
    // every other one.
    FString SourcePath;

    // Only consulted when the target path holds an asset stamped with a DIFFERENT
    // SourcePath, or no stamp at all. Recompiling one's own output is iteration and needs
    // no flag.
    bool bOverwrite = false;

    bool bSave = true;

    // Runs every stage except asset creation. Counts are still reported, so this is the
    // surface for iterating on a document before committing an asset.
    bool bValidateOnly = false;
};

// Informational only. A part is a named sub-region of the single output mesh; it is not
// an asset and has no path.
struct FPwModelPartInfo
{
    FString PartName;
    bool bAllowFloating = false;

    // Both are UDynamicMesh counts, NOT the asset's - see FPwModelCompileResult. There is no
    // per-part asset count and there cannot be one: the parts are merged into a single mesh
    // before the bake, so the asset has no record of which triangle came from which part.
    int32 MeshTriangleCount = 0;
    int32 MeshVertexCount = 0;

    // ---- winding, per part -------------------------------------------------------------
    //
    // PER PART and not only model-wide, because the model-wide number AVERAGES an inverted
    // part away: a correct 20-cube (+8000) beside an inverted 10-cube (-1000) sums to a
    // healthy-looking +7000, and one inverted part inside an otherwise correct model is the
    // realistic case. Measured after the part transform is baked, so a negative-determinant
    // `scale=` - which TransformMesh compensates for by reversing the winding - is reflected
    // here rather than reported against pre-transform geometry.
    //
    // MeshSignedVolume is only meaningful when bMeshIsClosed; on an open part it is the
    // integral of an unclosed surface. The gate is `isClosed && signedVolume > 0`.
    bool bMeshIsClosed = false;
    bool bMeshOrientationConsistent = true;
    double MeshSignedVolume = 0.0;

    // This part's own axis-aligned extent in MESH space, measured after the part transform is
    // baked. Per part as well as model-wide because the model-wide box says only that something
    // reaches an extreme; this says WHICH part does, which is the question an author fitting a
    // model to a fixed box actually has. Invalid (IsValid == 0) when the part produced no
    // geometry - 0 is a real coordinate and "not measured" is not.
    FBox MeshBounds = FBox(ForceInit);

};

// One material slot of the created asset, at its index in the slot table.
//
// BoundAssetPath is what `materials { }` bound to this slot, verbatim from the source. Empty
// means the geometry tagged a slot no binding names - a real state that ships an empty slot and
// already carries its own warning; it is not "unknown".
struct FPwModelSlotReport
{
    FString Name;
    FString BoundAssetPath;
};

struct FPwModelCompileResult
{
    bool bSuccess = false;

    // From the source's `pwmodel <N>` header. -1 when the header was missing or unparsable.
    int32 Version = 0;

    // The one asset. Empty when nothing was created (validate-only, or any failure).
    FString AssetPath;

    // Whether bSkeletal below is an ANSWER or an absence.
    //
    // The class a .pwmodel produces is decided by one declaration - `use skeleton from` - so it
    // is known as soon as the parse recovers that line, and NOT known when the parse failed
    // before reaching it. A bare `bSkeletal = false` cannot tell those apart, and reporting the
    // second as "UStaticMesh, skeletal: false" is a confident wrong answer: a source whose
    // skeletal intent is unambiguous was told it was building a static mesh because a comma was
    // missing forty lines earlier. The RPC omits the class fields entirely while this is false.
    bool bAssetClassKnown = false;

    // A skeleton use selects the skeletal creation seam. These fields remain empty/default
    // for an ordinary static model and are measured from the resolved/created skeleton rather
    // than echoed from source text.
    bool bSkeletal = false;
    FString SkeletonPath;
    FString SkeletonPackageName;
    TArray<FString> ClearedFeatures;
    bool bSkeletonSavedToDisk = false;
    EAssetSaveState SkeletonSaveState = EAssetSaveState::NotRequested;

    TArray<FPwModelPartInfo> Parts;

    // Counts of the MERGED UDynamicMesh rather than a sum that ignores what the booleans
    // removed. Measured once, before creation, so a bValidateOnly run reports the same numbers
    // as one that writes the asset.
    //
    // BOTH are named for the mesh because NEITHER is the asset's, which is the correction this
    // pair carries. The old `TriangleCount` was documented here as "IS the asset's triangle
    // count" and is not: the StaticMesh build drops every triangle with two corners closer than
    // THRESH_POINTS_ARE_SAME (FMeshBuildSettings::bRemoveDegenerates, default true;
    // StaticMeshBuilder.cpp:1666-1674), so the asset's count is <= this one. The build moves the
    // vertex count the other way, splitting a position into one render vertex per distinct
    // normal/tangent/UV/color, so the asset's count is >= this one.
    //
    // Both directions, and the measured numbers, live in ONE place: docs/pwmodel-format.md,
    // section "Four counts: two for the mesh, two for the asset". The worked per-example pairs
    // that used to be inlined here were one of five byte-identical hand-copies; inter-copy
    // agreement therefore proved nothing while all five drifted away from the example corpus
    // together. Cite the doc; do not re-inline a number.
    //
    // The failure this naming exists to stop: an author adds split_normals - whose whole purpose
    // is to create those seams - sees a BYTE-IDENTICAL compile response, and concludes the op did
    // nothing. Neither field is a size gate. Use AssetTriangleCount / AssetVertexCount below, or
    // static_mesh.describe.
    int32 MeshTriangleCount = 0;
    int32 MeshVertexCount = 0;

    // The most recent boolean's repair window. The compiler welds coincident boundary edges and
    // deletes degenerate triangles after a successful boolean; these fields make that repair
    // observable instead of leaving the response with only the final mesh count. The before and
    // after counts include both repair passes, while SliversRemoved is the triangle delta from
    // the degenerate-delete pass. -1 means no boolean reached cleanup (for example, a generator-
    // only document or a parse failure).
    int32 TrianglesBefore = -1;
    int32 TrianglesAfter = -1;
    int32 SliversRemoved = 0;

    // The counts the ASSET actually has, read back off LOD0's built render data inside
    // CreateStaticMesh. -1 on any path that wrote no asset - a validate-only run, or a failed
    // compile - because 0 is a real answer for an empty LOD and "not measured" is not.
    //
    // There is no per-part equivalent: parts merge into one mesh before the bake, so the asset
    // cannot attribute a triangle to a part.
    int32 AssetTriangleCount = -1;
    int32 AssetVertexCount = -1;

    // Skin coverage is the compiler's exhaustive pre-write gate. The sentinel means the
    // document never reached the skin stages; zero is a measured mesh result, not "unknown".
    bool bHasSkinWeights = false;
    bool bFullyWeighted = false;
    int32 SkinVertexCount = -1;
    int32 SkinWeightedVertexCount = -1;
    int32 SkinUnweightedVertexCount = -1;

    // The merged mesh's health, measured once at the end of stage 4 by
    // GeometryUtils::MeasureMeshHealth - the same walk geometry.check_health reports, which the
    // compiler previously never ran. It is why every example model could compile clean while
    // carrying degenerates and, at one point, 272 boundary edges: an open shell shipped as a
    // successful asset. Per-example degenerate counts are not repeated here; the corpus reading
    // is in docs/wiki-src/model.examples.md and moves as examples are reworked.
    //
    // Reported as numbers rather than folded into bSuccess, and the matching diagnostics are
    // warnings, because none of these conditions is unconditionally wrong: an open mesh is
    // correct for a card or a surface model, and degenerates usually die in the static-mesh
    // build's own welding. What was wrong was model.compile answering with no way to SEE them.
    // A caller that wants a solid gates on MeshBoundaryEdges == 0 itself.
    //
    // -1 on any path that never reached stage 4, for the same reason as the asset counts above:
    // 0 boundary edges is a real answer and "not measured" is not.
    int32 MeshBoundaryEdges = -1;
    int32 MeshDegenerateTriangles = -1;
    int32 MeshNonManifoldVertices = -1;
    int32 MeshComponentCount = -1;

    // Vertices of the merged mesh that no triangle names. It is what reconciles the two counts
    // either side of it: MeshVertexCount is the vertex buffer, while the reported bounds is
    // reduced over TRIANGLES (see GetTriangleReferencedBounds), so a vertex a cut stranded is in
    // one and excluded from the other - correctly, and until this field with nothing saying so.
    // Non-zero is also the signal in its own right that an op removed geometry and left the
    // leftovers behind, which is the class of fault the bounds reduction was written for.
    //
    // Not folded into any verdict: an orphan reaches no asset, because the static-mesh build is
    // driven by triangles. -1 on any path that never reached stage 4, like the four above.
    int32 MeshUnreferencedVertices = -1;

    // WINDING, which none of the four above can see. A closed mesh wound uniformly inside out
    // matches a correct one on every one of them - closed, 0 boundary edges, 0 bowties, one
    // component, identical counts - and renders identically, because backface culling shows the
    // camera whichever wall faces it. That is how an inverted shell shipped in an example and
    // survived two investigations; an A/B luminance comparison of the two moved by 0.000004.
    //
    // It matters because winding, not shading, is what the OFFLINE consumers read: the mesh
    // distance field decides inside from outside by counting backface hits
    // (MeshDistanceFieldUtilities.cpp:261-281), so an inverted shell inverts its field and
    // Lumen / DFAO light the part as though the camera were inside it.
    //
    // MeshSignedVolume is only meaningful when MeshBoundaryEdges == 0. -1 / 0 / true on any
    // path that never reached stage 4, matching the four above.
    int32 MeshInconsistentEdges = -1;
    double MeshSignedVolume = 0.0;

    // EMBEDDING, which none of the six above can see - and it is the one that defeats the
    // documented gate `isClosed && signedVolume > 0` outright rather than partially.
    //
    // A membrane spanning a solid's interior (a revolve capping a closed section to the axis at
    // both ends) is two oppositely wound fans, so their contributions to MeshSignedVolume cancel
    // EXACTLY: the number is the correct figure for the ring that was wanted, on a ring with a
    // lid. A sweep whose walls have been pushed through each other moves MeshSignedVolume
    // SMOOTHLY instead - 62% and 25% of the analytic volume, with boundary edges, orientation,
    // degenerates and bowties all still clean - and changes sign only long after it stopped
    // being a solid. Neither has a threshold on any field above.
    //
    // Measured per edge-connected COMPONENT and summed, never across components: appended parts
    // and appended sibling ops interpenetrate on purpose in this format, and pooling those pairs
    // in here would put a non-zero count on a large fraction of correct models. See
    // GeometryUtils::MeasureMeshSelfIntersection.
    //
    // -1 on any path that never reached stage 4 AND on a mesh the measurement declined (over
    // GeometryUtils::SelfIntersectionMaxTriangles), for the same reason as the counts above:
    // 0 crossings is a real answer and "not measured" is not, and the two must not share a value.
    // bMeshSelfIntersectionTruncated says the count is a floor rather than a total.
    int32 MeshSelfIntersections = -1;
    int32 MeshSelfIntersectingComponents = -1;
    bool bMeshSelfIntersectionTruncated = false;

    // The merged mesh's axis-aligned extent, in mesh space, measured at the same point as the
    // health walk above and therefore reported by model.validate as well as model.compile.
    //
    // It is here because SIZE is an acceptance criterion and nothing else in this result carries
    // it. A model that replaces an existing asset in place, fills a socket, or has to match a
    // prefab slot is judged on its box, and without this field the only way to read one is to
    // WRITE an asset and call static_mesh.describe - which makes model.validate, the surface
    // documented as the one that "creates nothing", unusable for the most common numeric check
    // an author runs. Fitting a bounding box is also iterative rather than arithmetic: a
    // noise_deform displaces along the vertex normal and a harmonic_deform scales the
    // perpendicular radius, so the extent a primitive reaches is not the `radius=` the source
    // names and cannot be derived from the text.
    //
    // static_mesh.describe publishes the same box for the built asset as origin + extent; this
    // is min/max/size/center because that is the form an acceptance test is written in, and
    // converting between them by hand is where the mistake goes.
    //
    // Invalid (IsValid == 0) on any path that never reached stage 4, matching the -1 sentinels
    // above.
    FBox MeshBounds = FBox(ForceInit);

    // Spatial isolation report from the shared component walk. Warnings remain warnings, and
    // intentionally floating parts are marked suppressed in the rows rather than disappearing.
    MeshAudit::FFloatingReport FloatingGeometry;

    // The slot table, in the model-wide FIRST-USE order the created asset's section list takes.
    // `MaterialSlots` is this array's length and stays for callers that only want the count.
    //
    // WHY the names and not just the count. Slot ORDER is the contract an in-place rebuild has
    // to hold: a rebuilt mesh keeps its referencers, and every one of them addresses sections by
    // INDEX - a placed actor's OverrideMaterials, a section's own material assignment, an LOD
    // section setting. Reorder the parts and every index silently means a different section,
    // with a green compile and no field in the response that moved. The count cannot see it:
    // six slots before and six slots after is what a renumbering looks like.
    //
    // It matters most on model.validate, which creates nothing and is therefore the only place
    // an author can check the order BEFORE overwriting a shipped asset. Reading it off the
    // written asset instead costs a compile plus an asset.dump - and by then the overwrite has
    // already happened.
    //
    // Populated after the merge, so it includes any slot MergeParts had to pad in for an
    // append_buffers material_id past the end of the table. Empty on a run that never merged.
    TArray<FPwModelSlotReport> MaterialSlotList;

    // Slots the CREATOR left on the default surface material: bound to nothing, or bound to a
    // path that would not load. Both asset creators have always computed this
    // (FStaticMeshCreateResult / FSkeletalMeshCreateResult, both named UnboundSlots) and the
    // model pipeline dropped it on the floor, so a model shipped a grey section with the
    // response reporting `success: true` and a slot count that looked right.
    //
    // Distinct from MaterialSlotList's empty BoundAssetPath, which reports what the SOURCE says.
    // This reports what the ASSET got, and the two differ exactly where it matters: a slot bound
    // to a real path whose asset fails to load is bound in the source and unbound in the mesh.
    //
    // Populated only on a run that created an asset. model.validate runs no creator, and an
    // empty array there would read as "every slot is bound" - the same false confidence this
    // field exists to remove - so the RPC omits it rather than publishing one. The validate path
    // covers the same ground with diagnostics instead (PWMODEL_UNBOUND_MATERIAL from the parser,
    // and FCompiler::WarnOnUnloadableMaterialBindings for a path that will not load).
    TArray<FString> UnboundSlots;

    int32 MaterialSlots = 0;

    // Elements the `collision` block produced. Written verbatim into the pre-build UBodySetup,
    // so this is also the asset's count; 0 when the document declares no collision.
    int32 CollisionElements = 0;

    bool bSavedToDisk = false;

    // WHY, when bSavedToDisk is false. A compile that reports saved:false is otherwise
    // unactionable: a deferred edit needs an editor.save_all, a failed write needs the cause
    // cleared first, and an unmounted package will never persist at all. NotRequested on a
    // validate-only run and whenever Options.bSave is false.
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;

    // Errors and warnings from every stage, in the order they were produced. Warnings can
    // be present on a successful compile; PwDiagnosticsHaveError is the failure test.
    TArray<FPwDiagnostic> Diagnostics;
};

class FPwModelCompiler
{
public:
    // Parses, builds, validates and (unless bValidateOnly) creates exactly one asset.
    //
    // Never partially creates: every failure short-circuits before CreateStaticMesh, so a
    // document whose second part fails leaves nothing on disk. Must run on the game thread
    // - it constructs UObjects and, on the create path, touches the asset registry.
    static FPwModelCompileResult Compile(FStringView Source, const FPwModelCompileOptions& Options);
};
