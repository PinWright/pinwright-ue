// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for B-asset-import-next-tick-escapes-safepoint.
// asset.import deliberately completes asynchronously, but that continuation still
// belongs to the originating RPC and must retain its serialization and unattended
// scopes until the response-producing body returns.

#include "Misc/AutomationTest.h"

#include "Containers/Ticker.h"
#include "CoreGlobals.h"
#include "Dispatch/RpcDispatcher.h"
#include "Dispatch/SafePoint.h"
#include "Dispatch/ScopedUnattendedRpc.h"
#include "Dom/JsonObject.h"
#include "Handlers/HandlerContext.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "PinWrightSettings.h"
#include "Tests/TestUtils.h"

namespace AssetImportSafePointTests
{
    bool LoadAssetImportRegistration(FString& OutBlock)
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        if (!Plugin.IsValid())
        {
            return false;
        }

        FString Source;
        const FString SourcePath = Plugin->GetBaseDir()
            / TEXT("Source/PinWright/Private/Handlers/Asset/AssetManageHandler.cpp");
        if (!FFileHelper::LoadFileToString(Source, *SourcePath))
        {
            return false;
        }
        Source = NeutralizeSourceText(Source);

        const FString RegistrationNeedle = TEXT("REGISTER_RPC_HANDLER(\"asset.import\"");
        const int32 Start = Source.Find(RegistrationNeedle);
        if (Start == INDEX_NONE)
        {
            return false;
        }

        int32 End = Source.Find(
            TEXT("REGISTER_RPC_HANDLER("), ESearchCase::CaseSensitive,
            ESearchDir::FromStart, Start + RegistrationNeedle.Len());
        if (End == INDEX_NONE)
        {
            End = Source.Len();
        }
        OutBlock = Source.Mid(Start, End - Start);
        return true;
    }
}

// Counterfactuals:
// - Restoring GEditor->GetTimerManager()->SetTimerForNextTick fails the source
//   ratchet before any destructive import can run.
// - Replacing DeferRequestToSafePoint with bare DeferToSafePoint releases the
//   dispatcher guard when the handler returns, so the pending/queue-order checks fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetImportSafePointContinuationRetainsRequestScopeTest,
    "PinWright.asset.import.SafePointContinuationRetainsRequestScope",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetImportSafePointContinuationRetainsRequestScopeTest::RunTest(
    const FString& /*Parameters*/)
{
    using namespace AssetImportSafePointTests;

    FString ImportRegistration;
    if (TestTrue(TEXT("asset.import registration loads from production source"),
                 LoadAssetImportRegistration(ImportRegistration)))
    {
        TestTrue(TEXT("asset.import uses the retained always-deferred helper"),
            ImportRegistration.Contains(TEXT("PinWrightSafePoint::DeferRequestToSafePoint(")));
        TestFalse(TEXT("asset.import no longer uses SetTimerForNextTick"),
            ImportRegistration.Contains(TEXT("SetTimerForNextTick")));
        TestFalse(TEXT("asset.import no longer reaches an editor timer manager"),
            ImportRegistration.Contains(TEXT("GetTimerManager")));
    }
    TestFalse(TEXT("asset.import remains absent from the tick-unsafe method table"),
        PinWrightSafePoint::GetTickUnsafeMethods().Contains(TEXT("asset.import")));

    UPinWrightSettings* Settings = GetMutableDefault<UPinWrightSettings>();
    if (!Settings)
    {
        AddError(TEXT("UPinWrightSettings CDO unavailable"));
        return false;
    }

    const bool bSavedSetting = Settings->bSuppressModalDialogsDuringRpc;
    const bool bSavedGlobal = GIsRunningUnattendedScript;
    ON_SCOPE_EXIT
    {
        PinWrightAutomationMode::ResetForTests();
        Settings->bSuppressModalDialogsDuringRpc = bSavedSetting;
        GIsRunningUnattendedScript = bSavedGlobal;
    };
    PinWrightAutomationMode::ResetForTests();
    Settings->bSuppressModalDialogsDuringRpc = true;
    GIsRunningUnattendedScript = false;

    FRpcDispatcher Dispatcher;
    TArray<FString> Events;
    bool bWorkRan = false;
    bool bCallbackWasSafe = false;
    bool bCallbackWasUnattended = false;
    bool bGuardWasActiveInCallback = false;
    bool bResponderReportedDeferred = false;
    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();

    Dispatcher.RegisterHandler(TEXT("test.queued"),
        [&Events](const FString&, const FString&, const TSharedPtr<FJsonObject>&) -> bool
        {
            Events.Add(TEXT("queued"));
            return true;
        });
    Dispatcher.RegisterHandler(TEXT("test.asset_import_continuation"),
        [&Dispatcher, &Events, &bWorkRan, &bCallbackWasSafe,
         &bCallbackWasUnattended, &bGuardWasActiveInCallback,
         &bResponderReportedDeferred, Capture]
        (const FString& RequestId, const FString& Action,
         const TSharedPtr<FJsonObject>& Payload) -> bool
        {
            Events.Add(TEXT("schedule"));
            FHandlerContext Ctx = FHandlerContext::MakeTestContextWithSharedCapture(
                RequestId, Action, Payload, Capture);
            Ctx.SetDispatcherForTesting(&Dispatcher);
            return PinWrightSafePoint::DeferRequestToSafePoint(
                Ctx, TEXT("asset.import test continuation"),
                [&Dispatcher, &Events, &bWorkRan, &bCallbackWasSafe,
                 &bCallbackWasUnattended, &bGuardWasActiveInCallback,
                 &bResponderReportedDeferred]
                (const PinWrightSafePoint::FSafePointResponder& Responder)
                {
                    Events.Add(TEXT("deferred.begin"));
                    bWorkRan = true;
                    bCallbackWasSafe = PinWrightSafePoint::IsSafeNow();
                    bCallbackWasUnattended = PinWrightAutomationMode::IsActive()
                        && GIsRunningUnattendedScript;
                    bGuardWasActiveInCallback =
                        Dispatcher.IsProcessingRequestForTesting();
                    bResponderReportedDeferred = Responder.IsDeferred();
                    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
                    Result->SetBoolField(TEXT("ok"), true);
                    Responder.SendSuccess(TEXT("deferred import probe complete"), Result);
                    Events.Add(TEXT("deferred.end"));
                });
        });

    TestTrue(TEXT("the automation caller starts at a safe point"),
        PinWrightSafePoint::IsSafeNow());
    Dispatcher.ProcessRequest(
        TEXT("import-id"), TEXT("test.asset_import_continuation"),
        MakeShared<FJsonObject>());

    TestFalse(TEXT("the import continuation never runs inline"), bWorkRan);
    TestFalse(TEXT("no async response is sent inline"), Capture->bWasCalled);
    TestTrue(TEXT("the active request remains retained while the callback is pending"),
        Dispatcher.IsProcessingRequestForTesting());

    Dispatcher.ProcessRequest(
        TEXT("queued-id"), TEXT("test.queued"), MakeShared<FJsonObject>());
    Dispatcher.ProcessPendingRequests();
    TestEqual(TEXT("a second request stays queued until the callback completes"),
        FString::Join(Events, TEXT(",")), FString(TEXT("schedule")));

    FTSTicker::GetCoreTicker().Tick(0.0f);

    TestTrue(TEXT("the deferred callback ran"), bWorkRan);
    TestTrue(TEXT("the callback observed a safe point"), bCallbackWasSafe);
    TestTrue(TEXT("the callback observed unattended mode"), bCallbackWasUnattended);
    TestTrue(TEXT("the dispatcher guard stayed active inside the callback"),
        bGuardWasActiveInCallback);
    TestTrue(TEXT("the responder reports the deferred path"),
        bResponderReportedDeferred);
    TestTrue(TEXT("the responder delivered a result"), Capture->bWasCalled);
    TestTrue(TEXT("the responder delivered success"), Capture->bSuccess);
    TestFalse(TEXT("the retained guard clears after callback completion"),
        Dispatcher.IsProcessingRequestForTesting());
    TestFalse(TEXT("the continuation leaves no unattended scope open"),
        PinWrightAutomationMode::IsActive());
    TestEqual(TEXT("callback completion precedes the queued request"),
        FString::Join(Events, TEXT(",")),
        FString(TEXT("schedule,deferred.begin,deferred.end,queued")));
    return true;
}
