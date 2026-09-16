// Copyright (c) 2026 Alexander Penkin. MIT License.

// SkeletonCompileHandler.cpp - the source-format RPC surface for .pwskel.
//
// This file deliberately contains no parser or asset-construction logic.  The parser owns
// source diagnostics and the creation seam owns UObject/provenance/save policy; the handler only
// reads a source file, marshals the result, and exposes the three skeleton.* verbs.
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "PwSkel/PwSkelAssetCreate.h"
#include "PwSkel/PwSkelParser.h"
#include "PwSource/PwDiagnostic.h"
#include "PwSource/PwParamSpec.h"
#include "PwSource/PwSourcePathUtils.h"
#include "PwSource/PwSuggest.h"
#include "Utils/AssetUtils.h"
#include "Utils/JsonUtils.h"

#include "Containers/StringView.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

namespace PinWrightSkeletonHandlers
{
    bool HasField(const FHandlerContext& Ctx, const TCHAR* Key)
    {
        const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
        return Payload.IsValid() && Payload->HasField(Key);
    }

    FString ResolveSourcePath(const FString& RawPath)
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

    FString ProvenancePath(const FString& ResolvedPath)
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

    FString HashSource(FStringView Source)
    {
        FMD5 Md5;
        if (Source.Len() > 0)
        {
            const FTCHARToUTF8 Utf8(Source.GetData(), Source.Len());
            Md5.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
        }

        uint8 Digest[16] = {};
        Md5.Final(Digest);
        return BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
    }
    bool LoadSourceFile(FHandlerContext& Ctx, const FString& RawPath,
                        FString& OutSource, FString& OutResolvedPath)
    {
        OutResolvedPath = ResolveSourcePath(RawPath);
        if (!FPaths::FileExists(OutResolvedPath))
        {
            Ctx.SendError(ErrorCodes::ERR_FILE_NOT_FOUND,
                FString::Printf(TEXT("No .pwskel source at '%s' (resolved to '%s')."),
                    *RawPath, *OutResolvedPath));
            return false;
        }

        if (!FFileHelper::LoadFileToString(OutSource, *OutResolvedPath))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("Could not read '%s' as text. .pwskel sources are UTF-8 text files."),
                    *OutResolvedPath));
            return false;
        }

        return true;
    }

    const TCHAR* SeverityToString(EPwSeverity Severity)
    {
        return Severity == EPwSeverity::Warning ? TEXT("warning") : TEXT("error");
    }

    struct FDiagnosticView
    {
        int32 Limit = 5;
        bool bCollapse = true;
        TOptional<EPwSeverity> Severity;
        FString SeverityFilter = TEXT("all");
    };

    bool ReadDiagnosticView(FHandlerContext& Ctx, FDiagnosticView& OutView)
    {
        OutView.Limit = Ctx.GetInt(TEXT("diagnosticLimit"), 5);
        if (OutView.Limit < 0)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("'diagnosticLimit' must be 0 or greater; got %d. 0 means every diagnostic."),
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
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                FString::Printf(TEXT("'diagnosticSeverity' must be 'all', 'error' or 'warning'; got '%s'."),
                    *Requested));
            return false;
        }

        OutView.SeverityFilter = Requested.ToLower();
        return true;
    }

    struct FDiagnosticGroup
    {
        const FPwDiagnostic* First = nullptr;
        int32 Count = 0;
        TArray<const FPwDiagnostic*> Sites;
        int32 SitesOmitted = 0;
        TSet<FString> Messages;
    };

    FString DiagnosticGroupKey(const FPwDiagnostic& Diagnostic)
    {
        return FString::Printf(TEXT("%s|%s|%s"),
            SeverityToString(Diagnostic.Severity), *Diagnostic.Code, *Diagnostic.ScopeName);
    }

    void AddDiagnostics(const TSharedPtr<FJsonObject>& Result,
                        const TArray<FPwDiagnostic>& Diagnostics,
                        const FDiagnosticView& View)
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

        TArray<FDiagnosticGroup> Groups;
        TMap<FString, int32> GroupIndexByKey;
        Groups.Reserve(Kept.Num());
        for (const FPwDiagnostic* Diagnostic : Kept)
        {
            int32 GroupIndex = INDEX_NONE;
            if (View.bCollapse)
            {
                const FString Key = DiagnosticGroupKey(*Diagnostic);
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

            FDiagnosticGroup& Group = Groups[GroupIndex];
            if (Group.Count == 0)
            {
                Group.First = Diagnostic;
            }
            ++Group.Count;
            Group.Messages.Add(Diagnostic->Message);
            if (Group.Sites.Num() < 12)
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

        TArray<TSharedPtr<FJsonValue>> Values;
        Values.Reserve(Selected.Num());
        int32 Represented = 0;
        for (const int32 GroupIndex : Selected)
        {
            const FDiagnosticGroup& Group = Groups[GroupIndex];
            Represented += Group.Count;
            const FPwDiagnostic& Diagnostic = *Group.First;

            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("severity"), SeverityToString(Diagnostic.Severity));
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
                Entry->SetArrayField(TEXT("occurrenceSites"), MoveTemp(Sites));
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

    int32 CountBones(const TArray<FPwBone>& Bones)
    {
        int32 Count = 0;
        for (const FPwBone& Bone : Bones)
        {
            ++Count;
            Count += CountBones(Bone.Children);
        }
        return Count;
    }

    FString FailureMessage(const TArray<FPwDiagnostic>& Diagnostics)
    {
        for (const FPwDiagnostic& Diagnostic : Diagnostics)
        {
            if (Diagnostic.Severity == EPwSeverity::Error)
            {
                return Diagnostic.ToString();
            }
        }
        return TEXT("Skeleton compilation failed without reporting a diagnostic.");
    }

    TSharedPtr<FJsonObject> ParseResultToJson(
        const FPwSkelDocument& Document, const FString& ResolvedPath,
        const TArray<FPwDiagnostic>& Diagnostics, bool bSuccess,
        const FDiagnosticView& View)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetNumberField(TEXT("version"), Document.Header.Version);
        Result->SetBoolField(TEXT("success"), bSuccess);
        Result->SetStringField(TEXT("sourcePath"), ResolvedPath);
        Result->SetNumberField(TEXT("rootCount"), Document.Roots.Num());
        Result->SetNumberField(TEXT("boneCount"), CountBones(Document.Roots));
        AddDiagnostics(Result, Diagnostics, View);
        return Result;
    }

    TSharedPtr<FJsonObject> ParamSpecToJson(const FPwParamSpec& Spec)
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

    TSharedPtr<FJsonObject> OpSpecToJson(const FPwSkelOpSpec& Spec)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("name"), Spec.Name);
        Json->SetStringField(TEXT("context"), Spec.Context);
        Json->SetStringField(TEXT("description"), Spec.Description);
        Json->SetBoolField(TEXT("acceptsBlock"), Spec.bAcceptsBlock);
        Json->SetBoolField(TEXT("requiresBlock"), Spec.bRequiresBlock);

        TArray<TSharedPtr<FJsonValue>> Params;
        Params.Reserve(Spec.Params.Num());
        for (const FPwParamSpec& Param : Spec.Params)
        {
            Params.Add(MakeShared<FJsonValueObject>(ParamSpecToJson(Param)));
        }
        Json->SetArrayField(TEXT("params"), MoveTemp(Params));
        return Json;
    }
}

// ============================================================================
// skeleton.compile
// ============================================================================
REGISTER_RPC_HANDLER("skeleton.compile", "skeleton",
    "Compile a .pwskel source file into exactly one USkeleton asset.",
    RPC_PARAMS(
        RPC_PARAM_REQ("filePath", "string",
            "Filesystem path to the .pwskel source; a relative path resolves against the project directory."),
        RPC_PARAM_OPT("outputPath", "string",
            "Destination /Game/... asset path. Omit it only when filePath is a .pwskel directly below the "
            "project Content directory; the target then uses the same relative path and basename under /Game. "
            "Sources elsewhere require this explicit argument."),
        RPC_PARAM_DEF("overwrite", "boolean",
            "Permit replacing a foreign asset or deliberately discarding live state that this source does not reproduce.", "false"),
        RPC_PARAM_DEF("save", "boolean", "Save the generated asset to disk.", "true"),
        RPC_PARAM_OPT("text", "string",
            "Not accepted on skeleton.compile. Use skeleton.validate for inline text, then write the .pwskel file."),
        RPC_PARAM_DEF("diagnosticLimit", "integer",
            "Maximum number of diagnostic entries to emit; 0 means every entry.", "5"),
        RPC_PARAM_DEF("diagnosticSeverity", "string",
            "Print only 'error', 'warning' or 'all' diagnostics.", "all"),
        RPC_PARAM_DEF("collapseDiagnostics", "boolean",
            "Collapse repeated diagnostics by severity, code and bone.", "true")
    ))
{
    using namespace PinWrightSkeletonHandlers;

    if (HasField(Ctx, TEXT("text")))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            TEXT("skeleton.compile does not accept inline 'text'. Use skeleton.validate to iterate on text, "
                 "then write the document to a .pwskel file and pass filePath."));
        return true;
    }

    FString FilePath;
    if (!Ctx.RequireString(TEXT("filePath"), FilePath))
    {
        return true;
    }

    const FString ResolvedForOutput = ResolveSourcePath(FilePath);
    FString OutputPath;
    if (HasField(Ctx, TEXT("outputPath")))
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
                ResolvedForOutput, TEXT(".pwskel"), OutputPath, Reason))
        {
            Ctx.SendError(ErrorCodes::ERR_SOURCE_OUTPUT_PATH_NOT_DERIVABLE,
                FString::Printf(
                    TEXT("Cannot derive 'outputPath' from source '%s': %s. Pass explicit "
                         "outputPath='/Game/.../AssetName'. No default path was chosen."),
                    *ResolvedForOutput, *Reason));
            return true;
        }
    }

    FDiagnosticView View;
    if (!ReadDiagnosticView(Ctx, View))
    {
        return true;
    }

    FString Source;
    FString ResolvedPath;
    if (!LoadSourceFile(Ctx, FilePath, Source, ResolvedPath))
    {
        return true;
    }

    FPwSkelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bParsed = FPwSkelParser::Parse(FStringView(Source), Document, Diagnostics);
    TSharedPtr<FJsonObject> Result = ParseResultToJson(
        Document, ResolvedPath, Diagnostics, false, View);

    if (!bParsed)
    {
        Ctx.SendError(ErrorCodes::ERR_PARSE_FAILED, FailureMessage(Diagnostics), Result);
        return true;
    }

    FSkeletonCreateSpec CreateSpec;
    CreateSpec.AssetPath = OutputPath;
    CreateSpec.bOverwrite = Ctx.GetBool(TEXT("overwrite"), false);
    CreateSpec.bSave = Ctx.GetBool(TEXT("save"), true);
    CreateSpec.SourcePath = ProvenancePath(ResolvedPath);
    CreateSpec.SourceHash = HashSource(FStringView(Source));

    const FSkeletonCreateResult Created = CreateSkeleton(Document, CreateSpec);
    if (Created.Diagnostics.Num() > 0)
    {
        Diagnostics.Append(Created.Diagnostics);
        AddDiagnostics(Result, Diagnostics, View);
    }
    if (!Created.bSuccess)
    {
        Result->SetStringField(TEXT("errorCode"), Created.ErrorCode);
        Result->SetStringField(TEXT("errorMessage"), Created.ErrorMessage);
        Ctx.SendError(ErrorCodes::ERR_COMPILE_FAILED,
            Created.ErrorMessage.IsEmpty()
                ? TEXT("Skeleton asset creation failed.") : Created.ErrorMessage,
            Result);
        return true;
    }

    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("assetPath"), Created.AssetPath);
    Result->SetStringField(TEXT("assetClass"), TEXT("USkeleton"));
    Result->SetBoolField(TEXT("updatedInPlace"), Created.bUpdatedInPlace);
    Result->SetNumberField(TEXT("boneCount"), Created.BoneCount);
    Result->SetBoolField(TEXT("savedToDisk"), Created.bSavedToDisk);
    Result->SetBoolField(TEXT("pendingFlush"), Created.bPendingFlush);
    Result->SetNumberField(TEXT("sizeBytes"), static_cast<double>(Created.SizeBytes));
    AddAssetSaveReport(Result, CreateSpec.bSave, Created.bSavedToDisk, Created.SaveState);

    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// skeleton.validate
// ============================================================================
REGISTER_RPC_HANDLER("skeleton.validate", "skeleton",
    "Parse a .pwskel document without creating or writing a USkeleton asset.",
    RPC_PARAMS(
        RPC_PARAM_OPT("text", "string", "Inline .pwskel source. Supply exactly one of text or filePath."),
        RPC_PARAM_OPT("filePath", "string",
            "Filesystem path to a .pwskel source. Supply exactly one of text or filePath."),
        RPC_PARAM_DEF("diagnosticLimit", "integer",
            "Maximum number of diagnostic entries to emit; 0 means every entry.", "5"),
        RPC_PARAM_DEF("diagnosticSeverity", "string",
            "Print only 'error', 'warning' or 'all' diagnostics.", "all"),
        RPC_PARAM_DEF("collapseDiagnostics", "boolean",
            "Collapse repeated diagnostics by severity, code and bone.", "true")
    ))
{
    using namespace PinWrightSkeletonHandlers;

    const bool bHasText = HasField(Ctx, TEXT("text"));
    const bool bHasFilePath = HasField(Ctx, TEXT("filePath"));
    if (bHasText == bHasFilePath)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            bHasText
                ? TEXT("skeleton.validate takes exactly one of 'text' or 'filePath'; both were supplied.")
                : TEXT("skeleton.validate takes exactly one of 'text' or 'filePath'; neither was supplied."));
        return true;
    }

    FDiagnosticView View;
    if (!ReadDiagnosticView(Ctx, View))
    {
        return true;
    }

    FString Source;
    FString ResolvedPath;
    if (bHasFilePath)
    {
        FString FilePath;
        if (!Ctx.RequireString(TEXT("filePath"), FilePath) ||
            !LoadSourceFile(Ctx, FilePath, Source, ResolvedPath))
        {
            return true;
        }
    }
    else
    {
        Source = Ctx.GetString(TEXT("text"));
        if (Source.TrimStartAndEnd().IsEmpty())
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("'text' is empty. A .pwskel document starts with the version header 'pwskel 0'."));
            return true;
        }
    }

    FPwSkelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bParsed = FPwSkelParser::Parse(FStringView(Source), Document, Diagnostics);
    TSharedPtr<FJsonObject> Result = ParseResultToJson(
        Document, ResolvedPath, Diagnostics, bParsed, View);
    if (!bParsed)
    {
        Ctx.SendError(ErrorCodes::ERR_PARSE_FAILED, FailureMessage(Diagnostics), Result);
        return true;
    }

    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// skeleton.describe_ops
// ============================================================================
REGISTER_RPC_HANDLER("skeleton.describe_ops", "skeleton",
    "Emit the .pwskel vocabulary from the same parser table that validates bone declarations.",
    RPC_PARAMS(
        RPC_PARAM_OPT("op", "string",
            "Narrow to one skeleton construct. Omitted, the complete vocabulary is returned.")
    ))
{
    using namespace PinWrightSkeletonHandlers;

    const FString Requested = Ctx.GetString(TEXT("op"));
    TArray<TSharedPtr<FJsonValue>> OpValues;
    for (const FPwSkelOpSpec& Spec : PwSkelOpTable::Get())
    {
        if (Requested.IsEmpty() || Spec.Name == Requested)
        {
            OpValues.Add(MakeShared<FJsonValueObject>(OpSpecToJson(Spec)));
        }
    }

    if (OpValues.Num() == 0)
    {
        const TArray<FString> Names = PwSkelOpTable::Names();
        const FString Guess = PwSuggest::Closest(
            Requested, TArrayView<const FString>(Names));
        Ctx.SendError(ErrorCodes::ERR_UNKNOWN_OPERATION,
            Guess.IsEmpty()
                ? FString::Printf(TEXT("No .pwskel construct named '%s'. Call skeleton.describe_ops with no 'op' for the whole vocabulary."), *Requested)
                : FString::Printf(TEXT("No .pwskel construct named '%s'. Did you mean '%s'?"), *Requested, *Guess));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("opCount"), OpValues.Num());
    Result->SetArrayField(TEXT("ops"), MoveTemp(OpValues));
    // Keep the response shape explicit: skeleton has no non-op parameter sets.  A caller can
    // iterate format vocabularies without inferring whether a missing array means "none" or
    // "this format has a different response contract".
    Result->SetNumberField(TEXT("paramSetCount"), 0);
    Result->SetArrayField(TEXT("paramSets"), TArray<TSharedPtr<FJsonValue>>());
    Ctx.SendSuccess(Result);
    return true;
}






