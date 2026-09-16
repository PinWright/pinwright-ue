// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-connect-cue-nodes-crash.
//
// audio.authoring.connect_cue_nodes grew the source node's ChildNodes array and
// immediately called USoundCue::LinkGraphNodesFromSoundNodes() WITHOUT first
// reconstructing the paired USoundCueGraphNode's input pins. That engine routine
// (FSoundCueAudioEditor::LinkGraphNodesFromSoundNodes, SoundCueGraph.cpp:61)
// asserts check(InputPins.Num() == SoundNode->ChildNodes.Num()) for every node,
// so the first connect on any MCP-authored cue fatally check()-crashed the whole
// editor (crash dump UECC-Windows-E9E2955A..., CrashType=Assert).
//
// This test drives the real registered handlers (create_sound_cue -> add_cue_node
// x2 -> connect_cue_nodes) through the dispatcher and asserts the connect both
// succeeds (pre-fix it crashed the process, never returning) and leaves the engine
// invariant intact: the source node's input-pin count matches its child count and
// slot 0 is wired to the target. It also asserts a negative childIndex is rejected
// with a clean INVALID_CHILD_INDEX instead of an out-of-bounds fault. The fixture
// is built entirely in-code; it loads no external content.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
#include "UObject/UObjectGlobals.h"

#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "SoundCueGraph/SoundCueGraphNode.h"

#include "Tests/TestUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FConnectCueNodesSyncsInputPinsTest,
    "PinWright.Assets.SoundCue.ConnectCueNodes.SyncsInputPinsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConnectCueNodesSyncsInputPinsTest::RunTest(const FString& Parameters)
{
    const FString CueName = FString::Printf(TEXT("SC_ConnectPins_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString CuePath = TEXT("/Game/PinWrightTests/Audio");
    const FString PackagePath = FString::Printf(TEXT("%s/%s"), *CuePath, *CueName);

    // 1. Create an empty cue on disk. save=true anchors path resolution for the
    //    chained add/connect calls that re-load the cue by path.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), CueName);
        Payload->SetStringField(TEXT("path"), CuePath);
        Payload->SetBoolField(TEXT("save"), true);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("audio.authoring.create_sound_cue"), Payload, Capture);
        TestTrue(TEXT("create_sound_cue handler is registered"), bFound);
        TestTrue(TEXT("create_sound_cue succeeded"), Capture.bSuccess);
        if (!Capture.bSuccess)
        {
            return false;
        }
    }

    USoundCue* Cue = Cast<USoundCue>(
        StaticFindObject(USoundCue::StaticClass(), nullptr, *ToObjectPath(PackagePath)));
    TestNotNull(TEXT("created SoundCue resolves in memory"), Cue);
    if (!Cue)
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    // 2. Add two nodes via the real handler. add_cue_node also relinks, but each
    //    freshly constructed node has 0 children / 0 pins, so it never trips the
    //    invariant — only the connect (which grows ChildNodes) does.
    auto AddNode = [&](const TCHAR* NodeType) -> FString
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), PackagePath);
        Payload->SetStringField(TEXT("nodeType"), NodeType);
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("audio.authoring.add_cue_node"), Payload, Capture);
        TestTrue(*FString::Printf(TEXT("add_cue_node %s succeeded"), NodeType), Capture.bSuccess);
        FString NodeId;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetStringField(TEXT("nodeId"), NodeId);
        }
        return NodeId;
    };

    // Mixer accepts many children, so childIndex 0 is unambiguously valid.
    const FString SourceId = AddNode(TEXT("mixer"));
    const FString TargetId = AddNode(TEXT("random"));
    TestTrue(TEXT("source node id returned"), !SourceId.IsEmpty());
    TestTrue(TEXT("target node id returned"), !TargetId.IsEmpty());
    if (SourceId.IsEmpty() || TargetId.IsEmpty())
    {
        CleanupTestAsset(PackagePath);
        return false;
    }

    // 3. THE REGRESSION: connect source -> target at childIndex 0. Pre-fix this
    //    fatally check()-crashed the editor inside LinkGraphNodesFromSoundNodes
    //    (source ChildNodes grew to 1 while its EdGraph node still had 0 input
    //    pins). Post-fix the input pins are synced first and the connect succeeds.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), PackagePath);
        Payload->SetStringField(TEXT("sourceNodeId"), SourceId);
        Payload->SetStringField(TEXT("targetNodeId"), TargetId);
        Payload->SetNumberField(TEXT("childIndex"), 0);
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("audio.authoring.connect_cue_nodes"), Payload, Capture);
        TestTrue(TEXT("connect_cue_nodes handler is registered"), bFound);
        TestTrue(TEXT("connect_cue_nodes succeeded without crashing"), Capture.bSuccess);
    }

    // 4. Verify the wiring landed AND the engine invariant now holds on the source
    //    node — the exact condition the fatal check() guards.
    USoundNode* SourceNode = nullptr;
    USoundNode* TargetNode = nullptr;
    for (USoundNode* Node : Cue->AllNodes)
    {
        if (Node && Node->GetName() == SourceId) SourceNode = Node;
        if (Node && Node->GetName() == TargetId) TargetNode = Node;
    }
    TestNotNull(TEXT("source node found in cue"), SourceNode);
    TestNotNull(TEXT("target node found in cue"), TargetNode);
    if (SourceNode && TargetNode)
    {
        TestTrue(TEXT("source has a child slot 0"), SourceNode->ChildNodes.Num() >= 1);
        if (SourceNode->ChildNodes.Num() >= 1)
        {
            TestTrue(TEXT("child slot 0 wired to target"),
                SourceNode->ChildNodes[0].Get() == TargetNode);
        }
        USoundCueGraphNode* GraphNode = Cast<USoundCueGraphNode>(SourceNode->GetGraphNode());
        TestNotNull(TEXT("source has a paired graph node"), GraphNode);
        if (GraphNode)
        {
            TestEqual(TEXT("input pin count matches child count (invariant restored)"),
                GraphNode->GetInputCount(), SourceNode->ChildNodes.Num());
        }
    }

    // 5. A negative childIndex must be rejected cleanly, not fault on an
    //    out-of-bounds ChildNodes access.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), PackagePath);
        Payload->SetStringField(TEXT("sourceNodeId"), SourceId);
        Payload->SetStringField(TEXT("targetNodeId"), TargetId);
        Payload->SetNumberField(TEXT("childIndex"), -1);
        Payload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("audio.authoring.connect_cue_nodes"), Payload, Capture);
        TestFalse(TEXT("negative childIndex is not a fake success"), Capture.bSuccess);
        TestEqual(TEXT("negative childIndex error code"), Capture.ErrorCode,
            FString(TEXT("INVALID_CHILD_INDEX")));
    }

    CleanupTestAsset(PackagePath);
    return true;
}
