// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-cue-random-node-weights-zero-always-picks-first and
// E-cue-graph-verbs-cannot-set-firstnode.
//
// connect_cue_nodes used to grow the parent's ChildNodes with SetNum() + assign, which skips
// USoundNode::InsertChildNode — the only path that maintains the per-child arrays several node
// types keep parallel to ChildNodes. A cue authored through the RPC surface therefore came out
// with USoundNodeRandom::Weights zero-filled (ChooseNodeIndex's selection loop can never break
// at a zero weight sum, so child 0 plays on every trigger) and USoundNodeMixer::InputVolume
// empty (ParseNodes indexes it by child index with no bounds guard). Every readback the surface
// offered reported a correct graph.
//
// These tests drive the real registered handlers through the dispatcher and assert the arrays
// the engine's own insert path would have filled, plus the readback signals that make a
// degenerate node visible. The set_cue_root coverage is the other half: add_cue_node +
// connect_cue_nodes could build a complete tree and had no verb able to write FirstNode, so the
// documented workflow ended in a silent cue. Fixtures are built in-code; no external content.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundNodeLooping.h"
#include "Sound/SoundNodeMixer.h"
#include "Sound/SoundNodeRandom.h"
#include "Sound/SoundWave.h"
#include "SoundCueGraph/SoundCueGraphNode.h"

#include "Tests/TestUtils.h"

// File-local namespace (not anonymous) so these helper names cannot collide with a sibling
// test file's under a Unity merge.
namespace SoundCueChildAttachmentTests
{
    // Creates an empty cue on disk. save=true anchors path resolution for the chained add /
    // connect / describe calls, which all re-load the cue by path.
    bool CreateCue(FAutomationTestBase& Test, const FString& PackagePath, const FString& CueName)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), CueName);
        Payload->SetStringField(TEXT("path"), FPackageName::GetLongPackagePath(PackagePath));
        Payload->SetBoolField(TEXT("save"), true);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("audio.authoring.create_sound_cue"), Payload, Capture);
        Test.TestTrue(TEXT("create_sound_cue handler is registered"), bFound);
        Test.TestTrue(TEXT("create_sound_cue succeeded"), Capture.bSuccess);
        return Capture.bSuccess;
    }

    FString AddNode(FAutomationTestBase& Test, const FString& PackagePath, const TCHAR* NodeType)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), PackagePath);
        Payload->SetStringField(TEXT("nodeType"), NodeType);
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("audio.authoring.add_cue_node"), Payload, Capture);
        Test.TestTrue(*FString::Printf(TEXT("add_cue_node %s succeeded"), NodeType), Capture.bSuccess);

        FString NodeId;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetStringField(TEXT("nodeId"), NodeId);
        }
        return NodeId;
    }

    void Connect(const FString& PackagePath, const FString& SourceId, const FString& TargetId,
        int32 ChildIndex, FTestResponseCapture& Capture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), PackagePath);
        Payload->SetStringField(TEXT("sourceNodeId"), SourceId);
        Payload->SetStringField(TEXT("targetNodeId"), TargetId);
        Payload->SetNumberField(TEXT("childIndex"), ChildIndex);
        Payload->SetBoolField(TEXT("save"), false);

        InvokeHandlerWithCapture(TEXT("audio.authoring.connect_cue_nodes"), Payload, Capture);
    }

    USoundNode* FindNode(USoundCue* Cue, const FString& NodeId)
    {
        if (!Cue)
        {
            return nullptr;
        }
        for (USoundNode* Node : Cue->AllNodes)
        {
            if (Node && Node->GetName() == NodeId)
            {
                return Node;
            }
        }
        return nullptr;
    }

    USoundCue* ResolveCue(const FString& PackagePath)
    {
        return Cast<USoundCue>(StaticFindObject(USoundCue::StaticClass(), nullptr, *ToObjectPath(PackagePath)));
    }

    // Returns every warning decompile_sound_cue reports for the cue.
    TArray<FString> DecompileWarnings(FAutomationTestBase& Test, const FString& PackagePath, FString& OutIr)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), PackagePath);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("audio.authoring.decompile_sound_cue"), Payload, Capture);
        Test.TestTrue(TEXT("decompile_sound_cue succeeded"), Capture.bSuccess);

        TArray<FString> Warnings;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetStringField(TEXT("ir"), OutIr);
            const TArray<TSharedPtr<FJsonValue>>* WarningValues = nullptr;
            if (Capture.Result->TryGetArrayField(TEXT("warnings"), WarningValues) && WarningValues)
            {
                for (const TSharedPtr<FJsonValue>& Value : *WarningValues)
                {
                    if (Value.IsValid())
                    {
                        Warnings.Add(Value->AsString());
                    }
                }
            }
        }
        return Warnings;
    }

    bool AnyWarningContains(const TArray<FString>& Warnings, const TCHAR* Needle)
    {
        for (const FString& Warning : Warnings)
        {
            if (Warning.Contains(Needle))
            {
                return true;
            }
        }
        return false;
    }

    // The describe_sound_cue node block for NodeId, or an invalid pointer.
    TSharedPtr<FJsonObject> DescribeNode(FAutomationTestBase& Test, const FString& PackagePath, const FString& NodeId)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), PackagePath);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("audio.authoring.describe_sound_cue"), Payload, Capture);
        Test.TestTrue(TEXT("describe_sound_cue succeeded"), Capture.bSuccess);

        if (!Capture.Result.IsValid())
        {
            return nullptr;
        }

        const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
        if (!Capture.Result->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
        {
            return nullptr;
        }

        for (const TSharedPtr<FJsonValue>& Value : *Nodes)
        {
            const TSharedPtr<FJsonObject>* Entry = nullptr;
            if (Value.IsValid() && Value->TryGetObject(Entry) && Entry && (*Entry)->GetStringField(TEXT("name")) == NodeId)
            {
                return *Entry;
            }
        }
        return nullptr;
    }
}

// ============================================================================
// B-cue-random-node-weights-zero-always-picks-first
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSoundCueRandomWeightsDefaultToOneTest,
    "PinWright.audio.authoring.connect_cue_nodes.RandomWeightsDefaultToOne",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSoundCueRandomWeightsDefaultToOneTest::RunTest(const FString& Parameters)
{
    using namespace SoundCueChildAttachmentTests;

    const FString CueName = FString::Printf(TEXT("SC_RandomWeights_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackagePath = FString::Printf(TEXT("/Game/PinWrightTests/Audio/%s"), *CueName);

    if (!CreateCue(*this, PackagePath, CueName))
    {
        return false;
    }

    const FString RandomId = AddNode(*this, PackagePath, TEXT("random"));
    const FString WaveA = AddNode(*this, PackagePath, TEXT("wave_player"));
    const FString WaveB = AddNode(*this, PackagePath, TEXT("wave_player"));
    const FString WaveC = AddNode(*this, PackagePath, TEXT("wave_player"));
    if (RandomId.IsEmpty() || WaveA.IsEmpty() || WaveB.IsEmpty() || WaveC.IsEmpty())
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    const FString ChildIds[] = { WaveA, WaveB, WaveC };
    for (int32 Index = 0; Index < 3; ++Index)
    {
        FTestResponseCapture Capture;
        Connect(PackagePath, RandomId, ChildIds[Index], Index, Capture);
        TestTrue(*FString::Printf(TEXT("connect child %d succeeded"), Index), Capture.bSuccess);
    }

    USoundCue* Cue = ResolveCue(PackagePath);
    TestNotNull(TEXT("created SoundCue resolves in memory"), Cue);
    USoundNodeRandom* Random = Cast<USoundNodeRandom>(FindNode(Cue, RandomId));
    TestNotNull(TEXT("random node found in cue"), Random);

    if (Random)
    {
        // THE REGRESSION. Pre-fix: ChildNodes 3, Weights [] -> PostLoad's FixWeightsArray
        // AddZeroed's it to [0,0,0], ChooseNodeIndex's loop never breaks, child 0 plays always.
        TestEqual(TEXT("random has three children"), Random->ChildNodes.Num(), 3);
        TestEqual(TEXT("Weights has one entry per child"), Random->Weights.Num(), 3);
        for (int32 Index = 0; Index < Random->Weights.Num(); ++Index)
        {
            TestEqual(*FString::Printf(TEXT("Weights[%d] is the engine's 1.0 default"), Index),
                Random->Weights[Index], 1.0f);
        }

        // The other array USoundNodeRandom::InsertChildNode maintains; a short HasBeenUsed
        // breaks without-replacement selection the same silent way.
        TestEqual(TEXT("HasBeenUsed has one entry per child"), Random->HasBeenUsed.Num(), 3);

        USoundCueGraphNode* GraphNode = Cast<USoundCueGraphNode>(Random->GetGraphNode());
        TestNotNull(TEXT("random has a paired graph node"), GraphNode);
        if (GraphNode)
        {
            TestEqual(TEXT("input pin count matches child count"),
                GraphNode->GetInputCount(), Random->ChildNodes.Num());
        }
    }

    // Readback 1: the SCIR warnings channel names the defect when it is present, and stays
    // silent about weights when it is not.
    {
        FString Ir;
        const TArray<FString> Warnings = DecompileWarnings(*this, PackagePath, Ir);
        TestFalse(TEXT("no zero-weight warning on a correctly authored random node"),
            AnyWarningContains(Warnings, TEXT("Weights sum to 0")));
        TestFalse(TEXT("no per-child-array mismatch warning"),
            AnyWarningContains(Warnings, TEXT("out of step with ChildNodes")));
    }

    // Readback 2: describe_sound_cue pairs the weights with the child count, so a caller can
    // see the values without knowing that Weights is the node's on/off switch.
    {
        // The cue must be rooted for describe to walk the tree from FirstNode.
        TSharedPtr<FJsonObject> RootPayload = MakeShared<FJsonObject>();
        RootPayload->SetStringField(TEXT("assetPath"), PackagePath);
        RootPayload->SetStringField(TEXT("nodeId"), RandomId);
        RootPayload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture RootCapture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("audio.authoring.set_cue_root"), RootPayload, RootCapture);
        TestTrue(TEXT("set_cue_root handler is registered"), bFound);
        TestTrue(TEXT("set_cue_root succeeded"), RootCapture.bSuccess);

        TSharedPtr<FJsonObject> NodeJson = DescribeNode(*this, PackagePath, RandomId);
        TestTrue(TEXT("describe_sound_cue reports the random node"), NodeJson.IsValid());
        if (NodeJson.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* ChildValues = nullptr;
            const bool bHasChildValues = NodeJson->TryGetArrayField(TEXT("childValues"), ChildValues);
            TestTrue(TEXT("random node block carries childValues"), bHasChildValues && ChildValues);
            if (bHasChildValues && ChildValues && ChildValues->Num() > 0)
            {
                const TSharedPtr<FJsonObject>* Entry = nullptr;
                if ((*ChildValues)[0].IsValid() && (*ChildValues)[0]->TryGetObject(Entry) && Entry)
                {
                    TestEqual(TEXT("childValues names the Weights property"),
                        (*Entry)->GetStringField(TEXT("property")), FString(TEXT("Weights")));
                    TestTrue(TEXT("childValues matches the child count"),
                        (*Entry)->GetBoolField(TEXT("matchesChildCount")));

                    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
                    if ((*Entry)->TryGetArrayField(TEXT("values"), Values) && Values)
                    {
                        TestEqual(TEXT("childValues reports three weights"), Values->Num(), 3);
                        for (const TSharedPtr<FJsonValue>& Value : *Values)
                        {
                            TestEqual(TEXT("each reported weight is 1.0"),
                                static_cast<float>(Value->AsNumber()), 1.0f);
                        }
                    }
                }
            }
        }
    }

    CleanupTestAsset(PackagePath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSoundCueMixerInputVolumeMatchesChildrenTest,
    "PinWright.audio.authoring.connect_cue_nodes.MixerInputVolumeMatchesChildren",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSoundCueMixerInputVolumeMatchesChildrenTest::RunTest(const FString& Parameters)
{
    using namespace SoundCueChildAttachmentTests;

    const FString CueName = FString::Printf(TEXT("SC_MixerVolumes_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackagePath = FString::Printf(TEXT("/Game/PinWrightTests/Audio/%s"), *CueName);

    if (!CreateCue(*this, PackagePath, CueName))
    {
        return false;
    }

    const FString MixerId = AddNode(*this, PackagePath, TEXT("mixer"));
    const FString WaveA = AddNode(*this, PackagePath, TEXT("wave_player"));
    const FString WaveB = AddNode(*this, PackagePath, TEXT("wave_player"));
    if (MixerId.IsEmpty() || WaveA.IsEmpty() || WaveB.IsEmpty())
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    {
        FTestResponseCapture Capture;
        Connect(PackagePath, MixerId, WaveA, 0, Capture);
        TestTrue(TEXT("connect mixer child 0 succeeded"), Capture.bSuccess);
    }
    {
        FTestResponseCapture Capture;
        Connect(PackagePath, MixerId, WaveB, 1, Capture);
        TestTrue(TEXT("connect mixer child 1 succeeded"), Capture.bSuccess);
    }

    USoundNodeMixer* Mixer = Cast<USoundNodeMixer>(FindNode(ResolveCue(PackagePath), MixerId));
    TestNotNull(TEXT("mixer node found in cue"), Mixer);
    if (Mixer)
    {
        // Nothing in the engine repairs InputVolume — there is no PostLoad fix-up as there is
        // for Random's Weights — and USoundNodeMixer::ParseNodes indexes it by child index with
        // no bounds check, so a short array is an out-of-bounds read at playback.
        TestEqual(TEXT("mixer has two children"), Mixer->ChildNodes.Num(), 2);
        TestEqual(TEXT("InputVolume has one entry per child"), Mixer->InputVolume.Num(), 2);
        for (int32 Index = 0; Index < Mixer->InputVolume.Num(); ++Index)
        {
            TestEqual(*FString::Printf(TEXT("InputVolume[%d] is the engine's 1.0 default"), Index),
                Mixer->InputVolume[Index], 1.0f);
        }
    }

    CleanupTestAsset(PackagePath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSoundCueChildIndexBeyondMaxRejectedTest,
    "PinWright.audio.authoring.connect_cue_nodes.ChildIndexBeyondMaxRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSoundCueChildIndexBeyondMaxRejectedTest::RunTest(const FString& Parameters)
{
    using namespace SoundCueChildAttachmentTests;

    const FString CueName = FString::Printf(TEXT("SC_ChildIndexMax_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackagePath = FString::Printf(TEXT("/Game/PinWrightTests/Audio/%s"), *CueName);

    if (!CreateCue(*this, PackagePath, CueName))
    {
        return false;
    }

    // A modulator accepts exactly one child, so slot 1 does not exist. The old SetNum() growth
    // accepted it and produced a second child the engine's own readbacks clamp away as
    // unreachable; the insert path refuses it instead.
    const FString ModulatorId = AddNode(*this, PackagePath, TEXT("modulator"));
    const FString WaveId = AddNode(*this, PackagePath, TEXT("wave_player"));
    if (ModulatorId.IsEmpty() || WaveId.IsEmpty())
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    FTestResponseCapture Capture;
    Connect(PackagePath, ModulatorId, WaveId, 1, Capture);
    TestFalse(TEXT("a slot past the node's max child count is not a fake success"), Capture.bSuccess);
    TestEqual(TEXT("error code"), Capture.ErrorCode, FString(TEXT("INVALID_CHILD_INDEX")));

    USoundNode* Modulator = FindNode(ResolveCue(PackagePath), ModulatorId);
    TestNotNull(TEXT("modulator node found in cue"), Modulator);
    if (Modulator)
    {
        TestTrue(TEXT("the refused connect added no child slot"), Modulator->ChildNodes.Num() <= 1);
    }

    CleanupTestAsset(PackagePath);
    return true;
}

// ============================================================================
// E-cue-graph-verbs-cannot-set-firstnode
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSoundCueSetCueRootRootsTheGraphTest,
    "PinWright.audio.authoring.set_cue_root.RootsTheGraph",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSoundCueSetCueRootRootsTheGraphTest::RunTest(const FString& Parameters)
{
    using namespace SoundCueChildAttachmentTests;

    const FString CueName = FString::Printf(TEXT("SC_CueRoot_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackagePath = FString::Printf(TEXT("/Game/PinWrightTests/Audio/%s"), *CueName);

    if (!CreateCue(*this, PackagePath, CueName))
    {
        return false;
    }

    const FString ModulatorId = AddNode(*this, PackagePath, TEXT("modulator"));
    const FString WaveId = AddNode(*this, PackagePath, TEXT("wave_player"));
    if (ModulatorId.IsEmpty() || WaveId.IsEmpty())
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    {
        FTestResponseCapture Capture;
        Connect(PackagePath, ModulatorId, WaveId, 0, Capture);
        TestTrue(TEXT("connect modulator -> wave player succeeded"), Capture.bSuccess);
    }

    // Before rooting: a complete tree that cannot play, and a warning that now names the verb
    // which fixes it.
    {
        FString Ir;
        const TArray<FString> Warnings = DecompileWarnings(*this, PackagePath, Ir);
        TestTrue(TEXT("an unrooted cue warns it has no FirstNode"),
            AnyWarningContains(Warnings, TEXT("no FirstNode")));
        TestTrue(TEXT("the warning names set_cue_root"),
            AnyWarningContains(Warnings, TEXT("audio.authoring.set_cue_root")));
    }

    // The reserved sourceNodeId, which used to return SOURCE_NODE_NOT_FOUND.
    {
        FTestResponseCapture Capture;
        Connect(PackagePath, TEXT("Output"), ModulatorId, 0, Capture);
        TestTrue(TEXT("connect_cue_nodes accepts sourceNodeId \"Output\" as the cue root"),
            Capture.bSuccess);
    }

    USoundCue* Cue = ResolveCue(PackagePath);
    TestNotNull(TEXT("created SoundCue resolves in memory"), Cue);
    if (Cue)
    {
        TestNotNull(TEXT("FirstNode is set"), Cue->FirstNode.Get());
        if (Cue->FirstNode)
        {
            TestEqual(TEXT("FirstNode is the modulator"), Cue->FirstNode->GetName(), ModulatorId);
        }
    }

    {
        FString Ir;
        const TArray<FString> Warnings = DecompileWarnings(*this, PackagePath, Ir);
        TestFalse(TEXT("the rooted cue no longer warns about FirstNode"),
            AnyWarningContains(Warnings, TEXT("no FirstNode")));
        TestTrue(TEXT("SCIR marks the modulator as the root, not an orphan"),
            Ir.Contains(TEXT("root modulator")));
    }

    CleanupTestAsset(PackagePath);
    return true;
}

// ============================================================================
// create_sound_cue's own chain builds children the same way
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSoundCueCreateChainSyncsInputPinsTest,
    "PinWright.audio.authoring.create_sound_cue.ChainSyncsInputPins",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSoundCueCreateChainSyncsInputPinsTest::RunTest(const FString& Parameters)
{
    using namespace SoundCueChildAttachmentTests;

    // A SoundWave resolvable by path, so create_sound_cue builds a wave player and chains the
    // looping node above it. Transient: this test never needs it on disk.
    const FString WaveName = FString::Printf(TEXT("SW_CueChain_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString WavePackage = FString::Printf(TEXT("/Game/PinWrightTests/Audio/%s"), *WaveName);
    UPackage* WavePkg = CreatePackage(*WavePackage);
    USoundWave* Wave = NewObject<USoundWave>(WavePkg, USoundWave::StaticClass(), *WaveName,
        RF_Public | RF_Standalone | RF_Transient);
    TestNotNull(TEXT("fixture SoundWave created"), Wave);
    if (!Wave)
    {
        return false;
    }
    Wave->AddToRoot();
    FAssetRegistryModule::AssetCreated(Wave);
    ON_SCOPE_EXIT { Wave->RemoveFromRoot(); };

    const FString CueName = FString::Printf(TEXT("SC_CueChain_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackagePath = FString::Printf(TEXT("/Game/PinWrightTests/Audio/%s"), *CueName);

    // looping=true chains a USoundNodeLooping above the wave player. Pre-fix that chain pushed
    // straight into ChildNodes, leaving the looping node with one child and zero input pins —
    // and the LinkGraphNodesFromSoundNodes() call at the end of the same handler asserts those
    // are equal, so this create fatally check()-crashed the editor.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), CueName);
    Payload->SetStringField(TEXT("path"), FPackageName::GetLongPackagePath(PackagePath));
    Payload->SetStringField(TEXT("wavePath"), ToObjectPath(WavePackage));
    Payload->SetBoolField(TEXT("looping"), true);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("audio.authoring.create_sound_cue"), Payload, Capture);
    TestTrue(TEXT("create_sound_cue with a looping chain succeeded without crashing"), Capture.bSuccess);

    USoundCue* Cue = ResolveCue(PackagePath);
    TestNotNull(TEXT("created SoundCue resolves in memory"), Cue);
    if (Cue)
    {
        TestNotNull(TEXT("the cue is rooted"), Cue->FirstNode.Get());
        USoundNodeLooping* Looping = Cast<USoundNodeLooping>(Cue->FirstNode.Get());
        TestNotNull(TEXT("the looping node is the root"), Looping);
        if (Looping)
        {
            TestEqual(TEXT("looping has one child"), Looping->ChildNodes.Num(), 1);
            USoundCueGraphNode* GraphNode = Cast<USoundCueGraphNode>(Looping->GetGraphNode());
            TestNotNull(TEXT("looping has a paired graph node"), GraphNode);
            if (GraphNode)
            {
                TestEqual(TEXT("input pin count matches child count"),
                    GraphNode->GetInputCount(), Looping->ChildNodes.Num());
            }
        }
    }

    CleanupTestAsset(PackagePath);
    return true;
}
