// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the material slot an APPENDING modifier's output lands on - `sweep` and
// `extrude_along_spline`.
//
// The defect these pin: the two ops append through an engine call whose
// FGeometryScriptPrimitiveOptions::MaterialID is 0, and nothing in the compiler retagged the
// result. ID 0 is NOT a neutral default - the slot table is model-wide and allocated in first-use
// order, so 0 is whichever slot the FIRST PART IN THE DOCUMENT tagged. A rod swept in part `b`
// therefore shipped rendering in part `a`'s material: one part in two materials, on a green
// compile, with `materialSlots` unchanged and no diagnostic anywhere. The only workaround was to
// order the parts so the wanted slot fell at index 0, which breaks the moment a part is added
// above it. The ops took no `material=` either, so the author could not correct it.
//
// What each test here would let through if it were reverted:
//
//  - THE OUTPUT LANDS ON THE SLOT OF THE GEOMETRY IT EXTENDS. Asserted through the BAKED ASSET,
//    because that is the only place the answer exists: the slot COUNT is 2 either way, so every
//    field of the compile result agrees between the correct build and the broken one. Read as
//    "no polygon group spans both parts' X regions", which needs no assumption about how material
//    IDs map onto polygon-group ids or slot names through the static-mesh build.
//  - `material=` IS AUTHORABLE ON THE OP. Before this, writing it was an unknown-parameter error
//    and the compile stopped at stage 1, so a shape needing a second slot could not be swept.
//  - AMBIGUITY IS REPORTED. Appending onto geometry that carries more than one slot has no single
//    answer, and picking one in silence is the same class of defect as taking slot 0 in silence.
//  - THE ORDINARY DOCUMENT STAYS SILENT. A sweep onto single-slot geometry, and a sweep carrying
//    `material=`, must raise nothing - a warning on those would be turned off wholesale and take
//    the real case with it.
#include "Misc/AutomationTest.h"

#include "Model/PwModelCompiler.h"
#include "Model/PwModelDiagnostic.h"
#include "Tests/TestUtils.h"

#include "EditorAssetLibrary.h"
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"

namespace
{
// Prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper with a
// common name would collide with a sibling test TU once Unity merges them.

const TCHAR* const PwModifierMat_OutputRoot = TEXT("/Game/PinWrightTests/PwModelModifierMaterial");

// The X coordinate that separates part `a`'s geometry from part `b`'s in the probe below. Part `a`
// spans [-50, 50]; part `b`'s box spans [550, 650] and its swept rod [700, 900].
constexpr float PwModifierMat_PartSplitX = 250.0f;

FString PwModifierMat_AssetPath(const TCHAR* Leaf)
{
    return FString::Printf(TEXT("%s/%s"), PwModifierMat_OutputRoot, Leaf);
}

// CleanupTestAsset, never UEditorAssetLibrary::DeleteAsset: that routes through
// ObjectTools::ForceDeleteObjects, whose full-object-graph reference walk costs seconds per call
// against a live editor's ~300k UObjects. The discard renames the asset AND its package into
// /Transient, removes the registry entry and deletes the .uasset, which is everything the
// pre-compile callers need to leave AssetCreatePolicy::Resolve on its Create branch.
void PwModifierMat_DeleteIfPresent(const FString& AssetPath)
{
    if (UEditorAssetLibrary::DoesAssetExist(AssetPath))
    {
        CleanupTestAsset(AssetPath);
    }
}

FPwModelCompileResult PwModifierMat_Validate(const TCHAR* Source)
{
    FPwModelCompileOptions Options;
    Options.bValidateOnly = true;
    Options.bSave = false;
    return FPwModelCompiler::Compile(Source, Options);
}

FPwModelCompileResult PwModifierMat_CompileTo(const TCHAR* Source, const FString& AssetPath)
{
    FPwModelCompileOptions Options;
    Options.OutputAssetPath = AssetPath;
    Options.SourcePath = TEXT("Tests/PwModelModifierMaterial.pwmodel");
    Options.bOverwrite = true;
    Options.bSave = true;
    return FPwModelCompiler::Compile(Source, Options);
}

FString PwModifierMat_Describe(const FPwModelCompileResult& Result)
{
    TArray<FString> Slots;
    for (int32 Index = 0; Index < Result.MaterialSlotList.Num(); ++Index)
    {
        Slots.Add(FString::Printf(TEXT("%d:%s"), Index, *Result.MaterialSlotList[Index].Name));
    }

    TArray<FString> Diagnostics;
    for (const FPwDiagnostic& Diagnostic : Result.Diagnostics)
    {
        Diagnostics.Add(Diagnostic.ToString());
    }

    return FString::Printf(TEXT("slots [%s]; diagnostics [%s]"),
        *FString::Join(Slots, TEXT(", ")), *FString::Join(Diagnostics, TEXT(" | ")));
}

TArray<FPwDiagnostic> PwModifierMat_WithCode(const FPwModelCompileResult& Result, const TCHAR* Code)
{
    TArray<FPwDiagnostic> Matching;
    for (const FPwDiagnostic& Diagnostic : Result.Diagnostics)
    {
        if (Diagnostic.Code == Code)
        {
            Matching.Add(Diagnostic);
        }
    }
    return Matching;
}

// One material section of the baked mesh, described by WHERE its triangles are rather than by what
// its slot is called. That indirection is the point: a group whose triangles straddle both parts is
// the defect, and saying so needs no claim about how a material ID becomes a polygon group id or a
// slot name through the static-mesh build.
struct FPwModifierMat_GroupSpan
{
    int32 TriangleCount = 0;
    int32 TrianglesBelowSplit = 0;
    int32 TrianglesAboveSplit = 0;
};

TArray<FPwModifierMat_GroupSpan> PwModifierMat_MeasureGroups(const FMeshDescription& Description,
                                                             float SplitX)
{
    TArray<FPwModifierMat_GroupSpan> Spans;
    Spans.SetNum(Description.PolygonGroups().GetArraySize());

    const FStaticMeshConstAttributes Attributes(Description);
    const TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
    if (!Positions.IsValid())
    {
        return Spans;
    }

    for (const FTriangleID TriangleID : Description.Triangles().GetElementIDs())
    {
        const FPolygonGroupID GroupID = Description.GetTrianglePolygonGroup(TriangleID);
        if (!Spans.IsValidIndex(GroupID.GetValue()))
        {
            continue;
        }

        // Attributed by the triangle's own centroid rather than by any single corner: the two
        // regions are 500 units apart and no triangle in this probe spans them, so the centroid is
        // an exact classifier here and cannot be fooled by a shared vertex.
        float SumX = 0.0f;
        int32 Corners = 0;
        for (const FVertexID VertexID : Description.GetTriangleVertices(TriangleID))
        {
            SumX += Positions[VertexID].X;
            ++Corners;
        }
        if (Corners == 0)
        {
            continue;
        }

        FPwModifierMat_GroupSpan& Span = Spans[GroupID.GetValue()];
        ++Span.TriangleCount;
        if (SumX / Corners < SplitX)
        {
            ++Span.TrianglesBelowSplit;
        }
        else
        {
            ++Span.TrianglesAboveSplit;
        }
    }

    return Spans;
}

// Part `a` tags `Alpha` and is the FIRST part, so it owns slot 0 - the slot an untagged sweep used
// to fall to. Part `b` tags `Beta` and sweeps a bar past it. `allow_floating` because the bar does
// not touch the box and the isolation warning is not what this test is about.
//
// The rod runs along world +X because a sweep advances along each frame's local +X and every frame
// here is left at rotation (0, 0, 0); the profile lands in the frame's local YZ plane.
const TCHAR* PwModifierMat_TwoPartProbe()
{
    return
        TEXT("pwmodel 0\n")
        TEXT("materials {\n")
        TEXT("    Alpha = \"/Engine/EngineMaterials/WorldGridMaterial\"\n")
        TEXT("    Beta  = \"/Engine/EngineMaterials/DefaultMaterial\"\n")
        TEXT("}\n")
        TEXT("part a {\n")
        TEXT("    box size=(100, 100, 100) at=(0, 0, 0) material=\"Alpha\"\n")
        TEXT("}\n")
        TEXT("part b allow_floating=true {\n")
        TEXT("    box size=(100, 100, 100) at=(600, 0, 0) material=\"Beta\"\n")
        TEXT("    sweep profile=[(20, -20), (20, 20), (-20, 20), (-20, -20)] ")
        TEXT("path=[(700, 0, 0, 0, 0, 0), (900, 0, 0, 0, 0, 0)] cap=true\n")
        TEXT("}\n");
}
}

// ============================================================================
// Modifier output takes the slot of the geometry it modifies
// ============================================================================
//
// The behavioural half, and the one no field of the compile result can stand in for: the slot
// count, the slot names, the triangle counts, the bounds and the health block are IDENTICAL
// between a build that puts the swept rod on `Beta` and one that puts it on `Alpha`. Only the
// asset's sections differ, which is why this test bakes.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModifierMaterialInheritsTheModifiedGeometryTest,
    "PinWright.Model.MaterialSlots.ModifierOutputTakesTheSlotOfTheGeometryItModifies",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModifierMaterialInheritsTheModifiedGeometryTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = PwModifierMat_AssetPath(TEXT("SM_SweepInheritsPartSlot"));
    PwModifierMat_DeleteIfPresent(AssetPath);

    const FPwModelCompileResult Result =
        PwModifierMat_CompileTo(PwModifierMat_TwoPartProbe(), AssetPath);

    if (!TestTrue(*FString::Printf(TEXT("compile succeeded. %s"), *PwModifierMat_Describe(Result)),
            Result.bSuccess))
    {
        PwModifierMat_DeleteIfPresent(AssetPath);
        return false;
    }

    // Inheriting allocates nothing: the id is already in the table. Two tags, two slots, whether
    // or not the sweep is there at all.
    TestEqual(*FString::Printf(TEXT("the two tags produce two slots and the sweep adds none. %s"),
            *PwModifierMat_Describe(Result)), Result.MaterialSlots, 2);

    UStaticMesh* Mesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(AssetPath));
    if (!Mesh)
    {
        AddError(FString::Printf(TEXT("expected a UStaticMesh at %s. %s"),
            *AssetPath, *PwModifierMat_Describe(Result)));
        PwModifierMat_DeleteIfPresent(AssetPath);
        return false;
    }

    FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});

    const FMeshDescription* Baked = Mesh->GetMeshDescription(0);
    if (!Baked)
    {
        AddError(TEXT("the baked asset has no LOD0 mesh description"));
        PwModifierMat_DeleteIfPresent(AssetPath);
        return false;
    }

    const TArray<FPwModifierMat_GroupSpan> Spans =
        PwModifierMat_MeasureGroups(*Baked, PwModifierMat_PartSplitX);

    int32 PopulatedGroups = 0;
    int32 GroupsSpanningBothParts = 0;
    int32 LargestGroupConfinedToPartB = 0;
    for (const FPwModifierMat_GroupSpan& Span : Spans)
    {
        if (Span.TriangleCount == 0)
        {
            continue;
        }
        ++PopulatedGroups;
        if (Span.TrianglesBelowSplit > 0 && Span.TrianglesAboveSplit > 0)
        {
            ++GroupsSpanningBothParts;
        }
        else if (Span.TrianglesAboveSplit > 0)
        {
            LargestGroupConfinedToPartB =
                FMath::Max(LargestGroupConfinedToPartB, Span.TriangleCount);
        }
    }

    TestEqual(TEXT("the baked mesh carries one section per tagged slot"), PopulatedGroups, 2);

    // THE DEFECT. With the swept rod on slot 0, the section holding part `a`'s box also holds
    // geometry 700 units away in part `b` - one section, two parts, two materials' worth of
    // geometry on one of them.
    TestEqual(*FString::Printf(
            TEXT("no material section holds geometry from both parts. %s"),
            *PwModifierMat_Describe(Result)),
        GroupsSpanningBothParts, 0);

    // And the positive half: part `b`'s own section carries the rod as well as its box, so the
    // rod did not merely avoid `Alpha` - it joined `Beta`. A box alone is 12 triangles.
    TestTrue(*FString::Printf(
            TEXT("part b's section carries the swept rod as well as its box (%d triangles). %s"),
            LargestGroupConfinedToPartB, *PwModifierMat_Describe(Result)),
        LargestGroupConfinedToPartB > 12);

    PwModifierMat_DeleteIfPresent(AssetPath);
    return true;
}

// ============================================================================
// `material=` is authorable on the appending modifiers
// ============================================================================
//
// The other half of the fix. Inheritance answers the untagged case; this is how an author says
// something other than what the part already carries. Written the way the format already spells
// material tagging everywhere else - a slot NAME resolved through the model-wide table - which is
// the same parameter `append_triangle` gained for the same reason.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModifierMaterialTagIsAuthorableTest,
    "PinWright.Model.MaterialSlots.ModifierMaterialTagResolvesThroughTheSlotTable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModifierMaterialTagIsAuthorableTest::RunTest(const FString& Parameters)
{
    // `Gamma` is named by the sweep alone. Before this, `material=` on the op was an unknown
    // parameter and the compile stopped at stage 1.
    const FPwModelCompileResult Tagged = PwModifierMat_Validate(
        TEXT("pwmodel 0\n")
        TEXT("materials {\n")
        TEXT("    Alpha = \"/Engine/EngineMaterials/WorldGridMaterial\"\n")
        TEXT("    Beta  = \"/Engine/EngineMaterials/DefaultMaterial\"\n")
        TEXT("    Gamma = \"/Engine/BasicShapes/BasicShapeMaterial\"\n")
        TEXT("}\n")
        TEXT("part a {\n")
        TEXT("    box size=(100, 100, 100) at=(0, 0, 0) material=\"Alpha\"\n")
        TEXT("}\n")
        TEXT("part b allow_floating=true {\n")
        TEXT("    box size=(100, 100, 100) at=(600, 0, 0) material=\"Beta\"\n")
        TEXT("    sweep material=\"Gamma\" profile=[(20, -20), (20, 20), (-20, 20), (-20, -20)] ")
        TEXT("path=[(700, 0, 0, 0, 0, 0), (900, 0, 0, 0, 0, 0)] cap=true\n")
        TEXT("}\n"));

    if (!TestTrue(*FString::Printf(TEXT("a sweep carrying material= compiles. %s"),
            *PwModifierMat_Describe(Tagged)), Tagged.bSuccess))
    {
        return false;
    }

    if (TestEqual(*FString::Printf(TEXT("the tag opens its own slot. %s"),
            *PwModifierMat_Describe(Tagged)), Tagged.MaterialSlotList.Num(), 3))
    {
        // First-use order across the whole document, unchanged by the tag arriving from a
        // modifier rather than from a generator.
        TestEqual(TEXT("part a's tag keeps index 0"),
            Tagged.MaterialSlotList[0].Name, FString(TEXT("Alpha")));
        TestEqual(TEXT("part b's tag keeps index 1"),
            Tagged.MaterialSlotList[1].Name, FString(TEXT("Beta")));
        TestEqual(TEXT("the sweep's own tag takes the next index"),
            Tagged.MaterialSlotList[2].Name, FString(TEXT("Gamma")));
    }

    // A tag the `materials` block does not bind is reported exactly as it is on a generator - the
    // parser counts any part-level op carrying `material=`, so wiring the parameter in must not
    // have created a slot reference nothing can see.
    const FPwModelCompileResult Unbound = PwModifierMat_Validate(
        TEXT("pwmodel 0\n")
        TEXT("materials {\n")
        TEXT("    Beta = \"/Engine/EngineMaterials/DefaultMaterial\"\n")
        TEXT("}\n")
        TEXT("part b allow_floating=true {\n")
        TEXT("    box size=(100, 100, 100) at=(600, 0, 0) material=\"Beta\"\n")
        TEXT("    sweep material=\"Gamma\" profile=[(20, -20), (20, 20), (-20, 20), (-20, -20)] ")
        TEXT("path=[(700, 0, 0, 0, 0, 0), (900, 0, 0, 0, 0, 0)] cap=true\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("the unbound-tag document still compiles. %s"),
        *PwModifierMat_Describe(Unbound)), Unbound.bSuccess);
    TestEqual(*FString::Printf(TEXT("and the unbound slot the sweep named is reported. %s"),
            *PwModifierMat_Describe(Unbound)),
        PwModifierMat_WithCode(Unbound, PwModelDiagnosticCodes::PWMODEL_UNBOUND_MATERIAL).Num(), 1);

    return true;
}

// ============================================================================
// Ambiguous inheritance is reported, and unambiguous inheritance is silent
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModifierMaterialAmbiguityIsReportedTest,
    "PinWright.Model.MaterialSlots.AmbiguousModifierOutputIsReported",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModifierMaterialAmbiguityIsReportedTest::RunTest(const FString& Parameters)
{
    // Two slots in one part, so there is no single slot to inherit. The sphere is the larger of
    // the two by triangle count (12*(subdivisions-1)^2 = 108 against a box's 12), so the majority
    // rule has a decided answer rather than a tie, and the message must name it.
    const FPwModelCompileResult Mixed = PwModifierMat_Validate(
        TEXT("pwmodel 0\n")                                                          // 1
        TEXT("materials {\n")                                                        // 2
        TEXT("    Alpha = \"/Engine/EngineMaterials/WorldGridMaterial\"\n")           // 3
        TEXT("    Beta  = \"/Engine/EngineMaterials/DefaultMaterial\"\n")             // 4
        TEXT("}\n")                                                                  // 5
        TEXT("part mixed allow_floating=true {\n")                                    // 6
        TEXT("    box size=(100, 100, 100) at=(0, 0, 0) material=\"Alpha\"\n")        // 7
        TEXT("    sphere radius=50 subdivisions=4 at=(300, 0, 0) material=\"Beta\"\n") // 8
        TEXT("    sweep profile=[(20, -20), (20, 20), (-20, 20), (-20, -20)] ")        // 9
        TEXT("path=[(700, 0, 0, 0, 0, 0), (900, 0, 0, 0, 0, 0)] cap=true\n")
        TEXT("}\n"));                                                                // 10

    TestTrue(*FString::Printf(TEXT("the ambiguous document still compiles - it is a warning. %s"),
        *PwModifierMat_Describe(Mixed)), Mixed.bSuccess);

    const TArray<FPwDiagnostic> Reported = PwModifierMat_WithCode(
        Mixed, PwModelDiagnosticCodes::PWMODEL_MODIFIER_MATERIAL_AMBIGUOUS);
    if (!TestEqual(*FString::Printf(TEXT("exactly one ambiguity diagnostic. %s"),
            *PwModifierMat_Describe(Mixed)), Reported.Num(), 1))
    {
        return false;
    }

    const FPwDiagnostic& Diagnostic = Reported[0];
    TestEqual(TEXT("reported as a warning, not an error"), Diagnostic.Severity, EPwSeverity::Warning);
    TestEqual(TEXT("anchored on the sweep, not on the part or the materials block"),
        Diagnostic.Line, 9);
    TestEqual(TEXT("names the part the sweep sits in"),
        Diagnostic.ScopeName, FString(TEXT("mixed")));

    // The four facts a caller has to act on: which op, that there was no single answer, which
    // slots were on offer, and which one the compiler took.
    TestTrue(*FString::Printf(TEXT("names the op. %s"), *Diagnostic.Message),
        Diagnostic.Message.Contains(TEXT("'sweep'"), ESearchCase::CaseSensitive));
    TestTrue(*FString::Printf(TEXT("names both candidate slots. %s"), *Diagnostic.Message),
        Diagnostic.Message.Contains(TEXT("'Alpha'"), ESearchCase::CaseSensitive)
        && Diagnostic.Message.Contains(TEXT("'Beta'"), ESearchCase::CaseSensitive));
    TestTrue(*FString::Printf(TEXT("names the remedy. %s"), *Diagnostic.Message),
        Diagnostic.Message.Contains(TEXT("material=\"<Slot>\""), ESearchCase::CaseSensitive));

    // Tagging the sweep removes the ambiguity outright: there is nothing left to inherit.
    const FPwModelCompileResult Tagged = PwModifierMat_Validate(
        TEXT("pwmodel 0\n")
        TEXT("materials {\n")
        TEXT("    Alpha = \"/Engine/EngineMaterials/WorldGridMaterial\"\n")
        TEXT("    Beta  = \"/Engine/EngineMaterials/DefaultMaterial\"\n")
        TEXT("}\n")
        TEXT("part mixed allow_floating=true {\n")
        TEXT("    box size=(100, 100, 100) at=(0, 0, 0) material=\"Alpha\"\n")
        TEXT("    sphere radius=50 subdivisions=4 at=(300, 0, 0) material=\"Beta\"\n")
        TEXT("    sweep material=\"Beta\" profile=[(20, -20), (20, 20), (-20, 20), (-20, -20)] ")
        TEXT("path=[(700, 0, 0, 0, 0, 0), (900, 0, 0, 0, 0, 0)] cap=true\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("the tagged variant compiles. %s"),
        *PwModifierMat_Describe(Tagged)), Tagged.bSuccess);
    TestEqual(*FString::Printf(TEXT("a tagged sweep raises no ambiguity warning. %s"),
            *PwModifierMat_Describe(Tagged)),
        PwModifierMat_WithCode(Tagged, PwModelDiagnosticCodes::PWMODEL_MODIFIER_MATERIAL_AMBIGUOUS).Num(), 0);

    // And the ordinary case stays silent: one slot in front of the sweep is one answer. A warning
    // here would fire on the shape this format recommends for a rail on a post, and the code would
    // be ignored wholesale.
    const FPwModelCompileResult SingleSlot = PwModifierMat_Validate(PwModifierMat_TwoPartProbe());

    TestTrue(*FString::Printf(TEXT("the single-slot probe compiles. %s"),
        *PwModifierMat_Describe(SingleSlot)), SingleSlot.bSuccess);
    TestEqual(*FString::Printf(TEXT("inheriting one unambiguous slot is silent. %s"),
            *PwModifierMat_Describe(SingleSlot)),
        PwModifierMat_WithCode(SingleSlot,
            PwModelDiagnosticCodes::PWMODEL_MODIFIER_MATERIAL_AMBIGUOUS).Num(), 0);

    return true;
}
