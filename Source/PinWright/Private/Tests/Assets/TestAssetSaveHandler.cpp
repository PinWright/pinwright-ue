// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for asset.save (F-asset-save).
//
// asset.save is the generic single-asset persistence verb. Coverage:
//   1. asset.save is registered with category "asset".
//   2. asset.save with a missing assetPath → INVALID_PARAMS (RequireString).
//   3. asset.save with a nonexistent asset path → ASSET_NOT_FOUND.
//   4. Live Blueprint integrity gate: saving a UBlueprint with a stale
//      CreateDelegate ref surfaces integrityGate:"blocked" + integrityFailures
//      inline and reports saved:false. This is the inline verdict that, before
//      F-asset-save, lived only in blueprint.compile and was lost on every other
//      save path.
//
// Persistence honesty (B-niagara-save-no-disk-write sibling contract): the handler
// routes through SaveAssetToDiskReportingPresence (NOT the mark-dirty McpSafeAssetSave)
// so saved:true is gated on the .uasset reaching disk; the predicate is unit-tested in
// TestLevelSaveLoadUtils.cpp (FAssetSavePolicyRequiresDiskFileTest). If the handler were
// reverted to raw SaveLoadedAssetThrottled or omitted the handler-level integrity check,
// assertions 1/3/4 here fail.

#include "Misc/AutomationTest.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

#if defined(__has_include) && (__has_include("BlueprintGraph/K2Node_CreateDelegate.h") || __has_include("BlueprintGraph/Classes/K2Node_CreateDelegate.h") || __has_include("K2Node_CreateDelegate.h"))
#if __has_include("BlueprintGraph/K2Node_CreateDelegate.h")
#include "BlueprintGraph/K2Node_CreateDelegate.h"
#elif __has_include("BlueprintGraph/Classes/K2Node_CreateDelegate.h")
#include "BlueprintGraph/Classes/K2Node_CreateDelegate.h"
#else
#include "K2Node_CreateDelegate.h"
#endif
#define PW_HAS_CREATE_DELEGATE 1
#else
#define PW_HAS_CREATE_DELEGATE 0
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAssetSaveHandlerTest,
    "PinWright.asset.save",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetSaveHandlerTest::RunTest(const FString& Parameters)
{
    // ------------------------------------------------------------------
    // 1. asset.save must be registered with category "asset".
    // ------------------------------------------------------------------
    {
        bool bFound = false;
        for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
        {
            if (Reg.MethodName == TEXT("asset.save"))
            {
                bFound = true;
                TestEqual(TEXT("asset.save category"), Reg.Category, TEXT("asset"));
                TestTrue(TEXT("asset.save func pointer non-null"), Reg.Func != nullptr);
                break;
            }
        }
        TestTrue(TEXT("asset.save is registered"), bFound);
    }

    // ------------------------------------------------------------------
    // 2. asset.save with no assetPath → INVALID_PARAMS (RequireString).
    // ------------------------------------------------------------------
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("asset.save"), Params, Capture);
        TestTrue(TEXT("asset.save missing assetPath sent a response"), Capture.bWasCalled);
        TestFalse(TEXT("asset.save missing assetPath is not a success"), Capture.bSuccess);
        TestEqual(TEXT("asset.save missing assetPath error code"),
            Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
    }

    // ------------------------------------------------------------------
    // 3. asset.save with a nonexistent asset path → ASSET_NOT_FOUND.
    // ------------------------------------------------------------------
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("assetPath"), TEXT("/Game/Test/DoesNotExistAsset_XYZZY"));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("asset.save"), Params, Capture);
        TestTrue(TEXT("asset.save not_found sent a response"), Capture.bWasCalled);
        TestFalse(TEXT("asset.save not_found is not a success"), Capture.bSuccess);
        TestEqual(TEXT("asset.save not_found error code"),
            Capture.ErrorCode, FString(TEXT("ASSET_NOT_FOUND")));
    }

    // ------------------------------------------------------------------
    // 4. Live Blueprint integrity gate surfaced inline. Build a real BP,
    //    plant a stale CreateDelegate so ValidateBlueprintGraphIntegrity
    //    fails, then asset.save it and assert the inline verdict:
    //    integrityGate:"blocked" + integrityFailures + saved:false.
    // ------------------------------------------------------------------
#if PW_HAS_CREATE_DELEGATE
    {
        const FString AssetPath = FString::Printf(
            TEXT("/Game/__PW_GatewayTests/AssetSaveIntegrity_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));

        UPackage* Pkg = CreatePackage(*AssetPath);
        if (TestNotNull(TEXT("Package created"), Pkg))
        {
            UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
                AActor::StaticClass(), Pkg,
                FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
                BPTYPE_Normal, UBlueprint::StaticClass(),
                UBlueprintGeneratedClass::StaticClass());

            if (TestNotNull(TEXT("Blueprint created"), BP))
            {
                UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(BP);
                if (TestNotNull(TEXT("EventGraph exists"), EventGraph))
                {
                    // Plant a CreateDelegate whose SelectedFunctionName resolves
                    // against nothing on the generated class → integrity gate fails.
                    UK2Node_CreateDelegate* Stale = NewObject<UK2Node_CreateDelegate>(EventGraph);
                    Stale->SelectedFunctionName = FName(TEXT("DoesNotExistAnywhere_AssetSave"));
                    Stale->CreateNewGuid();
                    EventGraph->AddNode(Stale, /*bFromUI=*/false, /*bSelectNewNode=*/false);
                    Stale->AllocateDefaultPins();

                    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
                    // Use the ObjectPath form so LoadObject resolves the in-memory asset.
                    Params->SetStringField(TEXT("assetPath"),
                        FString::Printf(TEXT("%s.%s"), *AssetPath,
                            *FPackageName::GetLongPackageAssetName(AssetPath)));

                    FTestResponseCapture Capture;
                    InvokeHandlerWithCapture(TEXT("asset.save"), Params, Capture);

                    TestTrue(TEXT("asset.save integrity-block sent a response"), Capture.bWasCalled);
                    // The gate is reported via a successful response carrying the
                    // verdict fields, mirroring blueprint.compile's contract.
                    TestTrue(TEXT("asset.save integrity-block is a success response"), Capture.bSuccess);
                    if (Capture.Result.IsValid())
                    {
                        bool bSaved = true;
                        Capture.Result->TryGetBoolField(TEXT("saved"), bSaved);
                        TestFalse(TEXT("asset.save integrity-block reports saved:false"), bSaved);

                        FString Gate;
                        const bool bHasGate = Capture.Result->TryGetStringField(TEXT("integrityGate"), Gate);
                        TestTrue(TEXT("asset.save integrity-block carries integrityGate field"), bHasGate);
                        TestEqual(TEXT("asset.save integrityGate value is 'blocked'"),
                            Gate, FString(TEXT("blocked")));

                        const TArray<TSharedPtr<FJsonValue>>* Failures = nullptr;
                        const bool bHasFailures =
                            Capture.Result->TryGetArrayField(TEXT("integrityFailures"), Failures);
                        TestTrue(TEXT("asset.save integrity-block carries integrityFailures array"), bHasFailures);
                        TestTrue(TEXT("asset.save integrityFailures non-empty"),
                            bHasFailures && Failures && Failures->Num() >= 1);
                    }
                    else
                    {
                        AddError(TEXT("asset.save integrity-block returned no result object"));
                    }
                }
            }
        }
        CleanupTestAsset(AssetPath);
    }
#else
    PinWrightTestSkip::SkipAssertions(*this, TEXT("create-delegate-header-absent"),
        TEXT("UK2Node_CreateDelegate header unavailable; skipping live integrity-gate assertion (4)"));
#endif

    return true;
}
