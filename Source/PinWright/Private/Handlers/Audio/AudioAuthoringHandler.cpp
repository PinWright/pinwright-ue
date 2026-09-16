// Copyright (c) 2026 Alexander Penkin. MIT License.

// AudioAuthoringHandler.cpp - Migrated from PinWright_AudioAuthoringHandlers.cpp
// Phase 11: Complete Audio System Authoring
//
// Implements Sound Cues, MetaSounds, Sound Classes & Mixes,
// Attenuation & Spatialization, Dialogue System, and Audio Effects.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Audio/AudioPackagePathGuard.h"
#include "Handlers/Asset/MetaSoundDumpBuilder.h"
#include "Handlers/Asset/SoundCueDumpBuilder.h"
#include "Handlers/Asset/SoundWaveDumpBuilder.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "EditorAssetLibrary.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "Compat/EngineVersionCompat.h"

// Audio Core
#include "Sound/SoundCue.h"
#include "Sound/SoundWave.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundNodeMixer.h"
#include "Sound/SoundNodeRandom.h"
#include "Sound/SoundNodeModulator.h"
#include "Sound/SoundNodeLooping.h"
#include "Sound/SoundNodeAttenuation.h"
#include "Sound/SoundNodeConcatenator.h"
#include "Sound/SoundNodeDelay.h"
#include "Sound/SoundNodeSwitch.h"
#include "Sound/SoundNodeBranch.h"

// EdGraph node paired with each USoundNode — needed to keep a node's input pins
// in sync with its ChildNodes before USoundCue::LinkGraphNodesFromSoundNodes()
// (see SyncSoundCueGraphNodeInputPins / connect_cue_nodes).
#include "SoundCueGraph/SoundCueGraphNode.h"

// Audio Factories
#include "Factories/SoundCueFactoryNew.h"
#include "Factories/SoundClassFactory.h"
#include "Factories/SoundMixFactory.h"
#include "Factories/SoundAttenuationFactory.h"

// Dialogue
#if __has_include("Sound/DialogueVoice.h")
#include "Sound/DialogueVoice.h"
#include "Sound/DialogueWave.h"
#define MCP_HAS_DIALOGUE 1
#else
#define MCP_HAS_DIALOGUE 0
#endif

// Dialogue Factories
#if __has_include("Factories/DialogueVoiceFactory.h")
#include "Factories/DialogueVoiceFactory.h"
#include "Factories/DialogueWaveFactory.h"
#define MCP_HAS_DIALOGUE_FACTORY 1
#else
#define MCP_HAS_DIALOGUE_FACTORY 0
#endif

// Audio Effects
#if __has_include("Sound/SoundEffectSource.h")
#include "Sound/SoundEffectSource.h"
#define MCP_HAS_SOURCE_EFFECT 1
#else
#define MCP_HAS_SOURCE_EFFECT 0
#endif

#if __has_include("Sound/SoundSubmixSend.h")
#include "Sound/SoundSubmixSend.h"
#endif

#if __has_include("Sound/SoundSubmix.h")
#include "Sound/SoundSubmix.h"
#define MCP_HAS_SUBMIX 1
#else
#define MCP_HAS_SUBMIX 0
#endif

#if __has_include("AudioMixerTypes.h")
#include "AudioMixerTypes.h"
#endif

// Source Effect Chain
#if __has_include("SourceEffects/SourceEffectChain.h")
#include "SourceEffects/SourceEffectChain.h"
#elif __has_include("Sound/SoundEffectPreset.h")
#include "Sound/SoundEffectPreset.h"
#endif

// Reverb Effects
#if __has_include("Sound/ReverbEffect.h")
#include "Sound/ReverbEffect.h"
#define MCP_HAS_REVERB_EFFECT 1
#else
#define MCP_HAS_REVERB_EFFECT 0
#endif

// MetaSound support (UE 5.0+)
#if __has_include("MetasoundSource.h")
#if __has_include("MetasoundDocumentInterface.h")
#include "MetasoundDocumentInterface.h"
#define MCP_HAS_METASOUND_DOCUMENT_INTERFACE 1
#else
#define MCP_HAS_METASOUND_DOCUMENT_INTERFACE 0
#endif
#include "MetasoundSource.h"
#include "Handlers/Audio/MetaSound/MetaSoundLiteralFromTypeName.h"
// Shared value-param -> literal conversion and its wire errors (set_metasound_default).
#include "Handlers/Audio/MetaSound/MetaSoundLiteralParams.h"
#define MCP_HAS_METASOUND 1
#else
#define MCP_HAS_METASOUND_DOCUMENT_INTERFACE 0
#define MCP_HAS_METASOUND 0
#endif

#if __has_include("Metasound.h")
#include "Metasound.h"
#endif

#if __has_include("MetasoundBuilderSubsystem.h")
#include "MetasoundBuilderSubsystem.h"
#endif

// MetaSound Frontend Document Builder (UE 5.3+)
#if __has_include("MetasoundFrontendDocumentBuilder.h")
#include "MetasoundFrontendDocumentBuilder.h"
#include "MetasoundFrontendDocument.h"
// Shared 5.6 paged-graphs compat helpers (builder ctor macro, input-default access).
#include "Handlers/Audio/MetaSound/MetaSoundPathUtils.h"
#define MCP_HAS_METASOUND_FRONTEND 1
#else
#define MCP_HAS_METASOUND_FRONTEND 0
#endif

// MetaSound Factory (Editor)
#if __has_include("MetasoundFactory.h")
#include "MetasoundFactory.h"
#define MCP_HAS_METASOUND_FACTORY 1
#else
#define MCP_HAS_METASOUND_FACTORY 0
#endif

// MetaSound Editor Subsystem
#if __has_include("MetasoundEditorSubsystem.h")
#include "MetasoundEditorSubsystem.h"
#endif

// =========================================================================
// Shared static helpers
// =========================================================================

// Save an audio asset to disk for real and report whether the .uasset landed on disk.
// Despite the legacy name this no longer merely marks the package dirty: a save:true
// audio create that only dirtied the package reported existsAfter:true while nothing
// reached disk, so the asset vanished on a cold editor restart / git reset
// (B-audio-create-save-no-disk-write). SoundClass/SoundMix/ReverbEffect/SoundCue/
// Attenuation/Wave are non-Blueprint/non-SCS, so the bulkdata-corruption vector that
// forced the deferred mark-dirty on Blueprint edits (B-bp-saved-state-corruption-mcp-edits)
// does not apply. Registers the asset (so SaveLoadedAsset flushes a never-before-seen
// package) then routes through SaveAssetToDiskReportingPresence — the same forced-save +
// IFileManager::FileSize disk-probe helper, gated by ShouldTreatAssetSaveAsSuccess, that
// the niagara.create_* / metasound create fixes adopted. Returns true only when the
// .uasset is actually on disk; a not-requested save stays a no-op returning true.
// OutSaveState retains the measured reason so handlers that promise persistence can
// return a typed failure instead of collapsing every non-durable outcome to false.
static bool SaveAudioAsset(UObject* Asset, bool bShouldSave,
    EAssetSaveState* OutSaveState = nullptr)
{
    if (OutSaveState)
    {
        *OutSaveState = bShouldSave ? EAssetSaveState::Failed : EAssetSaveState::NotRequested;
    }
    if (!bShouldSave)
    {
        return true;
    }
    if (!Asset)
    {
        return false;
    }

    FAssetRegistryModule::AssetCreated(Asset);
    return SaveAssetToDiskReportingPresence(
        Asset, /*bForce=*/true, nullptr, nullptr, OutSaveState);
}

// Reparent a USoundClass while maintaining BOTH sides of the parent/child link, the way the
// SoundClass editor does. The engine stores the hierarchy in two synced UPROPERTYs —
// USoundClass::ParentClass (child -> parent) and USoundClass::ChildClasses (parent -> children).
// A raw `SoundClass->ParentClass = NewParent;` write only updates the child side, leaving every
// parent's ChildClasses empty/stale (SoundClass editor graph shows disconnected roots, GetDesc()
// reports Children:0). USoundClass::SetParentClass removes the child from the OLD parent's
// ChildClasses and sets the child pointer, but — matching the engine — does NOT add to the NEW
// parent; that side is mirrored here from the editor's PostEditChangeProperty(ParentClass) path,
// guarded by RecurseCheckChild to reject cycles. Returns false (and leaves the hierarchy
// untouched) if the requested parent would create a loop. bSave saves every touched class so
// the parent-side link persists — SaveAudioAsset saves only the single passed asset. When
// non-null, OutChildSavedToDisk receives the child's on-disk save result (false unless the
// full reparent path runs and force-saves it) so a caller that already routes the child
// through this helper need not re-save it.
static bool SetSoundClassParentMaintainingChildren(USoundClass* SoundClass, USoundClass* NewParent, bool bSave, bool* OutChildSavedToDisk = nullptr)
{
    if (OutChildSavedToDisk)
    {
        *OutChildSavedToDisk = false;
    }
    if (!SoundClass)
    {
        return false;
    }

    USoundClass* OldParent = SoundClass->ParentClass;
    if (OldParent == NewParent)
    {
        return true;
    }

    // Cycle guard: a class cannot be parented under itself or any of its own descendants.
    if (NewParent && (NewParent == SoundClass || SoundClass->RecurseCheckChild(NewParent)))
    {
        return false;
    }

    // Drops the child from OldParent->ChildClasses (with OldParent->Modify()) and sets the child
    // pointer + SoundClass->Modify(); does NOT touch the new parent's ChildClasses.
    SoundClass->SetParentClass(NewParent);

    // Mirror PostEditChangeProperty(ParentClass): add to the new parent's ChildClasses if absent.
    if (NewParent && !NewParent->ChildClasses.Contains(SoundClass))
    {
        NewParent->Modify();
        NewParent->ChildClasses.Add(SoundClass);
    }

    // Persist every class whose package was just dirtied (old parent, new parent, child) so the
    // parent-side link is not lost. SaveAudioAsset writes each to disk; no-ops on null or !bSave.
    SaveAudioAsset(OldParent, bSave);
    SaveAudioAsset(NewParent, bSave);
    const bool bChildSaved = SaveAudioAsset(SoundClass, bSave);
    if (OutChildSavedToDisk)
    {
        *OutChildSavedToDisk = bChildSaved;
    }

    return true;
}

static USoundWave* LoadSoundWaveFromPath(const FString& SoundPath)
{
    FString NormalizedPath = NormalizeContentAssetPath(SoundPath);
    return Cast<USoundWave>(StaticLoadObject(USoundWave::StaticClass(), nullptr, *NormalizedPath));
}

static USoundCue* LoadSoundCueFromPath(const FString& CuePath)
{
    return SoundCueDumpBuilder::LoadSoundCueFromPath(CuePath);
}

static USoundClass* LoadSoundClassFromPath(const FString& ClassPath)
{
    FString NormalizedPath = NormalizeContentAssetPath(ClassPath);
    return Cast<USoundClass>(StaticLoadObject(USoundClass::StaticClass(), nullptr, *NormalizedPath));
}

static USoundAttenuation* LoadSoundAttenuationFromPath(const FString& AttenPath)
{
    FString NormalizedPath = NormalizeContentAssetPath(AttenPath);
    return Cast<USoundAttenuation>(StaticLoadObject(USoundAttenuation::StaticClass(), nullptr, *NormalizedPath));
}

static USoundMix* LoadSoundMixFromPath(const FString& MixPath)
{
    FString NormalizedPath = NormalizeContentAssetPath(MixPath);
    return Cast<USoundMix>(StaticLoadObject(USoundMix::StaticClass(), nullptr, *NormalizedPath));
}

#if MCP_HAS_DIALOGUE
static UDialogueVoice* LoadDialogueVoiceFromPath(const FString& VoicePath)
{
    FString NormalizedPath = NormalizeContentAssetPath(VoicePath);
    return Cast<UDialogueVoice>(StaticLoadObject(UDialogueVoice::StaticClass(), nullptr, *NormalizedPath));
}

static UDialogueWave* LoadDialogueWaveFromPath(const FString& WavePath)
{
    FString NormalizedPath = NormalizeContentAssetPath(WavePath);
    return Cast<UDialogueWave>(StaticLoadObject(UDialogueWave::StaticClass(), nullptr, *NormalizedPath));
}
#endif

#if MCP_HAS_SUBMIX
// Resolves a USoundSubmix asset by path (NormalizeContentAssetPath aware). Mirrors LoadSoundClassFromPath shape.
static USoundSubmix* LoadSoundSubmixFromPath(const FString& SubmixPath)
{
    FString NormalizedPath = NormalizeContentAssetPath(SubmixPath);
    return Cast<USoundSubmix>(StaticLoadObject(USoundSubmix::StaticClass(), nullptr, *NormalizedPath));
}

// Submix counterpart of SetSoundClassParentMaintainingChildren. USoundSubmix::SetParentSubmix
// already maintains BOTH sides (drops the child from the old parent's ChildSubmixes, AddUnique()s
// it to the new parent's, with Modify() on every touched asset) — unlike SetParentClass, which
// skips the new-parent side. The only extra work needed is persistence: SaveAudioAsset dirties a
// single asset, so dirty old parent, new parent, and child here so the parent-side ChildSubmixes
// link survives a save. NewParent may be null to clear the parent.
static void SetSubmixParentMaintainingChildren(USoundSubmix* Submix, USoundSubmixBase* NewParent, bool bSave)
{
    USoundSubmixBase* OldParent = Submix->ParentSubmix;
    Submix->SetParentSubmix(NewParent);
    SaveAudioAsset(OldParent, bSave);
    SaveAudioAsset(NewParent, bSave);
    SaveAudioAsset(Submix, bSave);
}

// Shared implementation behind create_sound_submix.
// On failure, sends the appropriate error via Ctx and returns nullptr. ParentPath empty = no parent wired.
static USoundSubmix* CreateSoundSubmixAsset(const FString& Name, const FString& Path, const FString& ParentPath, bool bSave, FHandlerContext& Ctx)
{
    FString PackagePath;
    if (!PinWrightAudioPackagePath::ComposeAudioAssetPackagePathOrRefuse(Ctx, Path, Name, PackagePath))
    {
        return nullptr;
    }

    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(ErrorCodes::ERR_PACKAGE_ERROR, TEXT("Failed to create package"));
        return nullptr;
    }

    USoundSubmix* NewSubmix = NewObject<USoundSubmix>(Package, FName(*Name), RF_Public | RF_Standalone);
    if (!NewSubmix)
    {
        Ctx.SendError(ErrorCodes::ERR_CREATE_FAILED, TEXT("Failed to create submix"));
        return nullptr;
    }

    // Silently skip on load fail to match create_sound_class style (see line 1144-1152) —
    // creation still succeeds; caller can wire parent later via set_submix_parent.
    USoundSubmix* Parent = ParentPath.IsEmpty() ? nullptr : LoadSoundSubmixFromPath(ParentPath);
    if (Parent)
    {
        // Maintains both sides and saves Parent + NewSubmix (old parent is null on a fresh asset).
        SetSubmixParentMaintainingChildren(NewSubmix, Parent, bSave);
    }
    else
    {
        SaveAudioAsset(NewSubmix, bSave);
    }
    return NewSubmix;
}
#endif


// Keep a SoundCue node's EdGraph input pins in sync with its ChildNodes count.
//
// USoundCue::LinkGraphNodesFromSoundNodes() (FSoundCueAudioEditor) walks every
// node and asserts check(InputPins.Num() == SoundNode->ChildNodes.Num()) before
// wiring pins. A handler that mutates ChildNodes directly (grow + assign) leaves
// the paired USoundCueGraphNode with fewer input pins than children, so the very
// next relink fires that fatal check() and hard-crashes the editor
// (B-connect-cue-nodes-crash). The engine's own child-adding path
// (USoundNode::InsertChildNode) creates each paired pin via
// USoundCueGraphNode::CreateInputPin(); mirror that here by topping up input pins
// until they match the (already grown) child count so the invariant holds. Cast
// (not CastChecked) so a missing/foreign graph node degrades to a no-op instead
// of introducing a new crash.
static void SyncSoundCueGraphNodeInputPins(USoundNode* Node)
{
    if (!Node)
    {
        return;
    }
    USoundCueGraphNode* GraphNode = Cast<USoundCueGraphNode>(Node->GetGraphNode());
    if (!GraphNode)
    {
        return;
    }
    while (GraphNode->GetInputCount() < Node->ChildNodes.Num())
    {
        GraphNode->CreateInputPin();
    }
}

// Attach ChildNode into ParentNode's ChildNodes[ChildIndex], growing the slot list through
// the node's own USoundNode::InsertChildNode() instead of SetNum() + assign.
//
// Several USoundNode subclasses keep per-child state in a SEPARATE array that only
// InsertChildNode() maintains: USoundNodeRandom::Weights (plus HasBeenUsed),
// USoundNodeMixer::InputVolume, USoundNodeConcatenator::InputVolume,
// USoundNodeGroupControl::GroupSizes. Growing ChildNodes directly left every one of those
// behind, and the consequences are not cosmetic:
//   * Random - Weights stayed zero-filled (PostLoad's FixWeightsArray only resizes, and it
//     grows with AddZeroed, so a reload cements it). USoundNodeRandom::ChooseNodeIndex sums
//     the weights to 0, its selection loop can then never break, and the node returns its
//     NodeIndex initialiser: child 0 plays on every trigger while every readback reports a
//     correct multi-child random node (B-cue-random-node-weights-zero-always-picks-first).
//   * Mixer / Concatenator - InputVolume stayed EMPTY and nothing ever repairs it, so
//     ParseNodes' unguarded InputVolume[ChildNodeIndex] reads past the end at playback.
// The engine's editor-side insert contract fills each of those slots with its own default
// (Weights / InputVolume 1.0f, GroupSizes 1) and creates the paired EdGraph input pin, so
// routing every child attachment through it also keeps node types this file has never heard
// of consistent. Returns false with OutError when the parent cannot hold ChildIndex.
static bool AttachSoundCueChildNode(USoundNode* ParentNode, USoundNode* ChildNode, int32 ChildIndex, FString& OutError)
{
    const int32 MaxChildNodes = ParentNode->GetMaxChildNodes();
    if (ChildIndex >= MaxChildNodes)
    {
        OutError = FString::Printf(
            TEXT("childIndex %d is out of range for %s, which accepts at most %d child node(s)"),
            ChildIndex, *ParentNode->GetClass()->GetName(), MaxChildNodes);
        return false;
    }

    // Resync the parallel arrays to the CURRENT children before inserting. A node whose slots
    // were grown by the old direct-push path can hold e.g. 2 children and an EMPTY InputVolume,
    // and USoundNodeMixer::InsertChildNode would then run InputVolume.InsertUninitialized(2) on
    // a zero-length array — an out-of-bounds insert that fails a check() and kills the editor.
    // USoundNode::SetChildNodes is the engine's own fix-up for exactly this: each subclass
    // override tops its array up to ChildNodes.Num() with the same defaults its insert path
    // uses. Passing a copy of the current children makes it a pure resync.
    {
        TArray<USoundNode*> CurrentChildren;
        CurrentChildren.Reserve(ParentNode->ChildNodes.Num());
        for (const TObjectPtr<USoundNode>& Child : ParentNode->ChildNodes)
        {
            CurrentChildren.Add(Child.Get());
        }
        ParentNode->SetChildNodes(CurrentChildren);
    }

    while (ParentNode->ChildNodes.Num() <= ChildIndex)
    {
        const int32 NumBefore = ParentNode->ChildNodes.Num();
        ParentNode->InsertChildNode(NumBefore);
        if (ParentNode->ChildNodes.Num() == NumBefore)
        {
            // A subclass override declined the slot. Report it instead of spinning forever.
            OutError = FString::Printf(TEXT("%s refused a child slot at index %d"),
                *ParentNode->GetClass()->GetName(), NumBefore);
            return false;
        }
    }

    ParentNode->ChildNodes[ChildIndex] = ChildNode;

    // InsertChildNode creates one input pin per slot it adds, so this only tops up nodes whose
    // children predate this path (cues authored before the fix, or grown by a raw property
    // write). It keeps the B-connect-cue-nodes-crash guard in place for those.
    SyncSoundCueGraphNodeInputPins(ParentNode);
    return true;
}

// Resolve a cue node by the bare object name add_cue_node returns (e.g. SoundNodeRandom_0).
static USoundNode* FindSoundCueNodeByName(USoundCue* Cue, const FString& NodeId)
{
    for (USoundNode* Node : Cue->AllNodes)
    {
        if (Node && Node->GetName() == NodeId)
        {
            return Node;
        }
    }
    return nullptr;
}

// Reserved node ids naming the cue's graph root. The root is a USoundCueGraphNode_Root, not a
// USoundNode, so it never appears in AllNodes: callers who tried to connect to it got
// SOURCE_NODE_NOT_FOUND and no verb in the namespace could write FirstNode at all
// (E-cue-graph-verbs-cannot-set-firstnode).
static bool IsSoundCueRootNodeId(const FString& NodeId)
{
    return NodeId.Equals(TEXT("Output"), ESearchCase::IgnoreCase)
        || NodeId.Equals(TEXT("Root"), ESearchCase::IgnoreCase);
}

// Root the cue at RootNode and relink the EdGraph Root pin to it. Writing FirstNode alone
// leaves the Sound Cue editor's Output node unconnected, and the next graph edit recompiles
// FirstNode back to null from that unlinked graph, so the relink belongs to the write.
static void SetSoundCueRootNode(USoundCue* Cue, USoundNode* RootNode)
{
    Cue->FirstNode = RootNode;
    Cue->LinkGraphNodesFromSoundNodes();
}


// =========================================================================
// 11.1 Sound Cues (5 actions)
// =========================================================================

// ---- audio.authoring.create_sound_cue ----
REGISTER_RPC_HANDLER("audio.authoring.create_sound_cue", "audio.authoring", "Create a new USoundCue asset, optionally pre-wired to a SoundWave with looping/volume/pitch nodes inserted between the wave player and output. For complex node graphs, follow with audio.authoring.add_cue_node and connect_cue_nodes.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name of the SoundCue"),
        RPC_PARAM_OPT("path", "path", "Content path (default /Game/Audio/Cues)"),
        RPC_PARAM_OPT("wavePath", "path", "Path to a SoundWave to use"),
        RPC_PARAM_DEF("looping", "boolean", "Add looping node", "false"),
        RPC_PARAM_DEF("volume", "number", "Volume multiplier", "1.0"),
        RPC_PARAM_DEF("pitch", "number", "Pitch multiplier", "1.0"),
        RPC_PARAM_DEF("save", "boolean", "Save after creation", "true")
    ))
{
    FString Name;
    if (!Ctx.RequireString(TEXT("name"), Name)) return true;

    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/Audio/Cues")));
    FString WavePath = Ctx.GetString(TEXT("wavePath"));
    bool bLooping = Ctx.GetBool(TEXT("looping"), false);
    float Volume = static_cast<float>(Ctx.GetNumber(TEXT("volume"), 1.0));
    float Pitch = static_cast<float>(Ctx.GetNumber(TEXT("pitch"), 1.0));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    FString PackagePath;
    if (!PinWrightAudioPackagePath::ComposeAudioAssetPackagePathOrRefuse(Ctx, Path, Name, PackagePath))
    {
        return true;
    }

    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(ErrorCodes::ERR_PACKAGE_ERROR, TEXT("Failed to create package"));
        return true;
    }

    USoundCueFactoryNew* Factory = NewObject<USoundCueFactoryNew>();
    USoundCue* NewCue = Cast<USoundCue>(
        Factory->FactoryCreateNew(USoundCue::StaticClass(), Package,
                                  FName(*Name), RF_Public | RF_Standalone,
                                  nullptr, GWarn));
    if (!NewCue)
    {
        Ctx.SendError(ErrorCodes::ERR_CREATE_FAILED, TEXT("Failed to create SoundCue"));
        return true;
    }

    // Build the node graph. The wave player requires a wavePath, but the
    // looping/modulator nodes are valid standalone graph states (see
    // add_cue_node), so they are constructed whenever requested even when no
    // wave is supplied — otherwise looping/volume/pitch would be silently
    // dropped. The chain is rooted at FirstNode regardless of which pieces
    // are present.
    USoundNode* LastNode = nullptr;

    // Link the freshly-constructed node above the current chain (if any) and
    // advance the chain tip. Centralizes the parent-link rule so adding a
    // future chained node type is a one-line Chain() call. The attachment goes
    // through the shared helper so the node's paired EdGraph input pin grows with
    // ChildNodes — LinkGraphNodesFromSoundNodes() below check()-crashes on a
    // mismatch, which a raw ChildNodes.Add() produced for every looping/modulator
    // chain. Slot 0 always fits the single-child node types chained here, so the
    // helper cannot refuse.
    auto Chain = [&LastNode](USoundNode* Node)
    {
        if (LastNode)
        {
            FString AttachError;
            AttachSoundCueChildNode(Node, LastNode, 0, AttachError);
        }
        LastNode = Node;
    };

    if (!WavePath.IsEmpty())
    {
        USoundWave* Wave = LoadSoundWaveFromPath(WavePath);
        if (!Wave)
        {
            Ctx.SendError(ErrorCodes::ERR_WAVE_NOT_FOUND, FString::Printf(TEXT("Could not load SoundWave: %s"), *WavePath));
            return true;
        }
        USoundNodeWavePlayer* PlayerNode = NewCue->ConstructSoundNode<USoundNodeWavePlayer>();
        PlayerNode->SetSoundWave(Wave);
        Chain(PlayerNode);
    }

    if (bLooping)
    {
        USoundNodeLooping* LoopNode = NewCue->ConstructSoundNode<USoundNodeLooping>();
        Chain(LoopNode);
    }

    if (Volume != 1.0f || Pitch != 1.0f)
    {
        USoundNodeModulator* ModNode = NewCue->ConstructSoundNode<USoundNodeModulator>();
        ModNode->PitchMin = ModNode->PitchMax = Pitch;
        ModNode->VolumeMin = ModNode->VolumeMax = Volume;
        Chain(ModNode);
    }

    if (LastNode)
    {
        NewCue->FirstNode = LastNode;
        NewCue->LinkGraphNodesFromSoundNodes();
    }

    const bool bSavedToDisk = SaveAudioAsset(NewCue, bSave);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("assetPath"), NewCue->GetPathName());
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("SoundCue '%s' created"), *Name));
    AddAssetSaveReport(Result, bSave, bSavedToDisk);
    AddAssetVerification(Result, NewCue);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- audio.authoring.add_cue_node ----
REGISTER_RPC_HANDLER("audio.authoring.add_cue_node", "audio.authoring", "Add a node to a SoundCue graph",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the SoundCue"),
        RPC_PARAM_DEF("nodeType", "string", "Type: wave_player, mixer, random, modulator, looping, attenuation, concatenator, delay, switch, branch", "wave_player"),
        RPC_PARAM_OPT("wavePath", "path", "Wave path for wave_player nodes"),
        RPC_PARAM_DEF("volume", "number", "Volume for modulator nodes", "1.0"),
        RPC_PARAM_DEF("pitch", "number", "Pitch for modulator nodes", "1.0"),
        RPC_PARAM_DEF("indefinite", "boolean", "Loop indefinitely for looping nodes", "true"),
        RPC_PARAM_DEF("loopCount", "integer", "Loop count for looping nodes", "0"),
        RPC_PARAM_OPT("attenuationPath", "path", "Attenuation asset path for attenuation nodes"),
        RPC_PARAM_DEF("delay", "number", "Delay time for delay nodes", "0"),
        RPC_PARAM_DEF("save", "boolean", "Save after modification", "true")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString NodeType = Ctx.GetString(TEXT("nodeType"), TEXT("wave_player"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    USoundCue* Cue = LoadSoundCueFromPath(AssetPath);
    if (!Cue)
    {
        Ctx.SendError(ErrorCodes::ERR_CUE_NOT_FOUND, FString::Printf(TEXT("Could not load SoundCue: %s"), *AssetPath));
        return true;
    }

    USoundNode* NewNode = nullptr;
    FString NodeTypeLower = NodeType.ToLower();

    if (NodeTypeLower == TEXT("wave_player") || NodeTypeLower == TEXT("waveplayer"))
    {
        USoundNodeWavePlayer* Player = Cue->ConstructSoundNode<USoundNodeWavePlayer>();
        FString WavePath = Ctx.GetString(TEXT("wavePath"));
        if (!WavePath.IsEmpty())
        {
            USoundWave* Wave = LoadSoundWaveFromPath(WavePath);
            if (Wave)
            {
                Player->SetSoundWave(Wave);
            }
        }
        NewNode = Player;
    }
    else if (NodeTypeLower == TEXT("mixer"))
    {
        NewNode = Cue->ConstructSoundNode<USoundNodeMixer>();
    }
    else if (NodeTypeLower == TEXT("random"))
    {
        NewNode = Cue->ConstructSoundNode<USoundNodeRandom>();
    }
    else if (NodeTypeLower == TEXT("modulator"))
    {
        USoundNodeModulator* Mod = Cue->ConstructSoundNode<USoundNodeModulator>();
        Mod->VolumeMin = Mod->VolumeMax = static_cast<float>(Ctx.GetNumber(TEXT("volume"), 1.0));
        Mod->PitchMin = Mod->PitchMax = static_cast<float>(Ctx.GetNumber(TEXT("pitch"), 1.0));
        NewNode = Mod;
    }
    else if (NodeTypeLower == TEXT("looping"))
    {
        USoundNodeLooping* Loop = Cue->ConstructSoundNode<USoundNodeLooping>();
        Loop->bLoopIndefinitely = Ctx.GetBool(TEXT("indefinite"), true);
        Loop->LoopCount = Ctx.GetInt(TEXT("loopCount"), 0);
        NewNode = Loop;
    }
    else if (NodeTypeLower == TEXT("attenuation"))
    {
        USoundNodeAttenuation* Atten = Cue->ConstructSoundNode<USoundNodeAttenuation>();
        FString AttenPath = Ctx.GetString(TEXT("attenuationPath"));
        if (!AttenPath.IsEmpty())
        {
            USoundAttenuation* AttenAsset = LoadSoundAttenuationFromPath(AttenPath);
            if (AttenAsset)
            {
                Atten->AttenuationSettings = AttenAsset;
            }
        }
        NewNode = Atten;
    }
    else if (NodeTypeLower == TEXT("concatenator"))
    {
        NewNode = Cue->ConstructSoundNode<USoundNodeConcatenator>();
    }
    else if (NodeTypeLower == TEXT("delay"))
    {
        USoundNodeDelay* Delay = Cue->ConstructSoundNode<USoundNodeDelay>();
        Delay->DelayMin = Delay->DelayMax = static_cast<float>(Ctx.GetNumber(TEXT("delay"), 0.0));
        NewNode = Delay;
    }
    else if (NodeTypeLower == TEXT("switch"))
    {
        NewNode = Cue->ConstructSoundNode<USoundNodeSwitch>();
    }
    else if (NodeTypeLower == TEXT("branch"))
    {
        NewNode = Cue->ConstructSoundNode<USoundNodeBranch>();
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_UNKNOWN_NODE_TYPE, FString::Printf(TEXT("Unknown node type: %s"), *NodeType));
        return true;
    }

    if (!NewNode)
    {
        Ctx.SendError(ErrorCodes::ERR_CREATE_NODE_FAILED, TEXT("Failed to create sound node"));
        return true;
    }

    Cue->LinkGraphNodesFromSoundNodes();
    SaveAudioAsset(Cue, bSave);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("nodeId"), NewNode->GetName());
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Node '%s' added to SoundCue"), *NodeType));
    AddAssetVerification(Result, Cue);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- audio.authoring.connect_cue_nodes ----
REGISTER_RPC_HANDLER("audio.authoring.connect_cue_nodes", "audio.authoring", "Connect two nodes in a SoundCue graph. The child slot is grown through the node's own insertion path, so a Random node's Weights and a Mixer/Concatenator's InputVolume get the engine's 1.0 defaults instead of being left degenerate. Pass sourceNodeId \"Output\" to root the cue at the target instead (same write as set_cue_root).",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the SoundCue"),
        RPC_PARAM_REQ("sourceNodeId", "string", "Name of the source (parent) node, or \"Output\"/\"Root\" to make the target the cue's root node"),
        RPC_PARAM_REQ("targetNodeId", "string", "Name of the target (child) node"),
        RPC_PARAM_DEF("childIndex", "integer", "Child slot index on source; must be below the source node type's max child count. Ignored when sourceNodeId is \"Output\"", "0"),
        RPC_PARAM_DEF("save", "boolean", "Save after connecting", "true")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString SourceNodeId, TargetNodeId;
    if (!Ctx.RequireString(TEXT("sourceNodeId"), SourceNodeId)) return true;
    if (!Ctx.RequireString(TEXT("targetNodeId"), TargetNodeId)) return true;

    int32 ChildIndex = Ctx.GetInt(TEXT("childIndex"), 0);
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    USoundCue* Cue = LoadSoundCueFromPath(AssetPath);
    if (!Cue)
    {
        Ctx.SendError(ErrorCodes::ERR_CUE_NOT_FOUND, FString::Printf(TEXT("Could not load SoundCue: %s"), *AssetPath));
        return true;
    }

    USoundNode* SourceNode = FindSoundCueNodeByName(Cue, SourceNodeId);

    // "Output" / "Root" as the source means "make the target this cue's root". The graph root
    // is not a USoundNode so it can never resolve by name, and until this alias existed the
    // documented add_cue_node + connect_cue_nodes workflow could build a complete tree and then
    // had no way to root it, shipping a silent cue (E-cue-graph-verbs-cannot-set-firstnode).
    // Same write as audio.authoring.set_cue_root; childIndex plays no part on this path.
    const bool bRootConnect = !SourceNode && IsSoundCueRootNodeId(SourceNodeId);

    if (!SourceNode && !bRootConnect)
    {
        Ctx.SendError(ErrorCodes::ERR_SOURCE_NODE_NOT_FOUND,
            FString::Printf(TEXT("Source node not found: %s (use audio.authoring.set_cue_root, or sourceNodeId \"Output\", to root the cue)"), *SourceNodeId));
        return true;
    }

    USoundNode* TargetNode = FindSoundCueNodeByName(Cue, TargetNodeId);
    if (!TargetNode)
    {
        Ctx.SendError(ErrorCodes::ERR_TARGET_NODE_NOT_FOUND, FString::Printf(TEXT("Target node not found: %s"), *TargetNodeId));
        return true;
    }

    if (bRootConnect)
    {
        SetSoundCueRootNode(Cue, TargetNode);
        SaveAudioAsset(Cue, bSave);

        TSharedPtr<FJsonObject> RootResult = MakeShareable(new FJsonObject());
        RootResult->SetStringField(TEXT("firstNode"), TargetNode->GetName());
        RootResult->SetStringField(TEXT("message"), FString::Printf(TEXT("SoundCue root set to '%s'"), *TargetNode->GetName()));
        AddAssetVerification(RootResult, Cue);
        Ctx.SendSuccess(RootResult);
        return true;
    }

    // A negative slot has no pin/child to target and would make the
    // ChildNodes[ChildIndex] access below an out-of-bounds crash — reject it
    // cleanly rather than fault.
    if (ChildIndex < 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_CHILD_INDEX,
            FString::Printf(TEXT("childIndex must be >= 0 (got %d)"), ChildIndex));
        return true;
    }

    // Grows the slot through USoundNode::InsertChildNode so the source's per-child arrays
    // (Random Weights, Mixer/Concatenator InputVolume) and its EdGraph input pins keep the
    // engine's own defaults and counts — see AttachSoundCueChildNode.
    FString AttachError;
    if (!AttachSoundCueChildNode(SourceNode, TargetNode, ChildIndex, AttachError))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_CHILD_INDEX, AttachError);
        return true;
    }

    Cue->LinkGraphNodesFromSoundNodes();
    SaveAudioAsset(Cue, bSave);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("message"), TEXT("Nodes connected"));
    AddAssetVerification(Result, Cue);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- audio.authoring.set_cue_root ----
REGISTER_RPC_HANDLER("audio.authoring.set_cue_root", "audio.authoring", "Make an existing cue node the graph's root (USoundCue::FirstNode) - the node the audio device starts playback from. add_cue_node creates every node detached and connect_cue_nodes only wires nodes to each other, so a cue built with those verbs stays silent until it is rooted here. Also relinks the Sound Cue editor's Output pin, without which the next graph edit recompiles FirstNode back to null.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the SoundCue"),
        RPC_PARAM_REQ("nodeId", "string", "Name of the node to root the cue at, as returned by add_cue_node (e.g. SoundNodeModulator_0)"),
        RPC_PARAM_DEF("save", "boolean", "Save after rooting", "true")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString NodeId;
    if (!Ctx.RequireString(TEXT("nodeId"), NodeId)) return true;

    bool bSave = Ctx.GetBool(TEXT("save"), true);

    USoundCue* Cue = LoadSoundCueFromPath(AssetPath);
    if (!Cue)
    {
        Ctx.SendError(ErrorCodes::ERR_CUE_NOT_FOUND, FString::Printf(TEXT("Could not load SoundCue: %s"), *AssetPath));
        return true;
    }

    USoundNode* RootNode = FindSoundCueNodeByName(Cue, NodeId);
    if (!RootNode)
    {
        Ctx.SendError(ErrorCodes::ERR_NODE_NOT_FOUND, FString::Printf(TEXT("Node not found in SoundCue: %s"), *NodeId));
        return true;
    }

    SetSoundCueRootNode(Cue, RootNode);
    SaveAudioAsset(Cue, bSave);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("firstNode"), RootNode->GetName());
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("SoundCue root set to '%s'"), *RootNode->GetName()));
    AddAssetVerification(Result, Cue);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- audio.authoring.set_cue_attenuation ----
REGISTER_RPC_HANDLER("audio.authoring.set_cue_attenuation", "audio.authoring", "Set attenuation settings on a SoundCue",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the SoundCue"),
        RPC_PARAM_OPT("attenuationPath", "path", "Asset path of attenuation (empty to clear)"),
        RPC_PARAM_DEF("save", "boolean", "Save after modification", "true")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString AttenuationPath = Ctx.GetString(TEXT("attenuationPath"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    USoundCue* Cue = LoadSoundCueFromPath(AssetPath);
    if (!Cue)
    {
        Ctx.SendError(ErrorCodes::ERR_CUE_NOT_FOUND, FString::Printf(TEXT("Could not load SoundCue: %s"), *AssetPath));
        return true;
    }

    if (!AttenuationPath.IsEmpty())
    {
        USoundAttenuation* Atten = LoadSoundAttenuationFromPath(AttenuationPath);
        if (Atten) Cue->AttenuationSettings = Atten;
    }
    else
    {
        Cue->AttenuationSettings = nullptr;
    }

    SaveAudioAsset(Cue, bSave);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("message"), TEXT("Attenuation settings updated"));
    AddAssetVerification(Result, Cue);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- audio.authoring.set_cue_concurrency ----
REGISTER_RPC_HANDLER("audio.authoring.set_cue_concurrency", "audio.authoring", "Set concurrency settings on a SoundCue",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the SoundCue"),
        RPC_PARAM_OPT("concurrencyPath", "path", "Asset path of concurrency (empty to clear)"),
        RPC_PARAM_DEF("save", "boolean", "Save after modification", "true")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString ConcurrencyPath = Ctx.GetString(TEXT("concurrencyPath"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    USoundCue* Cue = LoadSoundCueFromPath(AssetPath);
    if (!Cue)
    {
        Ctx.SendError(ErrorCodes::ERR_CUE_NOT_FOUND, FString::Printf(TEXT("Could not load SoundCue: %s"), *AssetPath));
        return true;
    }

    if (!ConcurrencyPath.IsEmpty())
    {
        USoundConcurrency* Conc = Cast<USoundConcurrency>(
            StaticLoadObject(USoundConcurrency::StaticClass(), nullptr, *NormalizeContentAssetPath(ConcurrencyPath)));
        if (Conc)
        {
            Cue->ConcurrencySet.Empty();
            Cue->ConcurrencySet.Add(Conc);
        }
    }
    else
    {
        Cue->ConcurrencySet.Empty();
    }

    SaveAudioAsset(Cue, bSave);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("message"), TEXT("Concurrency settings updated"));
    AddAssetVerification(Result, Cue);
    Ctx.SendSuccess(Result);
    return true;
}

// Maps a resolutionRule string (the FSoundConcurrencySettings.ResolutionRule policy) to its enum
// value, case-insensitively, accepting the canonical EMaxConcurrentResolutionRule spellings.
// Returns false for an unrecognized value so create_sound_concurrency can reject it rather than
// silently defaulting (no fake-success). No bare "StopFarthest" alias: it is ambiguous between the
// ThenPreventNew and ThenOldest variants, so an unqualified value is rejected with the full list.
static bool ResolveConcurrencyResolutionRule(const FString& In, EMaxConcurrentResolutionRule::Type& Out)
{
    const FString S = In.TrimStartAndEnd();
    if (S.Equals(TEXT("PreventNew"), ESearchCase::IgnoreCase))                       { Out = EMaxConcurrentResolutionRule::PreventNew; return true; }
    if (S.Equals(TEXT("StopOldest"), ESearchCase::IgnoreCase))                       { Out = EMaxConcurrentResolutionRule::StopOldest; return true; }
    if (S.Equals(TEXT("StopFarthestThenPreventNew"), ESearchCase::IgnoreCase))       { Out = EMaxConcurrentResolutionRule::StopFarthestThenPreventNew; return true; }
    if (S.Equals(TEXT("StopFarthestThenOldest"), ESearchCase::IgnoreCase))           { Out = EMaxConcurrentResolutionRule::StopFarthestThenOldest; return true; }
    if (S.Equals(TEXT("StopLowestPriority"), ESearchCase::IgnoreCase))               { Out = EMaxConcurrentResolutionRule::StopLowestPriority; return true; }
    if (S.Equals(TEXT("StopQuietest"), ESearchCase::IgnoreCase))                     { Out = EMaxConcurrentResolutionRule::StopQuietest; return true; }
    if (S.Equals(TEXT("StopLowestPriorityThenPreventNew"), ESearchCase::IgnoreCase)) { Out = EMaxConcurrentResolutionRule::StopLowestPriorityThenPreventNew; return true; }
    return false;
}

// ---- audio.authoring.create_sound_concurrency ----
// Creates the USoundConcurrency asset that set_cue_concurrency / play_sound / create_ambient can
// only reference, never author (F-sound-concurrency-asset-authoring). Without it the shared-voice-
// limit workflow dead-ended at python.execute (SoundConcurrencyFactory). Mirrors the sibling
// create_sound_submix / create_source_effect_preset creators: NewObject a concrete Engine UObject
// (USoundConcurrency is MinimalAPI but its StaticClass() is exported — already used by
// set_cue_concurrency's StaticLoadObject just above — so NewObject<> links directly), map the
// publicly-settable FSoundConcurrencySettings knobs, save, and return asset verification. Closes
// create_sound_concurrency -> set_cue_concurrency entirely on the RPC surface.
REGISTER_RPC_HANDLER("audio.authoring.create_sound_concurrency", "audio.authoring", "Create a USoundConcurrency asset (a shared voice-limit group). Set maxCount + resolutionRule to cap concurrent voices and choose the steal policy, then point SoundCues at it with set_cue_concurrency (or pass its path as concurrencyPath to play_sound / create_ambient).",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name of the SoundConcurrency asset"),
        RPC_PARAM_OPT("path", "path", "Content path (default /Game/Audio/Concurrency)"),
        RPC_PARAM_OPT("maxCount", "integer", "Max concurrent active voices in this group (>=1). Omitted or <1 leaves the engine default."),
        RPC_PARAM_OPT("resolutionRule", "string", "Policy when MaxCount is reached: PreventNew, StopOldest, StopFarthestThenPreventNew, StopFarthestThenOldest, StopLowestPriority, StopQuietest, or StopLowestPriorityThenPreventNew. Omitted leaves the engine default (StopFarthestThenOldest)."),
        RPC_PARAM_OPT("limitToOwner", "boolean", "Limit concurrency per sound owner (the actor that plays the sound) instead of globally (default false)"),
        RPC_PARAM_DEF("save", "boolean", "Save after creation", "true")
    ))
{
    FString Name;
    if (!Ctx.RequireString(TEXT("name"), Name)) return true;

    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/Audio/Concurrency")));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    // Composed and checked ABOVE the resolutionRule resolution below, and that ordering is
    // load-bearing twice over. (1) A malformed `name` is a caller-argument fault that should be
    // reported whatever else is wrong with the payload. (2) It is what lets
    // PinWright.audio.authoring.create_sound_concurrency.NameCarryingAPathIsRefused assert the
    // refusal without ever being able to drive CreatePackage's Fatal: the test pairs a bad `name`
    // with a bad `resolutionRule`, so a build with this check removed bails at the rule check -
    // which is still above the concatenation - and goes red on the wrong error code instead of
    // taking the suite host down with it. Do not move this below the rule check.
    FString PackagePath;
    if (!PinWrightAudioPackagePath::ComposeAudioAssetPackagePathOrRefuse(Ctx, Path, Name, PackagePath))
    {
        return true;
    }

    // Resolve resolutionRule up front so a bad value fails before any package/asset is created.
    const FString ResolutionRuleStr = Ctx.GetString(TEXT("resolutionRule")).TrimStartAndEnd();
    const bool bHasResolutionRule = !ResolutionRuleStr.IsEmpty();
    EMaxConcurrentResolutionRule::Type ResolutionRule = EMaxConcurrentResolutionRule::StopFarthestThenOldest;
    if (bHasResolutionRule && !ResolveConcurrencyResolutionRule(ResolutionRuleStr, ResolutionRule))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_RESOLUTION_RULE,
            FString::Printf(TEXT("Unknown resolutionRule '%s'. Use one of: PreventNew, StopOldest, StopFarthestThenPreventNew, StopFarthestThenOldest, StopLowestPriority, StopQuietest, StopLowestPriorityThenPreventNew."), *ResolutionRuleStr));
        return true;
    }

    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(ErrorCodes::ERR_PACKAGE_ERROR, TEXT("Failed to create package"));
        return true;
    }

    USoundConcurrency* NewConcurrency = NewObject<USoundConcurrency>(
        Package, FName(*Name), RF_Public | RF_Standalone);
    if (!NewConcurrency)
    {
        Ctx.SendError(ErrorCodes::ERR_CREATE_FAILED, TEXT("Failed to create SoundConcurrency asset"));
        return true;
    }

    // Map the publicly-settable FSoundConcurrencySettings knobs. MaxCount clamps at 1 (engine
    // UIMin/ClampMin); only apply an explicitly-provided value >=1 so an omitted maxCount leaves the
    // engine default. (VolumeScale, the fourth field named in the ticket prose, is a private member
    // with only a getter and no public setter, so it is intentionally not exposed here.)
    const int32 MaxCount = Ctx.GetInt(TEXT("maxCount"), 0);
    if (MaxCount >= 1)
    {
        NewConcurrency->Concurrency.MaxCount = MaxCount;
    }
    if (bHasResolutionRule)
    {
        NewConcurrency->Concurrency.ResolutionRule = ResolutionRule;
    }
    NewConcurrency->Concurrency.bLimitToOwner = Ctx.GetBool(TEXT("limitToOwner"), false) ? 1 : 0;

    const bool bSavedToDisk = SaveAudioAsset(NewConcurrency, bSave);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("assetPath"), NewConcurrency->GetPathName());
    Result->SetNumberField(TEXT("maxCount"), NewConcurrency->Concurrency.MaxCount);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("SoundConcurrency '%s' created"), *Name));
    AddAssetSaveReport(Result, bSave, bSavedToDisk);
    AddAssetVerification(Result, NewConcurrency);
    Ctx.SendSuccess(Result);
    return true;
}

// =========================================================================
// 11.2 MetaSounds (6 actions)
// =========================================================================

// ---- audio.authoring.create_metasound ----
REGISTER_RPC_HANDLER("audio.authoring.create_metasound", "audio.authoring", "Create an empty UMetaSoundSource asset (procedural audio graph). Add inputs/outputs and nodes via audio.authoring.add_metasound_input / add_metasound_output / add_metasound_node, then connect them with connect_metasound_nodes.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name of the MetaSound"),
        RPC_PARAM_OPT("path", "path", "Content path (default /Game/Audio/MetaSounds)"),
        RPC_PARAM_DEF("save", "boolean", "Save after creation", "true")
    ))
{
#if MCP_HAS_METASOUND && MCP_HAS_METASOUND_FACTORY
    FString Name;
    if (!Ctx.RequireString(TEXT("name"), Name)) return true;

    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/Audio/MetaSounds")));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    FString PackagePath;
    if (!PinWrightAudioPackagePath::ComposeAudioAssetPackagePathOrRefuse(Ctx, Path, Name, PackagePath))
    {
        return true;
    }

    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(ErrorCodes::ERR_PACKAGE_ERROR, TEXT("Failed to create package"));
        return true;
    }

    UMetaSoundSourceFactory* Factory = NewObject<UMetaSoundSourceFactory>();
    UMetaSoundSource* MetaSound = Cast<UMetaSoundSource>(
        Factory->FactoryCreateNew(UMetaSoundSource::StaticClass(), Package,
                                  FName(*Name), RF_Public | RF_Standalone,
                                  nullptr, GWarn));

    if (!MetaSound)
    {
        Ctx.SendError(ErrorCodes::ERR_CREATE_FAILED, TEXT("Failed to create MetaSound asset"));
        return true;
    }

    // Register the brand-new asset before saving: SaveLoadedAsset declines to write a
    // never-before-seen in-memory package, so without this a save:true would report saved:false.
    // Mirrors create_metasound_patch / niagara.create_*.
    FAssetRegistryModule::AssetCreated(MetaSound);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("assetPath"), MetaSound->GetPathName());
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("MetaSound '%s' created"), *Name));
    // save:true writes the .uasset for real and reports the shared save verdict. It used to be an
    // unconditional mark-dirty that ignored bSave entirely, so the Source create lost the whole
    // asset on a cold restart while reporting success (B-metasound-create-save-no-disk-write).
    PinWright::MetaSound::SaveMetaSoundAndReport(Result, MetaSound, bSave);
    AddAssetVerification(Result, MetaSound);
    Ctx.SendSuccess(Result);
    return true;
#elif MCP_HAS_METASOUND
    // MetaSound available but no factory - create basic asset
    FString Name;
    if (!Ctx.RequireString(TEXT("name"), Name)) return true;

    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/Audio/MetaSounds")));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    FString PackagePath;
    if (!PinWrightAudioPackagePath::ComposeAudioAssetPackagePathOrRefuse(Ctx, Path, Name, PackagePath))
    {
        return true;
    }

    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(ErrorCodes::ERR_PACKAGE_ERROR, TEXT("Failed to create package"));
        return true;
    }

    UMetaSoundSource* MetaSound = NewObject<UMetaSoundSource>(Package, FName(*Name), RF_Public | RF_Standalone);
    if (!MetaSound)
    {
        Ctx.SendError(ErrorCodes::ERR_CREATE_FAILED, TEXT("Failed to create MetaSound asset"));
        return true;
    }

    FAssetRegistryModule::AssetCreated(MetaSound);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("assetPath"), MetaSound->GetPathName());
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("MetaSound '%s' created"), *Name));
    PinWright::MetaSound::SaveMetaSoundAndReport(Result, MetaSound, bSave);
    AddAssetVerification(Result, MetaSound);
    Ctx.SendSuccess(Result);
    return true;
#else
    Ctx.SendError(ErrorCodes::ERR_METASOUND_NOT_AVAILABLE, TEXT("MetaSound support not available in this engine version"));
    return true;
#endif
}

// ---- audio.authoring.add_metasound_node ----
REGISTER_RPC_HANDLER("audio.authoring.add_metasound_node", "audio.authoring", "Add a node to a MetaSound graph",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the MetaSound"),
        RPC_PARAM_OPT("nodeClassName", "string", "Dotted registry class name exactly as audio.authoring.search_metasound_nodes reports it (e.g. \"UE.Sine.Audio\", \"UE.Add.Float\")"),
        RPC_PARAM_OPT("nodeType", "string", "Shorthand type: oscillator, gain, add, waveplayer"),
        RPC_PARAM_OPT("versionMajor", "integer", "Major version to resolve (defaults to 1). The engine resolves the highest registered minor for this major."),
        RPC_PARAM_OPT("versionMinor", "integer", "Minor version hint. Accepted for parity with search_metasound_nodes; the engine resolves the highest minor for the chosen major."),
        RPC_PARAM_DEF("save", "boolean", "Save after modification", "true")
    ))
{
#if MCP_HAS_METASOUND && MCP_HAS_METASOUND_FRONTEND
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString NodeClassName = Ctx.GetString(TEXT("nodeClassName"));
    FString NodeType = Ctx.GetString(TEXT("nodeType"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PATH, TEXT("Asset path is required"));
        return true;
    }

    // Load gate accepts Source or Patch — see LoadMetaSoundDocumentObject.
    UObject* MetaSound = PinWright::MetaSound::LoadMetaSoundDocumentObject(AssetPath);
    if (!MetaSound)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, FString::Printf(TEXT("Could not load MetaSound: %s"), *AssetPath));
        return true;
    }

    // LoadMetaSoundDocumentObject already guarantees IMetaSoundDocumentInterface.
    TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface(MetaSound);
    PW_METASOUND_MAKE_BUILDER(Builder, ScriptInterface);

    // Determine node class name from nodeType if not explicitly provided
    FString ActualClassName = NodeClassName;
    if (ActualClassName.IsEmpty() && !NodeType.IsEmpty())
    {
        // Shorthands map to live UE.* registry keys (namespace "UE", not the stale
        // "Metasound.*" form). These are the canonical dotted class names that
        // search_metasound_nodes reports for the shipped Epic standard/engine nodes.
        FString NodeTypeLower = NodeType.ToLower();
        if (NodeTypeLower == TEXT("oscillator") || NodeTypeLower == TEXT("sine"))
            ActualClassName = TEXT("UE.Sine.Audio");
        else if (NodeTypeLower == TEXT("gain") || NodeTypeLower == TEXT("multiply"))
            ActualClassName = TEXT("UE.Multiply.Audio by Float");
        else if (NodeTypeLower == TEXT("add"))
            ActualClassName = TEXT("UE.Add.Float");
        else if (NodeTypeLower == TEXT("waveplayer"))
            ActualClassName = TEXT("UE.Wave Player.Mono");
        else
            ActualClassName = NodeType;
    }

    if (ActualClassName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_NODE_TYPE, TEXT("Node class name or type is required"));
        PW_METASOUND_FINISH_BUILDING(Builder);
        return true;
    }

    // Parse the dotted registry key (Namespace.Name[.Variant]) into the structured
    // FMetasoundFrontendClassName the registry indexes on. The previous code stuffed the
    // entire flat string into the Name field with empty Namespace/Variant, which can never
    // match a key like "UE.Sine.Audio" — that was the NODE_CLASS_NOT_FOUND root cause.
    // Parse() splits on '.' (Tokens[0]=Namespace, [1]=Name, [2]=Variant) so search output
    // round-trips directly into add. Bare names with no '.' fall back to the legacy form.
    FMetasoundFrontendClassName ClassName;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    if (!FMetasoundFrontendClassName::Parse(ActualClassName, ClassName))
    {
        ClassName = FMetasoundFrontendClassName(FName(), FName(*ActualClassName), FName());
    }
#else
    // FMetasoundFrontendClassName::Parse was added in UE 5.4; replicate its dotted-key
    // split (Namespace.Name[.Variant]) inline for UE 5.3. Bare names with no '.' fall
    // back to the legacy Name-only form below.
    {
        TArray<FString> Tokens;
        ActualClassName.ParseIntoArray(Tokens, TEXT("."));
        if (Tokens.Num() >= 2)
        {
            ClassName = FMetasoundFrontendClassName(
                FName(*Tokens[0]), FName(*Tokens[1]),
                Tokens.Num() > 2 ? FName(*Tokens[2]) : FName());
        }
        else
        {
            ClassName = FMetasoundFrontendClassName(FName(), FName(*ActualClassName), FName());
        }
    }
#endif

    // versionMajor selects the major; the public AddNodeByClassName overload resolves the
    // highest registered minor for that major (versionMinor is accepted for parity with
    // search but the engine picks the highest minor). Default major is 1.
    const int32 MajorVersion = FMath::Max(1, Ctx.GetInt(TEXT("versionMajor"), 1));
    const FMetasoundFrontendNode* NewNode = Builder.AddNodeByClassName(ClassName, MajorVersion, FGuid::NewGuid());

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    // Report the canonical dotted key the registry resolved (round-tripped from the
    // parsed struct), which matches what search_metasound_nodes emits.
    const FString ResolvedClassName = ClassName.ToString();
    const FString NewNodeId = NewNode ? NewNode->GetID().ToString() : FString();

    // Flush the builder's edits into the document BEFORE the save below: the save is a real
    // disk write now, and a write taken ahead of FinishBuilding would serialize the pre-edit
    // document (audio.authoring.metasound_gotchas).
    PW_METASOUND_FINISH_BUILDING(Builder);

    if (NewNode)
    {
        Result->SetStringField(TEXT("nodeId"), NewNodeId);
        Result->SetStringField(TEXT("nodeClassName"), ResolvedClassName);
        Result->SetStringField(TEXT("requestedClassName"), ActualClassName);
        Result->SetStringField(TEXT("message"), FString::Printf(TEXT("MetaSound node '%s' added"), *ResolvedClassName));
        PinWright::MetaSound::SaveMetaSoundAndReport(Result, MetaSound, bSave);
        AddAssetVerification(Result, MetaSound);
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_NODE_CLASS_NOT_FOUND, FString::Printf(TEXT("Node class '%s' not found in MetaSound registry"), *ActualClassName));
    }

    return true;
#elif MCP_HAS_METASOUND
    FString NodeType = Ctx.GetString(TEXT("nodeType"));
    Ctx.SendError(ErrorCodes::ERR_METASOUND_FRONTEND_NOT_SUPPORTED, FString::Printf(TEXT("Cannot add MetaSound node '%s' - Frontend Builder not available. Requires UE 5.3+"), *NodeType));
    return true;
#else
    Ctx.SendError(ErrorCodes::ERR_METASOUND_NOT_AVAILABLE, TEXT("MetaSound support not available"));
    return true;
#endif
}

// ---- audio.authoring.connect_metasound_nodes ----
REGISTER_RPC_HANDLER("audio.authoring.connect_metasound_nodes", "audio.authoring", "Connect two MetaSound nodes via an edge",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the MetaSound"),
        RPC_PARAM_REQ("sourceNodeId", "string", "GUID of the source node"),
        RPC_PARAM_REQ("sourceOutputName", "string", "Output pin name on source"),
        RPC_PARAM_REQ("targetNodeId", "string", "GUID of the target node"),
        RPC_PARAM_REQ("targetInputName", "string", "Input pin name on target"),
        RPC_PARAM_DEF("save", "boolean", "Save after connecting", "true")
    ))
{
#if MCP_HAS_METASOUND && MCP_HAS_METASOUND_FRONTEND
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString SourceNodeId, SourceOutputName, TargetNodeId, TargetInputName;
    if (!Ctx.RequireString(TEXT("sourceNodeId"), SourceNodeId)) return true;
    if (!Ctx.RequireString(TEXT("sourceOutputName"), SourceOutputName)) return true;
    if (!Ctx.RequireString(TEXT("targetNodeId"), TargetNodeId)) return true;
    if (!Ctx.RequireString(TEXT("targetInputName"), TargetInputName)) return true;

    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PATH, TEXT("Asset path is required"));
        return true;
    }

    // Load gate accepts Source or Patch — see LoadMetaSoundDocumentObject.
    UObject* MetaSound = PinWright::MetaSound::LoadMetaSoundDocumentObject(AssetPath);
    if (!MetaSound)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, FString::Printf(TEXT("Could not load MetaSound: %s"), *AssetPath));
        return true;
    }

    TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface(MetaSound);
    PW_METASOUND_MAKE_BUILDER(Builder, ScriptInterface);

    FGuid SourceGuid, TargetGuid;
    if (!FGuid::Parse(SourceNodeId, SourceGuid) || !FGuid::Parse(TargetNodeId, TargetGuid))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_GUID, TEXT("Invalid node ID format - must be valid GUID"));
        PW_METASOUND_FINISH_BUILDING(Builder);
        return true;
    }

    Metasound::Frontend::FNamedEdge NamedEdge{
        SourceGuid, FName(*SourceOutputName),
        TargetGuid, FName(*TargetInputName)
    };

    TSet<Metasound::Frontend::FNamedEdge> Edges;
    Edges.Add(NamedEdge);

    TArray<const FMetasoundFrontendEdge*> CreatedEdges;
    bool bSuccess = Builder.AddNamedEdges(Edges, &CreatedEdges, true);
    const int32 CreatedEdgeCount = CreatedEdges.Num();

    // Flush the builder before the (now real) disk write below — see add_metasound_node.
    PW_METASOUND_FINISH_BUILDING(Builder);

    if (bSuccess && CreatedEdgeCount > 0)
    {
        TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
        Result->SetStringField(TEXT("message"), TEXT("MetaSound nodes connected"));
        Result->SetNumberField(TEXT("edgesCreated"), CreatedEdgeCount);
        PinWright::MetaSound::SaveMetaSoundAndReport(Result, MetaSound, bSave);
        AddAssetVerification(Result, MetaSound);
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_EDGE_FAILED, TEXT("Failed to create edge connection"));
    }

    return true;
#elif MCP_HAS_METASOUND
    Ctx.SendError(ErrorCodes::ERR_METASOUND_FRONTEND_NOT_SUPPORTED, TEXT("Cannot connect MetaSound nodes - Frontend Builder not available. Requires UE 5.3+"));
    return true;
#else
    Ctx.SendError(ErrorCodes::ERR_METASOUND_NOT_AVAILABLE, TEXT("MetaSound support not available"));
    return true;
#endif
}

// ---- audio.authoring.add_metasound_input ----
REGISTER_RPC_HANDLER("audio.authoring.add_metasound_input", "audio.authoring", "Add a graph input to a MetaSound",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the MetaSound"),
        RPC_PARAM_REQ("inputName", "string", "Name of the input"),
        RPC_PARAM_DEF("inputType", "string", "Registered MetaSound data type: Float, Int/Int32, Bool/Boolean, String, Audio, Trigger, Time, WaveAsset, or any other registered name (AudioBusAsset, WaveTable, Enum:*)", "Float"),
        RPC_PARAM_DEF("save", "boolean", "Save after modification", "true")
    ))
{
#if MCP_HAS_METASOUND && MCP_HAS_METASOUND_FRONTEND
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString InputName;
    if (!Ctx.RequireString(TEXT("inputName"), InputName)) return true;

    FString InputType = Ctx.GetString(TEXT("inputType"), TEXT("Float"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PATH, TEXT("Asset path is required"));
        return true;
    }

    // Load gate accepts Source or Patch — see LoadMetaSoundDocumentObject.
    UObject* MetaSound = PinWright::MetaSound::LoadMetaSoundDocumentObject(AssetPath);
    if (!MetaSound)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, FString::Printf(TEXT("Could not load MetaSound: %s"), *AssetPath));
        return true;
    }

    TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface(MetaSound);
    PW_METASOUND_MAKE_BUILDER(Builder, ScriptInterface);

    FMetasoundFrontendClassInput ClassInput;
    ClassInput.Name = FName(*InputName);
    // Resolve the documented convenience name (e.g. "Int") to the registry's
    // canonical data-type key (e.g. "Int32") — same mapping the variable handler
    // uses, so "Int" inputs are accepted consistently with "Int" variables.
    ClassInput.TypeName = FName(*PinWright::MetaSound::CanonicalizeMetaSoundTypeName(InputType));
    ClassInput.VertexID = FGuid::NewGuid();
    ClassInput.NodeID = FGuid::NewGuid();
    ClassInput.AccessType = EMetasoundFrontendVertexAccessType::Reference;

#if MCP_HAS_METASOUND_LITERAL_HELPER
    FMetasoundFrontendLiteral Literal;
    if (!PinWright::MetaSound::MakeDefaultLiteralForMetaSoundType(InputType, Literal))
    {
        // §3: an unrecognized type name is an error naming the way out, never a fallback.
        PinWright::MetaSound::SendMetaSoundUnknownTypeError(Ctx, TEXT("inputType"), InputType);
        PW_METASOUND_FINISH_BUILDING(Builder);
        return true;
    }
    // Paged input defaults: InitDefault() on 5.5+, DefaultLiteral on 5.4.
    PinWright::MetaSound::SetClassInputDefault(ClassInput, Literal);
#endif // MCP_HAS_METASOUND_LITERAL_HELPER

    const FMetasoundFrontendNode* InputNode = Builder.AddGraphInput(ClassInput);
    const FString InputNodeId = InputNode ? InputNode->GetID().ToString() : FString();

    // Flush the builder before the (now real) disk write below — see add_metasound_node.
    PW_METASOUND_FINISH_BUILDING(Builder);

    if (InputNode)
    {
        TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
        Result->SetStringField(TEXT("inputName"), InputName);
        Result->SetStringField(TEXT("inputType"), InputType);
        Result->SetStringField(TEXT("nodeId"), InputNodeId);
        Result->SetStringField(TEXT("message"), FString::Printf(TEXT("MetaSound input '%s' added"), *InputName));
        PinWright::MetaSound::SaveMetaSoundAndReport(Result, MetaSound, bSave);
        AddAssetVerification(Result, MetaSound);
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_INPUT_FAILED, FString::Printf(TEXT("Failed to add input '%s' - type '%s' may not be valid"), *InputName, *InputType));
    }

    return true;
#elif MCP_HAS_METASOUND
    FString InputName = Ctx.GetString(TEXT("inputName"));
    FString InputType = Ctx.GetString(TEXT("inputType"), TEXT("Float"));

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("inputName"), InputName);
    Result->SetStringField(TEXT("inputType"), InputType);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("MetaSound input '%s' noted"), *InputName));
    Result->SetStringField(TEXT("note"), TEXT("MetaSound Frontend Builder not available - upgrade to UE 5.3+ for full support"));
    Ctx.SendSuccess(Result);
    return true;
#else
    Ctx.SendError(ErrorCodes::ERR_METASOUND_NOT_AVAILABLE, TEXT("MetaSound support not available"));
    return true;
#endif
}

// ---- audio.authoring.add_metasound_output ----
REGISTER_RPC_HANDLER("audio.authoring.add_metasound_output", "audio.authoring", "Add a graph output to a MetaSound",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the MetaSound"),
        RPC_PARAM_REQ("outputName", "string", "Name of the output"),
        RPC_PARAM_DEF("outputType", "string", "Type name (Audio, Float, etc.)", "Audio"),
        RPC_PARAM_DEF("save", "boolean", "Save after modification", "true")
    ))
{
#if MCP_HAS_METASOUND && MCP_HAS_METASOUND_FRONTEND
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString OutputName;
    if (!Ctx.RequireString(TEXT("outputName"), OutputName)) return true;

    FString OutputType = Ctx.GetString(TEXT("outputType"), TEXT("Audio"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PATH, TEXT("Asset path is required"));
        return true;
    }

    // Load gate accepts Source or Patch — see LoadMetaSoundDocumentObject.
    UObject* MetaSound = PinWright::MetaSound::LoadMetaSoundDocumentObject(AssetPath);
    if (!MetaSound)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, FString::Printf(TEXT("Could not load MetaSound: %s"), *AssetPath));
        return true;
    }

    TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface(MetaSound);
    PW_METASOUND_MAKE_BUILDER(Builder, ScriptInterface);

    FMetasoundFrontendClassOutput ClassOutput;
    ClassOutput.Name = FName(*OutputName);
    // Resolve the documented convenience name (e.g. "Int") to the registry's
    // canonical data-type key (e.g. "Int32") — same mapping the variable handler
    // uses, so "Int" outputs are accepted consistently with "Int" variables.
    ClassOutput.TypeName = FName(*PinWright::MetaSound::CanonicalizeMetaSoundTypeName(OutputType));
    ClassOutput.VertexID = FGuid::NewGuid();
    ClassOutput.NodeID = FGuid::NewGuid();
    ClassOutput.AccessType = EMetasoundFrontendVertexAccessType::Reference;

    const FMetasoundFrontendNode* OutputNode = Builder.AddGraphOutput(ClassOutput);
    const FString OutputNodeId = OutputNode ? OutputNode->GetID().ToString() : FString();

    // Flush the builder before the (now real) disk write below — see add_metasound_node.
    PW_METASOUND_FINISH_BUILDING(Builder);

    if (OutputNode)
    {
        TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
        Result->SetStringField(TEXT("outputName"), OutputName);
        Result->SetStringField(TEXT("outputType"), OutputType);
        Result->SetStringField(TEXT("nodeId"), OutputNodeId);
        Result->SetStringField(TEXT("message"), FString::Printf(TEXT("MetaSound output '%s' added"), *OutputName));
        PinWright::MetaSound::SaveMetaSoundAndReport(Result, MetaSound, bSave);
        AddAssetVerification(Result, MetaSound);
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_OUTPUT_FAILED, FString::Printf(TEXT("Failed to add output '%s' - type '%s' may not be valid"), *OutputName, *OutputType));
    }

    return true;
#elif MCP_HAS_METASOUND
    FString OutputName = Ctx.GetString(TEXT("outputName"));
    FString OutputType = Ctx.GetString(TEXT("outputType"), TEXT("Audio"));

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("outputName"), OutputName);
    Result->SetStringField(TEXT("outputType"), OutputType);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("MetaSound output '%s' noted"), *OutputName));
    Result->SetStringField(TEXT("note"), TEXT("MetaSound Frontend Builder not available - upgrade to UE 5.3+ for full support"));
    Ctx.SendSuccess(Result);
    return true;
#else
    Ctx.SendError(ErrorCodes::ERR_METASOUND_NOT_AVAILABLE, TEXT("MetaSound support not available"));
    return true;
#endif
}

// ---- audio.authoring.set_metasound_default ----
REGISTER_RPC_HANDLER("audio.authoring.set_metasound_default", "audio.authoring", "Set the default value of a MetaSound graph input. Exactly one value param is required — there is no default default. objectValue binds an asset (e.g. a USoundWave to a WaveAsset input) and is validated against the input's declared data type; an array-typed input (e.g. WaveAsset:Array) takes arrayValue instead.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the MetaSound"),
        RPC_PARAM_REQ("inputName", "string", "Name of the input"),
        RPC_PARAM_OPT("floatValue", "number", "Float default value"),
        RPC_PARAM_OPT("intValue", "integer", "Integer default value"),
        RPC_PARAM_OPT("boolValue", "boolean", "Boolean default value"),
        RPC_PARAM_OPT("stringValue", "string", "String default value"),
        RPC_PARAM_OPT("objectValue", "path", "Asset path of the object to bind (alias: objectPath). NOT assetPath, which names the MetaSound being edited."),
        RPC_PARAM_OPT("arrayValue", "array", "Values for an ARRAY-typed target, as a JSON array whose entries match the element type: numbers for Float:Array / Int32:Array, booleans for Bool:Array, strings for String:Array, asset paths for object arrays such as WaveAsset:Array. This is the ONLY param an array-typed target accepts, and a scalar-typed target refuses it. An empty array clears the value. Required to reach the Array.* node family (Array.Random Get, Array.Shuffle, Array.Get/Set/Concat), whose pins and the Weights pin of Array.Random Get are all array-typed."),
        RPC_PARAM_DEF("save", "boolean", "Save after modification", "true")
    ))
{
#if MCP_HAS_METASOUND && MCP_HAS_METASOUND_FRONTEND
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString InputName;
    if (!Ctx.RequireString(TEXT("inputName"), InputName)) return true;

    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PATH, TEXT("Asset path is required"));
        return true;
    }

    // Load gate accepts Source or Patch — see LoadMetaSoundDocumentObject.
    UObject* MetaSound = PinWright::MetaSound::LoadMetaSoundDocumentObject(AssetPath);
    if (!MetaSound)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, FString::Printf(TEXT("Could not load MetaSound: %s"), *AssetPath));
        return true;
    }

    TScriptInterface<IMetaSoundDocumentInterface> ScriptInterface(MetaSound);
    PW_METASOUND_MAKE_BUILDER(Builder, ScriptInterface);

    // Resolve the input up-front: its declared data type is what objectValue is validated
    // against, and naming a missing input here is a better answer than SetGraphInputDefault's
    // bare false.
    const FMetasoundFrontendClassInput* TargetInput = Builder.FindGraphInput(FName(*InputName));
    if (!TargetInput)
    {
        Ctx.SendError(ErrorCodes::ERR_INPUT_NOT_FOUND,
            FString::Printf(TEXT("MetaSound '%s' has no graph input named '%s'. List inputs with audio.authoring.describe_metasound (rootGraph.interface.inputs)."),
                *AssetPath, *InputName));
        PW_METASOUND_FINISH_BUILDING(Builder);
        return true;
    }
    const FString InputTypeName = TargetInput->TypeName.ToString();

    FMetasoundFrontendLiteral Literal;
    // A *set* verb with no value is a caller mistake, not a request to reset to the type
    // default. The removed fallback here was Literal.Set(0.0f), which wrote a float zero into
    // whatever type the input actually was and then reported success — a value the caller never
    // asked for, published as the value they did (rpc-design §1/§3).
    const PinWright::MetaSound::EMetaSoundLiteralParamOutcome LiteralOutcome =
        PinWright::MetaSound::BuildMetaSoundLiteralFromParams(Ctx, InputTypeName,
            /*bAllowTypeDefault*/ false, Literal);
    if (LiteralOutcome == PinWright::MetaSound::EMetaSoundLiteralParamOutcome::ErrorSent)
    {
        PW_METASOUND_FINISH_BUILDING(Builder);
        return true;
    }
    if (LiteralOutcome != PinWright::MetaSound::EMetaSoundLiteralParamOutcome::Ok)
    {
        PinWright::MetaSound::SendMetaSoundMissingValueError(Ctx,
            FString::Printf(TEXT("input '%s'"), *InputName), InputTypeName);
        PW_METASOUND_FINISH_BUILDING(Builder);
        return true;
    }

    const bool bSetAccepted = Builder.SetGraphInputDefault(FName(*InputName), Literal);

    // Commit the builder's caches before anything reads the document or saves it — see
    // audio.authoring.metasound_gotchas ("FinishBuilding() must be called before save").
    PW_METASOUND_FINISH_BUILDING(Builder);

    // Read the stored default back off the DOCUMENT, not through the builder that wrote it,
    // so the reported value cannot be an echo of the setter's own argument (rpc-design §4).
    FMetasoundFrontendLiteral StoredLiteral;
    bool bReadBack = false;
    if (const IMetaSoundDocumentInterface* DocInterface = Cast<IMetaSoundDocumentInterface>(MetaSound))
    {
        const FMetasoundFrontendDocument& Doc = PW_METASOUND_GET_CONST_DOCUMENT(DocInterface);
        const FMetasoundFrontendClassInterface& ClassInterface =
            PinWright::MetaSound::GetClassDefaultInterface(Doc.RootGraph);
        const FName InputFName(*InputName);
        for (const FMetasoundFrontendClassInput& DocInput : ClassInterface.Inputs)
        {
            if (DocInput.Name != InputFName)
            {
                continue;
            }
            if (const FMetasoundFrontendLiteral* Found = PinWright::MetaSound::FindClassInputDefault(DocInput))
            {
                StoredLiteral = *Found;
                bReadBack = true;
            }
            break;
        }
    }

    const bool bStoredMatches = bReadBack && StoredLiteral.IsEqual(Literal);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("inputName"), InputName);
    Result->SetStringField(TEXT("inputType"), InputTypeName);
    Result->SetBoolField(TEXT("readBack"), bReadBack);
    if (bReadBack)
    {
        TSharedPtr<FJsonObject> StoredJson = MakeShareable(new FJsonObject());
        PinWright::MetaSound::DescribeMetaSoundLiteral(StoredLiteral, StoredJson);
        Result->SetObjectField(TEXT("storedDefault"), StoredJson);
    }

    if (!bSetAccepted || !bStoredMatches)
    {
        Result->SetBoolField(TEXT("setAccepted"), bSetAccepted);
        Ctx.SendError(ErrorCodes::ERR_SET_DEFAULT_FAILED,
            FString::Printf(TEXT("Default for input '%s' (type '%s') did not land: builder %s and the document read back %s. Check the value's type against the input's."),
                *InputName, *InputTypeName,
                bSetAccepted ? TEXT("accepted the write") : TEXT("rejected the write"),
                bReadBack ? TEXT("a different literal") : TEXT("no default at all")),
            Result);
        return true;
    }

    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("MetaSound default for '%s' set"), *InputName));
    PinWright::MetaSound::SaveMetaSoundAndReport(Result, MetaSound, bSave);
    AddAssetVerification(Result, MetaSound);
    Ctx.SendSuccess(Result);
    return true;
#elif MCP_HAS_METASOUND
    FString InputName = Ctx.GetString(TEXT("inputName"));

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("MetaSound default for '%s' noted"), *InputName));
    Result->SetStringField(TEXT("note"), TEXT("MetaSound Frontend Builder not available - upgrade to UE 5.3+ for full support"));
    Ctx.SendSuccess(Result);
    return true;
#else
    Ctx.SendError(ErrorCodes::ERR_METASOUND_NOT_AVAILABLE, TEXT("MetaSound support not available"));
    return true;
#endif
}

// =========================================================================
// 11.3 Sound Classes & Mixes (6 actions)
// =========================================================================

// ---- audio.authoring.create_sound_class ----
REGISTER_RPC_HANDLER("audio.authoring.create_sound_class", "audio.authoring", "Create a USoundClass asset (groups sounds for shared volume/pitch/EQ control). Optionally seed initial volume/pitch and a parent class so the new class inherits its hierarchy position.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name of the SoundClass"),
        RPC_PARAM_OPT("path", "path", "Content path (default /Game/Audio/Classes)"),
        RPC_PARAM_DEF("volume", "number", "Initial volume", "1.0"),
        RPC_PARAM_DEF("pitch", "number", "Initial pitch", "1.0"),
        RPC_PARAM_OPT("parentClass", "path", "Path to a parent SoundClass asset; this class becomes a child of it."),
        RPC_PARAM_DEF("save", "boolean", "Save after creation", "true")
    ))
{
    FString Name;
    if (!Ctx.RequireString(TEXT("name"), Name)) return true;

    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/Audio/Classes")));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    FString PackagePath;
    if (!PinWrightAudioPackagePath::ComposeAudioAssetPackagePathOrRefuse(Ctx, Path, Name, PackagePath))
    {
        return true;
    }

    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(ErrorCodes::ERR_PACKAGE_ERROR, TEXT("Failed to create package"));
        return true;
    }

    USoundClass* NewClass = NewObject<USoundClass>(Package, FName(*Name), RF_Public | RF_Standalone);
    if (!NewClass)
    {
        Ctx.SendError(ErrorCodes::ERR_CREATE_FAILED, TEXT("Failed to create SoundClass"));
        return true;
    }

    NewClass->Properties.Volume = static_cast<float>(Ctx.GetNumber(TEXT("volume"), 1.0));
    NewClass->Properties.Pitch = static_cast<float>(Ctx.GetNumber(TEXT("pitch"), 1.0));

    FString ParentClassPath = Ctx.GetString(TEXT("parentClass"));
    USoundClass* Parent = ParentClassPath.IsEmpty() ? nullptr : LoadSoundClassFromPath(ParentClassPath);
    bool bSavedToDisk = false;
    if (Parent)
    {
        // Maintain both sides of the link (child's ParentClass AND parent's ChildClasses),
        // not just the raw child-side pointer. A fresh class has no descendants, so the
        // cycle guard inside cannot trip here. The helper force-saves the child (and both
        // parents) and reports the child's on-disk result, so it is not saved again below.
        SetSoundClassParentMaintainingChildren(NewClass, Parent, bSave, &bSavedToDisk);
    }
    else
    {
        bSavedToDisk = SaveAudioAsset(NewClass, bSave);
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("assetPath"), NewClass->GetPathName());
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("SoundClass '%s' created"), *Name));
    AddAssetSaveReport(Result, bSave, bSavedToDisk);
    AddAssetVerification(Result, NewClass);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- audio.authoring.set_class_properties ----
REGISTER_RPC_HANDLER("audio.authoring.set_class_properties", "audio.authoring", "Set properties on a SoundClass",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the SoundClass"),
        RPC_PARAM_OPT("volume", "number", "Volume"),
        RPC_PARAM_OPT("pitch", "number", "Pitch"),
        RPC_PARAM_OPT("lowPassFilterFrequency", "number", "Low pass filter frequency"),
        RPC_PARAM_OPT("lfeBleed", "number", "LFE bleed amount"),
        RPC_PARAM_OPT("voiceCenterChannelVolume", "number", "Center channel volume"),
        RPC_PARAM_OPT("parentSubmix", "path", "Asset path of a USoundSubmix to route this class's output into (empty clears)"),
        RPC_PARAM_DEF("save", "boolean", "Save after modification", "true")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    USoundClass* SoundClass = LoadSoundClassFromPath(AssetPath);
    if (!SoundClass)
    {
        Ctx.SendError(ErrorCodes::ERR_CLASS_NOT_FOUND, FString::Printf(TEXT("Could not load SoundClass: %s"), *AssetPath));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();
    if (Payload->HasField(TEXT("volume")))
        SoundClass->Properties.Volume = static_cast<float>(Ctx.GetNumber(TEXT("volume"), 1.0));
    if (Payload->HasField(TEXT("pitch")))
        SoundClass->Properties.Pitch = static_cast<float>(Ctx.GetNumber(TEXT("pitch"), 1.0));
    if (Payload->HasField(TEXT("lowPassFilterFrequency")))
        SoundClass->Properties.LowPassFilterFrequency = static_cast<float>(Ctx.GetNumber(TEXT("lowPassFilterFrequency"), 20000.0));
    if (Payload->HasField(TEXT("lfeBleed")))
        SoundClass->Properties.LFEBleed = static_cast<float>(Ctx.GetNumber(TEXT("lfeBleed"), 0.5));
    if (Payload->HasField(TEXT("voiceCenterChannelVolume")))
        SoundClass->Properties.VoiceCenterChannelVolume = static_cast<float>(Ctx.GetNumber(TEXT("voiceCenterChannelVolume"), 0.0));
#if MCP_HAS_SUBMIX
    if (Payload->HasField(TEXT("parentSubmix")))
    {
        FString SubmixPath = Ctx.GetString(TEXT("parentSubmix"));
        if (SubmixPath.IsEmpty())
        {
            SoundClass->Properties.DefaultSubmix = nullptr;
        }
        else
        {
            SoundClass->Properties.DefaultSubmix = LoadSoundSubmixFromPath(SubmixPath);
        }
    }
#endif

    SaveAudioAsset(SoundClass, bSave);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("message"), TEXT("Sound class properties updated"));
    AddAssetVerification(Result, SoundClass);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- audio.authoring.set_class_parent ----
REGISTER_RPC_HANDLER("audio.authoring.set_class_parent", "audio.authoring", "Set the parent SoundClass",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the SoundClass"),
        RPC_PARAM_OPT("parentPath", "path", "Asset path of the parent (empty to clear)"),
        RPC_PARAM_DEF("save", "boolean", "Save after modification", "true")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString ParentPath = Ctx.GetString(TEXT("parentPath"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    USoundClass* SoundClass = LoadSoundClassFromPath(AssetPath);
    if (!SoundClass)
    {
        Ctx.SendError(ErrorCodes::ERR_CLASS_NOT_FOUND, FString::Printf(TEXT("Could not load SoundClass: %s"), *AssetPath));
        return true;
    }

    USoundClass* NewParent = nullptr;
    if (!ParentPath.IsEmpty())
    {
        NewParent = LoadSoundClassFromPath(ParentPath);
        if (!NewParent)
        {
            Ctx.SendError(ErrorCodes::ERR_PARENT_CLASS_NOT_FOUND, FString::Printf(TEXT("Could not load parent SoundClass: %s"), *ParentPath));
            return true;
        }
    }

    // Maintain both sides of the hierarchy: clear the old parent's ChildClasses entry, set the
    // child's ParentClass, and add to the new parent's ChildClasses. Rejects cycles.
    if (!SetSoundClassParentMaintainingChildren(SoundClass, NewParent, bSave))
    {
        Ctx.SendError(ErrorCodes::ERR_PARENT_CYCLE, TEXT("Reparenting would create a cycle in the SoundClass hierarchy"));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("message"), TEXT("Sound class parent updated"));
    AddAssetVerification(Result, SoundClass);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- audio.authoring.set_submix_parent ----
REGISTER_RPC_HANDLER("audio.authoring.set_submix_parent", "audio.authoring", "Set the parent SoundSubmix for a USoundSubmix asset (mirrors set_class_parent)",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the SoundSubmix"),
        RPC_PARAM_OPT("parentPath", "path", "Asset path of the parent SoundSubmix (empty to clear)"),
        RPC_PARAM_DEF("save", "boolean", "Save after modification", "true")
    ))
{
#if MCP_HAS_SUBMIX
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString ParentPath = Ctx.GetString(TEXT("parentPath"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    USoundSubmix* Submix = LoadSoundSubmixFromPath(AssetPath);
    if (!Submix)
    {
        Ctx.SendError(ErrorCodes::ERR_SUBMIX_NOT_FOUND, FString::Printf(TEXT("Could not load SoundSubmix: %s"), *AssetPath));
        return true;
    }

    USoundSubmixBase* NewParent = nullptr;
    if (!ParentPath.IsEmpty())
    {
        NewParent = LoadSoundSubmixFromPath(ParentPath);
        if (!NewParent)
        {
            Ctx.SendError(ErrorCodes::ERR_PARENT_SUBMIX_NOT_FOUND, FString::Printf(TEXT("Could not load USoundSubmix at path: %s"), *ParentPath));
            return true;
        }
    }

    // Maintains both sides of the hierarchy (clears the old parent's ChildSubmixes entry, sets
    // ParentSubmix, AddUnique()s to the new parent) and persists every touched submix.
    SetSubmixParentMaintainingChildren(Submix, NewParent, bSave);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("message"), TEXT("Submix parent updated"));
    AddAssetVerification(Result, Submix);
    Ctx.SendSuccess(Result);
    return true;
#else
    Ctx.SendError(ErrorCodes::ERR_SUBMIX_NOT_AVAILABLE, TEXT("USoundSubmix support not compiled into this build"));
    return true;
#endif
}

// Single-source the FSoundClassAdjuster field+default mapping shared by
// create_sound_mix's seed loop and add_mix_modifier's append path. Both used to
// build this struct by hand with the same per-field defaults (1.0 / 1.0 / true);
// the seed copy silently dropped bApplyToChildren, which is the drift this helper
// prevents — a new adjuster field is added here once and both paths stay in sync
// (mirroring BuildMixEqJson on the read side). Callers resolve the SoundClass and
// parse their own JSON source keys, then hand the values in.
static FSoundClassAdjuster BuildClassAdjuster(USoundClass* SoundClass, double Volume, double Pitch, bool bApplyToChildren)
{
    FSoundClassAdjuster Adjuster;
    Adjuster.SoundClassObject = SoundClass;
    Adjuster.VolumeAdjuster = static_cast<float>(Volume);
    Adjuster.PitchAdjuster = static_cast<float>(Pitch);
    Adjuster.bApplyToChildren = bApplyToChildren;
    return Adjuster;
}

// ---- audio.authoring.create_sound_mix ----
REGISTER_RPC_HANDLER("audio.authoring.create_sound_mix", "audio.authoring", "Create a USoundMix asset (a stackable mixdown profile applying volume/pitch adjusters per SoundClass). Activate at runtime via audio.push_sound_mix; optionally seed adjusters at creation via classAdjusters.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name of the SoundMix"),
        RPC_PARAM_OPT("path", "path", "Content path (default /Game/Audio/Mixes)"),
        RPC_PARAM_OPT("classAdjusters", "array", "Array of {soundClass, volumeAdjuster, pitchAdjuster, applyToChildren} entries seeded onto the new mix (applyToChildren is optional, default true, matching add_mix_modifier)"),
        RPC_PARAM_OPT("fadeInTime", "number", "Mix-level fade-in time in seconds (USoundMix::FadeInTime)"),
        RPC_PARAM_OPT("fadeOutTime", "number", "Mix-level fade-out time in seconds (USoundMix::FadeOutTime)"),
        RPC_PARAM_DEF("save", "boolean", "Save after creation", "true")
    ))
{
    FString Name;
    if (!Ctx.RequireString(TEXT("name"), Name)) return true;

    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/Audio/Mixes")));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    FString PackagePath;
    if (!PinWrightAudioPackagePath::ComposeAudioAssetPackagePathOrRefuse(Ctx, Path, Name, PackagePath))
    {
        return true;
    }

    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(ErrorCodes::ERR_PACKAGE_ERROR, TEXT("Failed to create package"));
        return true;
    }

    USoundMixFactory* Factory = NewObject<USoundMixFactory>();
    USoundMix* NewMix = Cast<USoundMix>(
        Factory->FactoryCreateNew(USoundMix::StaticClass(), Package,
                                  FName(*Name), RF_Public | RF_Standalone,
                                  nullptr, GWarn));
    if (!NewMix)
    {
        Ctx.SendError(ErrorCodes::ERR_CREATE_FAILED, TEXT("Failed to create SoundMix"));
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* Adjusters = Ctx.GetArray(TEXT("classAdjusters"));
    if (Adjusters)
    {
        for (const TSharedPtr<FJsonValue>& Val : *Adjusters)
        {
            const TSharedPtr<FJsonObject> AdjObj = Val->AsObject();
            if (!AdjObj.IsValid()) continue;
            FString ClassPath;
            if (!AdjObj->TryGetStringField(TEXT("soundClass"), ClassPath)) continue;
            USoundClass* SC = LoadSoundClassFromPath(ClassPath);
            if (!SC) continue;

            // applyToChildren defaults true to match add_mix_modifier (the documented sibling
            // append path); the struct constructor would otherwise zero-init bApplyToChildren to
            // false, so a seeded adjuster would silently fail to propagate to child classes.
            // Field list + defaults are single-sourced through BuildClassAdjuster so the two
            // adjuster-creation paths cannot drift on a future field.
            NewMix->SoundClassEffects.Add(BuildClassAdjuster(
                SC,
                GetJsonNumberField(AdjObj, TEXT("volumeAdjuster"), 1.0),
                GetJsonNumberField(AdjObj, TEXT("pitchAdjuster"), 1.0),
                GetJsonBoolField(AdjObj, TEXT("applyToChildren"), true)));
        }
    }

    // Mix-level fade envelopes (USoundMix::FadeInTime/FadeOutTime) — optional at creation,
    // default to the factory value so omitting them leaves the engine default untouched.
    NewMix->FadeInTime = static_cast<float>(Ctx.GetNumber(TEXT("fadeInTime"), NewMix->FadeInTime));
    NewMix->FadeOutTime = static_cast<float>(Ctx.GetNumber(TEXT("fadeOutTime"), NewMix->FadeOutTime));

    const bool bSavedToDisk = SaveAudioAsset(NewMix, bSave);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("assetPath"), NewMix->GetPathName());
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("SoundMix '%s' created"), *Name));
    Result->SetNumberField(TEXT("fadeInTime"), NewMix->FadeInTime);
    Result->SetNumberField(TEXT("fadeOutTime"), NewMix->FadeOutTime);
    AddAssetSaveReport(Result, bSave, bSavedToDisk);
    AddAssetVerification(Result, NewMix);
    Ctx.SendSuccess(Result);
    return true;
}

// Build the mix-level EQ readback (applyEQ / eqPriority + the full 4-band
// FEQSettings ladder configure_mix_eq writes). Shared by configure_mix_eq's
// success echo and describe_sound_mix's readback so the write-side echo and the
// read-side describe can never drift on key names or band coverage.
static TSharedPtr<FJsonObject> BuildMixEqJson(const USoundMix* Mix)
{
    TSharedPtr<FJsonObject> EQInfo = MakeShareable(new FJsonObject());
    EQInfo->SetBoolField(TEXT("applyEQ"), Mix->bApplyEQ != 0);
    EQInfo->SetNumberField(TEXT("eqPriority"), Mix->EQPriority);
    EQInfo->SetNumberField(TEXT("frequencyCenter0"), Mix->EQSettings.FrequencyCenter0);
    EQInfo->SetNumberField(TEXT("gain0"), Mix->EQSettings.Gain0);
    EQInfo->SetNumberField(TEXT("bandwidth0"), Mix->EQSettings.Bandwidth0);
    EQInfo->SetNumberField(TEXT("frequencyCenter1"), Mix->EQSettings.FrequencyCenter1);
    EQInfo->SetNumberField(TEXT("gain1"), Mix->EQSettings.Gain1);
    EQInfo->SetNumberField(TEXT("bandwidth1"), Mix->EQSettings.Bandwidth1);
    EQInfo->SetNumberField(TEXT("frequencyCenter2"), Mix->EQSettings.FrequencyCenter2);
    EQInfo->SetNumberField(TEXT("gain2"), Mix->EQSettings.Gain2);
    EQInfo->SetNumberField(TEXT("bandwidth2"), Mix->EQSettings.Bandwidth2);
    EQInfo->SetNumberField(TEXT("frequencyCenter3"), Mix->EQSettings.FrequencyCenter3);
    EQInfo->SetNumberField(TEXT("gain3"), Mix->EQSettings.Gain3);
    EQInfo->SetNumberField(TEXT("bandwidth3"), Mix->EQSettings.Bandwidth3);
    return EQInfo;
}

// ---- audio.authoring.add_mix_modifier ----
REGISTER_RPC_HANDLER("audio.authoring.add_mix_modifier", "audio.authoring", "Add a sound class modifier to a SoundMix",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the SoundMix"),
        RPC_PARAM_REQ("soundClassPath", "path", "Asset path of the SoundClass to modify"),
        RPC_PARAM_DEF("volumeAdjuster", "number", "Volume adjustment", "1.0"),
        RPC_PARAM_DEF("pitchAdjuster", "number", "Pitch adjustment", "1.0"),
        RPC_PARAM_DEF("fadeInTime", "number", "Fade in time", "0"),
        RPC_PARAM_DEF("fadeOutTime", "number", "Fade out time", "0"),
        RPC_PARAM_DEF("applyToChildren", "boolean", "Apply to child classes", "true"),
        RPC_PARAM_DEF("save", "boolean", "Save after modification", "true")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString SoundClassPath = Ctx.GetString(TEXT("soundClassPath"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    USoundMix* Mix = LoadSoundMixFromPath(AssetPath);
    if (!Mix)
    {
        Ctx.SendError(ErrorCodes::ERR_MIX_NOT_FOUND, FString::Printf(TEXT("Could not load SoundMix: %s"), *AssetPath));
        return true;
    }

    USoundClass* SoundClass = LoadSoundClassFromPath(SoundClassPath);
    if (!SoundClass)
    {
        Ctx.SendError(ErrorCodes::ERR_CLASS_NOT_FOUND, FString::Printf(TEXT("Could not load SoundClass: %s"), *SoundClassPath));
        return true;
    }

    Mix->SoundClassEffects.Add(BuildClassAdjuster(
        SoundClass,
        Ctx.GetNumber(TEXT("volumeAdjuster"), 1.0),
        Ctx.GetNumber(TEXT("pitchAdjuster"), 1.0),
        Ctx.GetBool(TEXT("applyToChildren"), true)));

    // fadeInTime/fadeOutTime are mix-level fade envelopes (USoundMix::FadeInTime/FadeOutTime),
    // not per-FSoundClassAdjuster fields. Persist them onto the mix asset so the documented,
    // accepted params are not silently dropped. Guard with HasField so an add_mix_modifier call
    // that omits a fade leaves a previously-set one untouched (matches configure_mix_eq below).
    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();
    if (Payload->HasField(TEXT("fadeInTime")))
        Mix->FadeInTime = static_cast<float>(Ctx.GetNumber(TEXT("fadeInTime"), 0.0));
    if (Payload->HasField(TEXT("fadeOutTime")))
        Mix->FadeOutTime = static_cast<float>(Ctx.GetNumber(TEXT("fadeOutTime"), 0.0));

    SaveAudioAsset(Mix, bSave);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("message"), TEXT("Mix modifier added"));
    Result->SetNumberField(TEXT("fadeInTime"), Mix->FadeInTime);
    Result->SetNumberField(TEXT("fadeOutTime"), Mix->FadeOutTime);
    AddAssetVerification(Result, Mix);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- audio.authoring.configure_mix_eq ----
REGISTER_RPC_HANDLER("audio.authoring.configure_mix_eq", "audio.authoring", "Configure EQ settings on a SoundMix",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the SoundMix"),
        RPC_PARAM_DEF("applyEQ", "boolean", "Enable EQ", "true"),
        RPC_PARAM_OPT("eqPriority", "number", "EQ priority"),
        RPC_PARAM_OPT("eqSettings", "object", "4-band EQ {frequencyCenter0..3, gain0..3, bandwidth0..3}"),
        RPC_PARAM_OPT("lowFrequency", "number", "Flat API: low band frequency"),
        RPC_PARAM_OPT("lowGain", "number", "Flat API: low band gain"),
        RPC_PARAM_OPT("midFrequency", "number", "Flat API: mid band frequency"),
        RPC_PARAM_OPT("midGain", "number", "Flat API: mid band gain"),
        RPC_PARAM_OPT("highMidFrequency", "number", "Flat API: high-mid band frequency"),
        RPC_PARAM_OPT("highMidGain", "number", "Flat API: high-mid band gain"),
        RPC_PARAM_OPT("highFrequency", "number", "Flat API: high band frequency"),
        RPC_PARAM_OPT("highGain", "number", "Flat API: high band gain"),
        RPC_PARAM_DEF("save", "boolean", "Save after modification", "true")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    USoundMix* Mix = LoadSoundMixFromPath(AssetPath);
    if (!Mix)
    {
        Ctx.SendError(ErrorCodes::ERR_MIX_NOT_FOUND, FString::Printf(TEXT("Could not load SoundMix: %s"), *AssetPath));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();
    Mix->bApplyEQ = Ctx.GetBool(TEXT("applyEQ"), true);

    if (Payload->HasField(TEXT("eqPriority")))
        Mix->EQPriority = static_cast<float>(Ctx.GetNumber(TEXT("eqPriority"), 1.0));

    // EQ settings object - 4 bands available
    TSharedPtr<FJsonObject> EQObj = Ctx.GetObject(TEXT("eqSettings"));
    if (EQObj.IsValid())
    {
        if (EQObj->HasField(TEXT("frequencyCenter0")))
            Mix->EQSettings.FrequencyCenter0 = static_cast<float>(GetJsonNumberField(EQObj, TEXT("frequencyCenter0")));
        if (EQObj->HasField(TEXT("gain0")))
            Mix->EQSettings.Gain0 = static_cast<float>(GetJsonNumberField(EQObj, TEXT("gain0")));
        if (EQObj->HasField(TEXT("bandwidth0")))
            Mix->EQSettings.Bandwidth0 = static_cast<float>(GetJsonNumberField(EQObj, TEXT("bandwidth0")));

        if (EQObj->HasField(TEXT("frequencyCenter1")))
            Mix->EQSettings.FrequencyCenter1 = static_cast<float>(GetJsonNumberField(EQObj, TEXT("frequencyCenter1")));
        if (EQObj->HasField(TEXT("gain1")))
            Mix->EQSettings.Gain1 = static_cast<float>(GetJsonNumberField(EQObj, TEXT("gain1")));
        if (EQObj->HasField(TEXT("bandwidth1")))
            Mix->EQSettings.Bandwidth1 = static_cast<float>(GetJsonNumberField(EQObj, TEXT("bandwidth1")));

        if (EQObj->HasField(TEXT("frequencyCenter2")))
            Mix->EQSettings.FrequencyCenter2 = static_cast<float>(GetJsonNumberField(EQObj, TEXT("frequencyCenter2")));
        if (EQObj->HasField(TEXT("gain2")))
            Mix->EQSettings.Gain2 = static_cast<float>(GetJsonNumberField(EQObj, TEXT("gain2")));
        if (EQObj->HasField(TEXT("bandwidth2")))
            Mix->EQSettings.Bandwidth2 = static_cast<float>(GetJsonNumberField(EQObj, TEXT("bandwidth2")));

        if (EQObj->HasField(TEXT("frequencyCenter3")))
            Mix->EQSettings.FrequencyCenter3 = static_cast<float>(GetJsonNumberField(EQObj, TEXT("frequencyCenter3")));
        if (EQObj->HasField(TEXT("gain3")))
            Mix->EQSettings.Gain3 = static_cast<float>(GetJsonNumberField(EQObj, TEXT("gain3")));
        if (EQObj->HasField(TEXT("bandwidth3")))
            Mix->EQSettings.Bandwidth3 = static_cast<float>(GetJsonNumberField(EQObj, TEXT("bandwidth3")));
    }
    else
    {
        // Accept flat parameters for simpler API usage
        if (Payload->HasField(TEXT("lowFrequency")))
            Mix->EQSettings.FrequencyCenter0 = static_cast<float>(Ctx.GetNumber(TEXT("lowFrequency"), 600.0));
        if (Payload->HasField(TEXT("lowGain")))
            Mix->EQSettings.Gain0 = static_cast<float>(Ctx.GetNumber(TEXT("lowGain"), 1.0));
        if (Payload->HasField(TEXT("midFrequency")))
            Mix->EQSettings.FrequencyCenter1 = static_cast<float>(Ctx.GetNumber(TEXT("midFrequency"), 1000.0));
        if (Payload->HasField(TEXT("midGain")))
            Mix->EQSettings.Gain1 = static_cast<float>(Ctx.GetNumber(TEXT("midGain"), 1.0));
        if (Payload->HasField(TEXT("highMidFrequency")))
            Mix->EQSettings.FrequencyCenter2 = static_cast<float>(Ctx.GetNumber(TEXT("highMidFrequency"), 2000.0));
        if (Payload->HasField(TEXT("highMidGain")))
            Mix->EQSettings.Gain2 = static_cast<float>(Ctx.GetNumber(TEXT("highMidGain"), 1.0));
        if (Payload->HasField(TEXT("highFrequency")))
            Mix->EQSettings.FrequencyCenter3 = static_cast<float>(Ctx.GetNumber(TEXT("highFrequency"), 10000.0));
        if (Payload->HasField(TEXT("highGain")))
            Mix->EQSettings.Gain3 = static_cast<float>(Ctx.GetNumber(TEXT("highGain"), 1.0));
    }

    // Clamp EQ values to valid ranges
    auto ClampGain = [](float& Value) { Value = FMath::Clamp(Value, 0.0f, 4.0f); };
    auto ClampFreq = [](float& Value) { Value = FMath::Clamp(Value, 0.0f, 20000.0f); };
    auto ClampBandwidth = [](float& Value) { Value = FMath::Clamp(Value, 0.0f, 2.0f); };
    ClampGain(Mix->EQSettings.Gain0); ClampGain(Mix->EQSettings.Gain1);
    ClampGain(Mix->EQSettings.Gain2); ClampGain(Mix->EQSettings.Gain3);
    ClampFreq(Mix->EQSettings.FrequencyCenter0); ClampFreq(Mix->EQSettings.FrequencyCenter1);
    ClampFreq(Mix->EQSettings.FrequencyCenter2); ClampFreq(Mix->EQSettings.FrequencyCenter3);
    ClampBandwidth(Mix->EQSettings.Bandwidth0); ClampBandwidth(Mix->EQSettings.Bandwidth1);
    ClampBandwidth(Mix->EQSettings.Bandwidth2); ClampBandwidth(Mix->EQSettings.Bandwidth3);

    SaveAudioAsset(Mix, bSave);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetObjectField(TEXT("eqSettings"), BuildMixEqJson(Mix));
    Result->SetStringField(TEXT("message"), TEXT("Mix EQ configured"));
    AddAssetVerification(Result, Mix);
    Ctx.SendSuccess(Result);
    return true;
}

// =========================================================================
// 11.4 Attenuation & Spatialization (5 actions)
// =========================================================================

// ---- audio.authoring.create_attenuation_settings ----
REGISTER_RPC_HANDLER("audio.authoring.create_attenuation_settings", "audio.authoring", "Create a new SoundAttenuation asset",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name of the attenuation"),
        RPC_PARAM_OPT("path", "path", "Content path (default /Game/Audio/Attenuation)"),
        RPC_PARAM_OPT("innerRadius", "number", "Inner radius"),
        RPC_PARAM_OPT("falloffDistance", "number", "Falloff distance"),
        RPC_PARAM_DEF("save", "boolean", "Save after creation", "true")
    ))
{
    FString Name;
    if (!Ctx.RequireString(TEXT("name"), Name)) return true;

    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/Audio/Attenuation")));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    FString PackagePath;
    if (!PinWrightAudioPackagePath::ComposeAudioAssetPackagePathOrRefuse(Ctx, Path, Name, PackagePath))
    {
        return true;
    }

    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(ErrorCodes::ERR_PACKAGE_ERROR, TEXT("Failed to create package"));
        return true;
    }

    USoundAttenuationFactory* Factory = NewObject<USoundAttenuationFactory>();
    USoundAttenuation* NewAtten = Cast<USoundAttenuation>(
        Factory->FactoryCreateNew(USoundAttenuation::StaticClass(), Package,
                                  FName(*Name), RF_Public | RF_Standalone,
                                  nullptr, GWarn));
    if (!NewAtten)
    {
        Ctx.SendError(ErrorCodes::ERR_CREATE_FAILED, TEXT("Failed to create SoundAttenuation"));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();
    if (Payload->HasField(TEXT("innerRadius")))
        NewAtten->Attenuation.AttenuationShapeExtents.X = static_cast<float>(Ctx.GetNumber(TEXT("innerRadius"), 400.0));
    if (Payload->HasField(TEXT("falloffDistance")))
        NewAtten->Attenuation.FalloffDistance = static_cast<float>(Ctx.GetNumber(TEXT("falloffDistance"), 3600.0));

    const bool bSavedToDisk = SaveAudioAsset(NewAtten, bSave);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("assetPath"), NewAtten->GetPathName());
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("SoundAttenuation '%s' created"), *Name));
    AddAssetSaveReport(Result, bSave, bSavedToDisk);
    AddAssetVerification(Result, NewAtten);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- audio.authoring.configure_distance_attenuation ----
REGISTER_RPC_HANDLER("audio.authoring.configure_distance_attenuation", "audio.authoring", "Configure distance attenuation settings",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the SoundAttenuation"),
        RPC_PARAM_OPT("innerRadius", "number", "Inner radius"),
        RPC_PARAM_OPT("falloffDistance", "number", "Falloff distance"),
        RPC_PARAM_DEF("distanceAlgorithm", "string", "Algorithm: linear, logarithmic, inverse, naturalsound", "linear"),
        RPC_PARAM_DEF("save", "boolean", "Save after modification", "true")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    USoundAttenuation* Atten = LoadSoundAttenuationFromPath(AssetPath);
    if (!Atten)
    {
        Ctx.SendError(ErrorCodes::ERR_ATTENUATION_NOT_FOUND, FString::Printf(TEXT("Could not load SoundAttenuation: %s"), *AssetPath));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();
    if (Payload->HasField(TEXT("innerRadius")))
        Atten->Attenuation.AttenuationShapeExtents.X = static_cast<float>(Ctx.GetNumber(TEXT("innerRadius"), 400.0));
    if (Payload->HasField(TEXT("falloffDistance")))
        Atten->Attenuation.FalloffDistance = static_cast<float>(Ctx.GetNumber(TEXT("falloffDistance"), 3600.0));

    FString FunctionType = Ctx.GetString(TEXT("distanceAlgorithm"), TEXT("linear")).ToLower();
    if (FunctionType == TEXT("linear"))
        Atten->Attenuation.DistanceAlgorithm = EAttenuationDistanceModel::Linear;
    else if (FunctionType == TEXT("logarithmic"))
        Atten->Attenuation.DistanceAlgorithm = EAttenuationDistanceModel::Logarithmic;
    else if (FunctionType == TEXT("inverse"))
        Atten->Attenuation.DistanceAlgorithm = EAttenuationDistanceModel::Inverse;
    else if (FunctionType == TEXT("naturalsound"))
        Atten->Attenuation.DistanceAlgorithm = EAttenuationDistanceModel::NaturalSound;

    SaveAudioAsset(Atten, bSave);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("message"), TEXT("Distance attenuation configured"));
    AddAssetVerification(Result, Atten);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- audio.authoring.configure_spatialization ----
REGISTER_RPC_HANDLER("audio.authoring.configure_spatialization", "audio.authoring", "Configure spatialization settings",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the SoundAttenuation"),
        RPC_PARAM_DEF("spatialize", "boolean", "Enable spatialization", "true"),
        RPC_PARAM_OPT("spatializationAlgorithm", "string", "Algorithm: panner, hrtf/binaural"),
        RPC_PARAM_DEF("save", "boolean", "Save after modification", "true")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    USoundAttenuation* Atten = LoadSoundAttenuationFromPath(AssetPath);
    if (!Atten)
    {
        Ctx.SendError(ErrorCodes::ERR_ATTENUATION_NOT_FOUND, FString::Printf(TEXT("Could not load SoundAttenuation: %s"), *AssetPath));
        return true;
    }

    Atten->Attenuation.bSpatialize = Ctx.GetBool(TEXT("spatialize"), true);

    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();
    if (Payload->HasField(TEXT("spatializationAlgorithm")))
    {
        FString Algorithm = Ctx.GetString(TEXT("spatializationAlgorithm")).ToLower();
        if (Algorithm == TEXT("panner"))
            Atten->Attenuation.SpatializationAlgorithm = ESoundSpatializationAlgorithm::SPATIALIZATION_Default;
        else if (Algorithm == TEXT("hrtf") || Algorithm == TEXT("binaural"))
            Atten->Attenuation.SpatializationAlgorithm = ESoundSpatializationAlgorithm::SPATIALIZATION_HRTF;
    }

    SaveAudioAsset(Atten, bSave);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("message"), TEXT("Spatialization configured"));
    AddAssetVerification(Result, Atten);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- audio.authoring.configure_occlusion ----
REGISTER_RPC_HANDLER("audio.authoring.configure_occlusion", "audio.authoring", "Configure occlusion settings",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the SoundAttenuation"),
        RPC_PARAM_DEF("enableOcclusion", "boolean", "Enable occlusion", "true"),
        RPC_PARAM_OPT("occlusionLowPassFilterFrequency", "number", "Low pass filter frequency"),
        RPC_PARAM_OPT("occlusionVolumeAttenuation", "number", "Volume attenuation"),
        RPC_PARAM_OPT("occlusionInterpolationTime", "number", "Interpolation time"),
        RPC_PARAM_DEF("save", "boolean", "Save after modification", "true")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    USoundAttenuation* Atten = LoadSoundAttenuationFromPath(AssetPath);
    if (!Atten)
    {
        Ctx.SendError(ErrorCodes::ERR_ATTENUATION_NOT_FOUND, FString::Printf(TEXT("Could not load SoundAttenuation: %s"), *AssetPath));
        return true;
    }

    Atten->Attenuation.bEnableOcclusion = Ctx.GetBool(TEXT("enableOcclusion"), true);

    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();
    if (Payload->HasField(TEXT("occlusionLowPassFilterFrequency")))
        Atten->Attenuation.OcclusionLowPassFilterFrequency = static_cast<float>(Ctx.GetNumber(TEXT("occlusionLowPassFilterFrequency"), 20000.0));
    if (Payload->HasField(TEXT("occlusionVolumeAttenuation")))
        Atten->Attenuation.OcclusionVolumeAttenuation = static_cast<float>(Ctx.GetNumber(TEXT("occlusionVolumeAttenuation"), 0.0));
    if (Payload->HasField(TEXT("occlusionInterpolationTime")))
        Atten->Attenuation.OcclusionInterpolationTime = static_cast<float>(Ctx.GetNumber(TEXT("occlusionInterpolationTime"), 0.5));

    SaveAudioAsset(Atten, bSave);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("message"), TEXT("Occlusion configured"));
    AddAssetVerification(Result, Atten);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- audio.authoring.configure_reverb_send ----
REGISTER_RPC_HANDLER("audio.authoring.configure_reverb_send", "audio.authoring", "Configure reverb send settings",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the SoundAttenuation"),
        RPC_PARAM_DEF("enableReverbSend", "boolean", "Enable reverb send", "true"),
        RPC_PARAM_OPT("reverbWetLevelMin", "number", "Min wet level"),
        RPC_PARAM_OPT("reverbWetLevelMax", "number", "Max wet level"),
        RPC_PARAM_OPT("reverbDistanceMin", "number", "Min distance"),
        RPC_PARAM_OPT("reverbDistanceMax", "number", "Max distance"),
        RPC_PARAM_DEF("save", "boolean", "Save after modification", "true")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    USoundAttenuation* Atten = LoadSoundAttenuationFromPath(AssetPath);
    if (!Atten)
    {
        Ctx.SendError(ErrorCodes::ERR_ATTENUATION_NOT_FOUND, FString::Printf(TEXT("Could not load SoundAttenuation: %s"), *AssetPath));
        return true;
    }

    Atten->Attenuation.bEnableReverbSend = Ctx.GetBool(TEXT("enableReverbSend"), true);

    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();
    if (Payload->HasField(TEXT("reverbWetLevelMin")))
        Atten->Attenuation.ReverbWetLevelMin = static_cast<float>(Ctx.GetNumber(TEXT("reverbWetLevelMin"), 0.3));
    if (Payload->HasField(TEXT("reverbWetLevelMax")))
        Atten->Attenuation.ReverbWetLevelMax = static_cast<float>(Ctx.GetNumber(TEXT("reverbWetLevelMax"), 0.95));
    if (Payload->HasField(TEXT("reverbDistanceMin")))
        Atten->Attenuation.ReverbDistanceMin = static_cast<float>(Ctx.GetNumber(TEXT("reverbDistanceMin"), 0.0));
    if (Payload->HasField(TEXT("reverbDistanceMax")))
        Atten->Attenuation.ReverbDistanceMax = static_cast<float>(Ctx.GetNumber(TEXT("reverbDistanceMax"), 0.0));

    SaveAudioAsset(Atten, bSave);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("message"), TEXT("Reverb send configured"));
    AddAssetVerification(Result, Atten);
    Ctx.SendSuccess(Result);
    return true;
}

// =========================================================================
// 11.5 Dialogue System (3 actions)
// =========================================================================

// ---- audio.authoring.create_dialogue_voice ----
REGISTER_RPC_HANDLER("audio.authoring.create_dialogue_voice", "audio.authoring", "Create a new DialogueVoice asset",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name of the voice"),
        RPC_PARAM_OPT("path", "path", "Content path (default /Game/Audio/Dialogue)"),
        RPC_PARAM_DEF("gender", "string", "Gender: Masculine, Feminine, Neuter", "Masculine"),
        RPC_PARAM_DEF("plurality", "string", "Plurality: Singular, Plural", "Singular"),
        RPC_PARAM_DEF("save", "boolean", "Save after creation", "true")
    ))
{
#if MCP_HAS_DIALOGUE && MCP_HAS_DIALOGUE_FACTORY
    FString Name;
    if (!Ctx.RequireString(TEXT("name"), Name)) return true;

    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/Audio/Dialogue")));
    FString Gender = Ctx.GetString(TEXT("gender"), TEXT("Masculine"));
    FString Plurality = Ctx.GetString(TEXT("plurality"), TEXT("Singular"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    FString PackagePath;
    if (!PinWrightAudioPackagePath::ComposeAudioAssetPackagePathOrRefuse(Ctx, Path, Name, PackagePath))
    {
        return true;
    }

    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(ErrorCodes::ERR_PACKAGE_ERROR, TEXT("Failed to create package"));
        return true;
    }

    UDialogueVoiceFactory* Factory = NewObject<UDialogueVoiceFactory>();
    UDialogueVoice* NewVoice = Cast<UDialogueVoice>(
        Factory->FactoryCreateNew(UDialogueVoice::StaticClass(), Package,
                                  FName(*Name), RF_Public | RF_Standalone,
                                  nullptr, GWarn));
    if (!NewVoice)
    {
        Ctx.SendError(ErrorCodes::ERR_CREATE_FAILED, TEXT("Failed to create DialogueVoice"));
        return true;
    }

    if (Gender.ToLower() == TEXT("masculine")) NewVoice->Gender = EGrammaticalGender::Masculine;
    else if (Gender.ToLower() == TEXT("feminine")) NewVoice->Gender = EGrammaticalGender::Feminine;
    else if (Gender.ToLower() == TEXT("neuter")) NewVoice->Gender = EGrammaticalGender::Neuter;

    if (Plurality.ToLower() == TEXT("singular")) NewVoice->Plurality = EGrammaticalNumber::Singular;
    else if (Plurality.ToLower() == TEXT("plural")) NewVoice->Plurality = EGrammaticalNumber::Plural;

    SaveAudioAsset(NewVoice, bSave);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("assetPath"), NewVoice->GetPathName());
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("DialogueVoice '%s' created"), *Name));
    AddAssetVerification(Result, NewVoice);
    Ctx.SendSuccess(Result);
    return true;
#else
    Ctx.SendError(ErrorCodes::ERR_DIALOGUE_NOT_AVAILABLE, TEXT("Dialogue system not available"));
    return true;
#endif
}

// ---- audio.authoring.create_dialogue_wave ----
REGISTER_RPC_HANDLER("audio.authoring.create_dialogue_wave", "audio.authoring", "Create a new DialogueWave asset",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name of the dialogue wave"),
        RPC_PARAM_OPT("path", "path", "Content path (default /Game/Audio/Dialogue)"),
        RPC_PARAM_OPT("spokenText", "string", "The dialogue text"),
        RPC_PARAM_DEF("save", "boolean", "Save after creation", "true")
    ))
{
#if MCP_HAS_DIALOGUE && MCP_HAS_DIALOGUE_FACTORY
    FString Name;
    if (!Ctx.RequireString(TEXT("name"), Name)) return true;

    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/Audio/Dialogue")));
    FString SpokenText = Ctx.GetString(TEXT("spokenText"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    FString PackagePath;
    if (!PinWrightAudioPackagePath::ComposeAudioAssetPackagePathOrRefuse(Ctx, Path, Name, PackagePath))
    {
        return true;
    }

    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(ErrorCodes::ERR_PACKAGE_ERROR, TEXT("Failed to create package"));
        return true;
    }

    UDialogueWaveFactory* Factory = NewObject<UDialogueWaveFactory>();
    UDialogueWave* NewWave = Cast<UDialogueWave>(
        Factory->FactoryCreateNew(UDialogueWave::StaticClass(), Package,
                                  FName(*Name), RF_Public | RF_Standalone,
                                  nullptr, GWarn));
    if (!NewWave)
    {
        Ctx.SendError(ErrorCodes::ERR_CREATE_FAILED, TEXT("Failed to create DialogueWave"));
        return true;
    }

    NewWave->SpokenText = SpokenText;

    SaveAudioAsset(NewWave, bSave);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("assetPath"), NewWave->GetPathName());
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("DialogueWave '%s' created"), *Name));
    AddAssetVerification(Result, NewWave);
    Ctx.SendSuccess(Result);
    return true;
#else
    Ctx.SendError(ErrorCodes::ERR_DIALOGUE_NOT_AVAILABLE, TEXT("Dialogue system not available"));
    return true;
#endif
}

// ---- audio.authoring.set_dialogue_context ----
REGISTER_RPC_HANDLER("audio.authoring.set_dialogue_context", "audio.authoring", "Add or replace a dialogue context mapping",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the DialogueWave"),
        RPC_PARAM_OPT("speakerPath", "path", "Asset path of the speaker DialogueVoice"),
        RPC_PARAM_OPT("soundWavePath", "path", "Asset path of the SoundWave for this context"),
        RPC_PARAM_OPT("targetVoices", "array", "Array of target DialogueVoice paths"),
        RPC_PARAM_OPT("localizationKeyFormat", "string", "Localization key format"),
        RPC_PARAM_DEF("replace", "boolean", "Replace existing mapping with same speaker", "false"),
        RPC_PARAM_DEF("save", "boolean", "Save after modification", "true")
    ))
{
#if MCP_HAS_DIALOGUE
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString SpeakerPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("speakerPath")));
    FString SoundWavePath = NormalizeContentAssetPath(Ctx.GetString(TEXT("soundWavePath")));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    UDialogueWave* Wave = Cast<UDialogueWave>(StaticLoadObject(UDialogueWave::StaticClass(), nullptr, *AssetPath));
    if (!Wave)
    {
        Ctx.SendError(ErrorCodes::ERR_WAVE_NOT_FOUND, FString::Printf(TEXT("Could not load DialogueWave: %s"), *AssetPath));
        return true;
    }

    UDialogueVoice* SpeakerVoice = nullptr;
    if (!SpeakerPath.IsEmpty())
    {
        SpeakerVoice = Cast<UDialogueVoice>(StaticLoadObject(UDialogueVoice::StaticClass(), nullptr, *SpeakerPath));
        if (!SpeakerVoice)
        {
            Ctx.SendError(ErrorCodes::ERR_SPEAKER_NOT_FOUND, FString::Printf(TEXT("Could not load speaker DialogueVoice: %s"), *SpeakerPath));
            return true;
        }
    }

    USoundWave* ContextSoundWave = nullptr;
    if (!SoundWavePath.IsEmpty())
    {
        ContextSoundWave = LoadSoundWaveFromPath(SoundWavePath);
        if (!ContextSoundWave)
        {
            Ctx.SendError(ErrorCodes::ERR_SOUNDWAVE_NOT_FOUND, FString::Printf(TEXT("Could not load SoundWave: %s"), *SoundWavePath));
            return true;
        }
    }

    TArray<UDialogueVoice*> TargetVoices;
    const TArray<TSharedPtr<FJsonValue>>* TargetArray = Ctx.GetArray(TEXT("targetVoices"));
    if (TargetArray)
    {
        for (const TSharedPtr<FJsonValue>& TargetVal : *TargetArray)
        {
            FString TargetPath = NormalizeContentAssetPath(TargetVal->AsString());
            if (!TargetPath.IsEmpty())
            {
                UDialogueVoice* TargetVoice = Cast<UDialogueVoice>(
                    StaticLoadObject(UDialogueVoice::StaticClass(), nullptr, *TargetPath));
                if (TargetVoice) TargetVoices.Add(TargetVoice);
            }
        }
    }

    FDialogueContextMapping NewMapping;
    NewMapping.Context.Speaker = SpeakerVoice;
    for (UDialogueVoice* TargetVoice : TargetVoices)
    {
        NewMapping.Context.Targets.Add(TargetVoice);
    }
    NewMapping.SoundWave = ContextSoundWave;
    NewMapping.LocalizationKeyFormat = Ctx.GetString(TEXT("localizationKeyFormat"), TEXT("{ContextHash}"));

    // Reclaim the pristine engine-seeded ContextMappings[0] on the first authored
    // set instead of appending past it, so contextCount equals the contexts the
    // caller actually authored instead of being permanently one higher (mirrors
    // the struct-seed reclaim in BlueprintTypeDefinitionHandler::AddStructField).
    // The UDialogueWave ctor seeds one default FDialogueContextMapping
    // (DialogueWave.cpp) that create_dialogue_wave leaves untouched; its
    // FDialogueContext ctor pre-fills Targets with a lone zeroed element
    // (DialogueTypes.cpp Targets.AddZeroed()), so the pristine target list is
    // [null], not empty — accept empty-or-lone-null. Once a real context is
    // authored the slot is no longer pristine and later sets append as before.
    auto AddOrReclaimSeed = [](UDialogueWave* W, const FDialogueContextMapping& Mapping)
    {
        if (W->ContextMappings.Num() == 1)
        {
            const FDialogueContextMapping& Seed = W->ContextMappings[0];
            const bool bTargetsPristine =
                Seed.Context.Targets.Num() == 0 ||
                (Seed.Context.Targets.Num() == 1 && Seed.Context.Targets[0] == nullptr);
            if (Seed.Context.Speaker == nullptr &&
                Seed.SoundWave == nullptr &&
                bTargetsPristine &&
                Seed.LocalizationKeyFormat == TEXT("{ContextHash}"))
            {
                W->ContextMappings[0] = Mapping;
                return;
            }
        }
        W->ContextMappings.Add(Mapping);
    };

    bool bReplaceExisting = Ctx.GetBool(TEXT("replace"), false);
    if (bReplaceExisting)
    {
        bool bFound = false;
        for (FDialogueContextMapping& Mapping : Wave->ContextMappings)
        {
            if (Mapping.Context.Speaker == SpeakerVoice)
            {
                Mapping = NewMapping;
                bFound = true;
                break;
            }
        }
        if (!bFound) AddOrReclaimSeed(Wave, NewMapping);
    }
    else
    {
        AddOrReclaimSeed(Wave, NewMapping);
    }

    SaveAudioAsset(Wave, bSave);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetNumberField(TEXT("contextCount"), Wave->ContextMappings.Num());
    Result->SetStringField(TEXT("message"), TEXT("Dialogue context mapping added"));
    AddAssetVerification(Result, Wave);
    Ctx.SendSuccess(Result);
    return true;
#else
    Ctx.SendError(ErrorCodes::ERR_DIALOGUE_NOT_AVAILABLE, TEXT("Dialogue system not available"));
    return true;
#endif
}

// =========================================================================
// 11.6 Effects (4 actions)
// =========================================================================

// ---- audio.authoring.create_reverb_effect ----
REGISTER_RPC_HANDLER("audio.authoring.create_reverb_effect", "audio.authoring", "Create a new ReverbEffect asset",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name of the reverb effect"),
        RPC_PARAM_OPT("path", "path", "Content path (default /Game/Audio/Effects)"),
        RPC_PARAM_OPT("density", "number", "Reverb density"),
        RPC_PARAM_OPT("diffusion", "number", "Reverb diffusion"),
        RPC_PARAM_OPT("gain", "number", "Reverb gain"),
        RPC_PARAM_OPT("gainHF", "number", "HF gain"),
        RPC_PARAM_OPT("decayTime", "number", "Decay time"),
        RPC_PARAM_OPT("decayHFRatio", "number", "HF decay ratio"),
        RPC_PARAM_DEF("save", "boolean", "Save after creation", "true")
    ))
{
#if MCP_HAS_REVERB_EFFECT
    FString Name;
    if (!Ctx.RequireString(TEXT("name"), Name)) return true;

    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/Audio/Effects")));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    FString PackagePath;
    if (!PinWrightAudioPackagePath::ComposeAudioAssetPackagePathOrRefuse(Ctx, Path, Name, PackagePath))
    {
        return true;
    }

    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(ErrorCodes::ERR_PACKAGE_ERROR, TEXT("Failed to create package"));
        return true;
    }

    UReverbEffect* NewEffect = NewObject<UReverbEffect>(Package, FName(*Name), RF_Public | RF_Standalone);
    if (!NewEffect)
    {
        Ctx.SendError(ErrorCodes::ERR_CREATE_FAILED, TEXT("Failed to create ReverbEffect"));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();
    if (Payload->HasField(TEXT("density")))
        NewEffect->Density = static_cast<float>(Ctx.GetNumber(TEXT("density"), 1.0));
    if (Payload->HasField(TEXT("diffusion")))
        NewEffect->Diffusion = static_cast<float>(Ctx.GetNumber(TEXT("diffusion"), 1.0));
    if (Payload->HasField(TEXT("gain")))
        NewEffect->Gain = static_cast<float>(Ctx.GetNumber(TEXT("gain"), 0.32));
    if (Payload->HasField(TEXT("gainHF")))
        NewEffect->GainHF = static_cast<float>(Ctx.GetNumber(TEXT("gainHF"), 0.89));
    if (Payload->HasField(TEXT("decayTime")))
        NewEffect->DecayTime = static_cast<float>(Ctx.GetNumber(TEXT("decayTime"), 1.49));
    if (Payload->HasField(TEXT("decayHFRatio")))
        NewEffect->DecayHFRatio = static_cast<float>(Ctx.GetNumber(TEXT("decayHFRatio"), 0.83));

    const bool bSavedToDisk = SaveAudioAsset(NewEffect, bSave);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("assetPath"), NewEffect->GetPathName());
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("ReverbEffect '%s' created"), *Name));
    AddAssetSaveReport(Result, bSave, bSavedToDisk);
    AddAssetVerification(Result, NewEffect);
    Ctx.SendSuccess(Result);
    return true;
#else
    Ctx.SendError(ErrorCodes::ERR_REVERB_NOT_AVAILABLE, TEXT("Reverb effect not available"));
    return true;
#endif
}

// ---- audio.authoring.create_source_effect_chain ----
REGISTER_RPC_HANDLER("audio.authoring.create_source_effect_chain", "audio.authoring", "Create a source effect preset chain",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name of the effect chain"),
        RPC_PARAM_OPT("path", "path", "Content path (default /Game/Audio/Effects)"),
        RPC_PARAM_DEF("save", "boolean", "Save after creation", "true")
    ))
{
#if MCP_HAS_SOURCE_EFFECT
    FString Name;
    if (!Ctx.RequireString(TEXT("name"), Name)) return true;

    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/Audio/Effects")));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    FString PackagePath;
    if (!PinWrightAudioPackagePath::ComposeAudioAssetPackagePathOrRefuse(Ctx, Path, Name, PackagePath))
    {
        return true;
    }

    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(ErrorCodes::ERR_PACKAGE_ERROR, TEXT("Failed to create package"));
        return true;
    }

    USoundEffectSourcePresetChain* NewChain = NewObject<USoundEffectSourcePresetChain>(
        Package, FName(*Name), RF_Public | RF_Standalone);
    if (!NewChain)
    {
        Ctx.SendError(ErrorCodes::ERR_CREATE_FAILED, TEXT("Failed to create source effect chain"));
        return true;
    }

    McpSafeAssetSave(NewChain);
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    const bool bSavedToDisk = SaveAudioAsset(NewChain, bSave, &SaveState);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("assetPath"), NewChain->GetPathName());
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Source effect chain '%s' created"), *Name));
    AddAssetSaveReport(Result, bSave, bSavedToDisk, SaveState);
    AddAssetVerification(Result, NewChain);
    if (bSave && !bSavedToDisk)
    {
        Ctx.SendError(ErrorCodes::ERR_SAVE_FAILED,
            FString::Printf(TEXT("Source effect chain was created in memory, but its save was not durable (saveState=%s)."),
                AssetSaveStateToWire(SaveState)), Result);
        return true;
    }
    Ctx.SendSuccess(Result);
    return true;
#else
    FString Name = Ctx.GetString(TEXT("name"));
    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/Audio/Effects")));

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("assetPath"), Path / Name);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Source effect chain '%s' - AudioMixer module not available"), *Name));
    Result->SetStringField(TEXT("note"), TEXT("Enable AudioMixer plugin for full source effect chain support"));
    Ctx.SendSuccess(Result);
    return true;
#endif
}

// ---- audio.authoring.add_source_effect ----
REGISTER_RPC_HANDLER("audio.authoring.add_source_effect", "audio.authoring", "Add a source effect to an effect chain",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the effect chain"),
        RPC_PARAM_OPT("effectPresetPath", "path", "Asset path of the effect preset"),
        RPC_PARAM_OPT("effectType", "string", "Type of effect (informational)"),
        RPC_PARAM_DEF("bypass", "boolean", "Bypass this effect", "false"),
        RPC_PARAM_DEF("save", "boolean", "Save after modification", "true")
    ))
{
#if MCP_HAS_SOURCE_EFFECT
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));
    FString EffectPresetPath = Ctx.GetString(TEXT("effectPresetPath"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_MISSING_PATH, TEXT("Asset path is required"));
        return true;
    }

    USoundEffectSourcePresetChain* Chain = Cast<USoundEffectSourcePresetChain>(
        StaticLoadObject(USoundEffectSourcePresetChain::StaticClass(), nullptr, *AssetPath));
    if (!Chain)
    {
        Ctx.SendError(ErrorCodes::ERR_CHAIN_NOT_FOUND, FString::Printf(TEXT("Could not load source effect chain: %s"), *AssetPath));
        return true;
    }

    USoundEffectSourcePreset* EffectPreset = nullptr;
    if (!EffectPresetPath.IsEmpty())
    {
        EffectPreset = Cast<USoundEffectSourcePreset>(
            StaticLoadObject(USoundEffectSourcePreset::StaticClass(), nullptr, *NormalizeContentAssetPath(EffectPresetPath)));
    }

    if (EffectPreset)
    {
        FSourceEffectChainEntry NewEntry;
        NewEntry.Preset = EffectPreset;
        NewEntry.bBypass = Ctx.GetBool(TEXT("bypass"), false);
        Chain->Chain.Add(NewEntry);

        McpSafeAssetSave(Chain);
        EAssetSaveState SaveState = EAssetSaveState::NotRequested;
        const bool bSavedToDisk = SaveAudioAsset(Chain, bSave, &SaveState);

        TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
        Result->SetNumberField(TEXT("effectCount"), Chain->Chain.Num());
        Result->SetStringField(TEXT("message"), TEXT("Source effect added to chain"));
        AddAssetSaveReport(Result, bSave, bSavedToDisk, SaveState);
        AddAssetVerification(Result, Chain);
        if (bSave && !bSavedToDisk)
        {
            Ctx.SendError(ErrorCodes::ERR_SAVE_FAILED,
                FString::Printf(TEXT("Source effect was added in memory, but the chain save was not durable (saveState=%s)."),
                    AssetSaveStateToWire(SaveState)), Result);
            return true;
        }
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_PRESET_NOT_FOUND, TEXT("Effect preset path required or preset not found"));
    }

    return true;
#else
    FString EffectType = Ctx.GetString(TEXT("effectType"));
    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Source effect '%s' noted"), *EffectType));
    Result->SetStringField(TEXT("note"), TEXT("AudioMixer module not available - enable AudioMixer plugin for full support"));
    Ctx.SendSuccess(Result);
    return true;
#endif
}

// ---- audio.authoring.create_source_effect_preset ----
#if MCP_HAS_SOURCE_EFFECT
// Resolve a concrete source-effect preset UClass from a user-supplied identifier, by reflection.
// USoundEffectSourcePreset is abstract + MinimalAPI and its concrete subclasses
// (SourceEffectFilterPreset / EQ / BitCrusher / ...) live in the optional Synthesis plugin, so
// they can't be referenced across modules via StaticClass()/NewObject<T> — per the plugin's
// non-*_API-UCLASS rule, resolve them by object path instead. Accepts a full "/Script/Module.Class"
// path, an on-disk Blueprint preset path (either "/Game/.../BP_C" or the plain "/Game/.../BP" asset
// path), a bare class name (SourceEffectFilterPreset), or a short effect name (Filter / EQ /
// BitCrusher). Returns nullptr if nothing resolves.
static UClass* ResolveSourceEffectPresetClass(const FString& InEffectClass)
{
    const FString Trimmed = InEffectClass.TrimStartAndEnd();
    if (Trimmed.IsEmpty())
    {
        return nullptr;
    }

    if (Trimmed.Contains(TEXT("/")) || Trimmed.Contains(TEXT(".")))
    {
        // Explicit object path — delegate to the shared resolver: it FindObjects a native /Script
        // class already in memory, then handles on-disk Blueprint preset classes uniformly, whether
        // the caller passes the generated-class /Game/.../BP_C path or the plain /Game/.../BP asset
        // path (it appends _C / falls back to the UBlueprint's GeneratedClass).
        return ResolveUClass(Trimmed);
    }

    // Bare identifier: the concrete presets are all native Synthesis classes already registered at
    // module load, so FindObject resolves them without a (warning-logging) disk-load attempt. This
    // handler wants a concrete USoundEffectSourcePreset, so try the "...Preset"-suffixed spellings
    // FIRST — that way an input naming the runtime effect ("SourceEffectFilter") still lands on its
    // preset ("SourceEffectFilterPreset") instead of resolving the same-named non-preset runtime
    // class and then being rejected by the IsChildOf validation below. "SourceEffectFilterPreset",
    // "FilterPreset", and "Filter" all map to the same class.
    const FString Candidates[] = {
        FString::Printf(TEXT("/Script/Synthesis.SourceEffect%sPreset"), *Trimmed),
        FString::Printf(TEXT("/Script/Synthesis.%sPreset"), *Trimmed),
        FString::Printf(TEXT("/Script/Synthesis.SourceEffect%s"), *Trimmed),
        FString::Printf(TEXT("/Script/Synthesis.%s"), *Trimmed)
    };
    for (const FString& Candidate : Candidates)
    {
        if (UClass* Cls = FindObject<UClass>(nullptr, *Candidate))
        {
            return Cls;
        }
    }
    return nullptr;
}
#endif // MCP_HAS_SOURCE_EFFECT

// The missing third verb of the source-effect authoring surface: create_source_effect_chain makes
// the (empty) USoundEffectSourcePresetChain container and add_source_effect appends entries, but
// each entry needs a pre-existing USoundEffectSourcePreset — which nothing could create via RPC
// (F-source-effect-preset-authoring), so add_source_effect always dead-ended at [PRESET_NOT_FOUND].
// This creates that preset asset (a concrete Synthesis SourceEffect*Preset, resolved by reflection)
// so the whole chain is authorable inside the RPC surface: create_source_effect_chain ->
// create_source_effect_preset (xN) -> add_source_effect. Mirrors create_source_effect_chain /
// create_sound_submix (the analogous F-audio-submix-asset-authoring fix).
REGISTER_RPC_HANDLER("audio.authoring.create_source_effect_preset", "audio.authoring", "Create a USoundEffectSourcePreset asset (e.g. Filter/EQ/BitCrusher) so add_source_effect has a preset to reference. effectClass takes a short name (Filter), a class name (SourceEffectFilterPreset), or a /Script/Module.Class path.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name of the source effect preset asset"),
        RPC_PARAM_REQ("effectClass", "classref", "Source-effect preset class: short name (Filter/EQ/BitCrusher/Chorus/Phaser/...), class name (SourceEffectFilterPreset), or /Script/Module.Class path"),
        RPC_PARAM_OPT("path", "path", "Content path (default /Game/Audio/Effects)"),
        RPC_PARAM_DEF("save", "boolean", "Save after creation", "true")
    ))
{
#if MCP_HAS_SOURCE_EFFECT
    FString Name;
    if (!Ctx.RequireString(TEXT("name"), Name)) return true;

    FString EffectClass;
    if (!Ctx.RequireString(TEXT("effectClass"), EffectClass)) return true;

    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/Audio/Effects")));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    UClass* PresetClass = ResolveSourceEffectPresetClass(EffectClass);
    if (!PresetClass)
    {
        Ctx.SendError(ErrorCodes::ERR_EFFECT_CLASS_NOT_FOUND,
            FString::Printf(TEXT("Could not resolve source effect preset class '%s'. Pass a Synthesis effect short name (Filter/EQ/BitCrusher/...), a class name (SourceEffectFilterPreset), or a /Script/Module.Class path; the Synthesis plugin must be enabled."), *EffectClass));
        return true;
    }
    if (!PresetClass->IsChildOf(USoundEffectSourcePreset::StaticClass()) ||
        PresetClass->HasAnyClassFlags(CLASS_Abstract))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_EFFECT_CLASS,
            FString::Printf(TEXT("Class '%s' is not a concrete USoundEffectSourcePreset subclass"), *PresetClass->GetPathName()));
        return true;
    }

    FString PackagePath;
    if (!PinWrightAudioPackagePath::ComposeAudioAssetPackagePathOrRefuse(Ctx, Path, Name, PackagePath))
    {
        return true;
    }

    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(ErrorCodes::ERR_PACKAGE_ERROR, TEXT("Failed to create package"));
        return true;
    }

    USoundEffectSourcePreset* NewPreset = NewObject<USoundEffectSourcePreset>(
        Package, PresetClass, FName(*Name), RF_Public | RF_Standalone);
    if (!NewPreset)
    {
        Ctx.SendError(ErrorCodes::ERR_CREATE_FAILED, TEXT("Failed to create source effect preset"));
        return true;
    }

    const bool bSavedToDisk = SaveAudioAsset(NewPreset, bSave);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("assetPath"), NewPreset->GetPathName());
    Result->SetStringField(TEXT("assetClass"), PresetClass->GetName());
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Source effect preset '%s' (%s) created"), *Name, *PresetClass->GetName()));
    AddAssetSaveReport(Result, bSave, bSavedToDisk);
    AddAssetVerification(Result, NewPreset);
    Ctx.SendSuccess(Result);
    return true;
#else
    Ctx.SendError(ErrorCodes::ERR_SOURCE_EFFECT_NOT_AVAILABLE, TEXT("Source effect support not available - enable the AudioMixer/Synthesis plugins"));
    return true;
#endif
}

// ---- audio.authoring.create_sound_submix ----
// Canonical name for creating a USoundSubmix asset (mirrors create_sound_class / create_sound_mix).
// Optionally wires ParentSubmix at creation time so a bus tree can be built in a single call chain.
REGISTER_RPC_HANDLER("audio.authoring.create_sound_submix", "audio.authoring", "Create a USoundSubmix asset (a bus in the submix routing graph). Optionally wires ParentSubmix at creation time so a master→child bus tree can be built in one pass.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name of the SoundSubmix"),
        RPC_PARAM_OPT("path", "path", "Content path (default /Game/Audio/Submixes)"),
        RPC_PARAM_OPT("parentSubmix", "path", "Asset path of a USoundSubmix to set as ParentSubmix (empty = no parent)"),
        RPC_PARAM_DEF("save", "boolean", "Save after creation", "true")
    ))
{
#if MCP_HAS_SUBMIX
    FString Name;
    if (!Ctx.RequireString(TEXT("name"), Name)) return true;

    FString Path = NormalizeContentAssetPath(Ctx.GetString(TEXT("path"), TEXT("/Game/Audio/Submixes")));
    FString ParentPath = Ctx.GetString(TEXT("parentSubmix"));
    bool bSave = Ctx.GetBool(TEXT("save"), true);

    USoundSubmix* NewSubmix = CreateSoundSubmixAsset(Name, Path, ParentPath, bSave, Ctx);
    if (!NewSubmix) return true; // helper already SendError'd

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("assetPath"), NewSubmix->GetPathName());
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("SoundSubmix '%s' created"), *Name));
    AddAssetVerification(Result, NewSubmix);
    Ctx.SendSuccess(Result);
    return true;
#else
    Ctx.SendError(ErrorCodes::ERR_SUBMIX_NOT_AVAILABLE, TEXT("USoundSubmix support not compiled into this build"));
    return true;
#endif
}

// =========================================================================
// Utility (3 actions)
// =========================================================================

// ---- audio.authoring.describe_metasound ----
REGISTER_RPC_HANDLER("audio.authoring.describe_metasound", "audio.authoring", "Describe a MetaSound Source or Patch graph as structured JSON",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the MetaSound asset"),
        RPC_PARAM_OPT("compact", "boolean",
            "Drop each node's per-vertex inputs/outputs (the GUID + literal bulk that overflows the "
            "inline-readback limit even for tiny graphs); keep node id/classID/name + edges + interface "
            "input defaults (default false). Adds a compact/nodeCount/returnedNodeCount summary header."),
        RPC_PARAM_OPT("nodeIds", "array",
            "Return only the nodes whose id is in this set (at full vertex detail), e.g. to read just the "
            "pin names of a node you just added without fetching the whole graph. Node ids are the `id` "
            "field of a prior describe. Adds the same summary header.")
    ))
{
#if MCP_HAS_METASOUND_DOCUMENT_INTERFACE
    FString AssetPathRaw;
    if (!Ctx.RequireString(TEXT("assetPath"), AssetPathRaw)) return true;

    FString AssetPath = NormalizeContentAssetPath(AssetPathRaw);
    UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
    if (!Asset)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, FString::Printf(TEXT("Could not load asset: %s"), *AssetPath));
        return true;
    }

    IMetaSoundDocumentInterface* DocInterface = Cast<IMetaSoundDocumentInterface>(Asset);
    if (!DocInterface)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_TYPE, FString::Printf(TEXT("Asset is not a MetaSound document: %s"), *AssetPath));
        return true;
    }

    // Opt-in size knobs (default behavior is the full snapshot, parity with asset.dump). compact
    // strips the per-vertex bulk; nodeIds narrows nodes[] to a named subset for pin discovery.
    MetaSoundDumpBuilder::FMetaSoundDumpOptions DumpOptions;
    DumpOptions.bCompact = Ctx.GetBool(TEXT("compact"));
    DumpOptions.NodeIdFilter = Ctx.GetStringSet(TEXT("nodeIds"));

    TSharedPtr<FJsonObject> Result = MetaSoundDumpBuilder::BuildMetaSoundJson(Asset, DumpOptions);
    if (!Result.IsValid())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_TYPE, FString::Printf(TEXT("Asset is not a supported MetaSound document: %s"), *AssetPath));
        return true;
    }

    Ctx.SendSuccess(Result);
    return true;
#else
    Ctx.SendError(ErrorCodes::ERR_METASOUND_NOT_AVAILABLE, TEXT("MetaSound document support not available"));
    return true;
#endif
}

// ---- audio.authoring.describe_sound_cue ----
REGISTER_RPC_HANDLER("audio.authoring.describe_sound_cue", "audio.authoring", "Describe a SoundCue graph and cue-level settings (concurrency/attenuation/soundClass/volume/pitch) as structured JSON",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the SoundCue asset")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireString(TEXT("assetPath"), AssetPath)) return true;

    USoundCue* Cue = LoadSoundCueFromPath(AssetPath);
    if (!Cue)
    {
        Ctx.SendError(ErrorCodes::ERR_CUE_NOT_FOUND, FString::Printf(TEXT("Could not load SoundCue: %s"), *AssetPath));
        return true;
    }

    Ctx.SendSuccess(SoundCueDumpBuilder::BuildSoundCueJson(Cue));
    return true;
}

// ---- audio.authoring.describe_sound_wave ----
REGISTER_RPC_HANDLER("audio.authoring.describe_sound_wave", "audio.authoring",
    "Describe a SoundWave as structured JSON: duration, channels, sampleRate, looping, soundGroup, volume, pitch - same shape as sound_wave.json asset dumps.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the SoundWave asset")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireString(TEXT("assetPath"), AssetPath)) return true;

    USoundWave* Wave = LoadSoundWaveFromPath(AssetPath);
    if (!Wave)
    {
        Ctx.SendError(ErrorCodes::ERR_WAVE_NOT_FOUND,
            FString::Printf(TEXT("Could not load SoundWave: %s"), *AssetPath));
        return true;
    }

    Ctx.SendSuccess(SoundWaveDumpBuilder::BuildSoundWaveJson(Wave));
    return true;
}

// ---- audio.authoring.describe_attenuation ----
// Full live readback for SoundAttenuation: the four configure_* verbs
// (configure_distance_attenuation / configure_spatialization / configure_occlusion /
// configure_reverb_send) write ~12 FSoundAttenuationSettings fields, but get_audio_info
// echoes only falloffDistance + spatialize. This is the inspect-after-mutate read that
// confirms a configured attenuation profile without falling back to asset.dump's on-disk
// properties.json — the SoundAttenuation member of the SoundCue/MetaSound/SoundWave
// describe family. Hand-rolls exactly the fields the configure_* verbs set so the JSON
// keys match the configure params (innerRadius lives in AttenuationShapeExtents.X, not a
// RadiusMin field).
REGISTER_RPC_HANDLER("audio.authoring.describe_attenuation", "audio.authoring",
    "Describe a SoundAttenuation asset as structured JSON: distance attenuation, spatialization, occlusion, and reverb-send fields the configure_* verbs write - the full readback get_audio_info omits.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the SoundAttenuation asset")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireString(TEXT("assetPath"), AssetPath)) return true;

    USoundAttenuation* Atten = LoadSoundAttenuationFromPath(AssetPath);
    if (!Atten)
    {
        Ctx.SendError(ErrorCodes::ERR_ATTENUATION_NOT_FOUND,
            FString::Printf(TEXT("Could not load SoundAttenuation: %s"), *AssetPath));
        return true;
    }

    const FSoundAttenuationSettings& S = Atten->Attenuation;

    // Inverse of the token->enum parse ladder in configure_distance_attenuation (search this
    // file for that handler) — that ladder is the source of truth for the accepted token
    // strings. Keep these cases in lock-step with it: a new EAttenuationDistanceModel value or a
    // renamed/aliased token must be edited in BOTH places or the configure/describe round-trip
    // diverges (configure would accept a token describe never emits, or vice-versa).
    auto DistanceAlgorithmToken = [](EAttenuationDistanceModel Model) -> FString
    {
        switch (Model)
        {
        case EAttenuationDistanceModel::Linear:      return TEXT("linear");
        case EAttenuationDistanceModel::Logarithmic: return TEXT("logarithmic");
        case EAttenuationDistanceModel::Inverse:     return TEXT("inverse");
        case EAttenuationDistanceModel::NaturalSound:return TEXT("naturalsound");
        default:                                     return TEXT("custom");
        }
    };

    // Inverse of the token->enum parse ladder in configure_spatialization (source of truth for
    // the accepted token strings). Keep in lock-step with it for the same round-trip reason as
    // DistanceAlgorithmToken above. (configure also accepts "binaural" as an alias for "hrtf";
    // describe emits the canonical "hrtf".)
    auto SpatializationAlgorithmToken = [](ESoundSpatializationAlgorithm Algorithm) -> FString
    {
        switch (Algorithm)
        {
        case ESoundSpatializationAlgorithm::SPATIALIZATION_Default: return TEXT("panner");
        case ESoundSpatializationAlgorithm::SPATIALIZATION_HRTF:    return TEXT("hrtf");
        default:                                                    return TEXT("unknown");
        }
    };

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("type"), TEXT("SoundAttenuation"));

    // Distance attenuation (configure_distance_attenuation).
    Result->SetStringField(TEXT("distanceAlgorithm"), DistanceAlgorithmToken(S.DistanceAlgorithm));
    Result->SetNumberField(TEXT("falloffDistance"), S.FalloffDistance);
    Result->SetNumberField(TEXT("innerRadius"), S.AttenuationShapeExtents.X);

    // Spatialization (configure_spatialization).
    Result->SetBoolField(TEXT("spatialize"), S.bSpatialize);
    Result->SetStringField(TEXT("spatializationAlgorithm"), SpatializationAlgorithmToken(S.SpatializationAlgorithm));

    // Occlusion (configure_occlusion).
    Result->SetBoolField(TEXT("enableOcclusion"), S.bEnableOcclusion);
    Result->SetNumberField(TEXT("occlusionLowPassFilterFrequency"), S.OcclusionLowPassFilterFrequency);
    Result->SetNumberField(TEXT("occlusionVolumeAttenuation"), S.OcclusionVolumeAttenuation);
    Result->SetNumberField(TEXT("occlusionInterpolationTime"), S.OcclusionInterpolationTime);

    // Reverb send (configure_reverb_send).
    Result->SetBoolField(TEXT("enableReverbSend"), S.bEnableReverbSend);
    Result->SetNumberField(TEXT("reverbWetLevelMin"), S.ReverbWetLevelMin);
    Result->SetNumberField(TEXT("reverbWetLevelMax"), S.ReverbWetLevelMax);
    Result->SetNumberField(TEXT("reverbDistanceMin"), S.ReverbDistanceMin);
    Result->SetNumberField(TEXT("reverbDistanceMax"), S.ReverbDistanceMax);

    AddAssetVerification(Result, Atten);
    Result->SetStringField(TEXT("message"), TEXT("Attenuation settings retrieved"));
    Ctx.SendSuccess(Result);
    return true;
}

// ---- audio.authoring.describe_sound_class ----
// Full live readback for SoundClass: set_class_properties writes the whole
// FSoundClassProperties leaf surface (volume / pitch / lowPassFilterFrequency /
// lfeBleed / voiceCenterChannelVolume) plus the submix route, and
// set_class_parent / create_sound_class maintain BOTH sides of the hierarchy
// (child ParentClass + parent ChildClasses), but get_audio_info echoes only
// volume / pitch / parentClass / outputSubmix — never voiceCenterChannelVolume,
// lowPassFilterFrequency, lfeBleed, nor the parent-side childClasses list. This
// is the inspect-after-mutate read that confirms a configured SoundClass without
// falling back to asset.dump's properties.json — the SoundClass member of the
// describe_* family (describe_sound_cue / describe_sound_wave / describe_metasound
// / describe_attenuation). Keys mirror the set_class_properties params.
REGISTER_RPC_HANDLER("audio.authoring.describe_sound_class", "audio.authoring",
    "Describe a SoundClass as structured JSON: volume, pitch, lowPassFilterFrequency, lfeBleed, voiceCenterChannelVolume, parentClass, outputSubmix, and the childClasses array - the full readback get_audio_info omits.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the SoundClass asset")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireString(TEXT("assetPath"), AssetPath)) return true;

    USoundClass* SoundClass = LoadSoundClassFromPath(AssetPath);
    if (!SoundClass)
    {
        Ctx.SendError(ErrorCodes::ERR_CLASS_NOT_FOUND,
            FString::Printf(TEXT("Could not load SoundClass: %s"), *AssetPath));
        return true;
    }

    const FSoundClassProperties& P = SoundClass->Properties;

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("type"), TEXT("SoundClass"));

    // Leaf properties (set_class_properties). voiceCenterChannelVolume /
    // lowPassFilterFrequency / lfeBleed are exactly the fields get_audio_info omits.
    Result->SetNumberField(TEXT("volume"), P.Volume);
    Result->SetNumberField(TEXT("pitch"), P.Pitch);
    Result->SetNumberField(TEXT("lowPassFilterFrequency"), P.LowPassFilterFrequency);
    Result->SetNumberField(TEXT("lfeBleed"), P.LFEBleed);
    Result->SetNumberField(TEXT("voiceCenterChannelVolume"), P.VoiceCenterChannelVolume);

    // Hierarchy (set_class_parent / create_sound_class). Both sides: the child's
    // ParentClass and the parent-side ChildClasses list get_audio_info never emits.
    if (SoundClass->ParentClass)
        Result->SetStringField(TEXT("parentClass"), SoundClass->ParentClass->GetPathName());

    TArray<TSharedPtr<FJsonValue>> ChildClasses;
    for (USoundClass* Child : SoundClass->ChildClasses)
    {
        if (Child)
            ChildClasses.Add(MakeShareable(new FJsonValueString(Child->GetPathName())));
    }
    Result->SetArrayField(TEXT("childClasses"), ChildClasses);

#if MCP_HAS_SUBMIX
    if (P.DefaultSubmix)
        Result->SetStringField(TEXT("outputSubmix"), P.DefaultSubmix->GetPathName());
#endif

    AddAssetVerification(Result, SoundClass);
    Result->SetStringField(TEXT("message"), TEXT("Sound class described"));
    Ctx.SendSuccess(Result);
    return true;
}

// ---- audio.authoring.describe_sound_mix ----
// Full live readback for SoundMix: add_mix_modifier and create_sound_mix's
// classAdjusters write per-class FSoundClassAdjuster entries (soundClass /
// volumeAdjuster / pitchAdjuster / applyToChildren) into Mix->SoundClassEffects,
// and configure_mix_eq writes the mix-level applyEQ / eqPriority plus the full
// 4-band FEQSettings ladder (frequencyCenter0..3 / gain0..3 / bandwidth0..3), but
// get_audio_info echoes ONLY modifierCount (the entry count) — never the
// per-adjuster values nor any EQ field. This is the inspect-after-mutate read that
// confirms a configured mix's adjuster ladder + EQ without falling back to
// property.get on the SoundClassEffects array (or asset.dump's properties.json).
// The eqSettings object is built by the SAME BuildMixEqJson helper configure_mix_eq
// echoes in its own success response, so the write-side echo and this readback can
// never drift. No fade keys are emitted: FSoundClassAdjuster has no per-adjuster
// fade member, and no authoring verb writes the mix-level Mix->FadeInTime /
// FadeOutTime, so a fade readback would always report the asset default.
REGISTER_RPC_HANDLER("audio.authoring.describe_sound_mix", "audio.authoring",
    "Describe a SoundMix as structured JSON: modifierCount plus an adjusters[] array of per-class soundClass/volumeAdjuster/pitchAdjuster/applyToChildren values, and the mix-level applyEQ/eqPriority plus an eqSettings object carrying the full 4-band frequencyCenter0..3/gain0..3/bandwidth0..3 ladder - the per-adjuster + EQ surface get_audio_info omits.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the SoundMix asset")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireString(TEXT("assetPath"), AssetPath)) return true;

    USoundMix* Mix = LoadSoundMixFromPath(AssetPath);
    if (!Mix)
    {
        Ctx.SendError(ErrorCodes::ERR_MIX_NOT_FOUND,
            FString::Printf(TEXT("Could not load SoundMix: %s"), *AssetPath));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("type"), TEXT("SoundMix"));
    Result->SetNumberField(TEXT("modifierCount"), Mix->SoundClassEffects.Num());

    // Per-class adjusters (add_mix_modifier / create_sound_mix classAdjusters).
    TArray<TSharedPtr<FJsonValue>> Adjusters;
    for (const FSoundClassAdjuster& Adj : Mix->SoundClassEffects)
    {
        TSharedPtr<FJsonObject> AdjObj = MakeShareable(new FJsonObject());
        if (Adj.SoundClassObject)
            AdjObj->SetStringField(TEXT("soundClass"), Adj.SoundClassObject->GetPathName());
        AdjObj->SetNumberField(TEXT("volumeAdjuster"), Adj.VolumeAdjuster);
        AdjObj->SetNumberField(TEXT("pitchAdjuster"), Adj.PitchAdjuster);
        AdjObj->SetBoolField(TEXT("applyToChildren"), Adj.bApplyToChildren != 0);
        Adjusters.Add(MakeShareable(new FJsonValueObject(AdjObj)));
    }
    Result->SetArrayField(TEXT("adjusters"), Adjusters);

    // Mix-level EQ (configure_mix_eq): applyEQ + eqPriority at top level, plus the
    // full 4-band ladder under eqSettings via the same builder configure_mix_eq's
    // success echo uses, so the write-side echo and this readback never drift.
    Result->SetBoolField(TEXT("applyEQ"), Mix->bApplyEQ != 0);
    Result->SetNumberField(TEXT("eqPriority"), Mix->EQPriority);
    Result->SetObjectField(TEXT("eqSettings"), BuildMixEqJson(Mix));

    AddAssetVerification(Result, Mix);
    Result->SetStringField(TEXT("message"), TEXT("Sound mix described"));
    Ctx.SendSuccess(Result);
    return true;
}

// ---- audio.authoring.describe_dialogue_voice ----
// Live readback for DialogueVoice: create_dialogue_voice writes Gender + Plurality,
// but get_audio_info recognizes neither UDialogueVoice nor UDialogueWave — both fall
// through to its final else and return type:"Unknown" with nothing else. This is the
// Dialogue member of the describe_* family (describe_attenuation / describe_sound_class
// / describe_sound_mix), confirming a created voice's grammatical gender/plurality
// in-namespace instead of pivoting to property.get on the Gender field (which is even
// omitted from asset.dump's properties.json). Keys mirror the create_dialogue_voice
// params; the token ladders are the inverse of that handler's gender/plurality parse,
// so the create/describe round-trip can never diverge.
REGISTER_RPC_HANDLER("audio.authoring.describe_dialogue_voice", "audio.authoring",
    "Describe a DialogueVoice asset as structured JSON: gender and plurality - get_audio_info returns type:'Unknown' for DialogueVoice and echoes neither.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the DialogueVoice asset")
    ))
{
#if MCP_HAS_DIALOGUE
    FString AssetPath;
    if (!Ctx.RequireString(TEXT("assetPath"), AssetPath)) return true;

    UDialogueVoice* Voice = LoadDialogueVoiceFromPath(AssetPath);
    if (!Voice)
    {
        Ctx.SendError(ErrorCodes::ERR_VOICE_NOT_FOUND,
            FString::Printf(TEXT("Could not load DialogueVoice: %s"), *AssetPath));
        return true;
    }

    // Tokens come straight from the engine enum metadata (same StaticEnum +
    // GetNameStringByValue idiom SoundWaveAuthoringHandler uses for ESoundGroup), so the
    // EGrammaticalGender / EGrammaticalNumber value names (Neuter/Masculine/Feminine/Mixed,
    // Singular/Plural) round-trip with create_dialogue_voice's parse by construction — a new
    // engine enumerator surfaces under its real name with no hand-maintained ladder to drift.
    const UEnum* GenderEnum = StaticEnum<EGrammaticalGender::Type>();
    const UEnum* PluralityEnum = StaticEnum<EGrammaticalNumber::Type>();

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("type"), TEXT("DialogueVoice"));
    Result->SetStringField(TEXT("gender"), GenderEnum
        ? GenderEnum->GetNameStringByValue(static_cast<int64>(Voice->Gender)) : TEXT("Unknown"));
    Result->SetStringField(TEXT("plurality"), PluralityEnum
        ? PluralityEnum->GetNameStringByValue(static_cast<int64>(Voice->Plurality)) : TEXT("Unknown"));

    AddAssetVerification(Result, Voice);
    Result->SetStringField(TEXT("message"), TEXT("Dialogue voice described"));
    Ctx.SendSuccess(Result);
    return true;
#else
    Ctx.SendError(ErrorCodes::ERR_DIALOGUE_NOT_AVAILABLE, TEXT("Dialogue system not available"));
    return true;
#endif
}

// ---- audio.authoring.describe_dialogue_wave ----
// Live readback for DialogueWave: create_dialogue_wave writes SpokenText and
// set_dialogue_context writes per-context FDialogueContextMapping entries (speaker +
// targets voice paths), but get_audio_info returns type:"Unknown" for UDialogueWave
// and echoes neither. This is the Dialogue member of the describe_* family, confirming
// the spoken text and the speaker/target wiring in-namespace instead of pivoting to
// asset.dump's properties.json. Because it surfaces each mapping's Speaker AND the raw
// Targets list, it ALSO makes B-dialogue-context-null-target-prepended's stray-null
// target observable from the RPC surface (a null entry shows as an empty-string target
// path). Keys mirror the set_dialogue_context params.
REGISTER_RPC_HANDLER("audio.authoring.describe_dialogue_wave", "audio.authoring",
    "Describe a DialogueWave asset as structured JSON: spokenText plus a contexts[] array of per-mapping speaker + targets[] voice paths - get_audio_info returns type:'Unknown' for DialogueWave and echoes neither.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the DialogueWave asset")
    ))
{
#if MCP_HAS_DIALOGUE
    FString AssetPath;
    if (!Ctx.RequireString(TEXT("assetPath"), AssetPath)) return true;

    UDialogueWave* Wave = LoadDialogueWaveFromPath(AssetPath);
    if (!Wave)
    {
        Ctx.SendError(ErrorCodes::ERR_WAVE_NOT_FOUND,
            FString::Printf(TEXT("Could not load DialogueWave: %s"), *AssetPath));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("type"), TEXT("DialogueWave"));
    Result->SetStringField(TEXT("spokenText"), Wave->SpokenText);
    Result->SetNumberField(TEXT("contextCount"), Wave->ContextMappings.Num());

    // One object per FDialogueContextMapping (set_dialogue_context). Emit the speaker
    // and the raw Targets list — including any null entry — so the wiring is verifiable
    // in-namespace and a stray null surfaces as an empty target path rather than being
    // silently swallowed.
    TArray<TSharedPtr<FJsonValue>> Contexts;
    for (const FDialogueContextMapping& Mapping : Wave->ContextMappings)
    {
        TSharedPtr<FJsonObject> ContextObj = MakeShareable(new FJsonObject());
        ContextObj->SetStringField(TEXT("speaker"),
            Mapping.Context.Speaker ? Mapping.Context.Speaker->GetPathName() : FString());

        TArray<TSharedPtr<FJsonValue>> Targets;
        for (const UDialogueVoice* Target : Mapping.Context.Targets)
        {
            Targets.Add(MakeShareable(new FJsonValueString(
                Target ? Target->GetPathName() : FString())));
        }
        ContextObj->SetArrayField(TEXT("targets"), Targets);

        if (Mapping.SoundWave)
            ContextObj->SetStringField(TEXT("soundWave"), Mapping.SoundWave->GetPathName());
        ContextObj->SetStringField(TEXT("localizationKeyFormat"), Mapping.LocalizationKeyFormat);

        Contexts.Add(MakeShareable(new FJsonValueObject(ContextObj)));
    }
    Result->SetArrayField(TEXT("contexts"), Contexts);

    AddAssetVerification(Result, Wave);
    Result->SetStringField(TEXT("message"), TEXT("Dialogue wave described"));
    Ctx.SendSuccess(Result);
    return true;
#else
    Ctx.SendError(ErrorCodes::ERR_DIALOGUE_NOT_AVAILABLE, TEXT("Dialogue system not available"));
    return true;
#endif
}

// ---- audio.authoring.get_audio_info ----
REGISTER_RPC_HANDLER("audio.authoring.get_audio_info", "audio.authoring", "Get information about an audio asset",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the audio asset")
    ))
{
    FString AssetPath = NormalizeContentAssetPath(Ctx.GetString(TEXT("assetPath")));

    UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
    if (!Asset)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, FString::Printf(TEXT("Could not load asset: %s"), *AssetPath));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetStringField(TEXT("assetClass"), Asset->GetClass()->GetName());

    if (USoundCue* Cue = Cast<USoundCue>(Asset))
    {
        Result->SetStringField(TEXT("type"), TEXT("SoundCue"));
        Result->SetNumberField(TEXT("duration"), Cue->Duration);
        Result->SetNumberField(TEXT("nodeCount"), Cue->AllNodes.Num());
        if (Cue->AttenuationSettings)
            Result->SetStringField(TEXT("attenuationPath"), Cue->AttenuationSettings->GetPathName());
    }
    else if (USoundWave* Wave = Cast<USoundWave>(Asset))
    {
        Result->SetStringField(TEXT("type"), TEXT("SoundWave"));
        Result->SetNumberField(TEXT("duration"), Wave->Duration);
        Result->SetNumberField(TEXT("sampleRate"), Wave->GetSampleRateForCurrentPlatform());
        Result->SetNumberField(TEXT("numChannels"), Wave->NumChannels);
    }
    else if (USoundClass* SoundClass = Cast<USoundClass>(Asset))
    {
        Result->SetStringField(TEXT("type"), TEXT("SoundClass"));
        Result->SetNumberField(TEXT("volume"), SoundClass->Properties.Volume);
        Result->SetNumberField(TEXT("pitch"), SoundClass->Properties.Pitch);
        if (SoundClass->ParentClass)
            Result->SetStringField(TEXT("parentClass"), SoundClass->ParentClass->GetPathName());
#if MCP_HAS_SUBMIX
        if (SoundClass->Properties.DefaultSubmix)
            Result->SetStringField(TEXT("outputSubmix"), SoundClass->Properties.DefaultSubmix->GetPathName());
#endif
    }
    else if (USoundMix* Mix = Cast<USoundMix>(Asset))
    {
        Result->SetStringField(TEXT("type"), TEXT("SoundMix"));
        Result->SetNumberField(TEXT("modifierCount"), Mix->SoundClassEffects.Num());
    }
#if MCP_HAS_SUBMIX
    else if (USoundSubmix* Submix = Cast<USoundSubmix>(Asset))
    {
        Result->SetStringField(TEXT("type"), TEXT("SoundSubmix"));
        if (Submix->ParentSubmix)
            Result->SetStringField(TEXT("parentSubmix"), Submix->ParentSubmix->GetPathName());
    }
#endif
    else if (USoundAttenuation* Atten = Cast<USoundAttenuation>(Asset))
    {
        Result->SetStringField(TEXT("type"), TEXT("SoundAttenuation"));
        Result->SetNumberField(TEXT("falloffDistance"), Atten->Attenuation.FalloffDistance);
        Result->SetBoolField(TEXT("spatialize"), Atten->Attenuation.bSpatialize);
    }
#if MCP_HAS_DIALOGUE
    else if (UDialogueWave* DWave = Cast<UDialogueWave>(Asset))
    {
        // Thin recognition so get_audio_info no longer reports type:"Unknown" for a
        // DialogueWave; the full spokenText + context wiring readback lives in
        // describe_dialogue_wave (the inspect-after-mutate reader).
        Result->SetStringField(TEXT("type"), TEXT("DialogueWave"));
        Result->SetNumberField(TEXT("contextCount"), DWave->ContextMappings.Num());
    }
    else if (Asset->IsA<UDialogueVoice>())
    {
        // Thin recognition; gender/plurality readback lives in describe_dialogue_voice.
        Result->SetStringField(TEXT("type"), TEXT("DialogueVoice"));
    }
#endif
#if MCP_HAS_REVERB_EFFECT
    else if (UReverbEffect* Reverb = Cast<UReverbEffect>(Asset))
    {
        // Recognize the type create_reverb_effect authors so get_audio_info no longer
        // reports type:"Unknown" for it (mirrors the DialogueWave/DialogueVoice branches
        // above). Echo back the same float UPROPERTYs create_reverb_effect writes so the
        // readback is useful, not just a type label.
        Result->SetStringField(TEXT("type"), TEXT("ReverbEffect"));
        Result->SetNumberField(TEXT("decayTime"), Reverb->DecayTime);
        Result->SetNumberField(TEXT("gain"), Reverb->Gain);
        Result->SetNumberField(TEXT("gainHF"), Reverb->GainHF);
        Result->SetNumberField(TEXT("density"), Reverb->Density);
        Result->SetNumberField(TEXT("diffusion"), Reverb->Diffusion);
        Result->SetNumberField(TEXT("decayHFRatio"), Reverb->DecayHFRatio);
    }
#endif
    else
    {
        Result->SetStringField(TEXT("type"), TEXT("Unknown"));
    }

    Result->SetStringField(TEXT("message"), TEXT("Audio info retrieved"));
    Ctx.SendSuccess(Result);
    return true;
}
