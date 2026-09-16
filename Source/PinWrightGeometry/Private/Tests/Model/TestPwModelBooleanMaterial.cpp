// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the material slot the faces a BOOLEAN or a BEVEL creates land on.
//
// The defect these pin: every op inside a boolean block skipped material tagging outright, so its
// triangles kept material ID 0 and GeometryCore carried that 0 straight into the result. ID 0 is
// NOT a neutral default - the slot table is model-wide and allocated in first-use order, so 0 is
// whichever slot the FIRST PART IN THE DOCUMENT tagged. Three routes onto it, all measured on a
// real 18-part rifle where 18,559 of 21,420 triangles shipped on the first part's material:
//
//   `union { gen material="X" }` - ALL of the unioned geometry, with the author's own tag parsed
//     and discarded, because `material=` inside the block never reached ResolveSlot.
//   `subtract { tool }`          - the walls the cut opened, which come from the tool surface and
//     had no op of their own to tag; the base geometry kept its slot, so the fault looked partial.
//   `bevel`                      - every new chamfer face, because the op defaulted to
//     `infer_material_id=false, material_id=0`.
//
// Clean compile, `errors: 0`, and no field of the response moves: the slot COUNT, the slot names,
// `materialSlotList`, the triangle counts, the bounds and the whole health block are identical
// between a correct build and a broken one. Only which section a triangle renders in differs,
// which is why the behavioural tests here BAKE and measure the asset's polygon groups.
//
// What each test would let through if it were reverted:
//
//  - A BOOLEAN BLOCK'S GEOMETRY KEEPS ITS OWN SLOT. Asserted as "no material section holds
//    geometry from two different parts", which needs no assumption about how a material ID becomes
//    a polygon group id through the static-mesh build.
//  - A CUT'S WALLS TAKE THE SLOT THE OP NAMES. The only spelling for them - the walls have no op
//    of their own - so this is also the test that `material=` on a boolean is honoured rather than
//    merely accepted.
//  - BEVEL FACES JOIN THE SURFACE THEY CHAMFER instead of the first part's material.
//  - `material=` IS AUTHORABLE ON A BOOLEAN and `color=` still is not. Before this, both were
//    PWMODEL_MATERIAL_ON_BOOLEAN and the compile stopped at stage 1.
//  - AMBIGUITY IS REPORTED. Cutting into geometry that carries more than one slot has no single
//    answer, and picking one in silence is the same class of defect as taking slot 0 in silence.
//  - A TAG THAT REACHES NO FACE IS REPORTED, rather than shipping an empty section.
//  - THE ORDINARY DOCUMENT STAYS SILENT. A cut into single-slot geometry must raise nothing - a
//    warning there would be turned off wholesale and take the real case with it.
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

const TCHAR* const PwBoolMat_OutputRoot = TEXT("/Game/PinWrightTests/PwModelBooleanMaterial");

// The X coordinate that separates part `a`'s geometry from part `b`'s in the two-part probes
// below. Part `a` spans [-50, 50]; everything part `b` builds sits past x 500.
constexpr float PwBoolMat_PartSplitX = 250.0f;

FString PwBoolMat_AssetPath(const TCHAR* Leaf)
{
    return FString::Printf(TEXT("%s/%s"), PwBoolMat_OutputRoot, Leaf);
}

// CleanupTestAsset, never UEditorAssetLibrary::DeleteAsset: that routes through
// ObjectTools::ForceDeleteObjects, whose full-object-graph reference walk costs seconds per call
// against a live editor's ~300k UObjects. The discard renames the asset AND its package into
// /Transient, removes the registry entry and deletes the .uasset, which is everything the
// pre-compile callers need to leave AssetCreatePolicy::Resolve on its Create branch.
void PwBoolMat_DeleteIfPresent(const FString& AssetPath)
{
    if (UEditorAssetLibrary::DoesAssetExist(AssetPath))
    {
        CleanupTestAsset(AssetPath);
    }
}

FPwModelCompileResult PwBoolMat_Validate(const TCHAR* Source)
{
    FPwModelCompileOptions Options;
    Options.bValidateOnly = true;
    Options.bSave = false;
    return FPwModelCompiler::Compile(Source, Options);
}

FPwModelCompileResult PwBoolMat_CompileTo(const TCHAR* Source, const FString& AssetPath)
{
    FPwModelCompileOptions Options;
    Options.OutputAssetPath = AssetPath;
    Options.SourcePath = TEXT("Tests/PwModelBooleanMaterial.pwmodel");
    Options.bOverwrite = true;
    Options.bSave = true;
    return FPwModelCompiler::Compile(Source, Options);
}

FString PwBoolMat_Describe(const FPwModelCompileResult& Result)
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

TArray<FPwDiagnostic> PwBoolMat_WithCode(const FPwModelCompileResult& Result, const TCHAR* Code)
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
// its slot is called. That indirection is the point: a section whose triangles straddle two parts
// is the defect, and saying so needs no claim about how a material ID becomes a polygon group id
// or a slot name through the static-mesh build.
struct FPwBoolMat_GroupSpan
{
    int32 TriangleCount = 0;
    int32 TrianglesBelowSplit = 0;
    int32 TrianglesAboveSplit = 0;

    // Radial distance from the Z axis, over triangle centroids. Used by the bore probe, where the
    // question is whether a section lives on the cut wall or on the block's outside.
    double MinRadius = TNumericLimits<double>::Max();
    double MaxRadius = 0.0;
};

// Loads the baked LOD0 mesh description and measures one span per polygon group. Returns an empty
// array and reports the failure on Test when the asset cannot be read.
TArray<FPwBoolMat_GroupSpan> PwBoolMat_MeasureBakedGroups(FAutomationTestBase& Test,
                                                          const FString& AssetPath, float SplitX)
{
    TArray<FPwBoolMat_GroupSpan> Spans;

    UStaticMesh* Mesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(AssetPath));
    if (!Mesh)
    {
        Test.AddError(FString::Printf(TEXT("expected a UStaticMesh at %s"), *AssetPath));
        return Spans;
    }

    FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});

    const FMeshDescription* Baked = Mesh->GetMeshDescription(0);
    if (!Baked)
    {
        Test.AddError(TEXT("the baked asset has no LOD0 mesh description"));
        return Spans;
    }

    Spans.SetNum(Baked->PolygonGroups().GetArraySize());

    const FStaticMeshConstAttributes Attributes(*Baked);
    const TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
    if (!Positions.IsValid())
    {
        Test.AddError(TEXT("the baked asset carries no vertex positions"));
        return Spans;
    }

    for (const FTriangleID TriangleID : Baked->Triangles().GetElementIDs())
    {
        const FPolygonGroupID GroupID = Baked->GetTrianglePolygonGroup(TriangleID);
        if (!Spans.IsValidIndex(GroupID.GetValue()))
        {
            continue;
        }

        // Attributed by the triangle's own centroid rather than by any single corner: the probes
        // keep their regions far apart, so the centroid is an exact classifier here and cannot be
        // fooled by a vertex shared across a boundary.
        FVector3f Sum = FVector3f::ZeroVector;
        int32 Corners = 0;
        for (const FVertexID VertexID : Baked->GetTriangleVertices(TriangleID))
        {
            Sum += Positions[VertexID];
            ++Corners;
        }
        if (Corners == 0)
        {
            continue;
        }

        const FVector3f Centroid = Sum / static_cast<float>(Corners);

        FPwBoolMat_GroupSpan& Span = Spans[GroupID.GetValue()];
        ++Span.TriangleCount;
        if (Centroid.X < SplitX)
        {
            ++Span.TrianglesBelowSplit;
        }
        else
        {
            ++Span.TrianglesAboveSplit;
        }

        const double Radius = FMath::Sqrt(
            static_cast<double>(Centroid.X) * Centroid.X + static_cast<double>(Centroid.Y) * Centroid.Y);
        Span.MinRadius = FMath::Min(Span.MinRadius, Radius);
        Span.MaxRadius = FMath::Max(Span.MaxRadius, Radius);
    }

    return Spans;
}

int32 PwBoolMat_CountSectionsSpanningBothParts(const TArray<FPwBoolMat_GroupSpan>& Spans)
{
    int32 Count = 0;
    for (const FPwBoolMat_GroupSpan& Span : Spans)
    {
        if (Span.TrianglesBelowSplit > 0 && Span.TrianglesAboveSplit > 0)
        {
            ++Count;
        }
    }
    return Count;
}

int32 PwBoolMat_CountPopulatedSections(const TArray<FPwBoolMat_GroupSpan>& Spans)
{
    int32 Count = 0;
    for (const FPwBoolMat_GroupSpan& Span : Spans)
    {
        if (Span.TriangleCount > 0)
        {
            ++Count;
        }
    }
    return Count;
}
}

// ============================================================================
// A boolean block's geometry keeps its own slot
// ============================================================================
//
// Part `a` tags `Alpha` and is the FIRST part, so it owns slot 0 - the slot a boolean's output used
// to fall to. Part `b` tags `Beta` and unions a second box onto its own, tagging that box `Beta`
// too. The tag was parsed and discarded, so the unioned box shipped on `Alpha`: one section holding
// geometry 600 units apart in two different parts.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwBooleanBlockGeometryKeepsItsOwnSlotTest,
    "PinWright.Model.MaterialSlots.BooleanBlockGeometryKeepsItsOwnSlot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwBooleanBlockGeometryKeepsItsOwnSlotTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = PwBoolMat_AssetPath(TEXT("SM_UnionBlockKeepsItsSlot"));
    PwBoolMat_DeleteIfPresent(AssetPath);

    const FPwModelCompileResult Result = PwBoolMat_CompileTo(
        TEXT("pwmodel 0\n")
        TEXT("materials {\n")
        TEXT("    Alpha = \"/Engine/EngineMaterials/WorldGridMaterial\"\n")
        TEXT("    Beta  = \"/Engine/EngineMaterials/DefaultMaterial\"\n")
        TEXT("}\n")
        TEXT("part a {\n")
        TEXT("    box size=(100, 100, 100) at=(0, 0, 0) material=\"Alpha\"\n")
        TEXT("}\n")
        TEXT("part b {\n")
        TEXT("    box size=(100, 100, 100) at=(600, 0, 0) material=\"Beta\"\n")
        TEXT("    union {\n")
        TEXT("        box size=(100, 100, 100) at=(650, 0, 50) material=\"Beta\"\n")
        TEXT("    }\n")
        TEXT("}\n"),
        AssetPath);

    if (!TestTrue(*FString::Printf(TEXT("compile succeeded. %s"), *PwBoolMat_Describe(Result)),
            Result.bSuccess))
    {
        PwBoolMat_DeleteIfPresent(AssetPath);
        return false;
    }

    // Tagging inside a block allocates nothing new when the name is already in the table: two
    // names, two slots, whether or not the union is there at all.
    TestEqual(*FString::Printf(TEXT("the two names produce two slots and the union adds none. %s"),
            *PwBoolMat_Describe(Result)), Result.MaterialSlots, 2);

    const TArray<FPwBoolMat_GroupSpan> Spans =
        PwBoolMat_MeasureBakedGroups(*this, AssetPath, PwBoolMat_PartSplitX);

    TestEqual(TEXT("the baked mesh carries one section per tagged slot"),
        PwBoolMat_CountPopulatedSections(Spans), 2);

    // THE DEFECT. With the unioned box on slot 0, the section holding part `a`'s box also holds
    // geometry 600 units away in part `b`.
    TestEqual(*FString::Printf(TEXT("no material section holds geometry from both parts. %s"),
            *PwBoolMat_Describe(Result)),
        PwBoolMat_CountSectionsSpanningBothParts(Spans), 0);

    PwBoolMat_DeleteIfPresent(AssetPath);
    return true;
}

// ============================================================================
// A cut's walls take the slot the op names
// ============================================================================
//
// The walls a `subtract` opens are the one surface in the format with no op of its own to tag, so
// `material=` on the boolean is their only spelling. A through bore down Z: every wall triangle
// sits at the bore radius, every triangle of the block sits further out, so "which section is the
// wall in" is answerable from geometry alone.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSubtractCutWallsTakeTheOpsNamedSlotTest,
    "PinWright.Model.MaterialSlots.SubtractCutWallsTakeTheOpsNamedSlot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwSubtractCutWallsTakeTheOpsNamedSlotTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = PwBoolMat_AssetPath(TEXT("SM_BoreWallsTakeTheirSlot"));
    PwBoolMat_DeleteIfPresent(AssetPath);

    const FPwModelCompileResult Result = PwBoolMat_CompileTo(
        TEXT("pwmodel 0\n")
        TEXT("materials {\n")
        TEXT("    Shell = \"/Engine/EngineMaterials/WorldGridMaterial\"\n")
        TEXT("    Bore  = \"/Engine/EngineMaterials/DefaultMaterial\"\n")
        TEXT("}\n")
        TEXT("part body {\n")
        TEXT("    box size=(80, 80, 40) material=\"Shell\"\n")
        TEXT("    subtract material=\"Bore\" {\n")
        TEXT("        cylinder radius=10 height=200 segments=24\n")
        TEXT("    }\n")
        TEXT("}\n"),
        AssetPath);

    if (!TestTrue(*FString::Printf(TEXT("compile succeeded. %s"), *PwBoolMat_Describe(Result)),
            Result.bSuccess))
    {
        PwBoolMat_DeleteIfPresent(AssetPath);
        return false;
    }

    TestEqual(*FString::Printf(TEXT("the shell and the bore are two slots. %s"),
            *PwBoolMat_Describe(Result)), Result.MaterialSlots, 2);

    // A tag that IS honoured raises nothing: the walls exist and carry it.
    TestEqual(*FString::Printf(TEXT("an honoured tag on a boolean is not reported unused. %s"),
            *PwBoolMat_Describe(Result)),
        PwBoolMat_WithCode(Result, PwModelDiagnosticCodes::PWMODEL_BOOLEAN_MATERIAL_UNUSED).Num(), 0);

    // Single-slot geometry has an unambiguous answer, so nothing is reported there either.
    TestEqual(*FString::Printf(TEXT("a cut into single-slot geometry is not ambiguous. %s"),
            *PwBoolMat_Describe(Result)),
        PwBoolMat_WithCode(Result, PwModelDiagnosticCodes::PWMODEL_BOOLEAN_MATERIAL_AMBIGUOUS).Num(), 0);

    const TArray<FPwBoolMat_GroupSpan> Spans =
        PwBoolMat_MeasureBakedGroups(*this, AssetPath, PwBoolMat_PartSplitX);

    TestEqual(TEXT("the baked mesh carries one section per tagged slot"),
        PwBoolMat_CountPopulatedSections(Spans), 2);

    // THE DEFECT. The walls used to land on slot 0 - which here is `Shell`, the block's own slot,
    // so no count and no name can see it. What separates them is the radius of a triangle's own
    // centroid: every wall triangle stands on the r = 10 bore (centroids ~9.9 at 24 segments),
    // while the nearest triangle of the block's own surface is a poked cap fan reaching from that
    // bore out to the 40-unit half-width of the face, whose centroid cannot fall below ~20. A
    // section holding both is the walls having been merged into the block's.
    constexpr double BoreOnlyRadius = 13.0;
    constexpr double ShellReachRadius = 30.0;

    int32 WallSections = 0;
    int32 SectionsHoldingBothWallAndShell = 0;
    for (const FPwBoolMat_GroupSpan& Span : Spans)
    {
        if (Span.TriangleCount == 0)
        {
            continue;
        }
        if (Span.MinRadius < BoreOnlyRadius && Span.MaxRadius > ShellReachRadius)
        {
            ++SectionsHoldingBothWallAndShell;
        }
        else if (Span.MaxRadius < BoreOnlyRadius)
        {
            ++WallSections;
        }
    }

    TestEqual(*FString::Printf(
            TEXT("no section holds both the bore wall and the block's outer surface. %s"),
            *PwBoolMat_Describe(Result)),
        SectionsHoldingBothWallAndShell, 0);
    TestEqual(*FString::Printf(TEXT("the bore wall has a section of its own. %s"),
            *PwBoolMat_Describe(Result)), WallSections, 1);

    PwBoolMat_DeleteIfPresent(AssetPath);
    return true;
}

// ============================================================================
// Bevel faces join the surface they chamfer
// ============================================================================
//
// The same two-part probe: part `a` owns slot 0, part `b` bevels its own box. `bevel` defaulted to
// `infer_material_id=false, material_id=0`, so 32 of a bevelled cube's 44 triangles shipped in part
// `a`'s material while the 12 original face triangles kept part `b`'s - which is why the fault read
// as "the material looks slightly wrong along the edges" rather than as an obvious break.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwBevelFacesJoinTheSurfaceTheyChamferTest,
    "PinWright.Model.MaterialSlots.BevelFacesJoinTheSurfaceTheyChamfer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwBevelFacesJoinTheSurfaceTheyChamferTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = PwBoolMat_AssetPath(TEXT("SM_BevelFacesJoinTheirSurface"));
    PwBoolMat_DeleteIfPresent(AssetPath);

    const FPwModelCompileResult Result = PwBoolMat_CompileTo(
        TEXT("pwmodel 0\n")
        TEXT("materials {\n")
        TEXT("    Alpha = \"/Engine/EngineMaterials/WorldGridMaterial\"\n")
        TEXT("    Beta  = \"/Engine/EngineMaterials/DefaultMaterial\"\n")
        TEXT("}\n")
        TEXT("part a {\n")
        TEXT("    box size=(100, 100, 100) at=(0, 0, 0) material=\"Alpha\"\n")
        TEXT("}\n")
        TEXT("part b {\n")
        TEXT("    box size=(100, 100, 100) at=(600, 0, 0) material=\"Beta\"\n")
        TEXT("    bevel distance=5\n")
        TEXT("}\n"),
        AssetPath);

    if (!TestTrue(*FString::Printf(TEXT("compile succeeded. %s"), *PwBoolMat_Describe(Result)),
            Result.bSuccess))
    {
        PwBoolMat_DeleteIfPresent(AssetPath);
        return false;
    }

    TestEqual(*FString::Printf(TEXT("bevelling allocates no slot of its own. %s"),
            *PwBoolMat_Describe(Result)), Result.MaterialSlots, 2);

    const TArray<FPwBoolMat_GroupSpan> Spans =
        PwBoolMat_MeasureBakedGroups(*this, AssetPath, PwBoolMat_PartSplitX);

    TestEqual(TEXT("the baked mesh carries one section per tagged slot"),
        PwBoolMat_CountPopulatedSections(Spans), 2);

    TestEqual(*FString::Printf(
            TEXT("no material section holds the chamfer faces alongside the other part. %s"),
            *PwBoolMat_Describe(Result)),
        PwBoolMat_CountSectionsSpanningBothParts(Spans), 0);

    PwBoolMat_DeleteIfPresent(AssetPath);
    return true;
}

// ============================================================================
// The language surface: material= is authorable, color= still is not
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwBooleanTakesTheSlotTagAndStillRefusesColorTest,
    "PinWright.Model.MaterialSlots.BooleanTakesTheSlotTagAndStillRefusesColor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwBooleanTakesTheSlotTagAndStillRefusesColorTest::RunTest(const FString& Parameters)
{
    // A boolean creates FACES, which take a slot. Before this, writing the tag stopped the compile
    // at stage 1 with PWMODEL_MATERIAL_ON_BOOLEAN and there was no spelling at all for a cut's
    // walls.
    const FPwModelCompileResult Tagged = PwBoolMat_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(80, 80, 40)\n")
        TEXT("    subtract material=\"Bore\" {\n")
        TEXT("        cylinder radius=10 height=200\n")
        TEXT("    }\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("material= on a boolean compiles. %s"),
            *PwBoolMat_Describe(Tagged)), Tagged.bSuccess);
    TestEqual(*FString::Printf(TEXT("material= on a boolean is no longer refused. %s"),
            *PwBoolMat_Describe(Tagged)),
        PwBoolMat_WithCode(Tagged, PwModelDiagnosticCodes::PWMODEL_MATERIAL_ON_BOOLEAN).Num(), 0);

    // A boolean creates no VERTICES: every vertex in the result comes from one of the two operands
    // and already carries the colour its generator gave it, so a scalar colour here could only
    // overwrite one the author wrote. The asymmetry is the decision, not an oversight.
    const FPwModelCompileResult Coloured = PwBoolMat_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(80, 80, 40)\n")
        TEXT("    subtract color=(1, 0, 0, 1) {\n")
        TEXT("        cylinder radius=10 height=200\n")
        TEXT("    }\n")
        TEXT("}\n"));

    TestFalse(*FString::Printf(TEXT("color= on a boolean is still refused. %s"),
            *PwBoolMat_Describe(Coloured)), Coloured.bSuccess);

    const TArray<FPwDiagnostic> ColourRefusals =
        PwBoolMat_WithCode(Coloured, PwModelDiagnosticCodes::PWMODEL_MATERIAL_ON_BOOLEAN);
    if (TestEqual(*FString::Printf(TEXT("color= on a boolean names its own code. %s"),
            *PwBoolMat_Describe(Coloured)), ColourRefusals.Num(), 1))
    {
        // The remedy has to be in the sentence: the message used to send the author away to model
        // the interior as its own part, which is no longer the answer for either parameter.
        TestTrue(TEXT("the message names set_vertex_color as what was meant"),
            ColourRefusals[0].Message.Contains(TEXT("set_vertex_color")));
        TestTrue(TEXT("the message says material= IS accepted"),
            ColourRefusals[0].Message.Contains(TEXT("material=")));
    }

    // `material=` and the raw `material_id=` name the same triangles two ways, and the check is
    // keyed on the parameter names rather than on an op list - so `bevel`, which gained the slot
    // tag alongside the raw index it already had, inherits the refusal.
    const FPwModelCompileResult BothOnBevel = PwBoolMat_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(80, 80, 40)\n")
        TEXT("    bevel distance=2 material=\"Chamfer\" material_id=3\n")
        TEXT("}\n"));

    TestEqual(*FString::Printf(TEXT("both material spellings on one bevel is refused. %s"),
            *PwBoolMat_Describe(BothOnBevel)),
        PwBoolMat_WithCode(BothOnBevel, PwModelDiagnosticCodes::PWMODEL_MATERIAL_ID_CONFLICT).Num(), 1);

    return true;
}

// ============================================================================
// Ambiguity, and the tag that reaches nothing
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwBooleanMaterialAmbiguityIsReportedTest,
    "PinWright.Model.MaterialSlots.BooleanMaterialAmbiguityIsReported",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwBooleanMaterialAmbiguityIsReportedTest::RunTest(const FString& Parameters)
{
    // Two slots in front of the cut, so the walls have no single slot to inherit. The op takes the
    // slot most of that geometry is on - deterministic, and the least surprising of the available
    // guesses - and says so.
    const FPwModelCompileResult Mixed = PwBoolMat_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(80, 80, 40) material=\"Shell\"\n")
        TEXT("    box size=(20, 20, 60) at=(30, 30, 0) material=\"Trim\"\n")
        TEXT("    subtract {\n")
        TEXT("        cylinder radius=10 height=200\n")
        TEXT("    }\n")
        TEXT("}\n"));

    const TArray<FPwDiagnostic> Ambiguous =
        PwBoolMat_WithCode(Mixed, PwModelDiagnosticCodes::PWMODEL_BOOLEAN_MATERIAL_AMBIGUOUS);
    if (TestEqual(*FString::Printf(TEXT("a cut into two-slot geometry is reported. %s"),
            *PwBoolMat_Describe(Mixed)), Ambiguous.Num(), 1))
    {
        // Both remedies, because they are different ops: the boolean's own tag names the walls,
        // and a generator's tag names that generator's surface.
        TestTrue(TEXT("the message names both slots it chose between"),
            Ambiguous[0].Message.Contains(TEXT("Shell")) && Ambiguous[0].Message.Contains(TEXT("Trim")));
        TestTrue(TEXT("the message names the remedy"),
            Ambiguous[0].Message.Contains(TEXT("material=")));
    }

    // The same document with the walls named raises nothing: the author answered the question.
    const FPwModelCompileResult Named = PwBoolMat_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(80, 80, 40) material=\"Shell\"\n")
        TEXT("    box size=(20, 20, 60) at=(30, 30, 0) material=\"Trim\"\n")
        TEXT("    subtract material=\"Shell\" {\n")
        TEXT("        cylinder radius=10 height=200\n")
        TEXT("    }\n")
        TEXT("}\n"));

    TestEqual(*FString::Printf(TEXT("naming the walls answers the question. %s"),
            *PwBoolMat_Describe(Named)),
        PwBoolMat_WithCode(Named, PwModelDiagnosticCodes::PWMODEL_BOOLEAN_MATERIAL_AMBIGUOUS).Num(), 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwBooleanMaterialTagThatReachesNoFaceIsReportedTest,
    "PinWright.Model.MaterialSlots.BooleanMaterialTagThatReachesNoFaceIsReported",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwBooleanMaterialTagThatReachesNoFaceIsReportedTest::RunTest(const FString& Parameters)
{
    // A TOOL SOLID THAT NEVER MEETS THE TARGET is the case that provably produces no face. In a
    // subtract the result holds the target's surface outside the tool plus the tool's surface
    // INSIDE the target; a sphere 500 units away has no surface inside the target, so `Ghost`
    // reaches nothing, the slot is opened for it anyway, and the asset would ship an empty section.
    // The block still changes the target through the cylinder, so this is not the disjoint-boolean
    // case PWMODEL_BOOLEAN_NO_EFFECT already refuses - the op works and one of its tools is dead.
    //
    // THE FIXTURE THIS REPLACES ASSERTED SOMETHING FALSE, and the reason is worth keeping: it used
    // `trim material="Cap" fill_holes=false` on the belief that trim discards its whole cutting
    // surface. It does not. GeometryOps::Trim maps keep_inside onto Subtract / Intersection and
    // dispatches the same ApplyMeshBoolean the other three booleans use
    // (GeometryOps_Boolean.cpp), so a trim's tool surface BECOMES the cut face - `Cap` landed on
    // the flat top the trim opened, which is correct behaviour and correctly not reported.
    //
    // The op itself is left untagged on purpose: a `material=` on the boolean would become the
    // slot every engine-created face takes, which is exactly what must NOT be confused with the
    // dead tool's slot.
    const FPwModelCompileResult Result = PwBoolMat_Validate(
        TEXT("pwmodel 0\n")
        TEXT("materials {\n")
        TEXT("    Shell = \"/Engine/EngineMaterials/WorldGridMaterial\"\n")
        TEXT("    Ghost = \"/Engine/EngineMaterials/DefaultMaterial\"\n")
        TEXT("}\n")
        TEXT("part body {\n")
        TEXT("    box size=(80, 80, 40) material=\"Shell\"\n")
        TEXT("    subtract {\n")
        TEXT("        cylinder radius=10 height=200\n")
        TEXT("        sphere radius=8 at=(500, 0, 0) material=\"Ghost\"\n")
        TEXT("    }\n")
        TEXT("}\n"));

    const TArray<FPwDiagnostic> Unused =
        PwBoolMat_WithCode(Result, PwModelDiagnosticCodes::PWMODEL_BOOLEAN_MATERIAL_UNUSED);
    if (TestEqual(*FString::Printf(TEXT("a tag no triangle carries is reported. %s"),
            *PwBoolMat_Describe(Result)), Unused.Num(), 1))
    {
        TestTrue(TEXT("the message names the slot that reached nothing"),
            Unused[0].Message.Contains(TEXT("Ghost")));
    }

    // And it names ONLY that one. `Shell` is carried by the block and by the walls the cylinder
    // opened, so a check that fired on every slot the op touched would be useless.
    if (Unused.Num() == 1)
    {
        TestFalse(*FString::Printf(TEXT("the slot the cut walls DO carry is not reported. %s"),
                *PwBoolMat_Describe(Result)),
            Unused[0].Message.Contains(TEXT("Shell")));
    }

    // Single-slot geometry in front of the cut, so there is nothing ambiguous to report either -
    // the two codes must not fire together on one op.
    TestEqual(*FString::Printf(TEXT("a cut into single-slot geometry is not ambiguous. %s"),
            *PwBoolMat_Describe(Result)),
        PwBoolMat_WithCode(Result, PwModelDiagnosticCodes::PWMODEL_BOOLEAN_MATERIAL_AMBIGUOUS).Num(), 0);

    return true;
}
