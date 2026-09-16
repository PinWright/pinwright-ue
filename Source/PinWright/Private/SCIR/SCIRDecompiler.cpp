// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "SCIR/SCIRDecompiler.h"


#include "Handlers/Asset/SoundCueDumpBuilder.h"
#include "Handlers/Asset/SoundWaveDumpBuilder.h"
#include "IrCore/IrTextUtils.h"

#include "Curves/RichCurve.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Attenuation.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundNodeAttenuation.h"
#include "Sound/SoundNodeBranch.h"
#include "Sound/SoundNodeModulator.h"
#include "Sound/SoundNodeRandom.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundWave.h"

#include <initializer_list>

namespace
{
    FString SCIRNumber(float Value)
    {
        return FString::SanitizeFloat(Value);
    }

    FString SCIRBool(bool bValue)
    {
        return bValue ? TEXT("true") : TEXT("false");
    }

    FString SCIRIndent(int32 Depth)
    {
        return FString::ChrN(Depth * 2, TEXT(' '));
    }

    FString PathToken(const UObject* Object)
    {
        return Object ? FIrTextUtils::FormatNameToken(Object->GetPathName()) : FString();
    }

    FString NodeTypeToken(const USoundNode* Node)
    {
        if (!Node)
        {
            return TEXT("null");
        }

        FString Name = Node->GetClass()->GetName();
        Name.RemoveFromStart(TEXT("SoundNode"));
        return FIrTextUtils::CamelToSnakeIdentifier(Name);
    }

    FString NodePositionSuffix(const USoundNode* Node)
    {
        int32 X = 0;
        int32 Y = 0;

        if (const UEdGraphNode* GraphNode = Node ? Node->GetGraphNode() : nullptr)
        {
            X = GraphNode->NodePosX;
            Y = GraphNode->NodePosY;
        }

        return FIrTextUtils::FormatPositionSuffix(X, Y);
    }

    FString EnumValueToken(const UEnum* Enum, int64 Value)
    {
        return Enum ? FIrTextUtils::Quote(Enum->GetNameStringByValue(Value)) : FIrTextUtils::Quote(FString());
    }

    FString VectorLiteral(const FVector& Value)
    {
        return FString::Printf(
            TEXT("[%s, %s, %s]"),
            *SCIRNumber(Value.X),
            *SCIRNumber(Value.Y),
            *SCIRNumber(Value.Z));
    }

    bool HasRuntimeFloatCurve(const FRuntimeFloatCurve& Curve)
    {
        const FRichCurve* RichCurve = Curve.GetRichCurveConst();
        return Curve.ExternalCurve || (RichCurve && RichCurve->GetNumKeys() > 0);
    }

    void AddExplicitProperties(TSet<FName>& ExplicitProperties, std::initializer_list<const TCHAR*> PropertyNames)
    {
        for (const TCHAR* PropertyName : PropertyNames)
        {
            ExplicitProperties.Add(FName(PropertyName));
        }
    }

    void AppendWavePlayerFields(USoundNodeWavePlayer* WavePlayer, TSet<FName>& ExplicitProperties, TArray<FString>& Fields)
    {
        const FString WavePath = SoundCueDumpBuilder::ResolveSoundNodeWavePlayerPath(WavePlayer);
        if (!WavePath.IsEmpty())
        {
            Fields.Add(FString::Printf(TEXT("wave: %s"), *FIrTextUtils::FormatNameToken(WavePath)));
        }

        if (USoundWave* Wave = WavePlayer->GetSoundWave())
        {
            Fields.Add(FString::Printf(TEXT("duration: %s"), *SCIRNumber(Wave->Duration)));
            Fields.Add(FString::Printf(TEXT("numChannels: %d"), Wave->NumChannels));
            Fields.Add(FString::Printf(TEXT("sampleRate: %d"), SoundWaveDumpBuilder::GetRawSampleRate(Wave)));
        }

        AddExplicitProperties(ExplicitProperties, { TEXT("SoundWaveAssetPtr") });
    }

    void AppendAttenuationFields(USoundNodeAttenuation* AttenuationNode, TSet<FName>& ExplicitProperties, TArray<FString>& Fields)
    {
        if (AttenuationNode->AttenuationSettings)
        {
            Fields.Add(FString::Printf(TEXT("attenuation: %s"), *PathToken(AttenuationNode->AttenuationSettings)));
        }

        Fields.Add(FString::Printf(TEXT("overrideAttenuation: %s"), *SCIRBool(AttenuationNode->bOverrideAttenuation != 0)));

        const FSoundAttenuationSettings* Settings = AttenuationNode->GetAttenuationSettingsToApply();
        if (Settings)
        {
            Fields.Add(FString::Printf(TEXT("spatialize: %s"), *SCIRBool(Settings->bSpatialize != 0)));
            Fields.Add(FString::Printf(TEXT("spatializationAlgorithm: %s"),
                *EnumValueToken(StaticEnum<ESoundSpatializationAlgorithm>(),
                    static_cast<int64>(Settings->SpatializationAlgorithm.GetValue()))));
            Fields.Add(FString::Printf(TEXT("distanceAlgorithm: %s"),
                *EnumValueToken(StaticEnum<EAttenuationDistanceModel>(), static_cast<int64>(Settings->DistanceAlgorithm))));
            Fields.Add(FString::Printf(TEXT("attenuationShape: %s"),
                *EnumValueToken(StaticEnum<EAttenuationShape::Type>(),
                    static_cast<int64>(Settings->AttenuationShape.GetValue()))));
            Fields.Add(FString::Printf(TEXT("shapeExtents: %s"), *VectorLiteral(Settings->AttenuationShapeExtents)));
            Fields.Add(FString::Printf(TEXT("falloffDistance: %s"), *SCIRNumber(Settings->FalloffDistance)));
            Fields.Add(FString::Printf(TEXT("dBAttenuationAtMax: %s"), *SCIRNumber(Settings->dBAttenuationAtMax)));
            Fields.Add(FString::Printf(TEXT("hasCustomAttenuationCurve: %s"),
                *SCIRBool(HasRuntimeFloatCurve(Settings->CustomAttenuationCurve))));
        }

        AddExplicitProperties(ExplicitProperties, {
            TEXT("AttenuationSettings"),
            TEXT("AttenuationOverrides"),
            TEXT("bOverrideAttenuation")
        });
    }

    void AppendBranchFields(USoundNodeBranch* Branch, TSet<FName>& ExplicitProperties, TArray<FString>& Fields)
    {
        Fields.Add(FString::Printf(TEXT("BoolParameterName: %s"), *FIrTextUtils::Quote(Branch->BoolParameterName.ToString())));
        AddExplicitProperties(ExplicitProperties, { TEXT("BoolParameterName") });
    }

    void AppendModulatorFields(USoundNodeModulator* Modulator, TSet<FName>& ExplicitProperties, TArray<FString>& Fields)
    {
        Fields.Add(FString::Printf(TEXT("PitchMin: %s"), *SCIRNumber(Modulator->PitchMin)));
        Fields.Add(FString::Printf(TEXT("PitchMax: %s"), *SCIRNumber(Modulator->PitchMax)));
        Fields.Add(FString::Printf(TEXT("VolumeMin: %s"), *SCIRNumber(Modulator->VolumeMin)));
        Fields.Add(FString::Printf(TEXT("VolumeMax: %s"), *SCIRNumber(Modulator->VolumeMax)));
        AddExplicitProperties(ExplicitProperties, {
            TEXT("PitchMin"),
            TEXT("PitchMax"),
            TEXT("VolumeMin"),
            TEXT("VolumeMax")
        });
    }

    void AppendReflectedNodeFields(USoundNode* Node, const TSet<FName>& ExplicitProperties, TArray<FString>& OutFields)
    {
        UObject* DefaultNode = Node ? Node->GetClass()->GetDefaultObject() : nullptr;
        if (!Node || !DefaultNode)
        {
            return;
        }

        FReflectedFieldEmitOptions Options;
        Options.FieldSeparator = TEXT(": ");
        Options.bEmitArraysAsBracketList = true;

        FIrTextUtils::AppendReflectedFields(
            Node->GetClass(),
            Node,
            DefaultNode,
            Node,
            ExplicitProperties,
            [](const FStructProperty*)
            {
                return false;
            },
            [](FName PropertyName)
            {
                return PropertyName == FName(TEXT("ChildNodes"))
                    || PropertyName == FName(TEXT("GraphNode"));
            },
            Options,
            OutFields);
    }

    TArray<FString> BuildNodeFields(USoundNode* Node)
    {
        TArray<FString> Fields;
        TSet<FName> ExplicitProperties;

        if (USoundNodeWavePlayer* WavePlayer = Cast<USoundNodeWavePlayer>(Node))
        {
            AppendWavePlayerFields(WavePlayer, ExplicitProperties, Fields);
        }
        else if (USoundNodeAttenuation* Attenuation = Cast<USoundNodeAttenuation>(Node))
        {
            AppendAttenuationFields(Attenuation, ExplicitProperties, Fields);
        }
        else if (USoundNodeBranch* Branch = Cast<USoundNodeBranch>(Node))
        {
            AppendBranchFields(Branch, ExplicitProperties, Fields);
        }
        else if (USoundNodeModulator* Modulator = Cast<USoundNodeModulator>(Node))
        {
            AppendModulatorFields(Modulator, ExplicitProperties, Fields);
        }

        AppendReflectedNodeFields(Node, ExplicitProperties, Fields);
        return Fields;
    }

    // Per-child arrays are the one part of a cue graph whose defects are invisible in the tree
    // itself: the node, its type, its children and its wiring all read as correct while the node
    // does not work. Reported here because decompile_sound_cue is the readback callers verify a
    // freshly authored cue with (B-cue-random-node-weights-zero-always-picks-first).
    void AppendChildArrayWarnings(USoundNode* Node, TArray<FString>& Warnings)
    {
        TArray<SoundCueDumpBuilder::FSoundNodeChildArray> ChildArrays;
        SoundCueDumpBuilder::GetSoundNodeChildArrays(Node, ChildArrays);

        for (const SoundCueDumpBuilder::FSoundNodeChildArray& ChildArray : ChildArrays)
        {
            if (ChildArray.Num != Node->ChildNodes.Num())
            {
                // Only USoundNode::InsertChildNode keeps these in step. A short one is a live
                // fault, not a cosmetic one: USoundNodeMixer::ParseNodes indexes InputVolume by
                // child index with no bounds guard.
                Warnings.Add(FString::Printf(
                    TEXT("%s '%s': %s has %d entr%s for %d child node%s — the per-child array is out of step with ChildNodes."),
                    *Node->GetClass()->GetName(),
                    *Node->GetName(),
                    *ChildArray.PropertyName.ToString(),
                    ChildArray.Num,
                    ChildArray.Num == 1 ? TEXT("y") : TEXT("ies"),
                    Node->ChildNodes.Num(),
                    Node->ChildNodes.Num() == 1 ? TEXT("") : TEXT("s")));
            }
        }

        if (const USoundNodeRandom* Random = Cast<USoundNodeRandom>(Node))
        {
            float WeightSum = 0.0f;
            for (float Weight : Random->Weights)
            {
                WeightSum += Weight;
            }

            // USoundNodeRandom::ChooseNodeIndex multiplies FRand() by this sum and then walks the
            // weights looking for Choice < RunningSum. At a zero sum that comparison is never
            // true, the loop cannot break, and the function returns its NodeIndex initialiser —
            // child 0, on every single trigger. A zero weight sum has no legitimate use.
            if (Node->ChildNodes.Num() > 0 && WeightSum <= 0.0f)
            {
                Warnings.Add(FString::Printf(
                    TEXT("SoundNodeRandom '%s': Weights sum to 0 — the node always selects child 0 and never randomises. Set one weight per child (1.0 each matches the editor's default)."),
                    *Node->GetName()));
            }
        }
    }

    int32 ReachableChildCount(const USoundNode* Node)
    {
        if (!Node)
        {
            return 0;
        }

        return FMath::Min(Node->ChildNodes.Num(), Node->GetMaxChildNodes());
    }

    void EmitNode(
        USoundNode* Node,
        const FString& Role,
        int32 Depth,
        TSet<USoundNode*>& Visited,
        TArray<FString>& Lines,
        TArray<FString>& Warnings)
    {
        if (!Node)
        {
            Lines.Add(FString::Printf(TEXT("%s%s null ()"), *SCIRIndent(Depth), *Role));
            Warnings.Add(FString::Printf(TEXT("Encountered null %s node."), *Role));
            return;
        }

        const FString Header = FString::Printf(
            TEXT("%s%s %s %s%s %s"),
            *SCIRIndent(Depth),
            *Role,
            *NodeTypeToken(Node),
            *FIrTextUtils::FormatNameToken(Node->GetName()),
            *NodePositionSuffix(Node),
            *FIrTextUtils::FormatFieldList(BuildNodeFields(Node)));

        if (Visited.Contains(Node))
        {
            Lines.Add(Header + TEXT(" # already emitted"));
            Warnings.Add(FString::Printf(TEXT("SoundCue graph references node '%s' more than once."), *Node->GetName()));
            return;
        }

        Visited.Add(Node);
        AppendChildArrayWarnings(Node, Warnings);

        const int32 ChildCount = ReachableChildCount(Node);
        if (ChildCount == 0)
        {
            Lines.Add(Header);
            return;
        }

        Lines.Add(Header + TEXT(" {"));
        for (int32 Index = 0; Index < ChildCount; ++Index)
        {
            EmitNode(Node->ChildNodes[Index].Get(), TEXT("child"), Depth + 1, Visited, Lines, Warnings);
        }
        Lines.Add(SCIRIndent(Depth) + TEXT("}"));
    }

    void AppendCueFields(USoundCue* Cue, TArray<FString>& Lines)
    {
        if (Cue->AttenuationSettings)
        {
            Lines.Add(FString::Printf(TEXT("  attenuation %s"), *PathToken(Cue->AttenuationSettings)));
        }

        if (!Cue->ConcurrencySet.IsEmpty())
        {
            TArray<FString> ConcurrencyPaths;
            for (const TObjectPtr<USoundConcurrency>& Concurrency : Cue->ConcurrencySet)
            {
                if (Concurrency)
                {
                    ConcurrencyPaths.Add(PathToken(Concurrency.Get()));
                }
            }
            ConcurrencyPaths.Sort();
            Lines.Add(FString::Printf(TEXT("  concurrency [%s]"), *FString::Join(ConcurrencyPaths, TEXT(", "))));
        }

        if (Cue->SoundClassObject)
        {
            Lines.Add(FString::Printf(TEXT("  sound_class %s"), *PathToken(Cue->SoundClassObject)));
        }

        Lines.Add(FString::Printf(TEXT("  volume %s"), *SCIRNumber(Cue->VolumeMultiplier)));
        Lines.Add(FString::Printf(TEXT("  pitch %s"), *SCIRNumber(Cue->PitchMultiplier)));
    }
}

FSCIRResult SCIRDecompiler::BuildSoundCueIrText(USoundCue* Cue)
{
    if (!Cue)
    {
        return FSCIRResult::MakeError(TEXT("SoundCue is null."));
    }

    FSCIRResult Result;
    Result.bSuccess = true;

    TArray<FString> Lines;
    Lines.Add(FString::Printf(TEXT("sound_cue %s {"), *FIrTextUtils::FormatNameToken(Cue->GetPathName())));
    AppendCueFields(Cue, Lines);

    TSet<USoundNode*> Visited;
    if (Cue->FirstNode)
    {
        EmitNode(Cue->FirstNode, TEXT("root"), 1, Visited, Lines, Result.Warnings);
    }
    else
    {
        // Name the verb that fixes it: this warning used to leave a caller with a complete,
        // correct-looking tree and no way to discover that audio.authoring.set_cue_root is what
        // roots it (E-cue-graph-verbs-cannot-set-firstnode).
        Result.Warnings.Add(TEXT("SoundCue has no FirstNode — it cannot play. Root it with audio.authoring.set_cue_root."));
    }

    for (const TObjectPtr<USoundNode>& Entry : Cue->AllNodes)
    {
        USoundNode* Node = Entry.Get();
        if (!Node || Visited.Contains(Node))
        {
            continue;
        }
        EmitNode(Node, TEXT("orphan"), 1, Visited, Lines, Result.Warnings);
    }

    Lines.Add(TEXT("}"));
    Result.Text = FString::Join(Lines, TEXT("\n")) + TEXT("\n");
    return Result;
}
