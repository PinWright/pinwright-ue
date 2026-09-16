// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Sequencer/SequencerBindingUtils.h"
#include "Handlers/Sequencer/SequencerKeyInterp.h"
#include "PinWrightSubsystem.h"
#include "PinWrightHelpers.h"
// GEditor->GetEditorWorldContext().World() — the editor world the possessable resolver scans;
// used to bind the actorPath rig possessable in add_camera_rig_rail/crane.
#include "Editor.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneSequence.h"
#include "MovieSceneBindingOwnerInterface.h"
#include "MovieSceneTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Curves/RichCurve.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Tracks/MovieSceneSkeletalAnimationTrack.h"
#include "Sections/MovieSceneSkeletalAnimationSection.h"
#include "Animation/AnimSequence.h"
#include "Camera/CameraActor.h"
#include "CameraRig_Rail.h"
#include "CameraRig_Crane.h"
#include "Tracks/MovieSceneLevelVisibilityTrack.h"
#include "Sections/MovieSceneLevelVisibilitySection.h"
#include "Tracks/MovieSceneAudioTrack.h"
#include "Sections/MovieSceneAudioSection.h"
#include "Sound/SoundBase.h"
#include "AudioDefines.h"
#include "Utils/MovieSceneJsonUtils.h"
#include "UObject/SoftObjectPath.h"

namespace SequencerSectionHelpers
{
    // Convert a time in SECONDS to a frame number in the MovieScene's TickResolution.
    // Both section ranges (UMovieSceneSection::SetRange) and channel key times
    // (FMovieSceneFloatChannel::AddCubicKey) are indexed in tick-resolution frames, NOT
    // DisplayRate frames — DisplayRate.AsFrameTime(seconds) here would undersize the frame
    // by the TickResolution/DisplayRate ratio (1000x at the 24000/24fps defaults).
    // Shared by every seconds-input verb — the section-authoring verbs (add_camera_track /
    // add_animation_track / add_audio_track) and the channel-key verb (add_keyframe) — so
    // the conversion lives in exactly one place and no future verb can reintroduce the
    // frames-as-ticks bug.
    static FFrameNumber SecondsToTickFrame(const UMovieScene* MovieScene, double Seconds)
    {
        return MovieScene->GetTickResolution().AsFrameNumber(Seconds);
    }

    // ParseKeyInterpMode / ParseKeyTangentMode moved to Handlers/Sequencer/SequencerKeyInterp.h
    // so sequence.add_keyframe's transform path shares this exact vocabulary. Same namespace,
    // so every call site below is unchanged.
}

// ---- sequencer.add_keyframe ----
REGISTER_RPC_HANDLER("sequencer.add_keyframe", "sequencer", "Insert a keyframe on an existing float track in a ULevelSequence at the given seconds-based time. Distinct from the legacy 'sequence.add_keyframe' (frame-numbered, broader track types) — see the wiki for which to use.",
    RPC_PARAMS(
        RPC_PARAM_REQ("sequencePath", "path", "Path to the Level Sequence asset"),
        RPC_PARAM_REQ("bindingGuid", "string", "Existing object binding GUID"),
        RPC_PARAM_REQ("propertyName", "string", "Property name for the float track"),
        RPC_PARAM_REQ("time", "number", "Time in seconds for the keyframe"),
        RPC_PARAM_REQ("value", "number", "Numeric value for the keyframe"),
        RPC_PARAM_OPT("interp", "string", "Key interpolation: constant (holds/stepped) | linear | cubic. Default cubic."),
        RPC_PARAM_OPT("tangentMode", "string", "Cubic tangent mode: auto | user | break | none. Default auto; ignored for constant/linear."),
        RPC_PARAM_OPT("arriveTangent", "number", "Explicit incoming slope for this key, in curve value per TICK (not per second, not per display frame): a slope of V units/second is V / tickResolution. Requires tangentMode 'user' or 'break' — 'auto' re-solves every key and would discard it. Omit to keep whatever the channel solves."),
        RPC_PARAM_OPT("leaveTangent", "number", "Explicit outgoing slope for this key, same units and same tangentMode requirement as arriveTangent. Together they are the only way to author a non-zero velocity at a loop seam: auto tangents force the first key's leave and the last key's arrive flat, so a looping move stops dead where it is cut.")
    ))
{
    FString SequencePath = Ctx.GetString(TEXT("sequencePath"));
    FString BindingGuidStr = Ctx.GetString(TEXT("bindingGuid"));
    FString PropertyName = Ctx.GetString(TEXT("propertyName"));

    if (SequencePath.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("sequencePath required")); return true; }
    if (BindingGuidStr.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("bindingGuid required (existing object binding GUID)")); return true; }
    if (PropertyName.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("propertyName required")); return true; }

    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();
    double TimeSeconds = 0.0;
    if (!Payload->TryGetNumberField(TEXT("time"), TimeSeconds)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("time (seconds) required")); return true; }
    double Value = 0.0;
    if (!Payload->TryGetNumberField(TEXT("value"), Value)) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("value (number) required")); return true; }

    // Optional interpolation/tangent so callers can author constant (holds/stepped), linear, or eased
    // cubic keys — not cubic-only. Validate up front (before any track/section mutation) so a bad value
    // is rejected cleanly. Both default to the prior behavior (cubic / auto).
    ERichCurveInterpMode InterpMode = RCIM_Cubic;
    if (!SequencerSectionHelpers::ParseKeyInterpMode(Ctx.GetString(TEXT("interp")), InterpMode))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("interp must be one of: constant, linear, cubic"));
        return true;
    }
    ERichCurveTangentMode TangentMode = RCTM_Auto;
    if (!SequencerSectionHelpers::ParseKeyTangentMode(Ctx.GetString(TEXT("tangentMode")), TangentMode))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("tangentMode must be one of: auto, user, break, none"));
        return true;
    }
    // Explicit tangent values, validated in the same up-front block: writing them under a mode that
    // recomputes them would erase them on the spot and again on every load, so the mismatch is an
    // error rather than a silent no-op.
    const SequencerSectionHelpers::FKeyTangents Tangents =
        SequencerSectionHelpers::ParseKeyTangents(Payload);
    if (Tangents.IsSet() && !SequencerSectionHelpers::TangentModeKeepsExplicitTangents(TangentMode))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), SequencerSectionHelpers::ExplicitTangentModeError());
        return true;
    }

    ULevelSequence* LevelSequence = LoadObject<ULevelSequence>(nullptr, *SequencePath);
    if (!LevelSequence)
    {
        Ctx.SendError(TEXT("LOAD_FAILED"), TEXT("Failed to load LevelSequence"));
        return true;
    }
    UMovieScene* MovieScene = LevelSequence->GetMovieScene();
    if (!MovieScene)
    {
        Ctx.SendError(TEXT("INVALID_SEQUENCE"), TEXT("Sequence has no MovieScene"));
        return true;
    }

    FGuid BindingGuid;
    if (!FGuid::Parse(BindingGuidStr, BindingGuid))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Invalid bindingGuid"));
        return true;
    }

    FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuid);
    if (!Binding)
    {
        Ctx.SendError(TEXT("BINDING_NOT_FOUND"), TEXT("Binding not found in sequence"));
        return true;
    }

    UMovieSceneTrack* Track = nullptr;
    for (UMovieSceneTrack* T : Binding->GetTracks())
    {
        if (UMovieSceneFloatTrack* FT = Cast<UMovieSceneFloatTrack>(T))
        {
            if (FT->GetPropertyName().ToString().Equals(PropertyName, ESearchCase::IgnoreCase))
            {
                Track = FT;
                break;
            }
        }
    }
    if (!Track)
    {
        UMovieSceneFloatTrack* NewTrack = MovieScene->AddTrack<UMovieSceneFloatTrack>(BindingGuid);
        if (!NewTrack)
        {
            Ctx.SendError(TEXT("CREATE_TRACK_FAILED"), TEXT("Failed to create float track"));
            return true;
        }
        NewTrack->SetPropertyNameAndPath(FName(*PropertyName), PropertyName);
        Track = NewTrack;
    }

    UMovieSceneFloatTrack* FloatTrack = CastChecked<UMovieSceneFloatTrack>(Track);
    UMovieSceneSection* Section = nullptr;
    const TArray<UMovieSceneSection*>& Sections = FloatTrack->GetAllSections();
    if (Sections.Num() > 0)
    {
        Section = Sections[0];
    }
    else
    {
        Section = FloatTrack->CreateNewSection();
        FloatTrack->AddSection(*Section);
    }

    if (!Section)
    {
        Ctx.SendError(TEXT("SECTION_FAILED"), TEXT("Failed to create/find section"));
        return true;
    }
    UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section);
    if (!FloatSection)
    {
        Ctx.SendError(TEXT("SECTION_TYPE_MISMATCH"), TEXT("Section is not a float section"));
        return true;
    }

    // Seconds -> TICK-resolution frames: FMovieSceneFloatChannel::AddCubicKey key times are
    // indexed in the MovieScene's TickResolution, NOT its DisplayRate. Feeding a DisplayRate
    // frame here undersizes the key time by the TickResolution/DisplayRate ratio (1000x at the
    // 24000/24fps defaults), landing the key ~1000x too early. Reuse the shared helper the
    // sibling section-authoring verbs use so this conversion lives in exactly one place.
    const FFrameNumber FrameNumber = SequencerSectionHelpers::SecondsToTickFrame(MovieScene, TimeSeconds);
    FMovieSceneFloatChannel& Channel = FloatSection->GetChannel();
    const float FloatValue = static_cast<float>(Value);
    // Author the key with the requested interpolation. AddConstantKey/AddLinearKey set their own
    // interp mode internally; AddCubicKey takes the tangent mode (auto by default, so the interp-omitted
    // path is byte-identical to the previous unconditional AddCubicKey call).
    switch (InterpMode)
    {
    case RCIM_Constant: Channel.AddConstantKey(FrameNumber, FloatValue);            break;
    case RCIM_Linear:   Channel.AddLinearKey(FrameNumber, FloatValue);             break;
    default:            Channel.AddCubicKey(FrameNumber, FloatValue, TangentMode); break;
    }

    // Explicit tangents are stamped AFTER the adder, not passed through it: the typed adders take a
    // tangent MODE and solve the values themselves, so the only way to seat caller-supplied numbers
    // is to overwrite them on the stored FMovieSceneFloatValue. Safe under user/break (the only
    // modes that reach here, per the validation above) because AutoSetTangents skips those keys.
    TMovieSceneChannelData<FMovieSceneFloatValue> ChannelData = Channel.GetData();
    const int32 WrittenKeyIndex = ChannelData.FindKey(FrameNumber);
    if (Tangents.IsSet() && WrittenKeyIndex != INDEX_NONE)
    {
        SequencerSectionHelpers::ApplyKeyTangents(ChannelData.GetValues()[WrittenKeyIndex], Tangents);
    }

    MovieScene->Modify();

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    AddAssetVerification(Out, LevelSequence);
    Out->SetStringField(TEXT("bindingGuid"), BindingGuidStr);
    Out->SetStringField(TEXT("propertyName"), PropertyName);
    Out->SetNumberField(TEXT("time"), TimeSeconds);
    Out->SetNumberField(TEXT("value"), Value);
    Out->SetStringField(TEXT("interp"), MovieSceneJsonUtils::InterpModeToString(InterpMode));
    Out->SetStringField(TEXT("tangentMode"), MovieSceneJsonUtils::TangentModeToString(TangentMode));
    // Echo the tangents as STORED, read back off the key that was just written rather than from
    // the request: that is what distinguishes "the numbers were accepted" from "the numbers are on
    // the curve", and it is the same pair sequencer.list_sections {includeKeys:true} reports.
    if (WrittenKeyIndex != INDEX_NONE)
    {
        const FMovieSceneFloatValue& Written = ChannelData.GetValues()[WrittenKeyIndex];
        Out->SetNumberField(TEXT("arriveTangent"), Written.Tangent.ArriveTangent);
        Out->SetNumberField(TEXT("leaveTangent"), Written.Tangent.LeaveTangent);
    }
    Ctx.SendSuccess(Out);
    return true;
}

// ---- sequencer.manage_track ----
REGISTER_RPC_HANDLER("sequencer.manage_track", "sequencer", "Add or remove a float-property track on an existing sequencer binding. Convenience for the most common animation property type.",
    RPC_PARAMS(
        RPC_PARAM_REQ("sequencePath", "path", "Path to the Level Sequence asset"),
        RPC_PARAM_REQ("bindingGuid", "string", "Object binding GUID"),
        RPC_PARAM_REQ("propertyName", "string", "Property name for the track"),
        RPC_PARAM_REQ("op", "string", "Operation: add or remove")
    ))
{
    FString SequencePath = Ctx.GetString(TEXT("sequencePath"));
    FString BindingGuidStr = Ctx.GetString(TEXT("bindingGuid"));
    FString PropertyName = Ctx.GetString(TEXT("propertyName"));
    FString Op = Ctx.GetString(TEXT("op"));

    if (SequencePath.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("sequencePath required")); return true; }
    if (BindingGuidStr.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("bindingGuid required")); return true; }
    if (PropertyName.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("propertyName required")); return true; }
    if (Op.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("op required (add/remove)")); return true; }

    ULevelSequence* LevelSequence = LoadObject<ULevelSequence>(nullptr, *SequencePath);
    if (!LevelSequence)
    {
        Ctx.SendError(TEXT("LOAD_FAILED"), TEXT("Failed to load LevelSequence"));
        return true;
    }
    UMovieScene* MovieScene = LevelSequence->GetMovieScene();
    if (!MovieScene)
    {
        Ctx.SendError(TEXT("INVALID_SEQUENCE"), TEXT("Sequence has no MovieScene"));
        return true;
    }
    FGuid BindingGuid;
    if (!FGuid::Parse(BindingGuidStr, BindingGuid))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Invalid bindingGuid"));
        return true;
    }
    FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuid);
    if (!Binding)
    {
        Ctx.SendError(TEXT("BINDING_NOT_FOUND"), TEXT("Binding not found in sequence"));
        return true;
    }

    bool bSuccess = false;
    if (Op.Equals(TEXT("add"), ESearchCase::IgnoreCase))
    {
        UMovieSceneFloatTrack* NewTrack = MovieScene->AddTrack<UMovieSceneFloatTrack>(BindingGuid);
        if (NewTrack)
        {
            NewTrack->SetPropertyNameAndPath(FName(*PropertyName), PropertyName);
            UMovieSceneSection* NewSection = NewTrack->CreateNewSection();
            if (NewSection)
            {
                NewTrack->AddSection(*NewSection);
            }
            MovieScene->Modify();
            bSuccess = true;
        }
    }
    else if (Op.Equals(TEXT("remove"), ESearchCase::IgnoreCase))
    {
        for (int32 i = Binding->GetTracks().Num() - 1; i >= 0; --i)
        {
            if (UMovieSceneFloatTrack* FT = Cast<UMovieSceneFloatTrack>(Binding->GetTracks()[i]))
            {
                if (FT->GetPropertyName().ToString().Equals(PropertyName, ESearchCase::IgnoreCase))
                {
                    MovieScene->RemoveTrack(*FT);
                    MovieScene->Modify();
                    bSuccess = true;
                    break;
                }
            }
        }
    }
    else
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Unsupported op; use add/remove"));
        return true;
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    AddAssetVerification(Out, LevelSequence);
    Out->SetBoolField(TEXT("success"), bSuccess);
    Out->SetStringField(TEXT("bindingGuid"), BindingGuidStr);
    Out->SetStringField(TEXT("propertyName"), PropertyName);
    Out->SetStringField(TEXT("op"), Op);

    if (bSuccess)
    {
        Ctx.SendSuccess(Out);
    }
    else
    {
        Ctx.SendError(TEXT("TRACK_OP_FAILED"), TEXT("Track operation failed"));
    }
    return true;
}

// ---- sequencer.add_camera_track ----
REGISTER_RPC_HANDLER("sequencer.add_camera_track", "sequencer", "Add a UMovieSceneCameraCutTrack to a level sequence so it switches between cameras during playback. Each section on the track binds to a camera actor for its time range.",
    RPC_PARAMS(
        RPC_PARAM_REQ("sequencePath", "path", "Path to the Level Sequence asset"),
        RPC_PARAM_REQ("cameraActorPath", "path", "Path to the camera actor"),
        RPC_PARAM_OPT("startTime", "number", "Start time in seconds (default 0)"),
        RPC_PARAM_OPT("endTime", "number", "End time in seconds (default 5)")
    ))
{
    FString SequencePath = Ctx.GetString(TEXT("sequencePath"));
    FString CameraActorPath = Ctx.GetString(TEXT("cameraActorPath"));

    if (SequencePath.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("sequencePath required")); return true; }
    if (CameraActorPath.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("cameraActorPath required")); return true; }

    double StartTime = Ctx.GetNumber(TEXT("startTime"), 0.0);
    double EndTime = Ctx.GetNumber(TEXT("endTime"), 5.0);

    ULevelSequence* LevelSequence = LoadObject<ULevelSequence>(nullptr, *SequencePath);
    if (!LevelSequence)
    {
        Ctx.SendError(TEXT("LOAD_FAILED"), TEXT("Failed to load LevelSequence"));
        return true;
    }

    UMovieScene* MovieScene = LevelSequence->GetMovieScene();
    if (!MovieScene)
    {
        Ctx.SendError(TEXT("INVALID_SEQUENCE"), TEXT("Sequence has no MovieScene"));
        return true;
    }

    ACameraActor* CameraActor = LoadObject<ACameraActor>(nullptr, *CameraActorPath);
    if (!CameraActor)
    {
        Ctx.SendError(TEXT("CAMERA_LOAD_FAILED"), TEXT("Failed to load camera actor"));
        return true;
    }

    // Resolve the binding GUID that actually corresponds to the NAMED camera. The camera must
    // already be bound into this sequence (e.g. via sequencer.add_actors, which calls
    // BindPossessableObject against the editor world). FindBindingFromObject is the reverse
    // lookup keyed on object identity; the (UObject* Context) overload is deprecated in 5.5+
    // but is the only headless-viable resolution path across UE 5.3-5.7 (the
    // SharedPlaybackState overload is not constructible headless), matching the resolver in
    // ControlRigSequencerHandler. Scanning for "the first camera-class possessable" instead
    // would bind the cut to an arbitrary camera, ignoring cameraActorPath.
    UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    FGuid CameraGuid;
    if (EditorWorld)
    {
        PRAGMA_DISABLE_DEPRECATION_WARNINGS
        CameraGuid = LevelSequence->FindBindingFromObject(CameraActor, EditorWorld);
        PRAGMA_ENABLE_DEPRECATION_WARNINGS
    }
    if (!CameraGuid.IsValid())
    {
        Ctx.SendError(TEXT("CAMERA_NOT_BOUND"),
            TEXT("The named camera has no binding in this sequence. Bind it first with "
                 "sequencer.add_actors, then add the camera-cut track."));
        return true;
    }

    UMovieSceneTrack* CutBase = MovieScene->GetCameraCutTrack();
    UMovieSceneCameraCutTrack* CameraCutTrack = CutBase ? Cast<UMovieSceneCameraCutTrack>(CutBase) : nullptr;

    if (!CameraCutTrack)
    {
        UMovieSceneTrack* NewTrack = MovieScene->AddCameraCutTrack(UMovieSceneCameraCutTrack::StaticClass());
        CameraCutTrack = Cast<UMovieSceneCameraCutTrack>(NewTrack);
    }

    if (CameraCutTrack)
    {
        // Seconds -> TICK-resolution frames: SetRange stores ticks, not DisplayRate frames.
        const FFrameNumber StartFrame = SequencerSectionHelpers::SecondsToTickFrame(MovieScene, StartTime);
        const FFrameNumber EndFrame = SequencerSectionHelpers::SecondsToTickFrame(MovieScene, EndTime);

        UMovieSceneCameraCutSection* CameraCutSection = Cast<UMovieSceneCameraCutSection>(CameraCutTrack->CreateNewSection());
        if (CameraCutSection)
        {
            CameraCutTrack->AddSection(*CameraCutSection);
            CameraCutSection->SetRange(TRange<FFrameNumber>(StartFrame, EndFrame));
            CameraCutSection->SetCameraBindingID(FMovieSceneObjectBindingID(CameraGuid));
            MovieScene->Modify();
        }
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    AddAssetVerification(Resp, LevelSequence);
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("cameraActorPath"), CameraActorPath);
    Resp->SetNumberField(TEXT("startTime"), StartTime);
    Resp->SetNumberField(TEXT("endTime"), EndTime);
    Ctx.SendSuccess(Resp);
    return true;
}

namespace
{
    // Shared implementation for sequencer.add_camera_rig_rail / sequencer.add_camera_rig_crane.
    // Engine has no dedicated UMovieSceneCameraRig*Track in UE 5.6 — these rigs animate via
    // a transform track plus generic UMovieSceneFloatTracks on Interp-tagged float properties.
    bool AddCameraRigTrackInternal(FHandlerContext& Ctx, UClass* ExpectedClass, TArrayView<const TCHAR* const> FloatPropertyNames)
    {
        FString SequencePath;
        if (!Ctx.RequireString(TEXT("sequencePath"), SequencePath))
        {
            return true;
        }

        FString ActorPath = Ctx.GetString(TEXT("actorPath"));

        ULevelSequence* LevelSequence = LoadObject<ULevelSequence>(nullptr, *SequencePath);
        if (!LevelSequence)
        {
            Ctx.SendError(TEXT("LOAD_FAILED"), TEXT("Failed to load LevelSequence"));
            return true;
        }

        UMovieScene* MovieScene = LevelSequence->GetMovieScene();
        if (!MovieScene)
        {
            Ctx.SendError(TEXT("INVALID_SEQUENCE"), TEXT("Sequence has no MovieScene"));
            return true;
        }

        FGuid BindingGuid;
        FString ResolvedActorPath;

        if (!ActorPath.IsEmpty())
        {
            AActor* RigActor = LoadObject<AActor>(nullptr, *ActorPath);
            if (!RigActor)
            {
                Ctx.SendError(TEXT("ACTOR_LOAD_FAILED"), TEXT("Failed to load rig actor at actorPath"));
                return true;
            }
            if (!RigActor->IsA(ExpectedClass))
            {
                Ctx.SendError(TEXT("WRONG_ACTOR_CLASS"), FString::Printf(TEXT("Actor is not a %s"), *ExpectedClass->GetName()));
                return true;
            }
            // Shared actor-possessable path (SequencerBindingUtils::BindActor). AddPossessable alone
            // mints an object-UNBOUND possessable: the GUID resolves to no object, so
            // FindBindingFromObject (the reverse lookup used by playback / editor rebinding) never
            // matches it and the transform + float tracks keyed to this GUID drive nothing. The
            // helper always follows the AddPossessable with BindPossessableObject against the editor
            // world the resolver scans. The else (spawnable) branch carries its own object template
            // and needs no binding reference.
            BindingGuid = SequencerBindingUtils::BindActor(LevelSequence, RigActor).Guid;
            ResolvedActorPath = ActorPath;
        }
        else
        {
            UObject* DefaultObject = ExpectedClass->GetDefaultObject();
            if (!DefaultObject)
            {
                Ctx.SendError(TEXT("SPAWNABLE_CREATION_FAILED"), TEXT("Rig class has no CDO"));
                return true;
            }
            const FString ClassName = ExpectedClass->GetName();
            BindingGuid = MovieScene->AddSpawnable(ClassName, *DefaultObject);
            ResolvedActorPath = ExpectedClass->GetPathName();
        }

        if (!BindingGuid.IsValid())
        {
            Ctx.SendError(TEXT("BINDING_CREATION_FAILED"), TEXT("Failed to create binding"));
            return true;
        }

        TArray<TSharedPtr<FJsonValue>> TrackArr;

        UMovieScene3DTransformTrack* TransformTrack = MovieScene->AddTrack<UMovieScene3DTransformTrack>(BindingGuid);
        if (TransformTrack)
        {
            UMovieScene3DTransformSection* TransformSection = Cast<UMovieScene3DTransformSection>(TransformTrack->CreateNewSection());
            if (TransformSection)
            {
                TransformTrack->AddSection(*TransformSection);
                TransformSection->SetRange(MovieScene->GetPlaybackRange());
            }
            TrackArr.Add(MakeShared<FJsonValueObject>(MovieSceneJsonUtils::BuildTrackJson(TransformTrack).ToSharedRef()));
        }

        for (const TCHAR* PropName : FloatPropertyNames)
        {
            UMovieSceneFloatTrack* FloatTrack = MovieScene->AddTrack<UMovieSceneFloatTrack>(BindingGuid);
            if (!FloatTrack)
            {
                continue;
            }
            FloatTrack->SetPropertyNameAndPath(FName(PropName), FString(PropName));
            UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(FloatTrack->CreateNewSection());
            if (FloatSection)
            {
                FloatTrack->AddSection(*FloatSection);
                FloatSection->SetRange(MovieScene->GetPlaybackRange());
            }
            TrackArr.Add(MakeShared<FJsonValueObject>(MovieSceneJsonUtils::BuildTrackJson(FloatTrack).ToSharedRef()));
        }

        // UMovieScene::AddTrack already calls Modify() per add — no trailing Modify() needed here.

        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField(TEXT("success"), true);
        Resp->SetStringField(TEXT("sequencePath"), SequencePath);
        Resp->SetStringField(TEXT("bindingGuid"), BindingGuid.ToString());
        Resp->SetStringField(TEXT("actorPath"), ResolvedActorPath);
        Resp->SetStringField(TEXT("mode"), ActorPath.IsEmpty() ? TEXT("spawned") : TEXT("possessed"));
        Resp->SetArrayField(TEXT("tracks"), TrackArr);
        AddAssetVerification(Resp, LevelSequence);
        Ctx.SendSuccess(Resp);
        return true;
    }
}

// ---- sequencer.add_camera_rig_rail ----
REGISTER_RPC_HANDLER("sequencer.add_camera_rig_rail", "sequencer", "Bind an ACameraRig_Rail to a level sequence (possess existing actor via actorPath, or spawn the rig as a spawnable) and scaffold a UMovieScene3DTransformTrack plus a UMovieSceneFloatTrack on the rig's CurrentPositionOnRail property. Engine has no dedicated rig-rail track class in UE 5.6 — animation flows through generic property tracks.",
    RPC_PARAMS(
        RPC_PARAM_REQ("sequencePath", "path", "Path to the Level Sequence asset"),
        RPC_PARAM_OPT("actorPath", "path", "Bind existing rail actor; if empty, spawn a new ACameraRig_Rail as spawnable")
    ))
{
    static const TCHAR* const RailProps[] = { TEXT("CurrentPositionOnRail") };
    return AddCameraRigTrackInternal(Ctx, ACameraRig_Rail::StaticClass(), MakeArrayView(RailProps));
}

// ---- sequencer.add_camera_rig_crane ----
REGISTER_RPC_HANDLER("sequencer.add_camera_rig_crane", "sequencer", "Bind an ACameraRig_Crane to a level sequence (possess existing actor via actorPath, or spawn the rig as a spawnable) and scaffold a UMovieScene3DTransformTrack plus UMovieSceneFloatTracks on the rig's CranePitch / CraneYaw / CraneArmLength properties. Engine has no dedicated rig-crane track class in UE 5.6 — animation flows through generic property tracks.",
    RPC_PARAMS(
        RPC_PARAM_REQ("sequencePath", "path", "Path to the Level Sequence asset"),
        RPC_PARAM_OPT("actorPath", "path", "Bind existing crane actor; if empty, spawn a new ACameraRig_Crane as spawnable")
    ))
{
    static const TCHAR* const CraneProps[] = { TEXT("CranePitch"), TEXT("CraneYaw"), TEXT("CraneArmLength") };
    return AddCameraRigTrackInternal(Ctx, ACameraRig_Crane::StaticClass(), MakeArrayView(CraneProps));
}

// ---- sequencer.add_level_visibility_track ----
REGISTER_RPC_HANDLER("sequencer.add_level_visibility_track", "sequencer", "Add a UMovieSceneLevelVisibilityTrack section to a level sequence, toggling a set of streaming sublevels visible or hidden over a frame range. By default (reuseTrack=true) appends a new section to the existing master track; pass reuseTrack=false to force a new track.",
    RPC_PARAMS(
        RPC_PARAM_REQ("sequencePath", "path", "Level sequence asset path"),
        RPC_PARAM_REQ("levelNames", "array", "Streaming sublevel short package names"),
        RPC_PARAM_REQ("bVisible", "bool", "True - ELevelVisibility::Visible; false - Hidden"),
        RPC_PARAM_OPT("range", "object", "{ start, end } frame numbers; default = sequence playback range"),
        RPC_PARAM_OPT("rowIndex", "integer", "Track row index; default 0"),
        RPC_PARAM_OPT("reuseTrack", "bool", "If true (default) append to existing master track when present")
    ))
{
    FString SequencePath;
    if (!Ctx.RequireString(TEXT("sequencePath"), SequencePath)) { return true; }

    const TArray<TSharedPtr<FJsonValue>>* NamesArr = nullptr;
    if (!Ctx.RequireArray(TEXT("levelNames"), NamesArr)) { return true; }
    if (NamesArr->Num() == 0)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("levelNames must be a non-empty array"));
        return true;
    }

    TArray<FName> LevelNames;
    LevelNames.Reserve(NamesArr->Num());
    for (const TSharedPtr<FJsonValue>& V : *NamesArr)
    {
        if (V.IsValid() && V->Type == EJson::String)
        {
            LevelNames.Add(FName(*V->AsString()));
        }
    }

    bool bVisible = true;
    if (!Ctx.RequireBool(TEXT("bVisible"), bVisible)) { return true; }
    const int32 RowIndex = Ctx.GetInt(TEXT("rowIndex"), 0);
    const bool bReuseTrack = Ctx.GetBool(TEXT("reuseTrack"), true);

    ULevelSequence* LevelSequence = LoadObject<ULevelSequence>(nullptr, *SequencePath);
    if (!LevelSequence)
    {
        Ctx.SendError(TEXT("LOAD_FAILED"), TEXT("Failed to load LevelSequence"));
        return true;
    }

    UMovieScene* MovieScene = LevelSequence->GetMovieScene();
    if (!MovieScene)
    {
        Ctx.SendError(TEXT("INVALID_SEQUENCE"), TEXT("Sequence has no MovieScene"));
        return true;
    }

    TRange<FFrameNumber> Range = MovieScene->GetPlaybackRange();
    TSharedPtr<FJsonObject> RangeObj = Ctx.GetObject(TEXT("range"));
    if (RangeObj.IsValid())
    {
        double StartNum = 0.0;
        double EndNum = 0.0;
        const bool bHasStart = RangeObj->TryGetNumberField(TEXT("start"), StartNum);
        const bool bHasEnd = RangeObj->TryGetNumberField(TEXT("end"), EndNum);
        if (bHasStart && bHasEnd)
        {
            Range = TRange<FFrameNumber>(FFrameNumber(static_cast<int32>(StartNum)), FFrameNumber(static_cast<int32>(EndNum)));
        }
    }

    bool bReused = false;
    UMovieSceneLevelVisibilityTrack* Track = nullptr;
    if (bReuseTrack)
    {
        Track = MovieScene->FindTrack<UMovieSceneLevelVisibilityTrack>();
        bReused = (Track != nullptr);
    }
    if (!Track)
    {
        Track = MovieScene->AddTrack<UMovieSceneLevelVisibilityTrack>();
    }
    if (!Track)
    {
        Ctx.SendError(TEXT("TRACK_CREATION_FAILED"), TEXT("Failed to create UMovieSceneLevelVisibilityTrack"));
        return true;
    }

    UMovieSceneLevelVisibilitySection* Section = Cast<UMovieSceneLevelVisibilitySection>(Track->CreateNewSection());
    if (!Section)
    {
        Ctx.SendError(TEXT("SECTION_CREATION_FAILED"), TEXT("Failed to create UMovieSceneLevelVisibilitySection"));
        return true;
    }
    Track->AddSection(*Section);
    Section->SetRange(Range);
    Section->SetRowIndex(RowIndex);
    Section->SetLevelNames(LevelNames);
    Section->SetVisibility(bVisible ? ELevelVisibility::Visible : ELevelVisibility::Hidden);

    MovieScene->Modify();
    LevelSequence->MarkPackageDirty();

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("sequencePath"), SequencePath);
    Resp->SetObjectField(TEXT("track"), MovieSceneJsonUtils::BuildTrackJson(Track));
    Resp->SetObjectField(TEXT("section"), MovieSceneJsonUtils::BuildSectionJson(Section));
    Resp->SetBoolField(TEXT("reused"), bReused);
    AddAssetVerification(Resp, LevelSequence);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- sequencer.add_animation_track ----
REGISTER_RPC_HANDLER("sequencer.add_animation_track", "sequencer", "Add a UMovieSceneSkeletalAnimationTrack to a level sequence binding so the bound skeletal mesh actor plays an animation asset over a time range.",
    RPC_PARAMS(
        RPC_PARAM_REQ("sequencePath", "path", "Path to the Level Sequence asset"),
        RPC_PARAM_REQ("bindingGuid", "string", "Object binding GUID"),
        RPC_PARAM_REQ("animSequencePath", "path", "Path to the animation sequence asset"),
        RPC_PARAM_OPT("startTime", "number", "Start time in seconds (default 0)")
    ))
{
    FString SequencePath = Ctx.GetString(TEXT("sequencePath"));
    FString BindingGuidStr = Ctx.GetString(TEXT("bindingGuid"));
    FString AnimSequencePath = Ctx.GetString(TEXT("animSequencePath"));

    if (SequencePath.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("sequencePath required")); return true; }
    if (BindingGuidStr.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("bindingGuid required")); return true; }
    if (AnimSequencePath.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("animSequencePath required")); return true; }

    double StartTime = Ctx.GetNumber(TEXT("startTime"), 0.0);

    ULevelSequence* LevelSequence = LoadObject<ULevelSequence>(nullptr, *SequencePath);
    if (!LevelSequence)
    {
        Ctx.SendError(TEXT("LOAD_FAILED"), TEXT("Failed to load LevelSequence"));
        return true;
    }

    UMovieScene* MovieScene = LevelSequence->GetMovieScene();
    if (!MovieScene)
    {
        Ctx.SendError(TEXT("INVALID_SEQUENCE"), TEXT("Sequence has no MovieScene"));
        return true;
    }

    FGuid BindingGuid;
    if (!FGuid::Parse(BindingGuidStr, BindingGuid))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Invalid bindingGuid"));
        return true;
    }

    UAnimSequence* AnimSequence = LoadObject<UAnimSequence>(nullptr, *AnimSequencePath);
    if (!AnimSequence)
    {
        Ctx.SendError(TEXT("ANIM_LOAD_FAILED"), TEXT("Failed to load animation sequence"));
        return true;
    }

    UMovieSceneSkeletalAnimationTrack* AnimTrack = MovieScene->AddTrack<UMovieSceneSkeletalAnimationTrack>(BindingGuid);
    if (!AnimTrack)
    {
        Ctx.SendError(TEXT("TRACK_CREATION_FAILED"), TEXT("Failed to create animation track"));
        return true;
    }

    UMovieSceneSection* NewSection = AnimTrack->CreateNewSection();
    UMovieSceneSkeletalAnimationSection* AnimSection = Cast<UMovieSceneSkeletalAnimationSection>(NewSection);
    if (AnimSection)
    {
        AnimTrack->AddSection(*AnimSection);
        AnimSection->Params.Animation = AnimSequence;

        // Seconds -> TICK-resolution frames: SetRange stores ticks, not DisplayRate frames.
        const FFrameNumber StartFrame = SequencerSectionHelpers::SecondsToTickFrame(MovieScene, StartTime);
        const float AnimLength = AnimSequence->GetPlayLength();
        const FFrameNumber EndFrame = SequencerSectionHelpers::SecondsToTickFrame(MovieScene, StartTime + AnimLength);

        AnimSection->SetRange(TRange<FFrameNumber>(StartFrame, EndFrame));
        MovieScene->Modify();
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    AddAssetVerification(Resp, LevelSequence);
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("bindingGuid"), BindingGuidStr);
    Resp->SetStringField(TEXT("animSequencePath"), AnimSequencePath);
    Resp->SetNumberField(TEXT("startTime"), StartTime);
    Resp->SetNumberField(TEXT("animLength"), AnimSequence->GetPlayLength());
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- sequencer.add_transform_track ----
REGISTER_RPC_HANDLER("sequencer.add_transform_track", "sequencer", "Add a UMovieScene3DTransformTrack to a level sequence binding so the bound actor's location/rotation/scale can be animated. Each transform component is keyframed independently.",
    RPC_PARAMS(
        RPC_PARAM_REQ("sequencePath", "path", "Path to the Level Sequence asset"),
        RPC_PARAM_REQ("bindingGuid", "string", "Object binding GUID")
    ))
{
    FString SequencePath = Ctx.GetString(TEXT("sequencePath"));
    FString BindingGuidStr = Ctx.GetString(TEXT("bindingGuid"));

    if (SequencePath.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("sequencePath required")); return true; }
    if (BindingGuidStr.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("bindingGuid required")); return true; }

    ULevelSequence* LevelSequence = LoadObject<ULevelSequence>(nullptr, *SequencePath);
    if (!LevelSequence)
    {
        Ctx.SendError(TEXT("LOAD_FAILED"), TEXT("Failed to load LevelSequence"));
        return true;
    }

    UMovieScene* MovieScene = LevelSequence->GetMovieScene();
    if (!MovieScene)
    {
        Ctx.SendError(TEXT("INVALID_SEQUENCE"), TEXT("Sequence has no MovieScene"));
        return true;
    }

    FGuid BindingGuid;
    if (!FGuid::Parse(BindingGuidStr, BindingGuid))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Invalid bindingGuid"));
        return true;
    }

    UMovieScene3DTransformTrack* TransformTrack = MovieScene->AddTrack<UMovieScene3DTransformTrack>(BindingGuid);
    if (!TransformTrack)
    {
        Ctx.SendError(TEXT("TRACK_CREATION_FAILED"), TEXT("Failed to create transform track"));
        return true;
    }

    UMovieScene3DTransformSection* TransformSection = Cast<UMovieScene3DTransformSection>(TransformTrack->CreateNewSection());
    if (TransformSection)
    {
        TransformTrack->AddSection(*TransformSection);
        MovieScene->Modify();
    }

    // The section this handler just made carries no keys — CreateNewSection() seeds none and
    // nothing here writes a channel key. Report the section's REAL key count (0 on a fresh
    // section) and derive hasDefaultKeyframes from it, instead of the former hardcoded true that
    // asserted default keyframes existed on a section it had just created empty
    // (board E-sequencer-add-transform-track-phantom-default-keyframes). Callers author keys via
    // sequence.add_keyframe; the field auto-corrects if this handler ever starts seeding one.
    const int32 KeyCount = MovieSceneJsonUtils::CountSectionKeys(TransformSection);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    AddAssetVerification(Resp, LevelSequence);
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("bindingGuid"), BindingGuidStr);
    Resp->SetNumberField(TEXT("keyCount"), KeyCount);
    Resp->SetBoolField(TEXT("hasDefaultKeyframes"), KeyCount > 0);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- sequencer.add_audio_track ----
REGISTER_RPC_HANDLER("sequencer.add_audio_track", "sequencer", "Add a UMovieSceneAudioTrack to a level sequence — master (no bindingGuid) or attached to an object binding — and create a UMovieSceneAudioSection playing the given USoundBase (SoundWave/SoundCue/MetaSound source). Volume and pitch are written as channel defaults (no public section setters exist).",
    RPC_PARAMS(
        RPC_PARAM_REQ("sequencePath", "path", "Level sequence asset path"),
        RPC_PARAM_REQ("soundPath", "path", "Path to a USoundBase asset (SoundWave, SoundCue, MetaSound source)"),
        RPC_PARAM_OPT("bindingGuid", "string", "Object binding GUID; when omitted creates a root/master track"),
        RPC_PARAM_OPT("startTime", "number", "Start time in seconds (default 0)"),
        RPC_PARAM_OPT("duration", "number", "Duration in seconds; default = SoundBase->GetDuration() with looping-sentinel fallback to 1.0"),
        RPC_PARAM_OPT("volume", "number", "Volume channel default; default 1.0"),
        RPC_PARAM_OPT("pitch", "number", "Pitch multiplier channel default; default 1.0"),
        RPC_PARAM_OPT("rowIndex", "integer", "Track row index; default 0")
    ))
{
    FString SequencePath;
    if (!Ctx.RequireString(TEXT("sequencePath"), SequencePath)) { return true; }
    FString SoundPath;
    if (!Ctx.RequireString(TEXT("soundPath"), SoundPath)) { return true; }

    FGuid BindingGuid;
    const FString BindingGuidStr = Ctx.GetString(TEXT("bindingGuid"));
    if (!BindingGuidStr.IsEmpty())
    {
        if (!FGuid::Parse(BindingGuidStr, BindingGuid))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("bindingGuid is not a valid GUID"));
            return true;
        }
    }

    ULevelSequence* LevelSequence = LoadObject<ULevelSequence>(nullptr, *SequencePath);
    if (!LevelSequence)
    {
        Ctx.SendError(TEXT("LOAD_FAILED"), TEXT("Failed to load LevelSequence"));
        return true;
    }

    UMovieScene* MovieScene = LevelSequence->GetMovieScene();
    if (!MovieScene)
    {
        Ctx.SendError(TEXT("INVALID_SEQUENCE"), TEXT("Sequence has no MovieScene"));
        return true;
    }

    USoundBase* SoundBase = LoadObject<USoundBase>(nullptr, *SoundPath);
    if (!SoundBase)
    {
        Ctx.SendError(TEXT("SOUND_LOAD_FAILED"), TEXT("Failed to load USoundBase"));
        return true;
    }

    UMovieSceneAudioTrack* Track = BindingGuid.IsValid()
        ? MovieScene->AddTrack<UMovieSceneAudioTrack>(BindingGuid)
        : MovieScene->AddTrack<UMovieSceneAudioTrack>();
    if (!Track)
    {
        Ctx.SendError(TEXT("TRACK_CREATION_FAILED"), TEXT("Failed to create audio track"));
        return true;
    }

    UMovieSceneAudioSection* Section = Cast<UMovieSceneAudioSection>(Track->CreateNewSection());
    if (!Section)
    {
        Ctx.SendError(TEXT("SECTION_CREATION_FAILED"), TEXT("Failed to create audio section"));
        return true;
    }
    Track->AddSection(*Section);
    Section->SetSound(SoundBase);

    const double StartTime = Ctx.GetNumber(TEXT("startTime"), 0.0);
    const TSharedPtr<FJsonObject>& Raw = Ctx.GetRawPayload();
    const bool bHasDuration = Raw.IsValid() && Raw->HasField(TEXT("duration"));
    const bool bHasRowIndex = Raw.IsValid() && Raw->HasField(TEXT("rowIndex"));
    double Duration = 0.0;
    if (bHasDuration)
    {
        Duration = Ctx.GetNumber(TEXT("duration"), 0.0);
    }
    else
    {
        // Looping/streaming sounds report INDEFINITELY_LOOPING_DURATION (10000.0f from AudioDefines.h) — fall back to 1s.
        const float RawDur = SoundBase->GetDuration();
        Duration = (RawDur <= 0.f || RawDur == INDEFINITELY_LOOPING_DURATION) ? 1.0 : static_cast<double>(RawDur);
    }

    // Seconds -> TICK-resolution frames: SetRange stores ticks, not DisplayRate frames.
    const FFrameNumber StartFrame = SequencerSectionHelpers::SecondsToTickFrame(MovieScene, StartTime);
    const FFrameNumber EndFrame = SequencerSectionHelpers::SecondsToTickFrame(MovieScene, StartTime + Duration);
    Section->SetRange(TRange<FFrameNumber>(StartFrame, EndFrame));

    if (bHasRowIndex)
    {
        const int32 RowIndex = Ctx.GetInt(TEXT("rowIndex"), 0);
        Section->SetRowIndex(RowIndex);
    }

    const float Volume = static_cast<float>(Ctx.GetNumber(TEXT("volume"), 1.0));
    const float Pitch  = static_cast<float>(Ctx.GetNumber(TEXT("pitch"),  1.0));
    // UMovieSceneAudioSection exposes no public Volume/Pitch setters — channel defaults are the supported path.
    FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
    if (FMovieSceneFloatChannel* VolChan = Proxy.GetChannel<FMovieSceneFloatChannel>(0))
    {
        VolChan->SetDefault(Volume);
    }
    if (FMovieSceneFloatChannel* PitchChan = Proxy.GetChannel<FMovieSceneFloatChannel>(1))
    {
        PitchChan->SetDefault(Pitch);
    }

    MovieScene->Modify();
    LevelSequence->MarkPackageDirty();

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("sequencePath"), SequencePath);
    Resp->SetStringField(TEXT("soundPath"), SoundPath);
    if (BindingGuid.IsValid())
    {
        Resp->SetStringField(TEXT("bindingGuid"), BindingGuid.ToString());
    }
    else
    {
        Resp->SetBoolField(TEXT("master"), true);
    }
    Resp->SetNumberField(TEXT("startTime"), StartTime);
    Resp->SetNumberField(TEXT("duration"), Duration);
    Resp->SetNumberField(TEXT("volume"), Volume);
    Resp->SetNumberField(TEXT("pitch"), Pitch);
    Resp->SetObjectField(TEXT("track"), MovieSceneJsonUtils::BuildTrackJson(Track));
    Resp->SetObjectField(TEXT("section"), MovieSceneJsonUtils::BuildSectionJson(Section));
    AddAssetVerification(Resp, LevelSequence);
    Ctx.SendSuccess(Resp);
    return true;
}
