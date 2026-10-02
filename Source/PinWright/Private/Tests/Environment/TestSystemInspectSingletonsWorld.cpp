// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for board ticket B-inspect-singletons-pick-client-world.
//
// system.inspect.get_game_state / get_player_states (and their four singleton siblings) resolved
// the world through GEditor->PlayWorld, which is the LAST PIE world created: in a listen-server
// session with one client that is the client, so the verbs answered with the client's replicated
// copies and nothing in the response said so. They now take editor.console_command's `world`
// selector, default to the PIE authority, and echo the instance that answered.
//
// Counterfactual: before the fix the ListenSessionClientCreatedLast case had no helper to call
// (the old default picked the client), the handlers had no `world` parameter (an unknown selector
// was accepted and ignored) and no response carried world / worldPath / netMode.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Editor/PieWorldSelector.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

namespace SystemInspectSingletonsWorldTestUtils
{
    PieWorldSelector::FPieContextInfo MakeContext(int32 PieInstance, ENetMode NetMode)
    {
        PieWorldSelector::FPieContextInfo Context;
        Context.PieInstance = PieInstance;
        Context.NetMode = NetMode;
        return Context;
    }
}

// Pure: the omitted-world pick for reads is the authority wherever it sits in gather order.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemInspectSingletonsOmittedWorldPrefersAuthorityTest,
    "PinWright.system.inspect.singletons.OmittedWorldPrefersPieAuthority",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemInspectSingletonsOmittedWorldPrefersAuthorityTest::RunTest(const FString& Parameters)
{
    using namespace PieWorldSelector;
    using SystemInspectSingletonsWorldTestUtils::MakeContext;

    TestEqual(TEXT("no PIE -> editor world"), ResolveOmittedForRead({}), (int32)INDEX_NONE);

    TestEqual(TEXT("sole standalone PIE world answers"),
        ResolveOmittedForRead({ MakeContext(0, NM_Standalone) }), 0);

    TestEqual(TEXT("listen session, server first -> server"),
        ResolveOmittedForRead({ MakeContext(0, NM_ListenServer), MakeContext(1, NM_Client) }), 0);

    // The ticket's shape: the client is the newest world (what GEditor->PlayWorld pointed at).
    TestEqual(TEXT("listen session, client gathered first -> still the server"),
        ResolveOmittedForRead({ MakeContext(1, NM_Client), MakeContext(0, NM_ListenServer) }), 1);

    TestEqual(TEXT("dedicated server + two clients -> the dedicated server"),
        ResolveOmittedForRead({ MakeContext(1, NM_Client), MakeContext(2, NM_Client),
            MakeContext(0, NM_DedicatedServer) }), 2);

    TestEqual(TEXT("clients only (server out of process) -> first PIE world"),
        ResolveOmittedForRead({ MakeContext(1, NM_Client), MakeContext(2, NM_Client) }), 0);
    return true;
}

// Handler: the selector is validated and the answering world is echoed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemInspectGetGameStateWorldSelectorTest,
    "PinWright.system.inspect.get_game_state.WorldSelectorValidatedAndEchoed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemInspectGetGameStateWorldSelectorTest::RunTest(const FString& Parameters)
{
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("world"), TEXT("serverr"));
        FTestResponseCapture Capture;
        TestTrue(TEXT("get_game_state registered"),
            InvokeHandlerWithCapture(TEXT("system.inspect.get_game_state"), Payload, Capture));
        TestFalse(TEXT("unknown selector refused"), Capture.bSuccess);
        TestEqual(TEXT("unknown selector -> INVALID_ARGUMENT"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    }

    if (PieWorldSelector::GatherPieContexts().Num() > 0)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("pie-running"),
            TEXT("a PIE session is running; the no-PIE selector and default-world assertions need the editor world only"));
        return true;
    }
    UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!TestNotNull(TEXT("editor world"), EditorWorld))
    {
        return false;
    }

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("world"), TEXT("server"));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("system.inspect.get_game_state"), Payload, Capture);
        TestFalse(TEXT("'server' without PIE refused"), Capture.bSuccess);
        TestEqual(TEXT("'server' without PIE -> WORLD_NOT_FOUND"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_WORLD_NOT_FOUND));
    }

    // get_player_states succeeds on the editor world (empty array), so the echo is observable
    // without PIE: omitted world -> editor, defaulted, with path and net mode.
    {
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("system.inspect.get_player_states"), MakeShared<FJsonObject>(), Capture);
        TestTrue(TEXT("get_player_states succeeded"), Capture.bSuccess);
        if (!TestTrue(TEXT("result present"), Capture.Result.IsValid()))
        {
            return false;
        }
        FString World;
        FString WorldPath;
        FString NetMode;
        bool bDefaulted = false;
        TestTrue(TEXT("echoes world"), Capture.Result->TryGetStringField(TEXT("world"), World));
        TestEqual(TEXT("omitted world without PIE is the editor world"), World, FString(TEXT("editor")));
        TestTrue(TEXT("echoes worldDefaulted"), Capture.Result->TryGetBoolField(TEXT("worldDefaulted"), bDefaulted));
        TestTrue(TEXT("worldDefaulted is true"), bDefaulted);
        TestTrue(TEXT("echoes worldPath"), Capture.Result->TryGetStringField(TEXT("worldPath"), WorldPath));
        TestEqual(TEXT("worldPath is the editor world"), WorldPath, EditorWorld->GetPathName());
        TestTrue(TEXT("echoes netMode"), Capture.Result->TryGetStringField(TEXT("netMode"), NetMode));
        TestFalse(TEXT("no pieInstance for the editor world"), Capture.Result->HasField(TEXT("pieInstance")));
        TestTrue(TEXT("still carries playerStates"), Capture.Result->HasTypedField<EJson::Array>(TEXT("playerStates")));
    }

    // Explicit "editor" is echoed as given and not marked defaulted.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("world"), TEXT("editor"));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("system.inspect.get_player_states"), Payload, Capture);
        TestTrue(TEXT("explicit editor succeeded"), Capture.bSuccess);
        bool bDefaulted = true;
        TestTrue(TEXT("explicit editor echoes worldDefaulted"),
            Capture.Result.IsValid() && Capture.Result->TryGetBoolField(TEXT("worldDefaulted"), bDefaulted));
        TestFalse(TEXT("explicit editor is not defaulted"), bDefaulted);
    }
    return true;
}
