// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-bp-saved-state-corruption-mcp-edits P0-10:
// `ScrubStaleUFunctionsFromClass` must remove orphan UFunction entries from
// `GeneratedClass` (and `SkeletonGeneratedClass`) that were left behind after
// Phase 0 deletes their backing K2Node_CustomEvent / function graph. Without
// this scrub, `FKismetEditorUtilities::CompileBlueprint(..., RegenerateSkeletonOnly)`
// skips `CleanAndSanitizeClass` (the only engine pass that clears
// `Class->Children` / `FuncMap`), the orphan UFunction survives to disk, and
// cold reload crashes in `TFieldIterator<UFunction>` before K2Node logic runs.
//
// Test strategy: plant a `UK2Node_CustomEvent("StaleHost")` on a transient BP,
// compile so the UFunction lands on `GeneratedClass`. Then remove the node via
// raw `Graph->RemoveNode` — matches the uncleaned state Phase 0 leaves on the
// class if nothing scrubs. Confirm the orphan is still on the class (the bug's
// pre-scrub state). Call `ScrubStaleUFunctionsFromClass({"StaleHost"})`, then
// assert the orphan is gone from both `FindFunctionByName` (FuncMap) and
// `TFieldIterator<UFunction>` (Children linked list).
//
// Counterfactual: if the `ScrubStaleUFunctionsFromClass` call in Phase 0c is
// reverted (or the function's body no-ops), `FindFunctionByName` still returns
// non-null after the raw node remove, and `TFieldIterator` still yields the
// orphan — the `TestNull` and `TestFalse(bFoundInFieldIter)` assertions fail.

#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "Tests/TestUtils.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#if defined(__has_include) && __has_include("K2Node_CustomEvent.h")
#include "K2Node_CustomEvent.h"
#define MCP_TEST_HAS_CUSTOM_EVENT 1
#else
#define MCP_TEST_HAS_CUSTOM_EVENT 0
#endif

#if MCP_TEST_HAS_CUSTOM_EVENT

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirScrubStaleUFunctionsFromClassTest,
    "PinWright.blueprint.ScrubStaleUFunctionsFromClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirScrubStaleUFunctionsFromClassTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/ScrubStaleUFunctions_%s"),
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

    // Plant a custom event and compile so the UFunction lands on both
    // SkeletonGeneratedClass and GeneratedClass.
    const FName StaleName(TEXT("StaleHost"));
    UK2Node_CustomEvent* CustomEvent = NewObject<UK2Node_CustomEvent>(EventGraph);
    CustomEvent->CustomFunctionName = StaleName;
    CustomEvent->CreateNewGuid();
    EventGraph->AddNode(CustomEvent, /*bFromUI=*/false, /*bSelectNewNode=*/false);
    CustomEvent->AllocateDefaultPins();

    FKismetEditorUtilities::CompileBlueprint(BP);

    // Sanity: the UFunction exists on GeneratedClass after compile.
    if (!TestNotNull(TEXT("UFunction 'StaleHost' exists on GeneratedClass after compile"),
        BP->GeneratedClass
            ? BP->GeneratedClass->FindFunctionByName(StaleName, EIncludeSuperFlag::ExcludeSuper)
            : nullptr))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    // Remove the CustomEvent via raw `Graph->RemoveNode` — matches the uncleaned
    // state Phase 0 would leave on GeneratedClass if nothing scrubs.
    // UEdGraph::RemoveNode gained a third bAlwaysMarkDirty parameter in UE 5.6;
    // on 5.4/5.5 only the two-argument form is available.
#if UE_VERSION_OLDER_THAN(5, 6, 0)
    EventGraph->RemoveNode(CustomEvent, /*bBreakAllLinks=*/false);
#else
    EventGraph->RemoveNode(CustomEvent, /*bBreakAllLinks=*/false, /*bAlwaysMarkDirty=*/false);
#endif

    // Pre-scrub sanity: the UFunction is STILL findable on GeneratedClass.
    TestNotNull(
        TEXT("Pre-scrub: UFunction 'StaleHost' still on GeneratedClass after raw Graph->RemoveNode"),
        BP->GeneratedClass->FindFunctionByName(StaleName, EIncludeSuperFlag::ExcludeSuper));

    // Run the scrub. Expect >= 1 scrubbed (at least the GeneratedClass entry;
    // SkeletonGeneratedClass may also have it, in which case count will be 2).
    TSet<FName> Wiped;
    Wiped.Add(StaleName);
    const int32 Scrubbed = BlueprintHandlerUtils::ScrubStaleUFunctionsFromClass(BP, Wiped);

    TestTrue(TEXT("ScrubStaleUFunctionsFromClass returns >= 1"), Scrubbed >= 1);

    // FuncMap path: FindFunctionByName must miss.
    TestNull(
        TEXT("Post-scrub: UFunction 'StaleHost' no longer on GeneratedClass (FindFunctionByName)"),
        BP->GeneratedClass->FindFunctionByName(StaleName, EIncludeSuperFlag::ExcludeSuper));

    // Defensive: TFieldIterator confirms it's out of Class->Children, not just FuncMap.
    bool bFoundInFieldIter = false;
    for (TFieldIterator<UFunction> It(BP->GeneratedClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
    {
        if (*It && It->GetFName() == StaleName)
        {
            bFoundInFieldIter = true;
            break;
        }
    }
    TestFalse(
        TEXT("Post-scrub: TFieldIterator<UFunction> on GeneratedClass yields no 'StaleHost'"),
        bFoundInFieldIter);

    // Integrity gate now passes — orphan is gone from the gate's perspective.
    TArray<BlueprintHandlerUtils::FBlueprintIntegrityFailure> Failures;
    const bool bIntegrityOk = BlueprintHandlerUtils::ValidateBlueprintGraphIntegrity(BP, Failures);
    TestTrue(TEXT("Post-scrub: ValidateBlueprintGraphIntegrity returns true"), bIntegrityOk);
    TestEqual(TEXT("Post-scrub: no integrity failures recorded"), Failures.Num(), 0);

    CleanupTestAsset(AssetPath);
    return true;
}

#endif
