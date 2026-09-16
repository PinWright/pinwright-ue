// Copyright (c) 2026 Alexander Penkin. MIT License.

// Sequencer FBX interchange: sequencer.export_fbx / sequencer.import_fbx.
//
// Routes through the canonical engine entry points
// USequencerToolsFunctionLibrary::ExportLevelSequenceFBX(FSequencerExportFBXParams) and
// ImportLevelSequenceFBX(World, Sequence, Bindings[], Settings, Filename) in the
// SequencerScriptingEditor module (SequencerTools.h) — the same path Epic's Python FBX
// round-trip uses. Both signatures are stable across UE 5.3-5.7 (the struct-based export
// and the bindings-array import exist in every install; the 5.6+ ImportLevelSequenceFBX
// ActorContext arg is defaulted, so the 5-arg call is portable), so no version gate is
// needed. The engine exporter has NO sub-range parameter — export always covers the full
// playback range.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/ParamSpec.h"
#include "Utils/AtomicFileWriter.h"
#include "Utils/AssetUtils.h"
#include "Utils/MovieSceneJsonUtils.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneBindingProxy.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "ScopedTransaction.h"

#include "SequencerTools.h"
#include "MovieSceneToolsUserSettings.h"

namespace
{
    struct FSequencerFbxMovieSceneStats
    {
        int32 TrackCount = 0;
        int32 SectionCount = 0;
        int32 KeyCount = 0;
        TSet<FGuid> ObjectSignatures;

        bool IsSameOutcome(const FSequencerFbxMovieSceneStats& Other) const
        {
            if (TrackCount != Other.TrackCount || SectionCount != Other.SectionCount
                || KeyCount != Other.KeyCount || ObjectSignatures.Num() != Other.ObjectSignatures.Num())
            {
                return false;
            }
            for (const FGuid& Signature : ObjectSignatures)
            {
                if (!Other.ObjectSignatures.Contains(Signature))
                {
                    return false;
                }
            }
            return true;
        }
    };

    void SequencerFbxMeasureTrack(
        const UMovieSceneTrack* Track, FSequencerFbxMovieSceneStats& InOutStats)
    {
        if (!Track)
        {
            return;
        }
        ++InOutStats.TrackCount;
        InOutStats.ObjectSignatures.Add(Track->GetSignature());
        for (const UMovieSceneSection* Section : Track->GetAllSections())
        {
            if (Section)
            {
                ++InOutStats.SectionCount;
                InOutStats.ObjectSignatures.Add(Section->GetSignature());
                InOutStats.KeyCount += MovieSceneJsonUtils::CountSectionKeys(Section);
            }
        }
    }

    FSequencerFbxMovieSceneStats SequencerFbxMeasureMovieScene(const UMovieScene* MovieScene)
    {
        FSequencerFbxMovieSceneStats Stats;
        if (!MovieScene)
        {
            return Stats;
        }
        for (const UMovieSceneTrack* Track : MovieScene->GetTracks())
        {
            SequencerFbxMeasureTrack(Track, Stats);
        }
        SequencerFbxMeasureTrack(MovieScene->GetCameraCutTrack(), Stats);
        for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
        {
            for (const UMovieSceneTrack* Track : Binding.GetTracks())
            {
                SequencerFbxMeasureTrack(Track, Stats);
            }
        }
        return Stats;
    }

    TSharedPtr<FJsonObject> SequencerFbxBuildImportStats(
        const FSequencerFbxMovieSceneStats& Before, const FSequencerFbxMovieSceneStats& After)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetNumberField(TEXT("tracksBefore"), Before.TrackCount);
        Result->SetNumberField(TEXT("tracksAfter"), After.TrackCount);
        Result->SetNumberField(TEXT("sectionsBefore"), Before.SectionCount);
        Result->SetNumberField(TEXT("sectionsAfter"), After.SectionCount);
        Result->SetNumberField(TEXT("keysBefore"), Before.KeyCount);
        Result->SetNumberField(TEXT("keysAfter"), After.KeyCount);
        return Result;
    }

    // Resolve the `path` payload key to a loaded ULevelSequence. Tries an in-memory /
    // transient lookup first (LoadObject finds objects already in memory by full path),
    // then the asset-registry-backed editor load. Sends INVALID_SEQUENCE and returns
    // nullptr on failure. Named (not a shared helper) but kept in an anonymous namespace
    // because it is used only within this TU and takes no address that could ODR-collide.
    ULevelSequence* SequencerFbxResolveSequence(FHandlerContext& Ctx, FString& OutPath)
    {
        OutPath = Ctx.GetString(TEXT("path"));
        if (OutPath.IsEmpty())
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE,
                TEXT("A Level Sequence asset 'path' is required"));
            return nullptr;
        }

        ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *OutPath);
        if (!Sequence)
        {
            Sequence = Cast<ULevelSequence>(ResolveAsset(OutPath, /*bLoadObject=*/true).Object);
        }
        if (!Sequence)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE,
                FString::Printf(TEXT("Sequence not found or not a LevelSequence: %s"), *OutPath));
        }
        return Sequence;
    }

    // Build the list of binding proxies to operate on. Semantics:
    //   - `bindings` OMITTED -> every object binding in the sequence.
    //   - `bindings` PRESENT -> exactly those GUIDs; each entry must be a non-empty
    //     string that parses to a GUID present in the sequence, else the error is sent
    //     and bOutOk=false. A present-but-empty array is NOT treated as "all".
    // The proxy list is guaranteed non-empty when bOutOk=true: an explicit-empty /
    // all-skipped `bindings` array, or an omitted array on a sequence that has no
    // bindings, is rejected rather than forwarded as an empty set. The engine reads an
    // empty binding list as bSelectedOnly=false (MovieSceneToolHelpers:4030) and would
    // export/replace EVERY actor in the level — destructive on import (bReplaceTransformTrack)
    // and a whole-level dump on export — the exact opposite of a filtered/no-op intent.
    TArray<FMovieSceneBindingProxy> SequencerFbxCollectBindings(
        FHandlerContext& Ctx, ULevelSequence* Sequence, UMovieScene* MovieScene, bool& bOutOk)
    {
        bOutOk = true;
        TArray<FMovieSceneBindingProxy> Proxies;

        const TArray<TSharedPtr<FJsonValue>>* BindingsArr = nullptr;
        const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
        const bool bBindingsPresent = Payload.IsValid() &&
            Payload->TryGetArrayField(TEXT("bindings"), BindingsArr) && BindingsArr;

        if (bBindingsPresent)
        {
            for (const TSharedPtr<FJsonValue>& Val : *BindingsArr)
            {
                FString GuidStr;
                if (!Val.IsValid() || !Val->TryGetString(GuidStr) || GuidStr.IsEmpty())
                {
                    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                        TEXT("bindings entry is empty or not a GUID string"));
                    bOutOk = false;
                    return Proxies;
                }
                FGuid Guid;
                if (!FGuid::Parse(GuidStr, Guid) || !Guid.IsValid())
                {
                    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                        FString::Printf(TEXT("bindings entry is not a valid GUID: '%s'"), *GuidStr));
                    bOutOk = false;
                    return Proxies;
                }
                if (!MovieScene->FindBinding(Guid))
                {
                    Ctx.SendError(ErrorCodes::ERR_BINDING_NOT_FOUND,
                        FString::Printf(TEXT("No binding '%s' in the sequence"), *GuidStr));
                    bOutOk = false;
                    return Proxies;
                }
                Proxies.Emplace(Guid, Sequence);
            }

            if (Proxies.Num() == 0)
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    TEXT("bindings array is present but empty; omit it to target all bindings"));
                bOutOk = false;
            }
            return Proxies;
        }

        // Omitted -> select every binding. Const overload: the non-const GetBindings()
        // is UE_DEPRECATED(5.7).
        const UMovieScene* ConstMovieScene = MovieScene;
        for (const FMovieSceneBinding& Binding : ConstMovieScene->GetBindings())
        {
            Proxies.Emplace(Binding.GetObjectGuid(), Sequence);
        }
        if (Proxies.Num() == 0)
        {
            Ctx.SendError(ErrorCodes::ERR_BINDING_NOT_FOUND,
                TEXT("sequence has no object bindings to export/import"));
            bOutOk = false;
        }
        return Proxies;
    }

    // The editor world FBX export/import play a transient ULevelSequencePlayer in.
    UWorld* SequencerFbxEditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    // Resolve a `filePath` payload value to an absolute path. FPaths::ConvertRelativePathToFull
    // alone resolves a relative path against FPlatformProcess::BaseDir() (the editor binaries
    // dir, <Engine>/Binaries/Win64), NOT the project dir the parameter docs promise — which
    // would silently write exports into the engine folder and make imports miss project-relative
    // files. Anchor a relative path under the project dir first, then normalize to absolute.
    FString SequencerFbxResolveFilePath(const FString& InFilePath)
    {
        if (FPaths::IsRelative(InFilePath))
        {
            return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), InFilePath));
        }
        return FPaths::ConvertRelativePathToFull(InFilePath);
    }
}

// ============================================================================
// sequencer.export_fbx
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.export_fbx", "sequencer",
    "Export a Level Sequence's bound-object animation (transform + animated property tracks) to an "
    "external .fbx file via USequencerToolsFunctionLibrary::ExportLevelSequenceFBX. The engine exporter "
    "has no sub-range option — it always exports the whole playback range. Pass `bindings` (array of "
    "binding GUID strings) to restrict which object bindings export; omit it to export every binding. "
    "Existing files are refused unless `overwrite` is true; publication is atomic.",
    RPC_PARAMS(
        RPC_PARAM_REQ("path", "path", "Level Sequence asset path"),
        RPC_PARAM_REQ("filePath", "filepath", "Output .fbx file path (relative paths resolve against the project dir)"),
        RPC_PARAM_OPT("bindings", "array", "Optional array of binding GUID strings; default exports all bindings"),
        RPC_PARAM_DEF("overwrite", "bool", "Atomically replace an existing output file", "false")
    ))
{
    FString SeqPath;
    ULevelSequence* Sequence = SequencerFbxResolveSequence(Ctx, SeqPath);
    if (!Sequence)
    {
        return true;
    }
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, TEXT("LevelSequence has no MovieScene"));
        return true;
    }

    FString FilePath;
    if (!Ctx.RequireString(TEXT("filePath"), FilePath))
    {
        return true;
    }
    FilePath = SequencerFbxResolveFilePath(FilePath);
    const bool bOverwrite = Ctx.GetBool(TEXT("overwrite"), false);

    UWorld* World = SequencerFbxEditorWorld();
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_EDITOR_WORLD,
            TEXT("No editor world available for FBX export"));
        return true;
    }

    bool bBindingsOk = false;
    TArray<FMovieSceneBindingProxy> Bindings = SequencerFbxCollectBindings(Ctx, Sequence, MovieScene, bBindingsOk);
    if (!bBindingsOk)
    {
        return true;
    }
    if (IFileManager::Get().FileExists(*FilePath) && !bOverwrite)
    {
        Ctx.SendError(ErrorCodes::ERR_ALREADY_EXISTS,
            FString::Printf(TEXT("FBX output already exists: %s; pass overwrite=true to replace it"),
                *FilePath));
        return true;
    }

    // The engine exporter exposes a void WriteToFile underneath a boolean that is true even
    // when no bytes land. Export to a task-owned sibling first, then byte-verify and publish
    // through the shared atomic writer so a stale destination can never satisfy the check.
    const FString OutputDirectory = FPaths::GetPath(FilePath);
    if (!IFileManager::Get().DirectoryExists(*OutputDirectory)
        && !IFileManager::Get().MakeDirectory(*OutputDirectory, /*Tree=*/true))
    {
        Ctx.SendError(ErrorCodes::ERR_EXPORT_FAILED,
            FString::Printf(TEXT("Could not create FBX output directory: %s"), *OutputDirectory));
        return true;
    }
    const FString StagedPath = FilePath + TEXT(".")
        + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".stage.fbx");
    ON_SCOPE_EXIT
    {
        IFileManager::Get().Delete(
            *StagedPath, /*RequireExists=*/false, /*EvenReadOnly=*/true, /*Quiet=*/true);
    };

    FSequencerExportFBXParams Params;
    Params.World = World;
    Params.Sequence = Sequence;
    Params.RootSequence = Sequence;
    Params.Bindings = Bindings;
    // Tracks left empty (default) -> export all tracks on the selected bindings.
    Params.OverrideOptions = nullptr;
    Params.FBXFileName = StagedPath;

    const bool bExported = USequencerToolsFunctionLibrary::ExportLevelSequenceFBX(Params);
    TArray<uint8> StagedBytes;
    const int64 StagedFileSize = IFileManager::Get().FileSize(*StagedPath);
    if (!bExported || StagedFileSize <= 0 || !FFileHelper::LoadFileToArray(StagedBytes, *StagedPath)
        || StagedBytes.Num() != StagedFileSize)
    {
        Ctx.SendError(ErrorCodes::ERR_EXPORT_FAILED,
            FString::Printf(TEXT("ExportLevelSequenceFBX failed to stage '%s' for sequence %s "
                                 "(returned=%s, fileSize=%lld)"),
                *StagedPath, *SeqPath, bExported ? TEXT("true") : TEXT("false"), StagedFileSize));
        return true;
    }

    const AtomicFileWriter::FResult PublishResult = AtomicFileWriter::WriteBytes(
        FilePath, MakeArrayView(StagedBytes),
        bOverwrite ? AtomicFileWriter::EExistingFilePolicy::ReplaceExisting
                   : AtomicFileWriter::EExistingFilePolicy::FailIfExists);
    if (!PublishResult.IsSuccess())
    {
        const TCHAR* ErrorCode = PublishResult.Status == AtomicFileWriter::EStatus::AlreadyExists
            ? ErrorCodes::ERR_ALREADY_EXISTS
            : ErrorCodes::ERR_EXPORT_FAILED;
        Ctx.SendError(ErrorCode,
            FString::Printf(TEXT("Failed to publish FBX '%s' atomically: %s"),
                *FilePath, *PublishResult.Error));
        return true;
    }

    const int64 FileSize = IFileManager::Get().FileSize(*FilePath);
    if (FileSize != StagedBytes.Num())
    {
        Ctx.SendError(ErrorCodes::ERR_EXPORT_FAILED,
            FString::Printf(TEXT("Published FBX size verification failed for '%s' "
                                 "(expected=%d, actual=%lld)"),
                *FilePath, StagedBytes.Num(), FileSize));
        return true;
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("sequencePath"), SeqPath);
    Resp->SetStringField(TEXT("filePath"), FilePath);
    Resp->SetNumberField(TEXT("fileSize"), static_cast<double>(FileSize));
    Resp->SetNumberField(TEXT("bindingCount"), Bindings.Num());
    Resp->SetBoolField(TEXT("replaced"), PublishResult.bReplaced);
    Ctx.SendSuccess(Resp);
    return true;
}

// ============================================================================
// sequencer.import_fbx
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.import_fbx", "sequencer",
    "Import animation from an external .fbx file onto a Level Sequence's object bindings (baking "
    "transform + animated-property keys) via USequencerToolsFunctionLibrary::ImportLevelSequenceFBX. "
    "FBX nodes are matched to bindings by name. Pass `bindings` (array of binding GUID strings) to "
    "restrict the target bindings; omit it to target every binding in the sequence.",
    RPC_PARAMS(
        RPC_PARAM_REQ("path", "path", "Level Sequence asset path"),
        RPC_PARAM_REQ("filePath", "filepath", "Input .fbx file path (relative paths resolve against the project dir)"),
        RPC_PARAM_OPT("bindings", "array", "Optional array of binding GUID strings; default targets all bindings"),
        RPC_PARAM_DEF("matchByNameOnly", "bool", "Match FBX node names to binding names only", "true")
    ))
{
    FString SeqPath;
    ULevelSequence* Sequence = SequencerFbxResolveSequence(Ctx, SeqPath);
    if (!Sequence)
    {
        return true;
    }
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, TEXT("LevelSequence has no MovieScene"));
        return true;
    }
    if (MovieScene->IsReadOnly())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE,
            TEXT("MovieScene is read-only; cannot import FBX"));
        return true;
    }

    FString FilePath;
    if (!Ctx.RequireString(TEXT("filePath"), FilePath))
    {
        return true;
    }
    FilePath = SequencerFbxResolveFilePath(FilePath);
    if (IFileManager::Get().FileSize(*FilePath) <= 0)
    {
        Ctx.SendError(ErrorCodes::ERR_FILE_NOT_FOUND,
            FString::Printf(TEXT("FBX file not found or empty: %s"), *FilePath));
        return true;
    }

    UWorld* World = SequencerFbxEditorWorld();
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_EDITOR_WORLD,
            TEXT("No editor world available for FBX import"));
        return true;
    }

    bool bBindingsOk = false;
    TArray<FMovieSceneBindingProxy> Bindings = SequencerFbxCollectBindings(Ctx, Sequence, MovieScene, bBindingsOk);
    if (!bBindingsOk)
    {
        return true;
    }

    UMovieSceneUserImportFBXSettings* Settings =
        NewObject<UMovieSceneUserImportFBXSettings>(GetTransientPackage());
    Settings->bMatchByNameOnly = Ctx.GetBool(TEXT("matchByNameOnly"), true);
    Settings->bForceFrontXAxis = false;
    Settings->bConvertSceneUnit = false;
    Settings->bCreateCameras = false;
    Settings->bReplaceTransformTrack = true;
    Settings->bReduceKeys = false;

    const FSequencerFbxMovieSceneStats Before = SequencerFbxMeasureMovieScene(MovieScene);
    FScopedTransaction Transaction(NSLOCTEXT("PinWright", "SequencerImportFbx", "Import Level Sequence FBX"));
    Sequence->Modify();
    MovieScene->Modify();

    const bool bImported = USequencerToolsFunctionLibrary::ImportLevelSequenceFBX(
        World, Sequence, Bindings, Settings, FilePath);
    if (!bImported)
    {
        Transaction.Cancel();
        Ctx.SendError(ErrorCodes::ERR_IMPORT_FAILED,
            FString::Printf(TEXT("ImportLevelSequenceFBX failed for sequence %s from %s"), *SeqPath, *FilePath));
        return true;
    }

    const FSequencerFbxMovieSceneStats After = SequencerFbxMeasureMovieScene(MovieScene);
    TSharedPtr<FJsonObject> ImportStats = SequencerFbxBuildImportStats(Before, After);
    if (Before.IsSameOutcome(After))
    {
        Transaction.Cancel();
        Ctx.SendError(ErrorCodes::ERR_NOTHING_IMPORTED,
            FString::Printf(TEXT("FBX import changed no tracks, sections, or keys in sequence %s"),
                *SeqPath),
            ImportStats);
        return true;
    }

    TSharedPtr<FJsonObject> Resp = ImportStats;
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("sequencePath"), SeqPath);
    Resp->SetStringField(TEXT("filePath"), FilePath);
    Resp->SetNumberField(TEXT("bindingCount"), Bindings.Num());
    Ctx.SendSuccess(Resp);
    return true;
}
