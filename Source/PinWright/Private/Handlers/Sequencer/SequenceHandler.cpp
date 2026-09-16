// Copyright (c) 2026 Alexander Penkin. MIT License.

// Sequence handler - migrated from _SequenceHandlers.cpp to auto-registration
// Covers: sequence.create, sequence.set_display_rate, sequence.set_properties,
//   sequence.open, sequence.add_camera, sequence.play, sequence.add_actor,
//   sequence.add_actors, sequence.add_spawnable_from_class, sequence.remove_actors,
//   sequence.get_bindings, sequence.get_properties, sequence.set_playback_speed,
//   sequence.pause, sequence.stop, sequence.list, sequence.duplicate,
//   sequence.rename, sequence.delete, sequence.get_metadata, sequence.add_keyframe,
//   sequencer.add_keyframes,
//   sequence.add_section, sequence.set_tick_resolution, sequence.set_view_range,
//   sequence.set_track_muted, sequence.set_track_solo, sequence.set_track_locked,
//   sequence.remove_track, sequence.list_track_types, sequence.add_track,
//   sequence.list_tracks, sequencer.list_sections, sequencer.repoint_actor,
//   sequence.set_work_range

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Sequencer/SequencerBindingUtils.h"
#include "Handlers/Sequencer/SequencerKeyInterp.h"
#include "Handlers/Sequencer/SequencePlayheadUtils.h"
// UE_VERSION_OLDER_THAN — used to gate the 5.6+ time-warp variant header below.
#include "Misc/EngineVersionComparison.h"
#include "PinWrightSubsystem.h"
#include "PinWrightHelpers.h"
#include "Utils/AssetCreatePolicy.h"
#include "Utils/AssetUtils.h"
#include "Utils/ClassUtils.h"
#include "Utils/MovieSceneJsonUtils.h"
#include "State/PluginState.h"

#include "Dom/JsonObject.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
// UMovieSceneNameableTrack — the only place a track's caller-visible name can be stored
// (SetDisplayName); UMovieSceneTrack itself has no name setter. Root-level public header of
// the MovieScene module, same as MovieSceneTrack.h beside it.
#include "MovieSceneNameableTrack.h"
#include "MovieSceneSection.h"
#include "MovieSceneSequence.h"
#include "MovieSceneTrack.h"
#include "UObject/UObjectIterator.h"


#include "Editor.h"
#if __has_include("Subsystems/EditorActorSubsystem.h")
#include "Subsystems/EditorActorSubsystem.h"
#define MCP_SEQ_HAS_EDITOR_ACTOR_SUBSYSTEM 1
#elif __has_include("EditorActorSubsystem.h")
#include "EditorActorSubsystem.h"
#define MCP_SEQ_HAS_EDITOR_ACTOR_SUBSYSTEM 1
#else
#define MCP_SEQ_HAS_EDITOR_ACTOR_SUBSYSTEM 0
#endif

#include "AssetToolsModule.h"
#include "Editor/EditorEngine.h"
#include "Engine/Selection.h"
#include "Factories/Factory.h"
#include "IAssetTools.h"
#include "LevelSequenceEditorBlueprintLibrary.h"
// FScopedTransaction — explicit include; unity builds inherit it from sibling TUs,
// the -StrictIncludes -DisableUnity Rocket packaging build does not.
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"

#if __has_include("ILevelSequenceEditorToolkit.h")
#include "ILevelSequenceEditorToolkit.h"
#endif

#if __has_include("ISequencer.h")
#include "ISequencer.h"
#include "MovieSceneSequencePlayer.h"
#endif

#if __has_include("Tracks/MovieSceneFloatTrack.h")
#include "Sections/MovieSceneFloatSection.h"
#include "Tracks/MovieSceneFloatTrack.h"
#endif

#if __has_include("Tracks/MovieSceneBoolTrack.h")
#include "Sections/MovieSceneBoolSection.h"
#include "Tracks/MovieSceneBoolTrack.h"
#endif

#if __has_include("Tracks/MovieScene3DTransformTrack.h")
#include "Tracks/MovieScene3DTransformTrack.h"
#endif

#include "Tracks/MovieSceneAudioTrack.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "Tracks/MovieSceneSubTrack.h"
#include "Sections/MovieSceneSubSection.h"
// UE 5.6+ models sub-section play rate as a time-warp variant; the header is absent in 5.4/5.5.
// On 5.6+ FMovieSceneTimeWarpVariant has operator=(double), so `Parameters.TimeScale = <double>`
// compiles unchanged on both versions (plain float in 5.4) — only the include needs guarding.
#if __has_include("Variants/MovieSceneTimeWarpVariant.h")
#include "Variants/MovieSceneTimeWarpVariant.h"
#endif

#if __has_include("Sections/MovieScene3DTransformSection.h")
#include "Sections/MovieScene3DTransformSection.h"
#endif
#if __has_include("Channels/MovieSceneDoubleChannel.h")
#include "Channels/MovieSceneDoubleChannel.h"
#endif
#if __has_include("Channels/MovieSceneChannelProxy.h")
#include "Channels/MovieSceneChannelProxy.h"
#endif
// FSystemInterrogator drives the same entity-system evaluation Sequencer runs at
// playback (applies section easing + cross-section blending), giving get_binding_transform
// the composited transform rather than a raw per-channel curve read.
#if __has_include("EntitySystem/Interrogation/MovieSceneInterrogationLinker.h")
#include "EntitySystem/Interrogation/MovieSceneInterrogationLinker.h"
#endif
#if __has_include("MovieSceneTracksPropertyTypes.h")
#include "MovieSceneTracksPropertyTypes.h"
#endif
// FMovieSceneTracksComponentTypes::ComponentTransform — the property-composite handle
// get_binding_transform reads the interrogated transform back through (see there for why the
// FIntermediate3DTransform returned by QueryLocalSpaceTransforms cannot be trusted for rotation).
#if __has_include("MovieSceneTracksComponentTypes.h")
#include "MovieSceneTracksComponentTypes.h"
#endif
// FSystemInterrogator::QueryPropertyValues is a template that no engine TU instantiates, so its
// body's dependencies (FBuiltInComponentTypes::PropertyRegistry, FPropertyDefinition::Handler)
// have to be complete here rather than assumed present through the interrogation headers.
#if __has_include("EntitySystem/BuiltInComponentTypes.h")
#include "EntitySystem/BuiltInComponentTypes.h"
#endif

#if __has_include("Misc/ScopedTransaction.h")
#include "Misc/ScopedTransaction.h"
#endif
#if __has_include("Camera/CameraActor.h")
#include "Camera/CameraActor.h"
#endif

// ============================================================================
// Local helpers (file-scoped, replacing old member functions)
// ============================================================================

namespace SequenceHelpers
{
    // THE definition of "the name of a track" for every sequencer verb in this file, on both
    // the emit side and the resolve side. There are two candidate accessors and only one of
    // them is usable as an identifier:
    //   - UMovieSceneTrack::GetTrackName() is `virtual FName GetTrackName() const { return
    //     NAME_None; }` (MovieSceneTrack.h:390) and is overridden by exactly three engine
    //     classes (UMovieScenePropertyTrack, UMovieSceneTimeWarpTrack,
    //     UMovieSceneControlRigParameterTrack). For every other track type it stringifies to
    //     the literal "None", which resolves to nothing.
    //   - UObject::GetName() is the track's actual outer-unique name (MovieSceneSubTrack_0)
    //     and is what all the Contains() lookups below already match against.
    // Emitting GetTrackName() therefore hands the caller an identifier that no lookup accepts.
    // Route every trackName field and every trackName lookup through this pair so the string a
    // verb returns and the string the next verb resolves cannot drift apart.
    static FString GetTrackIdentifier(const UMovieSceneTrack* Track)
    {
        return Track ? Track->GetName() : FString();
    }

    // The single trackName lookup predicate. Matches GetName() OR GetDisplayName(), because
    // list_tracks reports both (SetStringField "trackName"/"displayName") and add_track writes
    // the caller's requested name into the display name — so a trackName piped back from any of
    // them resolves the same regardless of which field it came from. Substring (Contains)
    // semantics are preserved verbatim from the pre-existing predicates, including the fact
    // that an empty Query matches the first track: tightening that is a separate behaviour
    // change with its own callers to consider.
    static bool TrackMatchesIdentifier(const UMovieSceneTrack* Track, const FString& Query)
    {
        return Track
            && (Track->GetName().Contains(Query)
                || Track->GetDisplayName().ToString().Contains(Query));
    }

    // Resolve a sequence path from the payload or fall back to the current sequence path global.
    static FString ResolveSequencePath(const TSharedPtr<FJsonObject>& Payload)
    {
        FString Path;
        if (Payload.IsValid() && Payload->TryGetStringField(TEXT("path"), Path) &&
            !Path.IsEmpty())
        {
            if (Path.StartsWith(TEXT("/Engine/Transient/")))
            {
                return Path;
            }
            if (ResolveAsset(Path).bExists)
            {
                UObject* Obj = ResolveAsset(Path, /*bLoadObject=*/true).Object;
                if (Obj)
                    return Obj->GetPathName();
            }
            return Path;
        }
        FString& CurrentPath = FPluginState::Get().CurrentSequencePath();
        if (!CurrentPath.IsEmpty())
            return CurrentPath;
        return FString();
    }

    // Ensure there is an entry in the sequence registry for the given path.
    static TSharedPtr<FJsonObject> EnsureSequenceEntry(const FString& SeqPath)
    {
        if (SeqPath.IsEmpty())
            return nullptr;
        auto& Registry = FPluginState::Get().SequenceRegistry();
        if (TSharedPtr<FJsonObject>* Found = Registry.Find(SeqPath))
            return *Found;
        TSharedPtr<FJsonObject> NewObj = MakeShared<FJsonObject>();
        NewObj->SetStringField(TEXT("sequencePath"), SeqPath);
        Registry.Add(SeqPath, NewObj);
        return NewObj;
    }

    static TSharedPtr<FJsonObject> BuildListedSectionJson(
        const UMovieSceneTrack* Track,
        const UMovieSceneSection* Section,
        const FString& BindingGuid,
        bool bIncludeKeys = false)
    {
        TSharedPtr<FJsonObject> SectionObj = MovieSceneJsonUtils::BuildSectionJson(Section, bIncludeKeys);
        if (!Track)
        {
            return SectionObj;
        }

        SectionObj->SetStringField(TEXT("trackName"), GetTrackIdentifier(Track));
        SectionObj->SetStringField(TEXT("trackClass"), Track->GetClass()->GetName());
        SectionObj->SetStringField(TEXT("bindingGuid"), BindingGuid);
        return SectionObj;
    }

    // Aggregate lock read-back for list_tracks: sequencer.set_track_locked writes
    // SetIsLocked on every section of a track, so a track is "locked" only when all
    // its sections are locked. An empty track (no sections) reports false — there is
    // nothing locked to report.
    static bool AreAllSectionsLocked(const UMovieSceneTrack* Track)
    {
        if (!Track)
        {
            return false;
        }
        const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
        if (Sections.Num() == 0)
        {
            return false;
        }
        for (const UMovieSceneSection* Section : Sections)
        {
            if (!Section || !Section->IsLocked())
            {
                return false;
            }
        }
        return true;
    }

    // Scan every UMovieSceneSubTrack in the MovieScene for a sub-section whose signature matches.
    // FindTrack<T>() only returns the first track, but a MovieScene may legally hold multiple
    // sub-tracks (different row groupings), so signature lookup must iterate them all.
    static UMovieSceneSubSection* FindSubSectionBySignature(UMovieScene* MovieScene, const FGuid& Signature)
    {
        if (!MovieScene)
        {
            return nullptr;
        }
        for (UMovieSceneTrack* Track : MovieScene->GetTracks())
        {
            UMovieSceneSubTrack* SubTrack = Cast<UMovieSceneSubTrack>(Track);
            if (!SubTrack)
            {
                continue;
            }
            for (UMovieSceneSection* Section : SubTrack->GetAllSections())
            {
                if (UMovieSceneSubSection* SubSection = Cast<UMovieSceneSubSection>(Section))
                {
                    if (SubSection->GetSignature() == Signature)
                    {
                        return SubSection;
                    }
                }
            }
        }
        return nullptr;
    }

    // Convert a display-rate frame number into the MovieScene's tick resolution.
    // Centralizes the FFrameRate::TransformTime conversion used by the keyframe
    // handlers; FloorToFrame matches the transform-track branches' rounding.
    static FFrameNumber DisplayFrameToTick(UMovieScene* MovieScene, double Frame)
    {
        const FFrameNumber FrameNum = FFrameNumber(static_cast<int32>(Frame));
        return FFrameRate::TransformTime(
                   FFrameTime(FrameNum), MovieScene->GetDisplayRate(), MovieScene->GetTickResolution())
            .FloorToFrame();
    }

    // Resolve (or create) the binding's UMovieScene3DTransformTrack section and return the
    // double channels Sequencer exposes for it (layout: 0-2 Location, 3-5 Rotation, 6-8 Scale).
    // Both the "Transform" keyframe branch and the per-axis Location/Rotation/Scale branch share
    // this scaffolding — only the per-axis value write differs between them.
    // OutSection (optional) receives the resolved section so callers can expand it to span the keys.
    static TArrayView<FMovieSceneDoubleChannel*> GetOrAddTransformChannels(
        UMovieScene* MovieScene, const FGuid& BindingGuid, UMovieSceneSection** OutSection = nullptr)
    {
        if (OutSection)
        {
            *OutSection = nullptr;
        }
        if (!MovieScene)
        {
            return TArrayView<FMovieSceneDoubleChannel*>();
        }

        UMovieScene3DTransformTrack* Track =
            MovieScene->FindTrack<UMovieScene3DTransformTrack>(BindingGuid, FName("Transform"));
        if (!Track)
            Track = MovieScene->AddTrack<UMovieScene3DTransformTrack>(BindingGuid);
        if (!Track)
        {
            return TArrayView<FMovieSceneDoubleChannel*>();
        }

        bool bSectionAdded = false;
        UMovieScene3DTransformSection* Section =
            Cast<UMovieScene3DTransformSection>(Track->FindOrAddSection(0, bSectionAdded));
        if (!Section)
        {
            return TArrayView<FMovieSceneDoubleChannel*>();
        }

        if (OutSection)
        {
            *OutSection = Section;
        }

        return Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
    }

    static FString RepointGuidString(const FGuid& Guid)
    {
        return Guid.IsValid() ? Guid.ToString() : FString();
    }

    static const TCHAR* RepointReadbackPhaseString(
        const SequencerBindingUtils::FRepointOutcome& Outcome)
    {
        if (!Outcome.bReadbackMeasured)
        {
            return TEXT("");
        }

        // The helper measures the restored binding after an engine write rejection but
        // leaves FailureReadbackPhase at its default. The wire contract reports the phase
        // that supplied the measured failure detail, which is AfterRollback in that case.
        if (Outcome.bRollbackAttempted
            && Outcome.FailureReadbackPhase == SequencerBindingUtils::ERepointReadbackPhase::BeforeWrite
            && Outcome.ErrorCode == ErrorCodes::ERR_BINDING_FAILED)
        {
            return TEXT("AfterRollback");
        }

        switch (Outcome.FailureReadbackPhase)
        {
        case SequencerBindingUtils::ERepointReadbackPhase::BeforeWrite:
            return TEXT("BeforeWrite");
        case SequencerBindingUtils::ERepointReadbackPhase::AfterWrite:
            return TEXT("AfterWrite");
        case SequencerBindingUtils::ERepointReadbackPhase::AfterRollback:
            return TEXT("AfterRollback");
        default:
            return TEXT("");
        }
    }

    static void SetRepointWarnings(
        const TSharedPtr<FJsonObject>& Result,
        const TArray<FString>& Warnings)
    {
        TArray<TSharedPtr<FJsonValue>> WarningValues;
        WarningValues.Reserve(Warnings.Num());
        for (const FString& Warning : Warnings)
        {
            WarningValues.Add(MakeShared<FJsonValueString>(Warning));
        }
        Result->SetArrayField(TEXT("warnings"), WarningValues);
    }

    static TSharedPtr<FJsonObject> BuildRepointSuccessResult(
        const SequencerBindingUtils::FRepointOutcome& Outcome)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("bindingGuid"), RepointGuidString(Outcome.BindingGuid));
        Result->SetStringField(TEXT("bindingName"), Outcome.BindingName);
        Result->SetStringField(TEXT("parentBindingGuid"), RepointGuidString(Outcome.ParentGuid));
        Result->SetNumberField(TEXT("locatorCount"), Outcome.LocatorCount);
        Result->SetStringField(TEXT("authoredClass"), Outcome.AuthoredClassPath);
        Result->SetStringField(TEXT("newObjectClass"), Outcome.NewObjectClassPath);
        Result->SetStringField(TEXT("oldObjectPath"), Outcome.OldObjectPath);
        Result->SetStringField(TEXT("resolvedObjectPath"), Outcome.ResolvedObjectPath);
        Result->SetBoolField(TEXT("readbackMeasured"), Outcome.bReadbackMeasured);
        Result->SetBoolField(TEXT("resolvesToNewActor"), Outcome.bResolvesToNewActor);
        Result->SetBoolField(TEXT("oldObjectUnbound"), Outcome.bOldObjectUnbound);
        Result->SetBoolField(TEXT("classMismatch"), Outcome.bClassMismatch);
        SetRepointWarnings(Result, Outcome.Warnings);
        return Result;
    }

    static TSharedPtr<FJsonObject> BuildRepointFailureDetails(
        const SequencerBindingUtils::FRepointOutcome& Outcome,
        const FString& OldActorName,
        const FString& NewActorName)
    {
        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        Details->SetStringField(TEXT("bindingGuid"), RepointGuidString(Outcome.BindingGuid));
        Details->SetStringField(TEXT("bindingName"), Outcome.BindingName);
        Details->SetStringField(TEXT("parentBindingGuid"), RepointGuidString(Outcome.ParentGuid));
        Details->SetNumberField(TEXT("locatorCount"), Outcome.LocatorCount);
        Details->SetStringField(TEXT("oldActorName"), OldActorName);
        Details->SetStringField(TEXT("newActorName"), NewActorName);
        Details->SetStringField(TEXT("authoredClass"), Outcome.AuthoredClassPath);
        Details->SetStringField(TEXT("newObjectClass"), Outcome.NewObjectClassPath);
        Details->SetStringField(TEXT("oldObjectPath"), Outcome.OldObjectPath);
        Details->SetStringField(TEXT("resolvedObjectPath"), Outcome.ResolvedObjectPath);
        Details->SetBoolField(TEXT("readbackMeasured"), Outcome.bReadbackMeasured);
        Details->SetStringField(TEXT("readbackPhase"), RepointReadbackPhaseString(Outcome));
        Details->SetBoolField(TEXT("resolvesToNewActor"), Outcome.bResolvesToNewActor);
        Details->SetBoolField(TEXT("oldObjectUnbound"), Outcome.bOldObjectUnbound);
        Details->SetBoolField(TEXT("classMismatch"), Outcome.bClassMismatch);
        SetRepointWarnings(Details, Outcome.Warnings);
        Details->SetBoolField(TEXT("rollbackAttempted"), Outcome.bRollbackAttempted);
        Details->SetBoolField(TEXT("rollbackSucceeded"), Outcome.bRollbackSucceeded);
        Details->SetStringField(TEXT("restoredResolutionStatus"), Outcome.RestoredResolutionStatus);
        Details->SetStringField(TEXT("restoredResolvedObjectPath"), Outcome.RestoredResolvedObjectPath);
        return Details;
    }
} // namespace SequenceHelpers

// ============================================================================
// sequencer.create
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.create", "Sequencer", "Create a new level sequence asset. Idempotent: an existing ULevelSequence at the target path is returned as-is (existing:true, mode:\"updated_in_place\") with its tracks and bindings intact. Pass overwrite:true to discard and recreate it (ASSET_IN_USE when other packages reference it); ASSET_ALREADY_EXISTS when a different asset class occupies the path.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name of the sequence to create"),
        RPC_PARAM_OPT("path", "path", "Destination folder (defaults to /Game)"),
        RPC_PARAM_DEF("overwrite", "boolean", "Delete and recreate an existing sequence (discarding its tracks) instead of returning it. Rejected with ASSET_IN_USE when any package still references it.", "false")
    ))
{
    FString Name = Ctx.GetString("name");
    FString Path = Ctx.GetString("path");
    if (Name.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, "sequence_create requires name");
        return true;
    }

    FString FullPath = Path.IsEmpty()
        ? FString::Printf(TEXT("/Game/%s"), *Name)
        : FString::Printf(TEXT("%s/%s"), *Path, *Name);

    FString DestFolder = Path.IsEmpty() ? TEXT("/Game") : Path;
    if (DestFolder.StartsWith(TEXT("/Content"), ESearchCase::IgnoreCase))
        DestFolder = FString::Printf(TEXT("/Game%s"), *DestFolder.RightChop(8));

    // Replaces the registry-only DoesAssetExist pre-check: that missed a sequence
    // created earlier this session but not yet registered, which then reached
    // CanCreateAsset's modal chain instead of the early return.
    const AssetCreatePolicy::FResolution Resolution = AssetCreatePolicy::Resolve(
        FullPath, Name, ULevelSequence::StaticClass(), Ctx.GetBool(TEXT("overwrite"), false));
    if (Resolution.IsRejected())
    {
        return AssetCreatePolicy::SendRejection(Ctx, Resolution);
    }
    if (Resolution.Action == AssetCreatePolicy::EAction::UpdateInPlace)
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        AssetCreatePolicy::AddCreateReport(Resp, Resolution);
        VerifyAssetExists(Resp, FullPath);
        Ctx.SendSuccess(Resp);
        return true;
    }

    UClass* FactoryClass = FindObject<UClass>(
        nullptr, TEXT("/Script/LevelSequenceEditor.LevelSequenceFactoryNew"));
    if (!FactoryClass)
        FactoryClass = LoadClass<UClass>(
            nullptr, TEXT("/Script/LevelSequenceEditor.LevelSequenceFactoryNew"));

    if (FactoryClass)
    {
        UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
        FAssetToolsModule& AssetToolsModule =
            FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
        UObject* NewObj = AssetToolsModule.Get().CreateAsset(
            Name, DestFolder, ULevelSequence::StaticClass(), Factory);
        if (NewObj)
        {
            // Persist to disk for real (not the mark-dirty-only McpSafeAssetSave) and
            // report saved honestly, gated on the .uasset actually landing on disk. A
            // fresh ULevelSequence is not a Blueprint/SCS asset, so the bulkdata-corruption
            // vector that pins McpSafeAssetSave on Blueprint edits does not apply here —
            // mirrors the niagara/metasound/level create-save fixes.
            const bool bSaved = SaveAssetToDiskReportingPresence(NewObj, /*bForce=*/true);
            FPluginState::Get().CurrentSequencePath() = FullPath;
            TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
            AssetCreatePolicy::AddCreateReport(Resp, Resolution);
            Resp->SetBoolField(TEXT("saved"), bSaved);
            if (!bSaved)
            {
                Resp->SetBoolField(TEXT("pendingFlush"), true);
            }
            AddAssetVerification(Resp, NewObj);
            Ctx.SendSuccess(Resp);
            return true;
        }
        else
        {
            Ctx.SendError(ErrorCodes::ERR_CREATE_ASSET_FAILED, "Failed to create sequence asset");
            return true;
        }
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_FACTORY_NOT_AVAILABLE,
            "LevelSequenceFactoryNew class not found (Module not loaded?)");
        return true;
    }
}

// ============================================================================
// sequencer.set_display_rate
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.set_display_rate", "Sequencer", "Set the display rate (FPS) of a level sequence",
    RPC_PARAMS(
        RPC_PARAM_OPT("path", "path", "Sequence asset path"),
        RPC_PARAM_REQ("frameRate", "number", "Frame rate (e.g. '30fps', '24000/1001', or numeric)")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    TSharedPtr<FJsonObject> LocalPayload = Payload.IsValid() ? Payload : MakeShared<FJsonObject>();
    FString SeqPath = SequenceHelpers::ResolveSequencePath(LocalPayload);
    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "sequence_set_display_rate requires a sequence path");
        return true;
    }

    UObject* SeqObj = ResolveAsset(SeqPath, /*bLoadObject=*/true).Object;
    if (!SeqObj)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "Sequence not found");
        return true;
    }

    if (ULevelSequence* LevelSeq = Cast<ULevelSequence>(SeqObj))
    {
        if (UMovieScene* MovieScene = LevelSeq->GetMovieScene())
        {
            FString FrameRateStr;
            double FrameRateVal = 0.0;
            FFrameRate NewRate;
            bool bRateFound = false;

            if (LocalPayload->TryGetStringField(TEXT("frameRate"), FrameRateStr))
            {
                if (FrameRateStr.EndsWith(TEXT("fps")))
                {
                    FrameRateStr.RemoveFromEnd(TEXT("fps"));
                    NewRate = FFrameRate(FCString::Atoi(*FrameRateStr), 1);
                    bRateFound = true;
                }
                else if (FrameRateStr.Contains(TEXT("/")))
                {
                    FString NumStr, DenomStr;
                    if (FrameRateStr.Split(TEXT("/"), &NumStr, &DenomStr))
                    {
                        NewRate = FFrameRate(FCString::Atoi(*NumStr), FCString::Atoi(*DenomStr));
                        bRateFound = true;
                    }
                }
                else if (FrameRateStr.IsNumeric())
                {
                    NewRate = FFrameRate(FCString::Atoi(*FrameRateStr), 1);
                    bRateFound = true;
                }
            }
            else if (LocalPayload->TryGetNumberField(TEXT("frameRate"), FrameRateVal))
            {
                NewRate = FFrameRate(FMath::RoundToInt(FrameRateVal), 1);
                bRateFound = true;
            }

            if (bRateFound)
            {
                MovieScene->SetDisplayRate(NewRate);
                MovieScene->Modify();
                TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
                Resp->SetStringField(TEXT("displayRate"), NewRate.ToPrettyText().ToString());
                AddAssetVerification(Resp, LevelSeq);
                Ctx.SendSuccess(Resp);
                return true;
            }

            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, "Invalid frameRate format");
            return true;
        }
    }

    Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "Invalid sequence type");
    return true;
}

// ============================================================================
// sequencer.set_properties
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.set_properties", "Sequencer", "Set playback range, frame rate, and other properties on a level sequence",
    RPC_PARAMS(
        RPC_PARAM_OPT("path", "path", "Sequence asset path"),
        RPC_PARAM_OPT("frameRate", "number", "Display frame rate"),
        RPC_PARAM_OPT("playbackStart", "number", "Playback start (display-rate frame number)"),
        RPC_PARAM_OPT("playbackEnd", "number", "Playback end (display-rate frame number)"),
        RPC_PARAM_OPT("lengthInFrames", "number", "Length in display-rate frames from start")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    TSharedPtr<FJsonObject> LocalPayload = Payload.IsValid() ? Payload : MakeShared<FJsonObject>();
    FString SeqPath = SequenceHelpers::ResolveSequencePath(LocalPayload);
    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "sequence_set_properties requires a sequence path");
        return true;
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    UObject* SeqObj = ResolveAsset(SeqPath, /*bLoadObject=*/true).Object;
    if (!SeqObj)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "Sequence not found");
        return true;
    }

    if (ULevelSequence* LevelSeq = Cast<ULevelSequence>(SeqObj))
    {
        if (UMovieScene* MovieScene = LevelSeq->GetMovieScene())
        {
            bool bModified = false;
            double FrameRateValue = 0.0;
            double LengthInFramesValue = 0.0;
            double PlaybackStartValue = 0.0;
            double PlaybackEndValue = 0.0;

            const bool bHasFrameRate = LocalPayload->TryGetNumberField(TEXT("frameRate"), FrameRateValue);
            const bool bHasLengthInFrames = LocalPayload->TryGetNumberField(TEXT("lengthInFrames"), LengthInFramesValue);
            const bool bHasPlaybackStart = LocalPayload->TryGetNumberField(TEXT("playbackStart"), PlaybackStartValue);
            const bool bHasPlaybackEnd = LocalPayload->TryGetNumberField(TEXT("playbackEnd"), PlaybackEndValue);

            if (bHasFrameRate)
            {
                if (FrameRateValue <= 0.0)
                {
                    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, "frameRate must be > 0");
                    return true;
                }

                const int32 Rounded = FMath::Clamp<int32>(FMath::RoundToInt(FrameRateValue), 1, 960);
                FFrameRate CurrentRate = MovieScene->GetDisplayRate();
                FFrameRate NewRate(Rounded, 1);
                if (NewRate != CurrentRate)
                {
                    MovieScene->SetDisplayRate(NewRate);
                    bModified = true;
                }
            }

            if (bHasPlaybackStart || bHasPlaybackEnd || bHasLengthInFrames)
            {
                TRange<FFrameNumber> ExistingRange = MovieScene->GetPlaybackRange();
                FFrameNumber StartFrame = ExistingRange.GetLowerBoundValue();
                FFrameNumber EndFrame = ExistingRange.GetUpperBoundValue();

                // playbackStart/playbackEnd/lengthInFrames arrive as DISPLAY-RATE frame
                // numbers (the param schema labels them "frame"), but the movie scene stores
                // its playback range in tick-resolution frame numbers. Convert display frames
                // -> ticks through the same SequenceHelpers::DisplayFrameToTick the keyframe
                // path uses, so a caller passing frame 120 at 24fps/24000-tick gets a
                // 120000-tick (5 s) range instead of a bare, unconverted 120-tick (0.005 s)
                // range. lengthInFrames is a frame COUNT, and TransformTime is linear through
                // the origin, so converting it yields the correct tick delta to add to Start.
                if (bHasPlaybackStart)
                    StartFrame = SequenceHelpers::DisplayFrameToTick(MovieScene, PlaybackStartValue);
                if (bHasPlaybackEnd)
                    EndFrame = SequenceHelpers::DisplayFrameToTick(MovieScene, PlaybackEndValue);
                else if (bHasLengthInFrames)
                    EndFrame = StartFrame
                        + SequenceHelpers::DisplayFrameToTick(MovieScene, FMath::Max(0.0, LengthInFramesValue));

                if (EndFrame < StartFrame)
                    EndFrame = StartFrame;
                MovieScene->SetPlaybackRange(TRange<FFrameNumber>(StartFrame, EndFrame));
                bModified = true;
            }

            if (bModified)
                MovieScene->Modify();

            Resp->SetObjectField(TEXT("frameRate"),
                MovieSceneJsonUtils::MakeFrameRateObject(MovieScene->GetDisplayRate()));

            // playbackStart/playbackEnd below are echoed in tick-resolution units (matching
            // sequencer.get_properties); publish tickResolution so the echoed ticks are
            // self-describing and a caller can convert back to display frames.
            Resp->SetObjectField(TEXT("tickResolution"),
                MovieSceneJsonUtils::MakeFrameRateObject(MovieScene->GetTickResolution()));

            TRange<FFrameNumber> Range = MovieScene->GetPlaybackRange();
            const double Start = static_cast<double>(Range.GetLowerBoundValue().Value);
            const double End = static_cast<double>(Range.GetUpperBoundValue().Value);
            Resp->SetNumberField(TEXT("playbackStart"), Start);
            Resp->SetNumberField(TEXT("playbackEnd"), End);
            Resp->SetNumberField(TEXT("duration"), End - Start);
            Resp->SetBoolField(TEXT("applied"), bModified);

            Ctx.SendSuccess(Resp);
            return true;
        }
    }

    Resp->SetObjectField(TEXT("frameRate"), MakeShared<FJsonObject>());
    Resp->SetNumberField(TEXT("playbackStart"), 0.0);
    Resp->SetNumberField(TEXT("playbackEnd"), 0.0);
    Resp->SetNumberField(TEXT("duration"), 0.0);
    Resp->SetBoolField(TEXT("applied"), false);
    Ctx.SendError(ErrorCodes::ERR_NOT_IMPLEMENTED,
        "sequence_set_properties is not available in this editor build or for this sequence type");
    return true;
}

// ============================================================================
// sequencer.add_camera
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.add_camera", "Sequencer", "Spawn a camera and bind it to a level sequence",
    RPC_PARAMS(
        RPC_PARAM_OPT("path", "path", "Sequence asset path")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    TSharedPtr<FJsonObject> LocalPayload = Payload.IsValid() ? Payload : MakeShared<FJsonObject>();
    FString SeqPath = SequenceHelpers::ResolveSequencePath(LocalPayload);
    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "sequence_add_camera requires a sequence path");
        return true;
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    UObject* SeqObj = ResolveAsset(SeqPath, /*bLoadObject=*/true).Object;
    if (!SeqObj)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "Sequence not found");
        return true;
    }

#if MCP_SEQ_HAS_EDITOR_ACTOR_SUBSYSTEM
    if (GEditor)
    {
        UClass* CameraClass = ACameraActor::StaticClass();
        AActor* Spawned = ::SpawnActorInActiveWorld<AActor>(
            CameraClass, FVector::ZeroVector, FRotator::ZeroRotator,
            TEXT("SequenceCamera"));
        if (Spawned)
        {
            if (ULevelSequence* LevelSeq = Cast<ULevelSequence>(SeqObj))
            {
                if (UMovieScene* MovieScene = LevelSeq->GetMovieScene())
                {
                    // Group the possessable-create + object-bind into one undo
                    // transaction (convention: FScopedTransaction after validation,
                    // before the first mutation).
                    const FScopedTransaction Transaction(
                        NSLOCTEXT("PinWright", "SequencerAddCamera", "Add Camera to Sequence"));
                    // Shared actor-possessable path (SequencerBindingUtils::BindActor):
                    // AddPossessable alone mints an object-UNBOUND possessable whose GUID
                    // resolves to no object, so the helper always follows it with
                    // BindPossessableObject against the editor world the resolvers scan.
                    const SequencerBindingUtils::FBindingOutcome Outcome =
                        SequencerBindingUtils::BindActor(LevelSeq, Spawned);
                    if (Outcome.IsOk())
                    {
                        Resp->SetStringField(TEXT("bindingGuid"), Outcome.Guid.ToString());
                    }
                }
            }

            Resp->SetBoolField(TEXT("success"), true);
            Resp->SetStringField(TEXT("actorLabel"), Spawned->GetActorLabel());
            // The spawned camera's actor object path is required by the natural
            // successor sequencer.add_camera_track (its cameraActorPath param, resolved
            // via LoadObject<ACameraActor>). The label alone is not loadable and is a
            // fixed non-unique constant ("SequenceCamera"), so emit the loadable path
            // (and the unique engine-assigned actor name) here to make the
            // add_camera -> add_camera_track workflow chain without an actor.find_by_name
            // detour. Mirrors add_camera_rig_rail/crane returning actorPath.
            Resp->SetStringField(TEXT("cameraActorPath"), Spawned->GetPathName());
            Resp->SetStringField(TEXT("actorName"), Spawned->GetName());
            Ctx.SendSuccess(Resp);
            return true;
        }
    }
    Ctx.SendError(ErrorCodes::ERR_ADD_CAMERA_FAILED, "Failed to add camera");
    return true;
#else
    Ctx.SendError(ErrorCodes::ERR_NOT_AVAILABLE, "UEditorActorSubsystem not available");
    return true;
#endif
}

// ============================================================================
// sequencer.play
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.play", "Sequencer", "Open and play a level sequence in the editor",
    RPC_PARAMS(
        RPC_PARAM_OPT("path", "path", "Sequence asset path")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    TSharedPtr<FJsonObject> LocalPayload = Payload.IsValid() ? Payload : MakeShared<FJsonObject>();
    FString SeqPath = SequenceHelpers::ResolveSequencePath(LocalPayload);
    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "No sequence selected or path provided");
        return true;
    }

    ULevelSequence* LevelSeq =
        Cast<ULevelSequence>(ResolveAsset(SeqPath, /*bLoadObject=*/true).Object);
    if (LevelSeq)
    {
        if (ULevelSequenceEditorBlueprintLibrary::OpenLevelSequence(LevelSeq))
        {
            ULevelSequenceEditorBlueprintLibrary::Play();
            Ctx.SendSuccess(TSharedPtr<FJsonObject>(nullptr));
            return true;
        }
    }
    Ctx.SendError(ErrorCodes::ERR_EXECUTION_ERROR, "Failed to open or play sequence");
    return true;
}

// ============================================================================
// sequencer.add_actor
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.add_actor", "Sequencer",
    "Add a single actor - or one named component of it - to a level sequence as a possessable "
    "binding. With componentName the binding is a NESTED possessable parented to the actor's own "
    "binding, which is what lets a track animate one part of an actor (a wheel, a turret) instead of "
    "moving the whole thing.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name/label of the actor to bind"),
        FParamSpec{TEXT("componentName"), TEXT("string"),
            TEXT("Optional. Internal name of a component ON that actor (UActorComponent::GetName(), "
                 "matched exactly, case-insensitively - the name the Details panel shows, e.g. "
                 "'Wheel0'). Binds the COMPONENT as a nested possessable parented to the actor's "
                 "binding, so a transform track keyed to the returned bindingGuid animates just that "
                 "component. Omit it to bind the whole actor, which is unchanged behaviour. A name "
                 "that is not on the actor is refused with COMPONENT_NOT_FOUND listing every "
                 "component the actor does have."),
            false, TEXT(""), TArray<FString>({TEXT("component_name")})},
        RPC_PARAM_OPT("path", "path", "Sequence asset path")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    TSharedPtr<FJsonObject> LocalPayload = Payload.IsValid() ? Payload : MakeShared<FJsonObject>();
    FString ActorName = Ctx.GetString("actorName");
    if (ActorName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, "actorName required");
        return true;
    }
    FString ComponentName = Ctx.GetString(TEXT("componentName"));
    if (ComponentName.IsEmpty())
    {
        ComponentName = Ctx.GetString(TEXT("component_name"));
    }

    FString SeqPath = SequenceHelpers::ResolveSequencePath(LocalPayload);
    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "sequence_add_actor requires a sequence path");
        return true;
    }

    // Forward to add_actors logic with a single-element array
    UObject* SeqObj = ResolveAsset(SeqPath, /*bLoadObject=*/true).Object;
    if (!SeqObj)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "Sequence not found");
        return true;
    }
    if (!GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, "Editor not available");
        return true;
    }

#if MCP_SEQ_HAS_EDITOR_ACTOR_SUBSYSTEM
    if (UEditorActorSubsystem* ActorSS = GEditor->GetEditorSubsystem<UEditorActorSubsystem>())
    {
        TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("name"), ActorName);

        AActor* Found = Ctx.GetSubsystem()->FindActorByName(ActorName);
        if (!Found)
        {
            Item->SetBoolField(TEXT("success"), false);
            Item->SetStringField(TEXT("error"), TEXT("Actor not found"));
        }
        else
        {
            if (ULevelSequence* LevelSeq = Cast<ULevelSequence>(SeqObj))
            {
                UMovieScene* MovieScene = LevelSeq->GetMovieScene();
                if (MovieScene)
                {
                    // The named component must exist on THIS actor - a near miss is refused
                    // rather than bound to something else, and the rejection names every
                    // component the actor really has so the caller fixes the spelling without
                    // a second round trip. Resolved BEFORE the transaction opens: the
                    // convention here is FScopedTransaction after validation, before the
                    // first mutation.
                    UActorComponent* Component = nullptr;
                    if (!ComponentName.IsEmpty())
                    {
                        Component = SequencerBindingUtils::FindComponentByName(Found, ComponentName);
                        if (!Component)
                        {
                            TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
                            ErrData->SetStringField(TEXT("actorLabel"), Found->GetActorLabel());
                            ErrData->SetStringField(TEXT("actorObjectName"), Found->GetName());
                            SequencerBindingUtils::FillComponentNotFoundFields(
                                ErrData, ComponentName, Found);
                            Ctx.SendError(ErrorCodes::ERR_COMPONENT_NOT_FOUND,
                                SequencerBindingUtils::DescribeComponentNotFound(ComponentName, Found),
                                ErrData);
                            return true;
                        }
                    }

                    // Group the possessable-create + object-bind into one undo
                    // transaction (convention: FScopedTransaction after validation,
                    // before the first mutation).
                    const FScopedTransaction Transaction(
                        NSLOCTEXT("PinWright", "SequencerAddActor", "Add Actor to Sequence"));

                    if (Component)
                    {
                        const SequencerBindingUtils::FBindingOutcome Outcome =
                            SequencerBindingUtils::BindComponent(LevelSeq, Component);
                        if (!Outcome.IsOk())
                        {
                            TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
                            ErrData->SetStringField(TEXT("actorLabel"), Found->GetActorLabel());
                            ErrData->SetStringField(TEXT("componentName"), Component->GetName());
                            Ctx.SendError(Outcome.ErrorCode, Outcome.ErrorMessage, ErrData);
                            return true;
                        }

                        Item->SetBoolField(TEXT("success"), true);
                        SequencerBindingUtils::FillComponentBindingFields(
                            Item, Component->GetName(), Outcome);
                    }
                    else
                    {
                        const SequencerBindingUtils::FBindingOutcome Outcome =
                            SequencerBindingUtils::BindActor(LevelSeq, Found);
                        if (Outcome.IsOk())
                        {
                            Item->SetBoolField(TEXT("success"), true);
                            Item->SetStringField(TEXT("bindingGuid"), Outcome.Guid.ToString());
                        }
                        else
                        {
                            Item->SetBoolField(TEXT("success"), false);
                            Item->SetStringField(TEXT("error"), TEXT("Failed to create possessable binding"));
                        }
                    }
                }
                else
                {
                    Item->SetBoolField(TEXT("success"), false);
                    Item->SetStringField(TEXT("error"), TEXT("Sequence has no MovieScene"));
                }
            }
            else
            {
                Item->SetBoolField(TEXT("success"), false);
                Item->SetStringField(TEXT("error"), TEXT("Sequence object is not a LevelSequence"));
            }
        }

        TArray<TSharedPtr<FJsonValue>> Results;
        Results.Add(MakeShared<FJsonValueObject>(Item));
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetArrayField(TEXT("results"), Results);
        Ctx.SendSuccess(Out);
        return true;
    }
    Ctx.SendError(ErrorCodes::ERR_EDITOR_ACTOR_SUBSYSTEM_MISSING, "EditorActorSubsystem not available");
    return true;
#else
    Ctx.SendError(ErrorCodes::ERR_NOT_AVAILABLE, "UEditorActorSubsystem not available");
    return true;
#endif
}

// ============================================================================
// sequencer.add_actors
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.add_actors", "Sequencer",
    "Add multiple actors - or the same named component of each of them - to a level sequence as "
    "possessable bindings. One componentName applies to every actor in the batch, which is the shape "
    "the multi-part-object case wants (bind 'Wheel0' across six cart actors in one call, then "
    "'Wheel1').",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorNames", "array", "Array of actor name strings to bind"),
        FParamSpec{TEXT("componentName"), TEXT("string"),
            TEXT("Optional. Internal name of a component present on EACH named actor "
                 "(UActorComponent::GetName(), matched exactly, case-insensitively). Binds that "
                 "component as a nested possessable parented to its actor's binding, per actor. Omit "
                 "it to bind whole actors, which is unchanged behaviour. An actor that has no such "
                 "component gets a per-item failure carrying errorCode COMPONENT_NOT_FOUND and the "
                 "list of components it does have; the rest of the batch still runs."),
            false, TEXT(""), TArray<FString>({TEXT("component_name")})},
        RPC_PARAM_OPT("path", "path", "Sequence asset path")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    TSharedPtr<FJsonObject> LocalPayload = Payload.IsValid() ? Payload : MakeShared<FJsonObject>();
    const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
    LocalPayload->TryGetArrayField(TEXT("actorNames"), Arr);
    if (!Arr || Arr->Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, "actorNames required");
        return true;
    }
    FString ComponentName = Ctx.GetString(TEXT("componentName"));
    if (ComponentName.IsEmpty())
    {
        ComponentName = Ctx.GetString(TEXT("component_name"));
    }

    FString SeqPath = SequenceHelpers::ResolveSequencePath(LocalPayload);
    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "sequence_add_actors requires a sequence path");
        return true;
    }

    TArray<FString> Names;
    Names.Reserve(Arr->Num());
    for (const TSharedPtr<FJsonValue>& V : *Arr)
    {
        if (V.IsValid() && V->Type == EJson::String)
            Names.Add(V->AsString());
    }

    UObject* SeqObj = ResolveAsset(SeqPath, /*bLoadObject=*/true).Object;
    if (!SeqObj)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "Sequence not found");
        return true;
    }
    if (!GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, "Editor not available");
        return true;
    }

#if MCP_SEQ_HAS_EDITOR_ACTOR_SUBSYSTEM
    if (UEditorActorSubsystem* ActorSS = GEditor->GetEditorSubsystem<UEditorActorSubsystem>())
    {
        TArray<TSharedPtr<FJsonValue>> Results;
        Results.Reserve(Names.Num());
        // Group every possessable-create + object-bind in this batch into one undo
        // transaction (convention: FScopedTransaction after validation, before the
        // first mutation).
        const FScopedTransaction Transaction(
            NSLOCTEXT("PinWright", "SequencerAddActors", "Add Actors to Sequence"));
        for (const FString& Name : Names)
        {
            TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("name"), Name);
            AActor* Found = Ctx.GetSubsystem()->FindActorByName(Name);

            if (!Found)
            {
                Item->SetBoolField(TEXT("success"), false);
                Item->SetStringField(TEXT("error"), TEXT("Actor not found"));
            }
            else
            {
                if (ULevelSequence* LevelSeq = Cast<ULevelSequence>(SeqObj))
                {
                    UMovieScene* MovieScene = LevelSeq->GetMovieScene();
                    if (MovieScene)
                    {
                        // Component branch: a missing component on ONE actor is that item's
                        // failure, not the batch's - the remaining actors still bind, and the
                        // row names the components this actor does have so the caller can tell
                        // a typo from a genuinely different actor (rpc-design.md Sec.7/Sec.8).
                        UActorComponent* Component = nullptr;
                        bool bComponentMissing = false;
                        if (!ComponentName.IsEmpty())
                        {
                            Component = SequencerBindingUtils::FindComponentByName(Found, ComponentName);
                            bComponentMissing = (Component == nullptr);
                        }

                        if (bComponentMissing)
                        {
                            Item->SetBoolField(TEXT("success"), false);
                            Item->SetStringField(TEXT("errorCode"), ErrorCodes::ERR_COMPONENT_NOT_FOUND);
                            Item->SetStringField(TEXT("error"),
                                SequencerBindingUtils::DescribeComponentNotFound(ComponentName, Found));
                            SequencerBindingUtils::FillComponentNotFoundFields(
                                Item, ComponentName, Found);
                        }
                        else if (Component)
                        {
                            const SequencerBindingUtils::FBindingOutcome Outcome =
                                SequencerBindingUtils::BindComponent(LevelSeq, Component);
                            if (Outcome.IsOk())
                            {
                                Item->SetBoolField(TEXT("success"), true);
                                SequencerBindingUtils::FillComponentBindingFields(
                                    Item, Component->GetName(), Outcome);
                            }
                            else
                            {
                                Item->SetBoolField(TEXT("success"), false);
                                Item->SetStringField(TEXT("errorCode"), Outcome.ErrorCode);
                                Item->SetStringField(TEXT("error"), Outcome.ErrorMessage);
                            }
                        }
                        else
                        {
                            const SequencerBindingUtils::FBindingOutcome Outcome =
                                SequencerBindingUtils::BindActor(LevelSeq, Found);
                            if (Outcome.IsOk())
                            {
                                Item->SetBoolField(TEXT("success"), true);
                                Item->SetStringField(TEXT("bindingGuid"), Outcome.Guid.ToString());
                            }
                            else
                            {
                                Item->SetBoolField(TEXT("success"), false);
                                Item->SetStringField(TEXT("error"), TEXT("Failed to create possessable binding"));
                            }
                        }
                    }
                    else
                    {
                        Item->SetBoolField(TEXT("success"), false);
                        Item->SetStringField(TEXT("error"), TEXT("Sequence has no MovieScene"));
                    }
                }
                else
                {
                    Item->SetBoolField(TEXT("success"), false);
                    Item->SetStringField(TEXT("error"), TEXT("Sequence object is not a LevelSequence"));
                }
            }
            Results.Add(MakeShared<FJsonValueObject>(Item));
        }
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetArrayField(TEXT("results"), Results);
        Ctx.SendSuccess(Out);
        return true;
    }
    Ctx.SendError(ErrorCodes::ERR_EDITOR_ACTOR_SUBSYSTEM_MISSING, "EditorActorSubsystem not available");
    return true;
#else
    Ctx.SendError(ErrorCodes::ERR_NOT_AVAILABLE, "UEditorActorSubsystem not available");
    return true;
#endif
}

// ============================================================================
// sequencer.repoint_actor
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.repoint_actor", "Sequencer",
    "Replace the locator for one existing top-level possessable actor while preserving its binding GUID and authored data",
    RPC_PARAMS(
        RPC_PARAM_REQ("path", "path", "Sequence asset path"),
        RPC_PARAM_REQ("bindingGuid", "string", "GUID of the top-level possessable binding"),
        RPC_PARAM_REQ("oldActorName", "string", "Name or label of the actor currently bound"),
        RPC_PARAM_REQ("newActorName", "string", "Name or label of the actor to bind instead")
    ))
{
#if !UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    // The verb rewrites the FUniversalObjectLocator on a possessable's binding reference. UE 5.3
    // predates the locator model entirely - it stores bindings as FLevelSequenceBindingReferences
    // holding package/object path strings - so there is no locator to replace and no
    // FMovieSceneBindingReferences to replace it on. Refused by name rather than served a
    // different operation under the same verb; SequencerBindingUtils.h compiles the machinery out
    // on the same gate.
    Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_ENGINE_VERSION, TEXT(
        "sequencer.repoint_actor needs the universal-object-locator binding model "
        "(FMovieSceneBindingReferences / UMovieSceneSequence::MakeLocatorForObject), added in "
        "UE 5.4. On this engine, remove and re-add the binding instead."));
    return true;
#else
    auto Payload = Ctx.GetRawPayload();
    TSharedPtr<FJsonObject> LocalPayload = Payload.IsValid() ? Payload : MakeShared<FJsonObject>();
    FString Path;
    FString BindingGuidString;
    FString OldActorName;
    FString NewActorName;
    SequencerBindingUtils::FRepointOutcome Outcome;

    auto SendFailure = [&]() -> bool
    {
        const FString ErrorCode = Outcome.ErrorCode.IsEmpty()
            ? FString(ErrorCodes::ERR_BINDING_FAILED)
            : Outcome.ErrorCode;
        const FString ErrorMessage = Outcome.ErrorMessage.IsEmpty()
            ? FString(TEXT("sequencer.repoint_actor failed"))
            : Outcome.ErrorMessage;
        Ctx.SendError(
            ErrorCode,
            ErrorMessage,
            SequenceHelpers::BuildRepointFailureDetails(Outcome, OldActorName, NewActorName));
        return true;
    };

    auto Fail = [&](const FString& ErrorCode, const FString& ErrorMessage) -> bool
    {
        Outcome.Fail(ErrorCode, ErrorMessage);
        return SendFailure();
    };

    auto RequireRepointString = [&](const TCHAR* Key, FString& Value) -> bool
    {
        if (!Payload.IsValid() || !Payload->HasField(Key))
        {
            return Fail(
                ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("sequencer.repoint_actor requires field '%s'"), Key));
        }
        if (!Payload->HasTypedField<EJson::String>(Key))
        {
            return Fail(
                ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("sequencer.repoint_actor field '%s' must be a string"), Key));
        }

        Value = Payload->GetStringField(Key);
        if (Value.IsEmpty())
        {
            return Fail(
                ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("sequencer.repoint_actor field '%s' must not be empty"), Key));
        }
        return true;
    };

    if (!RequireRepointString(TEXT("path"), Path)
        || !RequireRepointString(TEXT("bindingGuid"), BindingGuidString)
        || !RequireRepointString(TEXT("oldActorName"), OldActorName)
        || !RequireRepointString(TEXT("newActorName"), NewActorName))
    {
        return true;
    }

    if (OldActorName.Equals(NewActorName, ESearchCase::IgnoreCase))
    {
        return Fail(
            ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("oldActorName and newActorName must identify different actors"));
    }

    FGuid BindingGuid;
    if (!FGuid::Parse(BindingGuidString, BindingGuid) || !BindingGuid.IsValid())
    {
        return Fail(
            ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("bindingGuid is not a valid GUID: '%s'"), *BindingGuidString));
    }
    Outcome.BindingGuid = BindingGuid;

    const FString SequencePath = SequenceHelpers::ResolveSequencePath(LocalPayload);
    if (SequencePath.IsEmpty())
    {
        return Fail(
            ErrorCodes::ERR_SEQUENCE_NOT_FOUND,
            TEXT("sequencer.repoint_actor requires a usable sequence path"));
    }

    UObject* SequenceObject = SequencePath.StartsWith(TEXT("/Engine/Transient/"))
        ? LoadObject<UObject>(nullptr, *SequencePath)
        : ResolveAsset(SequencePath, /*bLoadObject=*/true).Object;
    if (!SequenceObject)
    {
        // Transient fixtures are not asset-registry entries; the object-path load above is the
        // normal path for them. Keep this fallback for a transient object that was not loaded yet.
        SequenceObject = LoadObject<UObject>(nullptr, *SequencePath);
    }
    ULevelSequence* LevelSequence = Cast<ULevelSequence>(SequenceObject);
    if (!LevelSequence)
    {
        return Fail(
            ErrorCodes::ERR_SEQUENCE_NOT_FOUND,
            FString::Printf(TEXT("Level sequence not found: %s"), *SequencePath));
    }

    UMovieScene* MovieScene = LevelSequence->GetMovieScene();
    if (!MovieScene)
    {
        return Fail(
            ErrorCodes::ERR_SEQUENCE_NOT_FOUND,
            FString::Printf(TEXT("Level sequence has no MovieScene: %s"), *SequencePath));
    }

    FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuid);
    if (!Binding)
    {
        return Fail(
            ErrorCodes::ERR_BINDING_NOT_FOUND,
            FString::Printf(TEXT("Binding '%s' was not found in sequence %s"),
                *BindingGuidString, *SequencePath));
    }

    FMovieScenePossessable* Possessable = MovieScene->FindPossessable(BindingGuid);
    if (!Possessable)
    {
        if (FMovieSceneSpawnable* Spawnable = MovieScene->FindSpawnable(BindingGuid))
        {
            Outcome.BindingName = Spawnable->GetName();
        }
        return Fail(
            ErrorCodes::ERR_UNSUPPORTED_OPERATION,
            TEXT("sequencer.repoint_actor supports possessable actor bindings only"));
    }

    Outcome.BindingName = Possessable->GetName();
    Outcome.ParentGuid = Possessable->GetParent();
    Outcome.AuthoredClass = const_cast<UClass*>(Possessable->GetPossessedObjectClass());
    Outcome.AuthoredClassPath = Outcome.AuthoredClass
        ? Outcome.AuthoredClass->GetPathName()
        : FString();

    FMovieSceneBindingReferences* BindingReferences =
        static_cast<UMovieSceneSequence*>(LevelSequence)->GetBindingReferences();
    if (!BindingReferences)
    {
        return Fail(
            ErrorCodes::ERR_UNSUPPORTED_OPERATION,
            TEXT("sequence has no possessable binding references"));
    }

    const TArrayView<const FMovieSceneBindingReference> References =
        BindingReferences->GetReferences(BindingGuid);
    Outcome.LocatorCount = References.Num();
    if (References.Num() != 1 || SequencerBindingUtils::HasCustomBinding(References[0])
        || References[0].Locator.IsEmpty())
    {
        return Fail(
            ErrorCodes::ERR_UNSUPPORTED_OPERATION,
            TEXT("sequencer.repoint_actor requires exactly one non-custom possessable locator"));
    }

    if (Outcome.ParentGuid.IsValid())
    {
        return Fail(
            ErrorCodes::ERR_UNSUPPORTED_OPERATION,
            TEXT("sequencer.repoint_actor does not support parented or component possessables"));
    }

    UPinWrightSubsystem* Subsystem = Ctx.GetSubsystem();
    AActor* OldActor = Subsystem ? Subsystem->FindActorByName(OldActorName) : nullptr;
    if (!OldActor)
    {
        return Fail(
            ErrorCodes::ERR_ACTOR_NOT_FOUND,
            FString::Printf(TEXT("Old actor not found: %s"), *OldActorName));
    }
    Outcome.OldObjectPath = OldActor->GetPathName();

    AActor* NewActor = Subsystem ? Subsystem->FindActorByName(NewActorName) : nullptr;
    if (!NewActor)
    {
        return Fail(
            ErrorCodes::ERR_ACTOR_NOT_FOUND,
            FString::Printf(TEXT("New actor not found: %s"), *NewActorName));
    }
    Outcome.NewObjectClass = NewActor->GetClass();
    Outcome.NewObjectClassPath = Outcome.NewObjectClass
        ? Outcome.NewObjectClass->GetPathName()
        : FString();
    Outcome.bClassMismatch = Outcome.AuthoredClass != Outcome.NewObjectClass;
    if (Outcome.bClassMismatch)
    {
        Outcome.Warnings.Add(FString::Printf(
            TEXT("possessable authored class %s differs from new object class %s; authored class metadata retained"),
            *Outcome.AuthoredClassPath,
            *Outcome.NewObjectClassPath));
    }

    Outcome = SequencerBindingUtils::RepointTopLevelPossessableActor(
        LevelSequence, BindingGuid, OldActor, NewActor);
    if (!Outcome.bSuccess)
    {
        return SendFailure();
    }

    Ctx.SendSuccess(SequenceHelpers::BuildRepointSuccessResult(Outcome));
    return true;
#endif // UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
}

// ============================================================================
// sequencer.add_spawnable_from_class
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.add_spawnable_from_class", "Sequencer", "Add a spawnable object to a level sequence from a class name",
    RPC_PARAMS(
        RPC_PARAM_REQ("className", "classref", "Class name or asset path to spawn"),
        RPC_PARAM_OPT("path", "path", "Sequence asset path")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    TSharedPtr<FJsonObject> LocalPayload = Payload.IsValid() ? Payload : MakeShared<FJsonObject>();
    FString ClassName;
    LocalPayload->TryGetStringField(TEXT("className"), ClassName);
    if (ClassName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, "className required");
        return true;
    }

    FString SeqPath = SequenceHelpers::ResolveSequencePath(LocalPayload);
    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "sequence_add_spawnable_from_class requires a sequence path");
        return true;
    }

    UObject* SeqObj = ResolveAsset(SeqPath, /*bLoadObject=*/true).Object;
    if (!SeqObj)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "Sequence not found");
        return true;
    }

    UClass* ResolvedClass = nullptr;
    if (ClassName.StartsWith(TEXT("/")) || ClassName.Contains(TEXT("/")))
    {
        if (UObject* Loaded = ResolveAsset(ClassName, /*bLoadObject=*/true).Object)
        {
            if (UBlueprint* BP = Cast<UBlueprint>(Loaded))
                ResolvedClass = BP->GeneratedClass;
            else if (UClass* C = Cast<UClass>(Loaded))
                ResolvedClass = C;
        }
    }
    if (!ResolvedClass)
        ResolvedClass = ResolveClassByName(ClassName);
    if (!ResolvedClass)
    {
        Ctx.SendError(ErrorCodes::ERR_CLASS_NOT_FOUND, "Class not found");
        return true;
    }

    if (ULevelSequence* LevelSeq = Cast<ULevelSequence>(SeqObj))
    {
        UMovieScene* MovieScene = LevelSeq->GetMovieScene();
        if (MovieScene)
        {
            UObject* DefaultObject = ResolvedClass->GetDefaultObject();
            if (DefaultObject)
            {
                FGuid BindingGuid = MovieScene->AddSpawnable(ClassName, *DefaultObject);
                if (MovieScene->FindSpawnable(BindingGuid))
                {
                    MovieScene->Modify();
                    TSharedPtr<FJsonObject> SpawnableResp = MakeShared<FJsonObject>();
                    SpawnableResp->SetBoolField(TEXT("success"), true);
                    SpawnableResp->SetStringField(TEXT("className"), ClassName);
                    SpawnableResp->SetStringField(TEXT("bindingGuid"), BindingGuid.ToString());
                    Ctx.SendSuccess(SpawnableResp);
                    return true;
                }
            }
        }
        Ctx.SendError(ErrorCodes::ERR_SPAWNABLE_CREATION_FAILED, "Failed to create spawnable binding");
        return true;
    }
    Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE_TYPE, "Sequence object is not a LevelSequence");
    return true;
}

// ============================================================================
// sequencer.remove_actors
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.remove_actors", "Sequencer", "Remove actor bindings from a level sequence",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorNames", "array", "Array of actor name strings to remove"),
        RPC_PARAM_OPT("path", "path", "Sequence asset path")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    TSharedPtr<FJsonObject> LocalPayload = Payload.IsValid() ? Payload : MakeShared<FJsonObject>();
    const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
    LocalPayload->TryGetArrayField(TEXT("actorNames"), Arr);
    if (!Arr || Arr->Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, "actorNames required");
        return true;
    }

    FString SeqPath = SequenceHelpers::ResolveSequencePath(LocalPayload);
    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "sequence_remove_actors requires a sequence path");
        return true;
    }

    UObject* SeqObj = ResolveAsset(SeqPath, /*bLoadObject=*/true).Object;
    if (!SeqObj)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "Sequence not found");
        return true;
    }
    if (!GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, "Editor not available");
        return true;
    }

#if MCP_SEQ_HAS_EDITOR_ACTOR_SUBSYSTEM
    if (UEditorActorSubsystem* ActorSS = GEditor->GetEditorSubsystem<UEditorActorSubsystem>())
    {
        TArray<TSharedPtr<FJsonValue>> Removed;
        int32 RemovedCount = 0;
        for (const TSharedPtr<FJsonValue>& V : *Arr)
        {
            if (!V.IsValid() || V->Type != EJson::String)
                continue;
            FString Name = V->AsString();
            TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("name"), Name);

            // Resolve the caller's identifier through the SAME name-or-label-or-path resolver
            // sequencer.add_actors uses (FindActorByName), so we can also match on the resolved
            // actor's display label — the identity the binding is stored under, since
            // AddPossessable stores GetActorLabel(). Without this, an actor bound by its internal
            // object name could never be removed by that same name (the binding is label-keyed).
            // The match below is two-pass: an exact match on the caller's own string wins over a
            // resolved-label match, and the direct comparison also serves as the fallback for
            // bindings that resolve to no live actor — spawnables, or possessables whose actor was
            // since deleted — which remain removable by the exact name get_bindings reports.
            FString ResolvedLabel;
            if (UPinWrightSubsystem* Sub = Ctx.GetSubsystem())
            {
                if (AActor* ResolvedActor = Sub->FindActorByName(Name))
                {
                    ResolvedLabel = ResolvedActor->GetActorLabel();
                }
            }

            if (ULevelSequence* LevelSeq = Cast<ULevelSequence>(SeqObj))
            {
                UMovieScene* MovieScene = LevelSeq->GetMovieScene();
                if (MovieScene)
                {
                    auto GetBindingName = [MovieScene](const FMovieSceneBinding& Binding) -> FString
                    {
                        if (FMovieScenePossessable* Possessable =
                                MovieScene->FindPossessable(Binding.GetObjectGuid()))
                            return Possessable->GetName();
                        if (FMovieSceneSpawnable* Spawnable =
                                MovieScene->FindSpawnable(Binding.GetObjectGuid()))
                            return Spawnable->GetName();
                        return FString();
                    };

                    // Two passes so an EXACT match on the caller's own string always wins over a
                    // resolved-label match: with an internal-name/label collision two bindings can
                    // be candidates — one whose stored name equals the arg verbatim, one whose name
                    // equals the arg's resolved actor label — and a single OR test with
                    // break-on-first-binding would remove whichever iterated first, not the one the
                    // caller named verbatim.
                    FGuid TargetGuid;
                    bool bFound = false;
                    for (const FMovieSceneBinding& Binding :
                         const_cast<const UMovieScene*>(MovieScene)->GetBindings())
                    {
                        if (GetBindingName(Binding).Equals(Name, ESearchCase::IgnoreCase))
                        {
                            TargetGuid = Binding.GetObjectGuid();
                            bFound = true;
                            break;
                        }
                    }
                    if (!bFound && !ResolvedLabel.IsEmpty())
                    {
                        for (const FMovieSceneBinding& Binding :
                             const_cast<const UMovieScene*>(MovieScene)->GetBindings())
                        {
                            if (GetBindingName(Binding).Equals(ResolvedLabel, ESearchCase::IgnoreCase))
                            {
                                TargetGuid = Binding.GetObjectGuid();
                                bFound = true;
                                break;
                            }
                        }
                    }

                    bool bRemoved = false;
                    if (bFound)
                    {
                        MovieScene->Modify();
                        // RemovePossessable only searches the Possessables array and returns
                        // false for a spawnable GUID (engine MovieScene.cpp), so a matched
                        // spawnable binding was previously a silent no-op reported as success.
                        // The name matcher accepts both possessable and spawnable bindings, so
                        // fall through to RemoveSpawnable when the possessable removal misses,
                        // and report success only when something was actually removed.
                        bRemoved = MovieScene->RemovePossessable(TargetGuid);
                        if (!bRemoved)
                        {
                            bRemoved = MovieScene->RemoveSpawnable(TargetGuid);
                        }
                    }
                    if (bRemoved)
                    {
                        Item->SetBoolField(TEXT("success"), true);
                        Item->SetStringField(TEXT("status"), TEXT("Actor removed"));
                        RemovedCount++;
                    }
                    else if (bFound)
                    {
                        Item->SetBoolField(TEXT("success"), false);
                        Item->SetStringField(TEXT("error"), FString::Printf(
                            TEXT("Matched a binding for '%s' but the engine removed neither a "
                                 "possessable nor a spawnable under its GUID."), *Name));
                    }
                    else
                    {
                        Item->SetBoolField(TEXT("success"), false);
                        Item->SetStringField(TEXT("error"), FString::Printf(
                            TEXT("No sequence binding matches '%s' by internal name or display "
                                 "label. remove_actors matches each actorNames entry against the "
                                 "binding names sequencer.get_bindings reports (the actor's display "
                                 "label), resolving internal object names through the same resolver "
                                 "add_actors uses."), *Name));
                    }
                }
                else
                {
                    Item->SetBoolField(TEXT("success"), false);
                    Item->SetStringField(TEXT("error"), TEXT("Sequence has no MovieScene"));
                }
            }
            else
            {
                Item->SetBoolField(TEXT("success"), false);
                Item->SetStringField(TEXT("error"), TEXT("Sequence object is not a LevelSequence"));
            }
            Removed.Add(MakeShared<FJsonValueObject>(Item));
        }
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetArrayField(TEXT("removedActors"), Removed);
        Out->SetNumberField(TEXT("bindingsProcessed"), RemovedCount);
        Ctx.SendSuccess(Out);
        return true;
    }
    Ctx.SendError(ErrorCodes::ERR_EDITOR_ACTOR_SUBSYSTEM_MISSING, "EditorActorSubsystem not available");
    return true;
#else
    Ctx.SendError(ErrorCodes::ERR_NOT_AVAILABLE, "UEditorActorSubsystem not available");
    return true;
#endif
}

// ============================================================================
// sequencer.get_bindings
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.get_bindings", "Sequencer",
    "List every object binding in a level sequence, with its place in the binding hierarchy. Each row "
    "carries kind (possessable/spawnable) and parentId - the GUID of the binding it is nested under, "
    "empty for a top-level one. Without parentId a component binding is indistinguishable from an "
    "actor binding in this output, and a component binding whose parent link is missing resolves to "
    "nothing at playback.",
    RPC_PARAMS(
        RPC_PARAM_OPT("path", "path", "Sequence asset path")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    TSharedPtr<FJsonObject> LocalPayload = Payload.IsValid() ? Payload : MakeShared<FJsonObject>();
    FString SeqPath = SequenceHelpers::ResolveSequencePath(LocalPayload);
    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "sequence_get_bindings requires a sequence path");
        return true;
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    UObject* SeqObj = ResolveAsset(SeqPath, /*bLoadObject=*/true).Object;
    if (!SeqObj)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "Sequence not found");
        return true;
    }

    if (ULevelSequence* LevelSeq = Cast<ULevelSequence>(SeqObj))
    {
        if (UMovieScene* MovieScene = LevelSeq->GetMovieScene())
        {
            TArray<TSharedPtr<FJsonValue>> BindingsArray;
            for (const FMovieSceneBinding& B :
                 const_cast<const UMovieScene*>(MovieScene)->GetBindings())
            {
                TSharedPtr<FJsonObject> Bobj = MakeShared<FJsonObject>();
                Bobj->SetStringField(TEXT("id"), B.GetObjectGuid().ToString());

                // parentId is read off FMovieScenePossessable::GetParent(), the same field
                // MovieSceneHelpers::GetResolutionContext consults to decide whether a
                // binding resolves against its parent object or against the world. A
                // component binding is a possessable with a valid parent; an actor binding is
                // one without. Emitted for every row (empty string = top-level) so a caller
                // can see the hierarchy it just created rather than infer it from names.
                FString BindingName;
                FString ParentId;
                FString Kind;
                if (FMovieScenePossessable* Possessable =
                        MovieScene->FindPossessable(B.GetObjectGuid()))
                {
                    BindingName = Possessable->GetName();
                    Kind = TEXT("possessable");
                    if (Possessable->GetParent().IsValid())
                        ParentId = Possessable->GetParent().ToString();
                }
                else if (FMovieSceneSpawnable* Spawnable =
                             MovieScene->FindSpawnable(B.GetObjectGuid()))
                {
                    BindingName = Spawnable->GetName();
                    Kind = TEXT("spawnable");
                }

                Bobj->SetStringField(TEXT("name"), BindingName);
                Bobj->SetStringField(TEXT("kind"), Kind);
                Bobj->SetStringField(TEXT("parentId"), ParentId);
                BindingsArray.Add(MakeShared<FJsonValueObject>(Bobj));
            }
            Resp->SetArrayField(TEXT("bindings"), BindingsArray);
            Ctx.SendSuccess(Resp);
            return true;
        }
    }
    Resp->SetArrayField(TEXT("bindings"), TArray<TSharedPtr<FJsonValue>>());
    Ctx.SendSuccess(Resp);
    return true;
}

// ============================================================================
// sequencer.get_properties
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.get_properties", "Sequencer", "Get playback range and frame rate of a level sequence",
    RPC_PARAMS(
        RPC_PARAM_OPT("path", "path", "Sequence asset path")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    TSharedPtr<FJsonObject> LocalPayload = Payload.IsValid() ? Payload : MakeShared<FJsonObject>();
    FString SeqPath = SequenceHelpers::ResolveSequencePath(LocalPayload);
    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "sequence_get_properties requires a sequence path");
        return true;
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    UObject* SeqObj = ResolveAsset(SeqPath, /*bLoadObject=*/true).Object;
    if (!SeqObj)
    {
        SeqObj = LoadObject<UObject>(nullptr, *SeqPath);
    }
    if (!SeqObj)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "Sequence not found");
        return true;
    }

    if (ULevelSequence* LevelSeq = Cast<ULevelSequence>(SeqObj))
    {
        if (UMovieScene* MovieScene = LevelSeq->GetMovieScene())
        {
            FFrameRate FR = MovieScene->GetDisplayRate();
            TSharedPtr<FJsonObject> FrameRateObj = MakeShared<FJsonObject>();
            FrameRateObj->SetNumberField(TEXT("numerator"), FR.Numerator);
            FrameRateObj->SetNumberField(TEXT("denominator"), FR.Denominator);
            Resp->SetObjectField(TEXT("frameRate"), FrameRateObj);
            FFrameRate TickResolution = MovieScene->GetTickResolution();
            TSharedPtr<FJsonObject> TickResolutionObj = MakeShared<FJsonObject>();
            TickResolutionObj->SetNumberField(TEXT("numerator"), TickResolution.Numerator);
            TickResolutionObj->SetNumberField(TEXT("denominator"), TickResolution.Denominator);
            Resp->SetObjectField(TEXT("tickResolution"), TickResolutionObj);
            TRange<FFrameNumber> Range = MovieScene->GetPlaybackRange();
            const double Start = static_cast<double>(Range.GetLowerBoundValue().Value);
            const double End = static_cast<double>(Range.GetUpperBoundValue().Value);
            Resp->SetNumberField(TEXT("playbackStart"), Start);
            Resp->SetNumberField(TEXT("playbackEnd"), End);
            Resp->SetNumberField(TEXT("duration"), End - Start);
            Resp->SetNumberField(TEXT("bindingCount"), static_cast<const UMovieScene*>(MovieScene)->GetBindings().Num());
            Resp->SetNumberField(TEXT("spawnableCount"), MovieScene->GetSpawnableCount());
            Resp->SetNumberField(TEXT("possessableCount"), MovieScene->GetPossessableCount());
            Ctx.SendSuccess(Resp);
            return true;
        }
    }
    Resp->SetObjectField(TEXT("frameRate"), MakeShared<FJsonObject>());
    Resp->SetObjectField(TEXT("tickResolution"), MakeShared<FJsonObject>());
    Resp->SetNumberField(TEXT("playbackStart"), 0.0);
    Resp->SetNumberField(TEXT("playbackEnd"), 0.0);
    Resp->SetNumberField(TEXT("duration"), 0.0);
    Resp->SetNumberField(TEXT("bindingCount"), 0.0);
    Resp->SetNumberField(TEXT("spawnableCount"), 0.0);
    Resp->SetNumberField(TEXT("possessableCount"), 0.0);
    Ctx.SendSuccess(Resp);
    return true;
}

// ============================================================================
// sequencer.set_playback_speed
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.set_playback_speed", "Sequencer", "Set playback speed on the currently open sequencer",
    RPC_PARAMS(
        RPC_PARAM_OPT("path", "path", "Sequence asset path"),
        RPC_PARAM_DEF("speed", "number", "Playback speed multiplier", "1.0")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    TSharedPtr<FJsonObject> LocalPayload = Payload.IsValid() ? Payload : MakeShared<FJsonObject>();
    double Speed = 1.0;
    LocalPayload->TryGetNumberField(TEXT("speed"), Speed);
    if (Speed <= 0.0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, "Invalid speed (must be > 0)");
        return true;
    }

    FString SeqPath = SequenceHelpers::ResolveSequencePath(LocalPayload);
    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "sequence_set_playback_speed requires a sequence path");
        return true;
    }

    UObject* SeqObj = ResolveAsset(SeqPath, /*bLoadObject=*/true).Object;
    if (!SeqObj)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "Sequence not found");
        return true;
    }

    if (GEditor)
    {
        if (UAssetEditorSubsystem* AssetEditorSS =
                GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
        {
            IAssetEditorInstance* Editor = AssetEditorSS->FindEditorForAsset(SeqObj, false);
            if (ILevelSequenceEditorToolkit* LSEditor =
                    static_cast<ILevelSequenceEditorToolkit*>(Editor))
            {
                if (LSEditor->GetSequencer().IsValid())
                {
                    LSEditor->GetSequencer()->SetPlaybackSpeed(static_cast<float>(Speed));
                    Ctx.SendSuccess(TSharedPtr<FJsonObject>(nullptr));
                    return true;
                }
            }
        }
    }

    Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_OPEN, "Sequence editor not open or interface unavailable");
    return true;
}

// ============================================================================
// sequencer.pause
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.pause", "Sequencer", "Pause the currently playing level sequence",
    RPC_PARAMS(
        RPC_PARAM_OPT("path", "path", "Sequence asset path")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    TSharedPtr<FJsonObject> LocalPayload = Payload.IsValid() ? Payload : MakeShared<FJsonObject>();
    FString SeqPath = SequenceHelpers::ResolveSequencePath(LocalPayload);
    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "sequence_pause requires a sequence path");
        return true;
    }

    ULevelSequence* LevelSeq =
        Cast<ULevelSequence>(ResolveAsset(SeqPath, /*bLoadObject=*/true).Object);
    if (LevelSeq)
    {
        if (ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence() == LevelSeq)
        {
            ULevelSequenceEditorBlueprintLibrary::Pause();
            Ctx.SendSuccess(TSharedPtr<FJsonObject>(nullptr));
            return true;
        }
    }
    Ctx.SendError(ErrorCodes::ERR_EXECUTION_ERROR, "Sequence not currently open in editor");
    return true;
}

// ============================================================================
// sequencer.stop
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.stop", "Sequencer", "Stop playback and reset to start",
    RPC_PARAMS(
        RPC_PARAM_OPT("path", "path", "Sequence asset path")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    TSharedPtr<FJsonObject> LocalPayload = Payload.IsValid() ? Payload : MakeShared<FJsonObject>();
    FString SeqPath = SequenceHelpers::ResolveSequencePath(LocalPayload);
    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "sequence_stop requires a sequence path");
        return true;
    }

    ULevelSequence* LevelSeq =
        Cast<ULevelSequence>(ResolveAsset(SeqPath, /*bLoadObject=*/true).Object);
    if (LevelSeq)
    {
        if (ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence() == LevelSeq)
        {
            ULevelSequenceEditorBlueprintLibrary::Pause();

#if UE_VERSION_OLDER_THAN(5, 4, 0)
            // The FMovieSceneSequencePlaybackParams-based SetGlobalPosition() was added in UE 5.4;
            // on 5.3 scrub to frame 0 via the simpler SetCurrentTime(int32) API.
            ULevelSequenceEditorBlueprintLibrary::SetCurrentTime(0);
#else
            FMovieSceneSequencePlaybackParams PlaybackParams;
            PlaybackParams.Frame = FFrameTime(0);
            PlaybackParams.UpdateMethod = EUpdatePositionMethod::Scrub;
            ULevelSequenceEditorBlueprintLibrary::SetGlobalPosition(PlaybackParams);
#endif

            Ctx.SendSuccess(TSharedPtr<FJsonObject>(nullptr));
            return true;
        }
    }
    Ctx.SendError(ErrorCodes::ERR_EXECUTION_ERROR, "Sequence not currently open in editor");
    return true;
}

// ============================================================================
// sequencer.set_playhead
// ============================================================================

// ParseUpdateMethod, the open-in-Sequencer precondition and the position write itself now live
// in Handlers/Sequencer/SequencePlayheadUtils.h, so camera.animation_shots steps a burst through
// the very same calls this verb makes. Keeping a private copy here is how one of the two would
// eventually keep a stale engine-version branch or drop ForceUpdate.

REGISTER_RPC_HANDLER("sequencer.set_playhead", "Sequencer",
    "Move the Sequencer playhead to an exact position so the editor evaluates that frame in place, without entering PIE. "
    "This is the deterministic driver for frame bursts (set position -> capture -> repeat): while a sequence is scrubbed, "
    "Sequencer drives bound skeletal meshes (it sets bUpdateAnimationInEditor, which is transient/EditInstanceOnly and "
    "defaults false, so a plain SkeletalMeshActor does NOT animate in the viewport on its own) and evaluates bound Niagara "
    "components at the addressed age. Give the position as 'frame' (display-rate frame number) or 'time' (seconds); "
    "when both are supplied 'frame' is authoritative because it is exact and reproducible frame-to-frame.",
    RPC_PARAMS(
        ParamAliasUtils::MakeAliasParamSpec(TEXT("path"), TEXT("path"),
            TEXT("Sequence asset path (aliases sequencePath / sequence_path). Defaults to the current sequence."),
            /*bRequired=*/false, TArray<FString>({TEXT("path"), TEXT("sequencePath"), TEXT("sequence_path")})),
        ParamAliasUtils::MakeAliasParamSpec(TEXT("frame"), TEXT("integer"),
            TEXT("Target position as an integral DISPLAY-RATE frame number. Authoritative when both frame and time are supplied. Aliases frameNumber / frame_number."),
            /*bRequired=*/false, TArray<FString>({TEXT("frame"), TEXT("frameNumber"), TEXT("frame_number")})),
        ParamAliasUtils::MakeAliasParamSpec(TEXT("time"), TEXT("number"),
            TEXT("Target position in seconds. Converted to a display-rate frame time, sub-frame remainder preserved. Ignored when frame is supplied. Aliases seconds / timeSeconds / time_seconds."),
            /*bRequired=*/false, TArray<FString>({TEXT("time"), TEXT("seconds"), TEXT("timeSeconds"), TEXT("time_seconds")})),
        ParamAliasUtils::MakeAliasParamSpecWithDefault(TEXT("updateMethod"), TEXT("string"),
            TEXT("How Sequencer applies the position: scrub (default, evaluates as a user scrub), jump (no intervening events), play (fires events between the old and new position). Snake_case update_method accepted."),
            TEXT("scrub"), TArray<FString>({TEXT("updateMethod"), TEXT("update_method")})),
        ParamAliasUtils::MakeAliasParamSpecWithDefault(TEXT("forceUpdate"), TEXT("boolean"),
            TEXT("Force one immediate full re-evaluation after moving the playhead so the level reflects the new position before this call returns. Leave on for capture bursts; pass false to skip the refresh when a plain scrub is already sufficient. Snake_case force_update accepted."),
            TEXT("true"), TArray<FString>({TEXT("forceUpdate"), TEXT("force_update")})),
        ParamAliasUtils::MakeAliasParamSpecWithDefault(TEXT("open"), TEXT("boolean"),
            TEXT("Open the sequence in Sequencer when it is not the one currently open. false rejects with SEQUENCE_NOT_OPEN instead of opening it. Aliases openIfNeeded / open_if_needed."),
            TEXT("true"), TArray<FString>({TEXT("open"), TEXT("openIfNeeded"), TEXT("open_if_needed")}))
    ))
{
    auto Payload = Ctx.GetRawPayload();
    TSharedPtr<FJsonObject> LocalPayload = Payload.IsValid() ? Payload : MakeShared<FJsonObject>();

    // Position, read presence-aware so an explicit 0 is honored rather than
    // mistaken for "absent". frame wins over time when both are supplied.
    bool bHasFrame = false;
    bool bHasTime = false;
    int32 FrameIn = 0;
    double TimeIn = 0.0;
    if (Ctx.GetJsonValueFirstOf({TEXT("frame"), TEXT("frameNumber"), TEXT("frame_number")}).IsValid())
    {
        const TOptional<int32> ParsedFrame =
            Ctx.GetIntFirstOf({TEXT("frame"), TEXT("frameNumber"), TEXT("frame_number")});
        if (!ParsedFrame.IsSet())
        {
            return true;
        }
        bHasFrame = true;
        FrameIn = ParsedFrame.GetValue();
    }
    if (TSharedPtr<FJsonValue> TimeVal =
            Ctx.GetJsonValueFirstOf({TEXT("time"), TEXT("seconds"), TEXT("timeSeconds"), TEXT("time_seconds")}))
    {
        bHasTime = TimeVal->TryGetNumber(TimeIn);
    }

    if (!bHasFrame && !bHasTime)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("sequencer.set_playhead requires 'frame' (display-rate frame number) or 'time' (seconds). "
                 "When both are supplied 'frame' is authoritative."));
        return true;
    }

    const TCHAR* PositionFrom = bHasFrame ? TEXT("frame") : TEXT("time");
    const double RequestedValue = bHasFrame ? static_cast<double>(FrameIn) : TimeIn;
    if (!FMath::IsFinite(RequestedValue))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("'%s' must be a finite number"), PositionFrom));
        return true;
    }

    const FString RequestedMethod =
        Ctx.GetStringFirstOf({TEXT("updateMethod"), TEXT("update_method")}, TEXT("scrub"));
    EUpdatePositionMethod UpdateMethod = EUpdatePositionMethod::Scrub;
    FString CanonicalMethod;
    if (!SequencePlayheadUtils::ParseUpdateMethod(RequestedMethod, UpdateMethod, CanonicalMethod))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Unknown updateMethod '%s' (expected 'scrub', 'jump' or 'play')"),
                *RequestedMethod));
        return true;
    }

    // Transport siblings (play/pause/stop/set_playback_speed) name this param
    // 'path'; the track-authoring verbs in SequencerHandler.cpp name it
    // 'sequencePath'. Accept either so callers don't have to know which family
    // they are talking to.
    FString SeqPath = SequenceHelpers::ResolveSequencePath(LocalPayload);
    if (SeqPath.IsEmpty())
    {
        const FString AliasPath = Ctx.GetStringFirstOf({TEXT("sequencePath"), TEXT("sequence_path")});
        if (!AliasPath.IsEmpty())
        {
            TSharedPtr<FJsonObject> AliasPayload = MakeShared<FJsonObject>();
            AliasPayload->SetStringField(TEXT("path"), AliasPath);
            SeqPath = SequenceHelpers::ResolveSequencePath(AliasPayload);
        }
    }
    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE,
            TEXT("sequencer.set_playhead requires a sequence path (none supplied and no current sequence)"));
        return true;
    }

    ULevelSequence* LevelSeq =
        Cast<ULevelSequence>(ResolveAsset(SeqPath, /*bLoadObject=*/true).Object);
    if (!LevelSeq)
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_FOUND,
            FString::Printf(TEXT("Level sequence not found at '%s'"), *SeqPath));
        return true;
    }

    UMovieScene* MovieScene = LevelSeq->GetMovieScene();
    if (!MovieScene)
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_INVALID,
            FString::Printf(TEXT("Level sequence '%s' has no MovieScene"), *SeqPath));
        return true;
    }

    // The ULevelSequenceEditorBlueprintLibrary playhead APIs address whichever
    // sequence is open in Sequencer, never an arbitrary asset — so the target
    // must be the open one before the position is written.
    const bool bOpenIfNeeded =
        Ctx.GetBoolFirstOf({TEXT("open"), TEXT("openIfNeeded"), TEXT("open_if_needed")}, true);
    bool bOpened = false;
    {
        FString OpenErrorCode;
        FString OpenErrorMessage;
        if (!SequencePlayheadUtils::EnsureSequenceOpenInSequencer(
                LevelSeq, bOpenIfNeeded, bOpened, OpenErrorCode, OpenErrorMessage))
        {
            Ctx.SendError(OpenErrorCode, OpenErrorMessage);
            return true;
        }
    }

    const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
    const FFrameRate TickResolution = MovieScene->GetTickResolution();

    // Both inputs land on a DISPLAY-RATE FFrameTime — the unit SetGlobalPosition
    // consumes (its EMovieSceneTimeUnit default is DisplayRate). Seconds keep
    // their sub-frame remainder instead of snapping, so a burst stepping by a
    // non-integral interval stays on its exact grid instead of drifting.
    const FFrameTime TargetDisplayTime =
        bHasFrame ? FFrameTime::FromDecimal(FrameIn) : DisplayRate.AsFrameTime(TimeIn);
    const double ResolvedSeconds = DisplayRate.AsSeconds(TargetDisplayTime);
    const FFrameTime TargetTickTime =
        FFrameRate::TransformTime(TargetDisplayTime, DisplayRate, TickResolution);

    // Scrubbing evaluates, but the editor can still present the previous frame
    // until the next tick — fatal for "set playhead then capture" bursts, which
    // would silently record an off-by-one frame. One immediate refresh closes that.
    const bool bForceUpdate = Ctx.GetBoolFirstOf({TEXT("forceUpdate"), TEXT("force_update")}, true);
    const FString AppliedMethod = SequencePlayheadUtils::ApplyPlayheadPosition(
        TargetDisplayTime, UpdateMethod, CanonicalMethod, bForceUpdate);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("sequencePath"), SeqPath);
    Resp->SetNumberField(TEXT("frame"), TargetDisplayTime.AsDecimal());
    Resp->SetNumberField(TEXT("time"), ResolvedSeconds);
    Resp->SetNumberField(TEXT("tickFrame"), TargetTickTime.AsDecimal());
    Resp->SetObjectField(TEXT("displayRate"), MovieSceneJsonUtils::MakeFrameRateObject(DisplayRate));
    Resp->SetObjectField(TEXT("tickResolution"), MovieSceneJsonUtils::MakeFrameRateObject(TickResolution));
    Resp->SetStringField(TEXT("positionFrom"), PositionFrom);
    Resp->SetStringField(TEXT("updateMethod"), AppliedMethod);
    Resp->SetBoolField(TEXT("forcedUpdate"), bForceUpdate);
    Resp->SetBoolField(TEXT("opened"), bOpened);
    Ctx.SendSuccess(Resp);
    return true;
}

// ============================================================================
// Shared transform-keyframe write path
// ============================================================================
//
// ONE implementation of "author a key on a binding's UMovieScene3DTransformTrack", used by both
// sequence.add_keyframe (one key per call) and sequencer.add_keyframes (a whole path per call).
// The batch verb is a loop over this, not a second copy of it: the two defects the singular path
// carries the fix for — a re-key APPENDING instead of replacing, and a cubic/auto key never getting
// its tangents solved — are both invisible from the wire, so a forked batch path would reintroduce
// them silently and no test of the singular verb would notice.
namespace SequenceKeyframeHelpers
{
    // Which of the transform section's nine double channels a `property` addresses.
    // Layout: 0-2 Location, 3-5 Rotation (Roll, Pitch, Yaw), 6-8 Scale.
    enum class ETransformKeyShape : uint8
    {
        None,       // not a transform property — sequence.add_keyframe falls through to its property-track branches
        FullNine,   // "Transform": a {location, rotation, scale} envelope addressing all nine
        AxisTriple, // "Location" / "Rotation" / "Scale": one three-channel group
    };

    static ETransformKeyShape ClassifyTransformProperty(const FString& PropertyName, int32& OutChannelBase)
    {
        OutChannelBase = 0;
        if (PropertyName.Equals(TEXT("Transform"), ESearchCase::IgnoreCase))
        {
            return ETransformKeyShape::FullNine;
        }
        if (PropertyName.Equals(TEXT("Location"), ESearchCase::IgnoreCase))
        {
            OutChannelBase = 0;
            return ETransformKeyShape::AxisTriple;
        }
        if (PropertyName.Equals(TEXT("Rotation"), ESearchCase::IgnoreCase))
        {
            OutChannelBase = 3;
            return ETransformKeyShape::AxisTriple;
        }
        if (PropertyName.Equals(TEXT("Scale"), ESearchCase::IgnoreCase))
        {
            OutChannelBase = 6;
            return ETransformKeyShape::AxisTriple;
        }
        return ETransformKeyShape::None;
    }

    // How many of the nine channels must exist for a given shape to be writable at all. Preserves
    // the two guards the singular verb's branches had verbatim (>= 9 for the envelope,
    // >= base + 3 for one axis group).
    static int32 RequiredChannelCount(ETransformKeyShape Shape, int32 ChannelBase)
    {
        return Shape == ETransformKeyShape::FullNine ? 9 : ChannelBase + 3;
    }

    // The number of double channels a 3D transform section exposes.
    static constexpr int32 TransformChannelCount = 9;

    // One authored key, fully resolved: which of the nine channels it touches, with what values,
    // and the curve shape stamped on each write. Every key of a batch is resolved into one of these
    // BEFORE any of them is written, which is what makes the batch verb all-or-nothing.
    struct FTransformKeyWrite
    {
        double Frame = 0.0;
        FFrameNumber TickFrame = 0;
        double Axis[TransformChannelCount] = {};
        bool bHasAxis[TransformChannelCount] = {};
        ERichCurveInterpMode InterpMode = RCIM_Cubic;
        ERichCurveTangentMode TangentMode = RCTM_Auto;
        SequencerSectionHelpers::FKeyTangents Tangents;

        bool TouchesAnyChannel() const
        {
            for (int32 ChannelIndex = 0; ChannelIndex < TransformChannelCount; ++ChannelIndex)
            {
                if (bHasAxis[ChannelIndex])
                {
                    return true;
                }
            }
            return false;
        }
    };

    // Read three consecutively-named components off an object into three consecutive channel slots.
    static void ReadNamedAxes(const TSharedPtr<FJsonObject>& Obj, const TCHAR* const* Names,
                              int32 ChannelBase, FTransformKeyWrite& OutKey)
    {
        for (int32 i = 0; i < 3; ++i)
        {
            if (Obj->TryGetNumberField(Names[i], OutKey.Axis[ChannelBase + i]))
            {
                OutKey.bHasAxis[ChannelBase + i] = true;
            }
        }
    }

    // The {x,y,z} / {X,Y,Z} object (rotation also honours {roll,pitch,yaw}) or the bare [a,b,c]
    // array form of a single axis group — verbatim the acceptance sequence.add_keyframe's per-axis
    // branch has always had, hoisted so the batch verb cannot drift from it.
    static void ReadAxisTriple(const TSharedPtr<FJsonValue>& Value, bool bIsRotation,
                               int32 ChannelBase, FTransformKeyWrite& OutKey)
    {
        static const TCHAR* const Keys[3] = { TEXT("x"), TEXT("y"), TEXT("z") };
        static const TCHAR* const RotKeys[3] = { TEXT("roll"), TEXT("pitch"), TEXT("yaw") };

        if (Value.IsValid() && Value->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject> ValObj = Value->AsObject();
            if (ValObj.IsValid())
            {
                for (int32 i = 0; i < 3; ++i)
                {
                    if (ValObj->TryGetNumberField(Keys[i], OutKey.Axis[ChannelBase + i]))
                    {
                        OutKey.bHasAxis[ChannelBase + i] = true;
                    }
                    else if (bIsRotation && ValObj->TryGetNumberField(RotKeys[i], OutKey.Axis[ChannelBase + i]))
                    {
                        OutKey.bHasAxis[ChannelBase + i] = true;
                    }
                }
            }
        }
        else if (Value.IsValid() && Value->Type == EJson::Array)
        {
            const TArray<TSharedPtr<FJsonValue>>& Arr = Value->AsArray();
            for (int32 i = 0; i < 3 && i < Arr.Num(); ++i)
            {
                if (Arr[i].IsValid() && Arr[i]->Type == EJson::Number)
                {
                    OutKey.Axis[ChannelBase + i] = Arr[i]->AsNumber();
                    OutKey.bHasAxis[ChannelBase + i] = true;
                }
            }
        }
    }

    // Resolve a key's `value` field into channel slots. Nothing is written when the shape does not
    // match — the caller decides whether that is a fall-through (singular verb) or a rejection
    // (batch verb, which cannot leave a half-authored path behind).
    static void ParseTransformKeyValue(ETransformKeyShape Shape, int32 ChannelBase,
                                       const TSharedPtr<FJsonValue>& Value, FTransformKeyWrite& OutKey)
    {
        if (Shape == ETransformKeyShape::FullNine)
        {
            static const TCHAR* const XyzKeys[3] = { TEXT("x"), TEXT("y"), TEXT("z") };
            static const TCHAR* const RotKeys[3] = { TEXT("roll"), TEXT("pitch"), TEXT("yaw") };

            const TSharedPtr<FJsonObject> ValueObj =
                (Value.IsValid() && Value->Type == EJson::Object) ? Value->AsObject() : nullptr;
            if (!ValueObj.IsValid())
            {
                return;
            }
            const TSharedPtr<FJsonObject>* SubObj = nullptr;
            if (ValueObj->TryGetObjectField(TEXT("location"), SubObj) && SubObj)
            {
                ReadNamedAxes(*SubObj, XyzKeys, 0, OutKey);
            }
            if (ValueObj->TryGetObjectField(TEXT("rotation"), SubObj) && SubObj)
            {
                ReadNamedAxes(*SubObj, RotKeys, 3, OutKey);
            }
            if (ValueObj->TryGetObjectField(TEXT("scale"), SubObj) && SubObj)
            {
                ReadNamedAxes(*SubObj, XyzKeys, 6, OutKey);
            }
            return;
        }
        if (Shape == ETransformKeyShape::AxisTriple)
        {
            ReadAxisTriple(Value, /*bIsRotation=*/ChannelBase == 3, ChannelBase, OutKey);
        }
    }

    // Apply one resolved key to the section's channels. Two contracts live here and nowhere else:
    //   1. UpdateOrAddKey, never AddKey. AddKey ALWAYS inserts, so re-keying a frame that already
    //      carried a key appended a SECOND key at the same tick (a state Sequencer's UI cannot
    //      produce, where the evaluated value is whichever entry happens to sort first).
    //      UpdateOrAddKey searches by FFrameNumber, so a re-key overwrites and the count holds.
    //   2. Touched channels are COLLECTED, not solved here. TMovieSceneChannelData stores the
    //      FMovieSceneDoubleValue verbatim — it cannot compute tangents, because AutoSetTangents()
    //      lives on the channel, not on its data view — so an RCTM_Auto key would otherwise keep
    //      0/0 tangents forever and evaluate as a flat-in/flat-out Hermite (the subject stops dead
    //      at every key). AutoSetTangentsOn below closes that once per channel, after the whole
    //      write rather than per key on a partial curve.
    static void ApplyTransformKeyWrite(TArrayView<FMovieSceneDoubleChannel*> Channels,
                                       const FTransformKeyWrite& Key,
                                       TArray<FMovieSceneDoubleChannel*>& TouchedChannels)
    {
        for (int32 ChannelIndex = 0; ChannelIndex < TransformChannelCount; ++ChannelIndex)
        {
            if (!Key.bHasAxis[ChannelIndex] || ChannelIndex >= Channels.Num())
            {
                continue;
            }
            FMovieSceneDoubleChannel* Channel = Channels[ChannelIndex];
            if (!Channel)
            {
                continue;
            }
            FMovieSceneDoubleValue KeyValue(Key.Axis[ChannelIndex]);
            KeyValue.InterpMode = Key.InterpMode;
            KeyValue.TangentMode = Key.TangentMode;
            // Explicit user/break tangents are seated at construction; AutoSetTangents skips those
            // modes, so they survive the solve below untouched.
            SequencerSectionHelpers::ApplyKeyTangents(KeyValue, Key.Tangents);
            Channel->GetData().UpdateOrAddKey(Key.TickFrame, KeyValue);
            TouchedChannels.AddUnique(Channel);
        }
    }

    // Solve auto tangents on every channel the write touched. RCTM_Auto is only an INTENT until the
    // channel itself runs this (see contract 2 above).
    static void AutoSetTangentsOn(const TArray<FMovieSceneDoubleChannel*>& TouchedChannels)
    {
        for (FMovieSceneDoubleChannel* Channel : TouchedChannels)
        {
            Channel->AutoSetTangents();
        }
    }

    // bindingId (a GUID) or actorName (a possessable/spawnable name) -> binding GUID. Both keyframe
    // verbs accept the same pair, so the lookup lives once. bindingId wins outright when supplied:
    // a malformed GUID yields an invalid FGuid rather than silently falling back to a name search.
    static FGuid ResolveBindingGuid(UMovieScene* MovieScene, const FString& BindingIdStr,
                                    const FString& ActorName)
    {
        FGuid BindingGuid;
        if (!BindingIdStr.IsEmpty())
        {
            FGuid::Parse(BindingIdStr, BindingGuid);
            return BindingGuid;
        }
        if (ActorName.IsEmpty() || !MovieScene)
        {
            return BindingGuid;
        }
        for (const FMovieSceneBinding& Binding : const_cast<const UMovieScene*>(MovieScene)->GetBindings())
        {
            FString BindingName;
            if (FMovieScenePossessable* Possessable = MovieScene->FindPossessable(Binding.GetObjectGuid()))
            {
                BindingName = Possessable->GetName();
            }
            else if (FMovieSceneSpawnable* Spawnable = MovieScene->FindSpawnable(Binding.GetObjectGuid()))
            {
                BindingName = Spawnable->GetName();
            }

            if (BindingName.Equals(ActorName, ESearchCase::IgnoreCase))
            {
                return Binding.GetObjectGuid();
            }
        }
        return BindingGuid;
    }
}

// ============================================================================
// sequence.add_keyframe
// ============================================================================
REGISTER_RPC_HANDLER("sequence.add_keyframe", "Sequencer", "Add a keyframe to a track in a level sequence",
    RPC_PARAMS(
        RPC_PARAM_OPT("path", "path", "Sequence asset path"),
        RPC_PARAM_OPT("bindingId", "string", "GUID of the binding (from add_actor/get_bindings)"),
        RPC_PARAM_OPT("actorName", "string", "Actor label (alternative to bindingId)"),
        RPC_PARAM_OPT("property", "string", "Property name (e.g. Transform, Location)"),
        RPC_PARAM_REQ("frame", "integer", "Frame number for the keyframe"),
        RPC_PARAM_OPT("value", "object|number|boolean", "Value for the keyframe"),
        RPC_PARAM_OPT("interp", "string", "Key interpolation: constant (holds/stepped) | linear | cubic. Default cubic. Pass linear for constant-rate motion — a 2-key span keyed cubic eases in and out."),
        RPC_PARAM_OPT("tangentMode", "string", "Cubic tangent mode: auto | user | break | none. Default auto; ignored for constant/linear."),
        RPC_PARAM_OPT("arriveTangent", "number", "Explicit incoming slope for this key, in curve value per TICK (not per second, not per display frame): a slope of V units/second is V / tickResolution. Requires tangentMode 'user' or 'break' — 'auto' re-solves every key and would discard it. Omit to keep whatever the channel solves."),
        RPC_PARAM_OPT("leaveTangent", "number", "Explicit outgoing slope for this key, same units and same tangentMode requirement as arriveTangent. Together they are the only way to author a non-zero velocity at a loop seam: auto tangents force the first key's leave and the last key's arrive flat, so a looping move stops dead where it is cut.")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    TSharedPtr<FJsonObject> LocalPayload = Payload.IsValid() ? Payload : MakeShared<FJsonObject>();
    FString SeqPath = SequenceHelpers::ResolveSequencePath(LocalPayload);
    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "sequence_add_keyframe requires a sequence path");
        return true;
    }

    FString BindingIdStr;
    LocalPayload->TryGetStringField(TEXT("bindingId"), BindingIdStr);
    FString ActorName;
    LocalPayload->TryGetStringField(TEXT("actorName"), ActorName);
    FString PropertyName;
    LocalPayload->TryGetStringField(TEXT("property"), PropertyName);

    if (BindingIdStr.IsEmpty() && ActorName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            "Either bindingId or actorName must be provided. bindingId is the "
            "GUID from add_actor/get_bindings. actorName is the label of an "
            "actor already bound to the sequence. Example: {\"actorName\": "
            "\"MySphere\", \"property\": \"Location\", \"frame\": 0, "
            "\"value\": {\"x\":0,\"y\":0,\"z\":0}}");
        return true;
    }

    double Frame = 0.0;
    if (!LocalPayload->TryGetNumberField(TEXT("frame"), Frame))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            "frame number is required. Example: {\"frame\": 30} for keyframe at frame 30");
        return true;
    }

    // Key interpolation. Both FMovieSceneDoubleValue and FMovieSceneFloatValue default-construct
    // with InterpMode = RCIM_Cubic, so every transform key this verb ever wrote was cubic with auto
    // tangents and there was no parameter to say otherwise. On a two-key span that is an ease
    // in/ease out: the bound actor starts from a dead stop, overshoots the mean rate mid-span and
    // glides to a halt at the last key, on every loop. Validate up front, before any track/section
    // mutation, so a bad value is rejected cleanly. Defaults preserve the prior behaviour exactly.
    ERichCurveInterpMode KeyInterpMode = RCIM_Cubic;
    if (!SequencerSectionHelpers::ParseKeyInterpMode(Ctx.GetString(TEXT("interp")), KeyInterpMode))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, "interp must be one of: constant, linear, cubic");
        return true;
    }
    ERichCurveTangentMode KeyTangentMode = RCTM_Auto;
    if (!SequencerSectionHelpers::ParseKeyTangentMode(Ctx.GetString(TEXT("tangentMode")), KeyTangentMode))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, "tangentMode must be one of: auto, user, break, none");
        return true;
    }
    // Explicit tangent VALUES, validated in the same up-front block. Writing them under a mode that
    // recomputes them would erase them on the spot and again on every asset load, so the mismatch is
    // an error rather than a silent no-op.
    const SequencerSectionHelpers::FKeyTangents KeyTangents =
        SequencerSectionHelpers::ParseKeyTangents(LocalPayload);
    if (KeyTangents.IsSet() && !SequencerSectionHelpers::TangentModeKeepsExplicitTangents(KeyTangentMode))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, SequencerSectionHelpers::ExplicitTangentModeError());
        return true;
    }
    // Float property tracks keep a local factory; the transform channels go through the shared
    // SequenceKeyframeHelpers write path that sequencer.add_keyframes also uses.
    const auto MakeFloatKey = [KeyInterpMode, KeyTangentMode, &KeyTangents](float InValue)
    {
        FMovieSceneFloatValue KeyValue(InValue);
        KeyValue.InterpMode = KeyInterpMode;
        KeyValue.TangentMode = KeyTangentMode;
        SequencerSectionHelpers::ApplyKeyTangents(KeyValue, KeyTangents);
        return KeyValue;
    };

    UObject* SeqObj = ResolveAsset(SeqPath, /*bLoadObject=*/true).Object;
    if (!SeqObj)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "Sequence not found");
        return true;
    }

    if (ULevelSequence* LevelSeq = Cast<ULevelSequence>(SeqObj))
    {
        UMovieScene* MovieScene = LevelSeq->GetMovieScene();
        if (MovieScene)
        {
            const FGuid BindingGuid =
                SequenceKeyframeHelpers::ResolveBindingGuid(MovieScene, BindingIdStr, ActorName);

            if (!BindingGuid.IsValid())
            {
                FString Target = !BindingIdStr.IsEmpty() ? BindingIdStr : ActorName;
                Ctx.SendError(ErrorCodes::ERR_BINDING_NOT_FOUND,
                    FString::Printf(TEXT("Binding not found for '%s'. Ensure actor is bound to sequence."), *Target));
                return true;
            }

            FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuid);
            if (!Binding)
            {
                Ctx.SendError(ErrorCodes::ERR_BINDING_NOT_FOUND, "Binding object not found in sequence");
                return true;
            }

            // Transform channels: "Transform" writes the whole {location, rotation, scale}
            // envelope, Location/Rotation/Scale write one three-channel group of the SAME
            // UMovieScene3DTransformTrack. Both shapes resolve into one FTransformKeyWrite and go
            // through the shared SequenceKeyframeHelpers write path, so sequencer.add_keyframes
            // cannot drift from what this verb does per key. Channel layout: 0-2 Location,
            // 3-5 Rotation (Roll, Pitch, Yaw), 6-8 Scale.
            int32 ChannelBase = 0;
            const SequenceKeyframeHelpers::ETransformKeyShape KeyShape =
                SequenceKeyframeHelpers::ClassifyTransformProperty(PropertyName, ChannelBase);
            if (KeyShape != SequenceKeyframeHelpers::ETransformKeyShape::None)
            {
                UMovieSceneSection* TransformSection = nullptr;
                TArrayView<FMovieSceneDoubleChannel*> Channels =
                    SequenceHelpers::GetOrAddTransformChannels(MovieScene, BindingGuid, &TransformSection);
                if (Channels.Num() >= SequenceKeyframeHelpers::RequiredChannelCount(KeyShape, ChannelBase))
                {
                    SequenceKeyframeHelpers::FTransformKeyWrite KeyWrite;
                    KeyWrite.Frame = Frame;
                    KeyWrite.TickFrame = SequenceHelpers::DisplayFrameToTick(MovieScene, Frame);
                    KeyWrite.InterpMode = KeyInterpMode;
                    KeyWrite.TangentMode = KeyTangentMode;
                    KeyWrite.Tangents = KeyTangents;
                    SequenceKeyframeHelpers::ParseTransformKeyValue(
                        KeyShape, ChannelBase, LocalPayload->TryGetField(TEXT("value")), KeyWrite);

                    if (KeyWrite.TouchesAnyChannel())
                    {
                        TArray<FMovieSceneDoubleChannel*> TouchedChannels;
                        SequenceKeyframeHelpers::ApplyTransformKeyWrite(Channels, KeyWrite, TouchedChannels);
                        // Honour the requested tangent mode: RCTM_Auto is only an intent until the
                        // channel solves it (see ApplyTransformKeyWrite's second contract).
                        SequenceKeyframeHelpers::AutoSetTangentsOn(TouchedChannels);
                        // Grow the section to span the written key — FindOrAddSection(0) creates it
                        // collapsed to [0,0], so without this the key sits outside the section
                        // bounds (the authored animation plays no time).
                        if (TransformSection)
                        {
                            TransformSection->ExpandToFrame(KeyWrite.TickFrame);
                        }
                        MovieScene->Modify();
                        Ctx.SendSuccess(TSharedPtr<FJsonObject>(nullptr));
                        return true;
                    }
                }
            }
            else
            {
                // Generic property tracks
                const TSharedPtr<FJsonValue> Val = LocalPayload->TryGetField(TEXT("value"));
                if (Val.IsValid() && Val->Type == EJson::Number)
                {
                    UMovieSceneFloatTrack* Track =
                        MovieScene->FindTrack<UMovieSceneFloatTrack>(BindingGuid, FName(*PropertyName));
                    if (!Track)
                    {
                        Track = MovieScene->AddTrack<UMovieSceneFloatTrack>(BindingGuid);
                        if (Track)
                            Track->SetPropertyNameAndPath(FName(*PropertyName), PropertyName);
                    }
                    if (Track)
                    {
                        bool bSectionAdded = false;
                        UMovieSceneFloatSection* Section = Cast<UMovieSceneFloatSection>(
                            Track->FindOrAddSection(0, bSectionAdded));
                        if (Section)
                        {
                            FFrameRate TickResolution = MovieScene->GetTickResolution();
                            FFrameRate DisplayRate = MovieScene->GetDisplayRate();
                            FFrameNumber FrameNum = FFrameNumber(static_cast<int32>(Frame));
                            FFrameNumber TickFrame =
                                FFrameRate::TransformTime(FFrameTime(FrameNum), DisplayRate, TickResolution)
                                    .GetFrame();

                            FMovieSceneFloatChannel* Channel =
                                Section->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0);
                            if (Channel)
                            {
                                Channel->GetData().UpdateOrAddKey(
                                    TickFrame, MakeFloatKey((float)Val->AsNumber()));
                                // Same tangent contract as the transform branches: the data view
                                // stores the value struct verbatim, so RCTM_Auto stays a 0/0
                                // tangent until the channel itself solves it.
                                Channel->AutoSetTangents();
                                // Grow the section to span the written key (see "Transform" branch above).
                                Section->ExpandToFrame(TickFrame);
                                MovieScene->Modify();
                                Ctx.SendSuccess(TSharedPtr<FJsonObject>(nullptr));
                                return true;
                            }
                        }
                    }
                }
                else if (Val.IsValid() && Val->Type == EJson::Boolean)
                {
                    UMovieSceneBoolTrack* Track =
                        MovieScene->FindTrack<UMovieSceneBoolTrack>(BindingGuid, FName(*PropertyName));
                    if (!Track)
                    {
                        Track = MovieScene->AddTrack<UMovieSceneBoolTrack>(BindingGuid);
                        if (Track)
                            Track->SetPropertyNameAndPath(FName(*PropertyName), PropertyName);
                    }
                    if (Track)
                    {
                        bool bSectionAdded = false;
                        UMovieSceneBoolSection* Section = Cast<UMovieSceneBoolSection>(
                            Track->FindOrAddSection(0, bSectionAdded));
                        if (Section)
                        {
                            FFrameRate TickResolution = MovieScene->GetTickResolution();
                            FFrameRate DisplayRate = MovieScene->GetDisplayRate();
                            FFrameNumber FrameNum = FFrameNumber(static_cast<int32>(Frame));
                            FFrameNumber TickFrame =
                                FFrameRate::TransformTime(FFrameTime(FrameNum), DisplayRate, TickResolution)
                                    .GetFrame();

                            FMovieSceneBoolChannel* Channel =
                                Section->GetChannelProxy().GetChannel<FMovieSceneBoolChannel>(0);
                            if (Channel)
                            {
                                Channel->GetData().UpdateOrAddKey(TickFrame, Val->AsBool());
                                // Grow the section to span the written key (see "Transform" branch above).
                                Section->ExpandToFrame(TickFrame);
                                MovieScene->Modify();
                                Ctx.SendSuccess(TSharedPtr<FJsonObject>(nullptr));
                                return true;
                            }
                        }
                    }
                }
            }

            Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_PROPERTY, "Unsupported property or failed to create track");
            return true;
        }
    }
    Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE_TYPE, "Sequence object is not a LevelSequence");
    return true;
}

// ============================================================================
// sequencer.add_keyframes
// ============================================================================
// The batch form of sequence.add_keyframe's frame-numbered transform path, and it exists for a
// structural reason rather than for call-count comfort: N single-key RPCs are N undo steps and N
// crash windows over a sequence that lives in memory until an explicit asset.save, so an editor
// that dies part-way leaves a valid, loadable, silently HALF-authored ULevelSequence with no way
// to tell which prefix landed. Here the whole path is resolved before anything is written and
// written inside ONE FScopedTransaction: it lands as a unit or not at all, and the response echoes
// enough for the caller to skip the confirming readback.
//
// Deliberately transform-only and frame-numbered - that is the shape cinematics author in; the
// seconds-based float-track form stays sequencer.add_keyframe. Every key goes through the same
// SequenceKeyframeHelpers write path the singular verb uses, so the update-or-add and
// solve-the-tangents contracts cannot be reintroduced here in a weaker form.
REGISTER_RPC_HANDLER("sequencer.add_keyframes", "Sequencer",
    "Write many transform keys onto ONE binding in one all-or-nothing, single-undo step. The batch form of sequence.add_keyframe's frame-numbered Transform/Location/Rotation/Scale path: state path/bindingId/property/interp once, then pass keys[] carrying a frame and a value each. Every key is validated before any key is written, so a bad entry at index 17 leaves nothing behind; the response echoes each key's resolved frame and tickFrame plus a written count and the section's resulting range, so no readback is needed to confirm the path landed.",
    RPC_PARAMS(
        RPC_PARAM_OPT("path", "path", "Sequence asset path"),
        RPC_PARAM_OPT("bindingId", "string", "GUID of the binding (from add_actor/get_bindings)"),
        RPC_PARAM_OPT("actorName", "string", "Actor label (alternative to bindingId)"),
        RPC_PARAM_OPT("property", "string", "Which channels the keys address: Transform (a {location,rotation,scale} envelope over all nine channels) | Location | Rotation | Scale (one three-channel group). Default Transform."),
        RPC_PARAM_REQ("keys", "array", "The keys to write: each {frame, value} in the shape `property` selects, plus optional per-key interp / tangentMode / arriveTangent / leaveTangent that override the batch-level defaults field by field. Must be non-empty. A key whose frame is missing or non-finite, or whose value names no channel, rejects the WHOLE batch with INVALID_ARGUMENT naming its index."),
        RPC_PARAM_OPT("interp", "string", "Batch default key interpolation: constant (holds/stepped) | linear | cubic. Default cubic. Pass linear for constant-rate motion - a 2-key span keyed cubic eases in and out."),
        RPC_PARAM_OPT("tangentMode", "string", "Batch default cubic tangent mode: auto | user | break | none. Default auto; ignored for constant/linear."),
        RPC_PARAM_OPT("arriveTangent", "number", "Batch default incoming slope, in curve value per TICK (a slope of V units/second is V / tickResolution). Requires tangentMode 'user' or 'break'."),
        RPC_PARAM_OPT("leaveTangent", "number", "Batch default outgoing slope, same units and same tangentMode requirement as arriveTangent. Set these per key on the first and last key of a looping move: auto tangents force exactly those two flat, which is what makes an RPC-authored loop stop dead at the seam.")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    TSharedPtr<FJsonObject> LocalPayload = Payload.IsValid() ? Payload : MakeShared<FJsonObject>();

    const FString SeqPath = SequenceHelpers::ResolveSequencePath(LocalPayload);
    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "sequencer.add_keyframes requires a sequence path");
        return true;
    }

    FString BindingIdStr;
    LocalPayload->TryGetStringField(TEXT("bindingId"), BindingIdStr);
    FString ActorName;
    LocalPayload->TryGetStringField(TEXT("actorName"), ActorName);
    if (BindingIdStr.IsEmpty() && ActorName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            "Either bindingId or actorName must be provided. bindingId is the GUID from "
            "add_actor/get_bindings; actorName is the label of an actor already bound to the sequence.");
        return true;
    }

    // property defaults to Transform: a batch is almost always a camera or prop path, and the
    // envelope shape is the one the singular verb's callers already reach for.
    FString PropertyName = Ctx.GetString(TEXT("property"));
    if (PropertyName.IsEmpty())
    {
        PropertyName = TEXT("Transform");
    }
    int32 ChannelBase = 0;
    const SequenceKeyframeHelpers::ETransformKeyShape KeyShape =
        SequenceKeyframeHelpers::ClassifyTransformProperty(PropertyName, ChannelBase);
    if (KeyShape == SequenceKeyframeHelpers::ETransformKeyShape::None)
    {
        Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_PROPERTY,
            FString::Printf(TEXT("property '%s' is not a transform channel group. This verb writes "
                                 "Transform, Location, Rotation or Scale; use sequence.add_keyframe "
                                 "for a float or bool property track."), *PropertyName));
        return true;
    }

    // Batch-level curve shape. Validated before the keys so a typo in the shared default is
    // reported once rather than N times, and before any mutation either way.
    ERichCurveInterpMode BatchInterpMode = RCIM_Cubic;
    if (!SequencerSectionHelpers::ParseKeyInterpMode(Ctx.GetString(TEXT("interp")), BatchInterpMode))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, "interp must be one of: constant, linear, cubic");
        return true;
    }
    ERichCurveTangentMode BatchTangentMode = RCTM_Auto;
    if (!SequencerSectionHelpers::ParseKeyTangentMode(Ctx.GetString(TEXT("tangentMode")), BatchTangentMode))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, "tangentMode must be one of: auto, user, break, none");
        return true;
    }
    const SequencerSectionHelpers::FKeyTangents BatchTangents =
        SequencerSectionHelpers::ParseKeyTangents(LocalPayload);
    if (BatchTangents.IsSet() &&
        !SequencerSectionHelpers::TangentModeKeepsExplicitTangents(BatchTangentMode))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, SequencerSectionHelpers::ExplicitTangentModeError());
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* KeysArray = nullptr;
    if (!LocalPayload->TryGetArrayField(TEXT("keys"), KeysArray) || !KeysArray)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            "keys must be an array of {frame, value} objects. Example: {\"keys\": "
            "[{\"frame\": 0, \"value\": {\"location\": {\"x\": 0, \"y\": 0, \"z\": 0}}}]}");
        return true;
    }
    if (KeysArray->Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, "keys is empty - there is nothing to write");
        return true;
    }

    UObject* SeqObj = ResolveAsset(SeqPath, /*bLoadObject=*/true).Object;
    if (!SeqObj)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "Sequence not found");
        return true;
    }
    ULevelSequence* LevelSeq = Cast<ULevelSequence>(SeqObj);
    if (!LevelSeq)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE_TYPE, "Sequence object is not a LevelSequence");
        return true;
    }
    UMovieScene* MovieScene = LevelSeq->GetMovieScene();
    if (!MovieScene)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "Sequence has no MovieScene");
        return true;
    }

    const FGuid BindingGuid =
        SequenceKeyframeHelpers::ResolveBindingGuid(MovieScene, BindingIdStr, ActorName);
    if (!BindingGuid.IsValid() || !MovieScene->FindBinding(BindingGuid))
    {
        const FString Target = !BindingIdStr.IsEmpty() ? BindingIdStr : ActorName;
        Ctx.SendError(ErrorCodes::ERR_BINDING_NOT_FOUND,
            FString::Printf(TEXT("Binding not found for '%s'. Ensure actor is bound to sequence."), *Target));
        return true;
    }

    // ALL-OR-NOTHING. Every key is resolved HERE, before the track or section is touched:
    // GetOrAddTransformChannels below is itself a mutation, so a validation failure discovered
    // after it would already have changed the asset. Nothing in this loop writes.
    TArray<SequenceKeyframeHelpers::FTransformKeyWrite> Resolved;
    Resolved.Reserve(KeysArray->Num());
    for (int32 KeyIndex = 0; KeyIndex < KeysArray->Num(); ++KeyIndex)
    {
        const TSharedPtr<FJsonValue>& Entry = (*KeysArray)[KeyIndex];
        const TSharedPtr<FJsonObject> EntryObj =
            (Entry.IsValid() && Entry->Type == EJson::Object) ? Entry->AsObject() : nullptr;
        if (!EntryObj.IsValid())
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("keys[%d] is not an object"), KeyIndex));
            return true;
        }

        SequenceKeyframeHelpers::FTransformKeyWrite KeyWrite;
        if (!EntryObj->TryGetNumberField(TEXT("frame"), KeyWrite.Frame) ||
            !FMath::IsFinite(KeyWrite.Frame))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("keys[%d] needs a finite 'frame' (display-rate frame number)"),
                    KeyIndex));
            return true;
        }
        KeyWrite.TickFrame = SequenceHelpers::DisplayFrameToTick(MovieScene, KeyWrite.Frame);

        // Per-key overrides inherit the batch default field by field, so a key that names only
        // arriveTangent keeps the batch interp, tangentMode and leaveTangent. An absent field is
        // an inherit, never a reset.
        KeyWrite.InterpMode = BatchInterpMode;
        FString KeyInterpText;
        if (EntryObj->TryGetStringField(TEXT("interp"), KeyInterpText) &&
            !SequencerSectionHelpers::ParseKeyInterpMode(KeyInterpText, KeyWrite.InterpMode))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("keys[%d].interp must be one of: constant, linear, cubic"),
                    KeyIndex));
            return true;
        }
        KeyWrite.TangentMode = BatchTangentMode;
        FString KeyTangentText;
        if (EntryObj->TryGetStringField(TEXT("tangentMode"), KeyTangentText) &&
            !SequencerSectionHelpers::ParseKeyTangentMode(KeyTangentText, KeyWrite.TangentMode))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("keys[%d].tangentMode must be one of: auto, user, break, none"),
                    KeyIndex));
            return true;
        }
        KeyWrite.Tangents = BatchTangents;
        const SequencerSectionHelpers::FKeyTangents EntryTangents =
            SequencerSectionHelpers::ParseKeyTangents(EntryObj);
        if (EntryTangents.bHasArrive)
        {
            KeyWrite.Tangents.bHasArrive = true;
            KeyWrite.Tangents.Arrive = EntryTangents.Arrive;
        }
        if (EntryTangents.bHasLeave)
        {
            KeyWrite.Tangents.bHasLeave = true;
            KeyWrite.Tangents.Leave = EntryTangents.Leave;
        }
        if (KeyWrite.Tangents.IsSet() &&
            !SequencerSectionHelpers::TangentModeKeepsExplicitTangents(KeyWrite.TangentMode))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("keys[%d]: %s"), KeyIndex,
                    SequencerSectionHelpers::ExplicitTangentModeError()));
            return true;
        }

        SequenceKeyframeHelpers::ParseTransformKeyValue(
            KeyShape, ChannelBase, EntryObj->TryGetField(TEXT("value")), KeyWrite);
        if (!KeyWrite.TouchesAnyChannel())
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("keys[%d].value names no channel of '%s'. Expected %s."),
                    KeyIndex, *PropertyName,
                    KeyShape == SequenceKeyframeHelpers::ETransformKeyShape::FullNine
                        ? TEXT("{location:{x,y,z}} and/or {rotation:{roll,pitch,yaw}} and/or {scale:{x,y,z}}")
                        : TEXT("{x,y,z} (rotation also accepts {roll,pitch,yaw}) or [a,b,c]")));
            return true;
        }
        Resolved.Add(MoveTemp(KeyWrite));
    }

    // ONE transaction for the whole path, so undo is one step and every mutation from here on -
    // including the track and section GetOrAddTransformChannels creates - is inside it.
    const FScopedTransaction Transaction(
        NSLOCTEXT("PinWright", "SequencerAddKeyframes", "Add Keyframes to Sequence"));

    UMovieSceneSection* TransformSection = nullptr;
    TArrayView<FMovieSceneDoubleChannel*> Channels =
        SequenceHelpers::GetOrAddTransformChannels(MovieScene, BindingGuid, &TransformSection);
    const int32 RequiredChannels =
        SequenceKeyframeHelpers::RequiredChannelCount(KeyShape, ChannelBase);
    if (Channels.Num() < RequiredChannels)
    {
        Ctx.SendError(ErrorCodes::ERR_SECTION_FAILED,
            FString::Printf(TEXT("Could not resolve the binding's transform section channels for "
                                 "property '%s' (needed %d, found %d)"),
                *PropertyName, RequiredChannels, Channels.Num()));
        return true;
    }

    // Modify() BEFORE the writes, not after: a transaction snapshots an object's state at the
    // Modify call, so a post-mutation Modify records the ALREADY-changed state and the single undo
    // step this verb promises would restore nothing.
    MovieScene->Modify();
    if (TransformSection)
    {
        TransformSection->Modify();
    }

    TArray<FMovieSceneDoubleChannel*> TouchedChannels;
    TArray<TSharedPtr<FJsonValue>> KeyEcho;
    KeyEcho.Reserve(Resolved.Num());
    const FString BindingGuidStr = BindingGuid.ToString();
    for (const SequenceKeyframeHelpers::FTransformKeyWrite& KeyWrite : Resolved)
    {
        SequenceKeyframeHelpers::ApplyTransformKeyWrite(Channels, KeyWrite, TouchedChannels);
        if (TransformSection)
        {
            // Grow the section to span every written key - FindOrAddSection(0) creates it collapsed
            // to [0,0], so without this the keys sit outside the section bounds and the authored
            // animation plays no time.
            TransformSection->ExpandToFrame(KeyWrite.TickFrame);
        }

        TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetNumberField(TEXT("frame"), KeyWrite.Frame);
        Row->SetNumberField(TEXT("tickFrame"), KeyWrite.TickFrame.Value);
        Row->SetStringField(TEXT("property"), PropertyName);
        Row->SetStringField(TEXT("bindingId"), BindingGuidStr);
        Row->SetStringField(TEXT("interp"), MovieSceneJsonUtils::InterpModeToString(KeyWrite.InterpMode));
        Row->SetStringField(TEXT("tangentMode"),
            MovieSceneJsonUtils::TangentModeToString(KeyWrite.TangentMode));
        KeyEcho.Add(MakeShared<FJsonValueObject>(Row));
    }
    // Solve auto tangents once per channel AFTER the whole batch: AutoSetTangents reads each key's
    // neighbours, so per-key solving would repeatedly solve a partial curve and the interior keys
    // of a path would be shaped against neighbours that did not exist yet.
    SequenceKeyframeHelpers::AutoSetTangentsOn(TouchedChannels);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("path"), SeqPath);
    Resp->SetStringField(TEXT("bindingId"), BindingGuidStr);
    Resp->SetStringField(TEXT("property"), PropertyName);
    Resp->SetNumberField(TEXT("written"), Resolved.Num());
    Resp->SetNumberField(TEXT("channelsTouched"), TouchedChannels.Num());
    Resp->SetArrayField(TEXT("keys"), KeyEcho);
    if (TransformSection)
    {
        Resp->SetObjectField(TEXT("sectionRange"),
            MovieSceneJsonUtils::MakeFrameRangeObject(TransformSection->GetRange()));
    }
    Resp->SetObjectField(TEXT("displayRate"),
        MovieSceneJsonUtils::MakeFrameRateObject(MovieScene->GetDisplayRate()));
    Resp->SetObjectField(TEXT("tickResolution"),
        MovieSceneJsonUtils::MakeFrameRateObject(MovieScene->GetTickResolution()));
    Ctx.SendSuccess(Resp);
    return true;
}

// ============================================================================
// sequencer.get_binding_transform
// ============================================================================
// Evaluated (interpolated) readback — the counterpart to the authoring verbs
// above. get_bindings / list_tracks / list_sections surface the *authored*
// asset structure (which keys exist); this force-evaluates the binding's
// UMovieScene3DTransformTrack at an arbitrary display-rate frame and returns
// the composited location/rotation/scale the sequence would apply there. The
// display-frame -> tick conversion goes through the SAME SequenceHelpers::
// DisplayFrameToTick the keyframe writer uses, so "evaluate at frame N" reads
// back in the exact frame space the keys were authored in.
//
// The evaluation runs through the MovieScene interrogation pipeline
// (UE::MovieScene::FSystemInterrogator) — the same entity-system evaluation
// Sequencer performs at playback — so the returned transform is the COMPOSITED
// value: section ease-in/out weighting AND blending across overlapping /
// additive (non-absolute) transform sections are applied, not the raw
// per-channel curve value — what Sequencer would actually apply, including at
// eased section boundaries and overlapping stacks. The composited value is read
// back through the ComponentTransform property composites rather than through
// QueryLocalSpaceTransforms; see the comment at the query below for why.
REGISTER_RPC_HANDLER("sequencer.get_binding_transform", "Sequencer",
    "Evaluate a binding's transform track at a display-rate frame and return the composited "
    "location/rotation/scale. Unlike list_tracks/list_sections, which return the authored keys, this "
    "reports the transform Sequencer would apply at an arbitrary (possibly non-key) frame via the "
    "MovieScene interrogation pipeline (section easing and overlapping/non-absolute-section blending "
    "are applied).",
    RPC_PARAMS(
        RPC_PARAM_OPT("path", "path", "Sequence asset path"),
        RPC_PARAM_REQ("bindingId", "string", "GUID of the binding (from add_actor/get_bindings)"),
        RPC_PARAM_REQ("frame", "integer", "Display-rate frame number to evaluate at")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    TSharedPtr<FJsonObject> LocalPayload = Payload.IsValid() ? Payload : MakeShared<FJsonObject>();
    FString SeqPath = SequenceHelpers::ResolveSequencePath(LocalPayload);
    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "sequencer.get_binding_transform requires a sequence path");
        return true;
    }

    FString BindingIdStr;
    if (!Ctx.RequireString(TEXT("bindingId"), BindingIdStr))
        return true;

    double Frame = 0.0;
    if (!Ctx.RequireNumber(TEXT("frame"), Frame))
        return true;

    ULevelSequence* LevelSeq =
        Cast<ULevelSequence>(ResolveAsset(SeqPath, /*bLoadObject=*/true).Object);
    if (!LevelSeq)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE,
            FString::Printf(TEXT("Sequence not found or not a LevelSequence: %s"), *SeqPath));
        return true;
    }
    UMovieScene* MovieScene = LevelSeq->GetMovieScene();
    if (!MovieScene)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "LevelSequence has no MovieScene");
        return true;
    }

    FGuid BindingGuid;
    if (!FGuid::Parse(BindingIdStr, BindingGuid) || !BindingGuid.IsValid())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("bindingId is not a valid GUID: '%s'"), *BindingIdStr));
        return true;
    }
    if (!MovieScene->FindBinding(BindingGuid))
    {
        Ctx.SendError(ErrorCodes::ERR_BINDING_NOT_FOUND,
            FString::Printf(TEXT("No binding '%s' in sequence %s"), *BindingIdStr, *SeqPath));
        return true;
    }

    // Find (never add) the binding's transform track — a readback must not mutate the asset.
    UMovieScene3DTransformTrack* Track =
        MovieScene->FindTrack<UMovieScene3DTransformTrack>(BindingGuid, FName("Transform"));
    if (!Track)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_TRANSFORM_TRACK,
            FString::Printf(TEXT("Binding '%s' has no UMovieScene3DTransformTrack to evaluate. Author transform "
                                 "keys first (sequence.add_keyframe with property Location/Rotation/Scale)."),
                *BindingIdStr));
        return true;
    }

    const FFrameNumber TickFrame = SequenceHelpers::DisplayFrameToTick(MovieScene, Frame);

    // Guard: a transform track with no evaluable transform section can't be interrogated.
    bool bHasTransformSection = false;
    for (UMovieSceneSection* S : Track->GetAllSections())
    {
        if (Cast<UMovieScene3DTransformSection>(S))
        {
            bHasTransformSection = true;
            break;
        }
    }
    if (!bHasTransformSection)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_TRANSFORM_SECTION,
            FString::Printf(TEXT("Transform track for '%s' has no evaluable section"), *BindingIdStr));
        return true;
    }

    // Evaluate the whole track through the MovieScene interrogation pipeline so the
    // result is the COMPOSITED transform (section easing weights + cross-section
    // blending applied), not a raw per-channel curve read. Import the track onto the
    // default interrogation channel, interrogate the single frame, run the entity
    // systems (Update), then read back the blended local-space transform. That is the
    // same entity-system evaluation Sequencer's own editor tooling drives (see
    // BuiltInChannelEditors.cpp / MovieSceneToolHelpers.cpp), so it reports what
    // Sequencer would actually apply.
    using namespace UE::MovieScene;
    FSystemInterrogator Interrogator;
    Interrogator.ImportTrack(Track, FInterrogationChannel::Default());
    Interrogator.AddInterrogation(FFrameTime(TickFrame));
    Interrogator.Update();

    // Probe: did the default channel evaluate at all? An eval-disabled track never imports,
    // and QueryLocalSpaceTransforms is the cheap way to detect that (it returns nothing for an
    // inactive channel). Its VALUES are not usable for rotation — see below — so it is only
    // ever consulted for emptiness, and it also gates the property read: querying property
    // values for a channel that imported nothing trips an ensure() inside the interrogator.
    TArray<FIntermediate3DTransform> LocalTransforms;
    Interrogator.QueryLocalSpaceTransforms(FInterrogationChannel::Default(), LocalTransforms);

    // QueryLocalSpaceTransforms' fully-animated fast path assembles its result as
    //   FIntermediate3DTransform(LocX, LocY, LocZ, RotY, RotZ, RotX, ScaleX, ScaleY, ScaleZ)
    // (MovieSceneInterrogationLinker.cpp, identical in 5.3-5.8), so Rotation.Y lands in R_X,
    // Rotation.Z in R_Y and Rotation.X in R_Z. R_X/R_Y/R_Z are Roll/Pitch/Yaw — GetRotation()
    // returns FRotator(R_Y, R_Z, R_X) — so every rotation component surfaces one slot out:
    // the reported roll is the pitch, the reported pitch is the yaw, the reported yaw is the
    // roll. The partially-animated path in the same function assigns R_X/R_Y/R_Z straight from
    // DoubleResult[3..5] and is correct, so a fixed re-shuffle here cannot undo it: which of
    // the two paths runs depends on the section's transform mask.
    // Read the same interrogation back through the property composite registry instead, which
    // maps DoubleResult[3] -> R_X, [4] -> R_Y, [5] -> R_Z exactly as ComponentTransform declares
    // them (MovieSceneTracksComponentTypes.cpp). That is path-independent and correctly labelled.
    TArray<FIntermediate3DTransform> EvalTransforms;
    if (LocalTransforms.Num() > 0)
    {
        Interrogator.QueryPropertyValues(
            FMovieSceneTracksComponentTypes::Get()->ComponentTransform,
            FInterrogationChannel::Default(),
            EvalTransforms);
    }
    if (EvalTransforms.Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_EVAL_FAILED,
            FString::Printf(TEXT("Interrogation produced no transform for binding '%s' at frame %g"),
                *BindingIdStr, Frame));
        return true;
    }

    // FIntermediate3DTransform's accessors are the engine's canonical read-back of a
    // composited transform (location = translation; rotation via GetRotation()'s
    // Roll/Pitch/Yaw; scale = GetScale()) — the exact struct Sequencer applies to a
    // scene component.
    const FIntermediate3DTransform& Eval = EvalTransforms[0];
    const FVector EvalLocation = Eval.GetTranslation();
    const FRotator EvalRotation = Eval.GetRotation();
    const FVector EvalScale = Eval.GetScale();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("path"), SeqPath);
    Result->SetStringField(TEXT("bindingId"), BindingIdStr);
    Result->SetNumberField(TEXT("frame"), Frame);
    Result->SetNumberField(TEXT("tick"), static_cast<double>(TickFrame.Value));
    // The transform track stores the object's LOCAL (relative) transform; without a
    // resolved world actor this is the value Sequencer would apply, not a world-space compose.
    Result->SetStringField(TEXT("space"), TEXT("local"));

    TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
    LocObj->SetNumberField(TEXT("x"), EvalLocation.X);
    LocObj->SetNumberField(TEXT("y"), EvalLocation.Y);
    LocObj->SetNumberField(TEXT("z"), EvalLocation.Z);
    Result->SetObjectField(TEXT("location"), LocObj);

    TSharedPtr<FJsonObject> RotObj = MakeShared<FJsonObject>();
    RotObj->SetNumberField(TEXT("roll"), EvalRotation.Roll);
    RotObj->SetNumberField(TEXT("pitch"), EvalRotation.Pitch);
    RotObj->SetNumberField(TEXT("yaw"), EvalRotation.Yaw);
    Result->SetObjectField(TEXT("rotation"), RotObj);

    TSharedPtr<FJsonObject> ScaleObj = MakeShared<FJsonObject>();
    ScaleObj->SetNumberField(TEXT("x"), EvalScale.X);
    ScaleObj->SetNumberField(TEXT("y"), EvalScale.Y);
    ScaleObj->SetNumberField(TEXT("z"), EvalScale.Z);
    Result->SetObjectField(TEXT("scale"), ScaleObj);

    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// sequencer.add_section
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.add_section", "Sequencer", "Add a section to a track in a level sequence",
    RPC_PARAMS(
        RPC_PARAM_OPT("path", "path", "Sequence asset path"),
        RPC_PARAM_OPT("trackName", "string", "Name of the track to add section to"),
        RPC_PARAM_OPT("actorName", "string", "Filter bindings by actor name"),
        RPC_PARAM_DEF("startFrame", "integer", "Start frame of the section", "0"),
        RPC_PARAM_DEF("endFrame", "integer", "End frame of the section", "100")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    FString SeqPath = SequenceHelpers::ResolveSequencePath(Payload);
    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "sequence_add_section requires a sequence path");
        return true;
    }

    FString TrackName;
    Payload->TryGetStringField(TEXT("trackName"), TrackName);
    FString ActorName;
    Payload->TryGetStringField(TEXT("actorName"), ActorName);
    double StartFrame = 0.0, EndFrame = 100.0;
    Payload->TryGetNumberField(TEXT("startFrame"), StartFrame);
    Payload->TryGetNumberField(TEXT("endFrame"), EndFrame);

    ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *SeqPath);
    if (!Sequence || !Sequence->GetMovieScene())
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_FOUND, "Sequence not found");
        return true;
    }

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    UMovieSceneTrack* Track = nullptr;

    // Check master tracks first
    for (UMovieSceneTrack* MasterTrack : MovieScene->GetTracks())
    {
        if (SequenceHelpers::TrackMatchesIdentifier(MasterTrack, TrackName))
        {
            Track = MasterTrack;
            break;
        }
    }

    // If not found, check bindings
    if (!Track)
    {
        for (const FMovieSceneBinding& Binding :
             const_cast<const UMovieScene*>(MovieScene)->GetBindings())
        {
            FString BindingName;
            if (FMovieScenePossessable* Possessable =
                    MovieScene->FindPossessable(Binding.GetObjectGuid()))
                BindingName = Possessable->GetName();
            else if (FMovieSceneSpawnable* Spawnable =
                         MovieScene->FindSpawnable(Binding.GetObjectGuid()))
                BindingName = Spawnable->GetName();

            if (ActorName.IsEmpty() || BindingName.Contains(ActorName))
            {
                for (UMovieSceneTrack* BindingTrack : Binding.GetTracks())
                {
                    if (SequenceHelpers::TrackMatchesIdentifier(BindingTrack, TrackName))
                    {
                        Track = BindingTrack;
                        break;
                    }
                }
                if (Track)
                    break;
            }
        }
    }

    if (!Track)
    {
        Ctx.SendError(ErrorCodes::ERR_TRACK_NOT_FOUND, "Track not found");
        return true;
    }

    UMovieSceneSection* NewSection = Track->CreateNewSection();
    if (NewSection)
    {
        // startFrame/endFrame arrive as DISPLAY-RATE frame numbers (the param schema labels
        // them "frame"), but UMovieSceneSection::SetRange stores tick-resolution frame
        // numbers. Convert through the same SequenceHelpers::DisplayFrameToTick the keyframe
        // and set_properties paths use, so the documented endFrame:100 at 24fps/24000-tick
        // spans 100000 ticks (~4.17 s) instead of a bare, unconverted 100 ticks (0.0042 s).
        const FFrameNumber Start = SequenceHelpers::DisplayFrameToTick(MovieScene, StartFrame);
        const FFrameNumber End = SequenceHelpers::DisplayFrameToTick(MovieScene, EndFrame);
        NewSection->SetRange(TRange<FFrameNumber>(Start, End));
        Track->AddSection(*NewSection);
        MovieScene->Modify();

        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("trackName"), SequenceHelpers::GetTrackIdentifier(Track));
        Resp->SetNumberField(TEXT("startFrame"), StartFrame);
        Resp->SetNumberField(TEXT("endFrame"), EndFrame);
        Ctx.SendSuccess(Resp);
        return true;
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_SECTION_CREATION_FAILED, "Failed to create section");
        return true;
    }
}

// ============================================================================
// sequencer.set_tick_resolution
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.set_tick_resolution", "Sequencer", "Set the tick resolution of a level sequence",
    RPC_PARAMS(
        RPC_PARAM_OPT("path", "path", "Sequence asset path"),
        RPC_PARAM_OPT("resolution", "string", "Tick resolution (e.g. '24000', '60000', '24000/1', or rational format)")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    FString ResolutionStr;
    Payload->TryGetStringField(TEXT("resolution"), ResolutionStr);

    FString SeqPath = SequenceHelpers::ResolveSequencePath(Payload);
    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, "path required");
        return true;
    }

    ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *SeqPath);
    if (Sequence && Sequence->GetMovieScene())
    {
        FFrameRate TickResolution = Sequence->GetMovieScene()->GetTickResolution();

        // Parse the resolution to a number/rational and build FFrameRate from the PARSED value.
        // Never Contains-match digit substrings: an earlier "24000"/"60000" fast-path ran before
        // the rational/numeric branches, so any string merely embedding those runs was clamped
        // (e.g. "240000" -> 24000/1, "600000" -> 60000/1) — a silent 10x error, and because the
        // fast-path preceded the '/' branch even the rational form "240000/1" had no workaround.
        // Rational "num/den" first, then a plain positive integer.
        if (!ResolutionStr.IsEmpty())
        {
            bool bParsed = false;
            if (ResolutionStr.Contains(TEXT("/")))
            {
                FString NumStr, DenomStr;
                if (ResolutionStr.Split(TEXT("/"), &NumStr, &DenomStr))
                {
                    const int32 Num = FCString::Atoi(*NumStr);
                    const int32 Denom = FCString::Atoi(*DenomStr);
                    if (Num > 0 && Denom > 0)
                    {
                        TickResolution = FFrameRate(Num, Denom);
                        bParsed = true;
                    }
                }
            }
            else if (ResolutionStr.IsNumeric())
            {
                const int32 Num = FCString::Atoi(*ResolutionStr);
                if (Num > 0)
                {
                    TickResolution = FFrameRate(Num, 1);
                    bParsed = true;
                }
            }

            if (!bParsed)
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    "Invalid resolution; expected a positive integer (e.g. '24000') or a 'num/den' rational (e.g. '24000/1')");
                return true;
            }
        }

        Sequence->GetMovieScene()->SetTickResolutionDirectly(TickResolution);
        Sequence->GetMovieScene()->Modify();

        // Echo the applied tick resolution (mirrors sequencer.get_properties / set_display_rate)
        // so the caller can verify the write instead of trusting a bare {} success.
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetObjectField(TEXT("tickResolution"),
            MovieSceneJsonUtils::MakeFrameRateObject(TickResolution));
        Ctx.SendSuccess(Resp);
        return true;
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_FOUND, "Sequence not found");
        return true;
    }
}

// ============================================================================
// sequencer.set_view_range
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.set_view_range", "Sequencer", "Set the view range of a level sequence in the editor",
    RPC_PARAMS(
        RPC_PARAM_OPT("path", "path", "Sequence asset path"),
        RPC_PARAM_DEF("start", "number", "View range start (seconds)", "0"),
        RPC_PARAM_DEF("end", "number", "View range end (seconds)", "10")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    double Start = 0;
    double End = 10;
    Payload->TryGetNumberField(TEXT("start"), Start);
    Payload->TryGetNumberField(TEXT("end"), End);
    FString SeqPath = SequenceHelpers::ResolveSequencePath(Payload);

    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, "path required");
        return true;
    }

    ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *SeqPath);
    if (Sequence && Sequence->GetMovieScene())
    {
        Sequence->GetMovieScene()->SetViewRange(Start, End);
        Sequence->GetMovieScene()->Modify();
        Ctx.SendSuccess(TSharedPtr<FJsonObject>(nullptr));
        return true;
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_FOUND, "Sequence not found");
        return true;
    }
}

// ============================================================================
// sequencer.set_track_muted
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.set_track_muted", "Sequencer", "Mute or unmute a track in a level sequence",
    RPC_PARAMS(
        RPC_PARAM_OPT("path", "path", "Sequence asset path"),
        RPC_PARAM_OPT("trackName", "string", "Name of the track"),
        RPC_PARAM_DEF("muted", "boolean", "Whether to mute the track", "true")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    FString SeqPath = SequenceHelpers::ResolveSequencePath(Payload);
    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "sequence path required");
        return true;
    }

    FString TrackName;
    Payload->TryGetStringField(TEXT("trackName"), TrackName);
    bool bMuted = true;
    Payload->TryGetBoolField(TEXT("muted"), bMuted);

    ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *SeqPath);
    if (!Sequence || !Sequence->GetMovieScene())
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_FOUND, "Sequence not found");
        return true;
    }

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    UMovieSceneTrack* Track = nullptr;

    for (UMovieSceneTrack* MasterTrack : MovieScene->GetTracks())
    {
        if (SequenceHelpers::TrackMatchesIdentifier(MasterTrack, TrackName))
        {
            Track = MasterTrack;
            break;
        }
    }

    if (!Track)
    {
        for (const FMovieSceneBinding& Binding :
             const_cast<const UMovieScene*>(MovieScene)->GetBindings())
        {
            for (UMovieSceneTrack* BindingTrack : Binding.GetTracks())
            {
                if (SequenceHelpers::TrackMatchesIdentifier(BindingTrack, TrackName))
                {
                    Track = BindingTrack;
                    break;
                }
            }
            if (Track)
                break;
        }
    }

    if (!Track)
    {
        Ctx.SendError(ErrorCodes::ERR_TRACK_NOT_FOUND, "Track not found");
        return true;
    }

    Track->SetEvalDisabled(bMuted);
    MovieScene->Modify();

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("trackName"), SequenceHelpers::GetTrackIdentifier(Track));
    Resp->SetBoolField(TEXT("muted"), bMuted);
    Ctx.SendSuccess(Resp);
    return true;
}

// ============================================================================
// sequencer.set_track_solo
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.set_track_solo", "Sequencer", "Solo a track (simulated by muting all others)",
    RPC_PARAMS(
        RPC_PARAM_OPT("path", "path", "Sequence asset path"),
        RPC_PARAM_OPT("trackName", "string", "Name of the track to solo"),
        RPC_PARAM_DEF("solo", "boolean", "Whether to enable solo", "true")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    FString SeqPath = SequenceHelpers::ResolveSequencePath(Payload);
    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "sequence path required");
        return true;
    }

    FString TrackName;
    Payload->TryGetStringField(TEXT("trackName"), TrackName);
    bool bSolo = true;
    Payload->TryGetBoolField(TEXT("solo"), bSolo);

    ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *SeqPath);
    if (!Sequence || !Sequence->GetMovieScene())
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_FOUND, "Sequence not found");
        return true;
    }

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    UMovieSceneTrack* SoloTrack = nullptr;
    TArray<UMovieSceneTrack*> AllTracks;

    for (UMovieSceneTrack* Track : MovieScene->GetTracks())
    {
        if (Track)
        {
            AllTracks.Add(Track);
            if (SequenceHelpers::TrackMatchesIdentifier(Track, TrackName))
                SoloTrack = Track;
        }
    }

    for (const FMovieSceneBinding& Binding :
         const_cast<const UMovieScene*>(MovieScene)->GetBindings())
    {
        for (UMovieSceneTrack* Track : Binding.GetTracks())
        {
            if (Track)
            {
                AllTracks.Add(Track);
                if (SequenceHelpers::TrackMatchesIdentifier(Track, TrackName))
                    SoloTrack = Track;
            }
        }
    }

    if (!SoloTrack)
    {
        Ctx.SendError(ErrorCodes::ERR_TRACK_NOT_FOUND, "Track not found");
        return true;
    }

    for (UMovieSceneTrack* Track : AllTracks)
    {
        if (bSolo)
            Track->SetEvalDisabled(Track != SoloTrack);
        else
            Track->SetEvalDisabled(false);
    }
    MovieScene->Modify();

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("trackName"), SequenceHelpers::GetTrackIdentifier(SoloTrack));
    Resp->SetBoolField(TEXT("solo"), bSolo);
    Resp->SetStringField(TEXT("note"), TEXT("Solo is simulated by muting all other tracks. Unreal Engine does not have native track solo support."));
    Ctx.SendSuccess(Resp);
    return true;
}

// ============================================================================
// sequencer.set_track_locked
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.set_track_locked", "Sequencer", "Lock or unlock a track's sections in a level sequence",
    RPC_PARAMS(
        RPC_PARAM_OPT("path", "path", "Sequence asset path"),
        RPC_PARAM_OPT("trackName", "string", "Name of the track"),
        RPC_PARAM_DEF("locked", "boolean", "Whether to lock the track", "true")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    FString SeqPath = SequenceHelpers::ResolveSequencePath(Payload);
    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "sequence path required");
        return true;
    }

    FString TrackName;
    Payload->TryGetStringField(TEXT("trackName"), TrackName);
    bool bLocked = true;
    Payload->TryGetBoolField(TEXT("locked"), bLocked);

    ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *SeqPath);
    if (!Sequence || !Sequence->GetMovieScene())
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_FOUND, "Sequence not found");
        return true;
    }

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    UMovieSceneTrack* Track = nullptr;

    for (UMovieSceneTrack* MasterTrack : MovieScene->GetTracks())
    {
        if (SequenceHelpers::TrackMatchesIdentifier(MasterTrack, TrackName))
        {
            Track = MasterTrack;
            break;
        }
    }

    if (!Track)
    {
        for (const FMovieSceneBinding& Binding :
             const_cast<const UMovieScene*>(MovieScene)->GetBindings())
        {
            for (UMovieSceneTrack* BindingTrack : Binding.GetTracks())
            {
                if (SequenceHelpers::TrackMatchesIdentifier(BindingTrack, TrackName))
                {
                    Track = BindingTrack;
                    break;
                }
            }
            if (Track)
                break;
        }
    }

    if (!Track)
    {
        Ctx.SendError(ErrorCodes::ERR_TRACK_NOT_FOUND, "Track not found");
        return true;
    }

    for (UMovieSceneSection* Section : Track->GetAllSections())
    {
        if (Section)
            Section->SetIsLocked(bLocked);
    }
    MovieScene->Modify();

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("trackName"), SequenceHelpers::GetTrackIdentifier(Track));
    Resp->SetBoolField(TEXT("locked"), bLocked);
    Ctx.SendSuccess(Resp);
    return true;
}

// ============================================================================
// sequencer.remove_track
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.remove_track", "Sequencer", "Remove a track from a level sequence",
    RPC_PARAMS(
        RPC_PARAM_OPT("path", "path", "Sequence asset path"),
        RPC_PARAM_OPT("trackName", "string", "Name of the track to remove")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    FString SeqPath = SequenceHelpers::ResolveSequencePath(Payload);
    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "sequence path required");
        return true;
    }

    FString TrackName;
    Payload->TryGetStringField(TEXT("trackName"), TrackName);

    ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *SeqPath);
    if (!Sequence || !Sequence->GetMovieScene())
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_FOUND, "Sequence not found");
        return true;
    }

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    bool bRemoved = false;
    FString RemovedTrackName;

    for (UMovieSceneTrack* Track : MovieScene->GetTracks())
    {
        // Matching is SequenceHelpers::TrackMatchesIdentifier — GetName() OR GetDisplayName(),
        // both fields list_tracks reports — so a trackName piped back from list_tracks or
        // add_track resolves the same regardless of which field it came from.
        if (SequenceHelpers::TrackMatchesIdentifier(Track, TrackName))
        {
            RemovedTrackName = SequenceHelpers::GetTrackIdentifier(Track);
            MovieScene->RemoveTrack(*Track);
            bRemoved = true;
            break;
        }
    }

    if (!bRemoved)
    {
        for (const FMovieSceneBinding& Binding :
             const_cast<const UMovieScene*>(MovieScene)->GetBindings())
        {
            for (UMovieSceneTrack* Track : Binding.GetTracks())
            {
                if (SequenceHelpers::TrackMatchesIdentifier(Track, TrackName))
                {
                    RemovedTrackName = SequenceHelpers::GetTrackIdentifier(Track);
                    MovieScene->RemoveTrack(*Track);
                    bRemoved = true;
                    break;
                }
            }
            if (bRemoved)
                break;
        }
    }

    // The camera-cut track lives on the MovieScene's dedicated GetCameraCutTrack() slot,
    // not in GetTracks() or any binding, so neither loop above ever reaches it. Because
    // list_tracks DOES enumerate that slot (isCameraCutTrack=true, trackName=GetName()),
    // the trackName it hands back would otherwise fall through to TRACK_NOT_FOUND here.
    // Match the cut track by GetName()/GetDisplayName() (both fields list_tracks reports)
    // and clear it via the engine's dedicated RemoveCameraCutTrack() — a bare
    // MovieScene->RemoveTrack() only mutates the Tracks array and would NOT clear this slot.
    if (!bRemoved)
    {
        if (UMovieSceneTrack* CameraCutTrack = MovieScene->GetCameraCutTrack())
        {
            if (SequenceHelpers::TrackMatchesIdentifier(CameraCutTrack, TrackName))
            {
                RemovedTrackName = SequenceHelpers::GetTrackIdentifier(CameraCutTrack);
                MovieScene->RemoveCameraCutTrack();
                bRemoved = true;
            }
        }
    }

    if (bRemoved)
    {
        MovieScene->Modify();
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("trackName"), RemovedTrackName);
        Ctx.SendSuccess(Resp);
        return true;
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_TRACK_NOT_FOUND, "Track not found");
        return true;
    }
}

// ============================================================================
// sequencer.list_track_types
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.list_track_types", "Sequencer", "List all available sequencer track types",
    RPC_NO_PARAMS)
{
    TArray<TSharedPtr<FJsonValue>> Types;
    Types.Add(MakeShared<FJsonValueString>(TEXT("transform")));
    Types.Add(MakeShared<FJsonValueString>(TEXT("3dtransform")));
    Types.Add(MakeShared<FJsonValueString>(TEXT("audio")));
    Types.Add(MakeShared<FJsonValueString>(TEXT("event")));

    TSet<FString> AddedNames;
    AddedNames.Add(TEXT("transform"));
    AddedNames.Add(TEXT("3dtransform"));
    AddedNames.Add(TEXT("audio"));
    AddedNames.Add(TEXT("event"));

    for (TObjectIterator<UClass> It; It; ++It)
    {
        if (It->IsChildOf(UMovieSceneTrack::StaticClass()) &&
            !It->HasAnyClassFlags(CLASS_Abstract) &&
            !AddedNames.Contains(It->GetName()))
        {
            Types.Add(MakeShared<FJsonValueString>(It->GetName()));
            AddedNames.Add(It->GetName());
        }
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetArrayField(TEXT("types"), Types);
    Resp->SetNumberField(TEXT("count"), Types.Num());
    Ctx.SendSuccess(Resp);
    return true;
}

// ============================================================================
// sequencer.add_track
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.add_track", "Sequencer",
    "Add a track to a binding in a level sequence. The returned trackName is read off the "
    "created track (its object name) and is the identifier add_section / set_track_muted / "
    "set_track_solo / set_track_locked / remove_track / list_sections resolve by; displayName "
    "carries the editor-visible label.",
    RPC_PARAMS(
        RPC_PARAM_OPT("path", "path", "Sequence asset path"),
        RPC_PARAM_REQ("trackType", "string", "Track type (e.g. Transform, Animation, Audio, Event)"),
        RPC_PARAM_OPT("trackName", "string", "Optional display name for the track. Stored on the track's DisplayName, so it is rejected with TRACK_NAME_NOT_APPLIED (adding nothing) when trackType resolves outside UMovieSceneNameableTrack. It is NOT the returned trackName identifier - the response reports both."),
        RPC_PARAM_OPT("actorName", "string", "Actor binding to add the track to")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    TSharedPtr<FJsonObject> LocalPayload = Payload.IsValid() ? Payload : MakeShared<FJsonObject>();
    FString SeqPath = SequenceHelpers::ResolveSequencePath(LocalPayload);
    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE,
            TEXT("sequence_add_track requires a sequence path"));
        return true;
    }

    ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *SeqPath);
    if (!Sequence)
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_FOUND, TEXT("Level sequence not found"));
        return true;
    }

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        Ctx.SendError(ErrorCodes::ERR_MOVIESCENE_UNAVAILABLE, TEXT("MovieScene not available"));
        return true;
    }

    FString TrackType;
    LocalPayload->TryGetStringField(TEXT("trackType"), TrackType);
    if (TrackType.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("trackType required (e.g., Transform, Animation, Audio, Event)"));
        return true;
    }

    FString TrackName;
    LocalPayload->TryGetStringField(TEXT("trackName"), TrackName);
    FString ActorName;
    LocalPayload->TryGetStringField(TEXT("actorName"), ActorName);

    FGuid BindingGuid;
    if (!ActorName.IsEmpty())
    {
        const UMovieScene* ConstMovieScene = MovieScene;
        for (const FMovieSceneBinding& Binding : ConstMovieScene->GetBindings())
        {
            FString BindingName;
            if (FMovieScenePossessable* Possessable =
                    MovieScene->FindPossessable(Binding.GetObjectGuid()))
                BindingName = Possessable->GetName();
            else if (FMovieSceneSpawnable* Spawnable =
                         MovieScene->FindSpawnable(Binding.GetObjectGuid()))
                BindingName = Spawnable->GetName();

            if (BindingName.Contains(ActorName))
            {
                BindingGuid = Binding.GetObjectGuid();
                break;
            }
        }
        if (!BindingGuid.IsValid())
        {
            Ctx.SendError(ErrorCodes::ERR_BINDING_NOT_FOUND,
                FString::Printf(TEXT("Binding not found for actor: %s"), *ActorName));
            return true;
        }
    }

    UClass* TrackClass = ResolveUClass(TrackType);
    if (!TrackClass)
        TrackClass = ResolveUClass(FString::Printf(TEXT("UMovieScene%sTrack"), *TrackType));
    if (!TrackClass)
        TrackClass = ResolveUClass(FString::Printf(TEXT("MovieScene%sTrack"), *TrackType));
    if (!TrackClass)
        TrackClass = ResolveUClass(FString::Printf(TEXT("U%s"), *TrackType));

    if (!TrackClass)
    {
        // Was TRACK_CREATION_FAILED ("Failed to add track of type: X"), which described a
        // creation attempt that never happened. Nothing was added because the type string
        // never resolved to a class, and that is the only thing the caller can act on.
        Ctx.SendError(ErrorCodes::ERR_CLASS_NOT_FOUND,
            FString::Printf(
                TEXT("trackType '%s' does not resolve to a class (tried it verbatim, then ")
                TEXT("prefixed as UMovieScene<type>Track, MovieScene<type>Track and U<type>). ")
                TEXT("Call sequencer.list_track_types for the registered UMovieSceneTrack ")
                TEXT("classes."),
                *TrackType));
        return true;
    }
    if (!TrackClass->IsChildOf(UMovieSceneTrack::StaticClass()))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_CLASS_TYPE,
            FString::Printf(TEXT("Class '%s' is not a UMovieSceneTrack"), *TrackClass->GetName()));
        return true;
    }

    // Validate before mutating. trackName is carried by UMovieSceneNameableTrack::DisplayName
    // (MovieSceneNameableTrack.h:34 `MOVIESCENE_API virtual void SetDisplayName(const FText&)`);
    // a track class outside that subtree has nowhere to put it. This used to be accepted,
    // dropped on the floor, and echoed back as though it had been applied — refuse instead,
    // and refuse before AddTrack so a rejected request leaves the sequence untouched.
    if (!TrackName.IsEmpty() && !TrackClass->IsChildOf(UMovieSceneNameableTrack::StaticClass()))
    {
        Ctx.SendError(ErrorCodes::ERR_TRACK_NAME_NOT_APPLIED,
            FString::Printf(
                TEXT("Track class '%s' is not a UMovieSceneNameableTrack, so the requested ")
                TEXT("trackName '%s' cannot be stored on it. No track was added. Retry without ")
                TEXT("trackName and use the trackName field of the response (the track's ")
                TEXT("object name) as the identifier for the other sequencer verbs."),
                *TrackClass->GetName(), *TrackName));
        return true;
    }

    UMovieSceneTrack* NewTrack = BindingGuid.IsValid()
        ? MovieScene->AddTrack(TrackClass, BindingGuid)
        : MovieScene->AddTrack(TrackClass);

    if (!NewTrack)
    {
        Ctx.SendError(ErrorCodes::ERR_TRACK_CREATION_FAILED,
            FString::Printf(TEXT("UMovieScene::AddTrack returned null for type: %s"), *TrackType));
        return true;
    }

    if (!TrackName.IsEmpty())
    {
        // The IsChildOf gate above already proved this cast succeeds; the branch is kept so a
        // future gate change degrades into the readback failure below rather than a crash.
        if (UMovieSceneNameableTrack* NameableTrack = Cast<UMovieSceneNameableTrack>(NewTrack))
        {
            NameableTrack->SetDisplayName(FText::FromString(TrackName));
        }
    }

    // Everything reported from here on is READ OFF THE TRACK, never off the request.
    const FString ResolvedTrackName = SequenceHelpers::GetTrackIdentifier(NewTrack);
    const FString ResolvedDisplayName = NewTrack->GetDisplayName().ToString();

    // Readback. UMovieSceneNameableTrack::SetDisplayName is a plain store (it early-outs only
    // when the value is already equal), so this comparison is expected to hold — which is the
    // point: the response asserts the name is on the track because the track was asked, not
    // because the write was assumed to have worked.
    if (!TrackName.IsEmpty() && !ResolvedDisplayName.Equals(TrackName, ESearchCase::CaseSensitive))
    {
        // Roll the half-made track back out so the caller is not left with an unnameable,
        // unaddressable track it was told did not get created.
        MovieScene->RemoveTrack(*NewTrack);
        Ctx.SendError(ErrorCodes::ERR_TRACK_NAME_NOT_APPLIED,
            FString::Printf(
                TEXT("Added a '%s' track but the name did not survive readback (requested '%s', ")
                TEXT("track reports name '%s' / displayName '%s'). The track has been removed ")
                TEXT("again so nothing is left behind under a name that resolves to nothing."),
                *TrackClass->GetName(), *TrackName, *ResolvedTrackName, *ResolvedDisplayName));
        return true;
    }

    Sequence->MarkPackageDirty();
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("sequencePath"), SeqPath);
    Resp->SetStringField(TEXT("trackType"), TrackType);
    // The track's own object name — the string sequencer.add_section / set_track_muted /
    // set_track_solo / set_track_locked / remove_track / list_sections resolve by. It used to
    // be `TrackName.IsEmpty() ? TrackType : TrackName`, i.e. the request read back out, which
    // resolved only by the accident that a trackType like "Audio" is a substring of the engine
    // auto-name and never resolved at all once trackName was supplied.
    Resp->SetStringField(TEXT("trackName"), ResolvedTrackName);
    Resp->SetStringField(TEXT("displayName"), ResolvedDisplayName);
    if (!ActorName.IsEmpty())
    {
        Resp->SetStringField(TEXT("actorName"), ActorName);
        Resp->SetStringField(TEXT("bindingGuid"), BindingGuid.ToString());
    }
    Ctx.SendSuccess(Resp);
    return true;
}

// ============================================================================
// sequencer.add_sub_sequence
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.add_sub_sequence", "Sequencer",
    "Add a sub-sequence section to a parent level sequence's sub-track",
    RPC_PARAMS(
        RPC_PARAM_REQ("path", "path", "Parent level sequence asset path"),
        RPC_PARAM_REQ("innerSequencePath", "path", "Child level sequence asset path"),
        RPC_PARAM_REQ("startFrame", "integer", "Section start frame (tick resolution)"),
        RPC_PARAM_REQ("durationFrames", "integer", "Section duration in frames (tick resolution)"),
        RPC_PARAM_OPT("rowIndex", "integer", "Track row index; default 0"),
        RPC_PARAM_OPT("timeScale", "number", "Sub-section time scale; default 1.0")
    ))
{
    FString ParentPath;
    if (!Ctx.RequireString(TEXT("path"), ParentPath)) { return true; }
    FString InnerSequencePath;
    if (!Ctx.RequireString(TEXT("innerSequencePath"), InnerSequencePath)) { return true; }
    int32 StartFrame = 0;
    if (!Ctx.RequireInt(TEXT("startFrame"), StartFrame)) { return true; }
    int32 DurationFrames = 0;
    if (!Ctx.RequireInt(TEXT("durationFrames"), DurationFrames)) { return true; }

    ULevelSequence* ParentSequence = LoadObject<ULevelSequence>(nullptr, *ParentPath);
    if (!ParentSequence)
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_FOUND,
            FString::Printf(TEXT("Parent level sequence not found: %s"), *ParentPath));
        return true;
    }
    ULevelSequence* InnerSequence = LoadObject<ULevelSequence>(nullptr, *InnerSequencePath);
    if (!InnerSequence)
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_FOUND,
            FString::Printf(TEXT("Inner level sequence not found: %s"), *InnerSequencePath));
        return true;
    }

    UMovieScene* MovieScene = ParentSequence->GetMovieScene();
    if (!MovieScene)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, TEXT("Parent MovieScene not available"));
        return true;
    }

    UMovieSceneSubTrack* SubTrack = MovieScene->FindTrack<UMovieSceneSubTrack>();
    if (!SubTrack)
    {
        SubTrack = MovieScene->AddTrack<UMovieSceneSubTrack>();
    }
    if (!SubTrack)
    {
        Ctx.SendError(ErrorCodes::ERR_TRACK_CREATION_FAILED, TEXT("Failed to create UMovieSceneSubTrack"));
        return true;
    }

    const int32 RowIndex = Ctx.GetInt(TEXT("rowIndex"), INDEX_NONE);
    UMovieSceneSubSection* NewSection = SubTrack->AddSequenceOnRow(
        InnerSequence, FFrameNumber(StartFrame), DurationFrames, RowIndex);
    if (!NewSection)
    {
        Ctx.SendError(ErrorCodes::ERR_SUB_SECTION_CREATE_FAILED,
            TEXT("UMovieSceneSubTrack::AddSequenceOnRow returned null"));
        return true;
    }

    // Only stamp TimeScale when the caller actually supplied it; default-construction already
    // gives a 1.0 fixed play rate, and overwriting blindly would clobber any future variant default.
    if (Ctx.GetRawPayload().IsValid() && Ctx.GetRawPayload()->HasField(TEXT("timeScale")))
    {
        const double TimeScale = Ctx.GetNumber(TEXT("timeScale"), 1.0);
        NewSection->Parameters.TimeScale = TimeScale;
    }

    MovieScene->Modify();
    ParentSequence->MarkPackageDirty();

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("path"), ParentPath);
    Resp->SetStringField(TEXT("innerSequencePath"), InnerSequencePath);
    Resp->SetStringField(TEXT("sectionGuid"), NewSection->GetSignature().ToString());
    // GetTrackIdentifier, not GetTrackName(): UMovieSceneSubTrack does not override
    // GetTrackName(), so that accessor returned the literal "None" here — an identifier that
    // resolves in none of the trackName lookups this response invites the caller to use next.
    Resp->SetStringField(TEXT("trackName"), SequenceHelpers::GetTrackIdentifier(SubTrack));
    Resp->SetNumberField(TEXT("rowIndex"), NewSection->GetRowIndex());
    Resp->SetObjectField(TEXT("section"), MovieSceneJsonUtils::BuildSectionJson(NewSection));
    Ctx.SendSuccess(Resp);
    return true;
}

// ============================================================================
// sequencer.set_sub_section_range
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.set_sub_section_range", "Sequencer",
    "Mutate the range (and optionally time-scale) of an existing sub-section by GUID",
    RPC_PARAMS(
        RPC_PARAM_REQ("path", "path", "Parent level sequence asset path"),
        RPC_PARAM_REQ("sectionGuid", "string", "FGuid string from add_sub_sequence (Section::GetSignature())"),
        RPC_PARAM_REQ("startFrame", "integer", "Section start frame (tick resolution)"),
        RPC_PARAM_REQ("durationFrames", "integer", "Section duration in frames (tick resolution)"),
        RPC_PARAM_OPT("timeScale", "number", "Sub-section time scale (fixed play rate)")
    ))
{
    FString ParentPath;
    if (!Ctx.RequireString(TEXT("path"), ParentPath)) { return true; }
    FString SectionGuidStr;
    if (!Ctx.RequireString(TEXT("sectionGuid"), SectionGuidStr)) { return true; }
    int32 StartFrame = 0;
    if (!Ctx.RequireInt(TEXT("startFrame"), StartFrame)) { return true; }
    int32 DurationFrames = 0;
    if (!Ctx.RequireInt(TEXT("durationFrames"), DurationFrames)) { return true; }

    ULevelSequence* ParentSequence = LoadObject<ULevelSequence>(nullptr, *ParentPath);
    if (!ParentSequence)
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_FOUND,
            FString::Printf(TEXT("Parent level sequence not found: %s"), *ParentPath));
        return true;
    }
    UMovieScene* MovieScene = ParentSequence->GetMovieScene();
    if (!MovieScene)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, TEXT("Parent MovieScene not available"));
        return true;
    }

    FGuid TargetGuid;
    if (!FGuid::Parse(SectionGuidStr, TargetGuid))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("sectionGuid is not a valid FGuid"));
        return true;
    }

    UMovieSceneSubSection* FoundSection = SequenceHelpers::FindSubSectionBySignature(MovieScene, TargetGuid);
    if (!FoundSection)
    {
        Ctx.SendError(ErrorCodes::ERR_SUB_SECTION_NOT_FOUND,
            FString::Printf(TEXT("No sub-section with guid %s on parent %s"),
                *SectionGuidStr, *ParentPath));
        return true;
    }

    FoundSection->SetRange(TRange<FFrameNumber>(
        FFrameNumber(StartFrame), FFrameNumber(StartFrame + DurationFrames)));

    double TimeScale = 1.0;
    const bool bTimeScaleProvided = Ctx.GetRawPayload().IsValid()
        && Ctx.GetRawPayload()->HasField(TEXT("timeScale"));
    if (bTimeScaleProvided)
    {
        TimeScale = Ctx.GetNumber(TEXT("timeScale"), 1.0);
        FoundSection->Parameters.TimeScale = TimeScale;
    }

    FoundSection->Modify();
    ParentSequence->MarkPackageDirty();

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("path"), ParentPath);
    Resp->SetStringField(TEXT("sectionGuid"), SectionGuidStr);
    TSharedPtr<FJsonObject> RangeObj = MakeShared<FJsonObject>();
    RangeObj->SetNumberField(TEXT("start"), StartFrame);
    RangeObj->SetNumberField(TEXT("end"), StartFrame + DurationFrames);
    Resp->SetObjectField(TEXT("range"), RangeObj);
    if (bTimeScaleProvided)
    {
        Resp->SetNumberField(TEXT("timeScale"), TimeScale);
    }
    Ctx.SendSuccess(Resp);
    return true;
}

// ============================================================================
// sequencer.list_tracks
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.list_tracks", "Sequencer", "List all tracks in a level sequence",
    RPC_PARAMS(
        RPC_PARAM_OPT("path", "path", "Sequence asset path")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    TSharedPtr<FJsonObject> LocalPayload = Payload.IsValid() ? Payload : MakeShared<FJsonObject>();
    FString SeqPath = SequenceHelpers::ResolveSequencePath(LocalPayload);
    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "sequence_list_tracks requires a sequence path");
        return true;
    }

    ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *SeqPath);
    if (!Sequence)
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_FOUND, "Level sequence not found");
        return true;
    }

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        Ctx.SendError(ErrorCodes::ERR_MOVIESCENE_UNAVAILABLE, "MovieScene not available");
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> TracksArray;

    // One definition of a list_tracks track entry, shared by the master-track and
    // binding-track loops. Read-back of mute/solo state (set_track_muted/set_track_solo
    // write SetEvalDisabled) and lock state (set_track_locked writes SetIsLocked per
    // section; a track counts as locked only when every section is locked, empty -> false).
    auto EmitTrack = [](const UMovieSceneTrack* Track, bool bMasterTrack)
    {
        TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
        TrackObj->SetStringField(TEXT("trackName"), SequenceHelpers::GetTrackIdentifier(Track));
        TrackObj->SetStringField(TEXT("trackType"), Track->GetClass()->GetName());
        TrackObj->SetStringField(TEXT("displayName"), Track->GetDisplayName().ToString());
        TrackObj->SetBoolField(TEXT("isMasterTrack"), bMasterTrack);
        TrackObj->SetNumberField(TEXT("sectionCount"), Track->GetAllSections().Num());
        TrackObj->SetBoolField(TEXT("isEvalDisabled"), Track->IsEvalDisabled());
        TrackObj->SetBoolField(TEXT("allSectionsLocked"), SequenceHelpers::AreAllSectionsLocked(Track));
        return TrackObj;
    };

    for (UMovieSceneTrack* Track : MovieScene->GetTracks())
    {
        if (!Track)
            continue;
        TracksArray.Add(MakeShared<FJsonValueObject>(EmitTrack(Track, /*bMasterTrack=*/true)));
    }

    // The camera-cut track lives on a dedicated MovieScene slot (GetCameraCutTrack()),
    // not in GetTracks(), so the master-track loop above never sees it. Enumerate it
    // here too with an isCameraCutTrack discriminator so a generic "list the tracks"
    // intent gets a complete picture instead of silently missing an authored cut track.
    // (get_camera_cut_track remains the full dump-shaped reader; this is the enumerator entry.)
    if (UMovieSceneTrack* CameraCutTrack = MovieScene->GetCameraCutTrack())
    {
        TSharedPtr<FJsonObject> TrackObj = EmitTrack(CameraCutTrack, /*bMasterTrack=*/true);
        TrackObj->SetBoolField(TEXT("isCameraCutTrack"), true);
        TracksArray.Add(MakeShared<FJsonValueObject>(TrackObj));
    }

    for (const FMovieSceneBinding& Binding :
         const_cast<const UMovieScene*>(MovieScene)->GetBindings())
    {
        FString BindingName;
        if (FMovieScenePossessable* Possessable =
                MovieScene->FindPossessable(Binding.GetObjectGuid()))
            BindingName = Possessable->GetName();
        else if (FMovieSceneSpawnable* Spawnable =
                     MovieScene->FindSpawnable(Binding.GetObjectGuid()))
            BindingName = Spawnable->GetName();

        for (UMovieSceneTrack* Track : Binding.GetTracks())
        {
            if (!Track)
                continue;
            TSharedPtr<FJsonObject> TrackObj = EmitTrack(Track, /*bMasterTrack=*/false);
            TrackObj->SetStringField(TEXT("bindingName"), BindingName);
            TrackObj->SetStringField(TEXT("bindingGuid"), Binding.GetObjectGuid().ToString());
            TracksArray.Add(MakeShared<FJsonValueObject>(TrackObj));
        }
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetArrayField(TEXT("tracks"), TracksArray);
    Resp->SetNumberField(TEXT("trackCount"), TracksArray.Num());
    Resp->SetStringField(TEXT("sequencePath"), SeqPath);
    Ctx.SendSuccess(Resp);
    return true;
}

// ============================================================================
// sequencer.get_camera_cut_track
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.get_camera_cut_track", "Sequencer", "Get the camera-cut track in a level sequence",
    RPC_PARAMS(
        RPC_PARAM_REQ("path", "path", "Sequence asset path")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    TSharedPtr<FJsonObject> LocalPayload = Payload.IsValid() ? Payload : MakeShared<FJsonObject>();
    FString SeqPath = SequenceHelpers::ResolveSequencePath(LocalPayload);
    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "sequencer_get_camera_cut_track requires a sequence path");
        return true;
    }

    ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *SeqPath);
    if (!Sequence)
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_FOUND, "Level sequence not found");
        return true;
    }

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        Ctx.SendError(ErrorCodes::ERR_MOVIESCENE_UNAVAILABLE, "MovieScene not available");
        return true;
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("sequencePath"), SeqPath);
    if (UMovieSceneTrack* CameraCutTrack = MovieScene->GetCameraCutTrack())
    {
        Resp->SetObjectField(TEXT("cameraCutTrack"), MovieSceneJsonUtils::BuildTrackJson(CameraCutTrack));
    }
    else
    {
        Resp->SetField(TEXT("cameraCutTrack"), MakeShared<FJsonValueNull>());
    }
    Ctx.SendSuccess(Resp);
    return true;
}

// ============================================================================
// sequencer.list_sections
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.list_sections", "Sequencer", "List all sections in a level sequence",
    RPC_PARAMS(
        RPC_PARAM_REQ("path", "path", "Sequence asset path"),
        RPC_PARAM_OPT("bindingGuid", "string", "Optional binding GUID filter"),
        RPC_PARAM_OPT("trackName", "string", "Optional emitted track name filter"),
        RPC_PARAM_DEF("includeKeys", "bool",
            "When true, each channel also carries a keys[] array of {frame, value?} (per-key frame "
            "numbers, plus the scalar value for numeric channels) so authored keyframes are verifiable; "
            "off by default to keep the payload compact", "false")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    TSharedPtr<FJsonObject> LocalPayload = Payload.IsValid() ? Payload : MakeShared<FJsonObject>();
    FString SeqPath = SequenceHelpers::ResolveSequencePath(LocalPayload);
    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "sequence_list_sections requires a sequence path");
        return true;
    }

    ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *SeqPath);
    if (!Sequence)
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_FOUND, "Level sequence not found");
        return true;
    }

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        Ctx.SendError(ErrorCodes::ERR_MOVIESCENE_UNAVAILABLE, "MovieScene not available");
        return true;
    }

    FString BindingGuidFilter;
    LocalPayload->TryGetStringField(TEXT("bindingGuid"), BindingGuidFilter);
    FString TrackNameFilter;
    LocalPayload->TryGetStringField(TEXT("trackName"), TrackNameFilter);
    bool bIncludeKeys = false;
    LocalPayload->TryGetBoolField(TEXT("includeKeys"), bIncludeKeys);

    TArray<TSharedPtr<FJsonValue>> SectionsArray;
    auto AppendTrackSections = [&SectionsArray, &BindingGuidFilter, &TrackNameFilter, bIncludeKeys](
        const UMovieSceneTrack* Track,
        const FString& BindingGuid)
    {
        if (!Track)
        {
            return;
        }

        if (!BindingGuidFilter.IsEmpty() && !BindingGuid.Equals(BindingGuidFilter, ESearchCase::IgnoreCase))
        {
            return;
        }

        // Filter with the same predicate every other trackName lookup uses. This used to be an
        // exact compare against GetTrackName(), which is "None" for every track type that does
        // not override it — so a trackName taken from list_tracks or add_track matched nothing
        // and the verb reported an empty sections[] for a track that has sections.
        if (!TrackNameFilter.IsEmpty()
            && !SequenceHelpers::TrackMatchesIdentifier(Track, TrackNameFilter))
        {
            return;
        }

        for (const UMovieSceneSection* Section : Track->GetAllSections())
        {
            if (!Section)
            {
                continue;
            }

            SectionsArray.Add(MakeShared<FJsonValueObject>(
                SequenceHelpers::BuildListedSectionJson(Track, Section, BindingGuid, bIncludeKeys).ToSharedRef()));
        }
    };

    for (UMovieSceneTrack* Track : MovieScene->GetTracks())
    {
        AppendTrackSections(Track, FString());
    }

    for (const FMovieSceneBinding& Binding : const_cast<const UMovieScene*>(MovieScene)->GetBindings())
    {
        const FString BindingGuid = Binding.GetObjectGuid().ToString();
        for (UMovieSceneTrack* Track : Binding.GetTracks())
        {
            AppendTrackSections(Track, BindingGuid);
        }
    }

    AppendTrackSections(MovieScene->GetCameraCutTrack(), FString());

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("sequencePath"), SeqPath);
    Resp->SetArrayField(TEXT("sections"), SectionsArray);
    Resp->SetNumberField(TEXT("sectionCount"), SectionsArray.Num());
    Ctx.SendSuccess(Resp);
    return true;
}

// ============================================================================
// sequencer.set_work_range
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.set_work_range", "Sequencer", "Set the work range of a level sequence",
    RPC_PARAMS(
        RPC_PARAM_OPT("path", "path", "Sequence asset path"),
        RPC_PARAM_DEF("start", "number", "Work range start (seconds)", "0.0"),
        RPC_PARAM_DEF("end", "number", "Work range end (seconds)", "0.0")
    ))
{
    auto Payload = Ctx.GetRawPayload();
    TSharedPtr<FJsonObject> LocalPayload = Payload.IsValid() ? Payload : MakeShared<FJsonObject>();
    FString SeqPath = SequenceHelpers::ResolveSequencePath(LocalPayload);
    if (SeqPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_SEQUENCE, "sequence_set_work_range requires a sequence path");
        return true;
    }

    ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *SeqPath);
    if (!Sequence)
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_FOUND, "Level sequence not found");
        return true;
    }

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        Ctx.SendError(ErrorCodes::ERR_MOVIESCENE_UNAVAILABLE, "MovieScene not available");
        return true;
    }

    double Start = 0.0, End = 0.0;
    LocalPayload->TryGetNumberField(TEXT("start"), Start);
    LocalPayload->TryGetNumberField(TEXT("end"), End);

    FFrameRate TickResolution = MovieScene->GetTickResolution();
    FFrameNumber StartFrame((int32)FMath::RoundToInt(Start * TickResolution.AsDecimal()));
    FFrameNumber EndFrame((int32)FMath::RoundToInt(End * TickResolution.AsDecimal()));

    MovieScene->SetWorkingRange(Start, End);
    MovieScene->Modify();

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetNumberField(TEXT("startFrame"), StartFrame.Value);
    Resp->SetNumberField(TEXT("endFrame"), EndFrame.Value);
    Resp->SetStringField(TEXT("sequencePath"), SeqPath);
    Ctx.SendSuccess(Resp);
    return true;
}
