// Copyright (c) 2026 Alexander Penkin. MIT License.

// Structural regression coverage for bevel safety and attribution. This intentionally drives the
// .pwmodel compiler only; no editor or live asset is needed to assert the diagnostic contract.
#include "Misc/AutomationTest.h"

#include "Model/PwModelCompiler.h"
#include "Model/PwModelDiagnostic.h"

namespace
{
bool PwModelBevelSafetyTest_HasUnsafeWarningForPart(
    const TArray<FPwDiagnostic>& Diagnostics,
    const TCHAR* PartName)
{
    for (const FPwDiagnostic& Diagnostic : Diagnostics)
    {
        if (Diagnostic.Code == PwModelDiagnosticCodes::PWMODEL_STAGE_WARNING
            && Diagnostic.ScopeName == PartName
            && Diagnostic.Message.Contains(TEXT("unsafe selected polygroup edge"), ESearchCase::IgnoreCase))
        {
            return true;
        }
    }
    return false;
}

bool PwModelBevelSafetyTest_HasAttributedBevelWarning(
    const TArray<FPwDiagnostic>& Diagnostics,
    const int32 ExpectedLine,
    const TCHAR* PartName)
{
    for (const FPwDiagnostic& Diagnostic : Diagnostics)
    {
        if (Diagnostic.Code == PwModelDiagnosticCodes::PWMODEL_STAGE_WARNING
            && Diagnostic.Line == ExpectedLine
            && Diagnostic.ScopeName == PartName
            && Diagnostic.Message.Contains(TEXT("bevel"), ESearchCase::IgnoreCase)
            && (Diagnostic.Message.Contains(TEXT("unsafe selected polygroup edge"), ESearchCase::IgnoreCase)
                || Diagnostic.Message.Contains(TEXT("self-intersecting triangle pair"), ESearchCase::IgnoreCase)))
        {
            return true;
        }
    }
    return false;
}

bool PwModelBevelSafetyTest_HasLineMinusOneSelfIntersection(
    const TArray<FPwDiagnostic>& Diagnostics)
{
    for (const FPwDiagnostic& Diagnostic : Diagnostics)
    {
        if (Diagnostic.Code == PwModelDiagnosticCodes::PWMODEL_SELF_INTERSECTING_SURFACE
            && Diagnostic.Line == -1)
        {
            return true;
        }
    }
    return false;
}

bool PwModelBevelSafetyTest_HasPriorBevelWarning(
    const TArray<FPwDiagnostic>& Diagnostics,
    const int32 ExpectedLine,
    const TCHAR* PartName)
{
    for (const FPwDiagnostic& Diagnostic : Diagnostics)
    {
        if (Diagnostic.Code == PwModelDiagnosticCodes::PWMODEL_STAGE_WARNING
            && Diagnostic.Line == ExpectedLine
            && Diagnostic.ScopeName == PartName
            && Diagnostic.Message.Contains(TEXT("already touched by an earlier bevel"), ESearchCase::IgnoreCase))
        {
            return true;
        }
    }
    return false;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelBevelAfterThroughCutSafetyTest,
    "PinWright.Model.Bevel.AfterThroughCutWarnsAtBevelLine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelBevelAfterThroughCutSafetyTest::RunTest(const FString& Parameters)
{
    FPwModelCompileOptions Options;
    Options.bValidateOnly = true;
    Options.bSave = false;

    const FPwModelCompileResult Result = FPwModelCompiler::Compile(
        TEXT("pwmodel 0\n")
        TEXT("part one_slot {\n")
        TEXT("    box size=(8.0, 4.2, 1.0) at=(0, 0, 8.5)\n")
        TEXT("    subtract {\n")
        TEXT("        box size=(0.58, 5.0, 0.55) at=(0, 0, 8.85)\n")
        TEXT("    }\n")
        TEXT("    bevel distance=0.12 segments=0 infer_material_id=true\n")
        TEXT("}\n"),
        Options);

    TestTrue(TEXT("the through-cut model still validates"), Result.bSuccess);
    TestTrue(TEXT("the through-cut bevel emits an attributed warning on the bevel line"),
        PwModelBevelSafetyTest_HasAttributedBevelWarning(Result.Diagnostics, 7, TEXT("one_slot")));
    TestEqual(TEXT("the rejected through-cut bevel leaves no self intersections"),
        Result.MeshSelfIntersections, 0);
    TestFalse(TEXT("the health diagnostic is not left at line -1 after attribution"),
        PwModelBevelSafetyTest_HasLineMinusOneSelfIntersection(Result.Diagnostics));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelBevelAfterBevelSafetyTest,
    "PinWright.Model.Bevel.AfterBevelWarnsAtSecondBevelLine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelBevelAfterBevelSafetyTest::RunTest(const FString& Parameters)
{
    FPwModelCompileOptions Options;
    Options.bValidateOnly = true;
    Options.bSave = false;

    const FPwModelCompileResult Result = FPwModelCompiler::Compile(
        TEXT("pwmodel 0\n")
        TEXT("part bevel_history {\n")
        TEXT("    box size=(8.0, 4.2, 1.0)\n")
        TEXT("    bevel distance=0.12 segments=0\n")
        TEXT("    bevel distance=0.12 segments=0\n")
        TEXT("}\n"),
        Options);

    TestTrue(TEXT("the bevel history model still validates"), Result.bSuccess);
    TestTrue(TEXT("the second bevel warns that earlier bevel output is unsafe"),
        PwModelBevelSafetyTest_HasPriorBevelWarning(Result.Diagnostics, 5, TEXT("bevel_history")));
    TestFalse(TEXT("the history diagnostic is not left at line -1"),
        PwModelBevelSafetyTest_HasLineMinusOneSelfIntersection(Result.Diagnostics));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelCleanBevelSafetyControlTest,
    "PinWright.Model.Bevel.CleanBoxRemainsSafe",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelCleanBevelSafetyControlTest::RunTest(const FString& Parameters)
{
    FPwModelCompileOptions Options;
    Options.bValidateOnly = true;
    Options.bSave = false;

    const FPwModelCompileResult Baseline = FPwModelCompiler::Compile(
        TEXT("pwmodel 0\n")
        TEXT("part clean_box {\n")
        TEXT("    box size=(8.0, 4.2, 1.0)\n")
        TEXT("}\n"),
        Options);
    const FPwModelCompileResult Result = FPwModelCompiler::Compile(
        TEXT("pwmodel 0\n")
        TEXT("part clean_box {\n")
        TEXT("    box size=(8.0, 4.2, 1.0)\n")
        TEXT("    bevel distance=0.12 segments=0\n")
        TEXT("}\n"),
        Options);

    TestTrue(TEXT("the clean box baseline validates"), Baseline.bSuccess);
    TestTrue(TEXT("a clean box bevel still validates"), Result.bSuccess);
    TestEqual(TEXT("a clean box bevel leaves no self intersections"),
        Result.MeshSelfIntersections, 0);
    TestTrue(TEXT("a clean box bevel changes the generated geometry"),
        Result.MeshTriangleCount != Baseline.MeshTriangleCount
        || Result.MeshVertexCount != Baseline.MeshVertexCount);
    TestFalse(TEXT("a clean box bevel is not overblocked as unsafe"),
        PwModelBevelSafetyTest_HasUnsafeWarningForPart(Result.Diagnostics, TEXT("clean_box")));
    TestFalse(TEXT("the clean control has no unanchored self-intersection diagnostic"),
        PwModelBevelSafetyTest_HasLineMinusOneSelfIntersection(Result.Diagnostics));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelOpenSpanBoundaryEndpointBevelTest,
    "PinWright.Model.Bevel.OpenSpanBoundaryEndpointRemainsEligible",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelOpenSpanBoundaryEndpointBevelTest::RunTest(const FString& Parameters)
{
    FPwModelCompileOptions Options;
    Options.bValidateOnly = true;
    Options.bSave = false;

    const TCHAR* const OpenFold =
        TEXT("pwmodel 0\n")
        TEXT("part open_span {\n")
        TEXT("    procedural_mesh\n")
        TEXT("    append_buffers vertices=[(-1,-1,0),(0,-1,0),(0,1,0),(-1,1,0)] triangles=[(0,1,2),(0,2,3)] normals=[(0,0,1),(0,0,1),(0,0,1),(0,0,1)] uvs=[(0,0),(1,0),(1,1),(0,1)] group_id=0\n")
        TEXT("    append_buffers vertices=[(0,-1,0),(0,1,0),(0,1,1),(0,-1,1)] triangles=[(0,2,1),(0,3,2)] normals=[(-1,0,0),(-1,0,0),(-1,0,0),(-1,0,0)] uvs=[(0,0),(1,0),(1,1),(0,1)] group_id=1\n")
        TEXT("    merge_vertices tolerance=0.001\n");
    const FPwModelCompileResult Baseline = FPwModelCompiler::Compile(
        FString(OpenFold) + TEXT("}\n"), Options);
    const FPwModelCompileResult Result = FPwModelCompiler::Compile(
        FString(OpenFold)
        + TEXT("    bevel distance=0.1 segments=0 filter_box_min=(-0.01,-2,-0.01) filter_box_max=(0.01,2,0.01) fully_contained=true\n")
        + TEXT("}\n"),
        Options);

    TestTrue(TEXT("the open-fold baseline validates"), Baseline.bSuccess);
    TestTrue(TEXT("the boundary-ended open-span bevel validates"), Result.bSuccess);
    TestEqual(TEXT("the boundary-ended open-span bevel stays intersection-free"),
        Result.MeshSelfIntersections, 0);
    TestTrue(TEXT("the boundary-ended open span was bevelled rather than silently skipped"),
        Result.MeshTriangleCount != Baseline.MeshTriangleCount
        || Result.MeshVertexCount != Baseline.MeshVertexCount);
    TestFalse(TEXT("the supported boundary-ended span is not reported unsafe"),
        PwModelBevelSafetyTest_HasUnsafeWarningForPart(Result.Diagnostics, TEXT("open_span")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelNestedBooleanBevelHistoryTest,
    "PinWright.Model.Bevel.NestedBooleanTracksPriorBevel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelNestedBooleanBevelHistoryTest::RunTest(const FString& Parameters)
{
    FPwModelCompileOptions Options;
    Options.bValidateOnly = true;
    Options.bSave = false;

    const FPwModelCompileResult Result = FPwModelCompiler::Compile(
        TEXT("pwmodel 0\n")
        TEXT("part nested_history {\n")
        TEXT("    box size=(8.0, 4.2, 1.0)\n")
        TEXT("    union {\n")
        TEXT("        box size=(2.0, 2.0, 2.0) at=(4.0, 0, 0)\n")
        TEXT("        bevel distance=0.12 segments=0\n")
        TEXT("        bevel distance=0.12 segments=0\n")
        TEXT("    }\n")
        TEXT("}\n"),
        Options);

    TestTrue(TEXT("the nested boolean bevel history model validates"), Result.bSuccess);
    TestTrue(TEXT("the nested second bevel is attributed to its own line"),
        PwModelBevelSafetyTest_HasPriorBevelWarning(
            Result.Diagnostics, 7, TEXT("nested_history")));
    TestFalse(TEXT("the nested history diagnostic is not left at line -1"),
        PwModelBevelSafetyTest_HasLineMinusOneSelfIntersection(Result.Diagnostics));

    return true;
}
