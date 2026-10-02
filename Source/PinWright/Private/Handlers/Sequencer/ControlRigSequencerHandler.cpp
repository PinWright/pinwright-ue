// Copyright (c) 2026 Alexander Penkin. MIT License.

// Control Rig in Sequencer: add a Control Rig track to a binding, enumerate its
// controls, key controls at frames, and read the keyed control channel values back.
//
// Implements the F-sequencer-controlrig-track capability gap: PinWright's sequencer
// namespace had zero Control Rig support (the controlrig namespace was CRIR asset
// round-trip only). These four verbs are the entry point to the CR-in-Sequencer
// cinematics workflow (bake / layers / spaces all build on a CR track existing).
//
// Headless design (this runs under automation with NO open Sequencer in the level
// editor): the engine's UControlRigSequencerEditorLibrary "Local" get/set functions
// route through GetSequencerFromAsset() and no-op without an open Sequencer, and its
// FindOrCreateControlRigTrack leaves an FK rig unbound (no controls). So we replicate
// the track editor's bind-then-initialize flow (FControlRigParameterTrackEditor::
// AddControlRig) directly and drive the UMovieSceneControlRigParameterSection's float
// channels ourselves, which is fully self-contained and needs no live Sequencer.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Utils/AssetUtils.h"
#include "Utils/ClassUtils.h"
#include "Compat/EngineVersionCompat.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "ScopedTransaction.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBindingProxy.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneChannelProxy.h"

#include "GameFramework/Actor.h"
#include "Animation/SkeletalMeshActor.h"
#include "Components/SkeletalMeshComponent.h"

#include "ControlRig.h"
#include "AnimationDataSource.h"
#include "ControlRigObjectBinding.h"
#include "Rigs/FKControlRig.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyElements.h"
#include "Sequencer/MovieSceneControlRigParameterTrack.h"
#include "Sequencer/MovieSceneControlRigParameterSection.h"
#include "Tracks/MovieScene3DAttachTrack.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Components/SceneComponent.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/Sequencer/ControlRigKeyTestHooks.h"

namespace PinWrightControlRigSequencer
{
    // Resolve the level sequence from a "sequence"/"path" payload key. Mirrors the
    // SequenceHandler resolution: accepts a /Game asset path.
    static ULevelSequence* ResolveLevelSequence(const TSharedPtr<FJsonObject>& Payload, FString& OutPath)
    {
        FString Path;
        if (Payload.IsValid())
        {
            if (!Payload->TryGetStringField(TEXT("sequence"), Path) || Path.IsEmpty())
            {
                Payload->TryGetStringField(TEXT("path"), Path);
            }
        }
        OutPath = Path;
        if (Path.IsEmpty() || !ResolveAsset(Path).bExists)
        {
            return nullptr;
        }
        return Cast<ULevelSequence>(ResolveAsset(Path, /*bLoadObject=*/true).Object);
    }

    // Read the binding GUID from "binding"/"bindingGuid"/"bindingId" (Digits form).
    static FGuid ResolveBindingGuid(const TSharedPtr<FJsonObject>& Payload)
    {
        FGuid Guid;
        if (!Payload.IsValid())
        {
            return Guid;
        }
        FString GuidStr;
        if (Payload->TryGetStringField(TEXT("bindingGuid"), GuidStr) ||
            Payload->TryGetStringField(TEXT("binding"), GuidStr) ||
            Payload->TryGetStringField(TEXT("bindingId"), GuidStr))
        {
            FGuid::Parse(GuidStr, Guid);
        }
        return Guid;
    }

    // Locate the object bound to a possessable GUID in the editor world by scanning
    // actors and asking the sequence for each actor's binding. This is the headless
    // reverse of BindPossessableObject and needs no live playback state.
    static USkeletalMeshComponent* ResolveBoundSkeletalMeshComponent(ULevelSequence* Sequence, const FGuid& BindingGuid)
    {
        if (!Sequence || !BindingGuid.IsValid() || !GEditor)
        {
            return nullptr;
        }
        UWorld* World = GEditor->GetEditorWorldContext().World();
        if (!World)
        {
            return nullptr;
        }
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (!Actor)
            {
                continue;
            }
            // The SharedPlaybackState overload is not constructible headless; the
            // (UObject* Context) reverse-lookup is deprecated in 5.7 but functional and
            // is the only headless-viable resolution path across UE 5.3-5.7.
            FGuid ActorBinding;
            PRAGMA_DISABLE_DEPRECATION_WARNINGS
            ActorBinding = Sequence->FindBindingFromObject(Actor, World);
            PRAGMA_ENABLE_DEPRECATION_WARNINGS
            if (ActorBinding == BindingGuid)
            {
                if (ASkeletalMeshActor* SkelActor = Cast<ASkeletalMeshActor>(Actor))
                {
                    if (USkeletalMeshComponent* Comp = SkelActor->GetSkeletalMeshComponent())
                    {
                        return Comp;
                    }
                }
                if (USkeletalMeshComponent* Root = Cast<USkeletalMeshComponent>(Actor->GetRootComponent()))
                {
                    return Root;
                }
                TArray<USkeletalMeshComponent*> Comps;
                Actor->GetComponents(Comps);
                if (Comps.Num() > 0)
                {
                    return Comps[0];
                }
            }
        }
        return nullptr;
    }

    // Find the Control Rig track already present on a binding (if any).
    static UMovieSceneControlRigParameterTrack* FindControlRigTrack(UMovieScene* MovieScene, const FGuid& BindingGuid)
    {
        if (!MovieScene || !BindingGuid.IsValid())
        {
            return nullptr;
        }
        TArray<UMovieSceneTrack*> Tracks =
            MovieScene->FindTracks(UMovieSceneControlRigParameterTrack::StaticClass(), BindingGuid, NAME_None);
        for (UMovieSceneTrack* Track : Tracks)
        {
            if (UMovieSceneControlRigParameterTrack* CRTrack = Cast<UMovieSceneControlRigParameterTrack>(Track))
            {
                return CRTrack;
            }
        }
        return nullptr;
    }

    // Convert an incoming display-rate frame number to the movie scene's tick
    // resolution (channels are keyed/evaluated in tick space).
    static FFrameNumber DisplayFrameToTick(const UMovieScene* MovieScene, double DisplayFrame)
    {
        if (!MovieScene)
        {
            return FFrameNumber(static_cast<int32>(FMath::RoundToInt(DisplayFrame)));
        }
        const FFrameRate Tick = MovieScene->GetTickResolution();
        const FFrameRate Display = MovieScene->GetDisplayRate();
        return FFrameRate::TransformTime(FFrameTime(FFrameNumber(static_cast<int32>(FMath::RoundToInt(DisplayFrame)))), Display, Tick).RoundToFrame();
    }

#if UE_VERSION_OLDER_THAN(5, 6, 0)
    // How many of the section's FLOAT channels a control of this type contributes. Bool /
    // Integer / Enum controls live in their own channel arrays and contribute none. Only
    // needed pre-5.6, where the section carries no per-channel metadata and the mapping has
    // to be reconstructed from the control's type (mirrors the engine's own
    // FControlRigSequencerHelpers::GetInfoAndNumFloatChannels, extended with the scalar and
    // 2D kinds that helper skips).
    static int32 NumFloatChannelsForControlType(ERigControlType Type)
    {
        switch (Type)
        {
        case ERigControlType::Float:            return 1;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        case ERigControlType::ScaleFloat:       return 1;
#endif
        case ERigControlType::Vector2D:         return 2;
        case ERigControlType::Position:
        case ERigControlType::Scale:
        case ERigControlType::Rotator:          return 3;
        case ERigControlType::TransformNoScale: return 6;
        case ERigControlType::Transform:
        case ERigControlType::EulerTransform:   return 9;
        default:                                return 0;
        }
    }

    // Start index (into the section's float-channel array) and channel count of a control,
    // read from the section's ControlChannelMap. Entries whose ChannelTypeName is set index
    // the bool/enum/integer arrays instead, so they are rejected here.
    static bool FloatChannelRangeForControl(UMovieSceneControlRigParameterSection* Section,
        FName ControlName, int32& OutStart, int32& OutCount)
    {
        OutStart = 0;
        OutCount = 0;
        UControlRig* Rig = Section ? Section->GetControlRig() : nullptr;
        const FChannelMapInfo* Info = Section ? Section->ControlChannelMap.Find(ControlName) : nullptr;
        const FRigControlElement* Element = Rig ? Rig->FindControl(ControlName) : nullptr;
        if (!Info || !Element || Info->ChannelTypeName != NAME_None)
        {
            return false;
        }
        const int32 Count = NumFloatChannelsForControlType(Element->Settings.ControlType);
        if (Count <= 0 || Info->ChannelIndex < 0)
        {
            return false;
        }
        OutStart = Info->ChannelIndex;
        OutCount = Count;
        return true;
    }
#endif // UE_VERSION_OLDER_THAN(5, 6, 0)

    // Return the section's float channels that belong to a named control, ordered by
    // their index within the control. Empty if the control has no float channels.
    static TArray<FMovieSceneFloatChannel*> ChannelsForControl(UMovieSceneControlRigParameterSection* Section, FName ControlName)
    {
        TArray<FMovieSceneFloatChannel*> Result;
        if (!Section)
        {
            return Result;
        }
        FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
        TArrayView<FMovieSceneFloatChannel*> FloatChannels = Proxy.GetChannels<FMovieSceneFloatChannel>();
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        // Collect (indexWithinControl, channel) then order by index so channel 0 is stable.
        TArray<TPair<int32, FMovieSceneFloatChannel*>> Matched;
        for (FMovieSceneFloatChannel* Channel : FloatChannels)
        {
            if (!Channel)
            {
                continue;
            }
            UE::MovieScene::FControlRigChannelMetaData Meta = Section->GetChannelMetaData(Channel);
            if (static_cast<bool>(Meta) && Meta.GetControlName() == ControlName)
            {
                Matched.Add(TPair<int32, FMovieSceneFloatChannel*>(Meta.GetChannelIndex(), Channel));
            }
        }
        Matched.Sort([](const TPair<int32, FMovieSceneFloatChannel*>& A, const TPair<int32, FMovieSceneFloatChannel*>& B)
        {
            return A.Key < B.Key;
        });
        for (const TPair<int32, FMovieSceneFloatChannel*>& Pair : Matched)
        {
            Result.Add(Pair.Value);
        }
#else
        // UMovieSceneControlRigParameterSection::GetChannelMetaData (and the
        // FControlRigChannelMetaData it returns) only exist on 5.6+. Before that the
        // control's channels are a contiguous run in the section's float-channel array,
        // located through the section's ControlChannelMap.
        int32 Start = 0;
        int32 Count = 0;
        if (!FloatChannelRangeForControl(Section, ControlName, Start, Count) ||
            Start + Count > FloatChannels.Num())
        {
            return Result;
        }
        for (int32 Index = 0; Index < Count; ++Index)
        {
            if (FMovieSceneFloatChannel* Channel = FloatChannels[Start + Index])
            {
                Result.Add(Channel);
            }
        }
#endif
        return Result;
    }

    // Names of the controls that have keyable float channels on this section, in section
    // channel order (so every listed control is guaranteed keyable via key_controls).
    static TArray<FName> ControlsWithFloatChannels(UMovieSceneControlRigParameterSection* Section)
    {
        TArray<FName> Names;
        if (!Section)
        {
            return Names;
        }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        TSet<FName> Seen;
        for (FMovieSceneFloatChannel* Channel : Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>())
        {
            if (!Channel)
            {
                continue;
            }
            UE::MovieScene::FControlRigChannelMetaData Meta = Section->GetChannelMetaData(Channel);
            if (!static_cast<bool>(Meta))
            {
                continue;
            }
            const FName CName = Meta.GetControlName();
            if (!Seen.Contains(CName))
            {
                Seen.Add(CName);
                Names.Add(CName);
            }
        }
#else
        // Pre-5.6: walk the section's control map and keep the controls that own float
        // channels, ordered by their float-channel start index to reproduce section order.
        //
        // ControlChannelMap is rebuilt from the rig's controls inside CacheChannelProxy(),
        // which the section runs lazily the first time its channel proxy is requested. A
        // section created this frame (sequencer.add_controlrig_track) therefore still has an
        // EMPTY map until someone touches GetChannelProxy(); reading it directly would list
        // zero controls for a rig that has hundreds. Request the proxy first so the map is
        // current. The 5.6+ branch above gets this for free by enumerating the proxy itself.
        Section->GetChannelProxy();

        TArray<TPair<int32, FName>> Ordered;
        for (const TPair<FName, FChannelMapInfo>& Entry : Section->ControlChannelMap)
        {
            int32 Start = 0;
            int32 Count = 0;
            if (FloatChannelRangeForControl(Section, Entry.Key, Start, Count))
            {
                Ordered.Add(TPair<int32, FName>(Start, Entry.Key));
            }
        }
        Ordered.Sort([](const TPair<int32, FName>& A, const TPair<int32, FName>& B)
        {
            return A.Key < B.Key;
        });
        for (const TPair<int32, FName>& Entry : Ordered)
        {
            Names.Add(Entry.Value);
        }
#endif
        return Names;
    }

    // Whether the rig composes additively (a layered rig), so a caller reading channel
    // values knows they are layer deltas. UControlRig::IsAdditive() arrived in 5.4; on 5.3
    // only the FK rig has an additive apply mode and every other rig is absolute.
    static bool IsRigAdditive(UControlRig* Rig)
    {
        if (!Rig)
        {
            return false;
        }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        return Rig->IsAdditive();
#else
        const UFKControlRig* FKRig = Cast<UFKControlRig>(Rig);
        return FKRig && FKRig->IsApplyModeAdditive();
#endif
    }

    static FString ControlTypeToString(ERigControlType Type)
    {
        switch (Type)
        {
        case ERigControlType::Bool:            return TEXT("Bool");
        case ERigControlType::Float:           return TEXT("Float");
        // ERigControlType::ScaleFloat was added in UE 5.4.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        case ERigControlType::ScaleFloat:      return TEXT("ScaleFloat");
#endif
        case ERigControlType::Integer:         return TEXT("Integer");
        case ERigControlType::Vector2D:        return TEXT("Vector2D");
        case ERigControlType::Position:        return TEXT("Position");
        case ERigControlType::Scale:           return TEXT("Scale");
        case ERigControlType::Rotator:         return TEXT("Rotator");
        case ERigControlType::Transform:       return TEXT("Transform");
        case ERigControlType::TransformNoScale:return TEXT("TransformNoScale");
        case ERigControlType::EulerTransform:  return TEXT("EulerTransform");
        default:                               return TEXT("Unknown");
        }
    }

    // Resolve the CR track + its section for the read/key verbs that operate on an
    // already-created track (list_controls, key_controls, get_control_value). On failure
    // it sends the appropriate SEQUENCE_NOT_FOUND / CONTROLRIG_TRACK_NOT_FOUND error and
    // returns false. bPreferSectionToKey mirrors the track editor's key path: key_controls
    // targets Track->GetSectionToKey() first, the read verbs take the first section.
    static bool ResolveControlRigSection(
        FHandlerContext& Ctx,
        const TSharedPtr<FJsonObject>& Payload,
        bool bPreferSectionToKey,
        FString& OutSeqPath,
        UMovieScene*& OutMovieScene,
        FGuid& OutBindingGuid,
        UMovieSceneControlRigParameterTrack*& OutTrack,
        UMovieSceneControlRigParameterSection*& OutSection)
    {
        OutMovieScene = nullptr;
        OutTrack = nullptr;
        OutSection = nullptr;

        ULevelSequence* Sequence = ResolveLevelSequence(Payload, OutSeqPath);
        if (!Sequence || !Sequence->GetMovieScene())
        {
            Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_FOUND,
                FString::Printf(TEXT("Level sequence not found for path '%s'."), *OutSeqPath));
            return false;
        }
        OutMovieScene = Sequence->GetMovieScene();
        OutBindingGuid = ResolveBindingGuid(Payload);
        OutTrack = FindControlRigTrack(OutMovieScene, OutBindingGuid);
        if (OutTrack)
        {
            if (bPreferSectionToKey)
            {
                OutSection = Cast<UMovieSceneControlRigParameterSection>(OutTrack->GetSectionToKey());
            }
            if (!OutSection && OutTrack->GetAllSections().Num() > 0)
            {
                OutSection = Cast<UMovieSceneControlRigParameterSection>(OutTrack->GetAllSections()[0]);
            }
        }
        if (!OutTrack || !OutSection)
        {
            Ctx.SendError(ErrorCodes::ERR_CONTROLRIG_TRACK_NOT_FOUND,
                TEXT("No Control Rig track/section on this binding. Call sequencer.add_controlrig_track first."));
            return false;
        }
        return true;
    }
}

// sequencer.add_controlrig_track — add a Control Rig track to a skeletal-mesh binding.
// FK fallback (no rigClass): an FK Control Rig is generated from the bound skeletal
// mesh's skeleton, so its per-bone controls become keyable. Idempotent: returns the
// existing track if one already exists for the class on the binding.
REGISTER_RPC_HANDLER("sequencer.add_controlrig_track", "sequencer",
    "Add a Control Rig track to a skeletal-mesh binding (FK fallback when no rigClass given).",
    RPC_PARAMS(
        RPC_PARAM_OPT("sequence", "path", "Level sequence asset path (or 'path')"),
        RPC_PARAM_OPT("path", "path", "Alias for sequence"),
        RPC_PARAM_OPT("binding", "string", "Possessable binding GUID, Digits (or 'bindingGuid'/'bindingId')"),
        RPC_PARAM_OPT("bindingGuid", "string", "Alias for binding"),
        RPC_PARAM_OPT("bindingId", "string", "Alias for binding"),
        RPC_PARAM_OPT("rigClass", "classref", "Control Rig class/asset path; omit for FK Control Rig"),
        RPC_PARAM_OPT("layered", "bool", "Create the rig as a layered/additive rig")
    ))
{
    using namespace PinWrightControlRigSequencer;
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    FString SeqPath;
    ULevelSequence* Sequence = ResolveLevelSequence(Payload, SeqPath);
    if (!Sequence)
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_FOUND,
            FString::Printf(TEXT("Level sequence not found for path '%s'."), *SeqPath));
        return true;
    }
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_INVALID, TEXT("Level sequence has no MovieScene."));
        return true;
    }

    const FGuid BindingGuid = ResolveBindingGuid(Payload);
    if (!BindingGuid.IsValid() || !MovieScene->FindBinding(BindingGuid))
    {
        Ctx.SendError(ErrorCodes::ERR_BINDING_NOT_FOUND,
            TEXT("Binding GUID missing or not present in the sequence. Pass 'binding' as the possessable GUID (Digits)."));
        return true;
    }

    // Resolve the Control Rig class: explicit rigClass, else FK Control Rig.
    UClass* RigClass = nullptr;
    FString RigClassStr;
    if (Payload.IsValid() && Payload->TryGetStringField(TEXT("rigClass"), RigClassStr) && !RigClassStr.IsEmpty())
    {
        RigClass = ResolveUClass(RigClassStr);
        if (!RigClass || !RigClass->IsChildOf(UControlRig::StaticClass()))
        {
            Ctx.SendError(ErrorCodes::ERR_RIG_CLASS_NOT_FOUND,
                FString::Printf(TEXT("Could not resolve rigClass '%s' to a UControlRig subclass."), *RigClassStr));
            return true;
        }
    }
    else
    {
        RigClass = UFKControlRig::StaticClass();
    }

    bool bLayered = false;
    if (Payload.IsValid())
    {
        Payload->TryGetBoolField(TEXT("layered"), bLayered);
    }

#if UE_VERSION_OLDER_THAN(5, 4, 0)
    // UControlRig::SetIsAdditive() (layered rigs for arbitrary Control Rig classes) arrived
    // in 5.4. On 5.3 only the FK rig can run additively (its apply mode), so a layered
    // request for any other rig class is rejected rather than silently produced as an
    // absolute rig.
    if (bLayered && RigClass != UFKControlRig::StaticClass())
    {
        Ctx.SendUnsupportedEngineVersion(TEXT("5.4"),
            TEXT("layered/additive Control Rig tracks for a non-FK rig class"));
        return true;
    }
#endif

    // Idempotency: reuse an existing CR track of the same class on this binding.
    if (UMovieSceneControlRigParameterTrack* Existing = FindControlRigTrack(MovieScene, BindingGuid))
    {
        if (Existing->GetControlRig() && Existing->GetControlRig()->GetClass() == RigClass)
        {
            const int32 ExistingCount = Existing->GetControlRig()->GetHierarchy()
                ? Existing->GetControlRig()->GetHierarchy()->GetControls().Num() : 0;
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("sequence"), SeqPath);
            Result->SetStringField(TEXT("binding"), BindingGuid.ToString(EGuidFormats::Digits));
            Result->SetStringField(TEXT("rigClass"), RigClass->GetName());
            Result->SetNumberField(TEXT("controlCount"), ExistingCount);
            Result->SetBoolField(TEXT("created"), false);
            Ctx.SendSuccess(Result);
            return true;
        }
    }

    // FK fallback needs a skeletal mesh to generate controls from.
    USkeletalMeshComponent* SkelMeshComp = ResolveBoundSkeletalMeshComponent(Sequence, BindingGuid);
    if (!SkelMeshComp && RigClass == UFKControlRig::StaticClass())
    {
        Ctx.SendError(ErrorCodes::ERR_BINDING_NOT_SKELETAL,
            TEXT("FK Control Rig requires the binding to resolve to a skeletal-mesh actor in the editor world; "
                 "none was found. Bind the possessable to a spawned skeletal-mesh actor, or pass an explicit rigClass."));
        return true;
    }

    const FScopedTransaction Transaction(NSLOCTEXT("PinWright", "AddControlRigTrack", "Add Control Rig Track"));
    Sequence->Modify();
    MovieScene->Modify();

    UMovieSceneControlRigParameterTrack* Track =
        Cast<UMovieSceneControlRigParameterTrack>(MovieScene->AddTrack(UMovieSceneControlRigParameterTrack::StaticClass(), BindingGuid));
    if (!Track)
    {
        Ctx.SendError(ErrorCodes::ERR_TRACK_CREATE_FAILED, TEXT("Failed to add a Control Rig parameter track to the binding."));
        return true;
    }

    FString ObjectName = RigClass->GetName();
    ObjectName.RemoveFromEnd(TEXT("_C"));

    UControlRig* ControlRig = NewObject<UControlRig>(Track, RigClass, FName(*ObjectName), RF_Transactional);
    ControlRig->Modify();
    if (UFKControlRig* FKRig = Cast<UFKControlRig>(ControlRig))
    {
        if (bLayered)
        {
            FKRig->SetApplyMode(EControlRigFKRigExecuteMode::Additive);
        }
    }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    else
    {
        // SetIsAdditive is 5.4+; on 5.3 a non-FK layered rig was already rejected above.
        ControlRig->SetIsAdditive(bLayered);
    }
#endif

    // Bind the skeletal mesh BEFORE Initialize so FK control generation
    // (UFKControlRig::CreateRigElements reads GetObjectBinding()->GetBoundObject()'s
    // reference skeleton) has a source.
    ControlRig->SetObjectBinding(MakeShared<FControlRigObjectBinding>());
    if (SkelMeshComp)
    {
        ControlRig->GetObjectBinding()->BindToObject(SkelMeshComp);
        // Register the owner component as a data source BEFORE Initialize so a custom
        // rigClass whose construction graph imports its hierarchy from the skeleton
        // (e.g. FRigUnit_HierarchyImportFromSkeleton) can resolve the mesh; without it
        // such a rig builds an empty hierarchy (controlCount:0). Mirrors the engine
        // track editor's AddControlRig (ControlRigParameterTrackEditor.cpp).
        ControlRig->GetDataSourceRegistry()->RegisterDataSource(UControlRig::OwnerComponent, SkelMeshComp);
    }
    ControlRig->Initialize();
    ControlRig->Evaluate_AnyThread();

    Track->Modify();
    UMovieSceneSection* NewSection = Track->CreateControlRigSection(0, ControlRig, /*bOwnsControlRig*/ true);
    if (NewSection)
    {
        // CreateControlRigSection deliberately gives the section an infinite range
        // (TRange::All()) so controls key/evaluate at ANY frame. Do NOT clamp it to the
        // playback range: keys placed beyond the (default 5s) playback range would fall
        // outside the section and be silently dropped by real Sequencer evaluation.
        NewSection->Modify();
    }
    Track->SetTrackName(FName(*ObjectName));
    Track->SetDisplayName(FText::FromString(ObjectName));

    const int32 ControlCount = ControlRig->GetHierarchy() ? ControlRig->GetHierarchy()->GetControls().Num() : 0;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("sequence"), SeqPath);
    Result->SetStringField(TEXT("binding"), BindingGuid.ToString(EGuidFormats::Digits));
    Result->SetStringField(TEXT("rigClass"), RigClass->GetName());
    Result->SetNumberField(TEXT("controlCount"), ControlCount);
    Result->SetBoolField(TEXT("created"), true);
    Ctx.SendSuccess(Result);
    return true;
}

// sequencer.list_controls — list the Control Rig controls on a binding's CR track.
REGISTER_RPC_HANDLER("sequencer.list_controls", "sequencer",
    "List the Control Rig controls (name, type) on a binding's Control Rig track.",
    RPC_PARAMS(
        RPC_PARAM_OPT("sequence", "path", "Level sequence asset path (or 'path')"),
        RPC_PARAM_OPT("path", "path", "Alias for sequence"),
        RPC_PARAM_OPT("binding", "string", "Possessable binding GUID, Digits (or 'bindingGuid'/'bindingId')"),
        RPC_PARAM_OPT("bindingGuid", "string", "Alias for binding"),
        RPC_PARAM_OPT("bindingId", "string", "Alias for binding")
    ))
{
    using namespace PinWrightControlRigSequencer;
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    FString SeqPath;
    UMovieScene* MovieScene = nullptr;
    FGuid BindingGuid;
    UMovieSceneControlRigParameterTrack* Track = nullptr;
    UMovieSceneControlRigParameterSection* Section = nullptr;
    if (!ResolveControlRigSection(Ctx, Payload, /*bPreferSectionToKey*/ false,
            SeqPath, MovieScene, BindingGuid, Track, Section))
    {
        return true;
    }
    if (!Track->GetControlRig())
    {
        Ctx.SendError(ErrorCodes::ERR_CONTROLRIG_TRACK_NOT_FOUND,
            TEXT("No Control Rig track/section on this binding. Call sequencer.add_controlrig_track first."));
        return true;
    }

    UControlRig* ControlRig = Track->GetControlRig();
    // Enumerate controls from the section's float channels so every listed control is
    // guaranteed keyable via sequencer.key_controls; preserve section channel order.
    TArray<TSharedPtr<FJsonValue>> Controls;
    for (const FName& CName : ControlsWithFloatChannels(Section))
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), CName.ToString());
        if (FRigControlElement* Element = ControlRig->FindControl(CName))
        {
            Entry->SetStringField(TEXT("type"), ControlTypeToString(Element->Settings.ControlType));
        }
        Controls.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("sequence"), SeqPath);
    Result->SetStringField(TEXT("binding"), BindingGuid.ToString(EGuidFormats::Digits));
    Result->SetStringField(TEXT("rigClass"), ControlRig->GetClass()->GetName());
    Result->SetArrayField(TEXT("controls"), Controls);
    Result->SetNumberField(TEXT("count"), Controls.Num());
    Ctx.SendSuccess(Result);
    return true;
}

// sequencer.key_controls — key one or more controls at a frame. Each control's
// value is a number (written to the control's primary float channel) or an array of
// numbers (written to the control's channels in index order: transform =
// [TX,TY,TZ,RX,RY,RZ,SX,SY,SZ], vector = [X,Y,Z], float/bool = [v]). A scalar keys
// only the primary channel and leaves the rest untouched; pass an array to key a
// full transform/vector control. Idempotent per (control, channel, frame): updates
// the existing key rather than appending.
REGISTER_RPC_HANDLER("sequencer.key_controls", "sequencer",
    "Key Control Rig controls at a frame: controls maps name -> a number (primary channel) or an array of numbers (all channels in index order).",
    RPC_PARAMS(
        RPC_PARAM_OPT("sequence", "path", "Level sequence asset path (or 'path')"),
        RPC_PARAM_OPT("path", "path", "Alias for sequence"),
        RPC_PARAM_OPT("binding", "string", "Possessable binding GUID, Digits (or 'bindingGuid'/'bindingId')"),
        RPC_PARAM_OPT("bindingGuid", "string", "Alias for binding"),
        RPC_PARAM_OPT("bindingId", "string", "Alias for binding"),
        RPC_PARAM_REQ("frame", "integer", "Display-rate frame to key at"),
        RPC_PARAM_REQ("controls", "object", "Map of control name -> a number (primary channel) or an array of numbers (channels in index order: transform=[TX,TY,TZ,RX,RY,RZ,SX,SY,SZ], vector=[X,Y,Z])")
    ))
{
    using namespace PinWrightControlRigSequencer;
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    FString SeqPath;
    UMovieScene* MovieScene = nullptr;
    FGuid BindingGuid;
    UMovieSceneControlRigParameterTrack* Track = nullptr;
    UMovieSceneControlRigParameterSection* Section = nullptr;
    if (!ResolveControlRigSection(Ctx, Payload, /*bPreferSectionToKey*/ true,
            SeqPath, MovieScene, BindingGuid, Track, Section))
    {
        return true;
    }

    double FrameNum = 0.0;
    if (!Payload.IsValid() || !Payload->TryGetNumberField(TEXT("frame"), FrameNum))
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_FRAME, TEXT("A numeric 'frame' is required."));
        return true;
    }
    const TSharedPtr<FJsonObject>* ControlsObj = nullptr;
    if (!Payload->TryGetObjectField(TEXT("controls"), ControlsObj) || !ControlsObj || !(*ControlsObj).IsValid())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_CONTROLS, TEXT("A 'controls' object mapping control name -> value is required."));
        return true;
    }

    const FFrameNumber TickFrame = DisplayFrameToTick(MovieScene, FrameNum);

    const FScopedTransaction Transaction(NSLOCTEXT("PinWright", "KeyControlRigControls", "Key Control Rig Controls"));
    Section->Modify();

    TArray<TSharedPtr<FJsonValue>> Keyed;
    TArray<FString> Missing;      // named control has no keyable float channel on this rig
    TArray<FString> Rejected;     // value was neither a JSON number nor an array of numbers
    for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : (*ControlsObj)->Values)
    {
        // Parse the value as either a scalar (targets the control's primary channel) or an
        // array of numbers (targets the control's channels in index order). A JSON bool
        // converts to 0/1 via TryGetNumber and is accepted; objects/null and non-numeric
        // strings (and arrays with any non-numeric element) are recorded in Rejected rather
        // than silently dropped so the caller sees which values were refused.
        TArray<double> Values;
        if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Array)
        {
            bool bAllNumeric = true;
            for (const TSharedPtr<FJsonValue>& Elem : Pair.Value->AsArray())
            {
                double Component = 0.0;
                if (!Elem.IsValid() || !Elem->TryGetNumber(Component))
                {
                    bAllNumeric = false;
                    break;
                }
                Values.Add(Component);
            }
            if (!bAllNumeric || Values.Num() == 0)
            {
                Rejected.Add(Pair.Key);
                continue;
            }
        }
        else
        {
            double Scalar = 0.0;
            if (!Pair.Value.IsValid() || !Pair.Value->TryGetNumber(Scalar))
            {
                Rejected.Add(Pair.Key);
                continue;
            }
            Values.Add(Scalar);
        }

        TArray<FMovieSceneFloatChannel*> Channels = ChannelsForControl(Section, FName(*Pair.Key));
        if (Channels.Num() == 0)
        {
            Missing.Add(Pair.Key);
            continue;
        }
        // Map value[i] -> channel[i]. A scalar writes only channel 0; an array writes as
        // many channels as it supplies (capped at the control's channel count) so a full
        // transform/vector control can be keyed in one call.
        const int32 NumToWrite = FMath::Min(Values.Num(), Channels.Num());
        for (int32 ChannelIdx = 0; ChannelIdx < NumToWrite; ++ChannelIdx)
        {
            FMovieSceneFloatValue KeyValue(static_cast<float>(Values[ChannelIdx]));
            KeyValue.InterpMode = ERichCurveInterpMode::RCIM_Cubic;
            Channels[ChannelIdx]->GetData().UpdateOrAddKey(TickFrame, KeyValue);
        }
        Keyed.Add(MakeShared<FJsonValueString>(Pair.Key));
    }

    if (Keyed.Num() == 0)
    {
        TSharedPtr<FJsonObject> ErrResult = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> MissingArr;
        for (const FString& M : Missing)
        {
            MissingArr.Add(MakeShared<FJsonValueString>(M));
        }
        ErrResult->SetArrayField(TEXT("unknownControls"), MissingArr);
        TArray<TSharedPtr<FJsonValue>> RejectedArr;
        for (const FString& R : Rejected)
        {
            RejectedArr.Add(MakeShared<FJsonValueString>(R));
        }
        ErrResult->SetArrayField(TEXT("rejectedValues"), RejectedArr);
        Ctx.SendError(ErrorCodes::ERR_NO_CONTROLS_KEYED,
            TEXT("No controls were keyed: the named controls were either absent from this rig "
                 "(unknownControls) or supplied a non-numeric value (rejectedValues)."), ErrResult);
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("sequence"), SeqPath);
    Result->SetStringField(TEXT("binding"), BindingGuid.ToString(EGuidFormats::Digits));
    Result->SetNumberField(TEXT("frame"), FrameNum);
    Result->SetArrayField(TEXT("keyed"), Keyed);
    if (Missing.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> MissingArr;
        for (const FString& M : Missing)
        {
            MissingArr.Add(MakeShared<FJsonValueString>(M));
        }
        Result->SetArrayField(TEXT("unknownControls"), MissingArr);
    }
    if (Rejected.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> RejectedArr;
        for (const FString& R : Rejected)
        {
            RejectedArr.Add(MakeShared<FJsonValueString>(R));
        }
        Result->SetArrayField(TEXT("rejectedValues"), RejectedArr);
    }
    Ctx.SendSuccess(Result);
    return true;
}

// sequencer.get_control_value — read a control's keyed float-channel value(s) at a
// frame. Returns the primary channel as `value` and every channel of the control
// (transform = 9, vector = 3, float/bool = 1) as `values`, in index order, plus
// `channelCount` and `additive`. This is the channel CURVE value the section stores,
// NOT a rig-composed pose: when `additive` is true (a layered rig) the values are the
// additive DELTA the layer contributes, not the base+delta result, and headless
// evaluation cannot fold in spaces/constraints/driven controls. For the plain
// non-additive path the channel value is the control value.
REGISTER_RPC_HANDLER("sequencer.get_control_value", "sequencer",
    "Read a Control Rig control's keyed channel value(s) at a display-rate frame (value=primary channel, values=all channels; additive=true means the values are layer deltas).",
    RPC_PARAMS(
        RPC_PARAM_OPT("sequence", "path", "Level sequence asset path (or 'path')"),
        RPC_PARAM_OPT("path", "path", "Alias for sequence"),
        RPC_PARAM_OPT("binding", "string", "Possessable binding GUID, Digits (or 'bindingGuid'/'bindingId')"),
        RPC_PARAM_OPT("bindingGuid", "string", "Alias for binding"),
        RPC_PARAM_OPT("bindingId", "string", "Alias for binding"),
        RPC_PARAM_REQ("control", "string", "Control name"),
        RPC_PARAM_REQ("frame", "integer", "Display-rate frame to evaluate at")
    ))
{
    using namespace PinWrightControlRigSequencer;
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    FString SeqPath;
    UMovieScene* MovieScene = nullptr;
    FGuid BindingGuid;
    UMovieSceneControlRigParameterTrack* Track = nullptr;
    UMovieSceneControlRigParameterSection* Section = nullptr;
    if (!ResolveControlRigSection(Ctx, Payload, /*bPreferSectionToKey*/ false,
            SeqPath, MovieScene, BindingGuid, Track, Section))
    {
        return true;
    }

    FString ControlName;
    if (!Payload.IsValid() || !Payload->TryGetStringField(TEXT("control"), ControlName) || ControlName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_CONTROL, TEXT("A 'control' name is required."));
        return true;
    }
    double FrameNum = 0.0;
    if (!Payload->TryGetNumberField(TEXT("frame"), FrameNum))
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_FRAME, TEXT("A numeric 'frame' is required."));
        return true;
    }

    TArray<FMovieSceneFloatChannel*> Channels = ChannelsForControl(Section, FName(*ControlName));
    if (Channels.Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_CONTROL_NOT_FOUND,
            FString::Printf(TEXT("Control '%s' has no keyable float channel on this rig."), *ControlName));
        return true;
    }

    const FFrameNumber TickFrame = DisplayFrameToTick(MovieScene, FrameNum);
    // Evaluate every float channel of the control (not just the primary) so a caller can
    // read back a full transform/vector control, mirroring key_controls' multi-channel write.
    TArray<TSharedPtr<FJsonValue>> ValuesArr;
    float PrimaryValue = 0.0f;
    for (int32 ChannelIdx = 0; ChannelIdx < Channels.Num(); ++ChannelIdx)
    {
        float ChannelValue = 0.0f;
        Channels[ChannelIdx]->Evaluate(FFrameTime(TickFrame), ChannelValue);
        if (ChannelIdx == 0)
        {
            PrimaryValue = ChannelValue;
        }
        ValuesArr.Add(MakeShared<FJsonValueNumber>(ChannelValue));
    }

    // Surface whether the rig is layered/additive so the caller knows the returned channel
    // values are additive deltas rather than a composed pose (honest readback contract).
    const bool bAdditive = IsRigAdditive(Track->GetControlRig());

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("sequence"), SeqPath);
    Result->SetStringField(TEXT("binding"), BindingGuid.ToString(EGuidFormats::Digits));
    Result->SetStringField(TEXT("control"), ControlName);
    Result->SetNumberField(TEXT("frame"), FrameNum);
    Result->SetNumberField(TEXT("value"), PrimaryValue);
    Result->SetArrayField(TEXT("values"), ValuesArr);
    Result->SetNumberField(TEXT("channelCount"), Channels.Num());
    Result->SetBoolField(TEXT("additive"), bAdditive);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// Batch keying, batch readback and world space (F-sequencer-control-keys-batch), and the contact
// pin built on them (F-sequencer-pin-controls).
//
// WORLD SPACE IS COMPUTED HEADLESS, NOT THROUGH THE ENGINE'S OPEN-SEQUENCER API.
// UControlRigSequencerEditorLibrary::Get/SetControlRigWorldTransforms evaluate through an open,
// focused Sequencer (GetSequencerFromAsset) and do nothing without one. Here the rig is posed from
// the section's own channels at the frame (every control with float channels), the rig's forward
// solve runs when the control hangs off a bone (an FK rig places bones from its controls), and
// world = control global (component space) * the bound component's world transform. The rig pose
// is restored afterwards. Where that model is wrong the verbs refuse with
// CONTROL_WORLD_SPACE_UNAVAILABLE instead of answering: an additive rig (channels are deltas), and
// a binding moved by a transform or attach track (the component's editor placement is then not
// its per-frame placement).
// ponytail: posing skips Vector2D/bool/integer/enum controls and ignores a control's preferred
// euler rotation order; add both when a rig's transforms are seen to depend on them.
//
// EVERY WRITE IS READ BACK, AND A MISMATCH UNDOES THE WHOLE CALL. The touched channels are
// snapshotted before the first key, the call runs in one FScopedTransaction, and every written key
// is read back (local: the channel value; world: re-posed through the rig). Any key outside
// tolerance restores every snapshotted channel, cancels the transaction, verifies the restore and
// only then restores the package's dirty flag.
// ============================================================================
namespace PinWrightControlRigSequencer
{
    // Which transform parts a control type owns, in the section's channel order T, R, S.
    struct FControlTransformParts
    {
        bool bT = false;
        bool bR = false;
        bool bS = false;
        bool Any() const { return bT || bR || bS; }
        int32 NumChannels() const { return 3 * ((bT ? 1 : 0) + (bR ? 1 : 0) + (bS ? 1 : 0)); }
    };

    static FControlTransformParts TransformPartsForType(ERigControlType Type)
    {
        FControlTransformParts Parts;
        switch (Type)
        {
        case ERigControlType::Position:         Parts.bT = true; break;
        case ERigControlType::Rotator:          Parts.bR = true; break;
        case ERigControlType::Scale:            Parts.bS = true; break;
        case ERigControlType::TransformNoScale: Parts.bT = true; Parts.bR = true; break;
        case ERigControlType::Transform:
        case ERigControlType::EulerTransform:   Parts.bT = true; Parts.bR = true; Parts.bS = true; break;
        default: break;
        }
        return Parts;
    }

    static FControlTransformParts WireTransformParts()
    {
        FControlTransformParts Parts;
        Parts.bT = true;
        Parts.bR = true;
        Parts.bS = true;
        return Parts;
    }

    // Float-channel values (index order) -> transform. Rotation channels are [Roll, Pitch, Yaw],
    // the section's own layout; parts the type does not own stay identity.
    static FTransform ChannelValuesToTransform(const FControlTransformParts& Parts, TArrayView<const double> V)
    {
        FVector T = FVector::ZeroVector;
        FRotator R = FRotator::ZeroRotator;
        FVector S = FVector::OneVector;
        int32 I = 0;
        if (Parts.bT) { T = FVector(V[I], V[I + 1], V[I + 2]); I += 3; }
        if (Parts.bR) { R = FRotator(V[I + 1], V[I + 2], V[I]); I += 3; }
        if (Parts.bS) { S = FVector(V[I], V[I + 1], V[I + 2]); }
        return FTransform(R, T, S);
    }

    static TArray<double> TransformToChannelValues(const FControlTransformParts& Parts, const FTransform& X)
    {
        TArray<double> V;
        if (Parts.bT) { const FVector T = X.GetLocation(); V.Append({T.X, T.Y, T.Z}); }
        if (Parts.bR) { const FRotator R = X.Rotator(); V.Append({R.Roll, R.Pitch, R.Yaw}); }
        if (Parts.bS) { const FVector S = X.GetScale3D(); V.Append({S.X, S.Y, S.Z}); }
        return V;
    }

    static TArray<TSharedPtr<FJsonValue>> NumbersToJson(TArrayView<const double> V)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const double D : V)
        {
            Out.Add(MakeShared<FJsonValueNumber>(D));
        }
        return Out;
    }

    // Strict: every element must be a JSON number (TryGetNumber would also take bools and strings).
    static bool ParseNumberArray(const TSharedPtr<FJsonValue>& Value, TArray<double>& Out)
    {
        Out.Reset();
        if (!Value.IsValid() || Value->Type != EJson::Array)
        {
            return false;
        }
        for (const TSharedPtr<FJsonValue>& Element : Value->AsArray())
        {
            if (!Element.IsValid() || Element->Type != EJson::Number)
            {
                return false;
            }
            Out.Add(Element->AsNumber());
        }
        return true;
    }

    // A non-empty array of whole display frames.
    static bool ParseFrameArray(const TSharedPtr<FJsonValue>& Value, TArray<int32>& Out)
    {
        TArray<double> Numbers;
        Out.Reset();
        if (!ParseNumberArray(Value, Numbers) || Numbers.Num() == 0)
        {
            return false;
        }
        for (const double Frame : Numbers)
        {
            if (Frame != FMath::RoundToDouble(Frame) || FMath::Abs(Frame) > 1.0e8)
            {
                return false;
            }
            Out.Add(static_cast<int32>(Frame));
        }
        return true;
    }

    // [TX,TY,TZ,Roll,Pitch,Yaw] or [...,SX,SY,SZ] -> transform (scale 1 when omitted).
    static bool ParseWireTransform(const TSharedPtr<FJsonValue>& Value, FTransform& Out)
    {
        TArray<double> V;
        if (!ParseNumberArray(Value, V) || (V.Num() != 6 && V.Num() != 9))
        {
            return false;
        }
        if (V.Num() == 6)
        {
            V.Append({1.0, 1.0, 1.0});
        }
        Out = ChannelValuesToTransform(WireTransformParts(), V);
        return true;
    }

    static void WriteChannelKey(FMovieSceneFloatChannel* Channel, FFrameNumber Tick, double Value)
    {
        FMovieSceneFloatValue KeyValue(static_cast<float>(Value));
        KeyValue.InterpMode = ERichCurveInterpMode::RCIM_Cubic;
        Channel->GetData().UpdateOrAddKey(Tick, KeyValue);
    }

    struct FChannelSnapshot
    {
        FMovieSceneFloatChannel* Channel = nullptr;
        TArray<FFrameNumber> Times;
        TArray<FMovieSceneFloatValue> Values;
    };

    static FChannelSnapshot CaptureChannel(FMovieSceneFloatChannel* Channel)
    {
        FChannelSnapshot Snap;
        Snap.Channel = Channel;
        const auto Data = static_cast<const FMovieSceneFloatChannel*>(Channel)->GetData();
        Snap.Times.Append(Data.GetTimes().GetData(), Data.GetTimes().Num());
        Snap.Values.Append(Data.GetValues().GetData(), Data.GetValues().Num());
        return Snap;
    }

    static void SnapshotChannels(TArrayView<FMovieSceneFloatChannel* const> Channels, TArray<FChannelSnapshot>& InOut)
    {
        for (FMovieSceneFloatChannel* Channel : Channels)
        {
            if (Channel && !InOut.ContainsByPredicate([Channel](const FChannelSnapshot& S) { return S.Channel == Channel; }))
            {
                InOut.Add(CaptureChannel(Channel));
            }
        }
    }

    // Restores every snapshotted channel, drops the open transaction without applying it, verifies
    // the restore key for key, and only then puts the package's dirty flag back.
    static bool RollbackChannels(FScopedTransaction& Transaction, UPackage* Package, bool bPackageWasDirty,
        const TArray<FChannelSnapshot>& Snapshots)
    {
        for (const FChannelSnapshot& Snap : Snapshots)
        {
            Snap.Channel->Set(Snap.Times, Snap.Values);
        }
        Transaction.Cancel();
        for (const FChannelSnapshot& Snap : Snapshots)
        {
            const FChannelSnapshot Now = CaptureChannel(Snap.Channel);
            if (Now.Times != Snap.Times || Now.Values != Snap.Values)
            {
                return false;
            }
        }
        if (Package)
        {
            Package->SetDirtyFlag(bPackageWasDirty);
        }
        return true;
    }

    // What world-space evaluation needs, resolved once per call.
    struct FWorldSpaceContext
    {
        UControlRig* Rig = nullptr;
        FTransform ComponentWorld = FTransform::Identity;
    };

    // Refuses (and returns false) where headless world space would answer wrongly; see the
    // block comment above.
    static bool ResolveWorldSpaceContext(FHandlerContext& Ctx, UMovieScene* MovieScene, const FGuid& BindingGuid,
        UMovieSceneControlRigParameterSection* Section, FWorldSpaceContext& Out)
    {
        UControlRig* Rig = Section ? Section->GetControlRig() : nullptr;
        if (!Rig || !Rig->GetHierarchy())
        {
            Ctx.SendError(ErrorCodes::ERR_RIG_STATE_INVALID,
                TEXT("The Control Rig section has no current rig or hierarchy."));
            return false;
        }
        if (IsRigAdditive(Rig))
        {
            Ctx.SendError(ErrorCodes::ERR_CONTROL_WORLD_SPACE_UNAVAILABLE,
                TEXT("The rig is additive (layered): its channels are deltas over the base pose, so a world "
                     "transform cannot be keyed or read from them. Use space 'local'."));
            return false;
        }
        // The binding and every parent binding: a transform or attach track moves the component per
        // frame, which this headless evaluation does not replay.
        FGuid Guid = BindingGuid;
        for (int32 Depth = 0; Guid.IsValid() && Depth < 16; ++Depth)
        {
            if (MovieScene->FindTracks(UMovieScene3DTransformTrack::StaticClass(), Guid, NAME_None).Num() > 0 ||
                MovieScene->FindTracks(UMovieScene3DAttachTrack::StaticClass(), Guid, NAME_None).Num() > 0)
            {
                Ctx.SendError(ErrorCodes::ERR_CONTROL_WORLD_SPACE_UNAVAILABLE,
                    FString::Printf(TEXT("Binding '%s' has a transform or attach track, so the rig's component moves per "
                                         "frame and its editor placement is not the world parent of the controls. "
                                         "Use space 'local'."), *Guid.ToString(EGuidFormats::Digits)));
                return false;
            }
            const FMovieScenePossessable* Possessable = MovieScene->FindPossessable(Guid);
            Guid = Possessable ? Possessable->GetParent() : FGuid();
        }
        USceneComponent* Component = nullptr;
        if (const TSharedPtr<IControlRigObjectBinding> Binding = Rig->GetObjectBinding())
        {
            Component = Cast<USceneComponent>(Binding->GetBoundObject());
        }
        if (!Component)
        {
            Component = ResolveBoundSkeletalMeshComponent(MovieScene->GetTypedOuter<ULevelSequence>(), BindingGuid);
        }
        if (!Component)
        {
            Ctx.SendError(ErrorCodes::ERR_CONTROL_WORLD_SPACE_UNAVAILABLE,
                TEXT("The rig is not bound to a scene component in the editor world, so there is no world parent for "
                     "its controls. Bind the possessable to a live actor, or use space 'local'."));
            return false;
        }
        Out.Rig = Rig;
        Out.ComponentWorld = Component->GetComponentTransform();
        return true;
    }

    // An FK rig parents each control to its bone's parent bone, and bones are placed by the rig's
    // forward solve, so such a control needs the rig run after posing.
    static bool ControlHangsOffBone(URigHierarchy* Hierarchy, const FRigElementKey& Key)
    {
        for (const FRigElementKey& Parent : Hierarchy->GetParents(Key, /*bRecursive=*/true))
        {
            if (Parent.Type == ERigElementType::Bone)
            {
                return true;
            }
        }
        return false;
    }

    // Write every float-channel control's value at Tick into the rig (a control whose channels have
    // nothing to evaluate keeps its current value).
    static void PoseRigFromSection(UMovieSceneControlRigParameterSection* Section, UControlRig* Rig,
        FFrameNumber Tick, bool bRunForwardSolve)
    {
        URigHierarchy* Hierarchy = Rig->GetHierarchy();
        for (const FName& Name : ControlsWithFloatChannels(Section))
        {
            FRigControlElement* Element = Rig->FindControl(Name);
            if (!Element)
            {
                continue;
            }
            TArray<double> V;
            bool bEvaluated = true;
            for (FMovieSceneFloatChannel* Channel : ChannelsForControl(Section, Name))
            {
                float F = 0.0f;
                bEvaluated &= Channel->Evaluate(FFrameTime(Tick), F);
                V.Add(F);
            }
            const ERigControlType Type = Element->Settings.ControlType;
            const FControlTransformParts Parts = TransformPartsForType(Type);
            FRigControlValue Value = Hierarchy->GetControlValue(Element, ERigControlValueType::Current);
            if (!bEvaluated)
            {
                continue;
            }
            if (Parts.Any() && V.Num() >= Parts.NumChannels())
            {
                Value.SetFromTransform(ChannelValuesToTransform(Parts, V), Type, Element->Settings.PrimaryAxis);
            }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
            else if ((Type == ERigControlType::Float || Type == ERigControlType::ScaleFloat) && V.Num() >= 1)
#else
            else if (Type == ERigControlType::Float && V.Num() >= 1)
#endif
            {
                Value = FRigControlValue::Make<float>(static_cast<float>(V[0]));
            }
            else
            {
                continue;
            }
            Hierarchy->SetControlValue(Element, Value, ERigControlValueType::Current);
        }
        if (bRunForwardSolve)
        {
            Rig->Evaluate_AnyThread();
        }
    }

    static FTransform ControlWorldAt(UMovieSceneControlRigParameterSection* Section, const FWorldSpaceContext& World,
        FName Control, FFrameNumber Tick)
    {
        PoseRigFromSection(Section, World.Rig, Tick,
            ControlHangsOffBone(World.Rig->GetHierarchy(), FRigElementKey(Control, ERigElementType::Control)));
        return World.Rig->GetControlGlobalTransform(Control) * World.ComponentWorld;
    }

    // The control's channel values that place it at WorldTarget at Tick, given every other
    // channel as currently keyed.
    static TArray<double> ChannelValuesForWorld(UMovieSceneControlRigParameterSection* Section,
        const FWorldSpaceContext& World, FName Control, FFrameNumber Tick, const FTransform& WorldTarget)
    {
        PoseRigFromSection(Section, World.Rig, Tick,
            ControlHangsOffBone(World.Rig->GetHierarchy(), FRigElementKey(Control, ERigElementType::Control)));
        const FRigControlElement* Element = World.Rig->FindControl(Control);
        const ERigControlType Type = Element->Settings.ControlType;
        const FRigControlValue Value = World.Rig->GetControlValueFromGlobalTransform(
            Control, WorldTarget.GetRelativeTransform(World.ComponentWorld), ERigTransformType::CurrentGlobal);
        return TransformToChannelValues(TransformPartsForType(Type),
            Value.GetAsTransform(Type, Element->Settings.PrimaryAxis));
    }

    struct FWorldKey
    {
        FName Control;
        int32 Frame = 0;
        FFrameNumber Tick;
        FTransform World;
    };

    struct FWorldKeyError
    {
        double Cm = 0.0;
        double Deg = 0.0;
        double Scale = 0.0;
    };

    // Writes world keys in order, each solved against everything keyed before it (so list a
    // parent control before its children), then reads every key back through the rig. The rig
    // pose is restored after each phase.
    static TArray<FWorldKeyError> WriteAndMeasureWorldKeys(UMovieSceneControlRigParameterSection* Section,
        const FWorldSpaceContext& World, const TArray<FWorldKey>& Keys)
    {
        URigHierarchy* Hierarchy = World.Rig->GetHierarchy();
        const FRigPose SavedPose = Hierarchy->GetPose();
        for (const FWorldKey& Key : Keys)
        {
            const TArray<double> V = ChannelValuesForWorld(Section, World, Key.Control, Key.Tick, Key.World);
            const TArray<FMovieSceneFloatChannel*> Channels = ChannelsForControl(Section, Key.Control);
            for (int32 Index = 0; Index < FMath::Min(V.Num(), Channels.Num()); ++Index)
            {
                WriteChannelKey(Channels[Index], Key.Tick, V[Index]);
                Channels[Index]->AutoSetTangents();
            }
        }
        Hierarchy->SetPose(SavedPose);

#if WITH_DEV_AUTOMATION_TESTS
        if (PinWrightControlRigKeyTestHooks::PostWriteHook())
        {
            PinWrightControlRigKeyTestHooks::PostWriteHook()(Section);
        }
#endif

        TArray<FWorldKeyError> Errors;
        for (const FWorldKey& Key : Keys)
        {
            const FTransform Got = ControlWorldAt(Section, World, Key.Control, Key.Tick);
            const FControlTransformParts Parts =
                TransformPartsForType(World.Rig->FindControl(Key.Control)->Settings.ControlType);
            FWorldKeyError& Error = Errors.AddDefaulted_GetRef();
            if (Parts.bT)
            {
                Error.Cm = FVector::Distance(Got.GetLocation(), Key.World.GetLocation());
            }
            if (Parts.bR)
            {
                Error.Deg = FMath::RadiansToDegrees(Got.GetRotation().AngularDistance(Key.World.GetRotation()));
            }
            if (Parts.bS)
            {
                Error.Scale = (Got.GetScale3D() - Key.World.GetScale3D()).GetAbsMax();
            }
        }
        Hierarchy->SetPose(SavedPose);
        return Errors;
    }

    static constexpr double WorldScaleTolerance = 1.0e-3;

    static bool ReadTolerance(FHandlerContext& Ctx, const TCHAR* Key, double& Out)
    {
        Out = Ctx.GetNumber(Key, 0.01);
        if (!(Out >= 0.0))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("'%s' must be a non-negative number."), Key));
            return false;
        }
        return true;
    }

    // A named control that exists on the rig and has float channels on this section.
    static bool ResolveKeyableControl(FHandlerContext& Ctx, UMovieSceneControlRigParameterSection* Section,
        const FString& Name, FRigControlElement*& OutElement, TArray<FMovieSceneFloatChannel*>& OutChannels)
    {
        UControlRig* Rig = Section->GetControlRig();
        OutElement = Rig ? Rig->FindControl(FName(*Name)) : nullptr;
        OutChannels = ChannelsForControl(Section, FName(*Name));
        if (!OutElement || OutChannels.Num() == 0)
        {
            Ctx.SendError(ErrorCodes::ERR_CONTROL_NOT_FOUND,
                FString::Printf(TEXT("Control '%s' has no keyable float channel on this rig; "
                                     "sequencer.list_controls lists the keyable ones."), *Name));
            return false;
        }
        return true;
    }

    static bool ParseSpace(FHandlerContext& Ctx, bool& bOutWorld)
    {
        const FString Space = Ctx.GetString(TEXT("space"));
        if (Space.Equals(TEXT("local"), ESearchCase::IgnoreCase))
        {
            bOutWorld = false;
            return true;
        }
        if (Space.Equals(TEXT("world"), ESearchCase::IgnoreCase))
        {
            bOutWorld = true;
            return true;
        }
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Unknown space '%s'; expected local or world."), *Space));
        return false;
    }
}

#define PINWRIGHT_CR_BATCH_TARGET_PARAMS \
    RPC_PARAM_REQ_ALIAS("sequence", "path", "Level sequence asset path", "path"), \
    ParamAliasUtils::MakeAliasParamSpec(TEXT("binding"), TEXT("string"), \
        TEXT("Binding GUID (Digits) that owns the Control Rig track"), /*bRequired=*/true, \
        TArray<FString>({TEXT("binding"), TEXT("bindingGuid"), TEXT("bindingId")}))

// sequencer.set_control_keys — many controls x many frames in one transaction, read back, and
// undone entirely on any mismatch.
REGISTER_RPC_HANDLER("sequencer.set_control_keys", "sequencer",
    "Key many Control Rig controls at many display frames in one undoable call, in local (channel values) or "
    "world space. Every key is read back; any mismatch undoes every key and fails CONTROL_KEY_READBACK_MISMATCH.",
    RPC_PARAMS(
        PINWRIGHT_CR_BATCH_TARGET_PARAMS,
        RPC_PARAM_REQ("space", "string", "local: values are the control's float-channel values in index order, as key_controls ([TX,TY,TZ,Roll,Pitch,Yaw,SX,SY,SZ] for a transform; a shorter array keys the leading channels). world: values are world transforms [TX,TY,TZ,Roll,Pitch,Yaw] or [TX,TY,TZ,Roll,Pitch,Yaw,SX,SY,SZ]"),
        RPC_PARAM_REQ_NESTED("keys", "array", "Entries {control, frames, values}: control name, display frames (whole numbers, no repeats per control), and one value array per frame. These keys are the whole entry schema and any other key inside an entry is refused with UNKNOWN_NESTED_PARAMS. Entries are written in order; in world space list a parent control before its children.",
            TEXT("control"), TEXT("frames"), TEXT("values")),
        RPC_PARAM_DEF("positionToleranceCm", "number", "World space only: readback position tolerance in cm", "0.01"),
        RPC_PARAM_DEF("rotationToleranceDeg", "number", "World space only: readback rotation tolerance in degrees", "0.01")
    ))
{
    using namespace PinWrightControlRigSequencer;
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    bool bWorld = false;
    if (!ParseSpace(Ctx, bWorld))
    {
        return true;
    }
    if (!bWorld && (Payload->HasField(TEXT("positionToleranceCm")) || Payload->HasField(TEXT("rotationToleranceDeg"))))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("positionToleranceCm / rotationToleranceDeg apply to space 'world' only; local keys are compared "
                 "to float precision."));
        return true;
    }
    double PositionTolerance = 0.0;
    double RotationTolerance = 0.0;
    if (!ReadTolerance(Ctx, TEXT("positionToleranceCm"), PositionTolerance) ||
        !ReadTolerance(Ctx, TEXT("rotationToleranceDeg"), RotationTolerance))
    {
        return true;
    }

    FString SeqPath;
    UMovieScene* MovieScene = nullptr;
    FGuid BindingGuid;
    UMovieSceneControlRigParameterTrack* Track = nullptr;
    UMovieSceneControlRigParameterSection* Section = nullptr;
    if (!ResolveControlRigSection(Ctx, Payload, /*bPreferSectionToKey*/ true,
            SeqPath, MovieScene, BindingGuid, Track, Section))
    {
        return true;
    }
    FWorldSpaceContext World;
    if (bWorld && !ResolveWorldSpaceContext(Ctx, MovieScene, BindingGuid, Section, World))
    {
        return true;
    }

    // Parse and validate everything before the first write.
    struct FEntry
    {
        FString Control;
        FControlTransformParts Parts;
        TArray<FMovieSceneFloatChannel*> Channels;
        TArray<int32> Frames;
        TArray<TArray<double>> Values;     // local
        TArray<FTransform> Transforms;     // world
    };
    TArray<FEntry> Entries;
    const TArray<TSharedPtr<FJsonValue>>* KeysArr = Ctx.GetArray(TEXT("keys"));
    if (!KeysArr || KeysArr->Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("'keys' must be a non-empty array of {control, frames, values}."));
        return true;
    }
    TSet<TPair<FString, int32>> SeenControlFrames;
    for (int32 EntryIndex = 0; EntryIndex < KeysArr->Num(); ++EntryIndex)
    {
        const TSharedPtr<FJsonObject>* EntryObj = nullptr;
        FEntry Entry;
        if (!(*KeysArr)[EntryIndex].IsValid() || !(*KeysArr)[EntryIndex]->TryGetObject(EntryObj) ||
            !(*EntryObj)->TryGetStringField(TEXT("control"), Entry.Control) || Entry.Control.IsEmpty() ||
            !ParseFrameArray((*EntryObj)->TryGetField(TEXT("frames")), Entry.Frames))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
                TEXT("keys[%d] needs a 'control' name and a non-empty 'frames' array of whole display frames."), EntryIndex));
            return true;
        }
        FRigControlElement* Element = nullptr;
        if (!ResolveKeyableControl(Ctx, Section, Entry.Control, Element, Entry.Channels))
        {
            return true;
        }
        Entry.Parts = TransformPartsForType(Element->Settings.ControlType);
        if (bWorld && !Entry.Parts.Any())
        {
            Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_TYPE, FString::Printf(
                TEXT("Control '%s' is a %s control with no transform; key it in space 'local'."),
                *Entry.Control, *ControlTypeToString(Element->Settings.ControlType)));
            return true;
        }
        const TArray<TSharedPtr<FJsonValue>>* ValuesArr = nullptr;
        if (!(*EntryObj)->TryGetArrayField(TEXT("values"), ValuesArr) || ValuesArr->Num() != Entry.Frames.Num())
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
                TEXT("keys[%d].values must hold exactly one value array per frame (%d)."), EntryIndex, Entry.Frames.Num()));
            return true;
        }
        for (int32 FrameIndex = 0; FrameIndex < Entry.Frames.Num(); ++FrameIndex)
        {
            bool bValid = false;
            if (bWorld)
            {
                FTransform Transform;
                bValid = ParseWireTransform((*ValuesArr)[FrameIndex], Transform);
                Entry.Transforms.Add(Transform);
            }
            else
            {
                TArray<double> V;
                bValid = ParseNumberArray((*ValuesArr)[FrameIndex], V) && V.Num() >= 1 && V.Num() <= Entry.Channels.Num();
                Entry.Values.Add(V);
            }
            if (!bValid)
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, bWorld
                    ? FString::Printf(TEXT("keys[%d].values[%d] must be [TX,TY,TZ,Roll,Pitch,Yaw] or 9 numbers with scale."), EntryIndex, FrameIndex)
                    : FString::Printf(TEXT("keys[%d].values[%d] must be an array of 1..%d numbers (the control's channels in index order)."),
                        EntryIndex, FrameIndex, Entry.Channels.Num()));
                return true;
            }
            bool bAlreadySeen = false;
            SeenControlFrames.Add(TPair<FString, int32>(Entry.Control, Entry.Frames[FrameIndex]), &bAlreadySeen);
            if (bAlreadySeen)
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
                    TEXT("Control '%s' is keyed twice at frame %d; one key per control per frame."),
                    *Entry.Control, Entry.Frames[FrameIndex]));
                return true;
            }
        }
        Entries.Add(MoveTemp(Entry));
    }

    TArray<FChannelSnapshot> Snapshots;
    for (const FEntry& Entry : Entries)
    {
        SnapshotChannels(Entry.Channels, Snapshots);
    }
    UPackage* Package = MovieScene->GetOutermost();
    const bool bPackageWasDirty = Package && Package->IsDirty();
    FScopedTransaction Transaction(NSLOCTEXT("PinWright", "SetControlRigKeys", "Set Control Rig Keys"));
    Section->Modify();

    // Write, then read back. Stats are per control, in request order.
    TArray<TSharedPtr<FJsonValue>> Mismatches;
    TArray<TSharedPtr<FJsonValue>> ControlsOut;
    int32 KeysWritten = 0;
    auto AddMismatch = [&Mismatches](const FString& Control, int32 Frame, const TSharedPtr<FJsonObject>& Detail)
    {
        Detail->SetStringField(TEXT("control"), Control);
        Detail->SetNumberField(TEXT("frame"), Frame);
        Mismatches.Add(MakeShared<FJsonValueObject>(Detail));
    };

    if (bWorld)
    {
        TArray<FWorldKey> WorldKeys;
        for (const FEntry& Entry : Entries)
        {
            for (int32 FrameIndex = 0; FrameIndex < Entry.Frames.Num(); ++FrameIndex)
            {
                FWorldKey& Key = WorldKeys.AddDefaulted_GetRef();
                Key.Control = FName(*Entry.Control);
                Key.Frame = Entry.Frames[FrameIndex];
                Key.Tick = DisplayFrameToTick(MovieScene, Key.Frame);
                Key.World = Entry.Transforms[FrameIndex];
            }
        }
        const TArray<FWorldKeyError> Errors = WriteAndMeasureWorldKeys(Section, World, WorldKeys);
        int32 KeyIndex = 0;
        for (const FEntry& Entry : Entries)
        {
            double MaxCm = 0.0, MaxDeg = 0.0, MaxScale = 0.0;
            for (int32 FrameIndex = 0; FrameIndex < Entry.Frames.Num(); ++FrameIndex, ++KeyIndex)
            {
                const FWorldKeyError& Error = Errors[KeyIndex];
                MaxCm = FMath::Max(MaxCm, Error.Cm);
                MaxDeg = FMath::Max(MaxDeg, Error.Deg);
                MaxScale = FMath::Max(MaxScale, Error.Scale);
                if (Error.Cm > PositionTolerance || Error.Deg > RotationTolerance || Error.Scale > WorldScaleTolerance)
                {
                    TSharedPtr<FJsonObject> Detail = MakeShared<FJsonObject>();
                    Detail->SetNumberField(TEXT("errorCm"), Error.Cm);
                    Detail->SetNumberField(TEXT("errorDeg"), Error.Deg);
                    Detail->SetNumberField(TEXT("scaleError"), Error.Scale);
                    AddMismatch(Entry.Control, Entry.Frames[FrameIndex], Detail);
                }
            }
            TSharedPtr<FJsonObject> Stats = MakeShared<FJsonObject>();
            Stats->SetStringField(TEXT("control"), Entry.Control);
            Stats->SetNumberField(TEXT("keysWritten"), Entry.Frames.Num());
            Stats->SetNumberField(TEXT("channelsWritten"), Entry.Parts.NumChannels());
            if (Entry.Parts.bT) { Stats->SetNumberField(TEXT("maxErrorCm"), MaxCm); }
            if (Entry.Parts.bR) { Stats->SetNumberField(TEXT("maxErrorDeg"), MaxDeg); }
            if (Entry.Parts.bS) { Stats->SetNumberField(TEXT("maxScaleError"), MaxScale); }
            ControlsOut.Add(MakeShared<FJsonValueObject>(Stats));
            KeysWritten += Entry.Frames.Num();
        }
    }
    else
    {
        for (const FEntry& Entry : Entries)
        {
            for (int32 FrameIndex = 0; FrameIndex < Entry.Frames.Num(); ++FrameIndex)
            {
                const FFrameNumber Tick = DisplayFrameToTick(MovieScene, Entry.Frames[FrameIndex]);
                for (int32 Channel = 0; Channel < Entry.Values[FrameIndex].Num(); ++Channel)
                {
                    WriteChannelKey(Entry.Channels[Channel], Tick, Entry.Values[FrameIndex][Channel]);
                }
            }
            for (FMovieSceneFloatChannel* Channel : Entry.Channels)
            {
                Channel->AutoSetTangents();
            }
        }
#if WITH_DEV_AUTOMATION_TESTS
        if (PinWrightControlRigKeyTestHooks::PostWriteHook())
        {
            PinWrightControlRigKeyTestHooks::PostWriteHook()(Section);
        }
#endif
        for (const FEntry& Entry : Entries)
        {
            double MaxError = 0.0;
            int32 ChannelsWritten = 0;
            for (int32 FrameIndex = 0; FrameIndex < Entry.Frames.Num(); ++FrameIndex)
            {
                const FFrameNumber Tick = DisplayFrameToTick(MovieScene, Entry.Frames[FrameIndex]);
                const TArray<double>& Want = Entry.Values[FrameIndex];
                ChannelsWritten = FMath::Max(ChannelsWritten, Want.Num());
                for (int32 Channel = 0; Channel < Want.Num(); ++Channel)
                {
                    float Got = 0.0f;
                    const bool bEvaluated = Entry.Channels[Channel]->Evaluate(FFrameTime(Tick), Got);
                    const double Error = FMath::Abs(static_cast<double>(Got) - Want[Channel]);
                    MaxError = FMath::Max(MaxError, Error);
                    // Channels store float: allow float rounding of the requested double, nothing more.
                    if (!bEvaluated || Error > 1.0e-4 * FMath::Max(1.0, FMath::Abs(Want[Channel])))
                    {
                        TSharedPtr<FJsonObject> Detail = MakeShared<FJsonObject>();
                        Detail->SetNumberField(TEXT("channel"), Channel);
                        Detail->SetNumberField(TEXT("requested"), Want[Channel]);
                        Detail->SetNumberField(TEXT("readBack"), Got);
                        AddMismatch(Entry.Control, Entry.Frames[FrameIndex], Detail);
                    }
                }
            }
            TSharedPtr<FJsonObject> Stats = MakeShared<FJsonObject>();
            Stats->SetStringField(TEXT("control"), Entry.Control);
            Stats->SetNumberField(TEXT("keysWritten"), Entry.Frames.Num());
            Stats->SetNumberField(TEXT("channelsWritten"), ChannelsWritten);
            Stats->SetNumberField(TEXT("maxReadbackError"), MaxError);
            ControlsOut.Add(MakeShared<FJsonValueObject>(Stats));
            KeysWritten += Entry.Frames.Num();
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("sequence"), SeqPath);
    Result->SetStringField(TEXT("binding"), BindingGuid.ToString(EGuidFormats::Digits));
    Result->SetStringField(TEXT("space"), bWorld ? TEXT("world") : TEXT("local"));
    Result->SetBoolField(TEXT("additive"), IsRigAdditive(Section->GetControlRig()));
    Result->SetArrayField(TEXT("controls"), ControlsOut);
    if (bWorld)
    {
        Result->SetNumberField(TEXT("positionToleranceCm"), PositionTolerance);
        Result->SetNumberField(TEXT("rotationToleranceDeg"), RotationTolerance);
    }

    if (Mismatches.Num() > 0)
    {
        const bool bRolledBack = RollbackChannels(Transaction, Package, bPackageWasDirty, Snapshots);
        Result->SetNumberField(TEXT("keysWritten"), 0);
        Result->SetNumberField(TEXT("mismatchCount"), Mismatches.Num());
        Mismatches.SetNum(FMath::Min(Mismatches.Num(), 20));
        Result->SetArrayField(TEXT("mismatches"), Mismatches);
        Result->SetBoolField(TEXT("rolledBack"), bRolledBack);
        Ctx.SendError(ErrorCodes::ERR_CONTROL_KEY_READBACK_MISMATCH, bRolledBack
            ? TEXT("A written key did not read back as requested; every key this call wrote was undone (see mismatches).")
            : TEXT("A written key did not read back as requested, and restoring the touched channels could not be verified; "
                   "the package is left dirty (see mismatches)."),
            Result);
        return true;
    }

    Result->SetNumberField(TEXT("keysWritten"), KeysWritten);
    Ctx.SendSuccess(Result);
    return true;
}

// sequencer.get_control_values — many controls x many frames, local channel values or world transforms.
REGISTER_RPC_HANDLER("sequencer.get_control_values", "sequencer",
    "Read many Control Rig controls at many display frames: local channel values (as get_control_value) or "
    "world transforms [TX,TY,TZ,Roll,Pitch,Yaw,SX,SY,SZ].",
    RPC_PARAMS(
        PINWRIGHT_CR_BATCH_TARGET_PARAMS,
        RPC_PARAM_REQ("controls", "array", "Control names"),
        RPC_PARAM_REQ("frames", "array", "Display frames (whole numbers)"),
        RPC_PARAM_REQ("space", "string", "local (float-channel values in index order; additive rigs report deltas) or world")
    ))
{
    using namespace PinWrightControlRigSequencer;
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    bool bWorld = false;
    if (!ParseSpace(Ctx, bWorld))
    {
        return true;
    }
    TArray<int32> Frames;
    if (!ParseFrameArray(Payload->TryGetField(TEXT("frames")), Frames))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("'frames' must be a non-empty array of whole display frames."));
        return true;
    }
    TArray<FString> Controls;
    const TArray<TSharedPtr<FJsonValue>>* ControlsArr = Ctx.GetArray(TEXT("controls"));
    for (int32 Index = 0; ControlsArr && Index < ControlsArr->Num(); ++Index)
    {
        FString Name;
        if (!(*ControlsArr)[Index].IsValid() || !(*ControlsArr)[Index]->TryGetString(Name) || Name.IsEmpty())
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(TEXT("controls[%d] must be a control name."), Index));
            return true;
        }
        Controls.Add(Name);
    }
    if (Controls.Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("'controls' must name at least one control."));
        return true;
    }

    FString SeqPath;
    UMovieScene* MovieScene = nullptr;
    FGuid BindingGuid;
    UMovieSceneControlRigParameterTrack* Track = nullptr;
    UMovieSceneControlRigParameterSection* Section = nullptr;
    if (!ResolveControlRigSection(Ctx, Payload, /*bPreferSectionToKey*/ false,
            SeqPath, MovieScene, BindingGuid, Track, Section))
    {
        return true;
    }
    FWorldSpaceContext World;
    if (bWorld && !ResolveWorldSpaceContext(Ctx, MovieScene, BindingGuid, Section, World))
    {
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> ControlsOut;
    TOptional<FRigPose> SavedPose;
    if (bWorld)
    {
        SavedPose = World.Rig->GetHierarchy()->GetPose();
    }
    for (const FString& Name : Controls)
    {
        FRigControlElement* Element = nullptr;
        TArray<FMovieSceneFloatChannel*> Channels;
        if (!ResolveKeyableControl(Ctx, Section, Name, Element, Channels))
        {
            if (SavedPose.IsSet())
            {
                World.Rig->GetHierarchy()->SetPose(SavedPose.GetValue());
            }
            return true;
        }
        if (bWorld && !TransformPartsForType(Element->Settings.ControlType).Any())
        {
            World.Rig->GetHierarchy()->SetPose(SavedPose.GetValue());
            Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_TYPE, FString::Printf(
                TEXT("Control '%s' is a %s control with no transform; read it in space 'local'."),
                *Name, *ControlTypeToString(Element->Settings.ControlType)));
            return true;
        }
        TArray<TSharedPtr<FJsonValue>> PerFrame;
        for (const int32 Frame : Frames)
        {
            const FFrameNumber Tick = DisplayFrameToTick(MovieScene, Frame);
            if (bWorld)
            {
                const FTransform W = ControlWorldAt(Section, World, FName(*Name), Tick);
                PerFrame.Add(MakeShared<FJsonValueArray>(NumbersToJson(TransformToChannelValues(WireTransformParts(), W))));
                continue;
            }
            TArray<TSharedPtr<FJsonValue>> Values;
            for (FMovieSceneFloatChannel* Channel : Channels)
            {
                float F = 0.0f;
                // A channel with no keys and no default has nothing to evaluate: null, not 0.
                if (Channel->Evaluate(FFrameTime(Tick), F))
                {
                    Values.Add(MakeShared<FJsonValueNumber>(F));
                }
                else
                {
                    Values.Add(MakeShared<FJsonValueNull>());
                }
            }
            PerFrame.Add(MakeShared<FJsonValueArray>(Values));
        }
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("control"), Name);
        Entry->SetStringField(TEXT("type"), ControlTypeToString(Element->Settings.ControlType));
        Entry->SetNumberField(TEXT("channelCount"), Channels.Num());
        Entry->SetArrayField(TEXT("values"), PerFrame);
        ControlsOut.Add(MakeShared<FJsonValueObject>(Entry));
    }
    if (SavedPose.IsSet())
    {
        World.Rig->GetHierarchy()->SetPose(SavedPose.GetValue());
    }

    TArray<TSharedPtr<FJsonValue>> FramesOut;
    for (const int32 Frame : Frames)
    {
        FramesOut.Add(MakeShared<FJsonValueNumber>(Frame));
    }
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("sequence"), SeqPath);
    Result->SetStringField(TEXT("binding"), BindingGuid.ToString(EGuidFormats::Digits));
    Result->SetStringField(TEXT("space"), bWorld ? TEXT("world") : TEXT("local"));
    Result->SetBoolField(TEXT("additive"), IsRigAdditive(Section->GetControlRig()));
    Result->SetArrayField(TEXT("frames"), FramesOut);
    Result->SetArrayField(TEXT("controls"), ControlsOut);
    Ctx.SendSuccess(Result);
    return true;
}

// sequencer.pin_controls — hold controls at a world transform over a frame range (contact lock),
// with smoothstep blends outside the range, in one transaction with readback and rollback.
REGISTER_RPC_HANDLER("sequencer.pin_controls", "sequencer",
    "Pin Control Rig controls (a planted foot, a hand on a rail) at a world transform over a display-frame range, with "
    "optional smoothstep blend-in/out frames outside it. Moves only the pinned controls (right for IK/effector controls, "
    "not for FK chains). Read back; any key outside tolerance undoes everything and fails CONTACT_TOLERANCE_EXCEEDED.",
    RPC_PARAMS(
        PINWRIGHT_CR_BATCH_TARGET_PARAMS,
        RPC_PARAM_REQ("controls", "array", "Control names to pin, parents before children"),
        RPC_PARAM_REQ("startFrame", "integer", "First display frame of the hold (inclusive)"),
        RPC_PARAM_REQ("endFrame", "integer", "Last display frame of the hold (inclusive, >= startFrame)"),
        RPC_PARAM_REQ("target", "string|object", "\"anchorFrame\" (hold each control where it is at anchorFrame) or an object mapping every pinned control to a world transform [TX,TY,TZ,Roll,Pitch,Yaw] or 9 numbers with scale"),
        RPC_PARAM_OPT("anchorFrame", "integer", "Display frame to sample when target is \"anchorFrame\" (required then, refused otherwise)"),
        RPC_PARAM_DEF("blendInFrames", "integer", "Frames before startFrame that ease from the original motion into the pin", "0"),
        RPC_PARAM_DEF("blendOutFrames", "integer", "Frames after endFrame that ease from the pin back to the original motion", "0"),
        RPC_PARAM_DEF("positionToleranceCm", "number", "Readback position tolerance in cm", "0.01"),
        RPC_PARAM_DEF("rotationToleranceDeg", "number", "Readback rotation tolerance in degrees", "0.01")
    ))
{
    using namespace PinWrightControlRigSequencer;
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    const int32 StartFrame = Ctx.GetInt(TEXT("startFrame"));
    const int32 EndFrame = Ctx.GetInt(TEXT("endFrame"));
    const int32 BlendIn = Ctx.GetInt(TEXT("blendInFrames"), 0);
    const int32 BlendOut = Ctx.GetInt(TEXT("blendOutFrames"), 0);
    if (EndFrame < StartFrame || BlendIn < 0 || BlendOut < 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("endFrame must be >= startFrame, and blendInFrames / blendOutFrames must be >= 0."));
        return true;
    }
    double PositionTolerance = 0.0;
    double RotationTolerance = 0.0;
    if (!ReadTolerance(Ctx, TEXT("positionToleranceCm"), PositionTolerance) ||
        !ReadTolerance(Ctx, TEXT("rotationToleranceDeg"), RotationTolerance))
    {
        return true;
    }

    TArray<FString> Controls;
    const TArray<TSharedPtr<FJsonValue>>* ControlsArr = Ctx.GetArray(TEXT("controls"));
    for (int32 Index = 0; ControlsArr && Index < ControlsArr->Num(); ++Index)
    {
        FString Name;
        if (!(*ControlsArr)[Index].IsValid() || !(*ControlsArr)[Index]->TryGetString(Name) || Name.IsEmpty() ||
            Controls.Contains(Name))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("controls[%d] must be a control name, each named once."), Index));
            return true;
        }
        Controls.Add(Name);
    }
    if (Controls.Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("'controls' must name at least one control."));
        return true;
    }

    // target: "anchorFrame" | {control: transform}
    const TSharedPtr<FJsonValue> TargetValue = Payload->TryGetField(TEXT("target"));
    const bool bAnchor = TargetValue.IsValid() && TargetValue->Type == EJson::String &&
        TargetValue->AsString().Equals(TEXT("anchorFrame"), ESearchCase::IgnoreCase);
    const bool bHasAnchorFrame = Payload->HasField(TEXT("anchorFrame"));
    TMap<FString, FTransform> ExplicitTargets;
    if (bAnchor != bHasAnchorFrame)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("anchorFrame is required when target is \"anchorFrame\" and refused otherwise."));
        return true;
    }
    if (!bAnchor)
    {
        const TSharedPtr<FJsonObject>* TargetObj = nullptr;
        if (!TargetValue.IsValid() || !TargetValue->TryGetObject(TargetObj) || (*TargetObj)->Values.Num() != Controls.Num())
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("target must be \"anchorFrame\" or an object with one world transform per pinned control."));
            return true;
        }
        for (const FString& Name : Controls)
        {
            FTransform Transform;
            if (!ParseWireTransform((*TargetObj)->TryGetField(Name), Transform))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
                    TEXT("target.%s must be [TX,TY,TZ,Roll,Pitch,Yaw] or 9 numbers with scale."), *Name));
                return true;
            }
            ExplicitTargets.Add(Name, Transform);
        }
    }

    FString SeqPath;
    UMovieScene* MovieScene = nullptr;
    FGuid BindingGuid;
    UMovieSceneControlRigParameterTrack* Track = nullptr;
    UMovieSceneControlRigParameterSection* Section = nullptr;
    if (!ResolveControlRigSection(Ctx, Payload, /*bPreferSectionToKey*/ true,
            SeqPath, MovieScene, BindingGuid, Track, Section))
    {
        return true;
    }
    FWorldSpaceContext World;
    if (!ResolveWorldSpaceContext(Ctx, MovieScene, BindingGuid, Section, World))
    {
        return true;
    }

    TArray<FMovieSceneFloatChannel*> AllChannels;
    for (const FString& Name : Controls)
    {
        FRigControlElement* Element = nullptr;
        TArray<FMovieSceneFloatChannel*> Channels;
        if (!ResolveKeyableControl(Ctx, Section, Name, Element, Channels))
        {
            return true;
        }
        const FControlTransformParts Parts = TransformPartsForType(Element->Settings.ControlType);
        if (!Parts.bT && !Parts.bR)
        {
            Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_TYPE, FString::Printf(
                TEXT("Control '%s' is a %s control with no position or rotation to pin."),
                *Name, *ControlTypeToString(Element->Settings.ControlType)));
            return true;
        }
        AllChannels.Append(Channels);
    }

    // Read the original motion (and the anchor) before any write.
    const int32 FirstFrame = StartFrame - BlendIn;
    const int32 LastFrame = EndFrame + BlendOut;
    const FRigPose SavedPose = World.Rig->GetHierarchy()->GetPose();
    TArray<FWorldKey> Keys;
    TArray<FTransform> Targets;
    for (const FString& Name : Controls)
    {
        const FName Control(*Name);
        const FTransform Target = bAnchor
            ? ControlWorldAt(Section, World, Control, DisplayFrameToTick(MovieScene, Ctx.GetInt(TEXT("anchorFrame"))))
            : ExplicitTargets[Name];
        Targets.Add(Target);
        for (int32 Frame = FirstFrame; Frame <= LastFrame; ++Frame)
        {
            // Smoothstep weight: 0 at the outer edge of a blend, 1 across the hold.
            double Weight = 1.0;
            if (Frame < StartFrame)
            {
                const double T = static_cast<double>(Frame - FirstFrame) / BlendIn;
                Weight = T * T * (3.0 - 2.0 * T);
            }
            else if (Frame > EndFrame)
            {
                const double T = static_cast<double>(LastFrame - Frame) / BlendOut;
                Weight = T * T * (3.0 - 2.0 * T);
            }
            FWorldKey& Key = Keys.AddDefaulted_GetRef();
            Key.Control = Control;
            Key.Frame = Frame;
            Key.Tick = DisplayFrameToTick(MovieScene, Frame);
            Key.World = Target;
            if (Weight < 1.0)
            {
                Key.World.Blend(ControlWorldAt(Section, World, Control, Key.Tick), Target, static_cast<float>(Weight));
            }
        }
    }
    World.Rig->GetHierarchy()->SetPose(SavedPose);

    TArray<FChannelSnapshot> Snapshots;
    SnapshotChannels(AllChannels, Snapshots);
    UPackage* Package = MovieScene->GetOutermost();
    const bool bPackageWasDirty = Package && Package->IsDirty();
    FScopedTransaction Transaction(NSLOCTEXT("PinWright", "PinControlRigControls", "Pin Control Rig Controls"));
    Section->Modify();
    const TArray<FWorldKeyError> Errors = WriteAndMeasureWorldKeys(Section, World, Keys);

    const int32 FramesPerControl = LastFrame - FirstFrame + 1;
    bool bExceeded = false;
    TArray<TSharedPtr<FJsonValue>> ControlsOut;
    for (int32 ControlIndex = 0; ControlIndex < Controls.Num(); ++ControlIndex)
    {
        const FControlTransformParts Parts =
            TransformPartsForType(World.Rig->FindControl(FName(*Controls[ControlIndex]))->Settings.ControlType);
        double MaxCm = 0.0, MaxDeg = 0.0;
        for (int32 Index = ControlIndex * FramesPerControl; Index < (ControlIndex + 1) * FramesPerControl; ++Index)
        {
            MaxCm = FMath::Max(MaxCm, Errors[Index].Cm);
            MaxDeg = FMath::Max(MaxDeg, Errors[Index].Deg);
        }
        bExceeded |= MaxCm > PositionTolerance || MaxDeg > RotationTolerance;
        TSharedPtr<FJsonObject> Stats = MakeShared<FJsonObject>();
        Stats->SetStringField(TEXT("control"), Controls[ControlIndex]);
        Stats->SetNumberField(TEXT("framesKeyed"), FramesPerControl);
        if (Parts.bT) { Stats->SetNumberField(TEXT("maxErrorCm"), MaxCm); }
        if (Parts.bR) { Stats->SetNumberField(TEXT("maxErrorDeg"), MaxDeg); }
        Stats->SetArrayField(TEXT("target"), NumbersToJson(TransformToChannelValues(WireTransformParts(), Targets[ControlIndex])));
        ControlsOut.Add(MakeShared<FJsonValueObject>(Stats));
    }

    auto MakeRange = [](int32 First, int32 Last)
    {
        TSharedPtr<FJsonObject> Range = MakeShared<FJsonObject>();
        Range->SetNumberField(TEXT("startFrame"), First);
        Range->SetNumberField(TEXT("endFrame"), Last);
        return Range;
    };
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("sequence"), SeqPath);
    Result->SetStringField(TEXT("binding"), BindingGuid.ToString(EGuidFormats::Digits));
    Result->SetObjectField(TEXT("holdRange"), MakeRange(StartFrame, EndFrame));
    Result->SetObjectField(TEXT("keyedRange"), MakeRange(FirstFrame, LastFrame));
    Result->SetNumberField(TEXT("positionToleranceCm"), PositionTolerance);
    Result->SetNumberField(TEXT("rotationToleranceDeg"), RotationTolerance);
    Result->SetArrayField(TEXT("controls"), ControlsOut);

    if (bExceeded)
    {
        const bool bRolledBack = RollbackChannels(Transaction, Package, bPackageWasDirty, Snapshots);
        Result->SetNumberField(TEXT("keysWritten"), 0);
        Result->SetBoolField(TEXT("rolledBack"), bRolledBack);
        Ctx.SendError(ErrorCodes::ERR_CONTACT_TOLERANCE_EXCEEDED, bRolledBack
            ? TEXT("A pinned control missed the position/rotation tolerance on readback; every key this call wrote was undone.")
            : TEXT("A pinned control missed the tolerance on readback, and restoring the touched channels could not be "
                   "verified; the package is left dirty."),
            Result);
        return true;
    }
    Result->SetNumberField(TEXT("keysWritten"), Keys.Num());
    Ctx.SendSuccess(Result);
    return true;
}

#undef PINWRIGHT_CR_BATCH_TARGET_PARAMS
