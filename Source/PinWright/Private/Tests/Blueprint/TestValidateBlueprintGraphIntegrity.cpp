// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"

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
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "K2Node_CustomEvent.h"
#if defined(__has_include) && (__has_include("BlueprintGraph/K2Node_CreateDelegate.h") || __has_include("BlueprintGraph/Classes/K2Node_CreateDelegate.h") || __has_include("K2Node_CreateDelegate.h"))
#if __has_include("BlueprintGraph/K2Node_CreateDelegate.h")
#include "BlueprintGraph/K2Node_CreateDelegate.h"
#elif __has_include("BlueprintGraph/Classes/K2Node_CreateDelegate.h")
#include "BlueprintGraph/Classes/K2Node_CreateDelegate.h"
#else
#include "K2Node_CreateDelegate.h"
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpValidateIntegrityDetectsStaleCreateDelegateTest,
    "PinWright.blueprint.ValidateIntegrityDetectsStaleCreateDelegate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpValidateIntegrityDetectsStaleCreateDelegateTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/ValidateIntegrity_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    UPackage* Pkg = CreatePackage(*AssetPath);
    if (!TestNotNull(TEXT("Package created"), Pkg))
        return true;

    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(), Pkg,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        BPTYPE_Normal, UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());

    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(BP);
    if (!TestNotNull(TEXT("EventGraph exists"), EventGraph))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    // Plant a UK2Node_CreateDelegate with a SelectedFunctionName that does not
    // exist on the generated class. A fresh Blueprint has no custom events, so
    // any non-default name will fail the generated-class resolve. This is the
    // frozen-state equivalent of the tester's repro where a CreateDelegate's
    // SelectedFunction resolves against SKEL at compile time but not against
    // the reloaded generated class.
    UK2Node_CreateDelegate* Stale = NewObject<UK2Node_CreateDelegate>(EventGraph);
    Stale->SelectedFunctionName = FName(TEXT("DoesNotExistAnywhere_StaleRef"));
    Stale->CreateNewGuid();
    EventGraph->AddNode(Stale, /*bFromUI=*/false, /*bSelectNewNode=*/false);
    // AllocateDefaultPins so the node has a delegate-out pin — otherwise the
    // "fully unwired" skip in ValidateBlueprintGraphIntegrity would early-out
    // and mask the assertion.
    Stale->AllocateDefaultPins();

    // Run the validator.
    TArray<BlueprintHandlerUtils::FBlueprintIntegrityFailure> Failures;
    const bool bResult = BlueprintHandlerUtils::ValidateBlueprintGraphIntegrity(BP, Failures);

    TestFalse(TEXT("ValidateBlueprintGraphIntegrity returns false for BP with stale SelectedFunctionName"), bResult);
    TestTrue(TEXT("At least one failure recorded"), Failures.Num() >= 1);

    // Find the CreateDelegate failure in the list.
    bool bFoundExpected = false;
    for (const BlueprintHandlerUtils::FBlueprintIntegrityFailure& F : Failures)
    {
        if (F.NodeKind == TEXT("K2Node_CreateDelegate")
            && F.Reason.Contains(TEXT("DoesNotExistAnywhere_StaleRef")))
        {
            bFoundExpected = true;
            break;
        }
    }
    TestTrue(TEXT("Failure list contains K2Node_CreateDelegate entry naming the stale SelectedFunctionName"), bFoundExpected);

    CleanupTestAsset(AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpCompileBpirIntegrityFailureReturnsPayloadTest,
    "PinWright.blueprint.compile_bpir.IntegrityFailureReturnsPayload",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpCompileBpirIntegrityFailureReturnsPayloadTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/BpirIntegrityPayload_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    UPackage* Pkg = CreatePackage(*AssetPath);
    if (!TestNotNull(TEXT("Package created"), Pkg))
        return true;

    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(), Pkg,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        BPTYPE_Normal, UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());

    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(BP);
    if (!TestNotNull(TEXT("EventGraph exists"), EventGraph))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    const FName StaleFunctionName(TEXT("DoesNotExistAnywhere_BpirPayload"));
    UK2Node_CreateDelegate* Stale = NewObject<UK2Node_CreateDelegate>(EventGraph);
    Stale->SelectedFunctionName = StaleFunctionName;
    Stale->SelectedFunctionGuid = FGuid::NewGuid();
    Stale->CreateNewGuid();
    EventGraph->AddNode(Stale, /*bFromUI=*/false, /*bSelectNewNode=*/false);
    Stale->AllocateDefaultPins();

    // The BPIR compile pipeline runs RefreshBpirDelegateNodes after the full compile,
    // which invokes UK2Node_CreateDelegate::HandleAnyChange. When the node's stored
    // SelectedFunctionName is unresolvable AND its delegate-out pin has zero LinkedTo
    // entries, the engine code clears SelectedFunctionName to NAME_None (see
    // K2Node_CreateDelegate::HandleAnyChangeWithoutNotifying). That clearing trips the
    // integrity gate's bFullyUnwired short-circuit and hides the failure we want to
    // assert. Plant a second CreateDelegate and forge a mutual link between their
    // delegate-out pins so both have LinkedTo.Num() > 0 — this preserves
    // SelectedFunctionName through HandleAnyChange. The gate's LinkedTo walk only
    // requires the linked pin's owning node be a member of this BP's graph set, which
    // both nodes satisfy.
    UK2Node_CreateDelegate* Sibling = NewObject<UK2Node_CreateDelegate>(EventGraph);
    Sibling->CreateNewGuid();
    EventGraph->AddNode(Sibling, /*bFromUI=*/false, /*bSelectNewNode=*/false);
    Sibling->AllocateDefaultPins();
    if (UEdGraphPin* StalePin = Stale->GetDelegateOutPin())
    {
        if (UEdGraphPin* SiblingPin = Sibling->GetDelegateOutPin())
        {
            StalePin->LinkedTo.Add(SiblingPin);
            SiblingPin->LinkedTo.Add(StalePin);
        }
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);
    Payload->SetStringField(TEXT("code"),
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call PrintString(InString: \"Integrity payload probe\")\n")
        TEXT("}\n"));
    Payload->SetStringField(TEXT("mode"), TEXT("replace"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("blueprint.compile_bpir handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.compile_bpir"), Payload, Capture));
    TestTrue(TEXT("Response was sent"), Capture.bWasCalled);
    TestFalse(TEXT("Response is an error"), Capture.bSuccess);
    TestEqual(TEXT("Error code is INTEGRITY_FAILURE"),
        Capture.ErrorCode, FString(TEXT("INTEGRITY_FAILURE")));
    TestTrue(TEXT("Error result payload is present"), Capture.Result.IsValid());

    const TArray<TSharedPtr<FJsonValue>>* IntegrityFailures = nullptr;
    const bool bHasIntegrityFailures = Capture.Result.IsValid()
        && Capture.Result->TryGetArrayField(TEXT("integrityFailures"), IntegrityFailures);
    TestTrue(TEXT("Error result exposes integrityFailures"), bHasIntegrityFailures);
    TestTrue(TEXT("Integrity failure list is non-empty"),
        IntegrityFailures && IntegrityFailures->Num() > 0);

    bool bFoundStaleDelegate = false;
    if (IntegrityFailures)
    {
        for (const TSharedPtr<FJsonValue>& FailureValue : *IntegrityFailures)
        {
            const TSharedPtr<FJsonObject> FailureObject =
                FailureValue.IsValid() ? FailureValue->AsObject() : nullptr;
            if (FailureObject.IsValid()
                && FailureObject->GetStringField(TEXT("nodeKind")) == TEXT("K2Node_CreateDelegate")
                && FailureObject->GetStringField(TEXT("reason")).Contains(StaleFunctionName.ToString()))
            {
                bFoundStaleDelegate = true;
                break;
            }
        }
    }
    TestTrue(TEXT("Integrity payload names the stale CreateDelegate"), bFoundStaleDelegate);

    CleanupTestAsset(AssetPath);
    return true;
}

#endif
