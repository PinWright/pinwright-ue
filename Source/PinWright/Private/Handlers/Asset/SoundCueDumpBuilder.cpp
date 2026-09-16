// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/SoundCueDumpBuilder.h"

#include "Utils/JsonBuilders.h"
#include "Utils/PathUtils.h"
#include "Utils/PropertyUtils.h"

#include "Dom/JsonValue.h"
#include "Misc/Paths.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundWave.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Utils/JsonSidecarRegistry.h"

namespace
{
    TSharedPtr<FJsonObject> BuildNodeJson(USoundNode* Node)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (!Node)
        {
            return Obj;
        }

        UClass* NodeClass = Node->GetClass();
        Obj->SetStringField(TEXT("name"), Node->GetName());
        Obj->SetStringField(TEXT("path"), Node->GetPathName());
        Obj->SetStringField(TEXT("className"), NodeClass->GetName());
        Obj->SetStringField(TEXT("class"), NodeClass->GetPathName());

        UObject* Baseline = NodeClass->GetDefaultObject();
        Obj->SetObjectField(TEXT("properties"), BuildSparsePropertyDiffJson(Node, Baseline));

        TArray<TSharedPtr<FJsonValue>> Edges;
        Edges.Reserve(Node->ChildNodes.Num());
        for (const TObjectPtr<USoundNode>& Child : Node->ChildNodes)
        {
            if (USoundNode* ChildPtr = Child.Get())
            {
                Edges.Add(MakeShared<FJsonValueString>(ChildPtr->GetPathName()));
            }
            else
            {
                Edges.Add(MakeShared<FJsonValueNull>());
            }
        }
        Obj->SetArrayField(TEXT("edges"), Edges);

        if (const USoundNodeWavePlayer* WavePlayer = Cast<USoundNodeWavePlayer>(Node))
        {
            Obj->SetStringField(TEXT("soundWave"), SoundCueDumpBuilder::ResolveSoundNodeWavePlayerPath(WavePlayer));
        }

        // Per-child arrays, paired with the child count they must match. The CDO diff above
        // cannot carry this: a Mixer whose InputVolume was never grown still equals its CDO
        // (an empty array) and so prints nothing at all, while a Random node's [0.0, 0.0]
        // prints as an ordinary property with no hint that a zero weight sum means the node
        // can never choose anything but child 0
        // (B-cue-random-node-weights-zero-always-picks-first). Emitted only for node types
        // that own such an array, so every other node's JSON is unchanged.
        TArray<SoundCueDumpBuilder::FSoundNodeChildArray> ChildArrays;
        SoundCueDumpBuilder::GetSoundNodeChildArrays(Node, ChildArrays);
        if (ChildArrays.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> ChildArrayValues;
            for (const SoundCueDumpBuilder::FSoundNodeChildArray& ChildArray : ChildArrays)
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("property"), ChildArray.PropertyName.ToString());
                Entry->SetNumberField(TEXT("count"), ChildArray.Num);
                Entry->SetNumberField(TEXT("childCount"), Node->ChildNodes.Num());
                Entry->SetBoolField(TEXT("matchesChildCount"), ChildArray.Num == Node->ChildNodes.Num());
                if (ChildArray.bNumeric)
                {
                    TArray<TSharedPtr<FJsonValue>> Values;
                    Values.Reserve(ChildArray.Values.Num());
                    for (double Value : ChildArray.Values)
                    {
                        Values.Add(MakeShared<FJsonValueNumber>(Value));
                    }
                    Entry->SetArrayField(TEXT("values"), Values);
                }
                ChildArrayValues.Add(MakeShared<FJsonValueObject>(Entry));
            }
            Obj->SetArrayField(TEXT("childValues"), ChildArrayValues);
        }

        return Obj;
    }

    void RecursiveCollect(USoundNode* Node, TSet<USoundNode*>& Visited, TArray<USoundNode*>& Ordered)
    {
        if (!Node || Visited.Contains(Node))
        {
            return;
        }
        Visited.Add(Node);
        Ordered.Add(Node);

        const int32 MaxChildNodes = Node->GetMaxChildNodes();
        const int32 ChildCount = FMath::Min(Node->ChildNodes.Num(), MaxChildNodes);
        for (int32 Index = 0; Index < ChildCount; ++Index)
        {
            RecursiveCollect(Node->ChildNodes[Index].Get(), Visited, Ordered);
        }
    }
}

namespace SoundCueDumpBuilder
{
    FString NormalizeSoundCuePath(const FString& Path)
    {
        FString Candidate = Path;
        FPaths::NormalizeFilename(Candidate);
        Candidate.ReplaceInline(TEXT("\\"), TEXT("/"));
        while (Candidate.Contains(TEXT("//")))
        {
            Candidate.ReplaceInline(TEXT("//"), TEXT("/"));
        }
        if (!Candidate.StartsWith(TEXT("/")))
        {
            Candidate = TEXT("/") + Candidate;
        }

        if (Candidate.StartsWith(TEXT("/Content/")))
        {
            Candidate = TEXT("/Game/") + Candidate.Mid(9);
        }
        else if (Candidate == TEXT("/Content"))
        {
            Candidate = TEXT("/Game");
        }

        FString Normalized = SanitizeProjectRelativePath(Candidate);
        while (Normalized.EndsWith(TEXT("/")))
        {
            Normalized.LeftChopInline(1);
        }

        return Normalized;
    }

    USoundCue* LoadSoundCueFromPath(const FString& CuePath, FString* OutNormalizedPath)
    {
        const FString NormalizedPath = NormalizeSoundCuePath(CuePath);
        if (OutNormalizedPath)
        {
            *OutNormalizedPath = NormalizedPath;
        }
        return NormalizedPath.IsEmpty()
            ? nullptr
            : Cast<USoundCue>(StaticLoadObject(USoundCue::StaticClass(), nullptr, *NormalizedPath));
    }

    void GetSoundNodeChildArrays(const USoundNode* Node, TArray<FSoundNodeChildArray>& OutArrays)
    {
        OutArrays.Reset();
        if (!Node)
        {
            return;
        }

        // CPF_EditFixedSize is the engine's own marker for "only InsertChildNode may resize
        // this": USoundNodeRandom::Weights, USoundNodeMixer / USoundNodeConcatenator::InputVolume,
        // USoundNodeGroupControl::GroupSizes and USoundNodeDistanceCrossFade::CrossFadeInput all
        // carry it. Discovering them by that flag rather than by a hardcoded class list means a
        // node type added later is covered without touching this code.
        for (TFieldIterator<FArrayProperty> It(Node->GetClass()); It; ++It)
        {
            const FArrayProperty* ArrayProperty = *It;
            if (!ArrayProperty->HasAnyPropertyFlags(CPF_EditFixedSize))
            {
                continue;
            }

            FScriptArrayHelper_InContainer Helper(ArrayProperty, Node);

            FSoundNodeChildArray Entry;
            Entry.PropertyName = ArrayProperty->GetFName();
            Entry.Num = Helper.Num();

            if (const FNumericProperty* Numeric = CastField<FNumericProperty>(ArrayProperty->Inner))
            {
                Entry.bNumeric = true;
                Entry.Values.Reserve(Entry.Num);
                for (int32 Index = 0; Index < Entry.Num; ++Index)
                {
                    const void* ElementPtr = Helper.GetRawPtr(Index);
                    Entry.Values.Add(Numeric->IsFloatingPoint()
                        ? Numeric->GetFloatingPointPropertyValue(ElementPtr)
                        : static_cast<double>(Numeric->GetSignedIntPropertyValue(ElementPtr)));
                }
            }

            OutArrays.Add(MoveTemp(Entry));
        }
    }

    FString ResolveSoundNodeWavePlayerPath(const USoundNodeWavePlayer* WavePlayer)
    {
        if (!WavePlayer)
        {
            return FString();
        }

        if (USoundWave* Live = WavePlayer->GetSoundWave())
        {
            return Live->GetPathName();
        }

        static const FSoftObjectProperty* AssetProp = CastField<FSoftObjectProperty>(
            USoundNodeWavePlayer::StaticClass()->FindPropertyByName(TEXT("SoundWaveAssetPtr")));
        if (AssetProp)
        {
            const FSoftObjectPtr* SoftPtr = AssetProp->GetPropertyValuePtr_InContainer(WavePlayer);
            if (SoftPtr)
            {
                return SoftPtr->ToSoftObjectPath().ToString();
            }
        }

        return FString();
    }

    TSharedPtr<FJsonObject> BuildSoundCueJson(const USoundCue* Cue)
    {
        if (!Cue)
        {
            return nullptr;
        }

        TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetStringField(TEXT("assetKind"), TEXT("SoundCue"));
        Root->SetStringField(TEXT("path"), Cue->GetPathName());
        Root->SetStringField(TEXT("firstNode"), Cue->FirstNode ? Cue->FirstNode->GetPathName() : FString());

        // Cue-level settings the set_cue_concurrency / set_cue_attenuation setters write. Without
        // these, describe_sound_cue (and the shared sound_cue.json sidecar built by this same
        // function) returned byte-identical output whether a cue was throttled by a concurrency
        // group or not, so the overlay's named inspect-after-mutate readback could not verify its
        // own siblings' writes and callers had to fall back to decompile_sound_cue (SCIR). Mirrors
        // the cue-level block SCIRDecompiler::AppendCueFields emits
        // (E-describe-sound-cue-omits-cue-level-fields). Emitted with a stable schema (empty string
        // for an unset reference, empty array for no concurrency) so callers can read unconditionally.
        Root->SetStringField(TEXT("attenuation"), JsonBuilders::GetObjectPathSafe(Cue->AttenuationSettings));
        Root->SetStringField(TEXT("soundClass"), JsonBuilders::GetObjectPathSafe(Cue->SoundClassObject));

        TArray<FString> ConcurrencyPaths;
        for (const TObjectPtr<USoundConcurrency>& Concurrency : Cue->ConcurrencySet)
        {
            if (Concurrency)
            {
                ConcurrencyPaths.Add(Concurrency->GetPathName());
            }
        }
        ConcurrencyPaths.Sort();
        Root->SetArrayField(TEXT("concurrency"), JsonBuilders::BuildStringArrayJson(ConcurrencyPaths));

        Root->SetNumberField(TEXT("volume"), Cue->VolumeMultiplier);
        Root->SetNumberField(TEXT("pitch"), Cue->PitchMultiplier);

        TSet<USoundNode*> Visited;
        TArray<USoundNode*> Ordered;
        RecursiveCollect(Cue->FirstNode, Visited, Ordered);

        TArray<TSharedPtr<FJsonValue>> Nodes;
        Nodes.Reserve(Ordered.Num());
        for (USoundNode* Node : Ordered)
        {
            Nodes.Add(MakeShared<FJsonValueObject>(BuildNodeJson(Node)));
        }

        for (const TObjectPtr<USoundNode>& Entry : Cue->AllNodes)
        {
            USoundNode* Node = Entry.Get();
            if (!Node || Visited.Contains(Node))
            {
                continue;
            }
            Visited.Add(Node);
            TSharedPtr<FJsonObject> Block = BuildNodeJson(Node);
            if (Block.IsValid())
            {
                Block->SetBoolField(TEXT("orphaned"), true);
                Nodes.Add(MakeShared<FJsonValueObject>(Block));
            }
        }

        Root->SetArrayField(TEXT("nodes"), Nodes);
        return Root;
    }
}

namespace
{
    UClass* GetSoundCueSidecarClass()
    {
        return USoundCue::StaticClass();
    }

    TSharedPtr<FJsonObject> BuildSoundCueSidecar(UObject* Asset)
    {
        return SoundCueDumpBuilder::BuildSoundCueJson(Cast<USoundCue>(Asset));
    }
}

REGISTER_DUMP_JSON_SIDECAR(TEXT("sound_cue"), DumpFileNames::SoundCue,
    &GetSoundCueSidecarClass, &BuildSoundCueSidecar,
    nullptr, nullptr, nullptr, 100);
