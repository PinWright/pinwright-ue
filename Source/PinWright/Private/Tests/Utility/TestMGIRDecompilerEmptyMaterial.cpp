// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-asset-dump-mgir-empty-graph-ambiguous.
//
// MGIR dumps of materials with no expression graph (engine-generated HLOD,
// bUseMaterialAttributes fallthrough, runtime stand-ins) were previously
// indistinguishable from a failed graph extraction: both produced
// `entry material <name-token> {\n}`. The decompiler now emits a canonical
// `    # no expression graph` marker inside the body when zero expression
// or output lines were appended.
//
// Counterfactual: If the empty-body detection in DecompileMaterial is
// reverted, this test fails because the resulting text is
// `entry material <name-token> {\n}` with no marker line.
#include "Misc/AutomationTest.h"

#include "MGIR/MGIRDecompiler.h"
#include "Materials/Material.h"
#include "UObject/Package.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMGIRDecompileEmptyMaterial_EmitsNoGraphMarker,
    "PinWright.mgir.decompile.EmptyMaterialEmitsMarker",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMGIRDecompileEmptyMaterial_EmitsNoGraphMarker::RunTest(const FString& Parameters)
{
    UMaterial* Mat = NewObject<UMaterial>(GetTransientPackage());
    TestNotNull(TEXT("Transient UMaterial created"), Mat);
    if (!Mat) return false;

    const FMGIRDecompileResult Result = FMGIRDecompiler::DecompileMaterial(Mat);
    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    const FString& Text = Result.MGIRText;
    const int32 MarkerIndex = Text.Find(TEXT("# no expression graph"));
    TestTrue(FString::Printf(TEXT("Marker present (text='%s')"), *Text), MarkerIndex != INDEX_NONE);
    if (MarkerIndex == INDEX_NONE) return false;

    const int32 OpenBraceIndex = Text.Find(TEXT("{"));
    const int32 CloseBraceIndex = Text.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
    TestTrue(TEXT("Body has '{'"), OpenBraceIndex != INDEX_NONE);
    TestTrue(TEXT("Body has '}'"), CloseBraceIndex != INDEX_NONE);
    TestTrue(TEXT("Marker sits after '{'"), MarkerIndex > OpenBraceIndex);
    TestTrue(TEXT("Marker sits before '}'"), MarkerIndex < CloseBraceIndex);

    return true;
}
