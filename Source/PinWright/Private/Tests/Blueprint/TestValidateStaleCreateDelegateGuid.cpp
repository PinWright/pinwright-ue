// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpValidateIntegrityDetectsStaleCreateDelegateGuidTest,
    "PinWright.blueprint.ValidateIntegrityDetectsStaleCreateDelegateGuid",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpValidateIntegrityDetectsStaleCreateDelegateGuidTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/ValidateStaleGuid_%s"),
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

    // Plant a UK2Node_CustomEvent named "HostEvent" so the BP has a real custom
    // event that we can look up the canonical GUID for after compile.
    UK2Node_CustomEvent* CustomEvent = NewObject<UK2Node_CustomEvent>(EventGraph);
    CustomEvent->CustomFunctionName = FName(TEXT("HostEvent"));
    CustomEvent->CreateNewGuid();
    EventGraph->AddNode(CustomEvent, /*bFromUI=*/false, /*bSelectNewNode=*/false);
    CustomEvent->AllocateDefaultPins();

    // Compile so that "HostEvent" gets a real UFunction on the generated class and
    // is registered with a canonical GUID in the blueprint's function-graph map.
    FKismetEditorUtilities::CompileBlueprint(BP);

    // Look up the canonical GUID for the freshly compiled custom event.
    FGuid CanonicalGuid;
    const bool bGotCanonical = FBlueprintEditorUtils::GetFunctionGuidFromClassByFieldName(
        BP->GeneratedClass, FName(TEXT("HostEvent")), CanonicalGuid);

    if (!TestTrue(TEXT("GetFunctionGuidFromClassByFieldName succeeded for HostEvent"), bGotCanonical))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }
    if (!TestTrue(TEXT("Canonical GUID for HostEvent is valid"), CanonicalGuid.IsValid()))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    // Plant a UK2Node_CreateDelegate whose SelectedFunctionName matches ("HostEvent")
    // but whose SelectedFunctionGuid is a fresh random GUID — not the canonical one.
    // This is the stale-GUID condition: name resolves via ResolveMember but the
    // stored identity no longer matches the event's canonical registration.
    UK2Node_CreateDelegate* StaleDelegate = NewObject<UK2Node_CreateDelegate>(EventGraph);
    StaleDelegate->SelectedFunctionName = FName(TEXT("HostEvent"));
    StaleDelegate->SelectedFunctionGuid = FGuid::NewGuid(); // random, not canonical
    StaleDelegate->CreateNewGuid();
    EventGraph->AddNode(StaleDelegate, /*bFromUI=*/false, /*bSelectNewNode=*/false);
    StaleDelegate->AllocateDefaultPins();

    // Run the validator. The GUID-equality branch added to IsCreateDelegateNodeValid
    // should catch the mismatch between SelectedFunctionGuid and the canonical GUID
    // returned by GetFunctionGuidFromClassByFieldName.
    TArray<BlueprintHandlerUtils::FBlueprintIntegrityFailure> Failures;
    const bool bResult = BlueprintHandlerUtils::ValidateBlueprintGraphIntegrity(BP, Failures);

    TestFalse(TEXT("ValidateBlueprintGraphIntegrity returns false for BP with stale CreateDelegate GUID"), bResult);
    TestTrue(TEXT("At least one failure recorded"), Failures.Num() >= 1);

    // Find the CreateDelegate failure naming the stale GUID and "HostEvent".
    bool bFoundExpected = false;
    for (const BlueprintHandlerUtils::FBlueprintIntegrityFailure& F : Failures)
    {
        if (F.NodeKind == TEXT("K2Node_CreateDelegate")
            && F.Reason.Contains(TEXT("stale GUID"))
            && F.Reason.Contains(TEXT("HostEvent")))
        {
            bFoundExpected = true;
            break;
        }
    }
    TestTrue(
        TEXT("Failure list contains K2Node_CreateDelegate entry with 'stale GUID' and 'HostEvent'"),
        bFoundExpected);

    CleanupTestAsset(AssetPath);
    return true;
}

#endif
