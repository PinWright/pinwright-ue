// Copyright (c) 2026 Alexander Penkin. MIT License.

// widget.set's `slot` through the REAL dispatcher (board B-widget-set-slot-declared-integer).
// `slot` was declared `integer` while the handler reads it with GetObjectField, so the declared-type
// gate refused every slot object before the body ran, and the one shape it admitted ("5") reached a
// body that ignored it and answered {"success": true, "propertiesSet": 0}. The widget tests beside
// this file use InvokeHandlerWithCapture, which skips ValidateHandlerParams, so they never saw it.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"
#include "Tests/Widget/WidgetTestFixtures.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Infra/ParamSpecTestHelpers.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/ErrorCodes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "WidgetBlueprint.h"

namespace WidgetSetSlotDispatchTests
{
    // Dispatch and drain any safe-point deferral (a bare dispatcher has no ticker), as
    // TestParamTypeGate.cpp's DispatchAndSettle does.
    DispatcherTestHelpers::FSinkCapture DispatchAndSettle(FRpcDispatcher& Dispatcher,
        DispatcherTestHelpers::FSinkPtr& Sink, const FString& Method, const FString& RequestId,
        const TSharedPtr<FJsonObject>& Payload)
    {
        bool bSuccess = false;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, Method, RequestId, Payload, bSuccess, ErrorCode);
        if (!Sink->bWasCalled)
        {
            Dispatcher.ProcessPendingRequests();
        }
        return *Sink;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetSetSlotThroughDispatcherTest,
    "PinWright.widget.set.SlotObjectReachesHandlerThroughDispatcher",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetSetSlotThroughDispatcherTest::RunTest(const FString& Parameters)
{
    // Gate refusals log at Warning, which automation elevates to errors by default.
    bSuppressLogWarnings = true;

    const FString WidgetPath = WidgetTestFixtures::MakeWidgetAssetPath(TEXT("WBP_WidgetSetSlotDispatch"));
    UWidgetBlueprint* WBP = WidgetTestFixtures::MakeTransientWidgetBlueprint(WidgetPath);
    if (!TestNotNull(TEXT("widget blueprint allocated"), WBP) || !WBP->WidgetTree)
    {
        return false;
    }
    ON_SCOPE_EXIT
    {
        if (UPackage* Package = WBP->GetOutermost())
        {
            Package->SetDirtyFlag(false);
        }
        CleanupTestAsset(WidgetPath);
    };

    UCanvasPanel* Root = WidgetTestFixtures::AddCanvasRoot(WBP);
    UTextBlock* Label = WidgetTestFixtures::AddTextBlockToPanel(WBP, Root, TEXT("SlotLabel"));
    UCanvasPanelSlot* CanvasSlot = Label ? Cast<UCanvasPanelSlot>(Label->Slot) : nullptr;
    if (!TestNotNull(TEXT("label sits in a CanvasPanelSlot"), CanvasSlot))
    {
        return false;
    }
    CanvasSlot->SetAutoSize(false);

    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::FSinkPtr Sink;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    auto MakePayload = [&WidgetPath]()
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("widgetPath"), WidgetPath);
        Payload->SetStringField(TEXT("widgetName"), TEXT("SlotLabel"));
        return Payload;
    };

    // 1. A slot object reaches the body and writes. Before the fix: PARAM_TYPE_MISMATCH.
    TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();
    SlotObj->SetBoolField(TEXT("bAutoSize"), true);
    TSharedPtr<FJsonObject> ObjectPayload = MakePayload();
    ObjectPayload->SetObjectField(TEXT("slot"), SlotObj);

    const DispatcherTestHelpers::FSinkCapture ObjectOut = WidgetSetSlotDispatchTests::DispatchAndSettle(
        Dispatcher, Sink, TEXT("widget.set"), TEXT("widget-set-slot-object"), ObjectPayload);
    TestTrue(*FString::Printf(TEXT("slot object succeeds (code: %s, message: %s)"),
        *ObjectOut.ErrorCode, *ObjectOut.Message), ObjectOut.bSuccess);
    double PropertiesSet = -1.0;
    TestTrue(TEXT("result carries propertiesSet"),
        ObjectOut.Result.IsValid() && ObjectOut.Result->TryGetNumberField(TEXT("propertiesSet"), PropertiesSet));
    TestEqual(TEXT("one slot property set"), PropertiesSet, 1.0);
    TestTrue(TEXT("CanvasPanelSlot.bAutoSize written"), CanvasSlot->GetAutoSize());

    // 2. The scalar the old `integer` declaration admitted is now refused at the gate.
    // Before the fix: {"success": true, "propertiesSet": 0} with nothing written.
    CanvasSlot->SetAutoSize(false);
    TSharedPtr<FJsonObject> ScalarPayload = MakePayload();
    ScalarPayload->SetStringField(TEXT("slot"), TEXT("5"));

    const DispatcherTestHelpers::FSinkCapture ScalarOut = WidgetSetSlotDispatchTests::DispatchAndSettle(
        Dispatcher, Sink, TEXT("widget.set"), TEXT("widget-set-slot-scalar"), ScalarPayload);
    TestFalse(TEXT("slot \"5\" is not a success"), ScalarOut.bSuccess);
    TestEqual(*FString::Printf(TEXT("slot \"5\" refused by the gate (message: %s)"), *ScalarOut.Message),
        ScalarOut.ErrorCode, FString(ErrorCodes::ERR_PARAM_TYPE_MISMATCH));

    // 3. `object` also admits an array (ParamTypeCheck.h), which GetObjectField drops. The body must
    // refuse it rather than answer success with nothing written.
    for (const TCHAR* Field : {TEXT("slot"), TEXT("properties")})
    {
        TSharedPtr<FJsonObject> ArrayPayload = MakePayload();
        ArrayPayload->SetArrayField(Field, TArray<TSharedPtr<FJsonValue>>());

        const DispatcherTestHelpers::FSinkCapture ArrayOut = WidgetSetSlotDispatchTests::DispatchAndSettle(
            Dispatcher, Sink, TEXT("widget.set"), FString::Printf(TEXT("widget-set-%s-array"), Field),
            ArrayPayload);
        TestFalse(*FString::Printf(TEXT("%s: [] is not a success"), Field), ArrayOut.bSuccess);
        TestEqual(*FString::Printf(TEXT("%s: [] refused by the body (message: %s)"), Field, *ArrayOut.Message),
            ArrayOut.ErrorCode, FString(ErrorCodes::ERR_INVALID_PARAMETER));
    }
    TestFalse(TEXT("no refused call wrote bAutoSize"), CanvasSlot->GetAutoSize());

    return true;
}

// The same discrete-token sweep declared three boolean `*slot*` flags `integer`; the integer gate
// refuses JSON booleans, so `true` was unsendable for each.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetSlotNamedParamDeclarationsTest,
    "PinWright.widget.SlotNamedParamsDeclaredByShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetSlotNamedParamDeclarationsTest::RunTest(const FString& Parameters)
{
    struct FExpected { const TCHAR* Method; const TCHAR* Param; const TCHAR* Type; };
    const FExpected Expected[] = {
        {TEXT("widget.set"), TEXT("slot"), TEXT("object")},
        {TEXT("widget.set"), TEXT("properties"), TEXT("object")},
        {TEXT("widget.duplicate"), TEXT("copySlot"), TEXT("boolean")},
        {TEXT("widget.export_xml"), TEXT("omit_slot_chain"), TEXT("boolean")},
        {TEXT("widget.describe"), TEXT("include_slot"), TEXT("boolean")},
    };
    for (const FExpected& E : Expected)
    {
        const FParamSpec* Spec = ParamSpecTestHelpers::FindParamSpec(E.Method, E.Param);
        if (TestNotNull(*FString::Printf(TEXT("%s.%s is declared"), E.Method, E.Param), Spec))
        {
            TestEqual(*FString::Printf(TEXT("%s.%s declared type"), E.Method, E.Param),
                Spec->Type, FString(E.Type));
        }
    }
    return true;
}
