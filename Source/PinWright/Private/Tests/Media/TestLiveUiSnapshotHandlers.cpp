// Copyright (c) 2026 Alexander Penkin. MIT License.

// Contract tests for live UI snapshot handler modes.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/UI/LiveUiSnapshot.h"
#include "LiveUiSnapshotTestHelpers.h"
#include "Tests/TestUtils.h"

namespace
{
    using LiveUiSnapshotTestHelpers::IsAcceptedLiveCaptureFailureCode;

    const FHandlerRegistration* FindHandlerRegistration(const FString& MethodName)
    {
        for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
        {
            if (Reg.MethodName == MethodName)
            {
                return &Reg;
            }
        }
        return nullptr;
    }

    bool HasOptionalParam(const FHandlerRegistration& Reg, const FString& ParamName)
    {
        for (const FParamSpec& Param : Reg.Params)
        {
            if (Param.Name == ParamName)
            {
                return !Param.bRequired;
            }
        }
        return false;
    }

    TSharedPtr<FJsonObject> MakeLivePayload()
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("capture_source"), TEXT("live"));
        return Payload;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveUiSnapshotDescribeRegisteredTest,
    "PinWright.widget.describe.Registered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLiveUiSnapshotDescribeRegisteredTest::RunTest(const FString& Parameters)
{
    const FHandlerRegistration* Reg = FindHandlerRegistration(TEXT("widget.describe"));
    TestTrue(TEXT("widget.describe is registered"), Reg != nullptr);
    if (!Reg)
    {
        return true;
    }

    TestEqual(TEXT("category is widget"), Reg->Category, TEXT("widget"));
    TestTrue(TEXT("capture_source is optional"), HasOptionalParam(*Reg, TEXT("capture_source")));
    TestTrue(TEXT("verbose is optional"), HasOptionalParam(*Reg, TEXT("verbose")));
    TestTrue(TEXT("include_geometry is optional"), HasOptionalParam(*Reg, TEXT("include_geometry")));
    // Multi-root disambiguation selectors (F-widget-describe-live-root-disambiguation).
    // The selector logic itself is covered by TestLiveUiSnapshotRootSelection.cpp against
    // FLiveUiSnapshotService::SelectRootCandidate directly; those tests stay green even if
    // the params are dropped from the registration, at which point the dispatcher rejects
    // any caller passing them with UNKNOWN_PARAMS and the fix is dead on the wire.
    TestTrue(TEXT("instance_name is optional"), HasOptionalParam(*Reg, TEXT("instance_name")));
    TestTrue(TEXT("instanceName alias is optional"), HasOptionalParam(*Reg, TEXT("instanceName")));
    TestTrue(TEXT("root_index is optional"), HasOptionalParam(*Reg, TEXT("root_index")));
    TestTrue(TEXT("rootIndex alias is optional"), HasOptionalParam(*Reg, TEXT("rootIndex")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveUiSnapshotExportXmlRegisteredTest,
    "PinWright.widget.export_xml.ParamContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLiveUiSnapshotExportXmlRegisteredTest::RunTest(const FString& Parameters)
{
    const FHandlerRegistration* Reg = FindHandlerRegistration(TEXT("widget.export_xml"));
    TestTrue(TEXT("widget.export_xml is registered"), Reg != nullptr);
    if (!Reg)
    {
        return true;
    }

    TestEqual(TEXT("category is widget"), Reg->Category, TEXT("widget"));
    TestTrue(TEXT("capture_source is optional"), HasOptionalParam(*Reg, TEXT("capture_source")));
    TestTrue(TEXT("verbose is optional"), HasOptionalParam(*Reg, TEXT("verbose")));
    TestTrue(TEXT("include_geometry is optional"), HasOptionalParam(*Reg, TEXT("include_geometry")));
    // widget.export_xml must accept the identical selector vocabulary as widget.describe;
    // see the note on the describe registration test above.
    TestTrue(TEXT("instance_name is optional"), HasOptionalParam(*Reg, TEXT("instance_name")));
    TestTrue(TEXT("instanceName alias is optional"), HasOptionalParam(*Reg, TEXT("instanceName")));
    TestTrue(TEXT("root_index is optional"), HasOptionalParam(*Reg, TEXT("root_index")));
    TestTrue(TEXT("rootIndex alias is optional"), HasOptionalParam(*Reg, TEXT("rootIndex")));
    return true;
}

// The wire->request half of F-widget-describe-live-root-disambiguation. The root-selection
// tests construct FLiveUiSnapshotRequest in C++ and set InstanceName/RootIndex by hand, so
// they cannot observe FLiveUiSnapshotRequest::FromContext (LiveUiSnapshot.cpp) at all:
// delete its two selector reads and every one of those tests still passes while no caller
// payload can ever reach the selector. This drives FromContext with real payloads instead.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveUiSnapshotRequestFromContextParsesSelectorsTest,
    "PinWright.widget.LiveSnapshot.Request.FromContextParsesSelectors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLiveUiSnapshotRequestFromContextParsesSelectorsTest::RunTest(const FString& Parameters)
{
    // snake_case canonical names.
    {
        TSharedPtr<FJsonObject> Payload = MakeLivePayload();
        Payload->SetStringField(TEXT("instance_name"), TEXT("WBP_PlayerHUD_StateTree_C_0"));
        Payload->SetNumberField(TEXT("root_index"), 2);
        Payload->SetBoolField(TEXT("verbose"), true);
        Payload->SetBoolField(TEXT("include_geometry"), true);
        const FHandlerContext Ctx = FHandlerContext::MakeTestContext(
            TEXT("test-id"), TEXT("widget.describe"), Payload);
        const FLiveUiSnapshotRequest Request = FLiveUiSnapshotRequest::FromContext(Ctx);

        TestEqual(TEXT("instance_name reaches the request"),
            Request.InstanceName, FString(TEXT("WBP_PlayerHUD_StateTree_C_0")));
        TestTrue(TEXT("root_index reaches the request"), Request.RootIndex.IsSet());
        TestEqual(TEXT("root_index value is carried verbatim"), Request.RootIndex.Get(-1), 2);
        TestTrue(TEXT("verbose reaches the request"), Request.bVerbose);
        TestTrue(TEXT("include_geometry reaches the request"), Request.bIncludeGeometry);
    }

    // camelCase aliases must resolve to the same fields.
    {
        TSharedPtr<FJsonObject> Payload = MakeLivePayload();
        Payload->SetStringField(TEXT("instanceName"), TEXT("StateTree"));
        Payload->SetNumberField(TEXT("rootIndex"), 1);
        const FHandlerContext Ctx = FHandlerContext::MakeTestContext(
            TEXT("test-id"), TEXT("widget.describe"), Payload);
        const FLiveUiSnapshotRequest Request = FLiveUiSnapshotRequest::FromContext(Ctx);

        TestEqual(TEXT("instanceName alias reaches the request"),
            Request.InstanceName, FString(TEXT("StateTree")));
        TestTrue(TEXT("rootIndex alias reaches the request"), Request.RootIndex.IsSet());
        TestEqual(TEXT("rootIndex alias value is carried verbatim"),
            Request.RootIndex.Get(-1), 1);
    }

    // Absent selectors must stay absent — an unset RootIndex is what makes the
    // no-selector multi-root case report AMBIGUOUS_LIVE_ROOT instead of silently
    // picking root 0. A default-to-0 regression would be invisible without this.
    {
        TSharedPtr<FJsonObject> Payload = MakeLivePayload();
        const FHandlerContext Ctx = FHandlerContext::MakeTestContext(
            TEXT("test-id"), TEXT("widget.describe"), Payload);
        const FLiveUiSnapshotRequest Request = FLiveUiSnapshotRequest::FromContext(Ctx);

        TestTrue(TEXT("absent instance_name leaves the selector empty"), Request.InstanceName.IsEmpty());
        TestFalse(TEXT("absent root_index leaves the optional unset (not 0)"), Request.RootIndex.IsSet());
    }

    // An explicit 0 must be distinguishable from absent.
    {
        TSharedPtr<FJsonObject> Payload = MakeLivePayload();
        Payload->SetNumberField(TEXT("root_index"), 0);
        const FHandlerContext Ctx = FHandlerContext::MakeTestContext(
            TEXT("test-id"), TEXT("widget.describe"), Payload);
        const FLiveUiSnapshotRequest Request = FLiveUiSnapshotRequest::FromContext(Ctx);

        TestTrue(TEXT("explicit root_index 0 is set"), Request.RootIndex.IsSet());
        TestEqual(TEXT("explicit root_index 0 is 0"), Request.RootIndex.Get(-1), 0);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveUiSnapshotDescribeInvalidCaptureSourceTest,
    "PinWright.widget.describe.InvalidCaptureSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLiveUiSnapshotDescribeInvalidCaptureSourceTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("capture_source"), TEXT("runtime"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.describe handler found"),
        InvokeHandlerWithCapture(TEXT("widget.describe"), Payload, Capture));
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    TestFalse(TEXT("invalid capture_source fails"), Capture.bSuccess);
    TestEqual(TEXT("error code is INVALID_PARAMETER"), Capture.ErrorCode, TEXT("INVALID_PARAMETER"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveUiSnapshotExportXmlInvalidCaptureSourceTest,
    "PinWright.widget.export_xml.InvalidCaptureSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLiveUiSnapshotExportXmlInvalidCaptureSourceTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("capture_source"), TEXT("runtime"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.export_xml handler found"),
        InvokeHandlerWithCapture(TEXT("widget.export_xml"), Payload, Capture));
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    TestFalse(TEXT("invalid capture_source fails"), Capture.bSuccess);
    TestEqual(TEXT("error code is INVALID_PARAMETER"), Capture.ErrorCode, TEXT("INVALID_PARAMETER"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveUiSnapshotDescribeRejectsAssetPathTest,
    "PinWright.widget.describe.RejectsAssetPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLiveUiSnapshotDescribeRejectsAssetPathTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeLivePayload();
    Payload->SetStringField(TEXT("asset_path"), TEXT("/Game/UI/WBP_Test"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.describe handler found"),
        InvokeHandlerWithCapture(TEXT("widget.describe"), Payload, Capture));
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    TestFalse(TEXT("live capture with asset path fails"), Capture.bSuccess);
    TestEqual(TEXT("error code is INVALID_PARAMETER"), Capture.ErrorCode, TEXT("INVALID_PARAMETER"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveUiSnapshotExportXmlRejectsAssetPathTest,
    "PinWright.widget.export_xml.RejectsAssetPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLiveUiSnapshotExportXmlRejectsAssetPathTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeLivePayload();
    Payload->SetStringField(TEXT("widgetPath"), TEXT("/Game/UI/WBP_Test"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.export_xml handler found"),
        InvokeHandlerWithCapture(TEXT("widget.export_xml"), Payload, Capture));
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    TestFalse(TEXT("live capture with widgetPath fails"), Capture.bSuccess);
    TestEqual(TEXT("error code is INVALID_PARAMETER"), Capture.ErrorCode, TEXT("INVALID_PARAMETER"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveUiSnapshotDescribeNoPieOrRootErrorTest,
    "PinWright.widget.describe.NoPieOrRootError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLiveUiSnapshotDescribeNoPieOrRootErrorTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeLivePayload();
    Payload->SetBoolField(TEXT("verbose"), true);
    Payload->SetBoolField(TEXT("include_geometry"), true);

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.describe handler found"),
        InvokeHandlerWithCapture(TEXT("widget.describe"), Payload, Capture));
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    TestFalse(TEXT("live capture without PIE/live root fails"), Capture.bSuccess);
    TestTrue(TEXT("error code matches a current live capture precondition failure"),
        IsAcceptedLiveCaptureFailureCode(Capture.ErrorCode));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLiveUiSnapshotExportXmlNoPieOrRootErrorTest,
    "PinWright.widget.export_xml.NoPieOrRootError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FLiveUiSnapshotExportXmlNoPieOrRootErrorTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeLivePayload();
    Payload->SetBoolField(TEXT("verbose"), true);
    Payload->SetBoolField(TEXT("include_geometry"), true);

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.export_xml handler found"),
        InvokeHandlerWithCapture(TEXT("widget.export_xml"), Payload, Capture));
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    TestFalse(TEXT("live capture without PIE/live root fails"), Capture.bSuccess);
    TestTrue(TEXT("error code matches a current live capture precondition failure"),
        IsAcceptedLiveCaptureFailureCode(Capture.ErrorCode));
    return true;
}
