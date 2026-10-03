// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-compile-bpir-no-persistence-field: a successful
// blueprint.compile_bpir answered {compiled:true, status:"UpToDate", success:true} with no
// persistence field at all, while it writes nothing to disk, so eight authored graphs read as
// durable and died with the editor. The response now carries the measured mark-dirty block
// blueprint.set_default emits. Counterfactual: drop the AddMarkDirtySaveReport call in
// BpirCompilerHandler.cpp and every field assertion below fails.

#include "Misc/AutomationTest.h"
#include "Tests/TestAssetTeardown.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirCompileBpirPersistenceReportTest,
    "PinWright.blueprint.compile_bpir.ReportsUnsavedPersistence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirCompileBpirPersistenceReportTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/__PW_GatewayTests/BpirPersistenceReport_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    UPackage* Package = CreatePackage(*AssetPath);
    if (!TestNotNull(TEXT("Package created"), Package))
    {
        return true;
    }
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        Package,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());
    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPath(AssetPath));
        return true;
    }
    ON_SCOPE_EXIT
    {
        if (UPackage* BlueprintPackage = BP->GetOutermost())
        {
            BlueprintPackage->SetDirtyFlag(false);
        }
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPath(AssetPath));
    };
    BP->SetFlags(RF_Transactional);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);
    Payload->SetStringField(TEXT("code"), TEXT(
        "entry custom_event PersistenceReportProbe() {\n"
        "    call PrintString(InString: \"probe\")\n"
        "}\n"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("blueprint.compile_bpir handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.compile_bpir"), Payload, Capture));
    if (!TestTrue(FString::Printf(TEXT("compile_bpir succeeds (%s %s)"), *Capture.ErrorCode, *Capture.Message),
            Capture.bSuccess)
        || !TestTrue(TEXT("success payload present"), Capture.Result.IsValid()))
    {
        return true;
    }

    // The fixture package was never written, so the honest answer is "not on disk, flush needed".
    const TSharedPtr<FJsonObject>& Result = Capture.Result;
    bool bValue = false;
    TestTrue(TEXT("response carries saveRequested"), Result->TryGetBoolField(TEXT("saveRequested"), bValue));
    TestTrue(TEXT("response carries markedForSave:true"),
        Result->TryGetBoolField(TEXT("markedForSave"), bValue) && bValue);
    TestTrue(TEXT("response carries saved:false (nothing was written)"),
        Result->TryGetBoolField(TEXT("saved"), bValue) && !bValue);
    TestTrue(TEXT("response carries pendingFlush:true"),
        Result->TryGetBoolField(TEXT("pendingFlush"), bValue) && bValue);
    TestTrue(TEXT("compile_bpir left the package dirty for the pending flush"),
        BP->GetOutermost()->IsDirty());
    return true;
}
