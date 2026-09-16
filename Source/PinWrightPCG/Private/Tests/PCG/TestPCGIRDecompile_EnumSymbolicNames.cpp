// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for F-pcg-decompile-ir.
//
// Builds a transient PCG graph with a UPCGUnionSettings node whose
// `Type` enum (EPCGUnionType) is set to KeepAll (non-default; default
// is LeftToRightPriority). Decompiles and asserts the text contains
// the symbolic enum name "KeepAll" and does NOT contain the raw
// integer value "2" as the property value.
//
// Counterfactual: if FormatReflectedPropertyValue's enum branch is
// bypassed and ExportTextItem_InContainer is used instead, the value
// is emitted as `Type = "2"` rather than `Type = "KeepAll"` —
// symbolic-name assertion fails.
#if WITH_DEV_AUTOMATION_TESTS && __has_include("PCGGraph.h")

#include "Misc/AutomationTest.h"

#include "PCGIR/PCGIRDecompiler.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGSettings.h"
#include "Elements/PCGUnionElement.h"
#include "Data/PCGUnionData.h"
#include "UObject/Package.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGIRDecompileEnumProperty_EmitsSymbolicName,
    "PinWright.pcgir.decompile.EnumSymbolicNames",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGIRDecompileEnumProperty_EmitsSymbolicName::RunTest(const FString& Parameters)
{
    UPCGGraph* Graph = NewObject<UPCGGraph>(GetTransientPackage());
    TestNotNull(TEXT("Transient UPCGGraph created"), Graph);
    if (!Graph) return false;

    UPCGSettings* DefaultSettings = nullptr;
    UPCGNode* Node = Graph->AddNodeOfType(UPCGUnionSettings::StaticClass(), DefaultSettings);
    TestNotNull(TEXT("Union node created"), Node);
    if (!Node) return false;

    UPCGUnionSettings* UnionSettings = Cast<UPCGUnionSettings>(Node->GetSettings());
    TestNotNull(TEXT("Node settings is UPCGUnionSettings"), UnionSettings);
    if (!UnionSettings) return false;

    // KeepAll is non-default (default is LeftToRightPriority, value 0).
    UnionSettings->Type = EPCGUnionType::KeepAll;

    const FPCGIRDecompileResult Result = FPCGIRDecompiler::DecompileGraph(Graph);
    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    const FString& Text = Result.PCGIRText;

    TestTrue(FString::Printf(TEXT("Text contains symbolic enum name 'KeepAll' (text='%s')"), *Text),
        Text.Contains(TEXT("KeepAll")));
    // Raw integer "Type = \"2\"" would mean the enum branch was bypassed.
    // Use the qualified form to avoid colliding with other "2" digits in
    // the output (e.g. positions, sort priorities).
    TestFalse(FString::Printf(TEXT("Text does NOT emit raw integer for Type enum (text='%s')"), *Text),
        Text.Contains(TEXT("Type = \"2\"")));

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && __has_include("PCGGraph.h")
