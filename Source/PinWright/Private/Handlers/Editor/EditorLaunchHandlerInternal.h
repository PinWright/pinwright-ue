// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once
#include "CoreMinimal.h"
#include "HAL/PlatformProcess.h"

// Pure helper that builds the per-process command line for editor.launch_standalone.
// Exposed (DLL-exported) so unit tests in a separate TU can link to it directly
// without re-implementing the formatting logic.
PINWRIGHT_API FString BuildStandaloneCommandLine(
    const FString& ProjectPath, const FString& Map, int32 InstanceIndex,
    int32 NumClients, bool bListenServer, const FString& ExtraArgs);

// The log file a spawned -game process writes. When ExtraArgs already carries a log argument,
// its absolute path is returned, resolved with the engine's precedence (LOG= / LogFileName=,
// relative to the project log dir, win over ABSLOG=; a name without a .log/.txt extension falls
// back to <Project>.log), and CommandLine is left alone. Otherwise -abslog="DefaultLogFile" is
// appended (which also keeps the game from rotating the editor's own Saved/Logs/<Project>.log).
PINWRIGHT_API FString AppendStandaloneLogArg(FString& CommandLine, const FString& ExtraArgs,
    const FString& DefaultLogFile);

// Empty unless a caller log argument is known to be held by another process; else why. The engine
// opens its log exclusively and moves on to <name>_2.log, _3.log... when it is held, so a caller
// log argument shared by NumClients > 1 processes, or one resolving to the editor's own log
// (e.g. a name without an extension, when the editor uses the default <Project>.log), would
// report another process's file. A log shared with another running process (a previous launch,
// another editor) is not detected.
PINWRIGHT_API FString StandaloneCallerLogRefusal(const FString& ExtraArgs, int32 NumClients);

// Process registry behind editor.standalone_status (EditorStandaloneStatusHandler.cpp). The
// registry owns Handle from here on: it is polled (and the child reaped) by a core ticker, so
// an exited game never lingers as a zombie, and its exit code is kept.
PINWRIGHT_API void TrackStandaloneProcess(uint32 Pid, FProcHandle Handle,
    const FString& CommandLine, const FString& LogFile);

// The last MaxLines complete lines of the file at Path, reading at most its last MaxBytes
// bytes (a partial first line inside that window is dropped). False when the file does not
// exist or cannot be opened; OutFileSize is its size in bytes.
PINWRIGHT_API bool ReadLogTail(const FString& Path, int32 MaxLines, int64 MaxBytes,
    TArray<FString>& OutLines, int64& OutFileSize);

// One X11 top-level window of a process, captured to a PNG. LINUX/X11 ONLY: elsewhere (and
// without an X display) the capture fails with ERR_NOT_SUPPORTED.
struct FStandaloneWindowCapture
{
    uint64 WindowId = 0;
    FString Title;
    int32 Width = 0;
    int32 Height = 0;
    // Windows carrying _NET_WM_PID == Pid that were considered.
    int32 CandidateWindows = 0;
    // A compositing manager owns _NET_WM_CM_S<screen>. Without one, the parts of the window
    // covered by other windows read back as whatever covers them (XGetImage leaves them undefined).
    bool bCompositorActive = false;
    TArray<FColor> Pixels;
};

// Finds the largest viewable X window whose _NET_WM_PID is Pid and reads its pixels. False with
// OutErrorCode (an ErrorCodes::ERR_*) and OutError on failure.
PINWRIGHT_API bool CaptureProcessWindow(uint32 Pid, FStandaloneWindowCapture& Out,
    FString& OutErrorCode, FString& OutError);
