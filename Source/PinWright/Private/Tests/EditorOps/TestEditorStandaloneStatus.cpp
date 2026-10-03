// Copyright (c) 2026 Alexander Penkin. MIT License.

// editor.standalone_status and the -abslog / tracking half of editor.launch_standalone.
//
// A real -game launch is too heavy for the suite, so the tracked process here is a shell child
// registered through the same TrackStandaloneProcess launch_standalone calls; the verb cannot
// tell the difference. A live end-to-end check (launch_standalone, then standalone_status with
// capture:true on the game window) needs a cooked-or-editor-runnable map and an X display, and
// is left to a manual run.

#include "Misc/AutomationTest.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Editor/EditorLaunchHandlerInternal.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformOutputDevices.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#if PLATFORM_LINUX
#include <dlfcn.h>
#endif

namespace TestEditorStandaloneStatusHelpers
{
    FProcHandle SpawnShell(const TCHAR* Script, uint32& OutPid)
    {
#if PLATFORM_WINDOWS
        return FPlatformProcess::CreateProc(TEXT("cmd.exe"), *FString::Printf(TEXT("/c %s"), Script),
            true, true, true, &OutPid, 0, nullptr, nullptr);
#else
        return FPlatformProcess::CreateProc(TEXT("/bin/sh"), *FString::Printf(TEXT("-c \"%s\""), Script),
            true, true, true, &OutPid, 0, nullptr, nullptr);
#endif
    }

    TSharedPtr<FJsonObject> Status(uint32 Pid, FTestResponseCapture& Capture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetNumberField(TEXT("pid"), Pid);
        Payload->SetNumberField(TEXT("tailLines"), 2);
        InvokeHandlerWithCapture(TEXT("editor.standalone_status"), Payload, Capture);
        return Capture.Result;
    }

    TSharedPtr<FJsonObject> StatusOnceExited(uint32 Pid, FTestResponseCapture& Capture)
    {
        const double Deadline = FPlatformTime::Seconds() + 10.0;
        TSharedPtr<FJsonObject> Result = Status(Pid, Capture);
        while (Result && Result->GetBoolField(TEXT("running")) && FPlatformTime::Seconds() < Deadline)
        {
            FPlatformProcess::Sleep(0.05f);
            Result = Status(Pid, Capture);
        }
        return Result;
    }

    FString WriteLog(const FString& Text)
    {
        const FString Path = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("pw-standalone-"), TEXT(".log"));
        FFileHelper::SaveStringToFile(Text, *Path);
        return Path;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorLaunchStandaloneAbsLogArgTest,
    "PinWright.editor.launch_standalone.AbsLogArgAppendedOrHonoured",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorLaunchStandaloneAbsLogArgTest::RunTest(const FString& Parameters)
{
    FString Cmd = TEXT("\"P.uproject\" /Game/M -game");
    const FString Log = AppendStandaloneLogArg(Cmd, TEXT(""), TEXT("/tmp/pw/a b.log"));
    TestEqual(TEXT("without a caller -abslog the default log is used"), Log, FString(TEXT("/tmp/pw/a b.log")));
    TestTrue(TEXT("... and passed to the process, quoted"), Cmd.EndsWith(TEXT(" -abslog=\"/tmp/pw/a b.log\"")));

    FString CallerCmd = TEXT("\"P.uproject\" /Game/M -game -abslog=\"/x/mine.log\"");
    const FString Before = CallerCmd;
    const FString CallerLog = AppendStandaloneLogArg(CallerCmd, TEXT("-abslog=\"/x/mine.log\""), TEXT("/tmp/pw/default.log"));
    TestEqual(TEXT("a caller -abslog is the log reported"), CallerLog, FString(TEXT("/x/mine.log")));
    TestEqual(TEXT("... and no second -abslog is appended"), CallerCmd, Before);

    // The engine matches ABSLOG= without the dash and reads LOG= / LogFileName= first.
    FString Cmd2 = TEXT("x");
    TestEqual(TEXT("a dashless ABSLOG= is the caller's log too"),
        AppendStandaloneLogArg(Cmd2, TEXT("ABSLOG=/x/bare.log"), TEXT("/tmp/pw/default.log")), FString(TEXT("/x/bare.log")));
    TestEqual(TEXT("... nothing appended"), Cmd2, FString(TEXT("x")));
    FString Cmd3 = TEXT("x");
    TestEqual(TEXT("-log= wins over -abslog= and resolves under the project log dir"),
        AppendStandaloneLogArg(Cmd3, TEXT("-abslog=/x/abs.log -log=rel.log"), TEXT("/tmp/pw/default.log")),
        FPaths::ConvertRelativePathToFull(FPaths::ProjectLogDir() / TEXT("rel.log")));
    TestEqual(TEXT("... nothing appended"), Cmd3, FString(TEXT("x")));
    FString Cmd4 = TEXT("x");
    TestEqual(TEXT("a log name without .log/.txt falls back to <Project>.log, as the engine does"),
        AppendStandaloneLogArg(Cmd4, TEXT("-LogFileName=noext"), TEXT("/tmp/pw/default.log")),
        FPaths::ConvertRelativePathToFull(FPaths::ProjectLogDir() / FString(FApp::GetProjectName()) + TEXT(".log")));
    FString Cmd5 = TEXT("x");
    TestFalse(TEXT("a relative ABSLOG= is reported as an absolute path"),
        FPaths::IsRelative(AppendStandaloneLogArg(Cmd5, TEXT("ABSLOG=rel.log"), TEXT("/tmp/pw/default.log"))));

    // The engine locks its log and a process that finds it held writes <name>_2.log, so a caller log
    // shared by several slots, or resolving to the editor's own log, is refused rather than misreported.
    TestTrue(TEXT("no caller log: never refused, even with numClients 8"), StandaloneCallerLogRefusal(TEXT("-windowed"), 8).IsEmpty());
    TestTrue(TEXT("a caller log for one slot is accepted"), StandaloneCallerLogRefusal(TEXT("-abslog=/x/mine.log"), 1).IsEmpty());
    TestFalse(TEXT("a caller log shared by numClients 2 is refused"), StandaloneCallerLogRefusal(TEXT("-abslog=/x/mine.log"), 2).IsEmpty());
    const FString EditorLog = FPaths::ConvertRelativePathToFull(FPlatformOutputDevices::GetAbsoluteLogFilename());
    TestFalse(TEXT("a caller log resolving to the editor's own log is refused"),
        StandaloneCallerLogRefusal(FString::Printf(TEXT("-abslog=\"%s\""), *EditorLog), 1).IsEmpty());
    const bool bEditorLogIsDefault = FPaths::IsSamePath(EditorLog,
        FPaths::ConvertRelativePathToFull(FPaths::ProjectLogDir() / FString(FApp::GetProjectName()) + TEXT(".log")));
    TestEqual(TEXT("a name without .log/.txt resolves to <Project>.log: refused exactly when that is the editor's own log"),
        !StandaloneCallerLogRefusal(TEXT("-LogFileName=noext"), 1).IsEmpty(), bEditorLogIsDefault);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorStandaloneStatusExitCodeTest,
    "PinWright.editor.standalone_status.ReportsExitCodeAndLogTail",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorStandaloneStatusExitCodeTest::RunTest(const FString& Parameters)
{
    namespace Helpers = TestEditorStandaloneStatusHelpers;
    const FString Log = Helpers::WriteLog(TEXT("first\nsecond\nthird\n"));
    uint32 Pid = 0;
    FProcHandle Handle = Helpers::SpawnShell(TEXT("exit 7"), Pid);
    if (!TestTrue(TEXT("fixture: the shell child spawned"), Handle.IsValid() && Pid != 0))
    {
        return false;
    }
    TrackStandaloneProcess(Pid, Handle, TEXT("fixture"), Log);

    FTestResponseCapture Capture;
    const TSharedPtr<FJsonObject> Result = Helpers::StatusOnceExited(Pid, Capture);
    IFileManager::Get().Delete(*Log);
    if (!TestTrue(TEXT("status succeeds for a tracked pid"), Capture.bSuccess && Result.IsValid()))
    {
        return false;
    }
    TestFalse(TEXT("the exited child reads running:false"), Result->GetBoolField(TEXT("running")));
    double ExitCode = -1;
    TestTrue(TEXT("exitCode is reported"), Result->TryGetNumberField(TEXT("exitCode"), ExitCode));
    TestEqual(TEXT("... and is the child's real status"), static_cast<int32>(ExitCode), 7);
    TestTrue(TEXT("the log exists"), Result->GetBoolField(TEXT("logExists")));
    const TArray<TSharedPtr<FJsonValue>>& Tail = Result->GetArrayField(TEXT("logTail"));
    if (TestEqual(TEXT("tailLines:2 returns two lines"), Tail.Num(), 2))
    {
        TestEqual(TEXT("tail[0]"), Tail[0]->AsString(), FString(TEXT("second")));
        TestEqual(TEXT("tail[1]"), Tail[1]->AsString(), FString(TEXT("third")));
    }
    FTestResponseCapture WithCapture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("pid"), Pid);
    Payload->SetBoolField(TEXT("capture"), true);
    InvokeHandlerWithCapture(TEXT("editor.standalone_status"), Payload, WithCapture);
    const TSharedPtr<FJsonObject>* CaptureField = nullptr;
    if (TestTrue(TEXT("capture:true on an exited process still answers, with a capture block"),
            WithCapture.bSuccess && WithCapture.Result->TryGetObjectField(TEXT("capture"), CaptureField)))
    {
        TestFalse(TEXT("... that did not capture a window by a pid that may be reused"), (*CaptureField)->GetBoolField(TEXT("captured")));
        TestEqual(TEXT("... and says why"), (*CaptureField)->GetStringField(TEXT("errorCode")), FString(ErrorCodes::ERR_WINDOW_NOT_FOUND));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorStandaloneStatusRunningTest,
    "PinWright.editor.standalone_status.RunningProcessHasNoExitCode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorStandaloneStatusRunningTest::RunTest(const FString& Parameters)
{
    namespace Helpers = TestEditorStandaloneStatusHelpers;
    uint32 Pid = 0;
#if PLATFORM_WINDOWS
    FProcHandle Handle = Helpers::SpawnShell(TEXT("ping -n 30 127.0.0.1 >nul"), Pid);
#else
    FProcHandle Handle = Helpers::SpawnShell(TEXT("exec sleep 30"), Pid);
#endif
    if (!TestTrue(TEXT("fixture: the long-running child spawned"), Handle.IsValid() && Pid != 0))
    {
        return false;
    }
    FProcHandle KillHandle = Handle;  // shares the process state with the registry's copy
    TrackStandaloneProcess(Pid, Handle, TEXT("fixture"), FPaths::ProjectIntermediateDir() / TEXT("pw-standalone-no-such.log"));

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Result = Helpers::Status(Pid, Capture);
    if (!TestTrue(TEXT("status succeeds"), Capture.bSuccess && Result.IsValid()))
    {
        FPlatformProcess::TerminateProc(KillHandle, /*KillTree*/ true);
        return false;
    }
    TestTrue(TEXT("a live child reads running:true"), Result->GetBoolField(TEXT("running")));
    TestFalse(TEXT("... with no exitCode"), Result->HasField(TEXT("exitCode")));
    TestFalse(TEXT("a missing log reads logExists:false"), Result->GetBoolField(TEXT("logExists")));
    TestEqual(TEXT("... with an empty tail"), Result->GetArrayField(TEXT("logTail")).Num(), 0);

    FPlatformProcess::TerminateProc(KillHandle, /*KillTree*/ true);
    Result = Helpers::StatusOnceExited(Pid, Capture);
    if (TestTrue(TEXT("the terminated child reads running:false"), Result.IsValid() && !Result->GetBoolField(TEXT("running"))))
    {
#if PLATFORM_LINUX
        TestFalse(TEXT("a signal death reports no exit code"), Result->HasField(TEXT("exitCode")));
        TestTrue(TEXT("... and says so"), Result->HasField(TEXT("exitCodeUnavailable")));
#endif
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorStandaloneStatusUntrackedTest,
    "PinWright.editor.standalone_status.UntrackedPidIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorStandaloneStatusUntrackedTest::RunTest(const FString& Parameters)
{
    // The editor's own pid is alive and certainly not a launch_standalone child.
    FTestResponseCapture Capture;
    TestEditorStandaloneStatusHelpers::Status(FPlatformProcess::GetCurrentProcessId(), Capture);
    TestFalse(TEXT("an untracked pid is not answered as if it were observable"), Capture.bSuccess);
    TestEqual(TEXT("... it is refused"), Capture.ErrorCode, FString(ErrorCodes::ERR_STANDALONE_NOT_TRACKED));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorStandaloneLogTailPartialLineTest,
    "PinWright.editor.standalone_status.LogTailDropsPartialFirstLine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorStandaloneLogTailPartialLineTest::RunTest(const FString& Parameters)
{
    const FString Log = TestEditorStandaloneStatusHelpers::WriteLog(TEXT("aaaa\nbbbb\ncccc\n"));
    TArray<FString> Lines;
    int64 Size = 0;
    // The last 7 bytes are "b\ncccc\n": the window starts inside "bbbb".
    TestTrue(TEXT("the file reads"), ReadLogTail(Log, 10, 7, Lines, Size));
    TestEqual(TEXT("size is the whole file"), Size, static_cast<int64>(15));
    TestEqual(TEXT("the cut line is dropped, not returned as 'b'"), Lines, TArray<FString>({TEXT("cccc")}));

    TestTrue(TEXT("the file reads whole"), ReadLogTail(Log, 10, 1024, Lines, Size));
    TestEqual(TEXT("a whole-file window keeps its first line"), Lines, TArray<FString>({TEXT("aaaa"), TEXT("bbbb"), TEXT("cccc")}));
    IFileManager::Get().Delete(*Log);

    TestFalse(TEXT("a missing file is reported missing"), ReadLogTail(Log, 10, 1024, Lines, Size));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorStandaloneWindowCaptureTest,
    "PinWright.editor.standalone_status.WindowCaptureReadsTheProcessWindow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorStandaloneWindowCaptureTest::RunTest(const FString& Parameters)
{
#if PLATFORM_LINUX
    // A window of known colour stamped with a _NET_WM_PID no real process can have (pid_max is
    // at most 2^22), standing in for the game window SDL stamps with the game's pid.
    using XDisplay = void*;
    void* Xlib = FPlatformMisc::GetEnvironmentVariable(TEXT("DISPLAY")).IsEmpty() ? nullptr : dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
    const auto OpenDisplay = Xlib ? reinterpret_cast<XDisplay (*)(const char*)>(dlsym(Xlib, "XOpenDisplay")) : nullptr;
    XDisplay Display = OpenDisplay ? OpenDisplay(nullptr) : nullptr;
    if (!Display)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-x-display"),
            TEXT("window capture reads an X window and this editor has no X display."));
        return true;
    }
    const auto CloseDisplay = reinterpret_cast<int (*)(XDisplay)>(dlsym(Xlib, "XCloseDisplay"));
    const auto RootWindow = reinterpret_cast<unsigned long (*)(XDisplay)>(dlsym(Xlib, "XDefaultRootWindow"));
    const auto CreateSimpleWindow = reinterpret_cast<unsigned long (*)(XDisplay, unsigned long, int, int, unsigned int, unsigned int,
        unsigned int, unsigned long, unsigned long)>(dlsym(Xlib, "XCreateSimpleWindow"));
    const auto InternAtom = reinterpret_cast<unsigned long (*)(XDisplay, const char*, int)>(dlsym(Xlib, "XInternAtom"));
    const auto ChangeProperty = reinterpret_cast<int (*)(XDisplay, unsigned long, unsigned long, unsigned long, int, int,
        const unsigned char*, int)>(dlsym(Xlib, "XChangeProperty"));
    const auto MapRaised = reinterpret_cast<int (*)(XDisplay, unsigned long)>(dlsym(Xlib, "XMapRaised"));
    const auto DestroyWindow = reinterpret_cast<int (*)(XDisplay, unsigned long)>(dlsym(Xlib, "XDestroyWindow"));
    const auto Sync = reinterpret_cast<int (*)(XDisplay, int)>(dlsym(Xlib, "XSync"));
    if (!TestTrue(TEXT("fixture: every libX11 entry point resolves"), CloseDisplay && RootWindow && CreateSimpleWindow
            && InternAtom && ChangeProperty && MapRaised && DestroyWindow && Sync))
    {
        return false;  // the connection leaks with the failed fixture; it cannot be closed without XCloseDisplay
    }

    const uint32 FakePid = 0x40000000u + FPlatformProcess::GetCurrentProcessId();
    FStandaloneWindowCapture Capture;
    FString Code;
    FString Error;
    TestFalse(TEXT("before the window exists nothing is captured"), CaptureProcessWindow(FakePid, Capture, Code, Error));
    TestEqual(TEXT("... and the refusal is WINDOW_NOT_FOUND"), Code, FString(ErrorCodes::ERR_WINDOW_NOT_FOUND));

    // TrueColor 24-bit pixel value = 0xRRGGBB.
    const unsigned long Window = CreateSimpleWindow(Display, RootWindow(Display), 0, 0, 64, 48, 0, 0, 0x3366CCul);
    const long PidValue = FakePid;
    // XA_CARDINAL = 6, format 32, PropModeReplace = 0.
    ChangeProperty(Display, Window, InternAtom(Display, "_NET_WM_PID", 0), 6, 32, 0,
        reinterpret_cast<const unsigned char*>(&PidValue), 1);
    MapRaised(Display, Window);
    Sync(Display, 0);

    bool bCaptured = false;
    const double Deadline = FPlatformTime::Seconds() + 3.0;
    while (!bCaptured && FPlatformTime::Seconds() < Deadline)
    {
        bCaptured = CaptureProcessWindow(FakePid, Capture, Code, Error)
            && Capture.Pixels.IsValidIndex(24 * Capture.Width + 32)
            && Capture.Pixels[24 * Capture.Width + 32].B > 0xA0;
        if (!bCaptured)
        {
            FPlatformProcess::Sleep(0.05f);
        }
    }
    if (TestTrue(FString::Printf(TEXT("the stamped window is captured (last error %s: %s)"), *Code, *Error), bCaptured))
    {
        TestEqual(TEXT("width is the window's"), Capture.Width, 64);
        TestEqual(TEXT("height is the window's"), Capture.Height, 48);
        TestEqual(TEXT("exactly one window carries the pid"), Capture.CandidateWindows, 1);
        TestEqual(TEXT("the window id is the one created"), Capture.WindowId, static_cast<uint64>(Window));
        const FColor Center = Capture.Pixels[24 * Capture.Width + 32];
        TestTrue(FString::Printf(TEXT("the pixels are the window's colour #3366CC (read %s)"), *Center.ToHex()),
            FMath::Abs(Center.R - 0x33) <= 2 && FMath::Abs(Center.G - 0x66) <= 2 && FMath::Abs(Center.B - 0xCC) <= 2);
    }

    DestroyWindow(Display, Window);
    Sync(Display, 0);
    CloseDisplay(Display);
    TestFalse(TEXT("once the window is gone nothing is captured"), CaptureProcessWindow(FakePid, Capture, Code, Error));
    TestEqual(TEXT("... WINDOW_NOT_FOUND again"), Code, FString(ErrorCodes::ERR_WINDOW_NOT_FOUND));
#else
    FStandaloneWindowCapture Capture;
    FString Code;
    FString Error;
    TestFalse(TEXT("window capture is Linux/X11 only and refuses elsewhere"), CaptureProcessWindow(1, Capture, Code, Error));
    TestEqual(TEXT("... with NOT_SUPPORTED"), Code, FString(ErrorCodes::ERR_NOT_SUPPORTED));
#endif
    return true;
}
