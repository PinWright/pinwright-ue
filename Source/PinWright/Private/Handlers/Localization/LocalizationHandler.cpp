// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Localization/LocalizationCommand.h"
#include "State/PluginState.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "UnrealEdMisc.h"

#if __has_include("LocalizationCommandletExecution.h")
#include "LocalizationCommandletExecution.h"
#define PINWRIGHT_HAS_LOCALIZATION_COMMANDLET_EXECUTION 1
#else
#define PINWRIGHT_HAS_LOCALIZATION_COMMANDLET_EXECUTION 0
#endif

namespace
{
    struct FLocalizationRequest
    {
        FString Target;
        FString ConfigRelativePath;
        FString ConfigFullPath;
    };

    FString MakeLogPath(const FString& Target, const PinWrightLocalization::EOperation Operation)
    {
        const FString Directory = FPaths::ProjectSavedDir() / TEXT("PinWright") / TEXT("Localization");
        IFileManager::Get().MakeDirectory(*Directory, /*Tree=*/true);

        const FString Filename = FString::Printf(TEXT("%s_%s_%s.log"),
            *Target,
            PinWrightLocalization::OperationName(Operation),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        return Directory / Filename;
    }

    FString MakeProjectRelativePath(const FString& FullPath)
    {
        FString Relative = FPaths::ConvertRelativePathToFull(FullPath);
        FPaths::MakePathRelativeTo(Relative, *FPaths::ProjectDir());
        FPaths::NormalizeFilename(Relative);
        return Relative;
    }

    bool ParseRequest(FHandlerContext& Ctx, const PinWrightLocalization::EOperation Operation,
        FLocalizationRequest& Out)
    {
        const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
        if (!Payload.IsValid() || !Payload->HasField(TEXT("target")) ||
            !Payload->HasTypedField<EJson::String>(TEXT("target")))
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("target must be a non-empty string"));
            return false;
        }

        FString Target;
        if (!Ctx.RequireString(TEXT("target"), Target))
        {
            return false;
        }

        FString ValidatedTarget;
        FString ValidationError;
        if (!PinWrightLocalization::ValidateTarget(Target, ValidatedTarget, ValidationError))
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"), ValidationError);
            return false;
        }

        FString ConfigInput;
        if (Payload->HasField(TEXT("config")))
        {
            if (!Payload->HasTypedField<EJson::String>(TEXT("config")))
            {
                Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("config must be a project-relative string path"));
                return false;
            }
            ConfigInput = Ctx.GetString(TEXT("config"));
        }

        if (!PinWrightLocalization::ResolveConfig(
                FPaths::ProjectDir(), ValidatedTarget, Operation, ConfigInput,
                Out.ConfigRelativePath, Out.ConfigFullPath, ValidationError))
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"), ValidationError);
            return false;
        }

        FString ConfigText;
        if (!FFileHelper::LoadFileToString(ConfigText, *Out.ConfigFullPath))
        {
        Ctx.SendError(ErrorCodes::ERR_CONFIG_READ_FAILED,
                FString::Printf(TEXT("Unable to read localization config: %s"), *Out.ConfigRelativePath));
            return false;
        }

        const TCHAR* ExpectedMarker = PinWrightLocalization::ExpectedCommandletClass(Operation);
        switch (PinWrightLocalization::ValidateConfigContents(ConfigText, ValidatedTarget, Operation))
        {
        case PinWrightLocalization::EConfigContentResult::OperationMismatch:
            Ctx.SendError(ErrorCodes::ERR_CONFIG_OPERATION_MISMATCH,
                FString::Printf(TEXT("config does not contain a %s commandlet step"), ExpectedMarker));
            return false;
        case PinWrightLocalization::EConfigContentResult::TargetMismatch:
            Ctx.SendError(ErrorCodes::ERR_CONFIG_TARGET_MISMATCH,
                FString::Printf(TEXT("config manifest does not match localization target '%s'"), *ValidatedTarget));
            return false;
        case PinWrightLocalization::EConfigContentResult::Valid:
            break;
        }

        Out.Target = ValidatedTarget;
        return true;
    }

#if PINWRIGHT_HAS_LOCALIZATION_COMMANDLET_EXECUTION
    class FLocalizationRunState : public TSharedFromThis<FLocalizationRunState>
    {
    public:
        FLocalizationRunState(const FLocalizationRequest& InRequest,
            const PinWrightLocalization::EOperation InOperation,
            const FString& InLogPath)
            : Request(InRequest)
            , Operation(InOperation)
            , LogPath(InLogPath)
            , CommandletExecutable(FUnrealEdMisc::Get().GetExecutableForCommandlets())
        {
        }

        void SetTicketId(const FString& InTicketId)
        {
            TicketId = InTicketId;
        }

        void Start(FJobOnComplete InOnComplete)
        {
            OnComplete = MoveTemp(InOnComplete);
            Process = FLocalizationCommandletProcess::Execute(Request.ConfigFullPath, true);
            if (!Process.IsValid())
            {
                Complete(-1, TEXT("CREATEPROC_FAILED"));
                return;
            }

            CommandletArguments = Process->GetProcessArguments();
            TSharedRef<FLocalizationRunState> Self = AsShared();
            FTSTicker::GetCoreTicker().AddTicker(
                FTickerDelegate::CreateLambda(
                    [Self](float DeltaTime)
                    {
                        return Self->Tick(DeltaTime);
                    }));
        }

        void Cancel()
        {
            bCancelled = true;
            if (Process.IsValid() && Process->GetHandle().IsValid() &&
                FPlatformProcess::IsProcRunning(Process->GetHandle()))
            {
                FPlatformProcess::TerminateProc(Process->GetHandle(), /*KillTree=*/true);
            }
        }

    private:
        void DrainOutput()
        {
            if (!Process.IsValid() || !Process->GetReadPipe())
            {
                return;
            }

            for (;;)
            {
                const FString Chunk = FPlatformProcess::ReadPipe(Process->GetReadPipe());
                if (Chunk.IsEmpty())
                {
                    break;
                }
                Output += Chunk;
            }
        }

        bool Tick(const float DeltaTime)
        {
            if (!Process.IsValid())
            {
                return false;
            }

            DrainOutput();
            ElapsedSeconds += DeltaTime;

            if (bCancelled)
            {
                WriteLog(-1, TEXT("cancelled"));
                Process.Reset();
                return false;
            }

            if (!TicketId.IsEmpty() && ElapsedSeconds - LastProgressSeconds >= 1.0)
            {
                LastProgressSeconds = ElapsedSeconds;
                auto Progress = MakeShared<FJsonObject>();
                Progress->SetNumberField(TEXT("elapsedSeconds"), ElapsedSeconds);
                Progress->SetNumberField(TEXT("outputBytes"), static_cast<double>(Output.Len()));
                FPluginState::Get().GetJobRegistry().RecordProgress(
                    TicketId,
                    FString::Printf(TEXT("Localization %s running"),
                        PinWrightLocalization::OperationName(Operation)),
                    Progress,
                    /*bBypassRateLimit=*/true);
            }

            int32 ReturnCode = 0;
            if (!FPlatformProcess::GetProcReturnCode(Process->GetHandle(), &ReturnCode))
            {
                if (FPlatformProcess::IsProcRunning(Process->GetHandle()))
                {
                    return true;
                }
                ReturnCode = -1;
            }

            DrainOutput();
            Complete(ReturnCode, FString());
            return false;
        }

        void WriteLog(const int32 ExitCode, const FString& State)
        {
            FString LogText = FString::Printf(
                TEXT("PinWright localization.%s\nTarget: %s\nConfig: %s\nCommandletArguments: %s\nExitCode: %d\nState: %s\n\n"),
                PinWrightLocalization::OperationName(Operation),
                *Request.Target,
                *Request.ConfigRelativePath,
                *CommandletArguments,
                ExitCode,
                *State);
            LogText += Output;
            bLogWritten = FFileHelper::SaveStringToFile(
                LogText, *LogPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        }

        void Complete(const int32 ExitCode, const FString& LaunchError)
        {
            if (bCompleted)
            {
                return;
            }
            bCompleted = true;

            DrainOutput();
            const bool bSuccess = LaunchError.IsEmpty() && ExitCode == 0;
            WriteLog(ExitCode, bSuccess ? TEXT("completed") : TEXT("failed"));

            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("operation"), PinWrightLocalization::OperationName(Operation));
            Result->SetStringField(TEXT("target"), Request.Target);
            Result->SetStringField(TEXT("config"), Request.ConfigRelativePath);
            Result->SetStringField(TEXT("configFullPath"), Request.ConfigFullPath);
            Result->SetStringField(TEXT("logPath"), MakeProjectRelativePath(LogPath));
            Result->SetStringField(TEXT("commandletExecutable"), CommandletExecutable);
            Result->SetStringField(TEXT("commandletArguments"), CommandletArguments);
            Result->SetNumberField(TEXT("exitCode"), ExitCode);
            Result->SetNumberField(TEXT("outputBytes"), static_cast<double>(Output.Len()));
            Result->SetBoolField(TEXT("logWritten"), bLogWritten);
            Result->SetBoolField(TEXT("success"), bSuccess);
            Result->SetStringField(TEXT("outputTail"), Output.Right(8192));

            FString Error = LaunchError;
            if (Error.IsEmpty() && !bSuccess)
            {
                Error = FString::Printf(TEXT("LOCALIZATION_COMMANDLET_FAILED (exit code %d)"), ExitCode);
            }

            FJobOnComplete Completion = MoveTemp(OnComplete);
            Process.Reset();
            Completion(bSuccess, Result, Error);
        }

        FLocalizationRequest Request;
        PinWrightLocalization::EOperation Operation;
        FString LogPath;
        FString TicketId;
        FString CommandletExecutable;
        FString CommandletArguments;
        FString Output;
        FJobOnComplete OnComplete;
        TSharedPtr<FLocalizationCommandletProcess> Process;
        double ElapsedSeconds = 0.0;
        double LastProgressSeconds = 0.0;
        bool bCancelled = false;
        bool bCompleted = false;
        bool bLogWritten = false;
    };
#endif

    bool RunLocalization(FHandlerContext& Ctx, const PinWrightLocalization::EOperation Operation)
    {
        FLocalizationRequest Request;
        if (!ParseRequest(Ctx, Operation, Request))
        {
            return true;
        }

#if !PINWRIGHT_HAS_LOCALIZATION_COMMANDLET_EXECUTION
        Ctx.SendError(TEXT("NOT_SUPPORTED"),
            TEXT("LocalizationCommandletExecution is unavailable in this Unreal Engine build"));
        return true;
#else
        const FString LocalizationLogPath = MakeLogPath(Request.Target, Operation);
        TSharedRef<FLocalizationRunState> State = MakeShared<FLocalizationRunState>(
            Request, Operation, LocalizationLogPath);

        FJobBindArgs Args;
        Args.Method = Operation == PinWrightLocalization::EOperation::Gather
            ? TEXT("localization.gather")
            : TEXT("localization.compile");
        Args.StartedPayload = MakeShared<FJsonObject>();
        Args.StartedPayload->SetStringField(TEXT("operation"), PinWrightLocalization::OperationName(Operation));
        Args.StartedPayload->SetStringField(TEXT("target"), Request.Target);
        Args.StartedPayload->SetStringField(TEXT("config"), Request.ConfigRelativePath);
        Args.StartedPayload->SetStringField(TEXT("configFullPath"), Request.ConfigFullPath);
        Args.StartedPayload->SetStringField(TEXT("logPath"), MakeProjectRelativePath(LocalizationLogPath));
        Args.StartedPayload->SetStringField(TEXT("commandletExecutable"),
            FUnrealEdMisc::Get().GetExecutableForCommandlets());
        Args.BindNativeDelegate = [State](FJobOnComplete OnComplete)
        {
            State->Start(MoveTemp(OnComplete));
        };

        const FString TicketId = Ctx.StartJob(Args);
        State->SetTicketId(TicketId);
        TWeakPtr<FLocalizationRunState> WeakState(State);
        FPluginState::Get().GetJobRegistry().SetCancelCallback(
            TicketId,
            [WeakState]()
            {
                if (const TSharedPtr<FLocalizationRunState> Pinned = WeakState.Pin())
                {
                    Pinned->Cancel();
                }
            });
        return true;
#endif
    }
}

REGISTER_RPC_HANDLER("localization.gather", "localization",
    "Run the named project's Unreal localization Gather config as a tracked commandlet job. The config is constrained to Config/Localization and no arbitrary command line is accepted.",
    RPC_PARAMS(
        RPC_PARAM_REQ("target", "string", "Localization target name (for example, Game)."),
        RPC_PARAM_OPT("config", "filepath", "Project-relative Config/Localization/*.ini path. Defaults to <target>_Gather.ini.")))
{
    return RunLocalization(Ctx, PinWrightLocalization::EOperation::Gather);
}

REGISTER_RPC_HANDLER("localization.compile", "localization",
    "Run the named project's Unreal localization Compile config as a tracked commandlet job. The config is constrained to Config/Localization and no arbitrary command line is accepted.",
    RPC_PARAMS(
        RPC_PARAM_REQ("target", "string", "Localization target name (for example, Game)."),
        RPC_PARAM_OPT("config", "filepath", "Project-relative Config/Localization/*.ini path. Defaults to <target>_Compile.ini.")))
{
    return RunLocalization(Ctx, PinWrightLocalization::EOperation::Compile);
}
