// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for EditorLaunchHandler.cpp (editor.launch_standalone).
//
// The handler body itself spawns OS processes via FPlatformProcess::CreateProc,
// so we don't exercise the full handler here. Instead we link directly to the
// pure command-line builder declared in EditorLaunchHandlerInternal.h — the
// only piece with non-trivial branching — and verify registration separately.
#include "Misc/AutomationTest.h"
#include "Handlers/Editor/EditorLaunchHandlerInternal.h"
#include "Tests/TestUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorLaunchBuildCmdLineBasicTest,
    "PinWright.editor.launch_standalone.BuildCmdLineBasic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorLaunchBuildCmdLineBasicTest::RunTest(const FString& Parameters)
{
    const FString Cmd = BuildStandaloneCommandLine(
        TEXT("C:\\Foo.uproject"), TEXT("/Game/Maps/M_Test"),
        /*InstanceIndex=*/0, /*NumClients=*/1, /*bListenServer=*/false, TEXT(""));

    TestTrue(TEXT("cmdline contains project file"), Cmd.Contains(TEXT("Foo.uproject")));
    TestTrue(TEXT("cmdline contains map path"), Cmd.Contains(TEXT("/Game/Maps/M_Test")));
    TestTrue(TEXT("cmdline contains -game"), Cmd.Contains(TEXT("-game")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorLaunchBuildCmdLineListenServerTest,
    "PinWright.editor.launch_standalone.BuildCmdLineListenServer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorLaunchBuildCmdLineListenServerTest::RunTest(const FString& Parameters)
{
    // Instance 0 with listenServer=true must carry the ?listen suffix.
    const FString Server = BuildStandaloneCommandLine(
        TEXT("C:\\Foo.uproject"), TEXT("/Game/Maps/M_Test"),
        /*InstanceIndex=*/0, /*NumClients=*/2, /*bListenServer=*/true, TEXT(""));
    TestTrue(TEXT("instance 0 cmdline contains map name"), Server.Contains(TEXT("M_Test")));
    TestTrue(TEXT("instance 0 cmdline contains ?listen"), Server.Contains(TEXT("?listen")));

    // Instance 1 connects to the listen server on loopback and must NOT carry ?listen.
    const FString Client = BuildStandaloneCommandLine(
        TEXT("C:\\Foo.uproject"), TEXT("/Game/Maps/M_Test"),
        /*InstanceIndex=*/1, /*NumClients=*/2, /*bListenServer=*/true, TEXT(""));
    TestTrue(TEXT("instance 1 cmdline points at 127.0.0.1"), Client.Contains(TEXT("127.0.0.1")));
    TestFalse(TEXT("instance 1 cmdline omits ?listen"), Client.Contains(TEXT("?listen")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorLaunchStandaloneRegisteredTest,
    "PinWright.editor.launch_standalone.Registered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorLaunchStandaloneRegisteredTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("editor.launch_standalone is registered"),
        IsHandlerRegistered(TEXT("editor.launch_standalone")));
    return true;
}
