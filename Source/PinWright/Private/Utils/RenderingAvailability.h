// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Misc/App.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"

// The one renderer-availability predicate, and the one handler-side guard built on it.
//
// An editor launched with -NullRHI (proxy launch mode `headless`) or a commandlet has no GPU
// renderer: captures read back nothing, thumbnails never draw, a benchmark times a frame that
// renders nothing. A verb that needs a renderer calls RequireRenderer first and returns on false,
// so the caller gets a typed RENDERING_UNAVAILABLE instead of a crash, a hang, or a blank frame
// reported as success. Tests skip through PinWrightTestSkip::SkipIfRenderingUnavailable
// (Tests/TestSkipReporting.h), which reads the same predicate.
//
// FApp::CanEverRender() is false for -nullrhi on the command line, commandlets without
// -AllowCommandletRendering, and dedicated servers (App.h, identical 5.3-5.8). It is Core-only,
// so this header adds no RHI module dependency to whichever module includes it.
namespace PinWrightRendering
{
    // Test seam: >0 forces IsAvailable() false, so the guard's NullRHI branch can be exercised in
    // an editor that does render. Only FScopedForceUnavailableForTesting touches it.
    inline int32& ForcedUnavailableDepth()
    {
        static int32 Depth = 0;
        return Depth;
    }

    inline bool IsAvailable()
    {
        return FApp::CanEverRender() && ForcedUnavailableDepth() == 0;
    }

    // Returns true when the verb may render. Otherwise sends RENDERING_UNAVAILABLE and returns
    // false; the caller returns immediately.
    inline bool RequireRenderer(const FHandlerContext& Ctx)
    {
        if (IsAvailable())
        {
            return true;
        }
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("method"), Ctx.GetMethod());
        TArray<TSharedPtr<FJsonValue>> Modes;
        Modes.Add(MakeShared<FJsonValueString>(TEXT("offscreen")));
        Modes.Add(MakeShared<FJsonValueString>(TEXT("visible")));
        Data->SetArrayField(TEXT("renderingModes"), Modes);
        Ctx.SendError(ErrorCodes::ERR_RENDERING_UNAVAILABLE,
            FString::Printf(
                TEXT("%s needs a GPU renderer, and this editor has none (launched with -NullRHI, ")
                TEXT("i.e. mode 'headless', or running as a commandlet). Relaunch the editor in ")
                TEXT("mode 'offscreen' or 'visible'."),
                *Ctx.GetMethod()),
            Data);
        return false;
    }

#if WITH_DEV_AUTOMATION_TESTS
    struct FScopedForceUnavailableForTesting
    {
        FScopedForceUnavailableForTesting() { ++ForcedUnavailableDepth(); }
        ~FScopedForceUnavailableForTesting() { --ForcedUnavailableDepth(); }
        FScopedForceUnavailableForTesting(const FScopedForceUnavailableForTesting&) = delete;
        FScopedForceUnavailableForTesting& operator=(const FScopedForceUnavailableForTesting&) = delete;
    };
#endif
}
