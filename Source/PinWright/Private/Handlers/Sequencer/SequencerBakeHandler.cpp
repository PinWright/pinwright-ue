// Copyright (c) 2026 Alexander Penkin. MIT License.

// Sequencer bake operations (board ticket F-sequencer-bake-controlrig):
//
//   sequencer.bake_to_controlrig   - the binding's EVALUATED animation becomes an
//                                    editable Control Rig track on that same binding.
//   sequencer.export_anim_sequence - the binding's EVALUATED performance becomes an
//                                    AnimSequence asset.
//   sequencer.bake_control_space   - an existing Control Rig control is baked into a
//                                    new parent space while preserving world motion.
//
// Together with sequencer.add_controlrig_track / list_controls / key_controls /
// get_control_value (ControlRigSequencerHandler.cpp) these close the CR cinematics
// loop: AnimSequence -> CR track -> edited control key -> exported AnimSequence.
//
// THE TWO HEADLESS VERBS DRIVE THE ENGINE'S OWN BAKE, NOT A HAND-ROLLED KEYFRAME TRANSFER.
// UControlRigSequencerEditorLibrary::BakeToControlRig and
// USequencerToolsFunctionLibrary::ExportAnimSequence each spin up a transient
// ULevelSequencePlayer, evaluate the sequence frame by frame through
// MovieSceneToolHelpers::ExportToAnimSequence, and write the result. That evaluation is
// the whole point: it composes every track, attachment and blend on the binding, which
// copying float channels between sections cannot reproduce. Both entry points create
// their own player when no Sequencer is open (ControlRigSequencerEditorLibrary.cpp:1395,
// SequencerTools.cpp:391), so both work headless.
//
// HEADLESS NOISE. BakeToControlRig calls the file-static GetSequencerFromAsset() before
// it decides which player to use, and that helper logs `LogControlRig Error: Can not open
// Sequencer for the LevelSequence None` whenever no Level Sequence editor is open. The
// bake then proceeds down the transient-player path and succeeds. The log line is
// cosmetic; nothing in this file reacts to it, and the regression test suppresses log
// errors rather than asserting on it.
//
// WHAT IS REPORTED IS MEASURED, NOT ECHOED (docs/rpc-design.md, response honesty).
// A bake that quietly writes nothing is the defect this ticket exists to prevent, so
// neither verb reports success off the engine's bool alone:
//   * bake_to_controlrig counts the keys actually present on the new Control Rig
//     section's float channels and the frame span they cover, and fails
//     NO_KEYS_WRITTEN when that count is zero even though the engine said true.
//   * export_anim_sequence walks the written AnimSequence's data model bone track by
//     bone track (IterateBoneKeys, the accessor that reports what the sequencer data
//     model actually stores) and fails NO_KEYS_WRITTEN on an empty result.
// A held pose legitimately collapses to ONE key per bone track in UE 5.8's sequencer
// data model, so a low per-track count is not a defect and is reported rather than gated.
//
// SPACE BAKE IS EDITOR-GATED. Unlike the two headless verbs above, the engine implementation
// requires the requested sequence to be the current open Level Sequence editor. The handler
// checks that precondition explicitly and creates the section's otherwise-missing space channel
// before calling the engine, closing both silent-false and silent-true/no-op paths.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Sequencer/SequencerBindingUtils.h"
#include "Utils/AssetUtils.h"
#include "Utils/ClassUtils.h"
#include "Utils/TransactionUtils.h"
#include "Compat/EngineVersionCompat.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/World.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBindingProxy.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneFloatChannel.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Exporters/AnimSeqExportOption.h"
#include "Factories/AnimSequenceFactory.h"
#include "GameFramework/Actor.h"

#include "ControlRig.h"
#include "ControlRigSequencerEditorLibrary.h"
#include "Rigs/FKControlRig.h"
#include "Rigs/RigHierarchy.h"
#include "Sequencer/MovieSceneControlRigParameterSection.h"
#include "Sequencer/MovieSceneControlRigParameterTrack.h"
#include "SequencerTools.h"

#include "BakingAnimationKeySettings.h"
#include "ILevelSequenceEditorToolkit.h"
#include "ISequencer.h"
#include "LevelSequenceEditorBlueprintLibrary.h"
#include "RigSpacePickerBakeSettings.h"
#include "Sequencer/MovieSceneControlRigSpaceChannel.h"
#include "Subsystems/AssetEditorSubsystem.h"

#include "AssetCompilingManager.h"
// UE 5.4 split IAssetCompilingManager out of AssetCompilingManager.h into its own header. On
// 5.3 the interface is declared inside AssetCompilingManager.h (included above).
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
#include "IAssetCompilingManager.h"
#endif



// Distinct namespace and distinct helper names from the sibling
// PinWrightControlRigSequencer (ControlRigSequencerHandler.cpp) so a unity build that
// merges the two translation units cannot collide on either.
namespace PinWrightSequencerBake
{
#if UE_VERSION_OLDER_THAN(5, 8, 0)
    // The engine's bake abandons a transient UAnimSequence that stays registered for
    // compilation, and the next anim-sequence creation in the session then kills the editor.
    //
    // UControlRigSequencerEditorLibrary::BakeToControlRig evaluates the binding into
    // NewObject<UAnimSequence>(GetTransientPackage()) and MarkAsGarbage()es it when it is done
    // (ControlRigSequencerEditorLibrary.cpp:1274). Giving that sequence its skeleton first
    // registers it with FAnimSequenceCompilingManager, and the garbage object stays referenced
    // by the baked track, so no GC ever marks it unreachable and the manager's
    // OnPostReachabilityAnalysis never drops it.
    //
    // A registered-but-invalid entry poisons the manager for the rest of the session, because
    // UAnimSequence::BeginCacheDerivedData calls FinishCompilation({ this, RefPoseSeq }) and
    // RefPoseSeq is normally null. UE 5.7 and older have no null guard there (5.8 added
    // `if (AnimSequence && RegisteredAnimSequences.Contains(AnimSequence))`); TWeakObjectPtr
    // equality reports two INVALID pointers as equal, and a TSet holding fewer than four
    // elements has a single hash bucket, so Contains(nullptr) matches the abandoned entry. The
    // null then reaches FCompilableAnimationSequence, whose GetName() dereferences a null
    // TStrongObjectPtr: `Assertion failed: IsValid()`, process gone. Measured on 5.7 -
    // PinWright.Sequencer.ControlRigBake.AnimSequenceRoundTrip took the whole suite down at its
    // own export step, one verb call after this bake.
    //
    // ProcessAsyncTasks is the cure and the only one: FAnimSequenceCompilingManager's
    // ProcessAnimSequences rebuilds RegisteredAnimSequences from the entries that are still
    // IsValid(), so one pass drops every abandoned entry while postponing sequences whose
    // compilation is genuinely still running. FinishAllCompilation does NOT do this - it only
    // walks the valid entries - and a GC cannot help while the dead object is still referenced.
    static void BakePurgeAbandonedAnimCompilations()
    {
        static const FName AnimSequenceManagerName(TEXT("UE-AnimationSequence"));
        for (IAssetCompilingManager* Manager : FAssetCompilingManager::Get().GetRegisteredManagers())
        {
            if (Manager && Manager->GetAssetTypeName() == AnimSequenceManagerName)
            {
                if (Manager->GetNumRemainingAssets() > 0)
                {
                    // Driven through FAssetCompilingManager because IAssetCompilingManager's own
                    // ProcessAsyncTasks is protected. bLimitExecutionTime=true is what the editor
                    // passes on its own tick, and it is load-bearing: the unlimited form drives
                    // EVERY manager - shaders and meshes included - to completion in one call,
                    // which can block the game thread for minutes right after a bake. The limit
                    // costs nothing here, because ProcessAnimSequences rebuilds
                    // RegisteredAnimSequences from the still-valid entries on every pass
                    // regardless of how many it processes.
                    FAssetCompilingManager::Get().ProcessAsyncTasks(/*bLimitExecutionTime=*/true);
                }
                return;
            }
        }
    }
#endif

    // Resolve the level sequence named by `sequence` (or its `path` alias).
    static ULevelSequence* BakeResolveSequence(const TSharedPtr<FJsonObject>& Payload, FString& OutPath)
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
        if (Path.IsEmpty())
        {
            return nullptr;
        }
        // CreatePackage IS NOT THE ONLY DOOR TO ITS OWN FATAL, and this is the other one.
        // StaticLoadObjectInternal calls ResolveName2(..., Create=true) (UObjectGlobals.cpp:1427)
        // and ResolveName2 calls CreatePackage on the partial name (:1310), so the LoadObject
        // below is a CreatePackage on whatever the caller put in `sequence` - and a "//" in it
        // ends the editor PROCESS rather than failing the load (board
        // B-createpackage-unvalidated-paths-plugin-wide). FindObject would be safe (Create=false,
        // :620); LoadObject is not. Refusing here answers SEQUENCE_NOT_FOUND, which is what an
        // unresolvable path already answered, so nothing that worked before is newly refused: a
        // path this check rejects could not have named a loadable sequence.
        const FString SequencePackageName = FPackageName::ObjectPathToPackageName(Path);
        if (!FPackageName::IsValidLongPackageName(SequencePackageName,
                                                 /*bIncludeReadOnlyRoots=*/true))
        {
            return nullptr;
        }
        // LoadObject first so an in-memory / transient sequence resolves; the asset-registry
        // load is the /Game case. Mirrors SequencerFbxResolveSequence.
        if (ULevelSequence* Loaded = LoadObject<ULevelSequence>(nullptr, *Path))
        {
            return Loaded;
        }
        if (!ResolveAsset(Path).bExists)
        {
            return nullptr;
        }
        return Cast<ULevelSequence>(ResolveAsset(Path, /*bLoadObject=*/true).Object);
    }

    // Read the binding GUID from `binding` / `bindingGuid` / `bindingId` (Digits form).
    static FGuid BakeResolveBindingGuid(const TSharedPtr<FJsonObject>& Payload)
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

    // The skeletal mesh component the binding resolves to through the PLAYBACK resolve
    // path (SequencerBindingUtils::ResolveBoundObjects runs the universal-object-locator
    // resolve, not the write path), or nullptr when the binding is not skeletal.
    static USkeletalMeshComponent* BakeResolveSkeletalMeshComponent(ULevelSequence* Sequence, const FGuid& BindingGuid)
    {
        if (!Sequence || !BindingGuid.IsValid())
        {
            return nullptr;
        }
        for (UObject* Bound : SequencerBindingUtils::ResolveBoundObjects(Sequence, BindingGuid))
        {
            if (USkeletalMeshComponent* AsComponent = Cast<USkeletalMeshComponent>(Bound))
            {
                return AsComponent;
            }
            if (AActor* Actor = Cast<AActor>(Bound))
            {
                TArray<USkeletalMeshComponent*> Components;
                Actor->GetComponents(Components);
                for (USkeletalMeshComponent* Component : Components)
                {
                    if (Component && Component->GetSkeletalMeshAsset())
                    {
                        return Component;
                    }
                }
                if (Components.Num() > 0)
                {
                    return Components[0];
                }
            }
        }
        return nullptr;
    }

    static UMovieSceneControlRigParameterTrack* BakeFindControlRigTrack(UMovieScene* MovieScene, const FGuid& BindingGuid)
    {
        if (!MovieScene || !BindingGuid.IsValid())
        {
            return nullptr;
        }
        for (UMovieSceneTrack* Track :
                 MovieScene->FindTracks(UMovieSceneControlRigParameterTrack::StaticClass(), BindingGuid, NAME_None))
        {
            if (UMovieSceneControlRigParameterTrack* CRTrack = Cast<UMovieSceneControlRigParameterTrack>(Track))
            {
                return CRTrack;
            }
        }
        return nullptr;
    }

    static UMovieSceneControlRigParameterSection* BakeFindControlRigSection(
        UMovieSceneControlRigParameterTrack* Track, FName ControlName)
    {
        if (!Track)
        {
            return nullptr;
        }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
        return Cast<UMovieSceneControlRigParameterSection>(Track->GetSectionToKey(ControlName));
#else
        // The control-specific overload arrived in 5.5. Older Control Rig tracks expose
        // one section-to-key for the track, which is the section BakeControlRigSpace uses.
        UMovieSceneControlRigParameterSection* Section =
            Cast<UMovieSceneControlRigParameterSection>(Track->GetSectionToKey());
        if (!Section && Track->GetAllSections().Num() > 0)
        {
            Section = Cast<UMovieSceneControlRigParameterSection>(Track->GetAllSections()[0]);
        }
        return Section;
#endif
    }

    static bool BakeParseRigElementType(const FString& TypeName, ERigElementType& OutType)
    {
        if (TypeName.Equals(TEXT("Bone"), ESearchCase::IgnoreCase))
        {
            OutType = ERigElementType::Bone;
        }
        else if (TypeName.Equals(TEXT("Null"), ESearchCase::IgnoreCase) ||
                 TypeName.Equals(TEXT("Space"), ESearchCase::IgnoreCase))
        {
            OutType = ERigElementType::Null;
        }
        else if (TypeName.Equals(TEXT("Control"), ESearchCase::IgnoreCase))
        {
            OutType = ERigElementType::Control;
        }
        else if (TypeName.Equals(TEXT("Reference"), ESearchCase::IgnoreCase))
        {
            OutType = ERigElementType::Reference;
        }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        else if (TypeName.Equals(TEXT("Socket"), ESearchCase::IgnoreCase))
        {
            OutType = ERigElementType::Socket;
        }
#endif
        else
        {
            return false;
        }
        return true;
    }

    static FFrameNumber BakeDisplayFrameToTick(const UMovieScene* MovieScene, int32 DisplayFrame)
    {
        return FFrameRate::TransformTime(FFrameTime(FFrameNumber(DisplayFrame)),
            MovieScene->GetDisplayRate(), MovieScene->GetTickResolution()).RoundToFrame();
    }

    static int32 BakeCountKeysInRange(
        TArrayView<const FFrameNumber> Times,
        FFrameNumber StartTick,
        FFrameNumber EndTick)
    {
        int32 Count = 0;
        for (const FFrameNumber Time : Times)
        {
            if (Time >= StartTick && Time <= EndTick)
            {
                ++Count;
            }
        }
        return Count;
    }

    struct FBakeControlSpaceKeyStats
    {
        int32 TransformChannelCount = 0;
        int32 TransformKeysInRange = 0;
        int32 SpaceKeysInRange = 0;
        bool bHasSpaceChannel = false;
    };

    struct FBakeFloatChannelState
    {
        TArray<FFrameNumber> Times;
        TArray<FMovieSceneFloatValue> Values;
    };

    struct FBakeControlSpaceChannelState
    {
        TArray<FBakeFloatChannelState> TransformChannels;
        bool bHasSpaceChannel = false;
        TArray<FFrameNumber> SpaceTimes;
        TArray<FMovieSceneControlRigSpaceBaseKey> SpaceValues;
    };

#if UE_VERSION_OLDER_THAN(5, 6, 0)
    static int32 BakeNumFloatChannelsForControlType(ERigControlType Type)
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

    static bool BakeFloatChannelRangeForControl(
        UMovieSceneControlRigParameterSection* Section,
        FName ControlName,
        int32& OutStart,
        int32& OutCount)
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
        const int32 Count = BakeNumFloatChannelsForControlType(Element->Settings.ControlType);
        if (Count <= 0 || Info->ChannelIndex < 0)
        {
            return false;
        }
        OutStart = Info->ChannelIndex;
        OutCount = Count;
        return true;
    }
#endif

    static TArray<FMovieSceneFloatChannel*> BakeFloatChannelsForControl(
        UMovieSceneControlRigParameterSection* Section,
        FName ControlName)
    {
        TArray<FMovieSceneFloatChannel*> Result;
        if (!Section)
        {
            return Result;
        }
        FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
        TArrayView<FMovieSceneFloatChannel*> FloatChannels = Proxy.GetChannels<FMovieSceneFloatChannel>();
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        TArray<TPair<int32, FMovieSceneFloatChannel*>> Matched;
        for (FMovieSceneFloatChannel* Channel : FloatChannels)
        {
            if (!Channel)
            {
                continue;
            }
            const UE::MovieScene::FControlRigChannelMetaData Meta = Section->GetChannelMetaData(Channel);
            if (static_cast<bool>(Meta) && Meta.GetControlName() == ControlName)
            {
                Matched.Add(TPair<int32, FMovieSceneFloatChannel*>(Meta.GetChannelIndex(), Channel));
            }
        }
        Matched.Sort([](const TPair<int32, FMovieSceneFloatChannel*>& A,
                        const TPair<int32, FMovieSceneFloatChannel*>& B)
        {
            return A.Key < B.Key;
        });
        for (const TPair<int32, FMovieSceneFloatChannel*>& Pair : Matched)
        {
            Result.Add(Pair.Value);
        }
#else
        int32 Start = 0;
        int32 Count = 0;
        if (!BakeFloatChannelRangeForControl(Section, ControlName, Start, Count) ||
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

    static FBakeControlSpaceKeyStats BakeMeasureControlSpaceKeys(
        UMovieSceneControlRigParameterSection* Section,
        FName ControlName,
        FFrameNumber StartTick,
        FFrameNumber EndTick)
    {
        FBakeControlSpaceKeyStats Stats;
        if (!Section)
        {
            return Stats;
        }

        for (FMovieSceneFloatChannel* Channel : BakeFloatChannelsForControl(Section, ControlName))
        {
            ++Stats.TransformChannelCount;
            Stats.TransformKeysInRange += BakeCountKeysInRange(
                Channel->GetData().GetTimes(), StartTick, EndTick);
        }

        if (FSpaceControlNameAndChannel* Space = Section->GetSpaceChannel(ControlName))
        {
            Stats.bHasSpaceChannel = true;
            Stats.SpaceKeysInRange = BakeCountKeysInRange(
                Space->SpaceCurve.GetData().GetTimes(), StartTick, EndTick);
        }
        return Stats;
    }

    static FBakeControlSpaceChannelState BakeCaptureControlSpaceChannelState(
        UMovieSceneControlRigParameterSection* Section,
        FName ControlName)
    {
        FBakeControlSpaceChannelState State;
        if (!Section)
        {
            return State;
        }

        for (FMovieSceneFloatChannel* Channel : BakeFloatChannelsForControl(Section, ControlName))
        {
            FBakeFloatChannelState& ChannelState = State.TransformChannels.AddDefaulted_GetRef();
            const auto Data = static_cast<const FMovieSceneFloatChannel*>(Channel)->GetData();
            const TArrayView<const FFrameNumber> Times = Data.GetTimes();
            const TArrayView<const FMovieSceneFloatValue> Values = Data.GetValues();
            ChannelState.Times.Append(Times.GetData(), Times.Num());
            ChannelState.Values.Append(Values.GetData(), Values.Num());
        }

        if (const FSpaceControlNameAndChannel* Space = Section->GetSpaceChannel(ControlName))
        {
            State.bHasSpaceChannel = true;
            const auto Data = static_cast<const FMovieSceneControlRigSpaceChannel&>(
                Space->SpaceCurve).GetData();
            const TArrayView<const FFrameNumber> Times = Data.GetTimes();
            const TArrayView<const FMovieSceneControlRigSpaceBaseKey> Values = Data.GetValues();
            State.SpaceTimes.Append(Times.GetData(), Times.Num());
            State.SpaceValues.Append(Values.GetData(), Values.Num());
        }
        return State;
    }

    static TSharedPtr<ISequencer> BakeFindMatchingOpenSequencer(ULevelSequence* Sequence)
    {
        if (!Sequence || !GEditor ||
            ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence() != Sequence)
        {
            return nullptr;
        }
        UAssetEditorSubsystem* AssetEditors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
        IAssetEditorInstance* Editor = AssetEditors
            ? AssetEditors->FindEditorForAsset(Sequence, /*bFocusIfOpen=*/false) : nullptr;
        ILevelSequenceEditorToolkit* Toolkit = static_cast<ILevelSequenceEditorToolkit*>(Editor);
        const TSharedPtr<ISequencer> Sequencer = Toolkit ? Toolkit->GetSequencer() : nullptr;
        return Sequencer.IsValid() && Sequencer->GetRootMovieSceneSequence() == Sequence
            ? Sequencer : nullptr;
    }

    static bool BakeCanSwitchToParent(
        URigHierarchy* Hierarchy,
        const FRigElementKey& Child,
        const FRigElementKey& Parent,
        FString& OutFailureReason)
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        return Hierarchy->CanSwitchToParent(
            Child, Parent, FEmptyRigDependenciesProvider(), &OutFailureReason);
#elif UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        return Hierarchy->CanSwitchToParent(
            Child, Parent, FNoDependenciesProvider(), &OutFailureReason);
#else
        // TElementDependencyMap is a typedef nested in URigHierarchy, not a free name.
        return Hierarchy->CanSwitchToParent(
            Child, Parent, URigHierarchy::TElementDependencyMap(), &OutFailureReason);
#endif
    }

    static bool BakeControlSpaceChannelStatesEqual(
        const FBakeControlSpaceChannelState& A,
        const FBakeControlSpaceChannelState& B)
    {
        if (A.bHasSpaceChannel != B.bHasSpaceChannel ||
            A.SpaceTimes != B.SpaceTimes || A.SpaceValues != B.SpaceValues ||
            A.TransformChannels.Num() != B.TransformChannels.Num())
        {
            return false;
        }
        for (int32 Index = 0; Index < A.TransformChannels.Num(); ++Index)
        {
            const FBakeFloatChannelState& AChannel = A.TransformChannels[Index];
            const FBakeFloatChannelState& BChannel = B.TransformChannels[Index];
            if (AChannel.Times != BChannel.Times ||
                AChannel.Values.Num() != BChannel.Values.Num())
            {
                return false;
            }
            for (int32 ValueIndex = 0; ValueIndex < AChannel.Values.Num(); ++ValueIndex)
            {
                const FMovieSceneFloatValue& AValue = AChannel.Values[ValueIndex];
                const FMovieSceneFloatValue& BValue = BChannel.Values[ValueIndex];
                if (AValue.Value != BValue.Value ||
                    AValue.Tangent.ArriveTangent != BValue.Tangent.ArriveTangent ||
                    AValue.Tangent.LeaveTangent != BValue.Tangent.LeaveTangent ||
                    AValue.Tangent.ArriveTangentWeight != BValue.Tangent.ArriveTangentWeight ||
                    AValue.Tangent.LeaveTangentWeight != BValue.Tangent.LeaveTangentWeight ||
                    AValue.Tangent.TangentWeightMode != BValue.Tangent.TangentWeightMode ||
                    AValue.InterpMode != BValue.InterpMode ||
                    AValue.TangentMode != BValue.TangentMode)
                {
                    return false;
                }
            }
        }
        return true;
    }

    static bool BakeRollbackControlSpace(
        FScopedTransaction& Transaction,
        UPackage* Package,
        bool bPackageWasDirty,
        UMovieSceneControlRigParameterSection* Section,
        FName ControlName,
        FFrameNumber StartTick,
        FFrameNumber EndTick,
        const FBakeControlSpaceChannelState& BeforeState,
        const TSharedPtr<ISequencer>& Sequencer,
        FFrameTime OriginalGlobalTime,
        FBakeControlSpaceKeyStats& OutRestored)
    {
        PinWrightTransactionUtils::ApplyAndCancelTransaction(Transaction);

        // Undo restores serialized channel arrays, but notifications were intentionally
        // suppressed by ApplyAndCancelTransaction. Rebuild the proxy and reevaluate the
        // editor explicitly so neither cached channels nor the live rig retain the failed bake.
        Section->ReconstructChannelProxy();
        Sequencer->NotifyMovieSceneDataChanged(EMovieSceneDataChangeType::MovieSceneStructureItemsChanged);
        Sequencer->SetGlobalTime(OriginalGlobalTime, /*bEvaluate=*/true);
        Sequencer->ForceEvaluate();

        OutRestored = BakeMeasureControlSpaceKeys(
            Section, ControlName, StartTick, EndTick);
        const FBakeControlSpaceChannelState RestoredState =
            BakeCaptureControlSpaceChannelState(Section, ControlName);
        if (!BakeControlSpaceChannelStatesEqual(BeforeState, RestoredState))
        {
            if (Package)
            {
                Package->SetDirtyFlag(true);
            }
            return false;
        }

        // Only clear a package dirtied by the failed attempt after exact target-control
        // transform/space key times and values have been verified against the snapshot.
        if (Package)
        {
            Package->SetDirtyFlag(bPackageWasDirty);
        }
        return !Package || Package->IsDirty() == bPackageWasDirty;
    }

    // What a bake actually deposited on a Control Rig section's float channels.
    struct FBakeChannelKeyStats
    {
        int32 ChannelCount = 0;
        int32 ChannelsWithKeys = 0;
        int32 TotalKeys = 0;
        bool bHasRange = false;
        FFrameNumber MinTick = FFrameNumber(0);
        FFrameNumber MaxTick = FFrameNumber(0);
    };

    // Count the keys the bake wrote. Deliberately reads the SECTION rather than trusting
    // the engine's bool: BakeToControlRig reports true as soon as it created the track,
    // and a track whose channels carry no keys is the silent-zero failure this measures.
    static FBakeChannelKeyStats BakeMeasureSectionKeys(UMovieSceneControlRigParameterSection* Section)
    {
        FBakeChannelKeyStats Stats;
        if (!Section)
        {
            return Stats;
        }
        FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
        for (FMovieSceneFloatChannel* Channel : Proxy.GetChannels<FMovieSceneFloatChannel>())
        {
            if (!Channel)
            {
                continue;
            }
            ++Stats.ChannelCount;
            const TArrayView<const FFrameNumber> Times = Channel->GetData().GetTimes();
            if (Times.Num() == 0)
            {
                continue;
            }
            ++Stats.ChannelsWithKeys;
            Stats.TotalKeys += Times.Num();
            // Channel key times are stored ascending, so the ends are the extremes.
            const FFrameNumber First = Times[0];
            const FFrameNumber Last = Times[Times.Num() - 1];
            if (!Stats.bHasRange)
            {
                Stats.bHasRange = true;
                Stats.MinTick = First;
                Stats.MaxTick = Last;
            }
            else
            {
                Stats.MinTick = FMath::Min(Stats.MinTick, First);
                Stats.MaxTick = FMath::Max(Stats.MaxTick, Last);
            }
        }
        return Stats;
    }

    // Tick-resolution frame -> display-rate frame, so every frame number on the wire is in
    // the unit a caller passes to sequencer.key_controls and reads in the Sequencer UI.
    static double BakeTickToDisplayFrame(const UMovieScene* MovieScene, FFrameNumber Tick)
    {
        if (!MovieScene)
        {
            return static_cast<double>(Tick.Value);
        }
        return FFrameRate::TransformTime(FFrameTime(Tick),
            MovieScene->GetTickResolution(), MovieScene->GetDisplayRate()).AsDecimal();
    }

    static TSharedPtr<FJsonObject> BakeMakeFrameRange(double StartFrame, double EndFrame)
    {
        TSharedPtr<FJsonObject> Range = MakeShared<FJsonObject>();
        Range->SetNumberField(TEXT("startFrame"), StartFrame);
        Range->SetNumberField(TEXT("endFrame"), EndFrame);
        return Range;
    }

    // What the exported AnimSequence asset ended up holding.
    struct FBakeAnimModelStats
    {
        bool bReadable = false;
        int32 BoneTrackCount = 0;
        int32 TotalBoneKeys = 0;
        int32 NumberOfFrames = 0;
        FFrameRate FrameRate;
        float PlayLengthSeconds = 0.0f;
    };

    // Read the written asset back through its data model. IterateBoneKeys is the accessor
    // that reports what the SEQUENCER data model stores (the deprecated FBoneAnimationTrack
    // accessors are deliberately left empty by UAnimationSequencerDataModel), so this is a
    // measurement of the file rather than a restatement of the request.
    static FBakeAnimModelStats BakeMeasureAnimSequence(UAnimSequence* AnimSequence)
    {
        FBakeAnimModelStats Stats;
        const IAnimationDataModel* Model = AnimSequence ? AnimSequence->GetDataModel() : nullptr;
        if (!Model)
        {
            return Stats;
        }
        Stats.bReadable = true;
        Stats.NumberOfFrames = Model->GetNumberOfFrames();
        Stats.FrameRate = Model->GetFrameRate();
        Stats.PlayLengthSeconds = AnimSequence->GetPlayLength();

        TArray<FName> TrackNames;
        Model->GetBoneTrackNames(TrackNames);
        Stats.BoneTrackCount = TrackNames.Num();
        for (const FName& TrackName : TrackNames)
        {
            int32 KeyCount = 0;
            Model->IterateBoneKeys(TrackName,
                [&KeyCount](const FVector3f&, const FQuat4f&, const FVector3f&, const FFrameNumber&)
                {
                    ++KeyCount;
                    return true;
                });
            Stats.TotalBoneKeys += KeyCount;
        }
        return Stats;
    }
}

// ============================================================================
// sequencer.bake_to_controlrig
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.bake_to_controlrig", "sequencer",
    "Bake a skeletal binding's evaluated animation into an editable Control Rig track "
    "(UControlRigSequencerEditorLibrary::BakeToControlRig). DESTRUCTIVE: the engine removes every "
    "existing Control Rig track on the binding and disables its skeletal animation track. Reports the "
    "keys actually written and the frames they span, and fails NO_KEYS_WRITTEN when the bake produced none.",
    RPC_PARAMS(
        RPC_PARAM_OPT("sequence", "path", "Level sequence asset path (or 'path')"),
        RPC_PARAM_OPT("path", "path", "Alias for sequence"),
        RPC_PARAM_OPT("binding", "string", "Possessable binding GUID, Digits (or 'bindingGuid'/'bindingId')"),
        RPC_PARAM_OPT("bindingGuid", "string", "Alias for binding"),
        RPC_PARAM_OPT("bindingId", "string", "Alias for binding"),
        RPC_PARAM_OPT("rigClass", "classref", "Control Rig class/asset path; omit for the FK Control Rig"),
        RPC_PARAM_OPT("reduceKeys", "bool", "Run the engine's key reduction over the baked curves (default false)"),
        RPC_PARAM_OPT("tolerance", "number", "Key-reduction tolerance, only used when reduceKeys is true (default 0.001)")
    ))
{
    using namespace PinWrightSequencerBake;
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    FString SeqPath;
    ULevelSequence* Sequence = BakeResolveSequence(Payload, SeqPath);
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

    const FGuid BindingGuid = BakeResolveBindingGuid(Payload);
    if (!BindingGuid.IsValid() || !MovieScene->FindBinding(BindingGuid))
    {
        Ctx.SendError(ErrorCodes::ERR_BINDING_NOT_FOUND,
            TEXT("Binding GUID missing or not present in the sequence. Pass 'binding' as the possessable GUID (Digits)."));
        return true;
    }

    UClass* RigClass = UFKControlRig::StaticClass();
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

    // The bake evaluates the binding through a transient ULevelSequencePlayer, which needs a
    // world; and it needs the binding to resolve to a skeletal mesh with a skeleton, because
    // that is what ExportToAnimSequence records. Both are checked here so the failure names the
    // cause - BakeToControlRig itself returns a bare false for either.
    UWorld* World = SequencerBindingUtils::GetBindingWorld();
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_EDITOR_WORLD, TEXT("No editor world available to evaluate the sequence in."));
        return true;
    }
    USkeletalMeshComponent* SkelMeshComp = BakeResolveSkeletalMeshComponent(Sequence, BindingGuid);
    if (!SkelMeshComp || !SkelMeshComp->GetSkeletalMeshAsset() ||
        !SkelMeshComp->GetSkeletalMeshAsset()->GetSkeleton())
    {
        Ctx.SendError(ErrorCodes::ERR_BINDING_NOT_SKELETAL,
            TEXT("The binding does not resolve to a skeletal-mesh component with a skeleton in the editor world. "
                 "Bake needs a live skeletal binding to evaluate; bind the possessable to a spawned skeletal-mesh actor first."));
        return true;
    }

    const bool bReduceKeys = Ctx.GetBool(TEXT("reduceKeys"), false);
    const double Tolerance = Ctx.GetNumber(TEXT("tolerance"), 0.001);

    // Measured BEFORE the bake, because BakeToControlRig REMOVES every existing Control Rig
    // track on the binding (ControlRigSequencerEditorLibrary.cpp:1444). Reporting the count
    // afterwards would report zero and hide what the call destroyed.
    int32 ReplacedTrackCount = 0;
    for (UMovieSceneTrack* Existing :
             MovieScene->FindTracks(UMovieSceneControlRigParameterTrack::StaticClass(), BindingGuid, NAME_None))
    {
        if (Cast<UMovieSceneControlRigParameterTrack>(Existing))
        {
            ++ReplacedTrackCount;
        }
    }

    UAnimSeqExportOption* ExportOptions = NewObject<UAnimSeqExportOption>(GetTransientPackage());
    const FMovieSceneBindingProxy BindingProxy(BindingGuid, Sequence);

    // The 7-argument overload is the one every supported engine version has: bResetControls
    // arrived on 5.4 with a default of true, which is also 5.3's fixed behaviour, so omitting
    // it keeps one call site and one behaviour across 5.3-5.8.
    const bool bEngineReportedSuccess = UControlRigSequencerEditorLibrary::BakeToControlRig(
        World, Sequence, RigClass, ExportOptions, bReduceKeys, static_cast<float>(Tolerance), BindingProxy);

    UMovieSceneControlRigParameterTrack* Track = BakeFindControlRigTrack(MovieScene, BindingGuid);
    UMovieSceneControlRigParameterSection* Section = nullptr;
    if (Track)
    {
        Section = Cast<UMovieSceneControlRigParameterSection>(Track->GetSectionToKey());
        if (!Section && Track->GetAllSections().Num() > 0)
        {
            Section = Cast<UMovieSceneControlRigParameterSection>(Track->GetAllSections()[0]);
        }
    }

    if (!bEngineReportedSuccess || !Track || !Section)
    {
        Ctx.SendError(ErrorCodes::ERR_BAKE_FAILED,
            FString::Printf(TEXT("BakeToControlRig did not produce a Control Rig track on binding '%s' "
                                 "(engine reported %s). Common causes: the rig class has no backwards-solve "
                                 "event, or the sequence's playback range evaluates to nothing."),
                *BindingGuid.ToString(EGuidFormats::Digits),
                bEngineReportedSuccess ? TEXT("success") : TEXT("failure")));
        return true;
    }

    const FBakeChannelKeyStats Stats = BakeMeasureSectionKeys(Section);
    UControlRig* BakedRig = Track->GetControlRig();
    const int32 ControlCount = (BakedRig && BakedRig->GetHierarchy())
        ? BakedRig->GetHierarchy()->GetControls().Num() : 0;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("sequence"), SeqPath);
    Result->SetStringField(TEXT("binding"), BindingGuid.ToString(EGuidFormats::Digits));
    Result->SetStringField(TEXT("rigClass"), RigClass->GetName());
    Result->SetNumberField(TEXT("controlCount"), ControlCount);
    Result->SetNumberField(TEXT("channelCount"), Stats.ChannelCount);
    Result->SetNumberField(TEXT("channelsWithKeys"), Stats.ChannelsWithKeys);
    Result->SetNumberField(TEXT("keysWritten"), Stats.TotalKeys);
    Result->SetNumberField(TEXT("replacedControlRigTracks"), ReplacedTrackCount);
    Result->SetBoolField(TEXT("reduceKeys"), bReduceKeys);
    if (bReduceKeys)
    {
        // Only meaningful when reduction ran; omitted rather than reported as an unused default.
        Result->SetNumberField(TEXT("tolerance"), Tolerance);
    }
    Result->SetObjectField(TEXT("playbackRange"), BakeMakeFrameRange(
        BakeTickToDisplayFrame(MovieScene, MovieScene->GetPlaybackRange().GetLowerBoundValue()),
        BakeTickToDisplayFrame(MovieScene, MovieScene->GetPlaybackRange().GetUpperBoundValue())));

#if UE_VERSION_OLDER_THAN(5, 8, 0)
    // Placed after every read of the baked track, and before BOTH exits: the bake has already
    // abandoned its transient evaluation sequence by this point, and leaving that registration
    // in place crashes the NEXT anim-sequence creation anywhere in the session.
    BakePurgeAbandonedAnimCompilations();
#endif

    if (Stats.TotalKeys == 0)
    {
        // The engine created a track and said true, and nothing was keyed. Reporting success
        // here is exactly the silent-zero bake this verb exists to make impossible.
        Ctx.SendError(ErrorCodes::ERR_NO_KEYS_WRITTEN,
            FString::Printf(TEXT("Bake produced a Control Rig track with %d controls but wrote NO keys to any of "
                                 "its %d float channels. The binding evaluated to nothing over the playback range."),
                ControlCount, Stats.ChannelCount),
            Result);
        return true;
    }

    // The span the keys actually cover, in display frames - not the requested range.
    Result->SetObjectField(TEXT("keyRange"), BakeMakeFrameRange(
        BakeTickToDisplayFrame(MovieScene, Stats.MinTick),
        BakeTickToDisplayFrame(MovieScene, Stats.MaxTick)));

    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// sequencer.bake_control_space
// ============================================================================
#if UE_VERSION_OLDER_THAN(5, 4, 0)
#define PINWRIGHT_BAKE_CONTROL_SPACE_TYPE_PARAM \
    RPC_PARAM_REQ("targetSpaceType", "string", "Rig element type: Bone, Null, Control, or Reference")
#else
#define PINWRIGHT_BAKE_CONTROL_SPACE_TYPE_PARAM \
    RPC_PARAM_REQ("targetSpaceType", "string", "Rig element type: Bone, Null, Control, Reference, or Socket")
#endif
REGISTER_RPC_HANDLER("sequencer.bake_control_space", "sequencer",
    "Bake one Control Rig control into a target parent space over a display-frame range while "
    "preserving evaluated world motion. Requires the requested Level Sequence to "
    "be open in Sequencer and, by default, focused. Creates the control's space channel when "
    "absent and reports measured transform/space key deltas around the engine bake.",
    RPC_PARAMS(
        RPC_PARAM_OPT("sequence", "path", "Level sequence asset path (or 'path')"),
        RPC_PARAM_OPT("path", "path", "Alias for sequence"),
        RPC_PARAM_OPT("binding", "string", "Binding GUID owning the existing Control Rig track"),
        RPC_PARAM_OPT("bindingGuid", "string", "Alias for binding"),
        RPC_PARAM_OPT("bindingId", "string", "Alias for binding"),
        RPC_PARAM_REQ("control", "string", "Control name on the binding's Control Rig track"),
        RPC_PARAM_REQ("targetSpaceName", "string", "Rig element name; use WorldSpace or DefaultParent with targetSpaceType=Reference"),
        PINWRIGHT_BAKE_CONTROL_SPACE_TYPE_PARAM,
        RPC_PARAM_REQ("startFrame", "integer", "Inclusive first display-rate frame"),
        RPC_PARAM_REQ("endFrame", "integer", "Inclusive last display-rate frame; must be greater than startFrame"),
        RPC_PARAM_OPT("keyMode", "string", "allFrames (default) or keysOnly"),
        RPC_PARAM_OPT("frameIncrement", "integer", "Display-frame step for allFrames mode (default 1)"),
        RPC_PARAM_OPT("reduceKeys", "bool", "Reduce baked keys in allFrames mode (default false)"),
        RPC_PARAM_OPT("tolerance", "number", "Key-reduction tolerance (default 0.001)"),
        RPC_PARAM_OPT("allowFocusedSequenceMismatch", "bool", "Allow the engine bake to run when Sequencer is focused inside a subsequence rather than on the requested root sequence (default false)")
    ))
#undef PINWRIGHT_BAKE_CONTROL_SPACE_TYPE_PARAM
{
    using namespace PinWrightSequencerBake;
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    FString ControlString;
    FString TargetSpaceName;
    FString TargetSpaceType;
    if (!Ctx.RequireString(TEXT("control"), ControlString) ||
        !Ctx.RequireString(TEXT("targetSpaceName"), TargetSpaceName) ||
        !Ctx.RequireString(TEXT("targetSpaceType"), TargetSpaceType))
    {
        return true;
    }
    const FName ControlName(*ControlString);

    const int32 StartFrame = Ctx.GetInt(TEXT("startFrame"));
    const int32 EndFrame = Ctx.GetInt(TEXT("endFrame"));
    const int32 FrameIncrement = Ctx.GetInt(TEXT("frameIncrement"), 1);
    const bool bReduceKeys = Ctx.GetBool(TEXT("reduceKeys"), false);
    const bool bAllowFocusedSequenceMismatch =
        Ctx.GetBool(TEXT("allowFocusedSequenceMismatch"), false);
    const double Tolerance = Ctx.GetNumber(TEXT("tolerance"), 0.001);
    FString KeyMode = Ctx.GetString(TEXT("keyMode"), TEXT("allFrames"));
    KeyMode.TrimStartAndEndInline();

    if (EndFrame <= StartFrame || FrameIncrement < 1 || Tolerance < 0.0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("endFrame must be greater than startFrame, frameIncrement must be at least 1, and tolerance cannot be negative."));
        return true;
    }
    EBakingKeySettings BakingKeyMode;
    if (KeyMode.Equals(TEXT("allFrames"), ESearchCase::IgnoreCase))
    {
        BakingKeyMode = EBakingKeySettings::AllFrames;
        KeyMode = TEXT("allFrames");
    }
    else if (KeyMode.Equals(TEXT("keysOnly"), ESearchCase::IgnoreCase))
    {
        BakingKeyMode = EBakingKeySettings::KeysOnly;
        KeyMode = TEXT("keysOnly");
        if (FrameIncrement != 1 || bReduceKeys)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("frameIncrement and reduceKeys apply only when keyMode is allFrames."));
            return true;
        }
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Unknown keyMode '%s'; expected allFrames or keysOnly."), *KeyMode));
        return true;
    }

    ERigElementType TargetType = ERigElementType::None;
    if (!BakeParseRigElementType(TargetSpaceType, TargetType))
    {
#if UE_VERSION_OLDER_THAN(5, 4, 0)
        const TCHAR* ExpectedTypes = TEXT("Bone, Null, Control, or Reference");
#else
        const TCHAR* ExpectedTypes = TEXT("Bone, Null, Control, Reference, or Socket");
#endif
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Unknown targetSpaceType '%s'; expected %s."),
                *TargetSpaceType, ExpectedTypes));
        return true;
    }
    const FRigElementKey TargetSpace(FName(*TargetSpaceName), TargetType);

    FString SeqPath;
    ULevelSequence* Sequence = BakeResolveSequence(Payload, SeqPath);
    UMovieScene* MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
    if (!Sequence || !MovieScene)
    {
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_FOUND,
            FString::Printf(TEXT("Level sequence not found for path '%s'."), *SeqPath));
        return true;
    }

    const FGuid BindingGuid = BakeResolveBindingGuid(Payload);
    if (!BindingGuid.IsValid() || !MovieScene->FindBinding(BindingGuid))
    {
        Ctx.SendError(ErrorCodes::ERR_BINDING_NOT_FOUND,
            TEXT("Binding GUID missing or not present in the sequence. Pass 'binding' as the possessable GUID (Digits)."));
        return true;
    }

    UMovieSceneControlRigParameterTrack* Track = BakeFindControlRigTrack(MovieScene, BindingGuid);
    UMovieSceneControlRigParameterSection* Section = BakeFindControlRigSection(Track, ControlName);
    UControlRig* Rig = Section ? Section->GetControlRig() : nullptr;
    URigHierarchy* Hierarchy = Rig ? Rig->GetHierarchy() : nullptr;
    if (!Track || !Section)
    {
        Ctx.SendError(ErrorCodes::ERR_CONTROLRIG_TRACK_NOT_FOUND,
            TEXT("No usable Control Rig track/section exists on this binding. Call sequencer.add_controlrig_track first."));
        return true;
    }
    if (!Rig || !Hierarchy)
    {
        Ctx.SendError(ErrorCodes::ERR_RIG_STATE_INVALID,
            TEXT("The Control Rig section has no current rig or hierarchy; the space bake was refused before mutation."));
        return true;
    }

    const TSharedPtr<ISequencer> OpenSequencer = BakeFindMatchingOpenSequencer(Sequence);
    if (!OpenSequencer.IsValid() || !GEditor || !GEditor->Trans)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_OPEN,
            TEXT("Control Rig space bake requires this Level Sequence to be the current open Sequencer editor "
                 "with an active editor transaction buffer."));
        return true;
    }
    UMovieSceneSequence* FocusedSequence = OpenSequencer->GetFocusedMovieSceneSequence();
    if (FocusedSequence != Sequence && !bAllowFocusedSequenceMismatch)
    {
        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        Details->SetStringField(TEXT("requestedSequence"), Sequence->GetPathName());
        Details->SetStringField(TEXT("focusedSequence"),
            FocusedSequence ? FocusedSequence->GetPathName() : TEXT(""));
        Details->SetBoolField(TEXT("allowFocusedSequenceMismatch"), false);
        Ctx.SendError(ErrorCodes::ERR_SEQUENCE_NOT_OPEN,
            TEXT("The requested root sequence is open, but Sequencer is focused on a different sequence. "
                 "Focus the requested sequence or pass allowFocusedSequenceMismatch=true to accept the focused context."),
            Details);
        return true;
    }
    if (GEditor->Trans->IsActive())
    {
        Ctx.SendError(ErrorCodes::ERR_BAKE_FAILED,
            TEXT("Control Rig space bake cannot start while another editor transaction is active; "
                 "retry after that edit completes."));
        return true;
    }

    const FRigControlElement* ControlElement = Rig->FindControl(ControlName);
    if (!ControlElement)
    {
        Ctx.SendError(ErrorCodes::ERR_CONTROL_NOT_FOUND,
            FString::Printf(TEXT("Control '%s' does not exist on the binding's Control Rig."), *ControlString));
        return true;
    }

    // GetTransform(CurrentGlobal) reads the target, its recursive parents, and the control
    // offset. Inspect only dirty flags before any evaluation or transform read so validation
    // itself cannot trigger the ensure it guards.
    auto ValidateCurrentTransformPath = [&]() -> bool
    {
        TArray<FRigElementKey> TransformKeys =
            Hierarchy->GetParents(ControlElement->GetKey(), true);
        TransformKeys.Add(ControlElement->GetKey());
        const FRigTransformElement* InvalidElement = nullptr;
        for (const FRigElementKey& TransformKey : TransformKeys)
        {
            const FRigTransformElement* Element =
                Hierarchy->Find<FRigTransformElement>(TransformKey);
            if (Element &&
                MCP_RIG_TRANSFORM_IS_DIRTY(Element, ERigTransformType::CurrentLocal) &&
                MCP_RIG_TRANSFORM_IS_DIRTY(Element, ERigTransformType::CurrentGlobal))
            {
                InvalidElement = Element;
                break;
            }
        }
        const bool bOffsetInvalid =
            MCP_RIG_CONTROL_OFFSET_IS_DIRTY(ControlElement, ERigTransformType::CurrentLocal) &&
            MCP_RIG_CONTROL_OFFSET_IS_DIRTY(ControlElement, ERigTransformType::CurrentGlobal);
        if (!InvalidElement && !bOffsetInvalid)
        {
            return true;
        }

        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        Details->SetStringField(TEXT("control"), ControlString);
        Details->SetStringField(TEXT("invalidElement"),
            InvalidElement ? InvalidElement->GetKey().ToString() : ControlElement->GetKey().ToString());
        Details->SetStringField(TEXT("invalidTransform"),
            bOffsetInvalid ? TEXT("controlOffset") : TEXT("pose"));
        Details->SetBoolField(TEXT("currentLocalDirty"), true);
        Details->SetBoolField(TEXT("currentGlobalDirty"), true);
        Ctx.SendError(ErrorCodes::ERR_RIG_STATE_INVALID,
            TEXT("The target control transform path has both current local and global representations "
                 "marked dirty; the space bake was refused before mutation."),
            Details);
        return false;
    };
    if (!ValidateCurrentTransformPath())
    {
        return true;
    }

    const FRigElementKey WorldSpace = URigHierarchy::GetWorldSpaceReferenceKey();
    const FRigElementKey DefaultParent = URigHierarchy::GetDefaultParentKey();
    if (TargetSpace != WorldSpace && TargetSpace != DefaultParent && !Hierarchy->Contains(TargetSpace))
    {
        Ctx.SendError(ErrorCodes::ERR_TARGET_NOT_FOUND,
            FString::Printf(TEXT("Target space '%s' is not present in the Control Rig hierarchy."),
                *TargetSpace.ToString()));
        return true;
    }
    if (TargetSpace != WorldSpace && TargetSpace != DefaultParent)
    {
        FString FailureReason;
        if (!BakeCanSwitchToParent(Hierarchy, ControlElement->GetKey(), TargetSpace, FailureReason))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("Control '%s' cannot switch to '%s': %s"),
                    *ControlString, *TargetSpace.ToString(), *FailureReason));
            return true;
        }
    }

    const FFrameNumber StartTick = BakeDisplayFrameToTick(MovieScene, StartFrame);
    const FFrameNumber EndTick = BakeDisplayFrameToTick(MovieScene, EndFrame);
    const FBakeControlSpaceKeyStats Before = BakeMeasureControlSpaceKeys(
        Section, ControlName, StartTick, EndTick);
    if (Before.TransformChannelCount == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_TYPE,
            FString::Printf(TEXT("Control '%s' has no transform channels and cannot be space-baked."), *ControlString));
        return true;
    }
    const FBakeControlSpaceChannelState BeforeState =
        BakeCaptureControlSpaceChannelState(Section, ControlName);

    UPackage* Package = Sequence->GetOutermost();
    const bool bPackageWasDirty = Package && Package->IsDirty();
    const FFrameTime OriginalGlobalTime = OpenSequencer->GetGlobalTime().Time;
    FScopedTransaction Transaction(
        NSLOCTEXT("PinWright", "BakeControlRigSpace", "Bake Control Rig Space"));
    if (!Transaction.IsOutstanding())
    {
        Ctx.SendError(ErrorCodes::ERR_BAKE_FAILED,
            TEXT("Could not start the editor transaction required for an atomic Control Rig space bake."));
        return true;
    }
    PinWrightTransactionUtils::PrepareTransactionalSnapshot(Sequence);
    PinWrightTransactionUtils::PrepareTransactionalSnapshot(MovieScene);
    PinWrightTransactionUtils::PrepareTransactionalSnapshot(Track);
    PinWrightTransactionUtils::PrepareTransactionalSnapshot(Rig);
    PinWrightTransactionUtils::PrepareTransactionalSnapshot(Hierarchy);
    PinWrightTransactionUtils::PrepareTransactionalSnapshot(Section);
    const bool bCreatedSpaceChannel = !Before.bHasSpaceChannel;
    if (bCreatedSpaceChannel)
    {
        Section->AddSpaceChannel(ControlName, /*bReconstructChannel=*/true);
    }

    FRigSpacePickerBakeSettings Settings;
    Settings.TargetSpace = TargetSpace;
    Settings.Settings.StartFrame = FFrameNumber(StartFrame);
    Settings.Settings.EndFrame = FFrameNumber(EndFrame);
    Settings.Settings.BakingKeySettings = BakingKeyMode;
    Settings.Settings.FrameIncrement = FrameIncrement;
    Settings.Settings.bReduceKeys = bReduceKeys;
    Settings.Settings.Tolerance = static_cast<float>(Tolerance);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
    Settings.Settings.bTimeWarp = false;
#endif

    TArray<FName> ControlsToBake;
    ControlsToBake.Add(ControlName);
    const bool bEngineReportedSuccess = UControlRigSequencerEditorLibrary::BakeControlRigSpace(
#if UE_VERSION_OLDER_THAN(5, 4, 0)
        Sequence, Rig, ControlsToBake, Settings, ESequenceTimeUnit::DisplayRate);
#else
        Sequence, Rig, ControlsToBake, Settings, EMovieSceneTimeUnit::DisplayRate);
#endif
    const FBakeControlSpaceKeyStats Attempted = BakeMeasureControlSpaceKeys(
        Section, ControlName, StartTick, EndTick);

    const int32 AttemptedTransformKeyDelta =
        Attempted.TransformKeysInRange - Before.TransformKeysInRange;
    const int32 AttemptedSpaceKeyDelta =
        Attempted.SpaceKeysInRange - Before.SpaceKeysInRange;
    const int32 AttemptedKeysWritten =
        FMath::Max(0, AttemptedTransformKeyDelta) + FMath::Max(0, AttemptedSpaceKeyDelta);
    const bool bNothingBaked = bEngineReportedSuccess && AttemptedKeysWritten == 0;
    const bool bIncompleteKeyResult = bEngineReportedSuccess && !bNothingBaked &&
        (!Attempted.bHasSpaceChannel || Attempted.TransformKeysInRange == 0 ||
         Attempted.SpaceKeysInRange == 0);
    const bool bRollbackAttempted =
        !bEngineReportedSuccess || bNothingBaked || bIncompleteKeyResult;
    bool bRollbackSucceeded = false;
    FBakeControlSpaceKeyStats After = Attempted;
    if (bRollbackAttempted)
    {
        bRollbackSucceeded = BakeRollbackControlSpace(
            Transaction, Package, bPackageWasDirty, Section, ControlName,
            StartTick, EndTick, BeforeState, OpenSequencer, OriginalGlobalTime, After);
        ensureAlwaysMsgf(bRollbackSucceeded,
            TEXT("Control Rig space bake rollback did not restore the target control's exact key times/values and package dirtiness."));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("sequence"), SeqPath);
    Result->SetStringField(TEXT("binding"), BindingGuid.ToString(EGuidFormats::Digits));
    Result->SetStringField(TEXT("control"), ControlString);
    TSharedPtr<FJsonObject> TargetJson = MakeShared<FJsonObject>();
    TargetJson->SetStringField(TEXT("name"), TargetSpace.Name.ToString());
    TargetJson->SetStringField(TEXT("type"), TargetSpaceType);
    Result->SetObjectField(TEXT("targetSpace"), TargetJson);
    Result->SetObjectField(TEXT("range"), BakeMakeFrameRange(StartFrame, EndFrame));
    Result->SetStringField(TEXT("keyMode"), KeyMode);
    Result->SetStringField(TEXT("focusedSequence"),
        FocusedSequence ? FocusedSequence->GetPathName() : TEXT(""));
    Result->SetBoolField(TEXT("allowFocusedSequenceMismatch"),
        bAllowFocusedSequenceMismatch);
    Result->SetNumberField(TEXT("frameIncrement"), FrameIncrement);
    Result->SetBoolField(TEXT("reduceKeys"), bReduceKeys);
    if (bReduceKeys)
    {
        Result->SetNumberField(TEXT("tolerance"), Tolerance);
    }
    Result->SetBoolField(TEXT("engineReportedSuccess"), bEngineReportedSuccess);
    Result->SetBoolField(TEXT("rollbackAttempted"), bRollbackAttempted);
    Result->SetBoolField(TEXT("rollbackSucceeded"), bRollbackSucceeded);
    Result->SetBoolField(TEXT("spaceChannelCreationAttempted"), bCreatedSpaceChannel);
    Result->SetBoolField(TEXT("spaceChannelCreated"),
        !Before.bHasSpaceChannel && After.bHasSpaceChannel);
    Result->SetNumberField(TEXT("transformChannelCount"), After.TransformChannelCount);
    Result->SetNumberField(TEXT("transformKeysBefore"), Before.TransformKeysInRange);
    Result->SetNumberField(TEXT("transformKeysAfter"), After.TransformKeysInRange);
    Result->SetNumberField(TEXT("spaceKeysBefore"), Before.SpaceKeysInRange);
    Result->SetNumberField(TEXT("spaceKeysAfter"), After.SpaceKeysInRange);
    Result->SetNumberField(TEXT("attemptedTransformKeysAfter"), Attempted.TransformKeysInRange);
    Result->SetNumberField(TEXT("attemptedSpaceKeysAfter"), Attempted.SpaceKeysInRange);
    Result->SetNumberField(TEXT("attemptedTransformKeyDelta"), AttemptedTransformKeyDelta);
    Result->SetNumberField(TEXT("attemptedSpaceKeyDelta"), AttemptedSpaceKeyDelta);
    Result->SetNumberField(TEXT("keysWritten"), AttemptedKeysWritten);

    if (!bEngineReportedSuccess)
    {
        Ctx.SendError(ErrorCodes::ERR_BAKE_FAILED,
            bRollbackSucceeded
                ? TEXT("BakeControlRigSpace returned false; the target control's transform/space key times and values were rolled back.")
                : TEXT("BakeControlRigSpace returned false and the target control's transform/space key times and values could not be verified as rolled back; the package remains dirty."),
            Result);
        return true;
    }
    if (bNothingBaked)
    {
        Ctx.SendError(ErrorCodes::ERR_NOTHING_BAKED,
            bRollbackSucceeded
                ? TEXT("BakeControlRigSpace reported success but added zero transform/space keys; the target control's key times and values were rolled back.")
                : TEXT("BakeControlRigSpace reported success but added zero transform/space keys and rollback could not be verified; the package remains dirty."),
            Result);
        return true;
    }
    if (bIncompleteKeyResult)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_KEYS_WRITTEN,
            bRollbackSucceeded
                ? TEXT("BakeControlRigSpace reported success but wrote no complete transform/space key result; the target control's key times and values were rolled back.")
                : TEXT("BakeControlRigSpace reported success but wrote no complete transform/space key result and the target control's key times/values could not be verified as rolled back; the package remains dirty."),
            Result);
        return true;
    }

    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// sequencer.export_anim_sequence
// ============================================================================
REGISTER_RPC_HANDLER("sequencer.export_anim_sequence", "sequencer",
    "Bake a skeletal binding's evaluated performance out to an AnimSequence asset "
    "(USequencerToolsFunctionLibrary::ExportAnimSequence). Creates the asset at outAssetPath, or rewrites "
    "an existing one when overwrite is true. Non-destructive to the sequence: nothing is linked back into it. "
    "Reports the bone tracks and keys actually written, and fails NO_KEYS_WRITTEN when the export produced none.",
    RPC_PARAMS(
        RPC_PARAM_OPT("sequence", "path", "Level sequence asset path (or 'path')"),
        RPC_PARAM_OPT("path", "path", "Alias for sequence"),
        RPC_PARAM_OPT("binding", "string", "Possessable binding GUID, Digits (or 'bindingGuid'/'bindingId')"),
        RPC_PARAM_OPT("bindingGuid", "string", "Alias for binding"),
        RPC_PARAM_OPT("bindingId", "string", "Alias for binding"),
        RPC_PARAM_REQ("outAssetPath", "path", "Destination AnimSequence asset path, e.g. /Game/Anims/A_Baked"),
        RPC_PARAM_OPT("overwrite", "bool", "Rewrite an existing asset at outAssetPath (default false)"),
        RPC_PARAM_OPT("startFrame", "integer", "First display-rate frame to export; requires endFrame. UE 5.5+ only"),
        RPC_PARAM_OPT("endFrame", "integer", "Last display-rate frame to export; requires startFrame. UE 5.5+ only"),
        RPC_PARAM_OPT("save", "bool", "Write the asset to disk (default true)")
    ))
{
    using namespace PinWrightSequencerBake;
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    // ---- outAssetPath is checked FIRST, above the sequence and binding resolution ----
    //
    // WHY IT IS FIRST, AND WHY IT MUST STAY FIRST. This path is the argument handed to
    // CreatePackage further down, and CreatePackage logs at Fatal for a name containing "//"
    // (UObjectGlobals.cpp:1094-1096) and for one that resolves to empty (:1118). Fatal is not
    // compiled out in any configuration, so such a call does not fail: it ends the editor
    // PROCESS and every unsaved package in it, and the `if (!Package)` below can never fire
    // because nothing after the call is reached (board
    // B-createpackage-unvalidated-paths-plugin-wide; the mechanism was measured on
    // B-foliage-add-type-name-with-slash-kills-the-editor).
    //
    // The ordering is load-bearing for the regression test, the same way it is in FoliageHandler
    // and LandscapeHandler: Tests/Sequencer/TestSequencerExportAnimSequencePathSafety.cpp drives
    // a path-shaped outAssetPath together with a `sequence` that is a well-formed long package
    // name naming NO asset, so a build with this block deleted or moved back below is refused
    // SEQUENCE_NOT_FOUND - which is ABOVE the CreatePackage - and the test goes red while the
    // process lives. Do not move this below BakeResolveSequence.
    //
    // BOTH clauses are the guard, and the second is not decoration. NormalizeAssetPath sets
    // bIsValid only after its own FPackageName::IsValidLongPackageName call, so today it already
    // carries the invariant - but that is an undocumented postcondition of a shared, exported
    // Utils helper with several callers. The precondition CreatePackage actually needs is
    // asserted here, in the file that makes the call.
    FString RequestedAssetPath;
    if (!Ctx.RequireString(TEXT("outAssetPath"), RequestedAssetPath))
    {
        return true;
    }
    const FNormalizedAssetPath Normalized = NormalizeAssetPath(RequestedAssetPath);
    FText PackageNameReason;
    if (!Normalized.bIsValid ||
        !FPackageName::IsValidLongPackageName(Normalized.Path, /*bIncludeReadOnlyRoots=*/true,
                                              &PackageNameReason))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PATH,
            FString::Printf(TEXT("outAssetPath '%s' is not a valid asset path: %s"),
                *RequestedAssetPath,
                Normalized.bIsValid ? *PackageNameReason.ToString() : *Normalized.ErrorMessage));
        return true;
    }
    const FString PackageName = Normalized.Path;
    const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);

    FString SeqPath;
    ULevelSequence* Sequence = BakeResolveSequence(Payload, SeqPath);
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

    const FGuid BindingGuid = BakeResolveBindingGuid(Payload);
    if (!BindingGuid.IsValid() || !MovieScene->FindBinding(BindingGuid))
    {
        Ctx.SendError(ErrorCodes::ERR_BINDING_NOT_FOUND,
            TEXT("Binding GUID missing or not present in the sequence. Pass 'binding' as the possessable GUID (Digits)."));
        return true;
    }

    UWorld* World = SequencerBindingUtils::GetBindingWorld();
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_EDITOR_WORLD, TEXT("No editor world available to evaluate the sequence in."));
        return true;
    }
    USkeletalMeshComponent* SkelMeshComp = BakeResolveSkeletalMeshComponent(Sequence, BindingGuid);
    USkeleton* Skeleton = (SkelMeshComp && SkelMeshComp->GetSkeletalMeshAsset())
        ? SkelMeshComp->GetSkeletalMeshAsset()->GetSkeleton() : nullptr;
    if (!Skeleton)
    {
        Ctx.SendError(ErrorCodes::ERR_BINDING_NOT_SKELETAL,
            TEXT("The binding does not resolve to a skeletal-mesh component with a skeleton in the editor world. "
                 "Export needs a live skeletal binding to evaluate; bind the possessable to a spawned skeletal-mesh actor first."));
        return true;
    }

    // Frame range. Both ends or neither: a half-specified range would silently fall back to the
    // whole playback range and produce an asset that does not match what was asked for.
    const bool bHasStart = Payload.IsValid() && Payload->HasField(TEXT("startFrame"));
    const bool bHasEnd = Payload.IsValid() && Payload->HasField(TEXT("endFrame"));
    if (bHasStart != bHasEnd)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("startFrame and endFrame must be supplied together; omit both to export the whole playback range."));
        return true;
    }
    const double StartFrame = Ctx.GetNumber(TEXT("startFrame"), 0.0);
    const double EndFrame = Ctx.GetNumber(TEXT("endFrame"), 0.0);
    if (bHasStart && EndFrame <= StartFrame)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("endFrame (%g) must be greater than startFrame (%g)."), EndFrame, StartFrame));
        return true;
    }
#if UE_VERSION_OLDER_THAN(5, 5, 0)
    if (bHasStart)
    {
        // UAnimSeqExportOption gained bUseCustomTimeRange / CustomStartFrame / CustomEndFrame /
        // CustomDisplayRate in 5.5. On 5.3 and 5.4 the exporter always covers the sequence's
        // playback range, and there is no way to narrow it - so a requested range is refused
        // rather than silently widened to the whole sequence.
        Ctx.SendUnsupportedEngineVersion(TEXT("5.5"), TEXT("a custom export frame range (startFrame/endFrame)"));
        return true;
    }
#endif

    const bool bOverwrite = Ctx.GetBool(TEXT("overwrite"), false);
    const bool bSave = Ctx.GetBool(TEXT("save"), true);

    // Resolve or create the destination asset. ExportAnimSequence takes an existing
    // UAnimSequence* rather than a path, so ownership of create/overwrite is this handler's.
    UAnimSequence* AnimSequence = nullptr;
    bool bCreatedAsset = false;
    if (ResolveAsset(PackageName).bExists)
    {
        if (!bOverwrite)
        {
            Ctx.SendError(ErrorCodes::ERR_ASSET_ALREADY_EXISTS,
                FString::Printf(TEXT("An asset already exists at '%s'; pass overwrite=true to rewrite it."), *PackageName));
            return true;
        }
        AnimSequence = Cast<UAnimSequence>(
            ResolveAsset(PackageName, /*bLoadObject=*/true).Object);
        if (!AnimSequence)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ASSET_TYPE,
                FString::Printf(TEXT("'%s' exists but is not an AnimSequence; refusing to overwrite it."), *PackageName));
            return true;
        }
    }
    else
    {
        UPackage* Package = CreatePackage(*PackageName);
        if (!Package)
        {
            Ctx.SendError(ErrorCodes::ERR_ASSET_CREATION_FAILED,
                FString::Printf(TEXT("Could not create package '%s'."), *PackageName));
            return true;
        }
        UAnimSequenceFactory* Factory = NewObject<UAnimSequenceFactory>();
        Factory->TargetSkeleton = Skeleton;
        AnimSequence = Cast<UAnimSequence>(Factory->FactoryCreateNew(
            UAnimSequence::StaticClass(), Package, FName(*AssetName),
            RF_Public | RF_Standalone, nullptr, GWarn));
        if (!AnimSequence)
        {
            Ctx.SendError(ErrorCodes::ERR_ASSET_CREATION_FAILED,
                FString::Printf(TEXT("Could not create AnimSequence '%s'."), *PackageName));
            return true;
        }
        bCreatedAsset = true;
        FAssetRegistryModule::AssetCreated(AnimSequence);
    }

    UAnimSeqExportOption* ExportOptions = NewObject<UAnimSeqExportOption>(GetTransientPackage());
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    if (bHasStart)
    {
        ExportOptions->bUseCustomTimeRange = true;
        ExportOptions->CustomStartFrame = FFrameNumber(static_cast<int32>(FMath::RoundToInt(StartFrame)));
        ExportOptions->CustomEndFrame = FFrameNumber(static_cast<int32>(FMath::RoundToInt(EndFrame)));
        // The custom frames are read in THIS rate, so it has to be the sequence's own display
        // rate or the exported span silently shifts.
        ExportOptions->CustomDisplayRate = MovieScene->GetDisplayRate();
    }
#endif

    // SetSkeleton (inside ExportAnimSequence) calls TryCancelAsyncTasks, which on 5.6/5.7 holds the
    // compressed-data READ scope across FRequestOwner::Cancel while the compression task itself needs
    // the WRITE scope - deadlock; 5.8 moved the task map behind its own CacheTasksMutex. Compression
    // is only still in flight under a saturated task pool, so it hangs the full suite and never an
    // isolated run. This waits via FinishCompilation WITHOUT the mutex, leaving nothing to cancel.
    AnimSequence->WaitOnExistingCompression(/*bWantResults*/ true);

    const FMovieSceneBindingProxy BindingProxy(BindingGuid, Sequence);
    // bCreateLink=false: linking would ADD a skeletal animation track to the sequence, which is
    // a mutation of the thing being exported. Export stays read-only with respect to the sequence.
    const bool bEngineReportedSuccess = USequencerToolsFunctionLibrary::ExportAnimSequence(
        World, Sequence, AnimSequence, ExportOptions, BindingProxy, /*bCreateLink*/ false);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("sequence"), SeqPath);
    Result->SetStringField(TEXT("binding"), BindingGuid.ToString(EGuidFormats::Digits));
    Result->SetStringField(TEXT("assetPath"), PackageName);
    Result->SetBoolField(TEXT("created"), bCreatedAsset);
    Result->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
    if (bHasStart)
    {
        // The range as REQUESTED, named separately from what the asset actually holds.
        Result->SetObjectField(TEXT("requestedRange"), BakeMakeFrameRange(StartFrame, EndFrame));
    }

    if (!bEngineReportedSuccess)
    {
        Ctx.SendError(ErrorCodes::ERR_EXPORT_FAILED,
            FString::Printf(TEXT("ExportAnimSequence failed for binding '%s'.%s"),
                *BindingGuid.ToString(EGuidFormats::Digits),
                bCreatedAsset
                    ? TEXT(" An empty AnimSequence was created at assetPath and is left in memory for inspection; delete it if unwanted.")
                    : TEXT("")),
            Result);
        return true;
    }

    const FBakeAnimModelStats Stats = BakeMeasureAnimSequence(AnimSequence);
    if (!Stats.bReadable)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_DATA_INVALID,
            TEXT("The exported AnimSequence has no readable data model, so what was written cannot be verified."),
            Result);
        return true;
    }

    Result->SetNumberField(TEXT("boneTrackCount"), Stats.BoneTrackCount);
    Result->SetNumberField(TEXT("boneKeysWritten"), Stats.TotalBoneKeys);
    Result->SetNumberField(TEXT("numberOfFrames"), Stats.NumberOfFrames);
    Result->SetNumberField(TEXT("playLengthSeconds"), Stats.PlayLengthSeconds);
    Result->SetStringField(TEXT("frameRate"),
        FString::Printf(TEXT("%d/%d"), Stats.FrameRate.Numerator, Stats.FrameRate.Denominator));

    if (Stats.BoneTrackCount == 0 || Stats.TotalBoneKeys == 0)
    {
        // ExportAnimSequence returned true and the asset holds nothing playable.
        Ctx.SendError(ErrorCodes::ERR_NO_KEYS_WRITTEN,
            FString::Printf(TEXT("Export wrote %d bone tracks and %d keys to '%s'. The binding evaluated to "
                                 "nothing over the exported range."),
                Stats.BoneTrackCount, Stats.TotalBoneKeys, *PackageName),
            Result);
        return true;
    }

    bool bSavedToDisk = false;
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    if (bSave)
    {
        bSavedToDisk = SaveAssetToDiskReportingPresence(
            AnimSequence, /*bForce*/ true, /*OutPackageName*/ nullptr, /*OutSizeBytes*/ nullptr, &SaveState);
        AddAssetSaveReport(Result, /*bSaveRequested*/ true, bSavedToDisk, SaveState);
    }
    else
    {
        AnimSequence->MarkPackageDirty();
        AddAssetSaveReport(Result, /*bSaveRequested*/ false, false);
    }

    Ctx.SendSuccess(Result);
    return true;
}
