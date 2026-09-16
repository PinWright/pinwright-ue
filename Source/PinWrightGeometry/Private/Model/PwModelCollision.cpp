// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Model/PwModelCollision.h"

#include "Model/PwModelDiagnostic.h"
#include "Model/PwModelParser.h"
#include "PwSource/PwValueRead.h"

#include "Compat/EngineVersionCompat.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Selections/MeshConnectedComponents.h"
#include "UDynamicMesh.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include "GeometryScript/CollisionFunctions.h"
#include "GeometryScript/MeshSimplifyFunctions.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper
// with a common name would collide with a sibling TU once Unity merges them.

// ---- Canonical element order ------------------------------------------------------------------

// `auto` collision generation returns its elements in WORKER-COMPLETION order, so the same source
// compiles to the same hulls in a different array order each run. That is not a theory: the engine
// splits the merged mesh into connected components (CollisionFunctions.cpp, ComputeCollisionFromMesh)
// and both FMeshSimpleShapeApproximation::Generate_ConvexHulls and ::Generate_ConvexHullDecompositions
// run ParallelFor over those components appending under an FCriticalSection
// (MeshSimpleShapeApproximation.cpp:306,335) - whoever finishes first lands first. Measured on UE 5.8:
// one eight-part document compiled four times produced four DISTINCT hull orderings with an identical
// hull SET every time.
//
// Physics does not care - FKAggregateGeom is a set to the solver - but the order is SERIALIZED into
// the .uasset, so without this the same text produces byte-different assets, which is precisely the
// claim .pwmodel exists to make. It also feeds FSimpleShapeSet3d::RemoveContainedGeometry, whose
// volume sort (SimpleShapeSet3.cpp:304) is TArray::Sort - introsort, NOT stable - so equal-volume
// hulls could otherwise be dropped differently between runs.
//
// Sorting on GEOMETRY rather than on generation index is what makes this a fix instead of a
// papering-over: the key is derived from the element itself, so it cannot depend on how the work
// was scheduled. Two elements that tie on every component are geometrically identical, and
// swapping those is a no-op.
int32 PwModelCollision_CompareVectors(const FVector& A, const FVector& B)
{
    if (A.X != B.X) return A.X < B.X ? -1 : 1;
    if (A.Y != B.Y) return A.Y < B.Y ? -1 : 1;
    if (A.Z != B.Z) return A.Z < B.Z ? -1 : 1;
    return 0;
}

void PwModelCollision_SortToCanonicalOrder(FKAggregateGeom& Geom)
{
    Geom.ConvexElems.Sort([](const FKConvexElem& A, const FKConvexElem& B)
    {
        // Bounds first (cheap and almost always decisive), then the vertex list, so two hulls
        // sharing a bounding box still get a total order.
        if (const int32 Min = PwModelCollision_CompareVectors(A.ElemBox.Min, B.ElemBox.Min)) return Min < 0;
        if (const int32 Max = PwModelCollision_CompareVectors(A.ElemBox.Max, B.ElemBox.Max)) return Max < 0;
        if (A.VertexData.Num() != B.VertexData.Num()) return A.VertexData.Num() < B.VertexData.Num();
        for (int32 Index = 0; Index < A.VertexData.Num(); ++Index)
        {
            if (const int32 Vertex = PwModelCollision_CompareVectors(A.VertexData[Index], B.VertexData[Index]))
            {
                return Vertex < 0;
            }
        }
        return false;
    });

    // The detect_boxes / detect_spheres / detect_capsules shapes and the non-hull `method` values
    // land through the same locked append (GetDetectedSimpleShape), so they need the same treatment.
    Geom.BoxElems.Sort([](const FKBoxElem& A, const FKBoxElem& B)
    {
        if (const int32 Centre = PwModelCollision_CompareVectors(A.Center, B.Center)) return Centre < 0;
        if (A.X != B.X) return A.X < B.X;
        if (A.Y != B.Y) return A.Y < B.Y;
        return A.Z < B.Z;
    });
    Geom.SphereElems.Sort([](const FKSphereElem& A, const FKSphereElem& B)
    {
        if (const int32 Centre = PwModelCollision_CompareVectors(A.Center, B.Center)) return Centre < 0;
        return A.Radius < B.Radius;
    });
    Geom.SphylElems.Sort([](const FKSphylElem& A, const FKSphylElem& B)
    {
        if (const int32 Centre = PwModelCollision_CompareVectors(A.Center, B.Center)) return Centre < 0;
        if (A.Radius != B.Radius) return A.Radius < B.Radius;
        return A.Length < B.Length;
    });
    // LevelSetElems are deliberately NOT sorted: FKLevelSetElem carries a dense grid with no cheap
    // geometric key, and `method=level_sets` is the one generator whose output this leaves in
    // completion order. Documented in docs/pwmodel-format.md under Reproducibility.
}

// ---- Enum-derived vocabularies ---------------------------------------------------------------

FString PwModelCollision_ToSnakeCase(const FString& In)
{
    FString Out;
    Out.Reserve(In.Len() * 2);

    for (int32 Index = 0; Index < In.Len(); ++Index)
    {
        const TCHAR Character = In[Index];
        const bool bStartsWord = Index > 0 && FChar::IsUpper(Character)
            && (!FChar::IsUpper(In[Index - 1])
                || (Index + 1 < In.Len() && FChar::IsLower(In[Index + 1])));

        if (bStartsWord)
        {
            Out.AppendChar(TEXT('_'));
        }
        Out.AppendChar(FChar::ToLower(Character));
    }

    return Out;
}

// `CTF_UseDefault` -> `UseDefault`. Enum-class entries arrive unprefixed and pass through.
FString PwModelCollision_StripEnumPrefix(const FString& Name)
{
    int32 Underscore = INDEX_NONE;
    if (Name.FindChar(TEXT('_'), Underscore) && Underscore > 0)
    {
        for (int32 Index = 0; Index < Underscore; ++Index)
        {
            if (!FChar::IsUpper(Name[Index]))
            {
                return Name;
            }
        }
        return Name.Mid(Underscore + 1);
    }
    return Name;
}

// One walk of a UEnum, skipping the synthesised _MAX sentinel. Returns short-name/value pairs.
void PwModelCollision_EnumEntries(const UEnum* Enum, TArray<TPair<FString, int64>>& Out)
{
    if (!Enum)
    {
        return;
    }

    const int32 Count = Enum->NumEnums();
    for (int32 Index = 0; Index < Count; ++Index)
    {
        const FString Name = Enum->GetNameStringByIndex(Index);
        if (Name.IsEmpty() || Name.EndsWith(TEXT("_MAX")))
        {
            continue;
        }
        Out.Emplace(Name, Enum->GetValueByIndex(Index));
    }
}

struct FPwModelCollisionComplexityEntry
{
    FString Name;
    ECollisionTraceFlag Flag = ECollisionTraceFlag::CTF_UseDefault;
};

const TArray<FPwModelCollisionComplexityEntry>& PwModelCollision_ComplexityTable()
{
    static const TArray<FPwModelCollisionComplexityEntry> Table = []()
    {
        TArray<TPair<FString, int64>> Entries;
        PwModelCollision_EnumEntries(StaticEnum<ECollisionTraceFlag>(), Entries);

        TArray<FPwModelCollisionComplexityEntry> Built;
        Built.Reserve(Entries.Num());

        for (const TPair<FString, int64>& Entry : Entries)
        {
            FString Spelling = PwModelCollision_ToSnakeCase(PwModelCollision_StripEnumPrefix(Entry.Key));

            // The prefix stripped above is `CTF_`, not `CTF_Use`, because `CTF_UseDefault` keeps
            // its `use_`: bare `default` reads as "this format's default" rather than "the
            // project's physics setting", which is the opposite of what the flag means. Every
            // other value names two collision representations, so its `use_` carries nothing.
            if (Spelling.StartsWith(TEXT("use_")))
            {
                const FString Rest = Spelling.RightChop(4);
                if (Rest.Contains(TEXT("_")))
                {
                    Spelling = Rest;
                }
            }

            FPwModelCollisionComplexityEntry Complexity;
            Complexity.Name = MoveTemp(Spelling);
            Complexity.Flag = static_cast<ECollisionTraceFlag>(Entry.Value);
            Built.Add(MoveTemp(Complexity));
        }

        return Built;
    }();

    return Table;
}

struct FPwModelCollisionMethodEntry
{
    FString Name;
    EGeometryScriptCollisionGenerationMethod Method = EGeometryScriptCollisionGenerationMethod::ConvexHulls;
};

const TArray<FPwModelCollisionMethodEntry>& PwModelCollision_MethodTable()
{
    static const TArray<FPwModelCollisionMethodEntry> Table = []()
    {
        TArray<TPair<FString, int64>> Entries;
        PwModelCollision_EnumEntries(StaticEnum<EGeometryScriptCollisionGenerationMethod>(), Entries);

        TArray<FPwModelCollisionMethodEntry> Built;
        Built.Reserve(Entries.Num());

        for (const TPair<FString, int64>& Entry : Entries)
        {
            FPwModelCollisionMethodEntry Method;
            Method.Name = PwModelCollision_ToSnakeCase(Entry.Key);
            Method.Method = static_cast<EGeometryScriptCollisionGenerationMethod>(Entry.Value);
            Built.Add(MoveTemp(Method));
        }

        return Built;
    }();

    return Table;
}

// AST values are read through PwValueRead.h, shared with the compiler. The six readers were
// duplicated here once, with `rotate=`'s (roll, pitch, yaw) -> FRotator(Pitch, Yaw, Roll)
// reordering spelled a second time in a different-looking form; flip one copy and a rotated
// collision element silently disagrees with the part transform it was authored against, with
// nothing to fail. Call sites qualify with PwValueRead:: rather than importing the names,
// because an unqualified GetNumber in an anonymous namespace is exactly what Unity merges into a
// sibling TU's.

// ---- Result helpers ----------------------------------------------------------------------

void PwModelCollision_Fail(FPwModelCollisionResult& Result, const TCHAR* Code,
                           int32 Line, int32 Column, FString Message)
{
    Result.bSuccess = false;
    Result.ErrorCode = Code;
    Result.ErrorMessage = MoveTemp(Message);
    Result.ErrorLine = Line;
    Result.ErrorColumn = Column;
    Result.Geom.EmptyElements();
    Result.ElementsWritten = 0;
}

TArray<FVector> PwModelCollision_ReadVertices(const UDynamicMesh* Mesh)
{
    TArray<FVector> Points;
    if (!Mesh)
    {
        return Points;
    }

    Mesh->ProcessMesh([&Points](const UE::Geometry::FDynamicMesh3& Source)
    {
        Points.Reserve(Source.VertexCount());
        for (const int32 VertexId : Source.VertexIndicesItr())
        {
            Points.Add(Source.GetVertex(VertexId));
        }
    });

    return Points;
}

// An empty element list to iterate when `auto` already replaced the aggregate geometry. A named
// static rather than a temporary in the range-for, which would copy the whole array on the other
// branch of the conditional.
const TArray<FPwOp>& PwModelCollision_NoElements()
{
    static const TArray<FPwOp> None;
    return None;
}

// A transient mesh held by a strong pointer for its whole lifetime. The raw NewObject form was
// unrooted across BuildOps (arbitrary user geometry ops) and across the simplify + generate pair,
// either of which can allocate enough to trigger a GC that collects it out from under us.
TStrongObjectPtr<UDynamicMesh> PwModelCollision_NewTransientMesh()
{
    return TStrongObjectPtr<UDynamicMesh>(NewObject<UDynamicMesh>(GetTransientPackage()));
}

// Reduces a point cloud to the convex hull's own vertices, with IndexData remapped onto them.
//
// Chaos hulls whatever VertexData holds at cook time (BodySetup.cpp:1925-1943 feeds it straight
// to FConvexBuilder::BuildIndices), so storing the raw cloud would collide identically. It is
// reduced anyway for two reasons: the stored element then IS the hull the format documents -
// open the asset and the interior points of a subtracted cavity are simply not there - and the
// discarded concavity is observable in a test rather than deferred to a cook nobody runs.
// Returns false when the points bound no volume.
bool PwModelCollision_HullFromPoints(TArray<FVector> Points, FKConvexElem& OutElem)
{
    if (Points.Num() < 4)
    {
        return false;
    }

    FKConvexElem Elem;
    Elem.VertexData = MoveTemp(Points);

    const TArray<int32> HullIndices = Elem.GetChaosConvexIndices();
    if (HullIndices.Num() == 0)
    {
        return false;
    }

    TArray<int32> Remap;
    Remap.Init(INDEX_NONE, Elem.VertexData.Num());

    TArray<FVector> HullVertices;
    TArray<int32> RemappedIndices;
    RemappedIndices.Reserve(HullIndices.Num());

    for (const int32 SourceIndex : HullIndices)
    {
        if (!Elem.VertexData.IsValidIndex(SourceIndex))
        {
            return false;
        }
        if (Remap[SourceIndex] == INDEX_NONE)
        {
            Remap[SourceIndex] = HullVertices.Add(Elem.VertexData[SourceIndex]);
        }
        RemappedIndices.Add(Remap[SourceIndex]);
    }

    if (HullVertices.Num() < 4)
    {
        return false;
    }

    Elem.VertexData = MoveTemp(HullVertices);
    Elem.IndexData = MoveTemp(RemappedIndices);
    Elem.UpdateElemBox();

    OutElem = MoveTemp(Elem);
    return true;
}

// One `auto` rule -> the whole aggregate geometry. Returns false with Result filled by
// PwModelCollision_Fail when the rule cannot be honoured.
//
// This REPLACES Result.Geom rather than adding to it, which is why the caller runs it before any
// explicit element can accumulate: an assignment reached from inside the element loop is a
// silent discard that happens to be correct only because a guard two hundred lines earlier
// forbids the combination.
bool PwModelCollision_ApplyAutoRule(const FPwOp& Element, UDynamicMesh* MergedMesh,
                                    FPwModelCollisionResult& Result)
{
    const FPwValue* MethodValue = PwValueRead::FindValue(Element.Params, TEXT("method"));
    EGeometryScriptCollisionGenerationMethod Method =
        EGeometryScriptCollisionGenerationMethod::ConvexHulls;

    if (MethodValue)
    {
        if (MethodValue->Type != EPwValueType::Identifier
            || !PwModelCollisionNames::TryParseAutoMethod(MethodValue->Text, Method))
        {
            PwModelCollision_Fail(Result, PwSourceDiagnosticCodes::PWSRC_BAD_VALUE,
                MethodValue->Line, MethodValue->Column,
                FString::Printf(TEXT("'auto method' expects one of: %s."),
                    *FString::Join(PwModelCollisionNames::AutoMethodNames(), TEXT(", "))));
            return false;
        }
    }

    if (!MergedMesh || MergedMesh->GetTriangleCount() == 0)
    {
        // The compiler validates the merged mesh before it gets here, so this is reachable only
        // from a direct caller. A warning rather than an error: there is nothing wrong with the
        // document, and ElementsWritten already reports the outcome.
        Result.Warnings.Add(TEXT("'auto' had no source geometry: the merged mesh is empty, so no collision was generated."));
        return true;
    }

    UDynamicMesh* Source = MergedMesh;
    TStrongObjectPtr<UDynamicMesh> Scratch;

    const int32 RequestedSimplifyTo = PwValueRead::GetInt(Element.Params, TEXT("simplify_to"), 0);
    if (RequestedSimplifyTo > 0 && MergedMesh->GetTriangleCount() > RequestedSimplifyTo)
    {
        // Four triangles is the fewest that bound a volume. Clamping warns rather than silently
        // altering the request, per the extracted-op contract's rule 5.
        const int32 SimplifyTo = FMath::Max(4, RequestedSimplifyTo);
        if (SimplifyTo != RequestedSimplifyTo)
        {
            Result.Warnings.Add(FString::Printf(
                TEXT("'auto simplify_to=%d' is below the 4 triangles needed to bound a volume; raised to %d."),
                RequestedSimplifyTo, SimplifyTo));
        }

        // On a copy: `auto` must not alter the mesh the asset is built from.
        Scratch = PwModelCollision_NewTransientMesh();
        Scratch->SetMesh(MergedMesh->GetMeshRef());

        FGeometryScriptSimplifyMeshOptions SimplifyOptions;
        SimplifyOptions.Method = EGeometryScriptRemoveMeshSimplificationType::StandardQEM;
        SimplifyOptions.bAllowSeamCollapse = true;

        UGeometryScriptLibrary_MeshSimplifyFunctions::ApplySimplifyToTriangleCount(
            Scratch.Get(), SimplifyTo, SimplifyOptions, nullptr);

        Source = Scratch.Get();
    }

    // max_hulls_per_component, NOT max_hulls. The number is a CEILING applied to EACH CONNECTED
    // COMPONENT of the merged mesh, never an asset-wide total and never a quota:
    // ComputeCollisionFromMesh splits the input into one submesh per component before any
    // decomposition runs (CollisionFunctions.cpp:70-91, the same split the canonical-order helper
    // above documents), then branches on the budget (:120). Above 1 it runs
    // Generate_ConvexHullDecompositions with ConvexDecompositionMaxPieces set to the budget
    // (:126, MeshSimpleShapeApproximation.cpp:335); at 1 or 0 it runs Generate_ConvexHulls
    // (:130, :306), which emits exactly one hull per submesh and never reads the budget.
    //
    // WHAT COMES BACK IS THE DECOMPOSER'S COUNT, NOT THE BUDGET'S, AND IT IS NOT STABLE. The
    // decomposition path ends in FConvexDecomposition3::Compute(MaxPieces, NumAdditionalSplits,
    // ErrorTolerance, MinPartThickness) (MeshSimpleShapeApproximation.cpp:442-443) with the three
    // tuning values left at their struct defaults (CollisionFunctions.h:79-85), so a piece already
    // convex enough comes back as one hull however much budget it is handed. Then the trim runs:
    // bRemoveFullyContainedShapes defaults true (CollisionFunctions.h:95) and we leave it there,
    // so RemoveContainedGeometry drops any hull fully inside another (CollisionFunctions.cpp:149).
    // Measured on ships_wheel's 11 components, one build, one session - budget -> elements:
    // 1 -> 11, 2 -> 11, 3 -> 13, 4 -> 44 on one run and 14 on the next five, 8 -> 22, 16 -> 37.
    // Not proportional, not monotonic, and not reproducible run to run. An earlier revision of
    // this comment claimed 1 -> 11, 2 -> 22, 16 -> 176 and "no trim after the loop"; the first is
    // one outcome of several and the second is plainly false. Do not pin a test to an exact
    // element count above budget 1 - only budget 1 is stable, because it skips the decomposer.
    //
    // Renamed rather than reinterpreted, because the old name could not be made true. Scaling
    // the figure down by the component count cannot honour a budget BELOW the component count -
    // the engine's floor is one hull per component, so `max_hulls=1` on an 11-piece mesh is 11
    // however the number is divided. The only real whole-asset cap is a post-pass
    // (MergeSimpleCollisionShapes with MaxShapeCount, CollisionFunctions.h:577-583), and that
    // MERGES hulls across disjoint pieces - the merged hull swallows the air between them, which
    // is worse collision than the naming confusion it would fix. FGeometryScriptCollisionFrom-
    // MeshOptions::MaxShapeCount is not that cap either: it is gated on
    // `MaxShapeCount < Components.Num()` (CollisionFunctions.cpp:152), i.e. on the COMPONENT
    // count rather than the produced count, so it does not fire in exactly the case that needs
    // it, and when it does fire it discards the smallest shapes outright. So the name moved to
    // the behaviour instead of the behaviour to the name.
    const FPwValue* PerComponentValue =
        PwValueRead::FindValue(Element.Params, TEXT("max_hulls_per_component"));
    const FPwValue* LegacyValue = PwValueRead::FindValue(Element.Params, TEXT("max_hulls"));

    int32 RequestedMaxHulls = 8;
    if (PerComponentValue)
    {
        RequestedMaxHulls = PwValueRead::GetInt(Element.Params, TEXT("max_hulls_per_component"), 8);
        if (LegacyValue)
        {
            Result.Warnings.Add(TEXT(
                "'auto' was given both 'max_hulls_per_component' and its old spelling 'max_hulls'; "
                "'max_hulls_per_component' is the one that ran. Delete the 'max_hulls='."));
        }
    }
    else if (LegacyValue)
    {
        RequestedMaxHulls = PwValueRead::GetInt(Element.Params, TEXT("max_hulls"), 8);
        // The old name is still accepted - documents were written against it - but it read as an
        // asset-wide budget and never was one, so it says so once per compile rather than never.
        Result.Warnings.Add(FString::Printf(
            TEXT("'auto max_hulls=%d' is the old spelling of 'max_hulls_per_component'. It was never "
                 "an asset-wide budget: the engine spends it on each connected component of the merged "
                 "mesh, so a mesh in N disconnected pieces produces up to N times this many elements. "
                 "Rename the parameter."),
            RequestedMaxHulls));
    }

    const int32 MaxHulls = FMath::Max(1, RequestedMaxHulls);
    if (MaxHulls != RequestedMaxHulls)
    {
        Result.Warnings.Add(FString::Printf(
            TEXT("'auto max_hulls_per_component=%d' cannot produce fewer than one hull; raised to %d."),
            RequestedMaxHulls, MaxHulls));
    }

    FGeometryScriptCollisionFromMeshOptions Options;
    Options.bEmitTransaction = false;
    Options.Method = Method;
    Options.MaxConvexHullsPerMesh = MaxHulls;
    // The engine defaults these three to true; the format defaults them to false, so they are
    // always written rather than left to whichever side changes first.
    Options.bAutoDetectBoxes = PwValueRead::GetBool(Element.Params, TEXT("detect_boxes"), false);
    Options.bAutoDetectSpheres = PwValueRead::GetBool(Element.Params, TEXT("detect_spheres"), false);
    Options.bAutoDetectCapsules = PwValueRead::GetBool(Element.Params, TEXT("detect_capsules"), false);

    // Counted from the mesh the generator will actually see - after any simplify_to, which can
    // weld pieces together - because it is the multiplier the budget is spent against, and a
    // warning that cannot name it leaves the author unable to predict the next number.
    int32 ComponentCount = 0;
    Source->ProcessMesh([&ComponentCount](const UE::Geometry::FDynamicMesh3& ReadMesh)
    {
        UE::Geometry::FMeshConnectedComponents Components(&ReadMesh);
        Components.FindConnectedTriangles();
        ComponentCount = Components.Num();
    });

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    const FGeometryScriptSimpleCollision Generated =
        UGeometryScriptLibrary_CollisionFunctions::GenerateCollisionFromMesh(Source, Options, nullptr);
    FKAggregateGeom GeneratedGeom = Generated.AggGeom;
#else
    // GenerateCollisionFromMesh - the mesh -> FGeometryScriptSimpleCollision entry point - arrived
    // in UE 5.5. On 5.4 the only public door onto the same UELocal::ComputeCollisionFromMesh is
    // the component-writing overload, so the generator runs against a transient component and the
    // result is read straight back off it. Same options struct, same decomposition, same order.
    FKAggregateGeom GeneratedGeom;
    {
        TStrongObjectPtr<UDynamicMeshComponent> CollisionScratch(
            NewObject<UDynamicMeshComponent>(GetTransientPackage()));
        UGeometryScriptLibrary_CollisionFunctions::SetDynamicMeshCollisionFromMesh(
            Source, CollisionScratch.Get(), Options, nullptr);
        GeneratedGeom = CollisionScratch->GetSimpleCollisionShapes();
        CollisionScratch->MarkAsGarbage();
    }
#endif

    if (Scratch)
    {
        Scratch->MarkAsGarbage();
    }

    // Assignment, not accumulation - see the note above. Nothing has been written yet.
    check(Result.Geom.GetElementCount() == 0);
    Result.Geom = MoveTemp(GeneratedGeom);

    // The generator hands back completion-ordered elements; re-key them on geometry so one source
    // compiles to one asset rather than to one of N permutations. See the helper's comment.
    PwModelCollision_SortToCanonicalOrder(Result.Geom);

    if (Result.Geom.GetElementCount() == 0)
    {
        Result.Warnings.Add(FString::Printf(
            TEXT("'auto method=%s' generated no collision elements from the merged mesh."),
            MethodValue ? *MethodValue->Text : TEXT("convex_hulls")));
    }
    else if (Result.Geom.GetElementCount() > MaxHulls)
    {
        // The one number an author cannot derive from the document, and the reason the response
        // has to carry it: the count is the decomposer's, not the budget's (see the note above),
        // so it cannot be predicted from the source and nothing else on this surface reports it.
        // It is not an error - the per-component ceiling is the engine's design and usually the
        // collision you want, one hull set per physical piece.
        Result.Warnings.Add(FString::Printf(
            TEXT("'auto max_hulls_per_component=%d' produced %d collision elements: the merged mesh is "
                 "in %d disconnected pieces and the budget is a ceiling applied to each one. This is the "
                 "parameter's design, not an overrun - lower it, or weld the pieces, if the element count "
                 "matters. The count is the decomposer's and is not reproducible run to run; read it here "
                 "rather than predicting it."),
            MaxHulls, Result.Geom.GetElementCount(), ComponentCount));
    }

    return true;
}
} // namespace

namespace PwModelCollisionNames
{
const TArray<FString>& ComplexityNames()
{
    static const TArray<FString> Names = []()
    {
        TArray<FString> Built;
        for (const FPwModelCollisionComplexityEntry& Entry : PwModelCollision_ComplexityTable())
        {
            Built.Add(Entry.Name);
        }
        return Built;
    }();

    return Names;
}

bool TryParseComplexity(const FString& Name, ECollisionTraceFlag& OutFlag)
{
    for (const FPwModelCollisionComplexityEntry& Entry : PwModelCollision_ComplexityTable())
    {
        if (Entry.Name == Name)
        {
            OutFlag = Entry.Flag;
            return true;
        }
    }
    return false;
}

const TArray<FString>& AutoMethodNames()
{
    static const TArray<FString> Names = []()
    {
        TArray<FString> Built;
        for (const FPwModelCollisionMethodEntry& Entry : PwModelCollision_MethodTable())
        {
            Built.Add(Entry.Name);
        }
        return Built;
    }();

    return Names;
}

bool TryParseAutoMethod(const FString& Name, EGeometryScriptCollisionGenerationMethod& OutMethod)
{
    for (const FPwModelCollisionMethodEntry& Entry : PwModelCollision_MethodTable())
    {
        if (Entry.Name == Name)
        {
            OutMethod = Entry.Method;
            return true;
        }
    }
    return false;
}
} // namespace PwModelCollisionNames

FPwModelCollisionResult BuildCollision(const FPwModelCollision& CollisionBlock,
                                       UDynamicMesh* MergedMesh,
                                       FBuildMeshFromOps BuildOps)
{
    FPwModelCollisionResult Result;
    Result.bSuccess = true;

    // `simple_and_complex` is the format's default, not CTF_UseDefault: a model that declares a
    // collision block has asked for simple shapes, and routing it through the project setting
    // would let a project-wide flag decide whether they are ever built.
    Result.Trace = ECollisionTraceFlag::CTF_UseSimpleAndComplex;

    if (CollisionBlock.Complexity.Type != EPwValueType::None)
    {
        if (CollisionBlock.Complexity.Type != EPwValueType::Identifier
            || !PwModelCollisionNames::TryParseComplexity(CollisionBlock.Complexity.Text, Result.Trace))
        {
            PwModelCollision_Fail(Result, PwSourceDiagnosticCodes::PWSRC_BAD_VALUE,
                CollisionBlock.Complexity.Line, CollisionBlock.Complexity.Column,
                FString::Printf(TEXT("'complexity' expects one of: %s."),
                    *FString::Join(PwModelCollisionNames::ComplexityNames(), TEXT(", "))));
            return Result;
        }
    }

    // The parser rejects `auto` beside explicit elements, but BuildCollision also accepts an AST
    // built in code, and letting `auto` overwrite hand-authored elements (or the reverse) is the
    // silent-wrong-collision this code exists to avoid.
    int32 AutoCount = 0;
    int32 ExplicitCount = 0;
    const FPwOp* FirstAuto = nullptr;

    for (const FPwOp& Element : CollisionBlock.Elements)
    {
        if (Element.OpName == TEXT("auto"))
        {
            ++AutoCount;
            if (!FirstAuto)
            {
                FirstAuto = &Element;
            }
        }
        else
        {
            ++ExplicitCount;
        }
    }

    if (AutoCount > 1 || (AutoCount > 0 && ExplicitCount > 0))
    {
        PwModelCollision_Fail(Result, PwModelDiagnosticCodes::PWMODEL_COLLISION_CONFLICT,
            FirstAuto->Line, FirstAuto->Column,
            FString::Printf(TEXT("A collision block carries either explicit elements or one 'auto' rule; found %d 'auto' rule(s) and %d explicit element(s)."),
                AutoCount, ExplicitCount));
        return Result;
    }

    // `auto` REPLACES the aggregate geometry, so it runs here rather than as a branch inside the
    // element loop: the guard above makes it exclusive, and handling it before anything can
    // accumulate is what makes that assignment structurally safe rather than safe-by-guard.
    if (FirstAuto)
    {
        if (!PwModelCollision_ApplyAutoRule(*FirstAuto, MergedMesh, Result))
        {
            return Result;
        }
    }

    // Every element below ACCUMULATES into Result.Geom. `auto` assigns, so it is handled above
    // rather than as a branch here, and the two are exclusive by the guard above - which is what
    // lets this loop be skipped outright instead of carrying an `auto` branch that can never run.
    const TArray<FPwOp>& ExplicitElements =
        FirstAuto ? PwModelCollision_NoElements() : CollisionBlock.Elements;

    for (const FPwOp& Element : ExplicitElements)
    {
        if (Element.OpName == TEXT("box"))
        {
            const FVector Size = PwValueRead::GetVector3(Element.Params, TEXT("size"), FVector::ZeroVector);

            FKBoxElem Box;
            Box.Center = PwValueRead::GetVector3(Element.Params, TEXT("at"), FVector::ZeroVector);
            Box.Rotation = PwValueRead::GetRotator(Element.Params, TEXT("rotate"));
            // FKBoxElem::X/Y/Z are full extents along each axis (BoxElem.h:34-44), so `size` maps
            // across unhalved.
            Box.X = static_cast<float>(Size.X);
            Box.Y = static_cast<float>(Size.Y);
            Box.Z = static_cast<float>(Size.Z);
            Result.Geom.BoxElems.Add(Box);
        }
        else if (Element.OpName == TEXT("sphere"))
        {
            FKSphereElem Sphere;
            Sphere.Center = PwValueRead::GetVector3(Element.Params, TEXT("at"), FVector::ZeroVector);
            Sphere.Radius = static_cast<float>(
                PwValueRead::GetNumber(Element.Params, TEXT("radius"), 0.0));
            Result.Geom.SphereElems.Add(Sphere);
        }
        else if (Element.OpName == TEXT("capsule"))
        {
            const double Radius = PwValueRead::GetNumber(Element.Params, TEXT("radius"), 0.0);
            const double Height = PwValueRead::GetNumber(Element.Params, TEXT("height"), 0.0);

            // FKSphylElem::Length is the cylindrical segment only - "add Radius to both ends to
            // find total length" (SphylElem.h:38) - while the document's `height` is the total.
            const double Length = Height - 2.0 * Radius;
            if (Length < 0.0)
            {
                Result.Warnings.Add(FString::Printf(
                    TEXT("capsule height=%g is below 2 * radius (%g), which no FKSphylElem can represent; Length clamped to 0, giving a sphere of radius %g."),
                    Height, 2.0 * Radius, Radius));
            }

            FKSphylElem Capsule;
            Capsule.Center = PwValueRead::GetVector3(Element.Params, TEXT("at"), FVector::ZeroVector);
            Capsule.Rotation = PwValueRead::GetRotator(Element.Params, TEXT("rotate"));
            Capsule.Radius = static_cast<float>(Radius);
            Capsule.Length = static_cast<float>(FMath::Max(0.0, Length));
            Result.Geom.SphylElems.Add(Capsule);
        }
        else if (Element.OpName == TEXT("hull"))
        {
            // Strong pointer: BuildOps runs arbitrary user geometry ops, any of which can allocate
            // enough to trigger a GC that would collect an unrooted transient mesh mid-build.
            TStrongObjectPtr<UDynamicMesh> HullMesh = PwModelCollision_NewTransientMesh();

            TArray<FString> BuildErrors;
            const bool bBuilt = BuildOps(Element.Children, HullMesh.Get(), BuildErrors);

            TArray<FVector> Points;
            if (bBuilt)
            {
                Points = PwModelCollision_ReadVertices(HullMesh.Get());
            }
            HullMesh->MarkAsGarbage();

            if (!bBuilt)
            {
                PwModelCollision_Fail(Result, PwModelDiagnosticCodes::PWMODEL_DEGENERATE_HULL,
                    Element.Line, Element.Column,
                    FString::Printf(TEXT("A 'hull' block's ops did not build: %s"),
                        BuildErrors.Num() > 0 ? *FString::Join(BuildErrors, TEXT("; ")) : TEXT("no reason reported")));
                return Result;
            }

            FKConvexElem Convex;
            if (!PwModelCollision_HullFromPoints(MoveTemp(Points), Convex))
            {
                PwModelCollision_Fail(Result, PwModelDiagnosticCodes::PWMODEL_DEGENERATE_HULL,
                    Element.Line, Element.Column,
                    TEXT("A 'hull' block produced no volume to hull: its ops yielded an empty or degenerate mesh."));
                return Result;
            }

            Result.Geom.ConvexElems.Add(MoveTemp(Convex));
        }
        else if (Element.OpName == TEXT("convex"))
        {
            TArray<FVector> Points = PwValueRead::GetPointList3(Element.Params, TEXT("points"));

            if (Points.Num() < 4)
            {
                PwModelCollision_Fail(Result, PwModelDiagnosticCodes::PWMODEL_DEGENERATE_HULL,
                    Element.Line, Element.Column,
                    FString::Printf(TEXT("'convex' needs at least 4 points to bound a volume, but %d were usable."),
                        Points.Num()));
                return Result;
            }

            // Exact, unlike `hull`: these points are machine-generated hull vertices already, and
            // re-hulling them would silently discard a caller's own decomposition.
            FKConvexElem Convex;
            Convex.VertexData = MoveTemp(Points);
            Convex.ComputeChaosConvexIndices(true);
            Convex.UpdateElemBox();
            Result.Geom.ConvexElems.Add(MoveTemp(Convex));
        }
        else
        {
            // The list is DERIVED from the op table rather than spelled here: a collision entry
            // added to the table and not to this loop would otherwise be advertised as valid by
            // model.describe_ops and rejected by a message claiming it does not exist.
            PwModelCollision_Fail(Result, PwSourceDiagnosticCodes::PWSRC_UNKNOWN_OP,
                Element.Line, Element.Column,
                FString::Printf(TEXT("'%s' is not a collision entry. Valid entries: %s."),
                    *Element.OpName,
                    *FString::Join(PwModelOpTable::NamesInContext(EPwModelOpContext::Collision), TEXT(", "))));
            return Result;
        }
    }

    Result.ElementsWritten = Result.Geom.GetElementCount();

    if (Result.Trace == ECollisionTraceFlag::CTF_UseComplexAsSimple)
    {
        Result.Warnings.Add(TEXT("complexity = complex_as_simple builds only the per-poly shape, which can be collided against but never simulated - the asset cannot be a moving rigid body."));

        if (Result.ElementsWritten > 0)
        {
            Result.Warnings.Add(FString::Printf(
                TEXT("complexity = complex_as_simple with %d simple collision element(s): those elements are built and never queried."),
                Result.ElementsWritten));
        }
    }

    return Result;
}
