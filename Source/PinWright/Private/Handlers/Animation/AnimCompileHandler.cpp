// Copyright (c) 2026 Alexander Penkin. MIT License.

// AnimCompileHandler.cpp - the source-file RPC surface for .pwanim.
//
// The parser/compiler stay free of JSON and FHandlerContext. This file owns the file-path
// policy, the response shape and the diagnostic presentation, matching the model format's
// compile/validate/describe_ops loop without sharing either format's vocabulary.
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Utils/AssetUtils.h"
#include "Utils/JsonUtils.h"

#include "PwAnim/PwAnimCompiler.h"
#include "PwAnim/PwAnimDiagnostic.h"
#include "PwAnim/PwAnimParser.h"
#include "PwSource/PwParamSpec.h"
#include "PwSource/PwSourcePathUtils.h"
#include "PwSource/PwSuggest.h"

#include "Containers/StringView.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
bool AnimHandler_HasField(const FHandlerContext& Ctx, const TCHAR* Key)
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    return Payload.IsValid() && Payload->HasField(Key);
}

FString AnimHandler_ResolveSourcePath(const FString& RawPath)
{
    FString Resolved = RawPath;
    if (FPaths::IsRelative(Resolved))
    {
        Resolved = FPaths::Combine(FPaths::ProjectDir(), Resolved);
    }
    Resolved = FPaths::ConvertRelativePathToFull(Resolved);
    FPaths::NormalizeFilename(Resolved);
    return Resolved;
}

FString AnimHandler_ProvenancePath(const FString& ResolvedPath)
{
    if (ResolvedPath.IsEmpty())
    {
        return ResolvedPath;
    }

    const FString ProjectDir = FPaths::ProjectDir();
    if (!FPaths::IsUnderDirectory(ResolvedPath, ProjectDir))
    {
        return ResolvedPath;
    }

    FString Relative = ResolvedPath;
    return FPaths::MakePathRelativeTo(Relative, *ProjectDir) ? Relative : ResolvedPath;
}

bool AnimHandler_LoadSourceFile(FHandlerContext& Ctx, const FString& RawPath,
                                FString& OutSource, FString& OutResolvedPath)
{
    OutResolvedPath = AnimHandler_ResolveSourcePath(RawPath);
    if (!FPaths::FileExists(OutResolvedPath))
    {
        Ctx.SendError(ErrorCodes::ERR_ANIM_FILE_NOT_FOUND,
            FString::Printf(TEXT("No .pwanim source at '%s' (resolved to '%s')."),
                *RawPath, *OutResolvedPath));
        return false;
    }

    if (!FFileHelper::LoadFileToString(OutSource, *OutResolvedPath))
    {
        Ctx.SendError(ErrorCodes::ERR_ANIM_INVALID_SOURCE,
            FString::Printf(TEXT("Could not read '%s' as text. .pwanim sources are UTF-8 text files."),
                *OutResolvedPath));
        return false;
    }
    return true;
}

const TCHAR* AnimHandler_SeverityToString(EPwSeverity Severity)
{
    return Severity == EPwSeverity::Warning ? TEXT("warning") : TEXT("error");
}

constexpr int32 AnimHandler_DefaultDiagnosticLimit = 5;
constexpr int32 AnimHandler_MaxOccurrenceSites = 12;

struct FAnimHandler_DiagnosticView
{
    int32 Limit = AnimHandler_DefaultDiagnosticLimit;
    bool bCollapse = true;
    TOptional<EPwSeverity> Severity;
    FString SeverityFilter = TEXT("all");
};

bool AnimHandler_ReadDiagnosticView(FHandlerContext& Ctx, FAnimHandler_DiagnosticView& OutView)
{
    OutView.Limit = Ctx.GetInt(TEXT("diagnosticLimit"), AnimHandler_DefaultDiagnosticLimit);
    if (OutView.Limit < 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS, FString::Printf(
            TEXT("'diagnosticLimit' must be 0 or greater; got %d. 0 means every diagnostic."),
            OutView.Limit));
        return false;
    }

    OutView.bCollapse = Ctx.GetBool(TEXT("collapseDiagnostics"), true);
    const FString Requested = Ctx.GetString(TEXT("diagnosticSeverity"), TEXT("all"));
    if (Requested.Equals(TEXT("all"), ESearchCase::IgnoreCase))
    {
        OutView.Severity.Reset();
    }
    else if (Requested.Equals(TEXT("error"), ESearchCase::IgnoreCase))
    {
        OutView.Severity = EPwSeverity::Error;
    }
    else if (Requested.Equals(TEXT("warning"), ESearchCase::IgnoreCase))
    {
        OutView.Severity = EPwSeverity::Warning;
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS, FString::Printf(
            TEXT("'diagnosticSeverity' must be 'all', 'error' or 'warning'; got '%s'."),
            *Requested));
        return false;
    }
    OutView.SeverityFilter = Requested.ToLower();
    return true;
}

struct FAnimHandler_DiagnosticGroup
{
    const FPwDiagnostic* First = nullptr;
    int32 Count = 0;
    TArray<const FPwDiagnostic*> Sites;
    int32 SitesOmitted = 0;
    TSet<FString> Messages;
};

FString AnimHandler_DiagnosticGroupKey(const FPwDiagnostic& Diagnostic)
{
    return FString::Printf(TEXT("%s|%s|%s"), AnimHandler_SeverityToString(Diagnostic.Severity),
        *Diagnostic.Code, *Diagnostic.ScopeName);
}

void AnimHandler_AddDiagnostics(const TSharedPtr<FJsonObject>& Result,
                                const TArray<FPwDiagnostic>& Diagnostics,
                                const FAnimHandler_DiagnosticView& View)
{
    int32 ErrorCount = 0;
    int32 WarningCount = 0;
    for (const FPwDiagnostic& Diagnostic : Diagnostics)
    {
        if (Diagnostic.Severity == EPwSeverity::Error)
        {
            ++ErrorCount;
        }
        else
        {
            ++WarningCount;
        }
    }

    TArray<const FPwDiagnostic*> Kept;
    Kept.Reserve(Diagnostics.Num());
    for (const FPwDiagnostic& Diagnostic : Diagnostics)
    {
        if (!View.Severity.IsSet() || Diagnostic.Severity == View.Severity.GetValue())
        {
            Kept.Add(&Diagnostic);
        }
    }
    const int32 SuppressedBySeverity = Diagnostics.Num() - Kept.Num();

    TArray<FAnimHandler_DiagnosticGroup> Groups;
    TMap<FString, int32> GroupIndexByKey;
    Groups.Reserve(Kept.Num());
    for (const FPwDiagnostic* Diagnostic : Kept)
    {
        int32 GroupIndex = INDEX_NONE;
        if (View.bCollapse)
        {
            const FString Key = AnimHandler_DiagnosticGroupKey(*Diagnostic);
            if (const int32* Existing = GroupIndexByKey.Find(Key))
            {
                GroupIndex = *Existing;
            }
            else
            {
                GroupIndex = Groups.AddDefaulted();
                GroupIndexByKey.Add(Key, GroupIndex);
            }
        }
        else
        {
            GroupIndex = Groups.AddDefaulted();
        }

        FAnimHandler_DiagnosticGroup& Group = Groups[GroupIndex];
        if (Group.Count == 0)
        {
            Group.First = Diagnostic;
        }
        ++Group.Count;
        Group.Messages.Add(Diagnostic->Message);
        if (Group.Sites.Num() < AnimHandler_MaxOccurrenceSites)
        {
            Group.Sites.Add(Diagnostic);
        }
        else
        {
            ++Group.SitesOmitted;
        }
    }

    const int32 Limit = View.Limit <= 0 ? Groups.Num() : FMath::Min(View.Limit, Groups.Num());
    TArray<int32> Selected;
    Selected.Reserve(Limit);
    for (int32 Pass = 0; Pass < 2 && Selected.Num() < Limit; ++Pass)
    {
        const EPwSeverity Wanted = Pass == 0 ? EPwSeverity::Error : EPwSeverity::Warning;
        for (int32 Index = 0; Index < Groups.Num() && Selected.Num() < Limit; ++Index)
        {
            if (Groups[Index].First->Severity == Wanted)
            {
                Selected.Add(Index);
            }
        }
    }
    Selected.Sort();

    int32 Represented = 0;
    TArray<TSharedPtr<FJsonValue>> Values;
    Values.Reserve(Selected.Num());
    for (const int32 GroupIndex : Selected)
    {
        const FAnimHandler_DiagnosticGroup& Group = Groups[GroupIndex];
        Represented += Group.Count;
        const FPwDiagnostic& Diagnostic = *Group.First;

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("severity"), AnimHandler_SeverityToString(Diagnostic.Severity));
        Entry->SetNumberField(TEXT("line"), Diagnostic.Line);
        Entry->SetNumberField(TEXT("column"), Diagnostic.Column);
        Entry->SetStringField(TEXT("code"), Diagnostic.Code);
        Entry->SetStringField(TEXT("message"), Diagnostic.Message);
        Entry->SetStringField(TEXT("bone"), Diagnostic.ScopeName);
        Entry->SetArrayField(TEXT("suggestions"), EmitStringArray(Diagnostic.Suggestions));

        if (Group.Count > 1)
        {
            Entry->SetNumberField(TEXT("occurrences"), Group.Count);
            TArray<TSharedPtr<FJsonValue>> Sites;
            Sites.Reserve(Group.Sites.Num());
            for (const FPwDiagnostic* Site : Group.Sites)
            {
                TSharedPtr<FJsonObject> SiteJson = MakeShared<FJsonObject>();
                SiteJson->SetNumberField(TEXT("line"), Site->Line);
                SiteJson->SetNumberField(TEXT("column"), Site->Column);
                Sites.Add(MakeShared<FJsonValueObject>(SiteJson));
            }
            Entry->SetArrayField(TEXT("occurrenceSites"), Sites);
            if (Group.SitesOmitted > 0)
            {
                Entry->SetNumberField(TEXT("occurrenceSitesOmitted"), Group.SitesOmitted);
            }
            if (Group.Messages.Num() > 1)
            {
                Entry->SetNumberField(TEXT("distinctMessages"), Group.Messages.Num());
            }
        }
        Values.Add(MakeShared<FJsonValueObject>(Entry));
    }

    Result->SetArrayField(TEXT("diagnostics"), Values);
    TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
    Summary->SetNumberField(TEXT("total"), Diagnostics.Num());
    Summary->SetNumberField(TEXT("errors"), ErrorCount);
    Summary->SetNumberField(TEXT("warnings"), WarningCount);
    Summary->SetNumberField(TEXT("emitted"), Values.Num());
    Summary->SetNumberField(TEXT("represented"), Represented);
    Summary->SetNumberField(TEXT("collapsedOccurrences"), Represented - Values.Num());
    Summary->SetNumberField(TEXT("suppressedBySeverity"), SuppressedBySeverity);
    Summary->SetNumberField(TEXT("suppressedByLimit"), Kept.Num() - Represented);
    Summary->SetBoolField(TEXT("complete"), Values.Num() == Diagnostics.Num());
    Summary->SetNumberField(TEXT("limit"), View.Limit);
    Summary->SetBoolField(TEXT("collapse"), View.bCollapse);
    Summary->SetStringField(TEXT("severityFilter"), View.SeverityFilter);
    Result->SetObjectField(TEXT("diagnosticSummary"), Summary);
}

TSharedPtr<FJsonObject> AnimHandler_FrameRateToJson(const FFrameRate& Rate)
{
    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetNumberField(TEXT("numerator"), Rate.Numerator);
    Json->SetNumberField(TEXT("denominator"), Rate.Denominator);
    return Json;
}

TSharedPtr<FJsonObject> AnimHandler_ResultToJson(
    const FPwAnimCompileResult& CompileResult,
    const FAnimHandler_DiagnosticView& View)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("version"), CompileResult.Version);
    Result->SetBoolField(TEXT("success"), CompileResult.bSuccess);
    Result->SetStringField(TEXT("assetPath"), CompileResult.AssetPath);
    Result->SetStringField(TEXT("skeletonPath"), CompileResult.SkeletonPath);
    if (CompileResult.SkeletonBoneCount >= 0)
    {
        Result->SetNumberField(TEXT("skeletonBoneCount"), CompileResult.SkeletonBoneCount);
    }

    Result->SetObjectField(TEXT("frameRate"),
        AnimHandler_FrameRateToJson(CompileResult.RequestedFrameRate));
    Result->SetNumberField(TEXT("frames"), CompileResult.RequestedNumberOfFrames);
    Result->SetNumberField(TEXT("keysPerTrack"), CompileResult.RequestedNumberOfKeys);
    Result->SetNumberField(TEXT("durationSeconds"), CompileResult.RequestedDurationSeconds);
    Result->SetBoolField(TEXT("loop"), CompileResult.bLoop);

    if (CompileResult.AssetNumberOfFrames >= 0)
    {
        Result->SetObjectField(TEXT("assetFrameRate"),
            AnimHandler_FrameRateToJson(CompileResult.AssetFrameRate));
        Result->SetNumberField(TEXT("assetNumberOfFrames"), CompileResult.AssetNumberOfFrames);
        Result->SetNumberField(TEXT("assetNumberOfKeys"), CompileResult.AssetNumberOfKeys);
        Result->SetNumberField(TEXT("assetTrackCount"), CompileResult.AssetTrackCount);
        Result->SetArrayField(TEXT("assetKeyCountPerTrack"),
            [&CompileResult]()
            {
                TArray<TSharedPtr<FJsonValue>> Values;
                Values.Reserve(CompileResult.AssetKeyCountPerTrack.Num());
                for (const int32 Count : CompileResult.AssetKeyCountPerTrack)
                {
                    Values.Add(MakeShared<FJsonValueNumber>(Count));
                }
                return Values;
            }());
        Result->SetArrayField(TEXT("syncMarkers"),
            [&CompileResult]()
            {
                TArray<TSharedPtr<FJsonValue>> Values;
                Values.Reserve(CompileResult.AssetSyncMarkers.Num());
                for (const FPwSyncMarkerSpec& Marker : CompileResult.AssetSyncMarkers)
                {
                    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
                    Json->SetStringField(TEXT("name"), Marker.MarkerName.ToString());
                    Json->SetNumberField(TEXT("time"), Marker.Time);
                    Values.Add(MakeShared<FJsonValueObject>(Json));
                }
                return Values;
            }());
    }

    Result->SetBoolField(TEXT("savedToDisk"), CompileResult.bSavedToDisk);
    if (!CompileResult.ErrorCode.IsEmpty())
    {
        Result->SetStringField(TEXT("errorCode"), CompileResult.ErrorCode);
    }
    if (!CompileResult.ErrorMessage.IsEmpty())
    {
        Result->SetStringField(TEXT("errorMessage"), CompileResult.ErrorMessage);
    }
    AnimHandler_AddDiagnostics(Result, CompileResult.Diagnostics, View);
    return Result;
}

const TCHAR* AnimHandler_FailureCode(FStringView Source)
{
    FPwAnimDocument Document;
    TArray<FPwDiagnostic> ParseDiagnostics;
    return FPwAnimParser::Parse(Source, Document, ParseDiagnostics)
        ? ErrorCodes::ERR_ANIM_COMPILE_FAILED
        : ErrorCodes::ERR_ANIM_PARSE_FAILED;
}

FString AnimHandler_FailureMessage(const FPwAnimCompileResult& CompileResult)
{
    TArray<FString> Errors;
    for (const FPwDiagnostic& Diagnostic : CompileResult.Diagnostics)
    {
        if (Diagnostic.Severity == EPwSeverity::Error)
        {
            Errors.Add(Diagnostic.ToString());
        }
    }

    if (Errors.Num() == 0)
    {
        return CompileResult.ErrorMessage.IsEmpty()
            ? TEXT("Compilation failed without reporting a diagnostic.")
            : CompileResult.ErrorMessage;
    }
    if (Errors.Num() == 1)
    {
        return CompileResult.ErrorMessage.IsEmpty()
            ? Errors[0]
            : FString::Printf(TEXT("%s: %s"), *Errors[0], *CompileResult.ErrorMessage);
    }
    return FString::Printf(TEXT("%s (and %d more)"), *Errors[0], Errors.Num() - 1);
}

void AnimHandler_SendOutcome(FHandlerContext& Ctx, FStringView Source,
                             const FPwAnimCompileResult& CompileResult,
                             const FString& ResolvedPath,
                             const FAnimHandler_DiagnosticView& View,
                             const TOptional<bool>& SaveRequested = TOptional<bool>())
{
    TSharedPtr<FJsonObject> Result = AnimHandler_ResultToJson(CompileResult, View);
    Result->SetStringField(TEXT("sourcePath"), ResolvedPath);

    if (!CompileResult.bSuccess)
    {
        Ctx.SendError(AnimHandler_FailureCode(Source),
            AnimHandler_FailureMessage(CompileResult), Result);
        return;
    }

    if (SaveRequested.IsSet())
    {
        AddAssetSaveReport(Result, SaveRequested.GetValue(), CompileResult.bSavedToDisk,
            CompileResult.SaveState);
    }
    Ctx.SendSuccess(Result);
}

TSharedPtr<FJsonObject> AnimHandler_ParamSpecToJson(const FPwParamSpec& Spec)
{
    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(TEXT("name"), Spec.Name);
    Json->SetStringField(TEXT("type"), PwParamTypeToString(Spec.Type));
    Json->SetBoolField(TEXT("required"), Spec.bRequired);
    Json->SetStringField(TEXT("default"), Spec.Default);
    Json->SetStringField(TEXT("description"), Spec.Description);
    Json->SetArrayField(TEXT("allowedValues"), EmitStringArray(Spec.AllowedValues));
    if (Spec.bHasRange)
    {
        Json->SetNumberField(TEXT("min"), Spec.MinValue);
        Json->SetNumberField(TEXT("max"), Spec.MaxValue);
    }
    return Json;
}

TSharedPtr<FJsonObject> AnimHandler_OpSpecToJson(const FPwAnimOpSpec& Spec)
{
    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(TEXT("name"), Spec.Name);
    Json->SetStringField(TEXT("context"), TEXT("bone"));
    Json->SetStringField(TEXT("description"), Spec.Description);
    Json->SetBoolField(TEXT("acceptsBlock"), Spec.bAcceptsBlock);
    Json->SetBoolField(TEXT("requiresBlock"), Spec.bRequiresBlock);

    TArray<TSharedPtr<FJsonValue>> Params;
    Params.Reserve(Spec.Params.Num());
    for (const FPwParamSpec& Param : Spec.Params)
    {
        Params.Add(MakeShared<FJsonValueObject>(AnimHandler_ParamSpecToJson(Param)));
    }
    Json->SetArrayField(TEXT("params"), Params);
    return Json;
}

struct FAnimHandler_ParamSetSpec
{
    const TCHAR* Name;
    const TCHAR* Context;
    const TCHAR* Description;
    TArrayView<const FPwParamSpec> (*Params)();
};

TArray<FAnimHandler_ParamSetSpec> AnimHandler_ParamSets()
{
    return {
        { TEXT("timebase"), TEXT("document"),
          TEXT("The document-level timebase. Rate is a positive rational frame rate and frames "
               "is the inclusive final frame."),
          &PwAnimOpTable::TimebaseParams },
        { TEXT("bone"), TEXT("bone_header"),
          TEXT("The bone header's default easing policy. Bone names are String literals and key "
               "statements live inside this block."),
          &PwAnimOpTable::BoneHeaderParams },
        { TEXT("sync_marker"), TEXT("document"),
          TEXT("One timeline marker statement. The quoted name may repeat at different frames; "
               "frame is an integer in the inclusive timebase range."),
          &PwAnimOpTable::SyncMarkerParams },
    };
}

TSharedPtr<FJsonObject> AnimHandler_ParamSetToJson(const FAnimHandler_ParamSetSpec& Set)
{
    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(TEXT("name"), Set.Name);
    Json->SetStringField(TEXT("context"), Set.Context);
    Json->SetStringField(TEXT("description"), Set.Description);

    TArray<TSharedPtr<FJsonValue>> Params;
    for (const FPwParamSpec& Param : Set.Params())
    {
        Params.Add(MakeShared<FJsonValueObject>(AnimHandler_ParamSpecToJson(Param)));
    }
    Json->SetArrayField(TEXT("params"), Params);
    return Json;
}
}

// ============================================================================
// anim.compile
// ============================================================================
REGISTER_RPC_HANDLER("anim.compile", "anim",
    "Compile a .pwanim source file into exactly one UAnimSequence.",
    RPC_PARAMS(
        RPC_PARAM_REQ("filePath", "filepath",
            "Filesystem path to the .pwanim source; a relative path resolves against the project directory. "
            "Inline text is refused so the generated asset always has recoverable provenance."),
        RPC_PARAM_OPT("outputPath", "path",
            "Destination /Game/... asset path. Omit it only when filePath is a .pwanim directly below the "
            "project Content directory; the target then uses the same relative path and basename under /Game. "
            "Sources elsewhere require this explicit argument."),
        RPC_PARAM_DEF("overwrite", "boolean",
            "Permission to take over an unstamped or differently generated asset, or to discard live state named by PWSRC_RECOMPILE_UNMANAGED_STATE. Ordinary same-source iteration does not need it.", "false"),
        RPC_PARAM_DEF("save", "boolean", "Save the generated asset to disk.", "true"),
        RPC_PARAM_OPT("text", "string",
            "NOT ACCEPTED on anim.compile. Use anim.validate to iterate on inline text, then write the file."),
        RPC_PARAM_DEF("diagnosticLimit", "integer",
            "Maximum number of diagnostic entries to print; 0 means every entry.", "5"),
        RPC_PARAM_DEF("diagnosticSeverity", "string",
            "Print only 'error', 'warning', or 'all'; filtered diagnostics remain counted in the summary.", "all"),
        RPC_PARAM_DEF("collapseDiagnostics", "boolean",
            "Fold repeated diagnostics by severity, code and bone into one bounded entry.", "true")
    ))
{
    if (AnimHandler_HasField(Ctx, TEXT("text")))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            TEXT("anim.compile does not accept inline 'text'. Use anim.validate to iterate on a string, "
                 "then write the document to a .pwanim file and pass filePath."));
        return true;
    }

    FString FilePath;
    if (!Ctx.RequireString(TEXT("filePath"), FilePath))
    {
        return true;
    }

    const FString ResolvedForOutput = AnimHandler_ResolveSourcePath(FilePath);
    FString OutputPath;
    if (AnimHandler_HasField(Ctx, TEXT("outputPath")))
    {
        if (!Ctx.RequireAssetPath(TEXT("outputPath"), OutputPath))
        {
            return true;
        }
    }
    else
    {
        FString Reason;
        if (!PinWrightPwSourcePaths::TryDeriveOutputAssetPath(
                ResolvedForOutput, TEXT(".pwanim"), OutputPath, Reason))
        {
            Ctx.SendError(ErrorCodes::ERR_SOURCE_OUTPUT_PATH_NOT_DERIVABLE,
                FString::Printf(
                    TEXT("Cannot derive 'outputPath' from source '%s': %s. Pass explicit "
                         "outputPath='/Game/.../AssetName'. No default path was chosen."),
                    *ResolvedForOutput, *Reason));
            return true;
        }
    }

    FAnimHandler_DiagnosticView View;
    if (!AnimHandler_ReadDiagnosticView(Ctx, View))
    {
        return true;
    }

    FString Source;
    FString ResolvedPath;
    if (!AnimHandler_LoadSourceFile(Ctx, FilePath, Source, ResolvedPath))
    {
        return true;
    }

    FPwAnimCompileOptions Options;
    Options.OutputAssetPath = OutputPath;
    Options.SourcePath = AnimHandler_ProvenancePath(ResolvedPath);
    Options.bOverwrite = Ctx.GetBool(TEXT("overwrite"), false);
    Options.bSave = Ctx.GetBool(TEXT("save"), true);
    Options.bValidateOnly = false;

    const FPwAnimCompileResult CompileResult =
        FPwAnimCompiler::Compile(FStringView(Source), Options);
    AnimHandler_SendOutcome(Ctx, FStringView(Source), CompileResult, ResolvedPath, View,
        Options.bSave);
    return true;
}

// ============================================================================
// anim.validate
// ============================================================================
REGISTER_RPC_HANDLER("anim.validate", "anim",
    "Parse and compile a .pwanim document without writing an asset.",
    RPC_PARAMS(
        RPC_PARAM_OPT("text", "string", "Inline .pwanim source. Supply exactly one of text or filePath."),
        RPC_PARAM_OPT("filePath", "filepath",
            "Filesystem path to a .pwanim source; a relative path resolves against the project directory. "
            "Supply exactly one of text or filePath."),
        RPC_PARAM_DEF("diagnosticLimit", "integer",
            "Maximum number of diagnostic entries to print; 0 means every entry.", "5"),
        RPC_PARAM_DEF("diagnosticSeverity", "string",
            "Print only 'error', 'warning', or 'all'; filtered diagnostics remain counted in the summary.", "all"),
        RPC_PARAM_DEF("collapseDiagnostics", "boolean",
            "Fold repeated diagnostics by severity, code and bone into one bounded entry.", "true")
    ))
{
    const bool bHasText = AnimHandler_HasField(Ctx, TEXT("text"));
    const bool bHasFilePath = AnimHandler_HasField(Ctx, TEXT("filePath"));
    if (bHasText == bHasFilePath)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            bHasText
                ? TEXT("anim.validate takes exactly one of 'text' or 'filePath'; both were supplied.")
                : TEXT("anim.validate takes exactly one of 'text' or 'filePath'; neither was supplied."));
        return true;
    }

    FAnimHandler_DiagnosticView View;
    if (!AnimHandler_ReadDiagnosticView(Ctx, View))
    {
        return true;
    }

    FString Source;
    FString ResolvedPath;
    if (bHasFilePath)
    {
        FString FilePath;
        if (!Ctx.RequireString(TEXT("filePath"), FilePath)
            || !AnimHandler_LoadSourceFile(Ctx, FilePath, Source, ResolvedPath))
        {
            return true;
        }
    }
    else
    {
        Source = Ctx.GetString(TEXT("text"));
        if (Source.TrimStartAndEnd().IsEmpty())
        {
            Ctx.SendError(ErrorCodes::ERR_ANIM_INVALID_SOURCE,
                TEXT("'text' is empty. A .pwanim document starts with the version header 'pwanim 0'."));
            return true;
        }
    }

    FPwAnimCompileOptions Options;
    Options.SourcePath = AnimHandler_ProvenancePath(ResolvedPath);
    Options.bOverwrite = false;
    Options.bSave = false;
    Options.bValidateOnly = true;

    const FPwAnimCompileResult CompileResult =
        FPwAnimCompiler::Compile(FStringView(Source), Options);
    AnimHandler_SendOutcome(Ctx, FStringView(Source), CompileResult, ResolvedPath, View);
    return true;
}

// ============================================================================
// anim.describe_ops
// ============================================================================
REGISTER_RPC_HANDLER("anim.describe_ops", "anim",
    "Emit the .pwanim key vocabulary and the timebase, bone-header and sync-marker parameter sets.",
    RPC_PARAMS(
        RPC_PARAM_OPT("op", "string",
            "Narrow to one key op or parameter-set name. Omitted, the whole animation vocabulary "
            "is returned.")
    ))
{
    const FString Requested = Ctx.GetString(TEXT("op"));

    TArray<TSharedPtr<FJsonValue>> OpValues;
    for (const FPwAnimOpSpec& Spec : PwAnimOpTable::Get())
    {
        if (!Requested.IsEmpty() && Spec.Name != Requested)
        {
            continue;
        }
        OpValues.Add(MakeShared<FJsonValueObject>(AnimHandler_OpSpecToJson(Spec)));
    }

    const TArray<FAnimHandler_ParamSetSpec> ParamSets = AnimHandler_ParamSets();
    TArray<TSharedPtr<FJsonValue>> ParamSetValues;
    for (const FAnimHandler_ParamSetSpec& Set : ParamSets)
    {
        if (!Requested.IsEmpty() && Requested != Set.Name)
        {
            continue;
        }
        ParamSetValues.Add(MakeShared<FJsonValueObject>(AnimHandler_ParamSetToJson(Set)));
    }

    if (OpValues.Num() == 0 && ParamSetValues.Num() == 0)
    {
        TArray<FString> AllNames = PwAnimOpTable::Names();
        for (const FAnimHandler_ParamSetSpec& Set : ParamSets)
        {
            AllNames.AddUnique(Set.Name);
        }

        const FString Guess = PwSuggest::Closest(Requested,
            TArrayView<const FString>(AllNames));
        Ctx.SendError(ErrorCodes::ERR_UNKNOWN_OPERATION,
            Guess.IsEmpty()
                ? FString::Printf(TEXT("No .pwanim operation or parameter set named '%s'. "
                                       "Call anim.describe_ops with no 'op' for the vocabulary."),
                                   *Requested)
                : FString::Printf(TEXT("No .pwanim operation or parameter set named '%s'. "
                                       "Did you mean '%s'?"), *Requested, *Guess));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("opCount"), OpValues.Num());
    Result->SetArrayField(TEXT("ops"), OpValues);
    Result->SetNumberField(TEXT("paramSetCount"), ParamSetValues.Num());
    Result->SetArrayField(TEXT("paramSets"), ParamSetValues);
    Ctx.SendSuccess(Result);
    return true;
}
