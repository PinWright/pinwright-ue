// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-landscape-handler-bypasses-ctx.
//
// Async handlers (landscape.*, asset.*) finish their work inside an
// AsyncTask(GameThread) lambda. Before the fix they captured RequestId +
// WeakSubsystem and called the subsystem's response APIs directly, bypassing
// FResponseCapture — so test fixtures driving the RPC through
// MakeTestContextWithCapture could never observe the async response, and the
// dispatcher's text-formatter capture path was silently skipped.
//
// The fix routes the async completion through FAsyncResponseToken (built from
// the context via Ctx.MakeAsyncToken()), which honours the capture pointer.
// These tests assert that the async success of two migrated handlers
// (landscape.create and asset.set_tags) lands on the capture, and that one
// representative method per migrated handler file stays registered.
//
// Counterfactual: if a handler reverts to Sub->SendAutomationResponse
// directly, Capture.bWasCalled stays false because the subsystem bypass skips
// the capture — the matching test fails.
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

// PumpUntilCaptured (the game-thread task-queue pump for shared-capture async
// tests) lives in Tests/TestUtils.h alongside InvokeHandlerWithSharedCapture.

// ---- landscape.create — async success flows through FResponseCapture ----
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAsyncLandscapeCreateRoutesThroughCaptureTest,
    "PinWright.landscape.create.AsyncRoutesThroughCapture",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAsyncLandscapeCreateRoutesThroughCaptureTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping landscape.create capture test"));
        return true;
    }

    TestTrue(TEXT("landscape.create handler registered"),
        IsHandlerRegistered(TEXT("landscape.create")));

    const FString LandscapeLabel = FString::Printf(TEXT("MCP_AsyncCaptureLandscape_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), LandscapeLabel);
    // Keep the grid tiny so creation is fast in the test world.
    Payload->SetNumberField(TEXT("componentsX"), 1);
    Payload->SetNumberField(TEXT("componentsY"), 1);
    Payload->SetNumberField(TEXT("quadsPerComponent"), 7);

    // Shared-owned capture: the handler completes inside an AsyncTask(GameThread)
    // lambda, which may drain after this test returns. A weak handle on the async
    // token means a late completion can't write through a freed capture.
    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    const bool bFound = InvokeHandlerWithSharedCapture(TEXT("landscape.create"), Payload, Capture);
    TestTrue(TEXT("landscape.create handler invoked"), bFound);
    if (!bFound)
    {
        return false;
    }

    // The handler dispatches its real work to the game thread; drain the queue.
    PumpUntilCaptured(*Capture, /*TimeoutSeconds=*/15.0);

    // Core regression assertion: the async response reached the capture struct.
    // Without the FAsyncResponseToken fix this stays false (subsystem bypass).
    TestTrue(TEXT("Async response reached FResponseCapture (bWasCalled)"),
        Capture->bWasCalled);

    if (Capture->bWasCalled && Capture->bSuccess)
    {
        TestNotNull(TEXT("Captured success result is non-null"), Capture->Result.Get());
        if (Capture->Result.IsValid())
        {
            FString LandscapePath;
            TestTrue(TEXT("Captured result carries landscapePath"),
                Capture->Result->TryGetStringField(TEXT("landscapePath"), LandscapePath));
            TestFalse(TEXT("landscapePath is non-empty"), LandscapePath.IsEmpty());
        }

        // Clean up the actor spawned by the async handler so repeated runs stay
        // deterministic. The actor is transient (not saved), so destroying it
        // from the world is sufficient.
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            if (It->GetActorLabel().Equals(LandscapeLabel, ESearchCase::IgnoreCase))
            {
                It->Destroy();
                break;
            }
        }
    }

    return true;
}

// ---- All five migrated handlers stay registered ----
// Guards against a handler being dropped or renamed out from under the
// FAsyncResponseToken migration. Registration is cheap and synchronous; the
// async-capture behaviour is exercised by the landscape.create and
// asset.set_tags tests below.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAsyncMigratedHandlersRegisteredTest,
    "PinWright.AsyncTokenMigration.HandlersRegistered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAsyncMigratedHandlersRegisteredTest::RunTest(const FString& Parameters)
{
    // One representative method per migrated handler file.
    const TCHAR* Methods[] = {
        TEXT("landscape.create"),     // LandscapeHandler.cpp
        TEXT("asset.import"),         // AssetManageHandler.cpp
        TEXT("asset.set_tags"),       // AssetMetadataHandler.cpp
        TEXT("asset.fixup_redirectors"), // AssetWorkflowHandler.cpp
        TEXT("blueprint.create"),     // BlueprintCreationHandler.cpp
    };
    for (const TCHAR* Method : Methods)
    {
        TestTrue(FString::Printf(TEXT("%s handler registered"), Method),
            IsHandlerRegistered(Method));
    }
    return true;
}

// ---- asset.set_tags — async success flows through FResponseCapture ----
// Counterfactual for issue #1 (AssetMetadataHandler.set_tags migration): the
// empty-tags no-op path completes inside the AsyncTask(GameThread) lambda via
// Token->SendSuccess. If the handler reverts to Subsystem->SendAutomationResponse
// directly, the capture is bypassed and Capture.bWasCalled stays false.
// This path needs only a non-empty assetPath and no asset on disk, so it is
// deterministic with no asset setup or cleanup.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAsyncSetTagsRoutesThroughCaptureTest,
    "PinWright.asset.set_tags.AsyncRoutesThroughCapture",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAsyncSetTagsRoutesThroughCaptureTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("asset.set_tags handler registered"),
        IsHandlerRegistered(TEXT("asset.set_tags")));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    // Non-empty assetPath passes the synchronous validation; an empty "tags"
    // array drives the async no-op success branch that routes through the token.
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/NonExistent_AsyncCaptureProbe"));
    Payload->SetArrayField(TEXT("tags"), TArray<TSharedPtr<FJsonValue>>());

    // Shared-owned capture: the no-op tag write completes inside an
    // AsyncTask(GameThread) lambda that may drain after this test returns. A weak
    // handle on the async token means a late completion can't write through a
    // freed capture (the original use-after-free crash).
    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
    const bool bFound = InvokeHandlerWithSharedCapture(TEXT("asset.set_tags"), Payload, Capture);
    TestTrue(TEXT("asset.set_tags handler invoked"), bFound);
    if (!bFound)
    {
        return false;
    }

    // The handler dispatches its completion to the game thread; drain the queue.
    PumpUntilCaptured(*Capture, /*TimeoutSeconds=*/15.0);

    // Core regression assertion: the async response reached the capture struct.
    // Without the FAsyncResponseToken fix this stays false (subsystem bypass).
    TestTrue(TEXT("Async response reached FResponseCapture (bWasCalled)"),
        Capture->bWasCalled);

    if (Capture->bWasCalled)
    {
        TestTrue(TEXT("No-op tag write reported success"), Capture->bSuccess);
        if (Capture->bSuccess && Capture->Result.IsValid())
        {
            double AppliedTags = -1.0;
            TestTrue(TEXT("Captured result carries appliedTags"),
                Capture->Result->TryGetNumberField(TEXT("appliedTags"), AppliedTags));
            TestEqual(TEXT("appliedTags is zero for empty input"), AppliedTags, 0.0);
        }
    }

    return true;
}
