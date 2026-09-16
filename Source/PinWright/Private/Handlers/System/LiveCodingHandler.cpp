// Copyright (c) 2026 Alexander Penkin. MIT License.

// LiveCodingHandler.cpp — system.live_coding_* : in-process Live Coding compile
// trigger + status readback over ILiveCodingModule (the Ctrl+Alt+F11 equivalent).
//
// Motivation: system.run_ubt spawns an EXTERNAL UBT child process that cannot patch
// the running editor. Agents iterating on C++ with the user need the editor's own
// in-process Live Coding compile plus a way to read whether the patch applied. This
// mirrors Epic's UE 5.8 LiveCodingToolset (one tool over ILiveCodingModule):
// Engine/Plugins/Experimental/Toolsets/LiveCodingToolset.
//
// Registration is unconditional so discovery lists system.live_coding_* even on a
// build without Live Coding (non-Windows / -game / Shipping); only the handler body
// branches on MCP_HAS_LIVE_CODING. Build.cs soft-links the LiveCoding module under
// the same `Target.bWithLiveCoding` gate UBT uses to define WITH_LIVE_CODING (Epic
// does the same in LiveCodingToolset.Build.cs). Mirrors the optional-engine-plugin
// convention of Handlers/Water/WaterHandler.cpp.
//
// Synchronous, not a job: ILiveCodingModule exposes the compile RESULT only through
// the synchronous Compile(WaitForCompletion, &Result) out-param — the async path
// (Compile(None,...) + GetOnPatchCompleteDelegate) carries no per-request result, so
// a job wrapper could not report the success/failure the ticket needs. So this
// follows Epic's synchronous toolset and caches the last result in FPluginState, which
// system.live_coding_status reads back durably (and which recovers the result if a
// long compile's wire response times out). Self-patching (recompiling PinWright
// itself) is survived because the whole operation completes in one game-thread call
// stack before returning to the pre-patch handler frame, and the FPluginState
// singleton's data survives Live++'s in-place code patch.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "State/PluginState.h"
#include "Dom/JsonObject.h"

#if WITH_LIVE_CODING && __has_include("ILiveCodingModule.h")
#define MCP_HAS_LIVE_CODING 1
#else
#define MCP_HAS_LIVE_CODING 0
#endif

#if MCP_HAS_LIVE_CODING
#include "ILiveCodingModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/ScopeLock.h"
#include "Misc/OutputDevice.h"

namespace
{
    // Human-readable form of the compile-result enum. Also the value cached in
    // FPluginState for system.live_coding_status to read back, so FPluginState.h
    // stays decoupled from the Windows-only ILiveCodingModule header.
    FString LiveCodingResultToString(ELiveCodingCompileResult Result)
    {
        switch (Result)
        {
        case ELiveCodingCompileResult::Success:            return TEXT("Success");
        case ELiveCodingCompileResult::NoChanges:          return TEXT("NoChanges");
        case ELiveCodingCompileResult::InProgress:         return TEXT("InProgress");
        case ELiveCodingCompileResult::CompileStillActive: return TEXT("CompileStillActive");
        case ELiveCodingCompileResult::NotStarted:         return TEXT("NotStarted");
        case ELiveCodingCompileResult::Failure:            return TEXT("Failure");
        case ELiveCodingCompileResult::Cancelled:          return TEXT("Cancelled");
        default:                                           return TEXT("Unknown");
        }
    }

    // Captures LogLiveCoding output during a compile so the RPC can return the
    // compiler/linker tail. Live Coding logs from worker threads, so Serialize must
    // be thread-safe (mirrors Epic's FLiveCodingOutputCollector).
    class FLiveCodingLogCollector : public FOutputDevice
    {
    public:
        virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
        {
            static const FName LiveCodingCategory(TEXT("LogLiveCoding"));
            if (Category != LiveCodingCategory)
            {
                return;
            }
            FScopeLock Lock(&CriticalSection);
            if (!Captured.IsEmpty())
            {
                Captured += TEXT("\n");
            }
            Captured += V;
        }

        virtual bool CanBeUsedOnAnyThread() const override { return true; }

        FString Consume()
        {
            FScopeLock Lock(&CriticalSection);
            return MoveTemp(Captured);
        }

    private:
        FCriticalSection CriticalSection;
        FString Captured;
    };

    // Resolves the in-process Live Coding module, or nullptr if it is not loaded
    // (e.g. never started this session). Does not force-load — matches Epic.
    ILiveCodingModule* GetLiveCodingModule()
    {
        return FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
    }
}
#endif // MCP_HAS_LIVE_CODING

// ---- system.live_coding_status ----
REGISTER_RPC_HANDLER("system.live_coding_status", "system",
    "Report in-process Live Coding availability: whether it is compiled into this build, the module is loaded, enabled for this session, started, currently compiling, and the last compile result. Read-only companion to system.live_coding_compile.",
    RPC_NO_PARAMS)
{
    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
#if MCP_HAS_LIVE_CODING
    ILiveCodingModule* LiveCoding = GetLiveCodingModule();
    const bool bModuleLoaded = (LiveCoding != nullptr);
    Out->SetBoolField(TEXT("compiledIntoBuild"), true);
    Out->SetBoolField(TEXT("moduleLoaded"), bModuleLoaded);
    Out->SetBoolField(TEXT("available"), bModuleLoaded && LiveCoding->IsEnabledForSession());
    if (bModuleLoaded)
    {
        Out->SetBoolField(TEXT("started"), LiveCoding->HasStarted());
        Out->SetBoolField(TEXT("enabledForSession"), LiveCoding->IsEnabledForSession());
        Out->SetBoolField(TEXT("canEnableForSession"), LiveCoding->CanEnableForSession());
        Out->SetBoolField(TEXT("compiling"), LiveCoding->IsCompiling());
        const FText EnableError = LiveCoding->GetEnableErrorText();
        if (!EnableError.IsEmpty())
        {
            Out->SetStringField(TEXT("enableError"), EnableError.ToString());
        }
    }
    else
    {
        // Compiled in but the module isn't loaded (never started this session).
        Out->SetBoolField(TEXT("started"), false);
        Out->SetBoolField(TEXT("enabledForSession"), false);
        Out->SetBoolField(TEXT("canEnableForSession"), false);
        Out->SetBoolField(TEXT("compiling"), false);
    }
#else
    // Live Coding not compiled into this build (non-Windows / -game / Shipping).
    Out->SetBoolField(TEXT("compiledIntoBuild"), false);
    Out->SetBoolField(TEXT("moduleLoaded"), false);
    Out->SetBoolField(TEXT("available"), false);
    Out->SetBoolField(TEXT("started"), false);
    Out->SetBoolField(TEXT("enabledForSession"), false);
    Out->SetBoolField(TEXT("canEnableForSession"), false);
    Out->SetBoolField(TEXT("compiling"), false);
    Out->SetStringField(TEXT("reason"),
        TEXT("Live Coding is not compiled into this build (requires a Windows x64 editor build with bWithLiveCoding)."));
#endif
    // Durable last-compile result recorded by system.live_coding_compile: survives
    // across requests (and across a Live++ self-patch); "None" until a compile runs.
    Out->SetStringField(TEXT("lastCompileResult"), FPluginState::Get().LiveCodingLastCompileResult());
    Ctx.SendSuccess(Out);
    return true;
}

// ---- system.live_coding_compile ----
REGISTER_RPC_HANDLER("system.live_coding_compile", "system",
    "Trigger the editor's own in-process Live Coding compile (Ctrl+Alt+F11 equivalent), wait for it, and return the patch result + captured LogLiveCoding tail. Unlike system.run_ubt (external UBT child process), this hot-patches the running editor. Requires Live Coding enabled for the session — check system.live_coding_status first.",
    RPC_NO_PARAMS)
{
#if MCP_HAS_LIVE_CODING
    ILiveCodingModule* LiveCoding = GetLiveCodingModule();
    if (!LiveCoding)
    {
        Ctx.SendError(TEXT("LIVE_CODING_NOT_AVAILABLE"),
            TEXT("Live Coding module is not loaded. Enable Live Coding in Editor Preferences and restart the editor session, then retry."));
        return true;
    }
    if (!LiveCoding->IsEnabledForSession())
    {
        TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
        ErrData->SetBoolField(TEXT("canEnableForSession"), LiveCoding->CanEnableForSession());
        const FText EnableError = LiveCoding->GetEnableErrorText();
        if (!EnableError.IsEmpty())
        {
            ErrData->SetStringField(TEXT("enableError"), EnableError.ToString());
        }
        Ctx.SendError(TEXT("LIVE_CODING_NOT_ENABLED"),
            TEXT("Live Coding is not enabled for this session. Enable it in Editor Preferences (Ctrl+Alt+F11), then retry. See system.live_coding_status."),
            ErrData);
        return true;
    }
    if (LiveCoding->IsCompiling())
    {
        Ctx.SendError(TEXT("LIVE_CODING_COMPILE_IN_PROGRESS"),
            TEXT("A Live Coding compile is already in progress. Poll system.live_coding_status until 'compiling' is false, then retry."));
        return true;
    }

    // Capture LogLiveCoding output for the duration of the (synchronous) compile.
    FLiveCodingLogCollector Collector;
    GLog->AddOutputDevice(&Collector);

    ELiveCodingCompileResult Result = ELiveCodingCompileResult::NotStarted;
    LiveCoding->Compile(ELiveCodingCompileFlags::WaitForCompletion, &Result);

    GLog->RemoveOutputDevice(&Collector);

    // Compile() always writes the real outcome into Result (ReturnResults sets the
    // out-param on every return path), so derive the reported result from Result —
    // NOT from Compile()'s bool return, which is false for a compile that ran and then
    // failed or was cancelled (it is true only for Success/NoChanges/InProgress).
    const FString ResultString = LiveCodingResultToString(Result);
    // Record the durable last result for system.live_coding_status readback and for
    // wire-timeout recovery on a long compile.
    FPluginState::Get().LiveCodingLastCompileResult() = ResultString;

    const FString Output = Collector.Consume();

    // "started" = the compile actually began: false only when the Live Coding monitor
    // could not start (NotStarted) or a prior compile was still active. A compile that
    // ran and then failed or was cancelled is still "started" (so this cannot use
    // Compile()'s bool return, which is false for Failure/Cancelled too).
    const bool bStarted = Result != ELiveCodingCompileResult::NotStarted
        && Result != ELiveCodingCompileResult::CompileStillActive;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("result"), ResultString);
    Payload->SetBoolField(TEXT("started"), bStarted);
    if (!Output.IsEmpty())
    {
        Payload->SetStringField(TEXT("output"), Output);
    }

    // A completed compile that applied changes (or had nothing to do) is a success;
    // a real compile failure must NOT fake-success — surface it as an error carrying
    // the same result/output payload (mirrors the "never a bare compiled:true"
    // convention for compile-shaped results).
    const bool bOk = Result == ELiveCodingCompileResult::Success
        || Result == ELiveCodingCompileResult::NoChanges;
    if (bOk)
    {
        Ctx.SendSuccess(Payload);
    }
    else
    {
        FString Code;
        switch (Result)
        {
        case ELiveCodingCompileResult::Failure:            Code = TEXT("LIVE_CODING_COMPILE_FAILED"); break;
        case ELiveCodingCompileResult::CompileStillActive: Code = TEXT("LIVE_CODING_COMPILE_IN_PROGRESS"); break;
        case ELiveCodingCompileResult::Cancelled:          Code = TEXT("LIVE_CODING_COMPILE_CANCELLED"); break;
        case ELiveCodingCompileResult::InProgress:         Code = TEXT("LIVE_CODING_COMPILE_IN_PROGRESS"); break;
        default:                                           Code = TEXT("LIVE_CODING_NOT_STARTED"); break;
        }
        Ctx.SendError(Code,
            FString::Printf(TEXT("Live Coding compile did not succeed (result: %s). See 'output' for compiler diagnostics."), *ResultString),
            Payload);
    }
    return true;
#else
    Ctx.SendError(TEXT("LIVE_CODING_NOT_AVAILABLE"),
        TEXT("Live Coding is not compiled into this build (requires a Windows x64 editor build with bWithLiveCoding). Use system.run_ubt for an external build instead."));
    return true;
#endif
}
