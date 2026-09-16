// Copyright (c) 2026 Alexander Penkin. MIT License.

// AnimationAuthoringHelpers.cpp
//
// Implementation of the cross-cluster animation.authoring helpers. Behaviour is
// preserved exactly from the former monolithic AnimationAuthoringHandler.cpp.

#include "AnimationAuthoringHelpers.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimTypes.h"  // GetTriggerTimeOffsetForType, EAnimEventTriggerOffsets
#include "Animation/BlendSpace.h"  // UBlendSpace, FBlendParameter
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Dom/JsonObject.h"
#include "Utils/JsonUtils.h"
#include "Utils/AssetUtils.h"
#include "Utils/PathUtils.h"  // NormalizeContentAssetPath, used by the four loaders below
#include "PinWrightSubsystem.h"  // LogPinWrightSubsystem

namespace AnimationAuthoringHelpers
{
    void LinkNotifyAtTime(FAnimNotifyEvent& Event, UAnimSequenceBase* Asset, float Time)
    {
        Event.Link(Asset, Time);
        Event.TriggerTimeOffset = GetTriggerTimeOffsetForType(EAnimEventTriggerOffsets::OffsetBefore);
    }

    void EnsureNotifyTrack(UAnimSequenceBase* Asset, FAnimNotifyEvent& Event)
    {
        if (!Asset)
        {
            return;
        }

        // Clamp an out-of-range index exactly the way the engine does before growing tracks:
        // UAnimSequenceBase::RefreshCacheData resets any index < 0 or > 20 to 0
        // (AnimSequenceBase.cpp, "Don't create lots of extra tracks if we are way off"). Without
        // the upper bound a client-supplied index (e.g. a fuzzed trackIndex=1000000) would drive
        // the grow loop below to allocate ~a million FAnimNotifyTrack entries -- an OOM/hang --
        // instead of clamping to a single track.
        if (Event.TrackIndex < 0 || Event.TrackIndex > 20)
        {
            Event.TrackIndex = 0;
        }

        // Grow the notify-track list until the event's track exists. RefreshCacheData()
        // otherwise ensures on a track index that does not exist (AnimSequenceBase.cpp).
        while (!Asset->AnimNotifyTracks.IsValidIndex(Event.TrackIndex))
        {
            Asset->AnimNotifyTracks.Add(FAnimNotifyTrack(
                FName(*FString::FromInt(Asset->AnimNotifyTracks.Num() + 1)), FLinearColor::White));
        }
    }

    UClass* ResolveConcreteNotifyClassOrNull(const FString& FullClassName)
    {
        UClass* NotifyUClass = FindFirstObject<UClass>(*FullClassName, EFindFirstObjectOptions::ExactClass);
        if (NotifyUClass && NotifyUClass->HasAnyClassFlags(CLASS_Abstract))
        {
            NotifyUClass = nullptr;
        }
        return NotifyUClass;
    }

    USkeleton* LoadSkeletonFromPathAnim(const FString& SkeletonPath)
    {
        FString NormalizedPath = NormalizeContentAssetPath(SkeletonPath);
        return Cast<USkeleton>(StaticLoadObject(USkeleton::StaticClass(), nullptr, *NormalizedPath));
    }

    USkeletalMesh* LoadSkeletalMeshFromPathAnim(const FString& MeshPath)
    {
        FString NormalizedPath = NormalizeContentAssetPath(MeshPath);
        return Cast<USkeletalMesh>(StaticLoadObject(USkeletalMesh::StaticClass(), nullptr, *NormalizedPath));
    }

    UAnimSequence* LoadAnimSequenceFromPath(const FString& AnimPath)
    {
        FString NormalizedPath = NormalizeContentAssetPath(AnimPath);
        return Cast<UAnimSequence>(StaticLoadObject(UAnimSequence::StaticClass(), nullptr, *NormalizedPath));
    }

    UAnimSequenceBase* LoadAnimSequenceBaseFromPath(const FString& AnimPath)
    {
        FString NormalizedPath = NormalizeContentAssetPath(AnimPath);
        return Cast<UAnimSequenceBase>(StaticLoadObject(UAnimSequenceBase::StaticClass(), nullptr, *NormalizedPath));
    }

    FString AdditiveAnimTypeToString(EAdditiveAnimationType Type)
    {
        if (const UEnum* Enum = StaticEnum<EAdditiveAnimationType>())
        {
            return Enum->GetNameStringByValue(static_cast<int64>(Type));
        }
        return FString();
    }

    // Single source of the logicType name<->ETransitionLogicType vocabulary. Both
    // TransitionLogicTypeToString and ParseTransitionLogicType draw on this table so
    // the dump builder's output and the authoring handler's input stay in lockstep.
    static const TMap<FString, ETransitionLogicType::Type>& TransitionLogicTypeNameMap()
    {
        static const TMap<FString, ETransitionLogicType::Type> Map = {
            {TEXT("StandardBlend"),   ETransitionLogicType::TLT_StandardBlend},
            {TEXT("Inertialization"), ETransitionLogicType::TLT_Inertialization},
            {TEXT("Custom"),          ETransitionLogicType::TLT_Custom},
        };
        return Map;
    }

    FString TransitionLogicTypeToString(ETransitionLogicType::Type LogicType)
    {
        for (const auto& Pair : TransitionLogicTypeNameMap())
        {
            if (Pair.Value == LogicType)
            {
                return Pair.Key;
            }
        }
        return FString::Printf(TEXT("%d"), static_cast<int32>(LogicType));
    }

    bool ParseTransitionLogicType(const FString& Text, ETransitionLogicType::Type& Out)
    {
        for (const auto& Pair : TransitionLogicTypeNameMap())
        {
            if (Text.Equals(Pair.Key, ESearchCase::IgnoreCase))
            {
                Out = Pair.Value;
                return true;
            }
        }
        return false;
    }

    bool ParseAlphaBlendOption(const FString& Text, EAlphaBlendOption& Out)
    {
        static const TMap<FString, EAlphaBlendOption> Map = {
            {TEXT("Linear"), EAlphaBlendOption::Linear},
            {TEXT("Cubic"), EAlphaBlendOption::Cubic},
            {TEXT("HermiteCubic"), EAlphaBlendOption::HermiteCubic},
            {TEXT("Sinusoidal"), EAlphaBlendOption::Sinusoidal},
            {TEXT("QuadraticInOut"), EAlphaBlendOption::QuadraticInOut},
            {TEXT("CubicInOut"), EAlphaBlendOption::CubicInOut},
            {TEXT("QuarticInOut"), EAlphaBlendOption::QuarticInOut},
            {TEXT("QuinticInOut"), EAlphaBlendOption::QuinticInOut},
            {TEXT("CircularIn"), EAlphaBlendOption::CircularIn},
            {TEXT("CircularOut"), EAlphaBlendOption::CircularOut},
            {TEXT("CircularInOut"), EAlphaBlendOption::CircularInOut},
            {TEXT("ExpIn"), EAlphaBlendOption::ExpIn},
            {TEXT("ExpOut"), EAlphaBlendOption::ExpOut},
            {TEXT("ExpInOut"), EAlphaBlendOption::ExpInOut},
            {TEXT("Custom"), EAlphaBlendOption::Custom},
        };
        for (const auto& Pair : Map)
        {
            if (Text.Equals(Pair.Key, ESearchCase::IgnoreCase))
            {
                Out = Pair.Value;
                return true;
            }
        }
        return false;
    }

    // UE 5.7+ Fix: Do not save immediately to avoid modal dialogs.
    // Mark dirty and notify registry instead. The response contract is explicit
    // about that choice: this helper reports Deferred, never a durable write.
    bool SaveAnimAsset(UObject* Asset, bool bShouldSave, EAssetSaveState& OutSaveState)
    {
        OutSaveState = EAssetSaveState::NotRequested;
        if (!bShouldSave)
        {
            return true;
        }

        if (!Asset)
        {
            OutSaveState = EAssetSaveState::NotPersistable;
            return true;
        }

        // Mark dirty and notify asset registry - do NOT save to disk. Keep this
        // on the shared helper so animation authoring and the other mark-dirty
        // paths cannot drift in their registry/dirty-package behavior.
        McpSafeAssetSave(Asset);
        OutSaveState = EAssetSaveState::Deferred;
        return true;
    }

    FVector GetVectorFromJsonAnim(const TSharedPtr<FJsonObject>& Obj)
    {
        if (!Obj.IsValid())
        {
            return FVector::ZeroVector;
        }
        return FVector(
            GetJsonNumberField(Obj, TEXT("x"), 0.0),
            GetJsonNumberField(Obj, TEXT("y"), 0.0),
            GetJsonNumberField(Obj, TEXT("z"), 0.0)
        );
    }

    FRotator GetRotatorFromJsonAnim(const TSharedPtr<FJsonObject>& Obj)
    {
        if (!Obj.IsValid())
        {
            return FRotator::ZeroRotator;
        }
        // Support both Euler (pitch/yaw/roll) and quaternion (x/y/z/w)
        if (Obj->HasField(TEXT("pitch")) || Obj->HasField(TEXT("yaw")) || Obj->HasField(TEXT("roll")))
        {
            return FRotator(
                GetJsonNumberField(Obj, TEXT("pitch"), 0.0),
                GetJsonNumberField(Obj, TEXT("yaw"), 0.0),
                GetJsonNumberField(Obj, TEXT("roll"), 0.0)
            );
        }
        else if (Obj->HasField(TEXT("w")))
        {
            FQuat Quat(
                GetJsonNumberField(Obj, TEXT("x"), 0.0),
                GetJsonNumberField(Obj, TEXT("y"), 0.0),
                GetJsonNumberField(Obj, TEXT("z"), 0.0),
                GetJsonNumberField(Obj, TEXT("w"), 1.0)
            );
            return Quat.Rotator();
        }
        return FRotator::ZeroRotator;
    }

    FBlendParameter* GetBlendParametersForWrite(UBlendSpace* BlendSpace)
    {
        if (!BlendSpace)
        {
            return nullptr;
        }

        FProperty* BlendParamsProp =
            UBlendSpace::StaticClass()->FindPropertyByName(TEXT("BlendParameters"));
        if (!BlendParamsProp)
        {
            UE_LOG(LogPinWrightSubsystem, Warning,
                   TEXT("GetBlendParametersForWrite: BlendParameters property not "
                        "found via reflection on %s"),
                   *BlendSpace->GetName());
            return nullptr;
        }

        return BlendParamsProp->ContainerPtrToValuePtr<FBlendParameter>(BlendSpace);
    }
}
