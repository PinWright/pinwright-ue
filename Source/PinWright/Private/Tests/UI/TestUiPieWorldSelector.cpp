// Copyright (c) 2026 Alexander Penkin. MIT License.

// F-ui-runtime-verbs-pie-instance-selector: the ui.* runtime verbs accept editor.console_command's
// `world` selector, resolved by PieWorldSelector::ResolveGameWorld. Multi-client PIE cannot be
// started in a headless suite, so the resolver contract is tested over synthetic contexts and the
// verbs are tested for routing the selector into it (no PIE running).

#include "Misc/AutomationTest.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Handlers/Editor/PieWorldSelector.h"
#include "Handlers/ErrorCodes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiPieWorldResolveGameWorldContractTest,
    "PinWright.ui.pie_world.ResolveGameWorldContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUiPieWorldResolveGameWorldContractTest::RunTest(const FString& Parameters)
{
    using namespace PieWorldSelector;

    FPieContextInfo Server;
    Server.PieInstance = 0;
    Server.NetMode = NM_ListenServer;
    FPieContextInfo Client;
    Client.PieInstance = 1;
    Client.NetMode = NM_Client;
    const TArray<FPieContextInfo> ListenPie = {Server, Client};

    auto Resolve = [](const TCHAR* Selector, const TArray<FPieContextInfo>& Contexts, int32& OutIndex, FString& OutCode)
    {
        FString Message;
        return ResolveGameWorld(Selector, Contexts, OutIndex, OutCode, Message);
    };

    int32 Index = 42;
    FString Code;
    TestTrue(TEXT("omitted, no PIE: resolves with no PIE world"), Resolve(TEXT(""), {}, Index, Code));
    TestEqual(TEXT("omitted, no PIE: index is INDEX_NONE"), Index, (int32)INDEX_NONE);

    TestTrue(TEXT("omitted, one PIE world: resolves"), Resolve(TEXT(" "), {Client}, Index, Code));
    TestEqual(TEXT("omitted, one PIE world: that world"), Index, 0);

    TestFalse(TEXT("omitted, listen server + client: refused"), Resolve(TEXT(""), ListenPie, Index, Code));
    TestEqual(TEXT("omitted, several PIE worlds: TARGET_AMBIGUOUS"), Code, FString(ErrorCodes::ERR_TARGET_AMBIGUOUS));

    TestTrue(TEXT("'client:1' resolves"), Resolve(TEXT("client:1"), ListenPie, Index, Code));
    TestEqual(TEXT("'client:1' is the client context"), Index, 1);
    TestTrue(TEXT("'server' resolves"), Resolve(TEXT("server"), ListenPie, Index, Code));
    TestEqual(TEXT("'server' is the listen server context"), Index, 0);
    TestTrue(TEXT("'pie:1' resolves"), Resolve(TEXT("pie:1"), ListenPie, Index, Code));
    TestEqual(TEXT("'pie:1' is PIEInstance 1"), Index, 1);

    TestFalse(TEXT("'client:2' with one client: refused"), Resolve(TEXT("client:2"), ListenPie, Index, Code));
    TestEqual(TEXT("'client:2' with one client: WORLD_NOT_FOUND"), Code, FString(ErrorCodes::ERR_WORLD_NOT_FOUND));
    TestFalse(TEXT("explicit selector, no PIE: refused"), Resolve(TEXT("server"), {}, Index, Code));
    TestEqual(TEXT("explicit selector, no PIE: WORLD_NOT_FOUND"), Code, FString(ErrorCodes::ERR_WORLD_NOT_FOUND));

    TestFalse(TEXT("'editor' is not a game world: refused"), Resolve(TEXT("editor"), ListenPie, Index, Code));
    TestEqual(TEXT("'editor': INVALID_ARGUMENT"), Code, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    TestFalse(TEXT("malformed selector: refused"), Resolve(TEXT("client:0"), ListenPie, Index, Code));
    TestEqual(TEXT("malformed selector: INVALID_ARGUMENT"), Code, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiPieWorldRuntimeVerbsRouteSelectorTest,
    "PinWright.ui.pie_world.RuntimeVerbsRouteSelector",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUiPieWorldRuntimeVerbsRouteSelectorTest::RunTest(const FString& Parameters)
{
    if (!GEditor || GEditor->PlayWorld || GEditor->IsPlaySessionInProgress())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("preexisting_pie_session"),
            TEXT("The no-PIE routing assertions need an editor with no PIE session running."));
        return true;
    }

    // Each payload passes the verb's own checks that run before world resolution (class path,
    // texture), so the response code comes from the selector. Before the fix the verbs ignored
    // `world` and answered NO_VIEWPORT / WIDGET_NOT_FOUND instead.
    auto MakePayload = [](const TCHAR* Method, const TCHAR* World)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        const FString Name(Method);
        if (Name == TEXT("ui.create_hud"))
        {
            Payload->SetStringField(TEXT("widgetPath"), TEXT("/Script/UMG.UserWidget"));
        }
        else if (Name == TEXT("ui.set_widget_image"))
        {
            Payload->SetStringField(TEXT("key"), TEXT("PW_PieWorldProbe"));
            Payload->SetStringField(TEXT("texturePath"), TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
        }
        else
        {
            Payload->SetStringField(TEXT("key"), TEXT("PW_PieWorldProbe"));
            Payload->SetStringField(TEXT("value"), TEXT("probe"));
        }
        Payload->SetStringField(TEXT("world"), World);
        return Payload;
    };

    const TCHAR* Methods[] = {
        TEXT("ui.create_hud"), TEXT("ui.set_widget_text"), TEXT("ui.set_widget_image"),
        TEXT("ui.set_widget_visibility"), TEXT("ui.remove_widget_from_viewport")};
    for (const TCHAR* Method : Methods)
    {
        FTestResponseCapture Capture;
        TestTrue(FString::Printf(TEXT("%s handler registered"), Method),
            InvokeHandlerWithCapture(Method, MakePayload(Method, TEXT("client:1")), Capture));
        TestFalse(FString::Printf(TEXT("%s world:'client:1' without PIE does not succeed"), Method), Capture.bSuccess);
        TestEqual(FString::Printf(TEXT("%s world:'client:1' without PIE: WORLD_NOT_FOUND"), Method),
            Capture.ErrorCode, FString(ErrorCodes::ERR_WORLD_NOT_FOUND));

        FTestResponseCapture BadCapture;
        InvokeHandlerWithCapture(Method, MakePayload(Method, TEXT("pie:x")), BadCapture);
        TestEqual(FString::Printf(TEXT("%s malformed world: INVALID_ARGUMENT"), Method),
            BadCapture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    }
    return true;
}
