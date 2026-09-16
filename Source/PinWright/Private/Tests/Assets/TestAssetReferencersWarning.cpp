// Copyright (c) 2026 Alexander Penkin. MIT License.

// Test: asset.delete and asset.duplicate surface referencingBlueprints in the response.
//
// Regression guard for B-bp-saved-state-corruption-mcp-edits (Task E):
// Without the GetReferencers query, the response has no referencingBlueprints
// field, causing the assertion at the end of this test to fail.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/Assets/AssetRefDirectionFixtures.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetReferencersWarningTest,
    "PinWright.asset.delete.ReferencersWarning",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetReferencersWarningTest::RunTest(const FString& Parameters)
{
    // -------------------------------------------------------------------------
    // 1. Build a hard dependency A -> B on disk (A references B) and rescan the
    //    asset registry, via the shared fixture.
    // -------------------------------------------------------------------------
    FString PathA, PathB;
    if (!AssetRefDirectionFixtures::BuildHardDependency(*this, TEXT("ReferencerWarn"), PathA, PathB))
    {
        return true;
    }

    // -------------------------------------------------------------------------
    // 2. Invoke asset.delete on B and assert referencingBlueprints contains A
    // -------------------------------------------------------------------------
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), PathB);

    FTestResponseCapture Capture;
    const bool bInvoked = InvokeHandlerWithCapture(TEXT("asset.delete"), Payload, Capture);
    TestTrue(TEXT("asset.delete handler found"), bInvoked);
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);

    if (Capture.Result.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* RefsArray = nullptr;
        const bool bHasField =
            Capture.Result->TryGetArrayField(TEXT("referencingBlueprints"), RefsArray);
        TestTrue(TEXT("response contains referencingBlueprints field"), bHasField);

        if (bHasField && RefsArray)
        {
            bool bFoundA = false;
            for (const TSharedPtr<FJsonValue>& Val : *RefsArray)
            {
                if (Val.IsValid() && Val->AsString() == PathA)
                {
                    bFoundA = true;
                    break;
                }
            }
            TestTrue(TEXT("referencingBlueprints contains PathA"), bFoundA);
        }
    }
    else
    {
        AddError(TEXT("response result object is invalid"));
    }

    // -------------------------------------------------------------------------
    // 3. Cleanup
    // -------------------------------------------------------------------------
    // B was deleted by the handler; A may still exist.
    CleanupTestAsset(PathA);
    CleanupTestAsset(PathB);
    return true;
}
