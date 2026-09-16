// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for FPwModelCompiler - source text to exactly one UStaticMesh.
//
// What each of these would let through if it were reverted:
//
//  - ONE FILE, ONE ASSET is the format's hardest invariant (docs/adr/0001-one-file-one-asset.md)
//    and the only thing standing between it and a per-part emitter is that nothing in the
//    compiler loops over parts creating assets. A three-part document producing three assets
//    would still look like a successful compile from the RPC response, because the response
//    reports one assetPath either way. ThreePartDocumentYieldsOneAsset checks the per-part
//    paths a per-part emitter would have written.
//  - SLOT IDENTITY IS THE NAME, model-wide. Two parts tagging "Shell" getting two slots is
//    invisible in every count except the slot count, and produces an asset whose second slot
//    silently renders with the default material.
//  - NOTHING IS CREATED ON FAILURE. With one asset this is trivially true - but only as long
//    as creation stays after validation. Move CreateStaticMesh one stage earlier and a
//    document whose second part fails leaves a half-model on disk with no diagnostic saying so.
//  - RESERVED BLOCKS report a reserved-construct diagnostic. `skeleton { }` parsing cleanly and then failing with
//    PWSRC_UNKNOWN_OP on its first inner line is exactly the experience reserving the keyword
//    was meant to prevent.
//  - UV CHANNELS ARE FILLED BEFORE THE MERGE, and the warning fires ONCE. A part with no
//    elements in a channel another part populates ships an untextured region with no error at
//    all: the merged mesh has elements in the channel, so the bake's MeshHasUsableUVs guard
//    does not fire, and only that part's triangles carry none. The warning test cannot see this
//    on its own - its `Filled` entry is appended unconditionally, so the projection can be
//    gutted to a no-op and every assertion still passes; FilledUVChannelsCarryRealUVsInTheAsset
//    is the one that reads the ELEMENTS out of the built asset.
//  - A UV OP THAT WROTE NOTHING IS A FAILURE. Every engine UV routine reports the "I did
//    nothing" case only through its GeometryScriptDebug argument, which is null at every call
//    site, so trusting the return value ships an empty channel as a clean compile.
//  - `rotate=` IS (roll, pitch, yaw), not FRotator's (pitch, yaw, roll). Pinned here and in
//    TestPwModelCollision, because the two read it through one shared function and the whole
//    point of sharing it is that a rotated collision element cannot disagree with its part.
//  - REPORTED COUNTS ARE THE MERGED MESH'S, on both the validate-only and the create path.
#include "Misc/AutomationTest.h"

#include "Model/PwModelAst.h"
#include "Model/PwModelCompiler.h"
#include "Model/PwModelDiagnostic.h"
#include "Tests/TestUtils.h"

#include "EditorAssetLibrary.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "PhysicsEngine/BodySetup.h"
#include "MeshDescription.h"
#include "Intersection/IntrTriangle2Triangle2.h"
#include "Polygon2.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"

namespace
{
// Prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper with a
// common name would collide with a sibling test TU once Unity merges them.
const TCHAR* const PwModelCompilerTest_OutputRoot = TEXT("/Game/PinWrightTests/PwModelCompiler");

bool PwModelCompilerTest_HasCode(const TArray<FPwDiagnostic>& Diagnostics, const TCHAR* Code)
{
    for (const FPwDiagnostic& Diagnostic : Diagnostics)
    {
        if (Diagnostic.Code == Code)
        {
            return true;
        }
    }
    return false;
}

const FPwDiagnostic* PwModelCompilerTest_FindCode(const TArray<FPwDiagnostic>& Diagnostics,
                                                       const TCHAR* Code)
{
    for (const FPwDiagnostic& Diagnostic : Diagnostics)
    {
        if (Diagnostic.Code == Code)
        {
            return &Diagnostic;
        }
    }
    return nullptr;
}

int32 PwModelCompilerTest_CountCode(const TArray<FPwDiagnostic>& Diagnostics, const TCHAR* Code)
{
    int32 Count = 0;
    for (const FPwDiagnostic& Diagnostic : Diagnostics)
    {
        if (Diagnostic.Code == Code)
        {
            ++Count;
        }
    }
    return Count;
}

// Every failure message carries the whole diagnostic list. A compiler test that fails with
// "expected true, got false" costs a rerun under a debugger to learn anything at all.
FString PwModelCompilerTest_Describe(const FPwModelCompileResult& Result)
{
    return FString::Printf(TEXT("success=%d parts=%d tris=%d slots=%d asset='%s' diagnostics=[%s]"),
        Result.bSuccess ? 1 : 0, Result.Parts.Num(), Result.MeshTriangleCount, Result.MaterialSlots,
        *Result.AssetPath, *JoinPwDiagnostics(Result.Diagnostics));
}

FString PwModelCompilerTest_AssetPath(const TCHAR* Leaf)
{
    return FString::Printf(TEXT("%s/%s"), PwModelCompilerTest_OutputRoot, Leaf);
}

// CleanupTestAsset, never UEditorAssetLibrary::DeleteAsset: that routes through
// ObjectTools::ForceDeleteObjects, whose full-object-graph reference walk costs seconds per call
// against a live editor's ~300k UObjects. The discard is what the pre-compile callers need too -
// it renames the asset AND its package into /Transient and removes the registry entry (so
// AssetCreatePolicy::Resolve's StaticFindObject misses) and deletes the .uasset (so its
// DoesPackageExist probe misses), which together leave Resolve on its Create branch.
void PwModelCompilerTest_DeleteIfPresent(const FString& AssetPath)
{
    if (UEditorAssetLibrary::DoesAssetExist(AssetPath))
    {
        CleanupTestAsset(AssetPath);
    }
}

FPwModelCompileResult PwModelCompilerTest_Validate(const TCHAR* Source)
{
    FPwModelCompileOptions Options;
    Options.bValidateOnly = true;
    Options.bSave = false;
    return FPwModelCompiler::Compile(Source, Options);
}

FPwModelCompileResult PwModelCompilerTest_CompileTo(const TCHAR* Source, const FString& AssetPath)
{
    FPwModelCompileOptions Options;
    Options.OutputAssetPath = AssetPath;
    Options.SourcePath = TEXT("Tests/PwModelCompiler.pwmodel");
    Options.bOverwrite = true;
    Options.bSave = true;
    return FPwModelCompiler::Compile(Source, Options);
}

// The span of a UV channel's coordinates over the vertex instances of the triangles whose
// vertices sit at or beyond MinX. Zero on both axes when every instance in the region carries
// the same coordinate - which is what a UV layer that exists but was never written looks like
// once baked, since the unwritten instances all land on (0, 0).
struct FPwModelCompilerTest_UVSpan
{
    int32 InstanceCount = 0;
    float SpanU = 0.0f;
    float SpanV = 0.0f;
};

FPwModelCompilerTest_UVSpan PwModelCompilerTest_MeasureUVSpan(const FMeshDescription& Description,
                                                              int32 UVChannel, float MinX)
{
    FPwModelCompilerTest_UVSpan Span;

    const FStaticMeshConstAttributes Attributes(Description);
    const TVertexInstanceAttributesConstRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
    const TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();

    if (!UVs.IsValid() || UVs.GetNumChannels() <= UVChannel)
    {
        return Span;
    }

    float MinU = TNumericLimits<float>::Max();
    float MaxU = TNumericLimits<float>::Lowest();
    float MinV = TNumericLimits<float>::Max();
    float MaxV = TNumericLimits<float>::Lowest();

    for (const FVertexInstanceID InstanceID : Description.VertexInstances().GetElementIDs())
    {
        if (Positions[Description.GetVertexInstanceVertex(InstanceID)].X < MinX)
        {
            continue;
        }

        const FVector2f UV = UVs.Get(InstanceID, UVChannel);
        MinU = FMath::Min(MinU, UV.X);
        MaxU = FMath::Max(MaxU, UV.X);
        MinV = FMath::Min(MinV, UV.Y);
        MaxV = FMath::Max(MaxV, UV.Y);
        ++Span.InstanceCount;
    }

    if (Span.InstanceCount > 0)
    {
        Span.SpanU = MaxU - MinU;
        Span.SpanV = MaxV - MinV;
    }
    return Span;
}

struct FPwModelCompilerTest_UVAreaStats
{
    int32 TriangleCount = 0;
    double WorldArea = 0.0;
    double UVArea = 0.0;
    bool bInsideUnitSquare = true;
    TArray<UE::Geometry::FTriangle2d> UVTriangles;
};

void PwModelCompilerTest_MeasureUVAreaByX(const FMeshDescription& Description, int32 UVChannel,
                                          float SplitX,
                                          FPwModelCompilerTest_UVAreaStats& OutLeft,
                                          FPwModelCompilerTest_UVAreaStats& OutRight)
{
    const FStaticMeshConstAttributes Attributes(Description);
    const TVertexInstanceAttributesConstRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
    const TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
    if (!UVs.IsValid() || UVs.GetNumChannels() <= UVChannel)
    {
        return;
    }

    for (const FTriangleID TriangleID : Description.Triangles().GetElementIDs())
    {
        const TArrayView<const FVertexInstanceID> Instances =
            Description.GetTriangleVertexInstances(TriangleID);
        const FVector3f P0 = Positions[Description.GetVertexInstanceVertex(Instances[0])];
        const FVector3f P1 = Positions[Description.GetVertexInstanceVertex(Instances[1])];
        const FVector3f P2 = Positions[Description.GetVertexInstanceVertex(Instances[2])];
        const FVector2f UV0 = UVs.Get(Instances[0], UVChannel);
        const FVector2f UV1 = UVs.Get(Instances[1], UVChannel);
        const FVector2f UV2 = UVs.Get(Instances[2], UVChannel);

        FPwModelCompilerTest_UVAreaStats& Stats = ((P0.X + P1.X + P2.X) / 3.0f < SplitX)
            ? OutLeft : OutRight;
        ++Stats.TriangleCount;
        Stats.WorldArea += 0.5 * FVector3f::CrossProduct(P1 - P0, P2 - P0).Size();
        Stats.UVArea += 0.5 * FMath::Abs(
            (UV1.X - UV0.X) * (UV2.Y - UV0.Y)
            - (UV1.Y - UV0.Y) * (UV2.X - UV0.X));
        Stats.UVTriangles.Emplace(
            FVector2d(UV0.X, UV0.Y), FVector2d(UV1.X, UV1.Y), FVector2d(UV2.X, UV2.Y));
        for (const FVector2f UV : { UV0, UV1, UV2 })
        {
            Stats.bInsideUnitSquare = Stats.bInsideUnitSquare
                && UV.X >= -KINDA_SMALL_NUMBER && UV.X <= 1.0f + KINDA_SMALL_NUMBER
                && UV.Y >= -KINDA_SMALL_NUMBER && UV.Y <= 1.0f + KINDA_SMALL_NUMBER;
        }
    }
}

bool PwModelCompilerTest_HasPositiveUVOverlap(
    const FPwModelCompilerTest_UVAreaStats& First,
    const FPwModelCompilerTest_UVAreaStats& Second)
{
    for (const UE::Geometry::FTriangle2d& FirstTriangle : First.UVTriangles)
    {
        for (const UE::Geometry::FTriangle2d& SecondTriangle : Second.UVTriangles)
        {
            UE::Geometry::FIntrTriangle2Triangle2d Intersection(
                FirstTriangle, SecondTriangle);
            if (!Intersection.Find() || Intersection.Quantity < 3)
            {
                continue;
            }

            UE::Geometry::FPolygon2d Polygon;
            for (int32 PointIndex = 0; PointIndex < Intersection.Quantity; ++PointIndex)
            {
                Polygon.AppendVertex(Intersection.Points[PointIndex]);
            }
            if (Polygon.Area() != 0.0)
            {
                return true;
            }
        }
    }
    return false;
}
}

// ============================================================================
// One file, one asset
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerOnePartOneAssetTest,
    "PinWright.Model.Compiler.OnePartDocumentYieldsOneAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerOnePartOneAssetTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = PwModelCompilerTest_AssetPath(TEXT("OnePart"));
    PwModelCompilerTest_DeleteIfPresent(AssetPath);

    const FPwModelCompileResult Result = PwModelCompilerTest_CompileTo(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(80, 50, 40)\n")
        TEXT("}\n"),
        AssetPath);

    TestTrue(*FString::Printf(TEXT("compile succeeded. %s"), *PwModelCompilerTest_Describe(Result)),
        Result.bSuccess);
    TestEqual(TEXT("one part reported"), Result.Parts.Num(), 1);
    TestEqual(TEXT("assetPath is the requested one"), Result.AssetPath, AssetPath);
    TestTrue(TEXT("the asset exists on disk"), UEditorAssetLibrary::DoesAssetExist(AssetPath));

    PwModelCompilerTest_DeleteIfPresent(AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerThreePartsOneAssetTest,
    "PinWright.Model.Compiler.ThreePartDocumentYieldsOneAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerThreePartsOneAssetTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = PwModelCompilerTest_AssetPath(TEXT("ThreeParts"));
    PwModelCompilerTest_DeleteIfPresent(AssetPath);

    // Three disjoint boxes: the merged triangle count is the sum, so a part silently dropped
    // from the merge shows up as a count mismatch rather than as a successful compile.
    const FPwModelCompileResult Result = PwModelCompilerTest_CompileTo(
        TEXT("pwmodel 0\n")
        TEXT("part a {\n")
        TEXT("    box size=(50, 50, 50)\n")
        TEXT("}\n")
        TEXT("part b {\n")
        TEXT("    box size=(50, 50, 50) at=(500, 0, 0)\n")
        TEXT("}\n")
        TEXT("part c {\n")
        TEXT("    box size=(50, 50, 50) at=(1000, 0, 0)\n")
        TEXT("}\n"),
        AssetPath);

    TestTrue(*FString::Printf(TEXT("compile succeeded. %s"), *PwModelCompilerTest_Describe(Result)),
        Result.bSuccess);
    TestEqual(TEXT("three parts reported"), Result.Parts.Num(), 3);

    int32 SumOfParts = 0;
    for (const FPwModelPartInfo& Part : Result.Parts)
    {
        SumOfParts += Part.MeshTriangleCount;
        TestTrue(*FString::Printf(TEXT("part '%s' contributed geometry"), *Part.PartName),
            Part.MeshTriangleCount > 0);
    }
    TestEqual(TEXT("merged triangle count is the sum of the parts"), Result.MeshTriangleCount, SumOfParts);

    TestTrue(TEXT("the one asset exists"), UEditorAssetLibrary::DoesAssetExist(AssetPath));

    // The failure mode this guards is a per-part emitter, which would write these.
    for (const TCHAR* PartName : { TEXT("a"), TEXT("b"), TEXT("c") })
    {
        const FString PerPartPath = FString::Printf(TEXT("%s_%s"), *AssetPath, PartName);
        TestFalse(*FString::Printf(TEXT("no per-part asset at %s"), *PerPartPath),
            UEditorAssetLibrary::DoesAssetExist(PerPartPath));
    }

    PwModelCompilerTest_DeleteIfPresent(AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerValidateOnlyCreatesNothingTest,
    "PinWright.Model.Compiler.ValidateOnlyReportsCountsAndCreatesNothing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerValidateOnlyCreatesNothingTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = PwModelCompilerTest_AssetPath(TEXT("ValidateOnly"));
    PwModelCompilerTest_DeleteIfPresent(AssetPath);

    FPwModelCompileOptions Options;
    Options.OutputAssetPath = AssetPath;
    Options.bValidateOnly = true;

    const FPwModelCompileResult Result = FPwModelCompiler::Compile(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(80, 50, 40)\n")
        TEXT("}\n"),
        Options);

    TestTrue(*FString::Printf(TEXT("validate succeeded. %s"), *PwModelCompilerTest_Describe(Result)),
        Result.bSuccess);
    TestTrue(TEXT("counts are reported"), Result.MeshTriangleCount > 0 && Result.MeshVertexCount > 0);
    TestEqual(TEXT("no assetPath is claimed"), Result.AssetPath, FString());
    TestFalse(TEXT("nothing was saved"), Result.bSavedToDisk);
    TestFalse(TEXT("no asset exists at the requested path"), UEditorAssetLibrary::DoesAssetExist(AssetPath));

    return true;
}

// ============================================================================
// Failure creates nothing
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerFailedSecondPartCreatesNothingTest,
    "PinWright.Model.Compiler.FailedSecondPartCreatesNothing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerFailedSecondPartCreatesNothingTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = PwModelCompilerTest_AssetPath(TEXT("FailedSecondPart"));
    PwModelCompilerTest_DeleteIfPresent(AssetPath);

    // Vertex 999999 does not exist on a default box, so the op fails an index-validity check
    // AFTER the first part has already built successfully.
    const FPwModelCompileResult Result = PwModelCompilerTest_CompileTo(
        TEXT("pwmodel 0\n")
        TEXT("part good {\n")
        TEXT("    box size=(50, 50, 50)\n")
        TEXT("}\n")
        TEXT("part bad {\n")
        TEXT("    box size=(50, 50, 50)\n")
        TEXT("    set_vertex_position index=999999 position=(1, 2, 3)\n")
        TEXT("}\n"),
        AssetPath);

    TestFalse(*FString::Printf(TEXT("compile failed. %s"), *PwModelCompilerTest_Describe(Result)),
        Result.bSuccess);
    TestTrue(TEXT("the op failure is reported"),
        PwModelCompilerTest_HasCode(Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_OP_FAILED));
    TestEqual(TEXT("no assetPath is claimed"), Result.AssetPath, FString());
    TestFalse(TEXT("nothing was written for the part that did build"),
        UEditorAssetLibrary::DoesAssetExist(AssetPath));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerOpFailureCarriesSourceLineTest,
    "PinWright.Model.Compiler.OpFailureCarriesSourceLine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerOpFailureCarriesSourceLineTest::RunTest(const FString& Parameters)
{
    // Line 1 is the header, 2 opens the part, 3 is the box, 4 is the failing op.
    const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(50, 50, 50)\n")
        TEXT("    set_vertex_position index=999999 position=(1, 2, 3)\n")
        TEXT("}\n"));

    TestFalse(*FString::Printf(TEXT("compile failed. %s"), *PwModelCompilerTest_Describe(Result)),
        Result.bSuccess);

    const FPwDiagnostic* Failure =
        PwModelCompilerTest_FindCode(Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_OP_FAILED);
    if (!Failure)
    {
        AddError(FString::Printf(TEXT("expected PWMODEL_OP_FAILED. %s"), *PwModelCompilerTest_Describe(Result)));
        return false;
    }

    TestEqual(TEXT("the diagnostic is anchored to the failing op's line"), Failure->Line, 4);
    TestEqual(TEXT("the diagnostic names the enclosing part"), Failure->ScopeName, FString(TEXT("body")));

    return true;
}

// ============================================================================
// Reserved constructs
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerSkeletonIsWrongFormatNotUnparsableTest,
    "PinWright.Model.Compiler.SkeletonBlockIsWrongFormatNotUnparsable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerSkeletonIsWrongFormatNotUnparsableTest::RunTest(const FString& Parameters)
{
    const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(50, 50, 50)\n")
        TEXT("}\n")
        TEXT("skeleton {\n")
        TEXT("    bone root at=(0, 0, 0)\n")
        TEXT("}\n"));

    TestFalse(*FString::Printf(TEXT("compile failed. %s"), *PwModelCompilerTest_Describe(Result)),
        Result.bSuccess);
    TestTrue(TEXT("the reserved keyword is rejected as a wrong-format signpost"),
        PwModelCompilerTest_HasCode(Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_CONSTRUCT_IN_WRONG_FORMAT));

    if (const FPwDiagnostic* Reserved = PwModelCompilerTest_FindCode(
            Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_CONSTRUCT_IN_WRONG_FORMAT))
    {
        TestFalse(TEXT("the reserved diagnostic does not promise a milestone"),
            Reserved->Message.Contains(TEXT("milestone"), ESearchCase::IgnoreCase));
        TestTrue(TEXT("the skeleton signpost names the .pwskel format"),
            Reserved->Message.Contains(TEXT(".pwskel"), ESearchCase::CaseSensitive));
        TestTrue(TEXT("the skeleton signpost names its documentation page"),
            Reserved->Message.Contains(TEXT("docs/pwskel-format.md"), ESearchCase::CaseSensitive));
    }
    // The whole point of reserving the keyword: the body is brace-checked, never interpreted,
    // so `bone` never reaches the op table.
    TestFalse(TEXT("the block body did not produce a parse error"),
        PwModelCompilerTest_HasCode(Result.Diagnostics, PwSourceDiagnosticCodes::PWSRC_UNKNOWN_OP));
    TestFalse(TEXT("the block body did not produce an unexpected-token error"),
        PwModelCompilerTest_HasCode(Result.Diagnostics, PwSourceDiagnosticCodes::PWSRC_UNEXPECTED_TOKEN));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerAnimationReservedMessageTest,
    "PinWright.Model.Compiler.AnimationReservedMessageNamesFormat",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerAnimationReservedMessageTest::RunTest(const FString& Parameters)
{
    const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(1, 1, 1)\n")
        TEXT("}\n")
        TEXT("animation walk {\n")
        TEXT("    key 0 { }\n")
        TEXT("}\n"));

    TestFalse(*FString::Printf(TEXT("animation is not implemented. %s"),
        *PwModelCompilerTest_Describe(Result)), Result.bSuccess);

    const FPwDiagnostic* Reserved = PwModelCompilerTest_FindCode(
        Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_CONSTRUCT_IN_WRONG_FORMAT);
    if (TestNotNull(TEXT("animation has a reserved-construct diagnostic"), Reserved))
    {
        TestFalse(TEXT("animation does not name a milestone"),
            Reserved->Message.Contains(TEXT("milestone"), ESearchCase::IgnoreCase));
        TestTrue(TEXT("animation names the .pwanim format"),
            Reserved->Message.Contains(TEXT(".pwanim"), ESearchCase::CaseSensitive));
        TestTrue(TEXT("animation names its documentation page"),
            Reserved->Message.Contains(TEXT("docs/pwanim-format.md"), ESearchCase::CaseSensitive));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerAnimationIsNotAUseKindTest,
    "PinWright.Model.Compiler.AnimationIsNotAUseKind",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerAnimationIsNotAUseKindTest::RunTest(const FString& Parameters)
{
    const FPwModelCompileResult AnimationUse = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("use animation from \"/Game/Animations/Walk.Walk\"\n")
        TEXT("part body {\n    box size=(1, 1, 1)\n}\n"));
    const FPwModelCompileResult SkeletonUse = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("use skeleton from \"/Game/Skeletons/Hero.Hero\"\n")
        TEXT("part body {\n    box size=(1, 1, 1)\n}\n"));

    TestTrue(TEXT("animation is rejected as a use kind"),
        PwModelCompilerTest_HasCode(AnimationUse.Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_VALUE));
    TestFalse(TEXT("animation use fails at parse, never reaching the wrong-format gate"),
        PwModelCompilerTest_HasCode(AnimationUse.Diagnostics,
            PwModelDiagnosticCodes::PWMODEL_CONSTRUCT_IN_WRONG_FORMAT));
    TestFalse(TEXT("skeleton remains a valid use kind at parse time"),
        PwModelCompilerTest_HasCode(SkeletonUse.Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_VALUE));
    // Wave 2 made `use skeleton` a LIVE reference consumed by ResolveAndValidateSkin, so it
    // is no longer a reserved construct. Only `skeleton`/`animation` BLOCKS are signposts.
    TestFalse(TEXT("skeleton use is a live reference, not a wrong-format construct"),
        PwModelCompilerTest_HasCode(SkeletonUse.Diagnostics,
            PwModelDiagnosticCodes::PWMODEL_CONSTRUCT_IN_WRONG_FORMAT));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerModelLevelVocabularyTest,
    "PinWright.Model.Compiler.ModelLevelVocabularyMatchesBothErrorPaths",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerModelLevelVocabularyTest::RunTest(const FString& Parameters)
{
    const FPwModelCompileResult NonIdentifier = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("123\n")
        TEXT("part body {\n    box size=(1, 1, 1)\n}\n"));
    const FPwModelCompileResult UnknownIdentifier = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("mystery\n")
        TEXT("part body {\n    box size=(1, 1, 1)\n}\n"));

    const FPwDiagnostic* NonIdentifierDiagnostic = PwModelCompilerTest_FindCode(
        NonIdentifier.Diagnostics, PwSourceDiagnosticCodes::PWSRC_UNEXPECTED_TOKEN);
    const FPwDiagnostic* UnknownIdentifierDiagnostic = PwModelCompilerTest_FindCode(
        UnknownIdentifier.Diagnostics, PwSourceDiagnosticCodes::PWSRC_UNEXPECTED_TOKEN);
    if (!TestNotNull(TEXT("the non-identifier path reports an unexpected token"),
            NonIdentifierDiagnostic)
        || !TestNotNull(TEXT("the unknown-identifier path reports an unexpected token"),
            UnknownIdentifierDiagnostic))
    {
        return true;
    }

    const FString FirstMarker = TEXT("a model-level construct: ");
    const FString SecondMarker = TEXT("valid here: ");
    const int32 FirstStart = NonIdentifierDiagnostic->Message.Find(FirstMarker);
    const int32 FirstEnd = NonIdentifierDiagnostic->Message.Find(
        TEXT(" but found"), ESearchCase::CaseSensitive, ESearchDir::FromStart, FirstStart);
    const int32 SecondStart = UnknownIdentifierDiagnostic->Message.Find(SecondMarker);

    TestTrue(TEXT("the non-identifier message contains the model vocabulary"),
        FirstStart != INDEX_NONE && FirstEnd > FirstStart);
    TestTrue(TEXT("the unknown-identifier message contains the model vocabulary"),
        SecondStart != INDEX_NONE);
    if (FirstStart != INDEX_NONE && FirstEnd > FirstStart && SecondStart != INDEX_NONE)
    {
        const FString FirstVocabulary = NonIdentifierDiagnostic->Message.Mid(
            FirstStart + FirstMarker.Len(), FirstEnd - (FirstStart + FirstMarker.Len()));
        const FString SecondVocabulary = UnknownIdentifierDiagnostic->Message.Mid(
            SecondStart + SecondMarker.Len()).LeftChop(1);
        TestEqual(TEXT("both model-level error paths use one vocabulary"),
            FirstVocabulary, SecondVocabulary);
    }

    return true;
}

// ============================================================================
// Material slots
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerSharedSlotNameTest,
    "PinWright.Model.Compiler.PartsSharingASlotNameShareOneSlot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerSharedSlotNameTest::RunTest(const FString& Parameters)
{
    const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("materials {\n")
        TEXT("    Shell = \"/Game/Materials/M_Metal\"\n")
        TEXT("}\n")
        TEXT("part a {\n")
        TEXT("    box size=(50, 50, 50) material=\"Shell\"\n")
        TEXT("}\n")
        TEXT("part b {\n")
        TEXT("    box size=(50, 50, 50) at=(200, 0, 0) material=\"Shell\"\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("compile succeeded. %s"), *PwModelCompilerTest_Describe(Result)),
        Result.bSuccess);
    TestEqual(TEXT("slot identity is the name, so both parts share one slot"), Result.MaterialSlots, 1);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerDistinctSlotNamesTest,
    "PinWright.Model.Compiler.PartsWithDistinctSlotNamesGetOneSlotEach",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerDistinctSlotNamesTest::RunTest(const FString& Parameters)
{
    // Different segment counts, so the per-part triangle counts differ and an attribution bug
    // (reporting the merged count, or the same count twice) is visible.
    const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("materials {\n")
        TEXT("    Shell = \"/Game/Materials/M_Metal\"\n")
        TEXT("    Trim  = \"/Game/Materials/M_Rust\"\n")
        TEXT("}\n")
        TEXT("part a {\n")
        TEXT("    box size=(50, 50, 50) material=\"Shell\"\n")
        TEXT("}\n")
        TEXT("part b {\n")
        TEXT("    box size=(50, 50, 50) segments=(3, 3, 3) at=(200, 0, 0) material=\"Trim\"\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("compile succeeded. %s"), *PwModelCompilerTest_Describe(Result)),
        Result.bSuccess);
    TestEqual(TEXT("two distinct names produce two slots"), Result.MaterialSlots, 2);

    if (Result.Parts.Num() == 2)
    {
        TestEqual(TEXT("first part reported by name"), Result.Parts[0].PartName, FString(TEXT("a")));
        TestEqual(TEXT("second part reported by name"), Result.Parts[1].PartName, FString(TEXT("b")));
        TestTrue(TEXT("the subdivided part carries more triangles than the plain one"),
            Result.Parts[1].MeshTriangleCount > Result.Parts[0].MeshTriangleCount);
        TestEqual(TEXT("merged count is the sum"), Result.MeshTriangleCount,
            Result.Parts[0].MeshTriangleCount + Result.Parts[1].MeshTriangleCount);
    }
    else
    {
        AddError(FString::Printf(TEXT("expected two parts. %s"), *PwModelCompilerTest_Describe(Result)));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerSlotListIsFirstUseOrderTest,
    "PinWright.Model.Compiler.SlotListReportsFirstUseOrderAndItsBindings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerSlotListIsFirstUseOrderTest::RunTest(const FString& Parameters)
{
    // The three bound names are declared alphabetically AND in that same order in `materials`,
    // while the GEOMETRY first uses them Charlie, Alpha, Bravo. That is the whole discriminator:
    // an implementation that reported declaration order, or that sorted, would produce
    // Alpha/Bravo/Charlie and pass a test whose fixture used a matching order. `Delta` is tagged
    // by geometry and bound by nothing, which is what proves an empty `material` means UNBOUND
    // rather than unmeasured.
    const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("materials {\n")
        TEXT("    Alpha   = \"/Game/Materials/M_Alpha\"\n")
        TEXT("    Bravo   = \"/Game/Materials/M_Bravo\"\n")
        TEXT("    Charlie = \"/Game/Materials/M_Charlie\"\n")
        TEXT("}\n")
        TEXT("part first {\n")
        TEXT("    box size=(40, 40, 40) material=\"Charlie\"\n")
        TEXT("}\n")
        TEXT("part second {\n")
        TEXT("    box size=(40, 40, 40) at=(100, 0, 0) material=\"Alpha\"\n")
        TEXT("}\n")
        TEXT("part third {\n")
        TEXT("    box size=(40, 40, 40) at=(200, 0, 0) material=\"Bravo\"\n")
        TEXT("}\n")
        TEXT("part fourth {\n")
        TEXT("    box size=(40, 40, 40) at=(300, 0, 0) material=\"Delta\"\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("compile succeeded. %s"), *PwModelCompilerTest_Describe(Result)),
        Result.bSuccess);
    TestEqual(TEXT("the count and the list it counts cannot disagree"),
        Result.MaterialSlots, Result.MaterialSlotList.Num());

    if (Result.MaterialSlotList.Num() == 4)
    {
        TestEqual(TEXT("slot 0 is the name the geometry used first"),
            Result.MaterialSlotList[0].Name, FString(TEXT("Charlie")));
        TestEqual(TEXT("slot 1 is the name the geometry used second"),
            Result.MaterialSlotList[1].Name, FString(TEXT("Alpha")));
        TestEqual(TEXT("slot 2 is the name the geometry used third"),
            Result.MaterialSlotList[2].Name, FString(TEXT("Bravo")));
        TestEqual(TEXT("slot 3 is the name the geometry used fourth"),
            Result.MaterialSlotList[3].Name, FString(TEXT("Delta")));

        TestEqual(TEXT("each slot carries the path bound to that NAME, not to that index"),
            Result.MaterialSlotList[0].BoundAssetPath, FString(TEXT("/Game/Materials/M_Charlie")));
        TestEqual(TEXT("a binding follows its name into whatever index the name landed on"),
            Result.MaterialSlotList[1].BoundAssetPath, FString(TEXT("/Game/Materials/M_Alpha")));
        TestEqual(TEXT("the third slot's binding likewise"),
            Result.MaterialSlotList[2].BoundAssetPath, FString(TEXT("/Game/Materials/M_Bravo")));
        TestTrue(TEXT("a tagged slot nothing bound reports an empty path"),
            Result.MaterialSlotList[3].BoundAssetPath.IsEmpty());
    }
    else
    {
        AddError(FString::Printf(TEXT("expected four slots. %s"), *PwModelCompilerTest_Describe(Result)));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerUntaggedGeometryGetsNoSlotWhenFullyTaggedTest,
    "PinWright.Model.Compiler.DefaultSlotOnlyExistsWhenSomethingIsUntagged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerUntaggedGeometryGetsNoSlotWhenFullyTaggedTest::RunTest(const FString& Parameters)
{
    // Two parts, both tagging the same name: one slot. Nothing is untagged, so no Default is
    // allocated - a Default that appeared anyway would ship an asset with an empty slot the
    // author never asked for and no geometry ever references.
    const FPwModelCompileResult Tagged = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("materials {\n")
        TEXT("    Shell = \"/Game/Materials/M_Metal\"\n")
        TEXT("}\n")
        TEXT("part a {\n")
        TEXT("    box size=(50, 50, 50) material=\"Shell\"\n")
        TEXT("}\n")
        TEXT("part b {\n")
        TEXT("    box size=(50, 50, 50) at=(200, 0, 0) material=\"Shell\"\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("tagged compile succeeded. %s"), *PwModelCompilerTest_Describe(Tagged)),
        Tagged.bSuccess);
    TestEqual(TEXT("fully tagged geometry grows no Default slot"), Tagged.MaterialSlots, 1);

    // Same document with the second part's tag removed: the untagged geometry needs somewhere
    // to land, so Default appears alongside Shell.
    const FPwModelCompileResult Mixed = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("materials {\n")
        TEXT("    Shell = \"/Game/Materials/M_Metal\"\n")
        TEXT("}\n")
        TEXT("part a {\n")
        TEXT("    box size=(50, 50, 50) material=\"Shell\"\n")
        TEXT("}\n")
        TEXT("part b {\n")
        TEXT("    box size=(50, 50, 50) at=(200, 0, 0)\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("mixed compile succeeded. %s"), *PwModelCompilerTest_Describe(Mixed)),
        Mixed.bSuccess);
    TestEqual(TEXT("untagged geometry adds exactly one more slot"), Mixed.MaterialSlots, 2);

    return true;
}

// ============================================================================
// Part transforms
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerPartTransformOffsetsOnlyThatPartTest,
    "PinWright.Model.Compiler.PartTransformOffsetsOnlyThatPart",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerPartTransformOffsetsOnlyThatPartTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = PwModelCompilerTest_AssetPath(TEXT("PartTransform"));
    PwModelCompilerTest_DeleteIfPresent(AssetPath);

    // `base` spans Z in [-25, 25]. `tower` is authored at the origin too and only its PART
    // header lifts it, so the merged bounds prove the part transform ran - and that it ran on
    // that part alone, since the lower bound stays at the base's.
    const FPwModelCompileResult Result = PwModelCompilerTest_CompileTo(
        TEXT("pwmodel 0\n")
        TEXT("part base {\n")
        TEXT("    box size=(50, 50, 50)\n")
        TEXT("}\n")
        TEXT("part tower at=(0, 0, 500) {\n")
        TEXT("    box size=(50, 50, 50)\n")
        TEXT("}\n"),
        AssetPath);

    TestTrue(*FString::Printf(TEXT("compile succeeded. %s"), *PwModelCompilerTest_Describe(Result)),
        Result.bSuccess);

    UStaticMesh* Mesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(AssetPath));
    if (!Mesh)
    {
        AddError(FString::Printf(TEXT("expected a UStaticMesh at %s. %s"),
            *AssetPath, *PwModelCompilerTest_Describe(Result)));
        PwModelCompilerTest_DeleteIfPresent(AssetPath);
        return false;
    }

    const FBox Bounds = Mesh->GetBoundingBox();
    TestTrue(*FString::Printf(TEXT("the transformed part reaches ~525 in Z (max %.1f)"), Bounds.Max.Z),
        Bounds.Max.Z > 500.0);
    TestTrue(*FString::Printf(TEXT("the untransformed part still sits at the origin (min %.1f)"), Bounds.Min.Z),
        Bounds.Min.Z < -20.0 && Bounds.Min.Z > -30.0);

    PwModelCompilerTest_DeleteIfPresent(AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerPartRotateIsRollPitchYawTest,
    "PinWright.Model.Compiler.PartRotateTupleIsRollPitchYaw",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerPartRotateIsRollPitchYawTest::RunTest(const FString& Parameters)
{
    // `rotate=` is (roll, pitch, yaw), which is NOT FRotator's constructor order (pitch, yaw,
    // roll). The compiler and the collision builder now read it through one shared function, and
    // this is the compiler-side half of pinning it: TestPwModelCollision's box test pins the same
    // convention on an FKBoxElem, so flipping the reordering breaks both rather than silently
    // making a rotated collision element disagree with the part it was authored against.
    //
    // A 200-long bar yawed 90 degrees about Z lies along Y. Under the wrong (roll) reading it
    // would be rolled about X instead and stay 200 long in X, which no count or diagnostic shows.
    const FString AssetPath = PwModelCompilerTest_AssetPath(TEXT("PartRotate"));
    PwModelCompilerTest_DeleteIfPresent(AssetPath);

    const FPwModelCompileResult Result = PwModelCompilerTest_CompileTo(
        TEXT("pwmodel 0\n")
        TEXT("part bar rotate=(0, 0, 90) {\n")
        TEXT("    box size=(200, 40, 40)\n")
        TEXT("}\n"),
        AssetPath);

    TestTrue(*FString::Printf(TEXT("compile succeeded. %s"), *PwModelCompilerTest_Describe(Result)),
        Result.bSuccess);

    UStaticMesh* Mesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(AssetPath));
    if (!Mesh)
    {
        AddError(FString::Printf(TEXT("expected a UStaticMesh at %s. %s"),
            *AssetPath, *PwModelCompilerTest_Describe(Result)));
        PwModelCompilerTest_DeleteIfPresent(AssetPath);
        return false;
    }

    const FVector Extent = Mesh->GetBoundingBox().GetSize();
    TestTrue(*FString::Printf(TEXT("the long axis moved onto Y (extent %.1f)"), Extent.Y),
        Extent.Y > 150.0);
    TestTrue(*FString::Printf(TEXT("X is now the short axis (extent %.1f)"), Extent.X),
        Extent.X < 100.0);

    PwModelCompilerTest_DeleteIfPresent(AssetPath);
    return true;
}

// ============================================================================
// Lightmap
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerLightmapResolutionReachesTheAssetTest,
    "PinWright.Model.Compiler.LightmapResolutionReachesTheAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerLightmapResolutionReachesTheAssetTest::RunTest(const FString& Parameters)
{
    // The whole chain the defect lived in, end to end: `resolution=` on the statement, through
    // BuildLightmapParams and FCompiler::CreateAsset, into FStaticMeshCreateSpec and onto the
    // built asset. Three shipped examples declared `lightmap channel=1` and paid for a
    // patch_builder + layout pass, and every one of them reported lightmapResolution 4 - the
    // bare UStaticMesh default - because the format could not say the resolution at all.
    const FString AssetPath = PwModelCompilerTest_AssetPath(TEXT("LightmapResolution"));
    PwModelCompilerTest_DeleteIfPresent(AssetPath);

    const FPwModelCompileResult Result = PwModelCompilerTest_CompileTo(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(100, 100, 100)\n")
        TEXT("    uv channel=0 mode=box\n")
        TEXT("    uv channel=1 mode=box\n")
        TEXT("}\n")
        TEXT("lightmap channel=1 resolution=256\n"),
        AssetPath);

    TestTrue(*FString::Printf(TEXT("compile succeeded. %s"), *PwModelCompilerTest_Describe(Result)),
        Result.bSuccess);

    UStaticMesh* Mesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(AssetPath));
    if (!Mesh)
    {
        AddError(FString::Printf(TEXT("expected a UStaticMesh at %s. %s"),
            *AssetPath, *PwModelCompilerTest_Describe(Result)));
        PwModelCompilerTest_DeleteIfPresent(AssetPath);
        return false;
    }

    // Both halves. The asset property is what the renderer and static_mesh.describe read; the
    // source model's MinLightmapResolution is what a lightmap-UV GENERATION pass would pack
    // against (CreateLightMapUVLayout - inert here, since this path authors the channel itself
    // and leaves bGenerateLightmapUVs off) and what the Static Mesh editor shows. Writing only
    // one of the two leaves the asset self-contradictory.
    TestEqual(TEXT("the authored lightmap resolution is on the asset"),
        Mesh->GetLightMapResolution(), 256);
    TestEqual(TEXT("and on the source model's build settings"),
        Mesh->GetSourceModel(0).BuildSettings.MinLightmapResolution, 256);
    TestEqual(TEXT("the channel still landed alongside it"), Mesh->GetLightMapCoordinateIndex(), 1);

    PwModelCompilerTest_DeleteIfPresent(AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerLightmapWithoutResolutionWarnsTest,
    "PinWright.Model.Compiler.LightmapWithoutResolutionWarns",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerLightmapWithoutResolutionWarnsTest::RunTest(const FString& Parameters)
{
    // The silent-waste case, which is what made the defect survive: the channel is honoured, the
    // lightmap UVs are real, the compile is clean - and the asset bakes them at 4x4. Omitting
    // `resolution` stays LEGAL (it is optional, and the engine default is a defensible choice),
    // so the only thing that can surface it is a warning.
    const FString AssetPath = PwModelCompilerTest_AssetPath(TEXT("LightmapNoResolution"));
    PwModelCompilerTest_DeleteIfPresent(AssetPath);

    const FPwModelCompileResult Result = PwModelCompilerTest_CompileTo(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(100, 100, 100)\n")
        TEXT("    uv channel=0 mode=box\n")
        TEXT("    uv channel=1 mode=box\n")
        TEXT("}\n")
        TEXT("lightmap channel=1\n"),
        AssetPath);

    TestTrue(*FString::Printf(TEXT("compile still succeeded. %s"), *PwModelCompilerTest_Describe(Result)),
        Result.bSuccess);

    UStaticMesh* Mesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(AssetPath));
    if (!Mesh)
    {
        AddError(FString::Printf(TEXT("expected a UStaticMesh at %s. %s"),
            *AssetPath, *PwModelCompilerTest_Describe(Result)));
        PwModelCompilerTest_DeleteIfPresent(AssetPath);
        return false;
    }

    TestEqual(TEXT("the asset keeps the UStaticMesh default of 4"), Mesh->GetLightMapResolution(), 4);

    // Scanned rather than taken from the first STAGE_WARNING: the bucket code is shared by every
    // stage, so matching only the code would pass on somebody else's warning.
    bool bWarned = false;
    for (const FPwDiagnostic& Diagnostic : Result.Diagnostics)
    {
        if (Diagnostic.Code == PwModelDiagnosticCodes::PWMODEL_STAGE_WARNING
            && Diagnostic.Message.Contains(TEXT("no lightmap resolution"))
            && Diagnostic.Message.Contains(TEXT("resolution=")))
        {
            bWarned = true;
            break;
        }
    }
    TestTrue(*FString::Printf(
        TEXT("a lightmap channel with no resolution is reported, naming the fix. %s"),
        *PwModelCompilerTest_Describe(Result)), bWarned);

    PwModelCompilerTest_DeleteIfPresent(AssetPath);
    return true;
}

// ============================================================================
// Reported counts
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerCountsMatchValidateOnlyTest,
    "PinWright.Model.Compiler.ReportedCountsAreTheMergedMeshOnBothPaths",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerCountsMatchValidateOnlyTest::RunTest(const FString& Parameters)
{
    // The header calls these "counts of the MERGED mesh". Creation used to re-stamp its own
    // copies over them, so the same document could report different numbers depending on
    // bValidateOnly - and validate-only is the surface an author iterates on before committing
    // an asset, which makes a disagreement between the two the worst place for one.
    const TCHAR* const Source =
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(80, 50, 40)\n")
        TEXT("    subtract { cylinder radius=10 height=200 }\n")
        TEXT("}\n")
        TEXT("collision {\n")
        TEXT("    box size=(80, 50, 40)\n")
        TEXT("    sphere radius=20 at=(0, 0, 30)\n")
        TEXT("}\n");

    const FString AssetPath = PwModelCompilerTest_AssetPath(TEXT("CountsAgree"));
    PwModelCompilerTest_DeleteIfPresent(AssetPath);

    const FPwModelCompileResult Validated = PwModelCompilerTest_Validate(Source);
    const FPwModelCompileResult Created = PwModelCompilerTest_CompileTo(Source, AssetPath);

    TestTrue(*FString::Printf(TEXT("validate-only succeeded. %s"),
        *PwModelCompilerTest_Describe(Validated)), Validated.bSuccess);
    TestTrue(*FString::Printf(TEXT("create succeeded. %s"),
        *PwModelCompilerTest_Describe(Created)), Created.bSuccess);

    TestTrue(TEXT("the document actually produced geometry"), Validated.MeshTriangleCount > 0);
    TestEqual(TEXT("two collision elements were built"), Validated.CollisionElements, 2);

    TestEqual(TEXT("triangle count is the same on both paths"),
        Created.MeshTriangleCount, Validated.MeshTriangleCount);
    TestEqual(TEXT("vertex count is the same on both paths"),
        Created.MeshVertexCount, Validated.MeshVertexCount);
    TestEqual(TEXT("collision element count is the same on both paths"),
        Created.CollisionElements, Validated.CollisionElements);

    // The asset counts are the ONE pair that must NOT match across the two paths: validate
    // writes no asset, so it has nothing to measure and says so with -1 rather than 0.
    TestEqual(TEXT("validate reports no asset triangle count"), Validated.AssetTriangleCount, -1);
    TestEqual(TEXT("validate reports no asset vertex count"), Validated.AssetVertexCount, -1);
    TestTrue(TEXT("create reports an asset triangle count"), Created.AssetTriangleCount >= 0);
    TestTrue(TEXT("create reports an asset vertex count"), Created.AssetVertexCount >= 0);

    PwModelCompilerTest_DeleteIfPresent(AssetPath);
    return true;
}

// The counts the compile response publishes have to be the counts the asset has.
//
// They were not. `triangleCount` was documented as "IS the asset's triangle count" and was the
// pre-bake UDynamicMesh's: the StaticMesh build drops every triangle with two corners inside
// THRESH_POINTS_ARE_SAME (FMeshBuildSettings::bRemoveDegenerates, default true;
// StaticMeshBuilder.cpp:1666-1674), so the published triangle count read high against the asset
// on every shipped example. The vertex count drifted the other way, the build splitting a
// position into one render vertex per distinct normal/tangent/UV/color.
//
// The measured per-example numbers are in docs/pwmodel-format.md under
// "Four counts: two for the mesh, two for the asset", and are deliberately not restated here -
// they were hand-copied into this comment and three others and went stale in all four. That the
// duplication stays collapsed is itself asserted, by PinWright.core.pwmodel_examples.* in
// Source/PinWright/Private/Tests/Core/TestPwModelExampleCatalog.cpp.
//
// This reads BOTH numbers straight off the built asset and compares them with what the compiler
// published, so it fails whether the drift comes back on either field or in either direction.
//
// Counterfactual: restore `Out.TriangleCount = MeshCopy.TriangleCount()` in
// GeometryAssetCreate.cpp and this fails on any document with a degenerate triangle; restore
// the vertex half and it fails on any document with a seam.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerReportsTheAssetsOwnCountsTest,
    "PinWright.Model.Compiler.ReportedAssetCountsMatchTheBuiltAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerReportsTheAssetsOwnCountsTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = PwModelCompilerTest_AssetPath(TEXT("SM_AssetCounts"));
    PwModelCompilerTest_DeleteIfPresent(AssetPath);

    // A box has six flat faces and therefore a hard normal seam at every edge, which is what
    // makes the vertex split observable: 8 mesh vertices become 24 render vertices.
    const FPwModelCompileResult Result = PwModelCompilerTest_CompileTo(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(50, 50, 50)\n")
        TEXT("}\n"),
        AssetPath);

    TestTrue(*FString::Printf(TEXT("compile succeeded. %s"), *PwModelCompilerTest_Describe(Result)),
        Result.bSuccess);

    UStaticMesh* Mesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(AssetPath));
    if (!Mesh)
    {
        AddError(FString::Printf(TEXT("expected a UStaticMesh at %s. %s"),
            *AssetPath, *PwModelCompilerTest_Describe(Result)));
        PwModelCompilerTest_DeleteIfPresent(AssetPath);
        return false;
    }

    FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});

    TestEqual(TEXT("assetTriangleCount is LOD0's triangle count"),
        Result.AssetTriangleCount, Mesh->GetNumTriangles(0));
    TestEqual(TEXT("assetVertexCount is LOD0's vertex count"),
        Result.AssetVertexCount, Mesh->GetNumVertices(0));

    // And that the mesh counts are still reported as themselves, so the pair stays a pair
    // rather than one number copied into two fields.
    TestTrue(TEXT("the mesh triangle count is reported too"), Result.MeshTriangleCount > 0);
    TestTrue(TEXT("the mesh vertex count is reported too"), Result.MeshVertexCount > 0);
    TestTrue(TEXT("the bake split vertices, so the asset carries more of them than the mesh"),
        Result.AssetVertexCount > Result.MeshVertexCount);

    PwModelCompilerTest_DeleteIfPresent(AssetPath);
    return true;
}

// The compiler half of the PWMODEL_UNUSED_MATERIAL correction.
//
// The parser used to warn "Slot 'Default' is bound but no geometry tags it; the binding is
// dropped" for this exact document. This test is what settles which side was wrong: it reads
// the WRITTEN ASSET and finds the slot present and bound to the material the document named,
// so the drop never happened and the diagnostic was the lie. The parser-side assertion that
// the warning is now absent lives in TestPwModelParser.cpp; it would pass just as well if the
// warning had simply been deleted, which is why the asset read-back is here.
//
// /Engine/BasicShapes/BasicShapeMaterial is deliberately not the engine's fallback: an unbound
// slot gets UMaterial::GetDefaultMaterial(MD_Surface) (CreateStaticMeshUtil.cpp:113), so
// finding BasicShapeMaterial on the slot cannot be a coincidence of the default path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerDefaultSlotBindingReachesTheAssetTest,
    "PinWright.Model.Compiler.DefaultSlotBindingReachesTheAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerDefaultSlotBindingReachesTheAssetTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = PwModelCompilerTest_AssetPath(TEXT("SM_DefaultSlotBinding"));
    PwModelCompilerTest_DeleteIfPresent(AssetPath);

    // No `material=` anywhere, so every triangle lands in the implicit Default slot.
    const FPwModelCompileResult Result = PwModelCompilerTest_CompileTo(
        TEXT("pwmodel 0\n")
        TEXT("materials {\n    Default = \"/Engine/BasicShapes/BasicShapeMaterial\"\n}\n")
        TEXT("part body {\n    box size=(50, 50, 50)\n}\n"),
        AssetPath);

    TestTrue(*FString::Printf(TEXT("compile succeeded. %s"), *PwModelCompilerTest_Describe(Result)),
        Result.bSuccess);
    TestFalse(*FString::Printf(TEXT("no PWMODEL_UNUSED_MATERIAL for the Default binding. %s"),
        *PwModelCompilerTest_Describe(Result)),
        PwModelCompilerTest_HasCode(Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_UNUSED_MATERIAL));
    TestEqual(TEXT("the model reports exactly one material slot"), Result.MaterialSlots, 1);

    UStaticMesh* Mesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(AssetPath));
    if (!Mesh)
    {
        AddError(FString::Printf(TEXT("expected a UStaticMesh at %s. %s"),
            *AssetPath, *PwModelCompilerTest_Describe(Result)));
        PwModelCompilerTest_DeleteIfPresent(AssetPath);
        return false;
    }

    FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});

    const TArray<FStaticMaterial>& Slots = Mesh->GetStaticMaterials();
    TestEqual(TEXT("the asset carries one slot"), Slots.Num(), 1);
    if (Slots.Num() == 1)
    {
        TestEqual(TEXT("the slot is named Default"),
            Slots[0].MaterialSlotName.ToString(), FString(PwModelDefaultSlotName));
        if (TestNotNull(TEXT("the slot carries a material"), Slots[0].MaterialInterface.Get()))
        {
            TestEqual(TEXT("the binding landed rather than being dropped"),
                Slots[0].MaterialInterface->GetPathName(),
                FString(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")));
        }
    }

    PwModelCompilerTest_DeleteIfPresent(AssetPath);
    return true;
}

// `append_buffers material="<Slot>"` resolves through the SAME model-wide slot table every
// shape primitive uses. Raw vertex data was the one way to build geometry that could not be
// given a material by name: the parser rejected the parameter, so a document that bound a slot
// for it was told the binding was dropped (PWMODEL_UNUSED_MATERIAL) and `material_id=0` did not
// help, because index 0 is whatever slot happened to be allocated first.
//
// Asserted through the slot COUNT rather than through "it compiled": a name resolved into its
// own table, or into an index rather than the shared list, still compiles - it just ships an
// asset with a slot too many and geometry on the wrong material.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerAppendBuffersSharesTheModelWideSlotTest,
    "PinWright.Model.Compiler.AppendBuffersMaterialNameSharesTheModelWideSlot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerAppendBuffersSharesTheModelWideSlotTest::RunTest(const FString& Parameters)
{
    const FPwModelCompileResult Shared = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("materials {\n")
        TEXT("    Shell = \"/Game/Materials/M_Metal\"\n")
        TEXT("}\n")
        TEXT("part a {\n")
        TEXT("    box size=(50, 50, 50) material=\"Shell\"\n")
        TEXT("}\n")
        TEXT("part b {\n")
        TEXT("    append_buffers vertices=[(0, 0, 0), (100, 0, 0), (50, 100, 0)] triangles=[(0, 1, 2)] ")
        TEXT("uvs=[(0, 0), (1, 0), (0.5, 1)] material=\"Shell\"\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("compile succeeded. %s"), *PwModelCompilerTest_Describe(Shared)),
        Shared.bSuccess);
    TestEqual(TEXT("a box and raw buffers naming one slot share it"), Shared.MaterialSlots, 1);

    // No padding warning: the tag rewrote the appended triangles' material IDs to the resolved
    // index, so nothing references an ID past the end of the list.
    TestFalse(*FString::Printf(TEXT("the tag rewrote the raw IDs rather than leaving them. %s"),
        *PwModelCompilerTest_Describe(Shared)),
        PwModelCompilerTest_HasCode(Shared.Diagnostics,
            PwModelDiagnosticCodes::PWMODEL_MATERIAL_ID_OUT_OF_RANGE));

    // Same document, a distinct name on the buffers: two slots, from the same table.
    const FPwModelCompileResult Distinct = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("materials {\n")
        TEXT("    Shell = \"/Game/Materials/M_Metal\"\n")
        TEXT("    Quartz = \"/Game/Materials/M_Quartz\"\n")
        TEXT("}\n")
        TEXT("part a {\n")
        TEXT("    box size=(50, 50, 50) material=\"Shell\"\n")
        TEXT("}\n")
        TEXT("part b {\n")
        TEXT("    append_buffers vertices=[(0, 0, 0), (100, 0, 0), (50, 100, 0)] triangles=[(0, 1, 2)] ")
        TEXT("uvs=[(0, 0), (1, 0), (0.5, 1)] material=\"Quartz\"\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("compile succeeded. %s"), *PwModelCompilerTest_Describe(Distinct)),
        Distinct.bSuccess);
    TestEqual(TEXT("a distinct name on the buffers allocates its own slot"), Distinct.MaterialSlots, 2);

    return true;
}

// The slot the tag names reaches the ASSET, on the geometry the tag was written on.
//
// The counts above cannot see whether the resolved index was ever written onto the triangles.
// Read back from the .uasset: one slot, named Quartz, carrying the material the document bound
// to it - and /Game is deliberately not used for the binding, because an unbound slot silently
// takes UMaterial::GetDefaultMaterial, so the assertion has to name an asset the default path
// cannot produce.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerAppendBuffersSlotReachesTheAssetTest,
    "PinWright.Model.Compiler.AppendBuffersMaterialSlotReachesTheAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerAppendBuffersSlotReachesTheAssetTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = PwModelCompilerTest_AssetPath(TEXT("SM_AppendBuffersSlot"));
    PwModelCompilerTest_DeleteIfPresent(AssetPath);

    const FPwModelCompileResult Result = PwModelCompilerTest_CompileTo(
        TEXT("pwmodel 0\n")
        TEXT("materials {\n    Quartz = \"/Engine/BasicShapes/BasicShapeMaterial\"\n}\n")
        TEXT("part shard {\n")
        TEXT("    append_buffers vertices=[(0, 0, 0), (100, 0, 0), (100, 100, 0), (0, 100, 0)] ")
        TEXT("triangles=[(0, 1, 2), (0, 2, 3)] uvs=[(0, 0), (1, 0), (1, 1), (0, 1)] material=\"Quartz\"\n")
        TEXT("}\n"),
        AssetPath);

    TestTrue(*FString::Printf(TEXT("compile succeeded. %s"), *PwModelCompilerTest_Describe(Result)),
        Result.bSuccess);
    TestFalse(*FString::Printf(TEXT("no PWMODEL_UNUSED_MATERIAL for the tagged slot. %s"),
        *PwModelCompilerTest_Describe(Result)),
        PwModelCompilerTest_HasCode(Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_UNUSED_MATERIAL));

    // One slot, and it is NOT the implicit Default: every triangle in the model carries the tag,
    // so nothing untagged remains for Default to hold.
    TestEqual(TEXT("the model reports exactly one material slot"), Result.MaterialSlots, 1);

    UStaticMesh* Mesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(AssetPath));
    if (!Mesh)
    {
        AddError(FString::Printf(TEXT("expected a UStaticMesh at %s. %s"),
            *AssetPath, *PwModelCompilerTest_Describe(Result)));
        PwModelCompilerTest_DeleteIfPresent(AssetPath);
        return false;
    }

    FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});

    const TArray<FStaticMaterial>& Slots = Mesh->GetStaticMaterials();
    TestEqual(TEXT("the asset carries one slot"), Slots.Num(), 1);
    if (Slots.Num() == 1)
    {
        TestEqual(TEXT("the slot carries the name the buffers were tagged with"),
            Slots[0].MaterialSlotName.ToString(), FString(TEXT("Quartz")));
        if (TestNotNull(TEXT("the slot carries a material"), Slots[0].MaterialInterface.Get()))
        {
            TestEqual(TEXT("the binding landed rather than being dropped"),
                Slots[0].MaterialInterface->GetPathName(),
                FString(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")));
        }
    }

    PwModelCompilerTest_DeleteIfPresent(AssetPath);
    return true;
}

// material_id= alone still writes the raw index, unrewritten. It exists so a generator that has
// already computed IDs can hand them over, and the padding warning is the proof the value
// reached the merged mesh: the compiler reads the mesh's own max material ID back out, so a tag
// path that had quietly started clearing IDs to a slot would leave 1 slot and no warning.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerAppendBuffersRawMaterialIdTest,
    "PinWright.Model.Compiler.AppendBuffersRawMaterialIdIsStillWrittenUnrewritten",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerAppendBuffersRawMaterialIdTest::RunTest(const FString& Parameters)
{
    const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part shard {\n")
        TEXT("    append_buffers vertices=[(0, 0, 0), (100, 0, 0), (50, 100, 0)] triangles=[(0, 1, 2)] ")
        TEXT("uvs=[(0, 0), (1, 0), (0.5, 1)] material_id=3\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("compile succeeded. %s"), *PwModelCompilerTest_Describe(Result)),
        Result.bSuccess);
    TestFalse(*FString::Printf(TEXT("no conflict is reported for material_id alone. %s"),
        *PwModelCompilerTest_Describe(Result)),
        PwModelCompilerTest_HasCode(Result.Diagnostics,
            PwModelDiagnosticCodes::PWMODEL_MATERIAL_ID_CONFLICT));
    TestTrue(*FString::Printf(TEXT("the raw ID reached the merged mesh, so the slot list was padded. %s"),
        *PwModelCompilerTest_Describe(Result)),
        PwModelCompilerTest_HasCode(Result.Diagnostics,
            PwModelDiagnosticCodes::PWMODEL_MATERIAL_ID_OUT_OF_RANGE));
    TestEqual(TEXT("slots are padded up to the raw ID, not clamped onto an existing one"),
        Result.MaterialSlots, 4);

    return true;
}

// Both spellings on one op fails at the PARSER, so the compiler never runs and nothing is built.
// Asserted from this side too because the conflict has to hold on the surface authors actually
// call - model.compile / model.validate route through FPwModelCompiler, not through the parser.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerMaterialIdConflictStopsTheCompileTest,
    "PinWright.Model.Compiler.BothMaterialSpellingsOnOneOpBuildsNothing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerMaterialIdConflictStopsTheCompileTest::RunTest(const FString& Parameters)
{
    const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("materials {\n    Quartz = \"/Game/Materials/M_Quartz\"\n}\n")
        TEXT("part shard {\n")
        TEXT("    append_buffers vertices=[(0, 0, 0), (100, 0, 0), (50, 100, 0)] triangles=[(0, 1, 2)] ")
        TEXT("material=\"Quartz\" material_id=3\n")
        TEXT("}\n"));

    TestFalse(*FString::Printf(TEXT("the compile fails. %s"), *PwModelCompilerTest_Describe(Result)),
        Result.bSuccess);
    TestTrue(*FString::Printf(TEXT("PWMODEL_MATERIAL_ID_CONFLICT was reported. %s"),
        *PwModelCompilerTest_Describe(Result)),
        PwModelCompilerTest_HasCode(Result.Diagnostics,
            PwModelDiagnosticCodes::PWMODEL_MATERIAL_ID_CONFLICT));

    // It stops at stage 1, before any mesh exists: nothing merged, so no slots were allocated
    // and no geometry-stage diagnostic can have been produced.
    TestEqual(TEXT("no mesh was built, so no slot was allocated"), Result.MaterialSlots, 0);
    TestFalse(*FString::Printf(TEXT("the padding warning cannot fire, since nothing merged. %s"),
        *PwModelCompilerTest_Describe(Result)),
        PwModelCompilerTest_HasCode(Result.Diagnostics,
            PwModelDiagnosticCodes::PWMODEL_MATERIAL_ID_OUT_OF_RANGE));

    return true;
}

// ============================================================================
// UV channel fill
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerFillsMissingUVChannelsOnceTest,
    "PinWright.Model.Compiler.MissingUVChannelsAreFilledWithOneWarning",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerFillsMissingUVChannelsOnceTest::RunTest(const FString& Parameters)
{
    // `textured` populates channel 1; neither other part touches it. Without the fill the
    // merged mesh has channel-1 elements - so the bake's usable-UV guard does not fire - while
    // two thirds of its triangles carry none, and the asset ships with an unlit lightmap
    // region and no error anywhere.
    const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part textured {\n")
        TEXT("    box size=(50, 50, 50)\n")
        TEXT("    uv channel=1 mode=box\n")
        TEXT("}\n")
        TEXT("part plain_one {\n")
        TEXT("    box size=(50, 50, 50) at=(200, 0, 0)\n")
        TEXT("}\n")
        TEXT("part plain_two {\n")
        TEXT("    box size=(50, 50, 50) at=(400, 0, 0)\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("compile succeeded. %s"), *PwModelCompilerTest_Describe(Result)),
        Result.bSuccess);

    // ONE diagnostic for the whole model, not one per part: a five-part model with no UVs
    // producing five identical warnings trains the reader to skip them.
    TestEqual(*FString::Printf(TEXT("exactly one fill warning. %s"), *PwModelCompilerTest_Describe(Result)),
        PwModelCompilerTest_CountCode(Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_UV_CHANNEL_FILLED), 1);

    const FPwDiagnostic* Filled =
        PwModelCompilerTest_FindCode(Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_UV_CHANNEL_FILLED);
    if (Filled)
    {
        TestTrue(TEXT("the warning names the first silent part"), Filled->Message.Contains(TEXT("plain_one")));
        TestTrue(TEXT("the warning names the second silent part"), Filled->Message.Contains(TEXT("plain_two")));
        TestTrue(TEXT("the warning names the channel"), Filled->Message.Contains(TEXT("channel 1")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerNoUVWarningWhenChannelsAgreeTest,
    "PinWright.Model.Compiler.NoUVWarningWhenEveryPartCarriesTheSameChannels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerNoUVWarningWhenChannelsAgreeTest::RunTest(const FString& Parameters)
{
    // The guard on the guard: a projection that re-ran over parts which already carry the
    // channel would destroy authored UVs, so the fill must be driven by element presence
    // rather than by which part declared a `uv` op.
    const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part a {\n")
        TEXT("    box size=(50, 50, 50)\n")
        TEXT("    uv channel=0 mode=box\n")
        TEXT("}\n")
        TEXT("part b {\n")
        TEXT("    box size=(50, 50, 50) at=(200, 0, 0)\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("compile succeeded. %s"), *PwModelCompilerTest_Describe(Result)),
        Result.bSuccess);
    TestEqual(*FString::Printf(TEXT("no fill warning. %s"), *PwModelCompilerTest_Describe(Result)),
        PwModelCompilerTest_CountCode(Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_UV_CHANNEL_FILLED), 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerFilledUVChannelIsPopulatedTest,
    "PinWright.Model.Compiler.FilledUVChannelsCarryRealUVsInTheAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerFilledUVChannelIsPopulatedTest::RunTest(const FString& Parameters)
{
    // The warning-only sibling above cannot see this: its `Filled` entry is appended
    // unconditionally after the projection runs, so gutting BoxProjectChannel to a no-op leaves
    // the warning count, the message and the named parts all correct - and ships two thirds of
    // the asset with an empty channel 1. This asserts the ELEMENTS, in the built asset, which is
    // the only place the difference is visible.
    const FString AssetPath = PwModelCompilerTest_AssetPath(TEXT("FilledUVChannel"));
    PwModelCompilerTest_DeleteIfPresent(AssetPath);

    const FPwModelCompileResult Result = PwModelCompilerTest_CompileTo(
        TEXT("pwmodel 0\n")
        TEXT("part textured {\n")
        TEXT("    box size=(50, 50, 50)\n")
        TEXT("    uv channel=1 mode=box\n")
        TEXT("}\n")
        TEXT("part plain_one {\n")
        TEXT("    box size=(50, 50, 50) at=(200, 0, 0)\n")
        TEXT("}\n")
        TEXT("part plain_two {\n")
        TEXT("    box size=(50, 50, 50) at=(400, 0, 0)\n")
        TEXT("}\n"),
        AssetPath);

    TestTrue(*FString::Printf(TEXT("compile succeeded. %s"), *PwModelCompilerTest_Describe(Result)),
        Result.bSuccess);

    UStaticMesh* Mesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(AssetPath));
    if (!Mesh)
    {
        AddError(FString::Printf(TEXT("expected a UStaticMesh at %s. %s"),
            *AssetPath, *PwModelCompilerTest_Describe(Result)));
        PwModelCompilerTest_DeleteIfPresent(AssetPath);
        return false;
    }

    FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});

    const FMeshDescription* Baked = Mesh->GetMeshDescription(0);
    if (!Baked)
    {
        AddError(TEXT("the baked asset has no LOD0 mesh description"));
        PwModelCompilerTest_DeleteIfPresent(AssetPath);
        return false;
    }

    const FStaticMeshConstAttributes Attributes(*Baked);
    TestTrue(TEXT("channel 1 survived the bake"),
        Attributes.GetVertexInstanceUVs().GetNumChannels() >= 2);

    // MinX = 150 excludes `textured`, which spans [-25, 25]; both silent parts sit past it.
    const FPwModelCompilerTest_UVSpan Span = PwModelCompilerTest_MeasureUVSpan(*Baked, 1, 150.0f);

    TestTrue(*FString::Printf(TEXT("the two silent parts contributed vertex instances (found %d)"),
        Span.InstanceCount), Span.InstanceCount > 0);
    TestTrue(*FString::Printf(TEXT("the filled channel varies in U across the silent parts (span %g)"),
        Span.SpanU), Span.SpanU > KINDA_SMALL_NUMBER);
    TestTrue(*FString::Printf(TEXT("the filled channel varies in V across the silent parts (span %g)"),
        Span.SpanV), Span.SpanV > KINDA_SMALL_NUMBER);

    PwModelCompilerTest_DeleteIfPresent(AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerPerPartLightmapUVOverlapTest,
    "PinWright.Model.Report.PerPartLightmapUVOverlapIsReportedAcrossParts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerPerPartLightmapUVOverlapTest::RunTest(const FString& Parameters)
{
    const FString Parts =
        TEXT("pwmodel 0\n")
        TEXT("part first {\n")
        TEXT("    append_buffers vertices=[(0, 0, 0), (50, 0, 0), (0, 50, 0)] triangles=[(0, 1, 2)]\n")
        TEXT("    uv channel=1 mode=box\n")
        TEXT("    uv channel=1 mode=layout texture_resolution=64\n")
        TEXT("}\n")
        TEXT("part second at=(200, 0, 0) allow_floating=true {\n")
        TEXT("    append_buffers vertices=[(0, 0, 0), (50, 0, 0), (0, 50, 0)] triangles=[(0, 1, 2)]\n")
        TEXT("    uv channel=1 mode=box\n")
        TEXT("    uv channel=1 mode=layout texture_resolution=64\n")
        TEXT("}\n");

    const FString LightmappedSource = Parts + TEXT("lightmap channel=1 resolution=64\n");
    const FPwModelCompileResult Lightmapped = PwModelCompilerTest_Validate(*LightmappedSource);
    TestFalse(*FString::Printf(TEXT("overlapping declared lightmap UVs fail. %s"),
        *PwModelCompilerTest_Describe(Lightmapped)), Lightmapped.bSuccess);
    TestEqual(TEXT("one model-level overlap produces exactly one stable diagnostic"),
        PwModelCompilerTest_CountCode(Lightmapped.Diagnostics,
            PwModelDiagnosticCodes::PWMODEL_UV_OVERLAP_ACROSS_PARTS), 1);

    const FPwDiagnostic* Overlap = PwModelCompilerTest_FindCode(
        Lightmapped.Diagnostics, PwModelDiagnosticCodes::PWMODEL_UV_OVERLAP_ACROSS_PARTS);
    if (TestNotNull(TEXT("the stable cross-part UV overlap code is emitted"), Overlap))
    {
        TestTrue(TEXT("the diagnostic names channel 1"),
            Overlap->Message.Contains(TEXT("channel 1")));
        TestTrue(TEXT("the diagnostic names the first part"),
            Overlap->Message.Contains(TEXT("first")));
        TestTrue(TEXT("the diagnostic names the second part"),
            Overlap->Message.Contains(TEXT("second")));
        TestEqual(TEXT("the diagnostic anchors to the last contributing part-level uv op"),
            Overlap->Line, 10);
        TestEqual(TEXT("a part-level uv anchor keeps part scope"), Overlap->ScopeLabel,
            FString(TEXT("part")));
        TestEqual(TEXT("the anchor part is the part carrying that op"),
            Overlap->ScopeName, FString(TEXT("second")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerPerPartOrdinaryUVOverlapTest,
    "PinWright.Model.Report.PerPartOrdinaryUVOverlapIsReportedAcrossParts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerPerPartOrdinaryUVOverlapTest::RunTest(const FString& Parameters)
{
    const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part first {\n")
        TEXT("    append_buffers vertices=[(0, 0, 0), (50, 0, 0), (0, 50, 0)] triangles=[(0, 1, 2)]\n")
        TEXT("    uv channel=2 mode=box\n")
        TEXT("}\n")
        TEXT("part second at=(200, 0, 0) allow_floating=true {\n")
        TEXT("    append_buffers vertices=[(0, 0, 0), (50, 0, 0), (0, 50, 0)] triangles=[(0, 1, 2)]\n")
        TEXT("    uv channel=2 mode=box\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("ordinary-channel overlap warns without failing. %s"),
        *PwModelCompilerTest_Describe(Result)), Result.bSuccess);
    TestEqual(TEXT("one populated ordinary channel produces one stable diagnostic"),
        PwModelCompilerTest_CountCode(Result.Diagnostics,
            PwModelDiagnosticCodes::PWMODEL_UV_OVERLAP_ACROSS_PARTS), 1);
    const FPwDiagnostic* Overlap = PwModelCompilerTest_FindCode(
        Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_UV_OVERLAP_ACROSS_PARTS);
    if (TestNotNull(TEXT("ordinary-channel overlap is reported"), Overlap))
    {
        TestTrue(TEXT("ordinary overlap is a warning"),
            Overlap->Severity == EPwSeverity::Warning);
        TestTrue(TEXT("the warning names channel 2"),
            Overlap->Message.Contains(TEXT("channel 2")));
        TestTrue(TEXT("the warning names both source parts"),
            Overlap->Message.Contains(TEXT("first"))
            && Overlap->Message.Contains(TEXT("second")));
        TestEqual(TEXT("ordinary overlap anchors to the last contributing uv op"),
            Overlap->Line, 8);
        TestEqual(TEXT("the anchor retains part scope"), Overlap->ScopeLabel,
            FString(TEXT("part")));
        TestEqual(TEXT("the anchor retains its part name"), Overlap->ScopeName,
            FString(TEXT("second")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerDeclaredLightmapUVIsUsableTest,
    "PinWright.Model.Report.DeclaredLightmapChannelMustBeUsable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerDeclaredLightmapUVIsUsableTest::RunTest(const FString& Parameters)
{
    const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n    box size=(50, 50, 50)\n}\n")
        TEXT("lightmap channel=1 resolution=64\n"));

    TestFalse(*FString::Printf(TEXT("an absent declared lightmap channel fails. %s"),
        *PwModelCompilerTest_Describe(Result)), Result.bSuccess);
    const FPwDiagnostic* Invalid = PwModelCompilerTest_FindCode(
        Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_LIGHTMAP_UV_INVALID);
    if (TestNotNull(TEXT("the unusable lightmap channel has a stable diagnostic"), Invalid))
    {
        TestEqual(TEXT("the diagnostic anchors to lightmap"), Invalid->Line, 5);
        TestEqual(TEXT("the diagnostic has model provenance"), Invalid->ScopeLabel,
            FString(TEXT("model")));
        TestTrue(TEXT("the diagnostic names channel 1"),
            Invalid->Message.Contains(TEXT("channel 1")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerModelUVLayoutPacksMergedPartsTest,
    "PinWright.Model.Compiler.ModelUVLayoutPacksMergedPartsAtWorldDensity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerModelUVLayoutPacksMergedPartsTest::RunTest(const FString& Parameters)
{
    const FString Parts =
        TEXT("pwmodel 0\n")
        TEXT("part first {\n")
        TEXT("    box size=(50, 50, 50)\n")
        TEXT("    uv channel=1 mode=box\n")
        TEXT("    uv channel=1 mode=layout texture_resolution=64\n")
        TEXT("}\n")
        TEXT("part second at=(200, 0, 0) allow_floating=true {\n")
        TEXT("    box size=(100, 100, 100)\n")
        TEXT("    uv channel=1 mode=box\n")
        TEXT("    uv channel=1 mode=layout texture_resolution=64\n")
        TEXT("}\n");

    const FString UnfixedSource = Parts + TEXT("lightmap channel=1 resolution=64\n");
    const FPwModelCompileResult Unfixed = PwModelCompilerTest_Validate(*UnfixedSource);
    TestTrue(TEXT("the counterfactual reaches the production overlap detector"),
        PwModelCompilerTest_HasCode(Unfixed.Diagnostics,
            PwModelDiagnosticCodes::PWMODEL_UV_OVERLAP_ACROSS_PARTS));

    const FString FixedSource = Parts
        + TEXT("uv_layout channel=0 texture_resolution=64\n")
        + TEXT("uv_layout channel=1 texture_resolution=64\n")
        + TEXT("lightmap channel=1 resolution=64\n");
    const FString AssetPath = PwModelCompilerTest_AssetPath(TEXT("ModelUVLayoutWorldDensity"));
    PwModelCompilerTest_DeleteIfPresent(AssetPath);
    const FPwModelCompileResult Fixed = PwModelCompilerTest_CompileTo(*FixedSource, AssetPath);
    TestTrue(*FString::Printf(TEXT("the merged layout produces a clean final atlas. %s"),
        *PwModelCompilerTest_Describe(Fixed)), Fixed.bSuccess);
    TestFalse(TEXT("a clean final atlas has no cross-part overlap error"),
        PwModelCompilerTest_HasCode(Fixed.Diagnostics,
            PwModelDiagnosticCodes::PWMODEL_UV_OVERLAP_ACROSS_PARTS));
    TestEqual(TEXT("the production layout preserves all box triangles"),
        Fixed.MeshTriangleCount, 24);

    UStaticMesh* Mesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(AssetPath));
    if (!Mesh)
    {
        AddError(FString::Printf(TEXT("expected a UStaticMesh at %s. %s"),
            *AssetPath, *PwModelCompilerTest_Describe(Fixed)));
    }
    else
    {
        FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});
        const FMeshDescription* Baked = Mesh->GetMeshDescription(0);
        if (!Baked)
        {
            AddError(TEXT("the global-layout asset has no LOD0 mesh description"));
        }
        else
        {
            FPwModelCompilerTest_UVAreaStats First;
            FPwModelCompilerTest_UVAreaStats Second;
            PwModelCompilerTest_MeasureUVAreaByX(*Baked, 1, 100.0f, First, Second);
            TestEqual(TEXT("the small part retains its triangles"), First.TriangleCount, 12);
            TestEqual(TEXT("the large part retains its triangles"), Second.TriangleCount, 12);
            TestTrue(TEXT("the packed UVs stay in the unit square"),
                First.bInsideUnitSquare && Second.bInsideUnitSquare);
            TestFalse(TEXT("the packed UVs have no positive-area cross-part overlap"),
                PwModelCompilerTest_HasPositiveUVOverlap(First, Second));
            TestTrue(TEXT("both parts retain positive world and UV area"),
                First.WorldArea > 0.0 && Second.WorldArea > 0.0
                && First.UVArea > 0.0 && Second.UVArea > 0.0);
            if (First.WorldArea > 0.0 && Second.WorldArea > 0.0
                && First.UVArea > 0.0 && Second.UVArea > 0.0)
            {
                const double FirstDensity = First.UVArea / First.WorldArea;
                const double SecondDensity = Second.UVArea / Second.WorldArea;
                TestTrue(*FString::Printf(
                    TEXT("Normalize gives equal UV area per world area (%.9g vs %.9g)"),
                    FirstDensity, SecondDensity),
                    FMath::IsNearlyEqual(FirstDensity, SecondDensity, FirstDensity * 0.05));
            }
        }
    }

    PwModelCompilerTest_DeleteIfPresent(AssetPath);

    const FPwModelCompileResult MissingIslands = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\npart body {\n    box size=(50, 50, 50)\n}\nuv_layout channel=2\n"));
    const FPwDiagnostic* LayoutFailure = PwModelCompilerTest_FindCode(
        MissingIslands.Diagnostics, PwModelDiagnosticCodes::PWMODEL_OP_FAILED);
    if (TestNotNull(TEXT("model-level layout failure is reported"), LayoutFailure))
    {
        TestEqual(TEXT("model-level layout failure keeps its source line"),
            LayoutFailure->Line, 5);
        TestEqual(TEXT("model-level layout failure has model scope"),
            LayoutFailure->ScopeLabel, FString(TEXT("model")));
        TestTrue(TEXT("model-level layout failure has no stale part name"),
            LayoutFailure->ScopeName.IsEmpty());
    }

    const FPwModelCompileResult Unpackable = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    append_buffers vertices=[(0,0,0),(1,0,0),(0,1,0),(2,0,0),(3,0,0),(2,1,0),(4,0,0),(5,0,0),(4,1,0),(6,0,0),(7,0,0),(6,1,0),(8,0,0),(9,0,0),(8,1,0)] triangles=[(0,1,2),(3,4,5),(6,7,8),(9,10,11),(12,13,14)] uvs=[(0,0),(1,0),(0,1),(0,0),(1,0),(0,1),(0,0),(1,0),(0,1),(0,0),(1,0),(0,1),(0,0),(1,0),(0,1)]\n")
        TEXT("}\n")
        TEXT("uv_layout channel=0 texture_resolution=2\n"));
    TestFalse(*FString::Printf(TEXT("five islands cannot pack into four texels. %s"),
        *PwModelCompilerTest_Describe(Unpackable)), Unpackable.bSuccess);
    const FPwDiagnostic* PackFailure = PwModelCompilerTest_FindCode(
        Unpackable.Diagnostics, PwModelDiagnosticCodes::PWMODEL_OP_FAILED);
    if (TestNotNull(TEXT("StandardPack false reaches the model diagnostic"), PackFailure))
    {
        TestEqual(TEXT("packing failure anchors to uv_layout"), PackFailure->Line, 5);
        TestEqual(TEXT("packing failure keeps model scope"), PackFailure->ScopeLabel,
            FString(TEXT("model")));
        TestTrue(TEXT("packing failure names the channel and resolution"),
            PackFailure->Message.Contains(TEXT("channel 0"))
            && PackFailure->Message.Contains(TEXT("texture_resolution=2")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerIntersectingUVBoundsQuietTest,
    "PinWright.Model.Report.IntersectingUVBoundsWithoutTriangleOverlapAreQuiet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerIntersectingUVBoundsQuietTest::RunTest(const FString& Parameters)
{
    auto CompileTwoUVTriangles = [](const TCHAR* SecondUVs)
    {
        return PwModelCompilerTest_Validate(*FString::Printf(
            TEXT("pwmodel 0\n")
            TEXT("part first {\n")
            TEXT("    append_buffers vertices=[(0, 0, 0), (100, 0, 0), (0, 100, 0)] triangles=[(0, 1, 2)] uvs=[(0, 0), (1, 0), (0, 1)]\n")
            TEXT("}\n")
            TEXT("part second at=(200, 0, 0) allow_floating=true {\n")
            TEXT("    append_buffers vertices=[(0, 0, 0), (40, 0, 0), (0, 40, 0)] triangles=[(0, 1, 2)] uvs=[%s]\n")
            TEXT("}\n")
            TEXT("lightmap channel=0 resolution=64\n"), SecondUVs));
    };

    // Both triangles' UV AABBs overlap in positive area, but the triangles themselves are
    // separated by the parallel lines U+V=1 and U+V=1.6. A bounds-only implementation fails.
    const FPwModelCompileResult Result =
        CompileTwoUVTriangles(TEXT("(1, 1), (1, 0.6), (0.6, 1)"));

    TestTrue(*FString::Printf(TEXT("disjoint UV triangles stay valid. %s"),
        *PwModelCompilerTest_Describe(Result)), Result.bSuccess);
    TestFalse(TEXT("overlapping UV AABBs alone do not trigger the diagnostic"),
        PwModelCompilerTest_HasCode(Result.Diagnostics,
            PwModelDiagnosticCodes::PWMODEL_UV_OVERLAP_ACROSS_PARTS));

    const FPwModelCompileResult EdgeContact =
        CompileTwoUVTriangles(TEXT("(1, 0), (0, 1), (1, 1)"));
    TestTrue(*FString::Printf(TEXT("shared UV edge stays valid. %s"),
        *PwModelCompilerTest_Describe(EdgeContact)), EdgeContact.bSuccess);
    TestEqual(TEXT("shared UV edge produces no overlap diagnostic"),
        PwModelCompilerTest_CountCode(EdgeContact.Diagnostics,
            PwModelDiagnosticCodes::PWMODEL_UV_OVERLAP_ACROSS_PARTS), 0);

    const FPwModelCompileResult PointContact =
        CompileTwoUVTriangles(TEXT("(1, 0), (2, 0), (1, 1)"));
    TestTrue(*FString::Printf(TEXT("shared UV point stays valid. %s"),
        *PwModelCompilerTest_Describe(PointContact)), PointContact.bSuccess);
    TestEqual(TEXT("shared UV point produces no overlap diagnostic"),
        PwModelCompilerTest_CountCode(PointContact.Diagnostics,
            PwModelDiagnosticCodes::PWMODEL_UV_OVERLAP_ACROSS_PARTS), 0);

    // The overlap is a very thin sliver between two otherwise full-scale triangles. Its area is
    // below the old absolute 1e-12 cutoff but is still positive and therefore invalid.
    const FPwModelCompileResult TinyOverlap =
        CompileTwoUVTriangles(TEXT("(0.9999998, 0), (1.9999998, 0), (0.9999998, 1)"));
    TestFalse(*FString::Printf(TEXT("tiny positive-area overlap fails. %s"),
        *PwModelCompilerTest_Describe(TinyOverlap)), TinyOverlap.bSuccess);
    TestEqual(TEXT("tiny overlap produces exactly one stable diagnostic"),
        PwModelCompilerTest_CountCode(TinyOverlap.Diagnostics,
            PwModelDiagnosticCodes::PWMODEL_UV_OVERLAP_ACROSS_PARTS), 1);
    const FPwDiagnostic* TinyDiagnostic = PwModelCompilerTest_FindCode(
        TinyOverlap.Diagnostics, PwModelDiagnosticCodes::PWMODEL_UV_OVERLAP_ACROSS_PARTS);
    if (TestNotNull(TEXT("tiny append-buffer overlap has the stable diagnostic"), TinyDiagnostic))
    {
        TestEqual(TEXT("append-buffer overlap falls back to the lightmap source line"),
            TinyDiagnostic->Line, 8);
        TestEqual(TEXT("append-buffer fallback uses model scope"), TinyDiagnostic->ScopeLabel,
            FString(TEXT("model")));
        TestTrue(TEXT("append-buffer fallback has no invented part scope name"),
            TinyDiagnostic->ScopeName.IsEmpty());
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerUVOpWritingNothingFailsTest,
    "PinWright.Model.Compiler.UVOpThatWritesNoElementsFails",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerUVOpWritingNothingFailsTest::RunTest(const FString& Parameters)
{
    // `layout` only repacks islands that already exist, so on a freshly grown channel it packs
    // nothing and returns having written zero UV elements. Every engine UV routine reports that
    // outcome only through the GeometryScriptDebug argument, which is null at every call site, so
    // an op that trusts the return value reports success on an empty channel - and the part ships
    // untextured with a clean compile. The post-condition asks the mesh instead.
    const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(50, 50, 50)\n")
        TEXT("    uv channel=2 mode=layout\n")
        TEXT("}\n"));

    TestFalse(*FString::Printf(TEXT("a uv op that wrote nothing fails the compile. %s"),
        *PwModelCompilerTest_Describe(Result)), Result.bSuccess);
    TestTrue(*FString::Printf(TEXT("the failure is reported as an op failure. %s"),
        *PwModelCompilerTest_Describe(Result)),
        PwModelCompilerTest_HasCode(Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_OP_FAILED));

    const FPwDiagnostic* Failure =
        PwModelCompilerTest_FindCode(Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_OP_FAILED);
    if (Failure)
    {
        TestTrue(TEXT("the message names the empty channel"),
            Failure->Message.Contains(TEXT("channel 2")));
    }

    return true;
}

// ============================================================================
// Booleans
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerNestedBooleanRunsTest,
    "PinWright.Model.Compiler.NestedBooleanBlockBuildsAToolMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerNestedBooleanRunsTest::RunTest(const FString& Parameters)
{
    const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(80, 50, 40)\n")
        TEXT("    subtract {\n")
        TEXT("        sphere radius=18 at=(25, 0, 0)\n")
        TEXT("    }\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("compile succeeded. %s"), *PwModelCompilerTest_Describe(Result)),
        Result.bSuccess);
    TestFalse(TEXT("an intersecting boolean is not reported as a no-op"),
        PwModelCompilerTest_HasCode(Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_BOOLEAN_NO_EFFECT));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerDisjointBooleanFailsTest,
    "PinWright.Model.Compiler.DisjointBooleanFailsRatherThanSucceedingSilently",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerDisjointBooleanFailsTest::RunTest(const FString& Parameters)
{
    // The engine returns the target unmodified when the two meshes do not intersect. This
    // shipped as a WARNING and twelve examples went out green because of it - pipe_junction's
    // two `subtract` bores both reported "changed nothing" and model.compile still answered
    // success:true for a pipe with no hole through it. The author asked for material to be
    // removed and none was, so the compile fails: there is no reading of that result that is
    // still correct, and nothing downstream repairs it.
    const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(50, 50, 50)\n")
        TEXT("    subtract {\n")
        TEXT("        sphere radius=10 at=(5000, 0, 0)\n")
        TEXT("    }\n")
        TEXT("}\n"));

    TestFalse(*FString::Printf(TEXT("a boolean that changed nothing fails the compile. %s"),
        *PwModelCompilerTest_Describe(Result)), Result.bSuccess);
    TestTrue(*FString::Printf(TEXT("the failure is reported as PWMODEL_BOOLEAN_NO_EFFECT. %s"),
        *PwModelCompilerTest_Describe(Result)),
        PwModelCompilerTest_HasCode(Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_BOOLEAN_NO_EFFECT));

    // Severity, not just presence: the whole defect was that this code carried Warning, and
    // bSuccess is derived from PwDiagnosticsHaveError. A future edit that flips it back
    // would leave the code in place and the assertion above green.
    if (const FPwDiagnostic* NoEffect =
            PwModelCompilerTest_FindCode(Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_BOOLEAN_NO_EFFECT))
    {
        TestTrue(TEXT("PWMODEL_BOOLEAN_NO_EFFECT is an error, not a warning"),
            NoEffect->Severity == EPwSeverity::Error);
        TestTrue(TEXT("it is anchored on the boolean's own line"), NoEffect->Line > 0);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerDisjointBooleanWritesNoAssetTest,
    "PinWright.Model.Compiler.DisjointBooleanWritesNoAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerDisjointBooleanWritesNoAssetTest::RunTest(const FString& Parameters)
{
    // The half the severity change alone would NOT have delivered. Raising the diagnostic to
    // Error without also unwinding the op would let Run reach CreateAsset, write the broken
    // .uasset, and only THEN report failure - which contradicts FPwModelCompiler's "never
    // partially creates". RunBoolean returns false instead, so the failure unwinds through
    // RunOps -> BuildParts -> Run and nothing is created.
    const FString AssetPath = PwModelCompilerTest_AssetPath(TEXT("SM_DisjointBoolean"));
    PwModelCompilerTest_DeleteIfPresent(AssetPath);

    const FPwModelCompileResult Result = PwModelCompilerTest_CompileTo(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(50, 50, 50)\n")
        TEXT("    subtract {\n")
        TEXT("        sphere radius=10 at=(5000, 0, 0)\n")
        TEXT("    }\n")
        TEXT("}\n"),
        AssetPath);

    TestFalse(*FString::Printf(TEXT("the compile failed. %s"), *PwModelCompilerTest_Describe(Result)),
        Result.bSuccess);
    TestTrue(TEXT("no asset path is reported"), Result.AssetPath.IsEmpty());
    TestFalse(TEXT("nothing was written at the output path"),
        UEditorAssetLibrary::DoesAssetExist(AssetPath));

    PwModelCompilerTest_DeleteIfPresent(AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerThroughCutOnUnsubdividedBoxTest,
    "PinWright.Model.Compiler.ThroughCutOnAnUnsubdividedBoxCompiles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerThroughCutOnUnsubdividedBoxTest::RunTest(const FString& Parameters)
{
    // A slab that takes the top off a default-tessellation box. It leaves a BOX - the same
    // twelve triangles it started with, two fifths of the material gone - and the no-effect
    // test used to be FOpResult::bChanged alone, which is a TRIANGLE-COUNT DELTA. So a cut
    // that had worked aborted the part, with a diagnostic asserting the operands did not
    // intersect.
    //
    // The engine was never involved: a boolean it declines fails bSuccess and is reported as
    // PWMODEL_OP_FAILED one branch earlier. Giving the TARGET a `segments=` made the identical
    // document compile only because a subdivided target lands on a different triangle count,
    // and giving the TOOL one never helped because the tool's count is not in that test. The
    // third compile below is the control that pins the two to the same solid.
    constexpr double Tol = 0.5;

    const FPwModelCompileResult Cut = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(100, 100, 100)\n")
        TEXT("    subtract {\n")
        TEXT("        box size=(300, 300, 100) at=(0, 0, 60)\n")
        TEXT("    }\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("a through-cut on an unsubdivided box compiles. %s"),
        *PwModelCompilerTest_Describe(Cut)), Cut.bSuccess);
    TestFalse(TEXT("a cut that removed two fifths of the target is not reported as a no-op"),
        PwModelCompilerTest_HasCode(Cut.Diagnostics, PwModelDiagnosticCodes::PWMODEL_BOOLEAN_NO_EFFECT));

    const FPwModelCompileResult Plain = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(100, 100, 100)\n")
        TEXT("}\n"));

    // THE COUNTERFACTUAL. Equal triangle counts across the cut is exactly the state the old
    // test read as "changed nothing". If this assertion ever stops holding, the one above has
    // stopped covering the defect and needs a new document rather than a green tick.
    if (Cut.bSuccess && Plain.bSuccess)
    {
        TestEqual(TEXT("the cut left the triangle count untouched - the state that used to abort"),
            Cut.MeshTriangleCount, Plain.MeshTriangleCount);
    }

    if (Cut.Parts.Num() == 1 && Cut.Parts[0].MeshBounds.IsValid != 0)
    {
        // 100 x 100 x 60 of the original 100 cube: the tool's underside sits at z = 10.
        TestEqual(TEXT("the cut plane is where the tool put it"),
            Cut.Parts[0].MeshBounds.Max.Z, 10.0, Tol);
        TestEqual(TEXT("the face the tool never reached is untouched"),
            Cut.Parts[0].MeshBounds.Min.Z, -50.0, Tol);
        TestEqual(TEXT("the remainder encloses the remainder's volume"),
            Cut.Parts[0].MeshSignedVolume, 600000.0, 1.0);
    }

    // The control row: the same cut on a target carrying segments=(3, 3, 3) always compiled.
    // It has to produce the same solid, which is what proves the operands were never at fault.
    const FPwModelCompileResult Subdivided = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(100, 100, 100) segments=(3, 3, 3)\n")
        TEXT("    subtract {\n")
        TEXT("        box size=(300, 300, 100) at=(0, 0, 60)\n")
        TEXT("    }\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("the subdivided control still compiles. %s"),
        *PwModelCompilerTest_Describe(Subdivided)), Subdivided.bSuccess);
    if (Cut.Parts.Num() == 1 && Subdivided.Parts.Num() == 1)
    {
        TestEqual(TEXT("target tessellation does not change the solid the cut produces"),
            Cut.Parts[0].MeshSignedVolume, Subdivided.Parts[0].MeshSignedVolume, 1.0);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerNoEffectNamesBothOperandsTest,
    "PinWright.Model.Compiler.BooleanNoEffectNamesBothOperandBounds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerNoEffectNamesBothOperandsTest::RunTest(const FString& Parameters)
{
    // The abort is correct here and stays. What could not be acted on was the message: it named
    // the op, told the author to move the cutter, and named no position for either body. The
    // part is aborted, so the response's parts[] holds only the parts that already succeeded,
    // and the tool never becomes geometry anyone can measure - so recovering where the
    // accumulated geometry had actually got to cost a delete-the-op-and-revalidate cycle per
    // attempt. Boxes rather than a sphere for both operands, so the bounds are exact digits.
    const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(50, 50, 50)\n")
        TEXT("    subtract {\n")
        TEXT("        box size=(20, 20, 20) at=(200, 0, 0)\n")
        TEXT("    }\n")
        TEXT("}\n"));

    TestFalse(*FString::Printf(TEXT("a boolean that changed nothing still fails the compile. %s"),
        *PwModelCompilerTest_Describe(Result)), Result.bSuccess);
    TestTrue(*FString::Printf(TEXT("it is reported as PWMODEL_BOOLEAN_NO_EFFECT. %s"),
        *PwModelCompilerTest_Describe(Result)),
        PwModelCompilerTest_HasCode(Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_BOOLEAN_NO_EFFECT));

    if (const FPwDiagnostic* NoEffect =
            PwModelCompilerTest_FindCode(Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_BOOLEAN_NO_EFFECT))
    {
        TestTrue(*FString::Printf(TEXT("the message names the accumulated geometry's bounds. '%s'"),
            *NoEffect->Message),
            NoEffect->Message.Contains(TEXT("(-25, -25, -25)..(25, 25, 25)")));
        TestTrue(*FString::Printf(TEXT("the message names the tool's bounds. '%s'"), *NoEffect->Message),
            NoEffect->Message.Contains(TEXT("(190, -10, -10)..(210, 10, 10)")));
        TestTrue(*FString::Printf(TEXT("the message names how far apart they are. '%s'"), *NoEffect->Message),
            NoEffect->Message.Contains(TEXT("(165, 0, 0)")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerNoEffectDoesNotClaimDisjointTest,
    "PinWright.Model.Compiler.BooleanNoEffectOverOverlappingBoundsDoesNotClaimDisjoint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerNoEffectDoesNotClaimDisjointTest::RunTest(const FString& Parameters)
{
    // An intersection whose tool CONTAINS the target returns the target: a genuine no-effect
    // boolean, and one whose operands could not overlap harder. The old message answered it
    // with "the block's geometry does not intersect the geometry accumulated so far", which is
    // a strictly stronger claim than the condition it was made from - all that was tested is
    // that the target came back unmoved - and it is flatly false here.
    const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(100, 100, 100)\n")
        TEXT("    intersection {\n")
        TEXT("        box size=(300, 300, 300)\n")
        TEXT("    }\n")
        TEXT("}\n"));

    TestFalse(*FString::Printf(TEXT("a boolean that changed nothing fails the compile. %s"),
        *PwModelCompilerTest_Describe(Result)), Result.bSuccess);
    TestTrue(*FString::Printf(TEXT("it is reported as PWMODEL_BOOLEAN_NO_EFFECT. %s"),
        *PwModelCompilerTest_Describe(Result)),
        PwModelCompilerTest_HasCode(Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_BOOLEAN_NO_EFFECT));

    if (const FPwDiagnostic* NoEffect =
            PwModelCompilerTest_FindCode(Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_BOOLEAN_NO_EFFECT))
    {
        TestFalse(*FString::Printf(TEXT("it does not claim the operands miss each other. '%s'"),
            *NoEffect->Message),
            NoEffect->Message.Contains(TEXT("does not intersect")));
        TestFalse(*FString::Printf(TEXT("it does not claim the boxes fail to meet. '%s'"),
            *NoEffect->Message),
            NoEffect->Message.Contains(TEXT("do not meet")));
        TestTrue(*FString::Printf(TEXT("it says the boxes overlap, and by how much. '%s'"),
            *NoEffect->Message),
            NoEffect->Message.Contains(TEXT("DO overlap, by 100 x 100 x 100 uu")));
        TestTrue(*FString::Printf(TEXT("it still names both operands. '%s'"), *NoEffect->Message),
            NoEffect->Message.Contains(TEXT("(-50, -50, -50)..(50, 50, 50)"))
            && NoEffect->Message.Contains(TEXT("(-150, -150, -150)..(150, 150, 150)")));
    }

    return true;
}

// ============================================================================
// Merged-mesh validation
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerEmptyMergedMeshFailsTest,
    "PinWright.Model.Compiler.EmptyMergedMeshFailsBeforeCreation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerEmptyMergedMeshFailsTest::RunTest(const FString& Parameters)
{
    // procedural_mesh is the one generator that appends nothing, so a part built only from it
    // reaches the merge with no triangles at all.
    const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    procedural_mesh\n")
        TEXT("}\n"));

    TestFalse(*FString::Printf(TEXT("compile failed. %s"), *PwModelCompilerTest_Describe(Result)),
        Result.bSuccess);
    TestTrue(TEXT("the empty merged mesh is reported"),
        PwModelCompilerTest_HasCode(Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_EMPTY_MESH));

    return true;
}

// ============================================================================
// Reproducibility
//
// The format's central claim is that the TEXT is the durable artefact and the asset is
// derived - which is only true if compiling one source twice lands on the same mesh. Nothing
// else in this file can fail when that stops holding: every other test compiles once, so a
// compile that produced a different result each run would pass all of them.
//
// The source below is deliberately the awkward one rather than a bare box. It carries the
// four stages with a plausible route to run-to-run variation, so the test fails on any of
// them rather than only on the compiler's own bookkeeping:
//
//   - a SEEDED noise_deform, whose whole purpose is to make the geometry seed-dependent;
//   - a boolean, which retriangulates along an intersection curve;
//   - an XAtlas unwrap, whose chart packing is the engine call most likely to carry
//     internal randomness;
//   - `collision { auto ... }`, whose convex decomposition is the other one.
//
// Assertions are on the counts, the material/collision element tallies and the built asset's
// BOUNDS. Bounds are the cheap proxy for "the vertices landed in the same places": a compile
// that reshuffled ordering but produced the same shape keeps them, while one that resampled
// noise from a different seed moves them. This does NOT pin vertex ORDER - see
// docs/pwmodel-format.md for what is and is not guaranteed.
// ============================================================================

namespace
{
// The ops whose determinism the FORMAT controls: a seeded noise field and a boolean that
// retriangulates along an intersection curve.
const TCHAR* const PwModelCompilerTest_ReproSource =
    TEXT("pwmodel 0\n")
    TEXT("part body {\n")
    TEXT("    box size=(80, 60, 40) segments=(4, 4, 4)\n")
    TEXT("    noise_deform magnitude=6 frequency=0.05 seed=4242\n")
    TEXT("    subtract {\n")
    TEXT("        sphere radius=22 at=(30, 0, 0)\n")
    TEXT("    }\n")
    TEXT("}\n");

// The two engine SOLVERS, kept in their own test so a regression is attributable to them
// rather than to the compiler: XAtlas chart packing and the convex decomposition behind
// `collision { auto }`.
//
// DELIBERATELY MULTI-PART, and that is the whole point. The engine splits the merged mesh
// into connected components and runs the hull generator with one ParallelFor task per
// component, so a SINGLE-component document reduces to ParallelFor(1, ...) and cannot
// observe the scheduling order at all. The first version of this fixture was one part,
// measured clean, and was measuring nothing: eight disjoint EQUAL-VOLUME boxes are what
// exercises the parallel append and the unstable volume sort downstream of it.
const TCHAR* const PwModelCompilerTest_ReproSolverSource =
    TEXT("pwmodel 0\n")
    TEXT("part a { box size=(20, 20, 20) at=(-90, 0, 0) }\n")
    TEXT("part b { box size=(20, 20, 20) at=(-45, 0, 0) }\n")
    TEXT("part c { box size=(20, 20, 20) at=(0, 0, 0) }\n")
    TEXT("part d { box size=(20, 20, 20) at=(45, 0, 0) }\n")
    TEXT("part e { box size=(20, 20, 20) at=(90, 0, 0) }\n")
    TEXT("part f { box size=(20, 20, 20) at=(0, 45, 0) }\n")
    TEXT("part g { box size=(20, 20, 20) at=(0, 90, 0) }\n")
    TEXT("part h { box size=(20, 20, 20) at=(0, -45, 0) }\n")
    TEXT("collision {\n")
    TEXT("    auto method=convex_hulls max_hulls=4\n")
    TEXT("}\n");

// The XAtlas half, kept separate so a chart-packing regression does not read as a
// collision one.
const TCHAR* const PwModelCompilerTest_ReproUnwrapSource =
    TEXT("pwmodel 0\n")
    TEXT("part body {\n")
    TEXT("    box size=(80, 60, 40) segments=(3, 3, 3)\n")
    TEXT("    subtract {\n")
    TEXT("        sphere radius=22 at=(30, 0, 0)\n")
    TEXT("    }\n")
    TEXT("    uv channel=0 mode=xatlas\n")
    TEXT("}\n");

// The generated collision hulls, IN ARRAY ORDER, one descriptor per element.
//
// Order is the whole point. The hull SET was never the unstable part - the engine's parallel
// generator produced the same eight hulls every run and only their array position moved, which is
// invisible to every count this file asserts and invisible to physics, but is serialized into the
// .uasset. Measured on UE 5.8 before the fix: one eight-part document compiled four times gave
// four DISTINCT orderings. PwModelCollision's canonical sort is what pins it; this is the
// assertion that fails if that sort is removed.
TArray<FString> PwModelCompilerTest_HullSequence(const FString& AssetPath)
{
    TArray<FString> Hulls;
    UStaticMesh* Mesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(AssetPath));
    if (!Mesh)
    {
        return Hulls;
    }
    const UBodySetup* Body = Mesh->GetBodySetup();
    if (!Body)
    {
        return Hulls;
    }
    for (const FKConvexElem& Elem : Body->AggGeom.ConvexElems)
    {
        // Bounds plus vertex count identifies a hull without depending on the vertex order INSIDE
        // it, which is a separate engine property this test is not trying to pin.
        Hulls.Add(FString::Printf(TEXT("%s|%s|%d"),
            *Elem.ElemBox.Min.ToString(), *Elem.ElemBox.Max.ToString(), Elem.VertexData.Num()));
    }
    return Hulls;
}

// The built asset's bounds. Read after StaticMeshCompiler has finished, because a mesh still
// in the async build queue reports the default-constructed box and two of those compare equal
// - which would make this test pass for the one reason that proves nothing.
bool PwModelCompilerTest_TryReadBounds(const FString& AssetPath, FBox& OutBounds)
{
    UStaticMesh* Mesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(AssetPath));
    if (!Mesh)
    {
        return false;
    }
    FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});

    const FMeshDescription* Baked = Mesh->GetMeshDescription(0);
    if (!Baked || Baked->Vertices().Num() == 0)
    {
        return false;
    }

    const FStaticMeshConstAttributes Attributes(*Baked);
    const TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();

    OutBounds = FBox(ForceInit);
    for (const FVertexID VertexID : Baked->Vertices().GetElementIDs())
    {
        OutBounds += FVector(Positions[VertexID]);
    }
    return true;
}

// Compile one source into TWO output paths and require the two results to agree.
//
// Two paths rather than one, deliberately: a second compile over an existing asset takes the
// UpdateInPlace branch of AssetCreatePolicy - a different code path with its own provenance
// and referencer gates - so "compile twice into one path" would be testing the update path,
// not reproducibility. Distinct paths keep both runs on the create branch, which is the one
// whose output has to be a function of the source alone.
void PwModelCompilerTest_AssertReproduces(FAutomationTestBase& T, const TCHAR* Source,
                                          const TCHAR* LeafA, const TCHAR* LeafB)
{
    const FString FirstPath = PwModelCompilerTest_AssetPath(LeafA);
    const FString SecondPath = PwModelCompilerTest_AssetPath(LeafB);
    PwModelCompilerTest_DeleteIfPresent(FirstPath);
    PwModelCompilerTest_DeleteIfPresent(SecondPath);

    const FPwModelCompileResult First = PwModelCompilerTest_CompileTo(Source, FirstPath);
    const FPwModelCompileResult Second = PwModelCompilerTest_CompileTo(Source, SecondPath);

    T.TestTrue(*FString::Printf(TEXT("first compile succeeded. %s"), *PwModelCompilerTest_Describe(First)),
        First.bSuccess);
    T.TestTrue(*FString::Printf(TEXT("second compile succeeded. %s"), *PwModelCompilerTest_Describe(Second)),
        Second.bSuccess);

    // Without this, a compiler that reported nothing at all would satisfy every equality below.
    T.TestTrue(*FString::Printf(TEXT("the fixture produced triangles. %s"), *PwModelCompilerTest_Describe(First)),
        First.MeshTriangleCount > 0);

    T.TestEqual(*FString::Printf(TEXT("triangle count reproduces. %s vs %s"),
            *PwModelCompilerTest_Describe(First), *PwModelCompilerTest_Describe(Second)),
        Second.MeshTriangleCount, First.MeshTriangleCount);
    T.TestEqual(TEXT("vertex count reproduces"), Second.MeshVertexCount, First.MeshVertexCount);
    T.TestEqual(TEXT("material slot count reproduces"), Second.MaterialSlots, First.MaterialSlots);
    // The half of `collision { auto }` a nondeterministic decomposition would move without
    // touching a single render triangle.
    T.TestEqual(TEXT("collision element count reproduces"),
        Second.CollisionElements, First.CollisionElements);
    T.TestEqual(TEXT("part count reproduces"), Second.Parts.Num(), First.Parts.Num());

    const TArray<FString> FirstHulls = PwModelCompilerTest_HullSequence(FirstPath);
    const TArray<FString> SecondHulls = PwModelCompilerTest_HullSequence(SecondPath);
    T.TestEqual(TEXT("hull count reproduces"), SecondHulls.Num(), FirstHulls.Num());
    if (FirstHulls.Num() == SecondHulls.Num())
    {
        for (int32 Index = 0; Index < FirstHulls.Num(); ++Index)
        {
            T.TestEqual(*FString::Printf(TEXT("hull %d reproduces IN ORDER"), Index),
                SecondHulls[Index], FirstHulls[Index]);
        }
    }

    // NOT asserted: bSavedToDisk (a function of file mtime and the pre-save dirty flag, not of
    // the source), AssetPath (different by construction), and diagnostic ORDER (emitted in
    // TMap bucket order, which is stable but is not the source's order).

    FBox FirstBounds(ForceInit);
    FBox SecondBounds(ForceInit);
    const bool bReadFirst = PwModelCompilerTest_TryReadBounds(FirstPath, FirstBounds);
    const bool bReadSecond = PwModelCompilerTest_TryReadBounds(SecondPath, SecondBounds);

    T.TestTrue(TEXT("the first asset's baked mesh is readable"), bReadFirst);
    T.TestTrue(TEXT("the second asset's baked mesh is readable"), bReadSecond);

    if (bReadFirst && bReadSecond)
    {
        // Exact, not approximate. Two runs of the same float arithmetic on one machine agree
        // bit for bit; a tolerance here would hide precisely the drift being tested for.
        T.TestTrue(*FString::Printf(TEXT("bounds minimum reproduces. %s vs %s"),
                *FirstBounds.Min.ToString(), *SecondBounds.Min.ToString()),
            SecondBounds.Min == FirstBounds.Min);
        T.TestTrue(*FString::Printf(TEXT("bounds maximum reproduces. %s vs %s"),
                *FirstBounds.Max.ToString(), *SecondBounds.Max.ToString()),
            SecondBounds.Max == FirstBounds.Max);
    }

    PwModelCompilerTest_DeleteIfPresent(FirstPath);
    PwModelCompilerTest_DeleteIfPresent(SecondPath);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerReproducibleTest,
    "PinWright.Model.Compiler.RecompilingOneSourceReproducesTheSameMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerReproducibleTest::RunTest(const FString& Parameters)
{
    PwModelCompilerTest_AssertReproduces(
        *this, PwModelCompilerTest_ReproSource, TEXT("ReproA"), TEXT("ReproB"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerReproducibleSolversTest,
    "PinWright.Model.Compiler.RecompilingSolverOpsReproducesTheSameMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerReproducibleSolversTest::RunTest(const FString& Parameters)
{
    PwModelCompilerTest_AssertReproduces(
        *this, PwModelCompilerTest_ReproSolverSource, TEXT("ReproSolverA"), TEXT("ReproSolverB"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerReproducibleUnwrapTest,
    "PinWright.Model.Compiler.RecompilingAnXAtlasUnwrapReproducesTheSameMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerReproducibleUnwrapTest::RunTest(const FString& Parameters)
{
    PwModelCompilerTest_AssertReproduces(
        *this, PwModelCompilerTest_ReproUnwrapSource, TEXT("ReproUnwrapA"), TEXT("ReproUnwrapB"));
    return true;
}

// ============================================================================
// shell on a UV-less open mesh used to take the whole editor down
// ============================================================================
//
// This is the regression test for a HARD CRASH, not for a diagnostic's wording. The document
// below - reproduced against a live editor twice - ended the process with:
//
//   EXCEPTION_ACCESS_VIOLATION
//     CalculateAverageUVScale<FJoinMeshLoops::Apply::lambda_1>  JoinMeshLoops.cpp:54
//     <- FJoinMeshLoops::Apply                                  JoinMeshLoops.cpp:82
//     <- ApplyMeshShell                                         MeshModelingFunctions.cpp:527
//     <- GeometryOps::Shell()
//     <- PwModelCompilerPrivate::FCompiler::DispatchModifier()
//
// ApplyMeshShell stitches the offset surface onto the original along every boundary loop, and
// the stitcher reads Mesh->Attributes()->PrimaryUV() with no null check (JoinMeshLoops.cpp:81 -
// the two lines below it in the same engine function both test Attributes() first). The mesh
// here has no usable UV channel because AppendBuffersToMesh calls SetNumUVLayers EXACTLY with
// the number of UV sets it was handed (MeshBasicEditFunctions.cpp:983), so a buffer append with
// no `uvs=` DELETES the default layer EnableAttributes had just created.
//
// If the guard in GeometryOps::Shell is removed, this test does not fail - the automation
// process dies mid-run and takes every other test in the suite with it. That is the point: a
// .pwmodel document is text, and no text file should be able to kill the editor.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerShellUVlessOpenMeshTest,
    "PinWright.Model.Compiler.ShellOnAUVlessOpenMeshDiagnosesRatherThanCrashes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerShellUVlessOpenMeshTest::RunTest(const FString& Parameters)
{
    const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part nouv_shell {\n")
        TEXT("    procedural_mesh\n")
        TEXT("    append_buffers vertices=[(0,0,0), (100,0,0), (100,100,0), (0,100,0)] triangles=[(0,1,2), (0,2,3)]\n")
        TEXT("    shell thickness=5\n")
        TEXT("}\n"));

    TestFalse(*FString::Printf(TEXT("the crashing document fails. %s"),
            *PwModelCompilerTest_Describe(Result)), Result.bSuccess);
    TestTrue(*FString::Printf(TEXT("it fails as an op failure. %s"),
            *PwModelCompilerTest_Describe(Result)),
        PwModelCompilerTest_HasCode(Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_OP_FAILED));

    // Line-anchored on the shell op, not on the part or the file. An author who cannot see
    // WHICH op refused has to bisect the document by hand, which for a crash-shaped defect is
    // exactly the loop that ends in another crash.
    if (const FPwDiagnostic* Failure = PwModelCompilerTest_FindCode(
            Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_OP_FAILED))
    {
        TestEqual(TEXT("the diagnostic points at the shell op line"), Failure->Line, 5);
        TestEqual(TEXT("it names the part"), Failure->ScopeName, FString(TEXT("nouv_shell")));
        TestTrue(*FString::Printf(TEXT("the message names the missing UVs. '%s'"), *Failure->Message),
            Failure->Message.Contains(TEXT("UV")));
    }

    return true;
}

// The same shape with UVs present is the control: it proves the guard rejects the UV-less mesh
// and not `shell` on an open mesh generally, which would be a far larger behaviour change than
// the crash it is fixing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerShellWithUVsStillWorksTest,
    "PinWright.Model.Compiler.ShellOnAnOpenMeshWithUVsStillCompiles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerShellWithUVsStillWorksTest::RunTest(const FString& Parameters)
{
    const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part uv_shell {\n")
        TEXT("    plane size=(100, 100)\n")
        TEXT("    shell thickness=5\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("shelling a UV-carrying open mesh still succeeds. %s"),
            *PwModelCompilerTest_Describe(Result)), Result.bSuccess);
    TestTrue(*FString::Printf(TEXT("and it produced the solid. %s"),
            *PwModelCompilerTest_Describe(Result)), Result.MeshTriangleCount > 2);
    return true;
}

// A CLOSED UV-less mesh is deliberately exempt: FMeshBoundaryLoops returns an empty loop list
// for it, so FJoinMeshLoops is never constructed and the crash is unreachable. Pinned so that
// tightening the guard to "no UVs, refuse" - which is the obvious simplification - shows up as
// a behaviour change rather than passing silently.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerShellOnClosedUVlessMeshTest,
    "PinWright.Model.Compiler.ShellOnAClosedUVlessMeshIsStillAllowed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerShellOnClosedUVlessMeshTest::RunTest(const FString& Parameters)
{
    // A tetrahedron: four triangles, no boundary edges, and no uvs= on the append.
    const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part closed_nouv {\n")
        TEXT("    procedural_mesh\n")
        TEXT("    append_buffers vertices=[(0,0,0), (100,0,0), (50,100,0), (50,50,100)] ")
        TEXT("triangles=[(0,2,1), (0,1,3), (1,2,3), (2,0,3)]\n")
        TEXT("    shell thickness=5\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("a closed UV-less mesh still shells. %s"),
            *PwModelCompilerTest_Describe(Result)), Result.bSuccess);
    return true;
}

// The PARTIALLY-UV'd part, and the fix that stops the compiler producing one.
//
// HISTORY, because the assertions below reversed. This document used to FAIL, and the test used
// to assert that it did: `plane` fills channel 0, `append_buffers` with no `uvs=` then appends a
// triangle that carries none, and shell's guard refused the resulting part rather than letting
// the engine's boundary stitcher read Elements[-2] (JoinMeshLoops.cpp:54 - the ops-level test
// PinWright.Geometry.Ops.Modeling.ShellRefusesAnOpenMeshWhoseUVsMissSomeTriangles still pins
// that guard, on a hand-built mesh, and is where to look if this one changes meaning again).
//
// Refusing was correct and insufficient: shell was the ONLY op that refused. The same part
// reached the merge and the UStaticMesh bake untouched by any other guard, shipping triangles
// with no UVs and no diagnostic. The compiler now repairs the state instead of only some ops
// declining to trip over it - FCompiler::ReconcileUVChannelsForAppend box-projects whichever
// side of an append is missing a channel the other populates, so this document compiles.
//
// Why the repair has to live in the compiler and not in GeometryOps::AppendBuffers: that op
// already pads incoming buffers to preserve the TARGET's UV layers, but the compiler runs every
// generator into a FRESH SCRATCH mesh, so the target it inspects is empty and its preserve count
// is 0. FDynamicMeshEditor::AppendMesh then copies min(target, source) UV layers
// (DynamicMeshEditor.cpp:2002) and sets only the triangles the SOURCE had set (:2256).
//
// Ops that add triangles IN PLACE (bridge, edge_split, anything reaching
// FDynamicMesh3::AppendTriangle on the part mesh) are not on the append path and can still
// produce the state mid-part; FillMissingUVChannels sweeps for it at the merge - see
// PinWright.Model.Compiler.PartiallyUVdChannelsAreReprojectedAtTheMerge.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerAppendedGeometryGetsUVsTest,
    "PinWright.Model.Compiler.GeometryAppendedWithNoUVsIsGivenThemBeforeItReachesShell",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerAppendedGeometryGetsUVsTest::RunTest(const FString& Parameters)
{
    const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part partial_uv {\n")
        TEXT("    plane size=(100, 100)\n")
        TEXT("    append_buffers vertices=[(500,0,0), (600,0,0), (600,100,0)] triangles=[(0,1,2)]\n")
        TEXT("    shell thickness=5\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("the once-failing document now compiles. %s"),
            *PwModelCompilerTest_Describe(Result)), Result.bSuccess);
    TestFalse(*FString::Printf(TEXT("and shell no longer refuses it. %s"),
            *PwModelCompilerTest_Describe(Result)),
        PwModelCompilerTest_HasCode(Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_OP_FAILED));

    // NOT silent. The appended triangle got UVs the author did not write, which is exactly the
    // kind of invention that has to be reported or it becomes folklore. ONE warning, model-wide.
    TestEqual(*FString::Printf(TEXT("exactly one UV-fill warning. %s"),
            *PwModelCompilerTest_Describe(Result)),
        PwModelCompilerTest_CountCode(Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_UV_CHANNEL_FILLED), 1);

    if (const FPwDiagnostic* Filled = PwModelCompilerTest_FindCode(
            Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_UV_CHANNEL_FILLED))
    {
        TestTrue(*FString::Printf(TEXT("the warning names the part. %s"), *Filled->Message),
            Filled->Message.Contains(TEXT("partial_uv")));
        TestTrue(*FString::Printf(TEXT("and the channel. %s"), *Filled->Message),
            Filled->Message.Contains(TEXT("channel 0")));
    }

    // The control that keeps the repair from being "always box-project everything": a part whose
    // UV state is uniform - here wholly ABSENT, which is what `procedural_mesh` + `append_buffers`
    // produces - must come out untouched and warning-free. Padding it would change the shape the
    // shell and bevel crash guards in GeometryOps_Modeling.cpp are written and tested against.
    const FPwModelCompileResult Uniform = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part uniform_nouv {\n")
        TEXT("    procedural_mesh\n")
        TEXT("    append_buffers vertices=[(500,0,0), (600,0,0), (600,100,0)] triangles=[(0,1,2)]\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("a wholly UV-less part still compiles. %s"),
            *PwModelCompilerTest_Describe(Uniform)), Uniform.bSuccess);
    TestEqual(*FString::Printf(TEXT("and is not padded - nothing to reconcile against. %s"),
            *PwModelCompilerTest_Describe(Uniform)),
        PwModelCompilerTest_CountCode(Uniform.Diagnostics, PwModelDiagnosticCodes::PWMODEL_UV_CHANNEL_FILLED), 0);

    return true;
}

// The IN-PLACE half, which the append reconciliation above cannot reach: `bridge` builds its
// strip with raw FDynamicMesh3::AppendTriangle calls on the part mesh itself
// (GeometryOps::Bridge), so the new triangles are unset in channel 0 while the two planes'
// elements keep ElementCount() positive.
//
// FillMissingUVChannels is the sweep that catches it, and until this change it did not: its
// per-part early-continue asked MeshHasUVElements, i.e. ElementCount() > 0, which a partially-set
// channel satisfies. So the compiler's own box-projection rescue skipped the one mesh shape it
// existed to repair. It now asks GeometryUtils::MeshHasUVsOnEveryTriangle for that channel.
//
// NOT EXECUTED when written: the change that added it was barred from running the editor and the
// automation suite (a concurrent session held the only editor). The compile-check is real; the
// runtime behaviour of `bridge` on two parallel planes is reasoned from its source, not observed.
// Assertions are kept to what holds regardless of how many triangles the strip has.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerPartialChannelReprojectedTest,
    "PinWright.Model.Compiler.PartiallyUVdChannelsAreReprojectedAtTheMerge",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerPartialChannelReprojectedTest::RunTest(const FString& Parameters)
{
    const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part bridged {\n")
        TEXT("    plane size=(100, 100)\n")
        TEXT("    plane size=(100, 100) at=(0, 0, 50)\n")
        TEXT("    bridge\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("the bridged document compiles. %s"),
            *PwModelCompilerTest_Describe(Result)), Result.bSuccess);

    // The strip really was built - otherwise the partial channel never existed and the rest of
    // this test would pass against a mesh that never reached the condition.
    TestTrue(*FString::Printf(TEXT("bridge added triangles beyond the two planes. %s"),
            *PwModelCompilerTest_Describe(Result)), Result.MeshTriangleCount > 4);

    // At most one, and if present it must be the REPROJECTED clause: the two planes populate
    // channel 0, so this is never the wholly-absent case.
    const int32 Warnings = PwModelCompilerTest_CountCode(
        Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_UV_CHANNEL_FILLED);
    TestTrue(*FString::Printf(TEXT("at most one UV-fill warning. %s"),
            *PwModelCompilerTest_Describe(Result)), Warnings <= 1);

    if (const FPwDiagnostic* Filled = PwModelCompilerTest_FindCode(
            Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_UV_CHANNEL_FILLED))
    {
        TestTrue(*FString::Printf(TEXT("the partial case reports a REPLACEMENT, not a fill. %s"),
                *Filled->Message), Filled->Message.Contains(TEXT("REPLACED")));
        TestTrue(*FString::Printf(TEXT("and it names the part. %s"), *Filled->Message),
            Filled->Message.Contains(TEXT("bridged")));
    }

    return true;
}

// ============================================================================
// sphere subdivisions has an effective floor of 2
// ============================================================================
//
// `sphere subdivisions=1` and `=2` both emit the identical 12-triangle / 8-vertex cube:
// AppendSphereBox's triangle count is 12*(N-1)^2 for N >= 2 and it draws the N=2 cube for N=1.
// GeometryUtils::ClampSegments only substitutes the default for values <= 0, so 1 was accepted,
// silently did nothing, and left the author looking for the bug in the next op instead.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerSphereSubdivisionFloorTest,
    "PinWright.Model.Compiler.SphereSubdivisionsBelowTwoIsClampedAndWarned",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerSphereSubdivisionFloorTest::RunTest(const FString& Parameters)
{
    const FPwModelCompileResult One = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part s1 { sphere radius=100 subdivisions=1 }\n"));
    const FPwModelCompileResult Two = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part s2 { sphere radius=100 subdivisions=2 }\n"));

    TestTrue(TEXT("subdivisions=1 still compiles"), One.bSuccess);
    TestTrue(TEXT("subdivisions=2 still compiles"), Two.bSuccess);

    // The clamp is REPORTED, which is the whole fix: the geometry was always going to be the
    // cube, and an author who asked for 1 needs to be told that 2 is what ran.
    TestTrue(*FString::Printf(TEXT("subdivisions=1 warns that it was clamped. %s"),
            *PwModelCompilerTest_Describe(One)),
        PwModelCompilerTest_HasCode(One.Diagnostics, PwModelDiagnosticCodes::PWMODEL_STAGE_WARNING));
    TestFalse(*FString::Printf(TEXT("subdivisions=2 is at the floor and warns about nothing. %s"),
            *PwModelCompilerTest_Describe(Two)),
        PwModelCompilerTest_HasCode(Two.Diagnostics, PwModelDiagnosticCodes::PWMODEL_STAGE_WARNING));

    // Pinned so a future switch from AppendSphereBox to a lat/long sphere - which would change
    // the polyhedron AND the count under every existing document - cannot land unnoticed.
    TestEqual(TEXT("subdivisions=1 emits the 12-triangle cube"), One.MeshTriangleCount, 12);
    TestEqual(TEXT("and so does subdivisions=2"), Two.MeshTriangleCount, 12);

    return true;
}

// ============================================================================
// Path-driven ops: sweep / extrude_along_spline / array_along_path
// ============================================================================
//
// What each of these would let through if it were reverted:
//
//  - `profile=` REACHES THE ENGINE CALL. GeometryOps::Sweep derived its cross-section from the
//    mesh's bounding box and nothing else, so an authored profile that is accepted by the parser
//    and dropped by the compiler compiles clean and silently sweeps a circle. Only a triangle
//    count that MOVES with the profile's vertex count can see that; the compile succeeds either
//    way. This is the same defect class as the 28 ops shipped narrower than the verb behind them.
//  - `cap=` REACHES IT TOO, for the same reason and with the same tell.
//  - A ONE-FRAME PATH IS A FAILURE, NOT A FALLBACK. Sweep selects its vertical fallback on
//    `Samples.Num() < 2`, so without the compiler's own check a one-frame path compiles green and
//    produces a vertical tube through the mesh's bounding box - geometry the document does not
//    describe, with no diagnostic. extrude_along_spline has no fallback and would produce nothing.
//  - array_along_path IS N COPIES FOR N FRAMES, not N+1. The alternative - leave the original in
//    place and append one copy per frame - doubles the geometry at the origin for every path that
//    starts there, and the only thing that distinguishes the two is the exact multiple.

namespace
{
// The merged triangle count of one validate-only compile, or -1 when it failed. Validate-only
// runs every engine op on a real mesh and skips only asset creation, so these counts are the
// real topology; the failure sentinel keeps a broken compile from reading as an empty mesh.
int32 PwModelCompilerTest_TriangleCountOf(FAutomationTestBase& Test, const TCHAR* Source)
{
    const FPwModelCompileResult Result = PwModelCompilerTest_Validate(Source);
    if (!Result.bSuccess)
    {
        Test.AddError(FString::Printf(TEXT("expected a clean compile. %s"),
            *PwModelCompilerTest_Describe(Result)));
        return -1;
    }
    return Result.MeshTriangleCount;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerSweepBuildsAlongThePathTest,
    "PinWright.Model.Compiler.SweepBuildsGeometryAlongTheAuthoredPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerSweepBuildsAlongThePathTest::RunTest(const FString& Parameters)
{
    const int32 BoxOnly = PwModelCompilerTest_TriangleCountOf(*this,
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(20, 20, 20)\n")
        TEXT("}\n"));

    const int32 TwoFrames = PwModelCompilerTest_TriangleCountOf(*this,
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(20, 20, 20)\n")
        TEXT("    sweep path=[(0, 0, 0, 0, 0, 0), (0, 0, 100, 0, 0, 0)]\n")
        TEXT("}\n"));

    const int32 FourFrames = PwModelCompilerTest_TriangleCountOf(*this,
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(20, 20, 20)\n")
        TEXT("    sweep path=[(0, 0, 0, 0, 0, 0), (0, 0, 50, 0, 0, 0), (0, 0, 100, 0, 0, 0), (0, 0, 150, 0, 0, 0)]\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("the sweep appends geometry (box %d -> %d)"), BoxOnly, TwoFrames),
        TwoFrames > BoxOnly);
    TestTrue(*FString::Printf(TEXT("a longer path sweeps more geometry (%d -> %d)"), TwoFrames, FourFrames),
        FourFrames > TwoFrames);

    // The box is still there afterwards: sweep APPENDS, it does not replace what sized it.
    TestTrue(*FString::Printf(TEXT("the appended count leaves room for the original box's %d triangles"), BoxOnly),
        TwoFrames - BoxOnly > 0 && BoxOnly == 12);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerSweepProfileReachesTheEngineTest,
    "PinWright.Model.Compiler.SweepProfileAndCapReachTheEngineCall",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerSweepProfileReachesTheEngineTest::RunTest(const FString& Parameters)
{
    const TCHAR* const PathLiteral = TEXT("path=[(0, 0, 0, 0, 0, 0), (0, 0, 100, 0, 0, 0)]");

    auto Sweep = [this, PathLiteral](const FString& Extra)
    {
        return PwModelCompilerTest_TriangleCountOf(*this, *FString::Printf(
            TEXT("pwmodel 0\npart body {\n    box size=(20, 20, 20)\n    sweep %s%s\n}\n"),
            PathLiteral, *Extra));
    };

    const int32 Triangular = Sweep(TEXT(" profile=[(0, 0), (10, 0), (5, 10)]"));
    const int32 Pentagonal = Sweep(TEXT(" profile=[(0, 0), (10, 0), (12, 8), (5, 14), (-2, 8)]"));
    const int32 Uncapped   = Sweep(TEXT(" profile=[(0, 0), (10, 0), (5, 10)] cap=false"));

    // A profile that is accepted and dropped gives the same circle every time, so equality here
    // is the whole failure signature.
    TestTrue(*FString::Printf(TEXT("profile vertex count drives the swept tube (3-gon %d, 5-gon %d)"),
        Triangular, Pentagonal), Pentagonal > Triangular);

    TestTrue(*FString::Printf(TEXT("cap=false removes the end caps (capped %d, uncapped %d)"),
        Triangular, Uncapped), Uncapped < Triangular);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerExtrudeAlongSplineBuildsTest,
    "PinWright.Model.Compiler.ExtrudeAlongSplineBuildsAClosedSweep",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerExtrudeAlongSplineBuildsTest::RunTest(const FString& Parameters)
{
    const int32 BoxOnly = PwModelCompilerTest_TriangleCountOf(*this,
        TEXT("pwmodel 0\npart body {\n    box size=(20, 20, 20)\n}\n"));

    const int32 Extruded = PwModelCompilerTest_TriangleCountOf(*this,
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(20, 20, 20)\n")
        TEXT("    extrude_along_spline profile=[(0, 0), (8, 0), (4, 8)] ")
        TEXT("path=[(0, 0, 0, 0, 0, 0), (60, 0, 0, 0, 0, 0), (60, 60, 0, 0, 0, 90)]\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("the extrude appends geometry (box %d -> %d)"), BoxOnly, Extruded),
        Extruded > BoxOnly);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerArrayAlongPathCopyCountTest,
    "PinWright.Model.Compiler.ArrayAlongPathPlacesOneCopyPerFrame",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerArrayAlongPathCopyCountTest::RunTest(const FString& Parameters)
{
    const int32 BoxOnly = PwModelCompilerTest_TriangleCountOf(*this,
        TEXT("pwmodel 0\npart body {\n    box size=(20, 20, 20)\n}\n"));

    const int32 ThreeFrames = PwModelCompilerTest_TriangleCountOf(*this,
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(20, 20, 20)\n")
        TEXT("    array_along_path path=[(0, 0, 0, 0, 0, 0), (100, 0, 0, 0, 0, 30), (200, 0, 0, 0, 0, 60)]\n")
        TEXT("}\n"));

    // Exactly three, not four: the original is the first frame's copy rather than a fourth body
    // left behind at the origin.
    TestEqual(TEXT("three frames produce exactly three copies"), ThreeFrames, BoxOnly * 3);

    // One frame is a single placement and must neither fail nor double the mesh.
    const int32 OneFrame = PwModelCompilerTest_TriangleCountOf(*this,
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(20, 20, 20)\n")
        TEXT("    array_along_path path=[(0, 0, 200, 0, 0, 0)]\n")
        TEXT("}\n"));

    TestEqual(TEXT("one frame produces exactly one copy"), OneFrame, BoxOnly);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerShortPathFailsTest,
    "PinWright.Model.Compiler.OneFramePathFailsRatherThanFallingBackSilently",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerShortPathFailsTest::RunTest(const FString& Parameters)
{
    const TCHAR* const Sources[] = {
        TEXT("pwmodel 0\npart body {\n    box size=(20, 20, 20)\n    sweep path=[(0, 0, 0, 0, 0, 0)]\n}\n"),
        TEXT("pwmodel 0\npart body {\n    box size=(20, 20, 20)\n    extrude_along_spline path=[(0, 0, 0, 0, 0, 0)]\n}\n"),
    };

    for (const TCHAR* Source : Sources)
    {
        const FPwModelCompileResult Result = PwModelCompilerTest_Validate(Source);

        TestFalse(*FString::Printf(TEXT("a one-frame path is refused. %s"),
            *PwModelCompilerTest_Describe(Result)), Result.bSuccess);
        TestTrue(*FString::Printf(TEXT("and it is reported as an op failure. %s"),
            *PwModelCompilerTest_Describe(Result)),
            PwModelCompilerTest_HasCode(Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_OP_FAILED));
    }

    // A profile too short to bound a face is the same class of silent fallback and is refused the
    // same way, rather than sweeping the bounding-box circle the author did not ask for.
    {
        const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
            TEXT("pwmodel 0\n")
            TEXT("part body {\n")
            TEXT("    box size=(20, 20, 20)\n")
            TEXT("    sweep profile=[(0, 0), (10, 0)] path=[(0, 0, 0, 0, 0, 0), (0, 0, 100, 0, 0, 0)]\n")
            TEXT("}\n"));

        TestFalse(*FString::Printf(TEXT("a two-point profile is refused. %s"),
            *PwModelCompilerTest_Describe(Result)), Result.bSuccess);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerDuplicateFramesTest,
    "PinWright.Model.Compiler.DuplicateConsecutivePathFramesCompile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerDuplicateFramesTest::RunTest(const FString& Parameters)
{
    // A zero-length segment is degenerate, not invalid: the engine's sweep and the append both
    // tolerate it, and refusing it would reject a legitimately-repeated control point. The
    // assertion is that the compile survives it and still reports geometry - the failure this
    // guards against is a crash or an empty mesh, not a triangle count.
    const TCHAR* const Sources[] = {
        TEXT("pwmodel 0\npart body {\n    box size=(20, 20, 20)\n")
        TEXT("    sweep path=[(0, 0, 0, 0, 0, 0), (0, 0, 100, 0, 0, 0), (0, 0, 100, 0, 0, 0), (0, 0, 200, 0, 0, 0)]\n}\n"),

        TEXT("pwmodel 0\npart body {\n    box size=(20, 20, 20)\n")
        TEXT("    extrude_along_spline path=[(0, 0, 0, 0, 0, 0), (0, 0, 100, 0, 0, 0), (0, 0, 100, 0, 0, 0)]\n}\n"),

        TEXT("pwmodel 0\npart body {\n    box size=(20, 20, 20)\n")
        TEXT("    array_along_path path=[(0, 0, 0, 0, 0, 0), (0, 0, 0, 0, 0, 0)]\n}\n"),
    };

    for (const TCHAR* Source : Sources)
    {
        const FPwModelCompileResult Result = PwModelCompilerTest_Validate(Source);

        TestTrue(*FString::Printf(TEXT("duplicate consecutive frames compile. %s"),
            *PwModelCompilerTest_Describe(Result)), Result.bSuccess);
        TestTrue(*FString::Printf(TEXT("and leave geometry behind. %s"),
            *PwModelCompilerTest_Describe(Result)), Result.MeshTriangleCount > 0);
    }

    return true;
}

// ============================================================================
// Siblings are APPENDED, not unioned
// ============================================================================
//
// The format's biggest authoring trap. Nothing in a part or a nested block merges two shapes:
// RunGenerator ends in AppendMesh. Two overlapping shapes therefore leave BURIED INTERIOR FACES
// and hand the next boolean a self-intersecting mesh - which is what left a white cross-shaped
// shard standing inside the gothic window's trefoil, three overlapping circles appended so the
// lobe partition walls survived inside the opening.
//
// The append is deliberately NOT changed to a union - that would alter every existing document
// and is wrong for disjoint parts and deliberate open shells - so the warning IS the fix.
//
// The negative cases below are the whole design. A plain FBox::Intersect passes the positive
// case and fails the flush-adjacent one, and a warning that fires on every adjacent pair is
// worse than no warning because it trains the author to ignore the one that matters.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerUnunionedOverlapTest,
    "PinWright.Model.Compiler.OverlappingSiblingsWarnAndAdjacentOnesDoNot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerUnunionedOverlapTest::RunTest(const FString& Parameters)
{
    // ---- fires: the second box lands 50 uu inside the first ------------------------------
    {
        const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
            TEXT("pwmodel 0\n")
            TEXT("part twin {\n")
            TEXT("    box size=(100, 100, 100)\n")
            TEXT("    box size=(100, 100, 100) at=(50, 0, 0)\n")
            TEXT("}\n"));

        TestTrue(*FString::Printf(TEXT("an overlapping pair still COMPILES - it is a warning, ")
                TEXT("never an error, because bounding boxes cannot prove the solids overlap. %s"),
                *PwModelCompilerTest_Describe(Result)), Result.bSuccess);

        const FPwDiagnostic* Warning = PwModelCompilerTest_FindCode(
            Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_UNUNIONED_OVERLAP);
        if (!Warning)
        {
            AddError(FString::Printf(TEXT("expected PWMODEL_UNUNIONED_OVERLAP. %s"),
                *PwModelCompilerTest_Describe(Result)));
            return false;
        }

        TestTrue(TEXT("it is a warning, never an error"),
            Warning->Severity == EPwSeverity::Warning);

        // Anchored on the SECOND op - the one the author would move or wrap - and naming the
        // first by line. An author who cannot see WHICH pair overlaps has to bisect by hand.
        TestEqual(TEXT("anchored on the second box, line 4"), Warning->Line, 4);
        TestTrue(*FString::Printf(TEXT("names the earlier op's line: '%s'"), *Warning->Message),
            Warning->Message.Contains(TEXT("line 3")));
        TestTrue(*FString::Printf(TEXT("names the fix: '%s'"), *Warning->Message),
            Warning->Message.Contains(TEXT("union { }")));

        // One per op, not one per pair. An op that lands inside four previous ones is one
        // authoring mistake, and four warnings for it would bury the next op's.
        TestEqual(TEXT("exactly one warning for one overlapping pair"),
            PwModelCompilerTest_CountCode(Result.Diagnostics,
                PwModelDiagnosticCodes::PWMODEL_UNUNIONED_OVERLAP), 1);
    }

    // ---- cross-slot: the union advice must stay true across a slot boundary -----------------
    //
    // A one-part model can legitimately append solids with different material slots, and this is
    // the case the advice used to have to hedge about: a generator inside a boolean block
    // allocated no slot, so unioning two differently-tagged solids silently dropped the tool's.
    // It no longer does - a block resolves `material=` through the same model-wide table - so
    // union is the answer here and the message must say so rather than steering the author away
    // from it. Keep this fixture synthetic and exercise both named bindings so a generic
    // string-presence check cannot pass by accident.
    {
        const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
            TEXT("pwmodel 0\n")
            TEXT("materials {\n")
            TEXT("    Shell = \"/Engine/BasicShapes/BasicShapeMaterial\"\n")
            TEXT("    Trim = \"/Engine/EngineMaterials/WorldGridMaterial\"\n")
            TEXT("}\n")
            TEXT("part mixed_slots {\n")
            TEXT("    box size=(100, 100, 100) material=\"Shell\"\n")
            TEXT("    box size=(100, 100, 100) at=(50, 0, 0) material=\"Trim\"\n")
            TEXT("}\n"));

        TestTrue(*FString::Printf(TEXT("cross-slot overlapping solids compile as a warning. %s"),
            *PwModelCompilerTest_Describe(Result)), Result.bSuccess);

        const FPwDiagnostic* Warning = PwModelCompilerTest_FindCode(
            Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_UNUNIONED_OVERLAP);
        if (!Warning)
        {
            AddError(FString::Printf(
                TEXT("expected the cross-slot overlap warning. %s"),
                *PwModelCompilerTest_Describe(Result)));
            return false;
        }

        // The advice used to be "use union { } ONLY when both solids use the same material slot",
        // because a generator inside a boolean block allocated no slot and its tag was discarded.
        // It is not discarded any more - the tag resolves through the same model-wide table a
        // part-level op uses - so union is now the answer here rather than the thing to avoid, and
        // this asserts the message says so. A stale "keep the slots matching" would send an author
        // to separate two solids that union resolves correctly.
        TestTrue(*FString::Printf(TEXT("union is offered as the resolution: '%s'"),
                *Warning->Message),
            Warning->Message.Contains(TEXT("union")));
        TestTrue(*FString::Printf(TEXT("and it says each solid keeps its own slot: '%s'"),
                *Warning->Message),
            Warning->Message.Contains(TEXT("keeps each solid's own material slot")));
        TestTrue(*FString::Printf(TEXT("separation is still offered as an alternative: '%s'"),
                *Warning->Message),
            Warning->Message.Contains(TEXT("move them apart")));
    }

    // ---- silent: FLUSH-ADJACENT, the commonest legitimate pattern in the format ----------
    //
    // A column on a base, a floor meeting a wall. The boxes share a face exactly, so
    // FBox::Intersect is TRUE and the intersection has zero thickness. This is the case the
    // positive-thickness rule exists for, and the one that decides whether the warning is
    // usable at all.
    {
        const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
            TEXT("pwmodel 0\n")
            TEXT("part stacked {\n")
            TEXT("    box size=(100, 100, 100)\n")
            TEXT("    box size=(100, 100, 100) at=(0, 0, 100)\n")
            TEXT("}\n"));

        TestTrue(*FString::Printf(TEXT("flush-stacked boxes compile. %s"),
            *PwModelCompilerTest_Describe(Result)), Result.bSuccess);
        TestFalse(*FString::Printf(TEXT("and do NOT warn - they share a face, they do not ")
                TEXT("interpenetrate. %s"), *PwModelCompilerTest_Describe(Result)),
            PwModelCompilerTest_HasCode(Result.Diagnostics,
                PwModelDiagnosticCodes::PWMODEL_UNUNIONED_OVERLAP));
    }

    // ---- silent: plainly disjoint --------------------------------------------------------
    {
        const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
            TEXT("pwmodel 0\n")
            TEXT("part apart {\n")
            TEXT("    box size=(100, 100, 100)\n")
            TEXT("    box size=(100, 100, 100) at=(1000, 0, 0)\n")
            TEXT("}\n"));

        TestFalse(*FString::Printf(TEXT("disjoint boxes do not warn. %s"),
                *PwModelCompilerTest_Describe(Result)),
            PwModelCompilerTest_HasCode(Result.Diagnostics,
                PwModelDiagnosticCodes::PWMODEL_UNUNIONED_OVERLAP));
    }

    // ---- fires INSIDE a nested block, which is where the trefoil defect actually lived ----
    //
    // The tool body of a `subtract` is built by RunOps exactly like a part, so two overlapping
    // shapes in it are appended too. Scope the footprint list to the PART instead of to the
    // MESH and this case goes silent while the part-level one above still passes.
    {
        const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
            TEXT("pwmodel 0\n")
            TEXT("part cut {\n")
            TEXT("    box size=(400, 400, 400)\n")
            TEXT("    subtract {\n")
            TEXT("        cylinder radius=50 height=500\n")
            TEXT("        cylinder radius=50 height=500 at=(40, 0, 0)\n")
            TEXT("    }\n")
            TEXT("}\n"));

        const FPwDiagnostic* Warning = PwModelCompilerTest_FindCode(
            Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_UNUNIONED_OVERLAP);
        if (!Warning)
        {
            AddError(FString::Printf(
                TEXT("expected the warning inside the subtract block too. %s"),
                *PwModelCompilerTest_Describe(Result)));
            return false;
        }
        TestEqual(TEXT("anchored on the second cylinder inside the block, line 6"),
            Warning->Line, 6);

        // And the part's own box is NOT compared against the block's cylinders, even though
        // they interpenetrate: the block is a separate mesh, and a boolean is exactly how those
        // are meant to be combined.
        TestEqual(TEXT("only the two siblings inside the block are compared"),
            PwModelCompilerTest_CountCode(Result.Diagnostics,
                PwModelDiagnosticCodes::PWMODEL_UNUNIONED_OVERLAP), 1);
    }

    // ---- silent after a modifier: recorded boxes stop describing the mesh ------------------
    //
    // Documented behaviour, not an oversight. `translate` moves geometry a footprint still
    // claims to describe, so the list is dropped. Coverage lost after the first modifier is the
    // price of never making a claim the compiler cannot support.
    {
        const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
            TEXT("pwmodel 0\n")
            TEXT("part moved {\n")
            TEXT("    box size=(100, 100, 100)\n")
            TEXT("    translate offset=(0, 0, 10)\n")
            TEXT("    box size=(100, 100, 100) at=(50, 0, 0)\n")
            TEXT("}\n"));

        TestFalse(*FString::Printf(TEXT("a modifier invalidates the recorded footprints. %s"),
                *PwModelCompilerTest_Describe(Result)),
            PwModelCompilerTest_HasCode(Result.Diagnostics,
                PwModelDiagnosticCodes::PWMODEL_UNUNIONED_OVERLAP));
    }

    return true;
}

// ============================================================================
// The compiler runs the mesh-health walk it never used to
// ============================================================================
//
// geometry.check_health has always found closedness, boundary edges, degenerate triangles and
// bowtie vertices. The compiler never ran it, so every example model compiled clean while
// carrying degenerates and, at one point, 272 boundary edges - an open shell shipped as a
// successful asset with model.compile answering success:true. Per-example degenerate counts are
// not repeated here; the corpus reading is in docs/wiki-src/model.examples.md.
//
// Warnings, not errors, and the audit is why: open is CORRECT for a card, a plane or an
// append_buffers surface, and origami_crane is exactly that, so an error would fail a shipped
// example. The numbers reach the caller as result fields, so a caller that DOES want a solid can
// gate on them without string-matching a diagnostic.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerMeshHealthTest,
    "PinWright.Model.Compiler.OpenAndDegenerateMeshesAreReportedNotHidden",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerMeshHealthTest::RunTest(const FString& Parameters)
{
    // ---- a closed box is clean and says so ------------------------------------------------
    {
        const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
            TEXT("pwmodel 0\npart solid {\n    box size=(100, 100, 100)\n}\n"));

        TestTrue(*FString::Printf(TEXT("a box compiles. %s"),
            *PwModelCompilerTest_Describe(Result)), Result.bSuccess);
        TestEqual(TEXT("and reports zero boundary edges - measured, not assumed"),
            Result.MeshBoundaryEdges, 0);
        TestEqual(TEXT("and one connected component"), Result.MeshComponentCount, 1);
        TestFalse(*FString::Printf(TEXT("and raises no open-mesh warning. %s"),
                *PwModelCompilerTest_Describe(Result)),
            PwModelCompilerTest_HasCode(Result.Diagnostics,
                PwModelDiagnosticCodes::PWMODEL_MESH_NOT_CLOSED));
    }

    // ---- an open shell compiles, and SAYS it is open ---------------------------------------
    //
    // Two triangles bounding nothing. Before this stage existed the response for this document
    // was indistinguishable from the box's above.
    {
        const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
            TEXT("pwmodel 0\n")
            TEXT("part card {\n")
            TEXT("    procedural_mesh\n")
            TEXT("    append_buffers vertices=[(0,0,0), (100,0,0), (100,100,0), (0,100,0)] triangles=[(0,1,2), (0,2,3)]\n")
            TEXT("}\n"));

        TestTrue(*FString::Printf(TEXT("an open shell still COMPILES - open is right for a card, ")
                TEXT("and only the author knows which it is. %s"),
                *PwModelCompilerTest_Describe(Result)), Result.bSuccess);

        const FPwDiagnostic* NotClosed = PwModelCompilerTest_FindCode(
            Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_MESH_NOT_CLOSED);
        if (!NotClosed)
        {
            AddError(FString::Printf(TEXT("expected PWMODEL_MESH_NOT_CLOSED. %s"),
                *PwModelCompilerTest_Describe(Result)));
            return false;
        }
        TestTrue(TEXT("as a warning, so it cannot fail a legitimate surface model"),
            NotClosed->Severity == EPwSeverity::Warning);

        // The quad's four outer edges. The COUNT is what makes this actionable - 4 is a card,
        // 272 is a subtract that broke through a wall - so it is asserted, not just presence.
        TestEqual(TEXT("the boundary edge count reaches the result"), Result.MeshBoundaryEdges, 4);
        TestTrue(*FString::Printf(TEXT("and the message carries it: '%s'"), *NotClosed->Message),
            NotClosed->Message.Contains(TEXT("4 boundary edge")));
    }

    return true;
}


// ============================================================================
// A patch can name its material slot
// ============================================================================
//
// `append_triangle` produces geometry and, until this landed, could not say what it was made
// of: the op took no `material=`, so RunGenerator's untagged path allocated `Default` for it.
// Measured on Examples/pwmodel/mobius_band.pwmodel - a part carrying `material="Cast"` on both
// its `procedural_mesh` and its `append_buffers`, patched with twelve `append_triangle` ops,
// compiled to materialSlots 2, and static_mesh.describe named
// [{slot:"Cast", …/BasicShapeMaterial}, {slot:"Default", …/WorldGridMaterial}] - so the twelve
// patch triangles shipped rendering in the grid material. The only advice available was to keep
// such a part single-slotted and unbound.
//
// Asserted through the ASSET rather than through the slot count alone: a name resolved into its
// own table, or accepted by the parser and then dropped before the material IDs were written,
// still counts as one slot while shipping the patch on the wrong surface. The untagged control
// below is the other half - the fix is the PARAMETER, not a silent fall-through to slot 0, and
// untagged geometry still opens `Default` exactly as every other generator's does.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerAppendTriangleSlotReachesTheAssetTest,
    "PinWright.Model.Compiler.AppendTriangleMaterialSlotReachesTheAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerAppendTriangleSlotReachesTheAssetTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = PwModelCompilerTest_AssetPath(TEXT("SM_AppendTriangleSlot"));
    PwModelCompilerTest_DeleteIfPresent(AssetPath);

    const FPwModelCompileResult Result = PwModelCompilerTest_CompileTo(
        TEXT("pwmodel 0\n")
        TEXT("materials {\n    Cast = \"/Engine/BasicShapes/BasicShapeMaterial\"\n}\n")
        TEXT("part shell {\n")
        TEXT("    procedural_mesh material=\"Cast\"\n")
        TEXT("    append_buffers vertices=[(0, 0, 0), (100, 0, 0), (100, 100, 0), (0, 100, 0)] ")
        TEXT("triangles=[(0, 1, 2), (0, 2, 3)] uvs=[(0, 0), (1, 0), (1, 1), (0, 1)] material=\"Cast\"\n")
        TEXT("    append_triangle v0=(0, 100, 0) v1=(100, 100, 0) v2=(50, 200, 0) material=\"Cast\"\n")
        TEXT("}\n"),
        AssetPath);

    TestTrue(*FString::Printf(TEXT("compile succeeded. %s"), *PwModelCompilerTest_Describe(Result)),
        Result.bSuccess);

    // ONE slot. Two is the defect: the patch opening its own `Default` beside the bound one.
    TestEqual(TEXT("the tagged patch shares the model-wide slot rather than opening a second"),
        Result.MaterialSlots, 1);

    UStaticMesh* Mesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(AssetPath));
    if (!Mesh)
    {
        AddError(FString::Printf(TEXT("expected a UStaticMesh at %s. %s"),
            *AssetPath, *PwModelCompilerTest_Describe(Result)));
        PwModelCompilerTest_DeleteIfPresent(AssetPath);
        return false;
    }

    FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});

    const TArray<FStaticMaterial>& Slots = Mesh->GetStaticMaterials();
    TestEqual(TEXT("the asset carries one slot"), Slots.Num(), 1);
    if (Slots.Num() == 1)
    {
        TestEqual(TEXT("named for the tag the patch was written with"),
            Slots[0].MaterialSlotName.ToString(), FString(TEXT("Cast")));
        if (TestNotNull(TEXT("the slot carries a material"), Slots[0].MaterialInterface.Get()))
        {
            // Named explicitly rather than merely "not null": an UNBOUND slot silently takes
            // UMaterial::GetDefaultMaterial, which is exactly what the defect shipped.
            TestEqual(TEXT("and it is the bound one, not the default surface material"),
                Slots[0].MaterialInterface->GetPathName(),
                FString(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")));
        }
    }

    PwModelCompilerTest_DeleteIfPresent(AssetPath);

    // The control. Drop the tag from the patch alone and `Default` comes back - untagged
    // geometry has always landed there and still does, on this op like every other.
    const FPwModelCompileResult Untagged = PwModelCompilerTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("materials {\n    Cast = \"/Engine/BasicShapes/BasicShapeMaterial\"\n}\n")
        TEXT("part shell {\n")
        TEXT("    append_buffers vertices=[(0, 0, 0), (100, 0, 0), (100, 100, 0), (0, 100, 0)] ")
        TEXT("triangles=[(0, 1, 2), (0, 2, 3)] uvs=[(0, 0), (1, 0), (1, 1), (0, 1)] material=\"Cast\"\n")
        TEXT("    append_triangle v0=(0, 100, 0) v1=(100, 100, 0) v2=(50, 200, 0)\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("the untagged control compiles. %s"),
        *PwModelCompilerTest_Describe(Untagged)), Untagged.bSuccess);
    TestEqual(TEXT("an untagged patch still opens Default - the fix is the parameter, not a "
                   "silent fall-through to slot 0"),
        Untagged.MaterialSlots, 2);

    return true;
}

// ============================================================================
// PWMODEL_UNUNIONED_OVERLAP needs two operands that enclose a volume
// ============================================================================
//
// The warning's whole message is about a SHARED VOLUME keeping buried interior faces, and its
// remedy is `union { }`. Neither clause is true of a mesh with boundary edges: it bounds no
// interior, and a boolean on it is not a legal operation. The check compared bounding boxes
// only, so two triangles sharing an edge - whose boxes always interpenetrate, because that is
// what a shared edge means in three dimensions - tripped it every time. A twelve-triangle patch
// on Examples/pwmodel/mobius_band.pwmodel produced NINE of these warnings, each advising a union
// of two shapes that between them enclose nothing.
//
// The predicate is closedness on BOTH operands, not a name test on `append_triangle`. A name
// test would have fixed one op and left the identical false positive on `plane`, `disc`, an
// uncapped `revolve` and every `procedural_mesh` + `append_buffers` shell - which is the third
// case below, pinned deliberately so the coverage this trades away is visible rather than
// discovered later.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerOverlapNeedsClosedOperandsTest,
    "PinWright.Model.Compiler.UnunionedOverlapSkipsOperandsThatEncloseNoVolume",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerOverlapNeedsClosedOperandsTest::RunTest(const FString& Parameters)
{
    // ---- silent: a patch of triangles meeting along shared edges -------------------------
    {
        const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
            TEXT("pwmodel 0\n")
            TEXT("part patch {\n")
            TEXT("    procedural_mesh\n")
            TEXT("    append_triangle v0=(0, 0, 0) v1=(100, 0, 0) v2=(100, 100, 0)\n")
            TEXT("    append_triangle v0=(0, 0, 0) v1=(100, 100, 0) v2=(0, 100, 0)\n")
            TEXT("    append_triangle v0=(0, 100, 0) v1=(100, 100, 0) v2=(50, 200, 0)\n")
            TEXT("}\n"));

        TestTrue(*FString::Printf(TEXT("the patch compiles. %s"),
            *PwModelCompilerTest_Describe(Result)), Result.bSuccess);
        TestEqual(*FString::Printf(TEXT("a triangle encloses no volume, so no pair involving one ")
                    TEXT("is compared. %s"), *PwModelCompilerTest_Describe(Result)),
            PwModelCompilerTest_CountCode(Result.Diagnostics,
                PwModelDiagnosticCodes::PWMODEL_UNUNIONED_OVERLAP), 0);
    }

    // ---- still fires: two closed solids, which is the case the warning exists for ---------
    //
    // Asserted here as well as in OverlappingSiblingsWarnAndAdjacentOnesDoNot, because a
    // closedness predicate that answered "open" for everything would silence the check
    // entirely and pass every negative case in this file.
    {
        const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
            TEXT("pwmodel 0\n")
            TEXT("part twin {\n")
            TEXT("    box size=(100, 100, 100)\n")
            TEXT("    box size=(100, 100, 100) at=(50, 0, 0)\n")
            TEXT("}\n"));

        TestEqual(*FString::Printf(TEXT("two interpenetrating closed solids still warn. %s"),
                *PwModelCompilerTest_Describe(Result)),
            PwModelCompilerTest_CountCode(Result.Diagnostics,
                PwModelDiagnosticCodes::PWMODEL_UNUNIONED_OVERLAP), 1);
    }

    // ---- and one closed operand is not enough --------------------------------------------
    //
    // A DOCUMENTED trade-off, not an oversight. Overlapping open shells still z-fight, and the
    // check never had advice for them: `union { }` on an open mesh is undefined, and "move them
    // apart" is the half of the message that survives. Pinned so the loss is a decision on the
    // record rather than something a later reader has to rediscover.
    {
        const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
            TEXT("pwmodel 0\n")
            TEXT("part mixed {\n")
            TEXT("    box size=(100, 100, 100)\n")
            TEXT("    plane size=(100, 100) at=(0, 0, 20)\n")
            TEXT("}\n"));

        TestEqual(*FString::Printf(TEXT("a plane inside a box is not reported - the remedy the ")
                    TEXT("message names cannot apply to it. %s"),
                *PwModelCompilerTest_Describe(Result)),
            PwModelCompilerTest_CountCode(Result.Diagnostics,
                PwModelDiagnosticCodes::PWMODEL_UNUNIONED_OVERLAP), 0);
    }

    return true;
}

// ============================================================================
// delete_vertex after delete_triangle
// ============================================================================
//
// FDynamicMesh3::RemoveTriangle drops the vertices its triangle leaves isolated
// (bRemoveIsolatedVertices defaults true), so the natural cut-then-clean-up pairing - cut a
// patch with `delete_triangle`, then name the interior vertex it stranded - reached
// GeometryOps::DeleteVertex with an id the ENGINE had already removed and aborted the whole
// compile:
//
//     PWMODEL_OP_FAILED  'delete_vertex' failed [INVALID_VERTEX]: Invalid vertex index: 4
//
// Nothing in the document says which vertices that was, so the author cannot write around it.
// It is now a no-op with PWMODEL_VERTEX_ALREADY_REMOVED.
//
// The second case is what stops this from being a swallowed-error patch. Sparse-but-present is
// the only condition covered: deletes leave the id space sparse rather than renumbering, so an
// id at or above MaxVertexID was NEVER a vertex of this mesh, nothing removed it, and it is
// still PWMODEL_OP_FAILED. Delete the MaxVertexID bound from the guard and case two goes green
// while case one still passes - which is the failure this pair exists to catch.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerDeleteVertexAfterDeleteTriangleTest,
    "PinWright.Model.Compiler.DeleteVertexOfAnAlreadyRemovedVertexIsANoOp",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerDeleteVertexAfterDeleteTriangleTest::RunTest(const FString& Parameters)
{
    // Triangle 2 is the only user of vertex 4, so removing it strands - and therefore removes -
    // that vertex. Triangles 0 and 1 keep the part non-empty, so the merge cannot fail for an
    // unrelated reason and mask the result.
    const FString Patch =
        TEXT("pwmodel 0\n")
        TEXT("part patch {\n")
        TEXT("    procedural_mesh\n")
        TEXT("    append_buffers vertices=[(0, 0, 0), (100, 0, 0), (100, 100, 0), (0, 100, 0), (50, 50, 80)] ")
        TEXT("triangles=[(0, 1, 2), (0, 2, 3), (0, 1, 4)] ")
        TEXT("uvs=[(0, 0), (1, 0), (1, 1), (0, 1), (0.5, 0.5)]\n")
        TEXT("    delete_triangle index=2\n");

    // ---- the pairing that used to abort the compile ---------------------------------------
    {
        const FString Source = Patch + TEXT("    delete_vertex index=4\n}\n");
        const FPwModelCompileResult Result = PwModelCompilerTest_Validate(*Source);

        TestTrue(*FString::Printf(TEXT("cut-then-clean-up compiles. %s"),
            *PwModelCompilerTest_Describe(Result)), Result.bSuccess);
        TestFalse(*FString::Printf(TEXT("and does not fail the op. %s"),
                *PwModelCompilerTest_Describe(Result)),
            PwModelCompilerTest_HasCode(Result.Diagnostics,
                PwModelDiagnosticCodes::PWMODEL_OP_FAILED));

        const FPwDiagnostic* Skipped = PwModelCompilerTest_FindCode(
            Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_VERTEX_ALREADY_REMOVED);
        if (!Skipped)
        {
            AddError(FString::Printf(TEXT("expected PWMODEL_VERTEX_ALREADY_REMOVED. %s"),
                *PwModelCompilerTest_Describe(Result)));
            return false;
        }
        TestTrue(TEXT("reported as a warning - the mesh already holds the asked-for state"),
            Skipped->Severity == EPwSeverity::Warning);
        TestEqual(TEXT("anchored on the delete_vertex statement, line 6"), Skipped->Line, 6);
        TestTrue(*FString::Printf(TEXT("names the vertex: '%s'"), *Skipped->Message),
            Skipped->Message.Contains(TEXT("vertex 4")));
    }

    // ---- an id the mesh never carried is still a hard failure ------------------------------
    {
        const FString Source = Patch + TEXT("    delete_vertex index=99\n}\n");
        const FPwModelCompileResult Result = PwModelCompilerTest_Validate(*Source);

        TestFalse(*FString::Printf(TEXT("a genuine bad index is not swallowed. %s"),
            *PwModelCompilerTest_Describe(Result)), Result.bSuccess);
        TestTrue(*FString::Printf(TEXT("it fails the op. %s"), *PwModelCompilerTest_Describe(Result)),
            PwModelCompilerTest_HasCode(Result.Diagnostics,
                PwModelDiagnosticCodes::PWMODEL_OP_FAILED));
        TestFalse(*FString::Printf(TEXT("and is NOT reported as an already-removed vertex. %s"),
                *PwModelCompilerTest_Describe(Result)),
            PwModelCompilerTest_HasCode(Result.Diagnostics,
                PwModelDiagnosticCodes::PWMODEL_VERTEX_ALREADY_REMOVED));
    }

    // ---- a live vertex is still deleted, silently -------------------------------------------
    {
        const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
            TEXT("pwmodel 0\n")
            TEXT("part patch {\n")
            TEXT("    procedural_mesh\n")
            TEXT("    append_buffers vertices=[(0, 0, 0), (100, 0, 0), (100, 100, 0), (0, 100, 0), (50, 50, 80)] ")
            TEXT("triangles=[(0, 1, 2), (0, 2, 3), (0, 1, 4)] ")
            TEXT("uvs=[(0, 0), (1, 0), (1, 1), (0, 1), (0.5, 0.5)]\n")
            TEXT("    delete_vertex index=4\n")
            TEXT("}\n"));

        TestTrue(*FString::Printf(TEXT("cutting a live vertex compiles. %s"),
            *PwModelCompilerTest_Describe(Result)), Result.bSuccess);
        TestFalse(*FString::Printf(TEXT("with no already-removed warning - the guard did not ")
                    TEXT("intercept a real delete. %s"), *PwModelCompilerTest_Describe(Result)),
            PwModelCompilerTest_HasCode(Result.Diagnostics,
                PwModelDiagnosticCodes::PWMODEL_VERTEX_ALREADY_REMOVED));
    }

    return true;
}

// ============================================================================
// model.validate resolves the material bindings
// ============================================================================
//
// Validate is the "check before you compile" surface, and it was blind to exactly one class of
// defect: a slot bound to a path that will not load returned ZERO diagnostics from validate and
// then warned on compile. "Validates clean, then warns on compile" is the outcome that surface
// exists to prevent - an author iterating on a string has no reason to run a compile they were
// told is unnecessary, so the model ships resolving to the default material and renders in grey.
//
// The message is asserted by TEXT, not merely by code, because it has to stay byte-identical to
// the one CreateStaticMesh emits on the compile path: two surfaces reporting one condition in
// two wordings is how a caller ends up grepping for the wrong string.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerValidateChecksMaterialBindingsTest,
    "PinWright.Model.Compiler.ValidateReportsAnUnloadableMaterialBinding",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelCompilerValidateChecksMaterialBindingsTest::RunTest(const FString& Parameters)
{
    const auto FindLoadWarning = [](const FPwModelCompileResult& Result) -> const FPwDiagnostic*
    {
        for (const FPwDiagnostic& Diagnostic : Result.Diagnostics)
        {
            if (Diagnostic.Code == PwModelDiagnosticCodes::PWMODEL_STAGE_WARNING
                && Diagnostic.Message.Contains(TEXT("could not be loaded")))
            {
                return &Diagnostic;
            }
        }
        return nullptr;
    };

    // ---- the defect: a used slot bound to nothing ------------------------------------------
    {
        const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
            TEXT("pwmodel 0\n")
            TEXT("materials {\n")
            TEXT("    Shell = \"/Game/DoesNotExist/M_NotThere\"\n")
            TEXT("}\n")
            TEXT("part body {\n")
            TEXT("    box size=(100, 100, 100) material=\"Shell\"\n")
            TEXT("}\n"));

        // A warning, never an error: an unresolved binding leaves a valid asset on the default
        // material, so it must not fail a validate that is otherwise clean.
        TestTrue(*FString::Printf(TEXT("validate still succeeds. %s"),
            *PwModelCompilerTest_Describe(Result)), Result.bSuccess);

        const FPwDiagnostic* Warning = FindLoadWarning(Result);
        if (!Warning)
        {
            AddError(FString::Printf(
                TEXT("expected validate to report the unloadable binding. %s"),
                *PwModelCompilerTest_Describe(Result)));
            return false;
        }

        TestTrue(TEXT("as a warning"), Warning->Severity == EPwSeverity::Warning);
        TestEqual(TEXT("the same sentence the compile path emits"), Warning->Message,
            FString(TEXT("Material slot 'Shell' is bound to '/Game/DoesNotExist/M_NotThere', ")
                    TEXT("which could not be loaded; using the default material")));

        // Anchored on the binding, which the compile path cannot do - CreateStaticMesh has no
        // position for its copy and reports it at -1.
        TestEqual(TEXT("anchored on the binding's own line, line 3"), Warning->Line, 3);
    }

    // ---- a binding that resolves is silent --------------------------------------------------
    {
        const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
            TEXT("pwmodel 0\n")
            TEXT("materials {\n")
            TEXT("    Shell = \"/Engine/BasicShapes/BasicShapeMaterial\"\n")
            TEXT("}\n")
            TEXT("part body {\n")
            TEXT("    box size=(100, 100, 100) material=\"Shell\"\n")
            TEXT("}\n"));

        TestTrue(*FString::Printf(TEXT("a resolvable binding validates clean. %s"),
            *PwModelCompilerTest_Describe(Result)), Result.bSuccess);
        TestNull(*FString::Printf(TEXT("and reports nothing. %s"),
            *PwModelCompilerTest_Describe(Result)), FindLoadWarning(Result));
    }

    // ---- an UNUSED binding is not resolved --------------------------------------------------
    //
    // It is dropped before the creator ever sees it and already has its own diagnostic from the
    // parser, so resolving it would report a path that cannot reach the asset.
    {
        const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
            TEXT("pwmodel 0\n")
            TEXT("materials {\n")
            TEXT("    Unused = \"/Game/DoesNotExist/M_NotThere\"\n")
            TEXT("}\n")
            TEXT("part body {\n")
            TEXT("    box size=(100, 100, 100)\n")
            TEXT("}\n"));

        TestTrue(*FString::Printf(TEXT("the unused binding is reported as unused. %s"),
                *PwModelCompilerTest_Describe(Result)),
            PwModelCompilerTest_HasCode(Result.Diagnostics,
                PwModelDiagnosticCodes::PWMODEL_UNUSED_MATERIAL));
        TestNull(*FString::Printf(TEXT("and not additionally as unloadable. %s"),
            *PwModelCompilerTest_Describe(Result)), FindLoadWarning(Result));
    }

    return true;
}

// ============================================================================
// The two sweeps floor a path at 2 frames; array_along_path does not
// ============================================================================
//
// ValidatePathFrames is NOT shared with `array_along_path`, and a comment above it claimed for a
// while that it was. The divergence is deliberate and load-bearing in both directions:
// GeometryOps::Sweep selects its VERTICAL FALLBACK on `Samples.Num() < 2`, so a one-frame path
// would compile clean and quietly produce a tube through the mesh's bounding box instead of the
// path the author wrote, while ExtrudeAlongSpline produces nothing at all. One frame in an
// ARRAY is a legitimate single placement, and its bound is ArrayAlongPath's own
// ValidateArrayCount (1..100) rather than the sweeps' 2..257.
//
// Pinned because a future reader who believes the comment and "unifies" the two checks breaks
// exactly one of these two cases, and neither is visible in any count.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerPathFrameFloorIsSweepOnlyTest,
    "PinWright.Model.Compiler.OneFramePathIsLegalForArrayAndIllegalForTheSweeps",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerPathFrameFloorIsSweepOnlyTest::RunTest(const FString& Parameters)
{
    {
        const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
            TEXT("pwmodel 0\n")
            TEXT("part placed {\n")
            TEXT("    box size=(50, 50, 50)\n")
            TEXT("    array_along_path path=[(200, 0, 0, 0, 0, 0)]\n")
            TEXT("}\n"));

        TestTrue(*FString::Printf(TEXT("one frame is one placement, and compiles. %s"),
            *PwModelCompilerTest_Describe(Result)), Result.bSuccess);
        TestFalse(*FString::Printf(TEXT("no path-shape failure. %s"),
                *PwModelCompilerTest_Describe(Result)),
            PwModelCompilerTest_HasCode(Result.Diagnostics,
                PwModelDiagnosticCodes::PWMODEL_OP_FAILED));
    }

    const TCHAR* const OneFrameSweeps[] =
    {
        TEXT("pwmodel 0\n")
        TEXT("part swept {\n")
        TEXT("    box size=(50, 50, 50)\n")
        TEXT("    sweep path=[(0, 0, 0, 0, 0, 0)]\n")
        TEXT("}\n"),

        TEXT("pwmodel 0\n")
        TEXT("part swept {\n")
        TEXT("    box size=(50, 50, 50)\n")
        TEXT("    extrude_along_spline path=[(0, 0, 0, 0, 0, 0)]\n")
        TEXT("}\n"),
    };

    for (const TCHAR* const Source : OneFrameSweeps)
    {
        const FPwModelCompileResult Result = PwModelCompilerTest_Validate(Source);

        TestFalse(*FString::Printf(TEXT("a one-frame sweep path is refused. %s"),
            *PwModelCompilerTest_Describe(Result)), Result.bSuccess);

        const FPwDiagnostic* Failure = PwModelCompilerTest_FindCode(
            Result.Diagnostics, PwModelDiagnosticCodes::PWMODEL_OP_FAILED);
        if (!Failure)
        {
            AddError(FString::Printf(TEXT("expected PWMODEL_OP_FAILED. %s"),
                *PwModelCompilerTest_Describe(Result)));
            continue;
        }
        TestTrue(*FString::Printf(TEXT("naming the 2-frame floor: '%s'"), *Failure->Message),
            Failure->Message.Contains(TEXT("at least 2 frames")));
    }

    return true;
}

// Size is an acceptance criterion, and until `bounds` existed nothing in this result carried it:
// a model that has to fit a required box could only be measured by WRITING an asset and calling
// static_mesh.describe on it, which made model.validate - the surface documented as creating
// nothing - unusable for the commonest numeric check an author runs. Every case below runs
// through the validate-only helper for exactly that reason.
//
// The per-part half is not the model-wide box repeated. It is what names WHICH part reaches an
// extreme, and a test that only checked the merged box would pass on an implementation that
// copied it into every entry.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCompilerBoundsTest,
    "PinWright.Model.Compiler.BoundsAreReportedPerPartAndForTheMergedMeshOnValidate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCompilerBoundsTest::RunTest(const FString& Parameters)
{
    constexpr double Tol = 0.001;

    // ---- one centred box: the box the generator names, on a run that wrote nothing ---------
    {
        const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
            TEXT("pwmodel 0\n")
            TEXT("part body {\n")
            TEXT("    box size=(80, 50, 40)\n")
            TEXT("}\n"));

        TestTrue(*FString::Printf(TEXT("a one-box document validates. %s"),
            *PwModelCompilerTest_Describe(Result)), Result.bSuccess);
        TestTrue(TEXT("bounds are measured on a validate-only run, which writes no asset"),
            Result.MeshBounds.IsValid != 0);
        TestTrue(TEXT("and that run really did write nothing"), Result.AssetPath.IsEmpty());

        // Generators are centre-placed, so size=(80, 50, 40) spans -40..40, -25..25, -20..20.
        TestEqual(TEXT("min.x"), Result.MeshBounds.Min.X, -40.0, Tol);
        TestEqual(TEXT("max.z"), Result.MeshBounds.Max.Z, 20.0, Tol);
        TestEqual(TEXT("size.y"), Result.MeshBounds.GetSize().Y, 50.0, Tol);

        if (Result.Parts.Num() == 1)
        {
            TestTrue(TEXT("the single part carries its own box too"),
                Result.Parts[0].MeshBounds.IsValid != 0);
            TestEqual(TEXT("which for one part equals the merged one"),
                Result.Parts[0].MeshBounds.GetSize().X, 80.0, Tol);
        }
        else
        {
            AddError(FString::Printf(TEXT("expected exactly one part. %s"),
                *PwModelCompilerTest_Describe(Result)));
        }
    }

    // ---- two parts: the per-part boxes DIFFER, and the merged one is their union -----------
    //
    // This is the case that fails on an implementation which reports the merged box per part.
    {
        const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
            TEXT("pwmodel 0\n")
            TEXT("part left {\n")
            TEXT("    box size=(20, 20, 20) at=(-100, 0, 0)\n")
            TEXT("}\n")
            TEXT("part right {\n")
            TEXT("    box size=(20, 20, 20) at=(100, 0, 0)\n")
            TEXT("}\n"));

        TestTrue(*FString::Printf(TEXT("a two-part document validates. %s"),
            *PwModelCompilerTest_Describe(Result)), Result.bSuccess);

        if (Result.Parts.Num() != 2)
        {
            AddError(FString::Printf(TEXT("expected two parts. %s"),
                *PwModelCompilerTest_Describe(Result)));
            return false;
        }

        TestEqual(TEXT("the merged box spans both parts"),
            Result.MeshBounds.GetSize().X, 220.0, Tol);
        TestEqual(TEXT("part 'left' carries only its own"),
            Result.Parts[0].MeshBounds.GetSize().X, 20.0, Tol);
        TestEqual(TEXT("centred where its own at= put it"),
            Result.Parts[0].MeshBounds.GetCenter().X, -100.0, Tol);
        TestEqual(TEXT("and part 'right' the same, mirrored"),
            Result.Parts[1].MeshBounds.GetCenter().X, 100.0, Tol);
    }

    // ---- the part transform is BAKED into the reported box --------------------------------
    //
    // A part box read before the part transform would describe part-local space, which is not
    // the space the merged mesh - or the asset - is in.
    {
        const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
            TEXT("pwmodel 0\n")
            TEXT("part moved at=(0, 0, 500) scale=(1, 1, 2) {\n")
            TEXT("    box size=(20, 20, 20)\n")
            TEXT("}\n"));

        TestTrue(*FString::Printf(TEXT("a transformed part validates. %s"),
            *PwModelCompilerTest_Describe(Result)), Result.bSuccess);

        if (Result.Parts.Num() == 1)
        {
            TestEqual(TEXT("the part header's at= moved the reported box"),
                Result.Parts[0].MeshBounds.GetCenter().Z, 500.0, Tol);
            TestEqual(TEXT("and its scale= is baked in, not applied twice"),
                Result.Parts[0].MeshBounds.GetSize().Z, 40.0, Tol);
        }
        else
        {
            AddError(FString::Printf(TEXT("expected exactly one part. %s"),
                *PwModelCompilerTest_Describe(Result)));
        }
    }

    // ---- a document that produced no geometry reports NO box, rather than a zero one -------
    //
    // An all-zero box would read as a model collapsed to a point. The sentinel has to be
    // distinguishable from a measurement, which is why this is IsValid and not (0, 0, 0).
    {
        const FPwModelCompileResult Result = PwModelCompilerTest_Validate(
            TEXT("pwmodel 0\n")
            TEXT("part broken {\n")
            TEXT("    bevel distance=2\n")
            TEXT("}\n"));

        TestFalse(TEXT("a part opening with a modifier does not compile"), Result.bSuccess);
        TestTrue(TEXT("and reports no box at all rather than an all-zero one"),
            Result.MeshBounds.IsValid == 0);
    }

    return true;
}
