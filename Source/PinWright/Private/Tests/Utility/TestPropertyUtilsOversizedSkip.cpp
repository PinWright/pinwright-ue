// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Utils/PropertyUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "UObject/Package.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyUtilsOversizedSkipTest,
    "PinWright.utils.property_utils.OversizedSkip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyUtilsOversizedSkipTest::RunTest(const FString& Parameters)
{
    UInstancedStaticMeshComponent* Comp =
        NewObject<UInstancedStaticMeshComponent>(GetTransientPackage());
    TestNotNull(TEXT("Constructed transient UInstancedStaticMeshComponent"), Comp);
    if (!Comp) return false;

    // Populate the array the placeholder summarizes. With zero instances the element
    // count in "$omitted" is indistinguishable from a hard-coded 0 or from the
    // non-FArrayProperty fallback, so the count half of the placeholder would be
    // untestable. Three distinct transforms make the count load-bearing.
    // (A mesh-less transient ISM accepts AddInstance — same pattern as
    // TestAssetDumpInstancedSubobjects.cpp:163.)
    Comp->AddInstance(FTransform(FVector(100.0, 0.0, 0.0)));
    Comp->AddInstance(FTransform(FVector(0.0, 200.0, 0.0)));
    Comp->AddInstance(FTransform(FVector(0.0, 0.0, 300.0)));
    TestEqual(TEXT("Fixture seeded three instances"), Comp->GetInstanceCount(), 3);

    TSharedPtr<FJsonObject> Result = BuildClassPropertyJson(Comp, nullptr);
    TestTrue(TEXT("BuildClassPropertyJson returned a valid object"), Result.IsValid());
    if (!Result.IsValid()) return false;

    TestTrue(TEXT("'PerInstanceSMData' field is present"),
             Result->Values.Contains(TEXT("PerInstanceSMData")));

    const TSharedPtr<FJsonObject>* PlaceholderObj = nullptr;
    const bool bGotObject = Result->TryGetObjectField(TEXT("PerInstanceSMData"), PlaceholderObj);
    TestTrue(TEXT("'PerInstanceSMData' value is an object (placeholder shape, not raw array)"),
             bGotObject && PlaceholderObj != nullptr && PlaceholderObj->IsValid());
    if (!bGotObject || !PlaceholderObj || !PlaceholderObj->IsValid()) return false;

    FString OmittedSummary;
    const bool bGotOmitted = (*PlaceholderObj)->TryGetStringField(TEXT("$omitted"), OmittedSummary);
    TestTrue(TEXT("Placeholder has non-empty '$omitted' summary"),
             bGotOmitted && !OmittedSummary.IsEmpty());
    // Pin the whole summary, element count included. Non-emptiness alone is satisfied by
    // the "?" fallback at PropertyExport.cpp:1295, so deleting the FArrayProperty branch
    // (PropertyExport.cpp:1287-1292) — the only thing that ever reports a real count —
    // would leave a bare non-empty assertion green. The count is the sole quantitative
    // signal a consumer gets in place of the omitted array.
    TestEqual(TEXT("'$omitted' names the real element count"),
              OmittedSummary, FString(TEXT("per-instance transforms, 3 elements")));

    FString ReasonStr;
    const bool bGotReason = (*PlaceholderObj)->TryGetStringField(TEXT("$reason"), ReasonStr);
    TestTrue(TEXT("Placeholder '$reason' is 'exceeds-llm-budget'"),
             bGotReason && ReasonStr == TEXT("exceeds-llm-budget"));

    FString TypeStr;
    const bool bGotType = (*PlaceholderObj)->TryGetStringField(TEXT("type"), TypeStr);
    TestTrue(TEXT("Placeholder 'type' is 'TArray<FInstancedStaticMeshInstanceData>'"),
             bGotType && TypeStr == TEXT("TArray<FInstancedStaticMeshInstanceData>"));

    return true;
}
