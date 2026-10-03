// Copyright (c) 2026 Alexander Penkin. MIT License.

// editor.standalone_status - observe a -game process spawned by editor.launch_standalone.
//
// PinWright's RPC gateway lives in the editor process only (every module is Editor-type and
// none loads under -game), so drive.observe / ui.screenshot / DOM export cannot reach a spawned
// game. What the editor CAN observe from outside is reported here, each item measured:
//   - process state: the registry holds the FProcHandle launch_standalone used to close, so
//     `running` comes from the OS and `exitCode` from the reaped child;
//   - its log: launch_standalone points each process at its own -abslog file, tailed here;
//   - its window pixels: Linux/X11 only, the process's largest _NET_WM_PID window read with
//     XGetImage over a private X connection (libX11 resolved with dlopen, no link dependency).
// Input into the game and widget/DOM introspection are NOT covered.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Editor/EditorLaunchHandlerInternal.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Utils/ScreenshotUtils.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"

#if PLATFORM_LINUX
#include <dlfcn.h>
#endif

namespace EditorStandaloneStatusLocal
{
    struct FEntry
    {
        FProcHandle Handle;
        FString CommandLine;
        FString LogFile;
        bool bExited = false;
        TOptional<int32> ExitCode;
    };

    TMap<uint32, FEntry>& Registry()
    {
        static TMap<uint32, FEntry> Entries;
        return Entries;
    }

    bool bTickerActive = false;

    void Poll(FEntry& Entry)
    {
        if (Entry.bExited || FPlatformProcess::IsProcRunning(Entry.Handle))
        {
            return;
        }
        Entry.bExited = true;
        int32 Code = 0;
        // Linux has no exit code for a child killed by a signal; GetProcReturnCode is false then.
        if (FPlatformProcess::GetProcReturnCode(Entry.Handle, &Code))
        {
            Entry.ExitCode = Code;
        }
        FPlatformProcess::CloseProc(Entry.Handle);
    }

    // Reaps exited children promptly (CloseProc of a detached handle used to do that through
    // a waiter thread) and unregisters itself once nothing is left running.
    bool Tick(float)
    {
        bool bAnyRunning = false;
        for (TPair<uint32, FEntry>& Pair : Registry())
        {
            Poll(Pair.Value);
            bAnyRunning |= !Pair.Value.bExited;
        }
        bTickerActive = bAnyRunning;
        return bAnyRunning;
    }

#if PLATFORM_LINUX
    using XDisplay = void*;
    using XWindow = unsigned long;
    using XAtom = unsigned long;
    using XErrorHandler = int (*)(XDisplay, void*);

    // Xlib's XImage, mirrored field for field (Xlib.h) because this file has no X11 headers.
    struct FXImage
    {
        int width, height, xoffset, format;
        char* data;
        int byte_order, bitmap_unit, bitmap_bit_order, bitmap_pad, depth, bytes_per_line, bits_per_pixel;
        unsigned long red_mask, green_mask, blue_mask;
        char* obdata;
        struct
        {
            void* create_image;
            int (*destroy_image)(FXImage*);
            unsigned long (*get_pixel)(FXImage*, int, int);
            void* put_pixel;
            void* sub_image;
            void* add_pixel;
        } f;
    };

    constexpr int32 MaxDescent = 16;

    struct FX11Api
    {
        XDisplay Display = nullptr;
        XWindow (*DefaultRootWindow)(XDisplay) = nullptr;
        int (*DefaultScreen)(XDisplay) = nullptr;
        int (*QueryTree)(XDisplay, XWindow, XWindow*, XWindow*, XWindow**, unsigned int*) = nullptr;
        int (*GetWindowProperty)(XDisplay, XWindow, XAtom, long, long, int, XAtom,
            XAtom*, int*, unsigned long*, unsigned long*, unsigned char**) = nullptr;
        int (*GetGeometry)(XDisplay, XWindow, XWindow*, int*, int*, unsigned int*, unsigned int*,
            unsigned int*, unsigned int*) = nullptr;
        FXImage* (*GetImage)(XDisplay, XWindow, int, int, unsigned int, unsigned int, unsigned long, int) = nullptr;
        int (*FetchName)(XDisplay, XWindow, char**) = nullptr;
        XAtom (*InternAtom)(XDisplay, const char*, int) = nullptr;
        XWindow (*GetSelectionOwner)(XDisplay, XAtom) = nullptr;
        int (*Free)(void*) = nullptr;
        int (*Sync)(XDisplay, int) = nullptr;
        XErrorHandler (*SetErrorHandler)(XErrorHandler) = nullptr;
        XAtom NetWmPid = 0;
        FString Error;
    };

    // Opened once and kept for the editor's lifetime, like FDriveOsInput's connection.
    FX11Api& GetApi()
    {
        static FX11Api Api;
        static bool bLoaded = false;
        if (bLoaded)
        {
            return Api;
        }
        bLoaded = true;
        void* Xlib = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
        if (!Xlib)
        {
            Api.Error = TEXT("window capture needs libX11.so.6, which could not be loaded (is this an X11 session?).");
            return Api;
        }
        const auto OpenDisplay = reinterpret_cast<XDisplay (*)(const char*)>(dlsym(Xlib, "XOpenDisplay"));
        Api.DefaultRootWindow = reinterpret_cast<decltype(Api.DefaultRootWindow)>(dlsym(Xlib, "XDefaultRootWindow"));
        Api.DefaultScreen = reinterpret_cast<decltype(Api.DefaultScreen)>(dlsym(Xlib, "XDefaultScreen"));
        Api.QueryTree = reinterpret_cast<decltype(Api.QueryTree)>(dlsym(Xlib, "XQueryTree"));
        Api.GetWindowProperty = reinterpret_cast<decltype(Api.GetWindowProperty)>(dlsym(Xlib, "XGetWindowProperty"));
        Api.GetGeometry = reinterpret_cast<decltype(Api.GetGeometry)>(dlsym(Xlib, "XGetGeometry"));
        Api.GetImage = reinterpret_cast<decltype(Api.GetImage)>(dlsym(Xlib, "XGetImage"));
        Api.FetchName = reinterpret_cast<decltype(Api.FetchName)>(dlsym(Xlib, "XFetchName"));
        Api.InternAtom = reinterpret_cast<decltype(Api.InternAtom)>(dlsym(Xlib, "XInternAtom"));
        Api.GetSelectionOwner = reinterpret_cast<decltype(Api.GetSelectionOwner)>(dlsym(Xlib, "XGetSelectionOwner"));
        Api.Free = reinterpret_cast<decltype(Api.Free)>(dlsym(Xlib, "XFree"));
        Api.Sync = reinterpret_cast<decltype(Api.Sync)>(dlsym(Xlib, "XSync"));
        Api.SetErrorHandler = reinterpret_cast<decltype(Api.SetErrorHandler)>(dlsym(Xlib, "XSetErrorHandler"));
        if (!OpenDisplay || !Api.DefaultRootWindow || !Api.DefaultScreen || !Api.QueryTree || !Api.GetWindowProperty
            || !Api.GetGeometry || !Api.GetImage || !Api.FetchName || !Api.InternAtom || !Api.GetSelectionOwner
            || !Api.Free || !Api.Sync || !Api.SetErrorHandler)
        {
            Api.Error = TEXT("libX11 loaded but an expected symbol is missing; window capture is unavailable.");
            return Api;
        }
        Api.Display = OpenDisplay(nullptr);
        if (!Api.Display)
        {
            Api.Error = TEXT("XOpenDisplay(NULL) failed: this editor has no X display (DISPLAY unset, or access denied).");
            return Api;
        }
        Api.NetWmPid = Api.InternAtom(Api.Display, "_NET_WM_PID", 0);
        return Api;
    }

    // A window can vanish mid-walk and XGetImage refuses unviewable windows with BadMatch;
    // Xlib's default handler would exit the editor for either.
    int IgnoreXError(XDisplay, void*)
    {
        return 0;
    }

    uint32 ReadPid(FX11Api& Api, XWindow Window)
    {
        XAtom Type = 0;
        int Format = 0;
        unsigned long Count = 0;
        unsigned long After = 0;
        unsigned char* Data = nullptr;
        uint32 Pid = 0;
        // 6 = XA_CARDINAL; format-32 items come back as C longs.
        if (Api.GetWindowProperty(Api.Display, Window, Api.NetWmPid, 0, 1, 0, 6,
                &Type, &Format, &Count, &After, &Data) == 0 && Data)
        {
            if (Format == 32 && Count == 1)
            {
                Pid = static_cast<uint32>(*reinterpret_cast<unsigned long*>(Data));
            }
            Api.Free(Data);
        }
        return Pid;
    }

    struct FCandidate
    {
        XWindow Window = 0;
        int64 Area = 0;
    };

    void CollectWindows(FX11Api& Api, XWindow Window, uint32 Pid, int32 Depth, TArray<FCandidate>& Out)
    {
        if (Depth > 0 && ReadPid(Api, Window) == Pid)
        {
            XWindow Root = 0;
            int X = 0, Y = 0;
            unsigned int W = 0, H = 0, Border = 0, BitDepth = 0;
            if (Api.GetGeometry(Api.Display, Window, &Root, &X, &Y, &W, &H, &Border, &BitDepth))
            {
                Out.Add({Window, static_cast<int64>(W) * H});
            }
        }
        if (Depth >= MaxDescent)
        {
            return;
        }
        XWindow Root = 0;
        XWindow Parent = 0;
        XWindow* Children = nullptr;
        unsigned int NumChildren = 0;
        if (!Api.QueryTree(Api.Display, Window, &Root, &Parent, &Children, &NumChildren))
        {
            return;
        }
        for (unsigned int i = 0; i < NumChildren; ++i)
        {
            CollectWindows(Api, Children[i], Pid, Depth + 1, Out);
        }
        if (Children)
        {
            Api.Free(Children);
        }
    }

    uint8 Channel(unsigned long Pixel, unsigned long Mask)
    {
        if (Mask == 0)
        {
            return 0;
        }
        const uint32 Shift = FMath::CountTrailingZeros64(Mask);
        const unsigned long Max = Mask >> Shift;
        return static_cast<uint8>(((Pixel & Mask) >> Shift) * 255 / Max);
    }
#endif
}

bool CaptureProcessWindow(uint32 Pid, FStandaloneWindowCapture& Out, FString& OutErrorCode, FString& OutError)
{
#if PLATFORM_LINUX
    namespace Local = EditorStandaloneStatusLocal;
    Local::FX11Api& Api = Local::GetApi();
    if (!Api.Display)
    {
        OutErrorCode = ErrorCodes::ERR_NOT_SUPPORTED;
        OutError = Api.Error;
        return false;
    }

    const Local::XErrorHandler Previous = Api.SetErrorHandler(&Local::IgnoreXError);
    TArray<Local::FCandidate> Candidates;
    Local::CollectWindows(Api, Api.DefaultRootWindow(Api.Display), Pid, 0, Candidates);
    Candidates.Sort([](const Local::FCandidate& A, const Local::FCandidate& B) { return A.Area > B.Area; });
    Out.CandidateWindows = Candidates.Num();

    const FString CmAtomName = FString::Printf(TEXT("_NET_WM_CM_S%d"), Api.DefaultScreen(Api.Display));
    Out.bCompositorActive = Api.GetSelectionOwner(Api.Display, Api.InternAtom(Api.Display, TCHAR_TO_UTF8(*CmAtomName), 0)) != 0;

    Local::FXImage* Image = nullptr;
    for (const Local::FCandidate& Candidate : Candidates)
    {
        Local::XWindow Root = 0;
        int X = 0, Y = 0;
        unsigned int W = 0, H = 0, Border = 0, BitDepth = 0;
        if (Candidate.Area <= 0 || !Api.GetGeometry(Api.Display, Candidate.Window, &Root, &X, &Y, &W, &H, &Border, &BitDepth))
        {
            continue;
        }
        // AllPlanes, ZPixmap = 2. Null for an unmapped window, or one not wholly on screen.
        Image = Api.GetImage(Api.Display, Candidate.Window, 0, 0, W, H, ~0UL, 2);
        if (Image)
        {
            Out.WindowId = Candidate.Window;
            char* Name = nullptr;
            if (Api.FetchName(Api.Display, Candidate.Window, &Name) && Name)
            {
                Out.Title = UTF8_TO_TCHAR(Name);
                Api.Free(Name);
            }
            break;
        }
    }
    Api.Sync(Api.Display, 0);
    Api.SetErrorHandler(Previous);

    if (!Image)
    {
        OutErrorCode = Candidates.Num() == 0 ? ErrorCodes::ERR_WINDOW_NOT_FOUND : ErrorCodes::ERR_CAPTURE_FAILED;
        OutError = Candidates.Num() == 0
            ? FString::Printf(TEXT("No X window carries _NET_WM_PID=%u (not created yet, already closed, or on another display)."), Pid)
            : FString::Printf(TEXT("%d window(s) carry _NET_WM_PID=%u but XGetImage read none: unmapped (minimized) or not wholly on screen."), Candidates.Num(), Pid);
        return false;
    }

    Out.Width = Image->width;
    Out.Height = Image->height;
    Out.Pixels.SetNumUninitialized(Out.Width * Out.Height);
    for (int32 Y = 0; Y < Out.Height; ++Y)
    {
        for (int32 X = 0; X < Out.Width; ++X)
        {
            const unsigned long Pixel = Image->f.get_pixel(Image, X, Y);
            Out.Pixels[Y * Out.Width + X] = FColor(
                Local::Channel(Pixel, Image->red_mask), Local::Channel(Pixel, Image->green_mask), Local::Channel(Pixel, Image->blue_mask), 255);
        }
    }
    Image->f.destroy_image(Image);
    return true;
#else
    OutErrorCode = ErrorCodes::ERR_NOT_SUPPORTED;
    OutError = TEXT("Standalone window capture is implemented for Linux/X11 only.");
    return false;
#endif
}

void TrackStandaloneProcess(uint32 Pid, FProcHandle Handle, const FString& CommandLine, const FString& LogFile)
{
    namespace Local = EditorStandaloneStatusLocal;
    Local::FEntry& Entry = Local::Registry().FindOrAdd(Pid);
    Entry = Local::FEntry();
    Entry.Handle = Handle;
    Entry.CommandLine = CommandLine;
    Entry.LogFile = LogFile;
    if (!Local::bTickerActive)
    {
        Local::bTickerActive = true;
        FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Local::Tick), 1.0f);
    }
}

bool ReadLogTail(const FString& Path, int32 MaxLines, int64 MaxBytes, TArray<FString>& OutLines, int64& OutFileSize)
{
    OutLines.Reset();
    OutFileSize = 0;
    // AllowWrite: the game still holds the log open for writing.
    TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Path, FILEREAD_AllowWrite));
    if (!Reader)
    {
        return false;
    }
    OutFileSize = Reader->TotalSize();
    const int64 Start = FMath::Max<int64>(0, OutFileSize - MaxBytes);
    TArray<uint8> Bytes;
    Bytes.SetNumUninitialized(OutFileSize - Start);
    Reader->Seek(Start);
    Reader->Serialize(Bytes.GetData(), Bytes.Num());

    const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
    FString Text(Converted.Length(), Converted.Get());
    TArray<FString> Lines;
    Text.ParseIntoArrayLines(Lines, /*CullEmpty*/ false);
    if (Start > 0 && Lines.Num() > 0)
    {
        Lines.RemoveAt(0);  // began mid-line
    }
    if (Lines.Num() > 0 && Lines.Last().IsEmpty())
    {
        Lines.Pop();  // the trailing newline
    }
    const int32 First = FMath::Max(0, Lines.Num() - MaxLines);
    for (int32 i = First; i < Lines.Num(); ++i)
    {
        OutLines.Add(MoveTemp(Lines[i]));
    }
    return true;
}

REGISTER_RPC_HANDLER("editor.standalone_status", "editor",
    "Observe a -game process spawned by editor.launch_standalone in this editor session: "
    "whether it is running (OS-measured), its exit code once it exits, the tail of its -abslog file, "
    "and optionally a PNG of its window (Linux/X11 only). The RPC gateway does not run inside a -game "
    "process, so drive.*/ui.*/DOM verbs cannot reach it; this is the observation surface that exists.",
    RPC_PARAMS(
        RPC_PARAM_REQ("pid", "number", "OS pid returned by editor.launch_standalone in this editor session."),
        RPC_PARAM_DEF("tailLines", "number", "Log lines to return from the end of the process's log. Clamped to [0,1000].", "50"),
        RPC_PARAM_DEF("capture", "boolean", "Capture the process's largest X window to a PNG under Saved/Screenshots/standalone (Linux/X11 only; reported in `capture`, never fails the call).", "false")
    ))
{
    namespace Local = EditorStandaloneStatusLocal;
    const uint32 Pid = static_cast<uint32>(Ctx.GetInt(TEXT("pid"), 0));
    Local::FEntry* Entry = Local::Registry().Find(Pid);
    if (!Entry)
    {
        Ctx.SendError(ErrorCodes::ERR_STANDALONE_NOT_TRACKED, FString::Printf(
            TEXT("pid %u was not launched by editor.launch_standalone in this editor session; only those are tracked."), Pid));
        return true;
    }
    Local::Poll(*Entry);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetNumberField(TEXT("pid"), Pid);
    Resp->SetBoolField(TEXT("running"), !Entry->bExited);
    if (Entry->bExited)
    {
        if (Entry->ExitCode.IsSet())
        {
            Resp->SetNumberField(TEXT("exitCode"), Entry->ExitCode.GetValue());
        }
        else
        {
            Resp->SetStringField(TEXT("exitCodeUnavailable"), TEXT("the process ended without an exit status (killed by a signal)"));
        }
    }
    Resp->SetStringField(TEXT("commandLine"), Entry->CommandLine);
    Resp->SetStringField(TEXT("logPath"), Entry->LogFile);

    TArray<FString> Lines;
    int64 LogBytes = 0;
    const int32 TailLines = FMath::Clamp(Ctx.GetInt(TEXT("tailLines"), 50), 0, 1000);
    const bool bLogExists = ReadLogTail(Entry->LogFile, TailLines, 1024 * 1024, Lines, LogBytes);
    Resp->SetBoolField(TEXT("logExists"), bLogExists);
    Resp->SetNumberField(TEXT("logBytes"), static_cast<double>(LogBytes));
    TArray<TSharedPtr<FJsonValue>> TailValues;
    for (const FString& Line : Lines)
    {
        TailValues.Add(MakeShared<FJsonValueString>(Line));
    }
    Resp->SetArrayField(TEXT("logTail"), TailValues);

    if (Ctx.GetBool(TEXT("capture"), false))
    {
        TSharedPtr<FJsonObject> CaptureObj = MakeShared<FJsonObject>();
        FStandaloneWindowCapture Capture;
        FString Code;
        FString Error;
        bool bCaptured = false;
        if (Entry->bExited)
        {
            // Never look up windows by a pid the OS may already have handed to someone else.
            Code = ErrorCodes::ERR_WINDOW_NOT_FOUND;
            Error = TEXT("the process has exited");
        }
        else if (CaptureProcessWindow(Pid, Capture, Code, Error))
        {
            TArray<uint8> Png;
            FString Filename;
            const FString Path = PinWrightScreenshotUtils::MakeScreenshotOutputPath(
                TEXT(""), FString::Printf(TEXT("standalone_%u"), Pid), TEXT("standalone"), Filename);
            if (PinWrightScreenshotUtils::EncodeBitmapToPng(Capture.Width, Capture.Height, Capture.Pixels, Png)
                && FFileHelper::SaveArrayToFile(Png, *Path))
            {
                bCaptured = true;
                const PinWrightRenderCapture::FCaptureImageStats Stats = PinWrightRenderCapture::CalculateCaptureImageStats(Capture.Pixels);
                CaptureObj->SetStringField(TEXT("path"), FPaths::ConvertRelativePathToFull(Path));
                CaptureObj->SetNumberField(TEXT("width"), Capture.Width);
                CaptureObj->SetNumberField(TEXT("height"), Capture.Height);
                CaptureObj->SetNumberField(TEXT("windowId"), static_cast<double>(Capture.WindowId));
                CaptureObj->SetStringField(TEXT("title"), Capture.Title);
                CaptureObj->SetNumberField(TEXT("candidateWindows"), Capture.CandidateWindows);
                CaptureObj->SetBoolField(TEXT("compositorActive"), Capture.bCompositorActive);
                TSharedPtr<FJsonObject> StatsObj = MakeShared<FJsonObject>();
                StatsObj->SetNumberField(TEXT("meanLuminance"), Stats.MeanLuminance);
                StatsObj->SetNumberField(TEXT("minLuminance"), Stats.MinLuminance);
                StatsObj->SetNumberField(TEXT("maxLuminance"), Stats.MaxLuminance);
                CaptureObj->SetObjectField(TEXT("imageStats"), StatsObj);
            }
            else
            {
                Code = ErrorCodes::ERR_CAPTURE_FAILED;
                Error = FString::Printf(TEXT("could not encode or write %s"), *Path);
            }
        }
        CaptureObj->SetBoolField(TEXT("captured"), bCaptured);
        if (!bCaptured)
        {
            CaptureObj->SetStringField(TEXT("errorCode"), Code);
            CaptureObj->SetStringField(TEXT("error"), Error);
        }
        Resp->SetObjectField(TEXT("capture"), CaptureObj);
    }

    Ctx.SendSuccess(Resp);
    return true;
}
