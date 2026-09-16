// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for F-pcg-decompile-ir.
//
// Builds a transient PCG graph with a UPCGLoadDataAssetSettings node
// whose `Asset` field (a TSoftObjectPtr<UPCGDataAsset>) is set to a
// stable engine-content path. Decompiles and asserts the bare asset
// path appears in the output — not the wrapped ExportText form
// `PCGDataAsset'/Game/.../Foo.Foo'`.
//
// Counterfactual: if FormatReflectedPropertyValue's FSoftObjectProperty
// branch is removed and the walker falls through to raw
// ExportTextItem_InContainer, soft refs emit as the wrapped
// `Class'/Path.Name'` form and the bare-path assertion fails.
// Elements/IO/PCGLoadAssetElement.h (UPCGLoadDataAssetSettings) is a UE 5.4+ PCG header; it
// does not exist on 5.3, so this test compiles only where the load-asset element is available.
#if WITH_DEV_AUTOMATION_TESTS && __has_include("PCGGraph.h") && __has_include("Elements/IO/PCGLoadAssetElement.h")

#include "Misc/AutomationTest.h"

#include "PCGIR/PCGIRDecompiler.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGSettings.h"
#include "Elements/IO/PCGLoadAssetElement.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCGIRDecompileAssetRef_EmitsPathName,
    "PinWright.pcgir.decompile.AssetRefProperty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPCGIRDecompileAssetRef_EmitsPathName::RunTest(const FString& Parameters)
{
    UPCGGraph* Graph = NewObject<UPCGGraph>(GetTransientPackage());
    TestNotNull(TEXT("Transient UPCGGraph created"), Graph);
    if (!Graph) return false;

    UPCGSettings* DefaultSettings = nullptr;
    UPCGNode* Node = Graph->AddNodeOfType(UPCGLoadDataAssetSettings::StaticClass(), DefaultSettings);
    TestNotNull(TEXT("LoadDataAsset node created"), Node);
    if (!Node) return false;

    UPCGLoadDataAssetSettings* LoadSettings = Cast<UPCGLoadDataAssetSettings>(Node->GetSettings());
    TestNotNull(TEXT("Node settings is UPCGLoadDataAssetSettings"), LoadSettings);
    if (!LoadSettings) return false;

    // Synthetic but well-formed soft path. The soft-ref property branch
    // emits whatever string the path holds; the asset doesn't have to
    // resolve for this test to verify the formatting branch.
    const FString TestPath = TEXT("/Game/PinWrightTests/TestAsset.TestAsset");
    LoadSettings->Asset = TSoftObjectPtr<UPCGDataAsset>(FSoftObjectPath(TestPath));

    const FPCGIRDecompileResult Result = FPCGIRDecompiler::DecompileGraph(Graph);
    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    const FString& Text = Result.PCGIRText;

    TestTrue(FString::Printf(TEXT("Text contains bare asset path '%s' (text='%s')"), *TestPath, *Text),
        Text.Contains(TestPath));
    // Wrapped ExportText form would be `PCGDataAsset'/Game/.../TestAsset.TestAsset'`;
    // the FSoftObjectProperty branch must produce the bare path instead.
    TestFalse(FString::Printf(TEXT("Text does NOT use wrapped ExportText form (text='%s')"), *Text),
        Text.Contains(TEXT("PCGDataAsset'/")));

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && __has_include("PCGGraph.h")
