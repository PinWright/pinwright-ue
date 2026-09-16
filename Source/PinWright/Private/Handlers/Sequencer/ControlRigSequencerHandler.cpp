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
            Ctx.SendError(TEXT("SEQUENCE_NOT_FOUND"),
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
            Ctx.SendError(TEXT("CONTROLRIG_TRACK_NOT_FOUND"),
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
        Ctx.SendError(TEXT("SEQUENCE_NOT_FOUND"),
            FString::Printf(TEXT("Level sequence not found for path '%s'."), *SeqPath));
        return true;
    }
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!MovieScene)
    {
        Ctx.SendError(TEXT("SEQUENCE_INVALID"), TEXT("Level sequence has no MovieScene."));
        return true;
    }

    const FGuid BindingGuid = ResolveBindingGuid(Payload);
    if (!BindingGuid.IsValid() || !MovieScene->FindBinding(BindingGuid))
    {
        Ctx.SendError(TEXT("BINDING_NOT_FOUND"),
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
            Ctx.SendError(TEXT("RIG_CLASS_NOT_FOUND"),
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
        Ctx.SendError(TEXT("BINDING_NOT_SKELETAL"),
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
        Ctx.SendError(TEXT("TRACK_CREATE_FAILED"), TEXT("Failed to add a Control Rig parameter track to the binding."));
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
        Ctx.SendError(TEXT("CONTROLRIG_TRACK_NOT_FOUND"),
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
        Ctx.SendError(TEXT("MISSING_FRAME"), TEXT("A numeric 'frame' is required."));
        return true;
    }
    const TSharedPtr<FJsonObject>* ControlsObj = nullptr;
    if (!Payload->TryGetObjectField(TEXT("controls"), ControlsObj) || !ControlsObj || !(*ControlsObj).IsValid())
    {
        Ctx.SendError(TEXT("MISSING_CONTROLS"), TEXT("A 'controls' object mapping control name -> value is required."));
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
        Ctx.SendError(TEXT("NO_CONTROLS_KEYED"),
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
        Ctx.SendError(TEXT("MISSING_CONTROL"), TEXT("A 'control' name is required."));
        return true;
    }
    double FrameNum = 0.0;
    if (!Payload->TryGetNumberField(TEXT("frame"), FrameNum))
    {
        Ctx.SendError(TEXT("MISSING_FRAME"), TEXT("A numeric 'frame' is required."));
        return true;
    }

    TArray<FMovieSceneFloatChannel*> Channels = ChannelsForControl(Section, FName(*ControlName));
    if (Channels.Num() == 0)
    {
        Ctx.SendError(TEXT("CONTROL_NOT_FOUND"),
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
