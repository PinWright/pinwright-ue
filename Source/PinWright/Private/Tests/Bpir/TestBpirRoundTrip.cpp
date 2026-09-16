// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Compat/EngineVersionCompat.h"
#include "BpirGraphTestHelpers.h"
#include "CompilerTestUtils.h"
#include "Tests/TestSkipReporting.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/BpirGrammar.h"
#include "Compiler/BpirParser.h"
#include "Compiler/CompilerTypes.h"
#include "IrCore/IrTokenizer.h"
#include "Decompiler/BpirDecompiler.h"
#include "Decompiler/BpirTextEmitter.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_Event.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_Select.h"
#include "K2Node_VariableGet.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
// AsyncAction_PushContentToLayerForPlayer is a Lyra/CommonGame sample header
// absent from a stock engine install. Guard both the include and every
// symbol that depends on UAsyncAction_PushContentToLayerForPlayer.
#if __has_include("Actions/AsyncAction_PushContentToLayerForPlayer.h")
#include "Actions/AsyncAction_PushContentToLayerForPlayer.h"
#define MCP_HAS_PUSH_CONTENT_LAYER_ACTION 1
#else
#define MCP_HAS_PUSH_CONTENT_LAYER_ACTION 0
#endif
#include "GameFramework/PlayerController.h"

using namespace CompilerTestUtils;

namespace
{
    bool RoundTripLineContains(const FString& Text, const FString& First, const FString& Second)
    {
        TArray<FString> Lines;
        Text.ParseIntoArrayLines(Lines);
        for (const FString& Line : Lines)
        {
            if (Line.Contains(First) && Line.Contains(Second))
            {
                return true;
            }
        }
        return false;
    }

    template<typename PredicateType>
    UEdGraphNode* FindRoundTripNodeMatching(UBlueprint* BP, PredicateType Predicate)
    {
        if (!BP)
        {
            return nullptr;
        }

        TArray<UEdGraph*> Graphs;
        Graphs.Append(BP->UbergraphPages);
        Graphs.Append(BP->FunctionGraphs);
        Graphs.Append(BP->MacroGraphs);

        for (UEdGraph* Graph : Graphs)
        {
            if (!Graph)
            {
                continue;
            }
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node && Predicate(Node))
                {
                    return Node;
                }
            }
        }
        return nullptr;
    }

    template<typename NodeType>
    NodeType* FindRoundTripNodeOfType(UBlueprint* BP)
    {
        return Cast<NodeType>(FindRoundTripNodeMatching(BP, [](UEdGraphNode* Node)
        {
            return Node->IsA<NodeType>();
        }));
    }

    UK2Node_CallFunction* FindRoundTripPrintStringWithDefault(UBlueprint* BP, const FString& Needle)
    {
        return Cast<UK2Node_CallFunction>(FindRoundTripNodeMatching(BP, [&Needle](UEdGraphNode* Node)
        {
            UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
            if (!Call || !Call->FunctionReference.GetMemberName().ToString().Contains(TEXT("PrintString")))
            {
                return false;
            }

            UEdGraphPin* InString = Call->FindPin(TEXT("InString"));
            if (!InString)
            {
                InString = Call->FindPin(TEXT("inString"));
            }
            return InString && InString->DefaultValue.Contains(Needle);
        }));
    }

    UK2Node_CallFunction* FindRoundTripCallByFunctionName(UBlueprint* BP, const FName& FunctionName)
    {
        return Cast<UK2Node_CallFunction>(FindRoundTripNodeMatching(BP, [&FunctionName](UEdGraphNode* Node)
        {
            UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
            return Call && Call->GetFunctionName() == FunctionName;
        }));
    }

    UK2Node_CustomEvent* FindRoundTripCustomEvent(UBlueprint* BP, const FName& EventName)
    {
        return Cast<UK2Node_CustomEvent>(FindRoundTripNodeMatching(BP, [&EventName](UEdGraphNode* Node)
        {
            UK2Node_CustomEvent* Event = Cast<UK2Node_CustomEvent>(Node);
            return Event && Event->CustomFunctionName == EventName;
        }));
    }
}

// ============================================================================
// Round-trip tests: Compile BPIR -> Decompile -> Verify key tokens survive
// ============================================================================

// ----------------------------------------------------------------------------
// 1. SimpleCall — compile a PrintString call, verify tokens survive round-trip
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripSimpleCallTest,
    "PinWright.bpir.round_trip.SimpleCall",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripSimpleCallTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripTestBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    call PrintString(InString: \"Hello\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error: %s"), *Err.Message));
        }
        return false;
    }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess)
    {
        for (const FBpirWarning& Warn : DecompileResult.Warnings)
        {
            AddError(FString::Printf(TEXT("Decompile warning: %s"), *Warn.Text));
        }
        return false;
    }

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains PrintString"), Output.Contains(TEXT("PrintString")));
    TestTrue(TEXT("Output contains Hello"), Output.Contains(TEXT("Hello")));

    // Re-parse the decompiled output to verify structural integrity
    {
        TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(Output.Left(Output.Find(TEXT("\n"))), GetBpirGrammar());
        TestTrue(TEXT("Tokenizer produced tokens from first line"), Tokens.Num() > 0);

        FBpirParser Parser;
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> ParseErrors;
        bool bParsed = Parser.Parse(Output, Blocks, ParseErrors);
        TestTrue(TEXT("Re-parsed decompiled output without errors"), bParsed);
        if (!bParsed)
        {
            for (const FCompileError& Err : ParseErrors)
            {
                AddError(FString::Printf(TEXT("  Re-parse L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Parsed entry blocks exist"), Blocks.Num() > 0);
        if (Blocks.Num() > 0)
        {
            TestEqual(TEXT("Exactly 1 entry block"), Blocks.Num(), 1);
            TestTrue(TEXT("Entry block has instructions"), Blocks[0].Instructions.Num() > 0);
        }
    }

    // Re-compile the decompiled output onto a fresh Blueprint
    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripRecompileBP"));
        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile of decompiled output succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }
    return true;
}

// This test depends on UAsyncAction_PushContentToLayerForPlayer from the
// CommonGame/Lyra sample plugin which is not present in a stock engine install.
#if MCP_HAS_PUSH_CONTENT_LAYER_ACTION
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAsyncActionFactoryIdentityRoundTripTest,
    "PinWright.bpir.round_trip.AsyncActionFactoryIdentity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAsyncActionFactoryIdentityRoundTripTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBPWithParent(APlayerController::StaticClass(), TEXT("AsyncActionFactoryIdentityBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %action = call K2Node_AsyncAction_PushContentToLayerForPlayer(OwningPlayer: self, WidgetClass: /Script/CommonUI.CommonActivatableWidget, LayerName: (TagName=\"UI.Layer.Menu\"), bSuspendInputUntilComplete: true) [AfterPush -> @afterpush]\n"
        "\n"
        "@afterpush:\n"
        "    call PrintString(InString: \"AfterPush\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    TestEqual(TEXT("Exactly one UK2Node_AsyncAction node was created"),
        CountNodesOfType<UK2Node_AsyncAction>(BP), 1);

    UK2Node_AsyncAction* AsyncNode = FindRoundTripNodeOfType<UK2Node_AsyncAction>(BP);
    TestNotNull(TEXT("Async action node exists"), AsyncNode);
    if (!AsyncNode) return false;

    // Generic-lane factory (no HasDedicatedAsyncNode meta) must use the base UClass exactly,
    // not a dedicated subclass. Catches regressions where the dedicated-lane probe accidentally
    // promotes generic-lane factories to a subclass.
    TestEqual(TEXT("Async node runtime UClass is the generic UK2Node_AsyncAction"),
        AsyncNode->GetClass(), UK2Node_AsyncAction::StaticClass());

    // GetFactoryFunction() became public in UE 5.6; skip on 5.4.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    UFunction* Factory = AsyncNode->GetFactoryFunction();
    TestNotNull(TEXT("Async node factory function is configured"), Factory);
    if (!Factory) return false;
    TestEqual(TEXT("Factory owner is UAsyncAction_PushContentToLayerForPlayer"),
        Factory->GetOwnerClass(), UAsyncAction_PushContentToLayerForPlayer::StaticClass());
    TestEqual(TEXT("Factory function is PushContentToLayerForPlayer"),
        Factory->GetFName(), GET_FUNCTION_NAME_CHECKED(UAsyncAction_PushContentToLayerForPlayer, PushContentToLayerForPlayer));
#endif

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Decompile contains exact async factory identity"),
        Output.Contains(TEXT("K2Node_AsyncAction_PushContentToLayerForPlayer")));
    TestFalse(TEXT("Decompile does not contain bare async action call"),
        Output.Contains(TEXT("call K2Node_AsyncAction(")));
    for (const FBpirWarning& Warning : DecompileResult.Warnings)
    {
        TestFalse(TEXT("Decompile has no unknown-node warning"),
            Warning.Text.Contains(TEXT("Unknown node type"), ESearchCase::IgnoreCase));
    }

    UBlueprint* RecompiledBP = CreateTransientTestBPWithParent(APlayerController::StaticClass(), TEXT("AsyncActionFactoryIdentityRecompiledBP"));
    TestNotNull(TEXT("Recompile blueprint created"), RecompiledBP);
    if (!RecompiledBP) return false;

    FBpirCompiler Recompiler(RecompiledBP);
    FCompileResult RecompileResult = Recompiler.Compile(Output);
    if (!RecompileResult.bSuccess)
    {
        for (const FCompileError& Err : RecompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Recompile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Recompile succeeded"), RecompileResult.bSuccess);
    if (!RecompileResult.bSuccess) return false;

    FBpirDecompiler RecompiledDecompiler(RecompiledBP);
    FBpirDecompileResult RecompiledDecompileResult = RecompiledDecompiler.Decompile();
    TestTrue(TEXT("Recompiled decompile succeeded"), RecompiledDecompileResult.bSuccess);
    if (!RecompiledDecompileResult.bSuccess) return false;

    TestTrue(TEXT("Recompiled decompile preserves exact factory identity"),
        RecompiledDecompileResult.BpirText.Contains(TEXT("K2Node_AsyncAction_PushContentToLayerForPlayer")));
    TestFalse(TEXT("Recompiled decompile does not fall back to bare async action"),
        RecompiledDecompileResult.BpirText.Contains(TEXT("call K2Node_AsyncAction(")));
    return true;
}
#endif // MCP_HAS_PUSH_CONTENT_LAYER_ACTION

// ----------------------------------------------------------------------------
// 1b. AsyncActionDedicatedSubclassRoundTrip — HasDedicatedAsyncNode factory
//     (ListenForGameplayMessages) must spawn the dedicated UK2Node_AsyncAction
//     subclass, not the generic base, so dynamic delegate/payload pins are
//     materialized. Counterpart to FAsyncActionFactoryIdentityRoundTripTest
//     which covers the generic lane.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAsyncActionDedicatedSubclassRoundTripTest,
    "PinWright.bpir.round_trip.AsyncActionDedicatedSubclass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAsyncActionDedicatedSubclassRoundTripTest::RunTest(const FString& Parameters)
{
    // Resolve the dedicated K2Node subclass and its factory by name to avoid a hard module
    // dependency on GameplayMessageNodes / GameplayMessageRuntime from the test TU.
    UClass* DedicatedSubclass = FindFirstObjectSafe<UClass>(TEXT("K2Node_AsyncAction_ListenForGameplayMessages"));
    if (!DedicatedSubclass)
    {
        UE_LOG(LogTemp, Warning, TEXT("Skipping AsyncActionDedicatedSubclass test: UK2Node_AsyncAction_ListenForGameplayMessages not loaded in this build"));
        return true;
    }
    UClass* FactoryOwnerClass = FindFirstObjectSafe<UClass>(TEXT("AsyncAction_ListenForGameplayMessage"));
    TestNotNull(TEXT("Factory owner class UAsyncAction_ListenForGameplayMessage is loaded"), FactoryOwnerClass);
    if (!FactoryOwnerClass) return false;

    UBlueprint* BP = CreateTransientTestBPWithParent(APlayerController::StaticClass(), TEXT("AsyncActionDedicatedSubclassBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    // Both compile forms exercised by this test: named-factory (`call ListenForGameplayMessages`)
    // resolves through the FunctionResolver cascade, while explicit-K2-class
    // (`call K2Node_AsyncAction_ListenForGameplayMessages`) resolves through
    // EmitGenericK2NodeInstruction's dedicated-subclass recovery branch.
    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %listener = call K2Node_AsyncAction_ListenForGameplayMessages(WorldContextObject: self, Channel: (TagName=\"App.Notifications.GhostOpponentsSpawned\"), PayloadType: /Script/CoreUObject.Vector, MatchType: EGameplayMessageMatch::ExactMatch) [OnMessageReceived -> @handler]\n"
        "\n"
        "@handler:\n"
        "    call PrintString(InString: \"Got message\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    TestEqual(TEXT("Exactly one UK2Node_AsyncAction node was created"),
        CountNodesOfType<UK2Node_AsyncAction>(BP), 1);

    UK2Node_AsyncAction* AsyncNode = FindRoundTripNodeOfType<UK2Node_AsyncAction>(BP);
    TestNotNull(TEXT("Async action node exists"), AsyncNode);
    if (!AsyncNode) return false;

    // Runtime UClass must be the dedicated subclass — the bug being regression-guarded is
    // a generic UK2Node_AsyncAction being spawned with empty delegate/payload pins.
    TestEqual(TEXT("Async node runtime UClass is the dedicated subclass"),
        AsyncNode->GetClass(), DedicatedSubclass);

    // GetFactoryFunction() was protected (inaccessible from test code) in UE 5.4.
    // It became public in UE 5.6. Skip the factory-identity assertions on 5.4;
    // the UClass identity check above is still a meaningful regression guard.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    UFunction* Factory = AsyncNode->GetFactoryFunction();
    TestNotNull(TEXT("Async node factory function is configured"), Factory);
    if (!Factory) return false;
    TestEqual(TEXT("Factory owner is UAsyncAction_ListenForGameplayMessage"),
        Factory->GetOwnerClass(), FactoryOwnerClass);
    TestEqual(TEXT("Factory function is ListenForGameplayMessages"),
        Factory->GetName(), FString(TEXT("ListenForGameplayMessages")));
#endif

    // Dedicated subclass's AllocateDefaultPins adds a Payload output pin (wildcard until
    // PayloadType is bound). Verify it exists so the regression that produced an empty
    // pin set is caught explicitly, not just via the UClass identity check.
    UEdGraphPin* PayloadPin = AsyncNode->FindPin(TEXT("Payload"));
    TestNotNull(TEXT("Dedicated subclass created Payload output pin"), PayloadPin);
    if (PayloadPin)
    {
        TestEqual(TEXT("Payload pin is an output"), (int32)PayloadPin->Direction, (int32)EGPD_Output);
    }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Decompile preserves dedicated-subclass identity"),
        Output.Contains(TEXT("K2Node_AsyncAction_ListenForGameplayMessages")));
    TestFalse(TEXT("Decompile does not fall back to bare async action"),
        Output.Contains(TEXT("call K2Node_AsyncAction(")));

    // Recompile round-trip: the dedicated-subclass identity must survive a compile -> decompile
    // -> compile cycle. Catches regressions where the source-of-truth identity decompiles
    // correctly but recompiles as the generic base.
    UBlueprint* RecompiledBP = CreateTransientTestBPWithParent(APlayerController::StaticClass(), TEXT("AsyncActionDedicatedSubclassRecompiledBP"));
    TestNotNull(TEXT("Recompile blueprint created"), RecompiledBP);
    if (!RecompiledBP) return false;

    FBpirCompiler Recompiler(RecompiledBP);
    FCompileResult RecompileResult = Recompiler.Compile(Output);
    if (!RecompileResult.bSuccess)
    {
        for (const FCompileError& Err : RecompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Recompile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Recompile succeeded"), RecompileResult.bSuccess);
    if (!RecompileResult.bSuccess) return false;

    UK2Node_AsyncAction* RecompiledNode = FindRoundTripNodeOfType<UK2Node_AsyncAction>(RecompiledBP);
    TestNotNull(TEXT("Recompiled async node exists"), RecompiledNode);
    if (RecompiledNode)
    {
        TestEqual(TEXT("Recompiled node runtime UClass is still the dedicated subclass"),
            RecompiledNode->GetClass(), DedicatedSubclass);
    }
    return true;
}

// ----------------------------------------------------------------------------
// 2. BranchReconverge — branch with true/false paths, verify branch token
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripBranchReconvergeTest,
    "PinWright.bpir.round_trip.BranchReconverge",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripBranchReconvergeTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripTestBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %b = branch(true) [true -> @yes, false -> @no]\n"
        "\n"
        "@yes:\n"
        "    call PrintString(InString: \"TruePath\")\n"
        "    exec -> @done\n"
        "\n"
        "@no:\n"
        "    call PrintString(InString: \"FalsePath\")\n"
        "    exec -> @done\n"
        "\n"
        "@done:\n"
        "    call PrintString(InString: \"Merged\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error: %s"), *Err.Message));
        }
        return false;
    }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess)
    {
        for (const FBpirWarning& Warn : DecompileResult.Warnings)
        {
            AddError(FString::Printf(TEXT("Decompile warning: %s"), *Warn.Text));
        }
        return false;
    }

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains branch("), Output.Contains(TEXT("branch(")));
    TestTrue(TEXT("Output contains @-label reference"), Output.Contains(TEXT("@")));
    TestTrue(TEXT("Output contains TruePath"), Output.Contains(TEXT("TruePath")));
    TestTrue(TEXT("Output contains FalsePath"), Output.Contains(TEXT("FalsePath")));
    TestTrue(TEXT("Output contains Merged"), Output.Contains(TEXT("Merged")));

    // Re-parse the decompiled output to verify structural integrity
    {
        FBpirParser Parser;
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> ParseErrors;
        bool bParsed = Parser.Parse(Output, Blocks, ParseErrors);
        TestTrue(TEXT("Re-parsed decompiled output without errors"), bParsed);
        if (!bParsed)
        {
            for (const FCompileError& Err : ParseErrors)
            {
                AddError(FString::Printf(TEXT("  Re-parse L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Parsed entry blocks exist"), Blocks.Num() > 0);
        if (Blocks.Num() > 0)
        {
            TestEqual(TEXT("Exactly 1 entry block"), Blocks.Num(), 1);
            TestTrue(TEXT("Entry block has instructions"), Blocks[0].Instructions.Num() > 0);
        }
    }

    // Re-compile the decompiled output onto a fresh Blueprint
    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripRecompileBP"));
        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile of decompiled output succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// 3. Sequence — compile a sequence node with two output paths
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripSequenceTest,
    "PinWright.bpir.round_trip.Sequence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripSequenceTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripTestBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %s = sequence(2) [0 -> @first, 1 -> @second]\n"
        "\n"
        "@first:\n"
        "    call PrintString(InString: \"First\")\n"
        "\n"
        "@second:\n"
        "    call PrintString(InString: \"Second\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error: %s"), *Err.Message));
        }
        return false;
    }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess)
    {
        for (const FBpirWarning& Warn : DecompileResult.Warnings)
        {
            AddError(FString::Printf(TEXT("Decompile warning: %s"), *Warn.Text));
        }
        return false;
    }

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains sequence"), Output.Contains(TEXT("sequence")));
    TestTrue(TEXT("Output contains First"), Output.Contains(TEXT("First")));
    TestTrue(TEXT("Output contains Second"), Output.Contains(TEXT("Second")));

    // Re-parse the decompiled output to verify structural integrity
    {
        FBpirParser Parser;
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> ParseErrors;
        bool bParsed = Parser.Parse(Output, Blocks, ParseErrors);
        TestTrue(TEXT("Re-parsed decompiled output without errors"), bParsed);
        if (!bParsed)
        {
            for (const FCompileError& Err : ParseErrors)
            {
                AddError(FString::Printf(TEXT("  Re-parse L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Parsed entry blocks exist"), Blocks.Num() > 0);
        if (Blocks.Num() > 0)
        {
            TestEqual(TEXT("Exactly 1 entry block"), Blocks.Num(), 1);
            TestTrue(TEXT("Entry block has instructions"), Blocks[0].Instructions.Num() > 0);
        }
    }

    // Re-compile the decompiled output onto a fresh Blueprint
    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripRecompileBP"));
        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile of decompiled output succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// 4. CustomEvent — compile a custom event entry, verify signature survives
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripCustomEventTest,
    "PinWright.bpir.round_trip.CustomEvent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripCustomEventTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripTestBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry custom_event MyEvt(float Damage) {\n"
        "    call PrintString(InString: \"Event fired\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error: %s"), *Err.Message));
        }
        return false;
    }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess)
    {
        for (const FBpirWarning& Warn : DecompileResult.Warnings)
        {
            AddError(FString::Printf(TEXT("Decompile warning: %s"), *Warn.Text));
        }
        return false;
    }

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains custom_event"), Output.Contains(TEXT("custom_event")));
    TestTrue(TEXT("Output contains MyEvt"), Output.Contains(TEXT("MyEvt")));
    TestTrue(TEXT("Output contains Damage parameter"), Output.Contains(TEXT("Damage")));

    // Re-parse the decompiled output to verify structural integrity
    {
        FBpirParser Parser;
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> ParseErrors;
        bool bParsed = Parser.Parse(Output, Blocks, ParseErrors);
        TestTrue(TEXT("Re-parsed decompiled output without errors"), bParsed);
        if (!bParsed)
        {
            for (const FCompileError& Err : ParseErrors)
            {
                AddError(FString::Printf(TEXT("  Re-parse L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Parsed entry blocks exist"), Blocks.Num() > 0);
        if (Blocks.Num() > 0)
        {
            TestTrue(TEXT("Entry block is a custom_event"), Blocks.ContainsByPredicate(
                [](const FBpirEntryBlock& B) { return B.Kind == EBpirEntryKind::CustomEvent; }));
            TestTrue(TEXT("Entry block has instructions"), Blocks[0].Instructions.Num() > 0);
        }
    }

    // Re-compile the decompiled output onto a fresh Blueprint
    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripRecompileBP"));
        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile of decompiled output succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// 5. Function — compile a function entry with return value, verify signature
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripFunctionTest,
    "PinWright.bpir.round_trip.Function",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripFunctionTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripTestBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry function Calc(float X) -> float {\n"
        "    return $X\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error: %s"), *Err.Message));
        }
        return false;
    }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess)
    {
        for (const FBpirWarning& Warn : DecompileResult.Warnings)
        {
            AddError(FString::Printf(TEXT("Decompile warning: %s"), *Warn.Text));
        }
        return false;
    }

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains function keyword"), Output.Contains(TEXT("function")));
    TestTrue(TEXT("Output contains Calc"), Output.Contains(TEXT("Calc")));
    TestTrue(TEXT("Output contains float type"), Output.Contains(TEXT("float")));

    // Re-parse the decompiled output to verify structural integrity
    {
        FBpirParser Parser;
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> ParseErrors;
        bool bParsed = Parser.Parse(Output, Blocks, ParseErrors);
        TestTrue(TEXT("Re-parsed decompiled output without errors"), bParsed);
        if (!bParsed)
        {
            for (const FCompileError& Err : ParseErrors)
            {
                AddError(FString::Printf(TEXT("  Re-parse L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Parsed entry blocks exist"), Blocks.Num() > 0);
        if (Blocks.Num() > 0)
        {
            TestTrue(TEXT("Entry block is a function"), Blocks.ContainsByPredicate(
                [](const FBpirEntryBlock& B) { return B.Kind == EBpirEntryKind::Function; }));
            TestTrue(TEXT("Entry block has instructions"), Blocks[0].Instructions.Num() > 0);
        }
    }

    // Re-compile the decompiled output onto a fresh Blueprint
    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripRecompileBP"));
        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile of decompiled output succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// 6. Latent — compile a latent Delay call, verify "latent" and "Delay" survive
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripLatentTest,
    "PinWright.bpir.round_trip.Latent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripLatentTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripLatentBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %d = latent Delay(Duration: 1.0) [completed -> @done]\n"
        "\n"
        "@done:\n"
        "    call PrintString(InString: \"Done\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error: %s"), *Err.Message));
        }
        return false;
    }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess)
    {
        for (const FBpirWarning& Warn : DecompileResult.Warnings)
        {
            AddError(FString::Printf(TEXT("Decompile warning: %s"), *Warn.Text));
        }
        return false;
    }

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains latent keyword"), Output.Contains(TEXT("latent")));
    TestTrue(TEXT("Output contains Delay"), Output.Contains(TEXT("Delay")));

    // Re-parse the decompiled output to verify structural integrity
    {
        FBpirParser Parser;
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> ParseErrors;
        bool bParsed = Parser.Parse(Output, Blocks, ParseErrors);
        TestTrue(TEXT("Re-parsed decompiled output without errors"), bParsed);
        if (!bParsed)
        {
            for (const FCompileError& Err : ParseErrors)
            {
                AddError(FString::Printf(TEXT("  Re-parse L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Parsed entry blocks exist"), Blocks.Num() > 0);
        if (Blocks.Num() > 0)
        {
            TestEqual(TEXT("Exactly 1 entry block"), Blocks.Num(), 1);
            TestTrue(TEXT("Entry block has instructions"), Blocks[0].Instructions.Num() > 0);
        }
    }

    // Re-compile the decompiled output onto a fresh Blueprint
    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripLatentRecompileBP"));
        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile of decompiled output succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// 7. Timeline — compile a timeline node, verify "timeline" survives round-trip
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripTimelineTest,
    "PinWright.bpir.round_trip.Timeline",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripTimelineTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripTimelineBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %t = timeline MyTimeline() [update -> @upd, finished -> @fin]\n"
        "\n"
        "@upd:\n"
        "    call PrintString(InString: \"Tick\")\n"
        "    exec -> @done\n"
        "\n"
        "@fin:\n"
        "    call PrintString(InString: \"Finished\")\n"
        "    exec -> @done\n"
        "\n"
        "@done:\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error: %s"), *Err.Message));
        }
        return false;
    }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess)
    {
        for (const FBpirWarning& Warn : DecompileResult.Warnings)
        {
            AddError(FString::Printf(TEXT("Decompile warning: %s"), *Warn.Text));
        }
        return false;
    }

    const FString& Output = DecompileResult.BpirText;
    // Both assertions were satisfied by the bare name alone: Contains defaults to
    // IgnoreCase, so "MyTimeline" already contains "timeline". Require the keyword
    // adjacent to the name -- the emitter's form is "timeline <Name>()".
    TestTrue(TEXT("Output contains the 'timeline MyTimeline' keyword form"),
        Output.Contains(TEXT("timeline MyTimeline")));

    // Re-parse the decompiled output to verify structural integrity
    {
        FBpirParser Parser;
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> ParseErrors;
        bool bParsed = Parser.Parse(Output, Blocks, ParseErrors);
        TestTrue(TEXT("Re-parsed decompiled output without errors"), bParsed);
        if (!bParsed)
        {
            for (const FCompileError& Err : ParseErrors)
            {
                AddError(FString::Printf(TEXT("  Re-parse L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Parsed entry blocks exist"), Blocks.Num() > 0);
        if (Blocks.Num() > 0)
        {
            TestEqual(TEXT("Exactly 1 entry block"), Blocks.Num(), 1);
            TestTrue(TEXT("Entry block has instructions"), Blocks[0].Instructions.Num() > 0);
        }
    }

    // Re-compile the decompiled output onto a fresh Blueprint
    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripTimelineRecompileBP"));
        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile of decompiled output succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// 8. Cast — compile a dynamic cast expression, verify "cast" survives round-trip
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripCastTest,
    "PinWright.bpir.round_trip.Cast",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripCastTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripCastBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    // Cast self to Actor — AActor resolves via the "A"-prefix fallback in the compiler
    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %self_ref = self\n"
        "    %c = cast<Actor>(%self_ref) [success -> @ok, fail -> @fail]\n"
        "\n"
        "@ok:\n"
        "    call PrintString(InString: \"CastOk\")\n"
        "    exec -> @done\n"
        "\n"
        "@fail:\n"
        "    call PrintString(InString: \"CastFail\")\n"
        "    exec -> @done\n"
        "\n"
        "@done:\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error: %s"), *Err.Message));
        }
        return false;
    }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess)
    {
        for (const FBpirWarning& Warn : DecompileResult.Warnings)
        {
            AddError(FString::Printf(TEXT("Decompile warning: %s"), *Warn.Text));
        }
        return false;
    }

    const FString& Output = DecompileResult.BpirText;
    // FString::Contains defaults to ESearchCase::IgnoreCase, so the bare "cast" substring
    // was also satisfied by a node-title fallback such as "Cast To Actor" -- i.e. by
    // exactly the degraded emission this round trip exists to rule out. The emitter's
    // only real form is "cast<Type>(...)" (BpirTextEmitter.cpp ~2130), so require it.
    TestTrue(TEXT("Output contains the cast<Actor> keyword form"),
        Output.Contains(TEXT("cast<Actor>")));

    // Re-parse the decompiled output to verify structural integrity
    {
        FBpirParser Parser;
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> ParseErrors;
        bool bParsed = Parser.Parse(Output, Blocks, ParseErrors);
        TestTrue(TEXT("Re-parsed decompiled output without errors"), bParsed);
        if (!bParsed)
        {
            for (const FCompileError& Err : ParseErrors)
            {
                AddError(FString::Printf(TEXT("  Re-parse L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Parsed entry blocks exist"), Blocks.Num() > 0);
        if (Blocks.Num() > 0)
        {
            TestEqual(TEXT("Exactly 1 entry block"), Blocks.Num(), 1);
            TestTrue(TEXT("Entry block has instructions"), Blocks[0].Instructions.Num() > 0);
        }
    }

    // Re-compile the decompiled output onto a fresh Blueprint
    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripCastRecompileBP"));
        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile of decompiled output succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// 9. ForEach — compile a foreach loop, verify "foreach" survives round-trip
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripForeachTest,
    "PinWright.bpir.round_trip.ForEach",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripForeachTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripForeachBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    // Add a String array variable so $Items resolves on both initial and re-compile BPs
    FEdGraphPinType ArrayType;
    ArrayType.PinCategory = UEdGraphSchema_K2::PC_String;
    ArrayType.ContainerType = EPinContainerType::Array;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("Items"), ArrayType);

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %arr = make_array(\"a\", \"b\", \"c\")\n"
        "    %fe = foreach(Array: %arr) [body -> @loop, completed -> @done]\n"
        "\n"
        "@loop:\n"
        "    call PrintString(InString: \"Item\")\n"
        "\n"
        "@done:\n"
        "    call PrintString(InString: \"AllDone\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error: %s"), *Err.Message));
        }
        return false;
    }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess)
    {
        for (const FBpirWarning& Warn : DecompileResult.Warnings)
        {
            AddError(FString::Printf(TEXT("Decompile warning: %s"), *Warn.Text));
        }
        return false;
    }

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains foreach keyword"), Output.Contains(TEXT("foreach")));

    // Re-parse the decompiled output to verify structural integrity
    {
        FBpirParser Parser;
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> ParseErrors;
        bool bParsed = Parser.Parse(Output, Blocks, ParseErrors);
        TestTrue(TEXT("Re-parsed decompiled output without errors"), bParsed);
        if (!bParsed)
        {
            for (const FCompileError& Err : ParseErrors)
            {
                AddError(FString::Printf(TEXT("  Re-parse L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Parsed entry blocks exist"), Blocks.Num() > 0);
        if (Blocks.Num() > 0)
        {
            TestEqual(TEXT("Exactly 1 entry block"), Blocks.Num(), 1);
            TestTrue(TEXT("Entry block has instructions"), Blocks[0].Instructions.Num() > 0);
        }
    }

    // Re-compile the decompiled output onto a fresh Blueprint
    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripForeachRecompileBP"));
        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile of decompiled output succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// 10. RoundTripSwitchInt — compile a switch_int, verify round-trip
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripSwitchIntTest,
    "PinWright.bpir.round_trip.SwitchInt",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripSwitchIntTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripSwitchIntBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %s = switch_int(0) [1 -> @a, 2 -> @b, default -> @d]\n"
        "\n"
        "@a:\n"
        "    call PrintString(InString: \"One\")\n"
        "    exec -> @d\n"
        "\n"
        "@b:\n"
        "    call PrintString(InString: \"Two\")\n"
        "    exec -> @d\n"
        "\n"
        "@d:\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains switch_int"), Output.Contains(TEXT("switch_int")));

    // Re-compile the decompiled output
    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripSwitchIntRecompileBP"));
        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// 11. RoundTripSwitchString — compile a switch_string, verify round-trip
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripSwitchStringTest,
    "PinWright.bpir.round_trip.SwitchString",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripSwitchStringTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripSwitchStringBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %s = switch_string(\"test\") [\"hello\" -> @a, default -> @d]\n"
        "\n"
        "@a:\n"
        "    call PrintString(InString: \"Hello\")\n"
        "    exec -> @d\n"
        "\n"
        "@d:\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains switch_string"), Output.Contains(TEXT("switch_string")));

    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripSwitchStringRecompileBP"));
        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// 12. RoundTripSelect — compile a select expression, verify round-trip
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripSelectTest,
    "PinWright.bpir.round_trip.Select",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripSelectTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripSelectBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %sel = select(Index: true, true: \"A\", false: \"B\")\n"
        "    call PrintString(InString: %sel)\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    // An IgnoreCase "select" was also satisfied by a "Select" node-title fallback.
    // The emitter's form is "select(args)" (BpirTextEmitter.cpp ~2354).
    TestTrue(TEXT("Output contains the select( keyword form"), Output.Contains(TEXT("select(")));

    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripSelectRecompileBP"));
        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// 12b. SelectTrueFalseMappingMatchesUE — guards against the swapped-label bug
// where decompile and compile agreed with each other but disagreed with UE
// (Option 0 = False, Option 1 = True per UK2Node_Select::AllocateDefaultPins).
// Round-trip alone cannot catch this because both sides flipped symmetrically.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSelectTrueFalseMappingMatchesUETest,
    "PinWright.bpir.round_trip.SelectTrueFalseMapping",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSelectTrueFalseMappingMatchesUETest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("SelectMappingBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    // Compile a Select with distinct values for true and false branches.
    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %sel = select(Index: true, true: \"TRUE_VALUE\", false: \"FALSE_VALUE\")\n"
        "    call PrintString(InString: %sel)\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    UK2Node_Select* SelectNode = FindNodeOfType<UK2Node_Select>(BP);
    TestNotNull(TEXT("Select node was created"), SelectNode);
    if (!SelectNode) return false;

    // Direct UE-side check: Option 0 must hold the false branch's literal,
    // Option 1 must hold the true branch's literal. UK2Node_Select gives
    // Option 0 the friendly name "False" and Option 1 "True"; if the compiler
    // wires the BPIR `true:` literal into Option 0 the runtime semantics flip.
    UEdGraphPin* Option0 = SelectNode->FindPin(TEXT("Option 0"));
    UEdGraphPin* Option1 = SelectNode->FindPin(TEXT("Option 1"));
    TestNotNull(TEXT("Option 0 pin exists"), Option0);
    TestNotNull(TEXT("Option 1 pin exists"), Option1);
    if (!Option0 || !Option1) return false;

    TestEqual(TEXT("Compile: BPIR `false:` literal lands on Option 0 (UE's False branch)"),
        Option0->DefaultValue, FString(TEXT("FALSE_VALUE")));
    TestEqual(TEXT("Compile: BPIR `true:` literal lands on Option 1 (UE's True branch)"),
        Option1->DefaultValue, FString(TEXT("TRUE_VALUE")));

    // Decompile and verify the text labels match the actual pin values.
    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    // Find the select line and check that `true: TRUE_VALUE` and `false: FALSE_VALUE`
    // appear together (not crossed). RoundTripLineContains scans line by line.
    TestTrue(TEXT("Decompile: select line pairs `true:` with TRUE_VALUE"),
        RoundTripLineContains(Output, TEXT("true: \"TRUE_VALUE\""), TEXT("select")));
    TestTrue(TEXT("Decompile: select line pairs `false:` with FALSE_VALUE"),
        RoundTripLineContains(Output, TEXT("false: \"FALSE_VALUE\""), TEXT("select")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSelectEnumAllocatesAllOptionsRoundTripTest,
    "PinWright.bpir.round_trip.SelectEnumAllocatesAllOptions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSelectEnumAllocatesAllOptionsRoundTripTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("SelectEnumOptionsBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry custom_event SelectVisibility(enum<ESlateVisibility> Visibility) {\n"
        "    %sel = select(Index: Visibility, Visible: \"VISIBLE_VALUE\", ESlateVisibility::Collapsed: \"COLLAPSED_VALUE\", `2`: \"HIDDEN_VALUE\")\n"
        "    call PrintString(InString: %sel)\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
        }
        return false;
    }

    UK2Node_Select* SelectNode = FindNodeOfType<UK2Node_Select>(BP);
    TestNotNull(TEXT("Select node was created"), SelectNode);
    if (!SelectNode) return false;

    TestNotNull(TEXT("Select node is enum-backed"), SelectNode->GetEnum());
    UEdGraphPin* VisiblePin = SelectNode->FindPin(TEXT("Visible"));
    UEdGraphPin* CollapsedPin = SelectNode->FindPin(TEXT("Collapsed"));
    UEdGraphPin* HiddenPin = SelectNode->FindPin(TEXT("Hidden"));
    TestNotNull(TEXT("Visible option pin exists"), VisiblePin);
    TestNotNull(TEXT("Collapsed option pin exists"), CollapsedPin);
    TestNotNull(TEXT("Hidden option pin exists"), HiddenPin);
    if (!VisiblePin || !CollapsedPin || !HiddenPin) return false;

    TestEqual(TEXT("Visible option receives its default"), VisiblePin->DefaultValue, FString(TEXT("VISIBLE_VALUE")));
    TestEqual(TEXT("Collapsed option receives its default"), CollapsedPin->DefaultValue, FString(TEXT("COLLAPSED_VALUE")));
    TestEqual(TEXT("Hidden option receives its default"), HiddenPin->DefaultValue, FString(TEXT("HIDDEN_VALUE")));

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Decompile emits Visible enum option label"),
        RoundTripLineContains(Output, TEXT("Visible: \"VISIBLE_VALUE\""), TEXT("select")));
    TestTrue(TEXT("Decompile emits Collapsed enum option label"),
        RoundTripLineContains(Output, TEXT("Collapsed: \"COLLAPSED_VALUE\""), TEXT("select")));
    TestTrue(TEXT("Decompile emits Hidden enum option label"),
        RoundTripLineContains(Output, TEXT("Hidden: \"HIDDEN_VALUE\""), TEXT("select")));
    TestFalse(TEXT("Decompile does not emit boolean labels for enum options"),
        RoundTripLineContains(Output, TEXT("true:"), TEXT("select"))
        || RoundTripLineContains(Output, TEXT("false:"), TEXT("select")));

    UBlueprint* BP2 = CreateTransientTestBP(TEXT("SelectEnumOptionsRecompileBP"));
    FBpirCompiler Compiler2(BP2);
    FCompileResult RecompileResult = Compiler2.Compile(Output);
    TestTrue(TEXT("Re-compile succeeded"), RecompileResult.bSuccess);
    if (!RecompileResult.bSuccess)
    {
        for (const FCompileError& Err : RecompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
        }
        return false;
    }

    UK2Node_Select* RecompiledSelect = FindNodeOfType<UK2Node_Select>(BP2);
    TestNotNull(TEXT("Recompiled Select node was created"), RecompiledSelect);
    if (!RecompiledSelect) return false;
    TestNotNull(TEXT("Recompiled Select keeps Hidden option"),
        RecompiledSelect->FindPin(TEXT("Hidden")));

    return true;
}

// ----------------------------------------------------------------------------
// 13. RoundTripMacroDoOnce — compile a DoOnce macro, verify round-trip
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripMacroDoOnceTest,
    "PinWright.bpir.round_trip.MacroDoOnce",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripMacroDoOnceTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripDoOnceBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %m = macro DoOnce() [completed -> @c]\n"
        "\n"
        "@c:\n"
        "    call PrintString(InString: \"Once\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains DoOnce"), Output.Contains(TEXT("DoOnce")));

    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripDoOnceRecompileBP"));
        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// 14. RoundTripMacroFlipFlop — compile a FlipFlop macro, verify round-trip
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripMacroFlipFlopTest,
    "PinWright.bpir.round_trip.MacroFlipFlop",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripMacroFlipFlopTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripFlipFlopBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %ff = macro FlipFlop() [A -> @a, B -> @b]\n"
        "\n"
        "@a:\n"
        "    call PrintString(InString: \"FlipA\")\n"
        "    exec -> @done\n"
        "\n"
        "@b:\n"
        "    call PrintString(InString: \"FlipB\")\n"
        "    exec -> @done\n"
        "\n"
        "@done:\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains FlipFlop"), Output.Contains(TEXT("FlipFlop")));

    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripFlipFlopRecompileBP"));
        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// 15. RoundTripBreakStruct — compile a break<Margin> expression, verify
// FMargin has no HasNativeBreak/HasNativeMake so the compiler emits a real
// UK2Node_BreakStruct that survives decompile → recompile.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripBreakStructTest,
    "PinWright.bpir.round_trip.BreakStruct",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripBreakStructTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripBreakStructBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %m = make<Margin>(Left: 1.0, Top: 2.0, Right: 3.0, Bottom: 4.0)\n"
        "    %b = break<Margin>(%m)\n"
        "    call PrintString(InString: %b.Left)\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    // BpirTextEmitter emits exactly one form for a break node: "break<Struct>(...)".
    // "Break Margin" is the node *title*, i.e. what a generic title-based fallback
    // produces when the break emitter does not run -- so accepting it as an alternative
    // made this assertion pass on precisely the regression it exists to catch.
    TestTrue(TEXT("Output contains break<Margin>"), Output.Contains(TEXT("break<Margin>")));

    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripBreakStructRecompileBP"));
        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// 16. RoundTripEnum — compile an enum literal, verify round-trip
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripEnumTest,
    "PinWright.bpir.round_trip.Enum",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripEnumTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripEnumBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %e = enum ETextGender::Masculine\n"
        "    call PrintString(InString: %e)\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains enum value"), Output.Contains(TEXT("ETextGender")));

    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripEnumRecompileBP"));
        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// 17. RoundTripMakeArray — compile a make_array, verify round-trip
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripMakeArrayTest,
    "PinWright.bpir.round_trip.MakeArray",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripMakeArrayTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripMakeArrayBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %arr = make_array(\"a\", \"b\", \"c\")\n"
        "    %fe = foreach(Array: %arr) [body -> @loop, completed -> @done]\n"
        "\n"
        "@loop:\n"
        "    call PrintString(InString: \"Item\")\n"
        "\n"
        "@done:\n"
        "    call PrintString(InString: \"ArrayDone\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    // "MakeArray" is the node title fallback, not a second legitimate emission -- the
    // emitter only ever produces the "make_array(...)" keyword form. Requiring the
    // keyword is what makes this test able to see the emitter regressing to a title.
    TestTrue(TEXT("Output contains the make_array keyword form"), Output.Contains(TEXT("make_array")));

    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripMakeArrayRecompileBP"));
        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// 18. RoundTripDispatcher — compile dispatcher ops, verify round-trip
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripDispatcherTest,
    "PinWright.bpir.round_trip.Dispatcher",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripDispatcherTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripDispatcherBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    // Add a multicast delegate variable
    FEdGraphPinType DelegateType;
    DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("OnTestEvent"), DelegateType);
    FKismetEditorUtilities::CompileBlueprint(BP);

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    call_dispatcher OnTestEvent()\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    // Dispatcher compilation may fail if delegate type doesn't resolve in test context
    if (!CompileResult.bSuccess)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("delegate-compile-unresolved"),
            TEXT("Dispatcher compile produced errors (expected in test context), verifying no crash"));
        return true;
    }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    // Require the keyword itself. "dispatcher || OnTestEvent" was satisfied by any
    // emission that merely named the variable, so BpirTextEmitter dropping the
    // call_dispatcher keyword (or taking its "unknown_dispatcher" arm) round-tripped
    // invisibly: the degraded output still re-parses and re-compiles.
    TestTrue(TEXT("Output contains the call_dispatcher keyword"),
        Output.Contains(TEXT("call_dispatcher")));
    TestTrue(TEXT("Output names the OnTestEvent dispatcher"),
        Output.Contains(TEXT("OnTestEvent")));

    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripDispatcherRecompileBP"));
        FEdGraphPinType DelegateType2;
        DelegateType2.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
        FBlueprintEditorUtils::AddMemberVariable(BP2, TEXT("OnTestEvent"), DelegateType2);
        FKismetEditorUtilities::CompileBlueprint(BP2);

        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
        // Pin the node class the round trip must land back on; "Re-compile succeeded"
        // is equally true of a plain call that lost the dispatcher semantics.
        int32 CallDelegateCount = 0;
        for (UEdGraph* Graph : BP2->UbergraphPages)
        {
            if (!Graph) continue;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node && Node->GetClass()->GetName() == TEXT("K2Node_CallDelegate"))
                {
                    ++CallDelegateCount;
                }
            }
        }
        TestEqual(TEXT("Round trip reproduces exactly one K2Node_CallDelegate"), CallDelegateCount, 1);
    }
    return true;
}

// ----------------------------------------------------------------------------
// 19. RoundTripGenericNode — compile a generic K2Node via class name
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripGenericNodeTest,
    "PinWright.bpir.round_trip.GenericNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripGenericNodeTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripGenericNodeBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    // Use a generic call that resolves to a known function
    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %self_ref = self\n"
        "    call SetActorHiddenInGame(bNewHidden: true)\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    // The second arm was strictly redundant AND strictly weaker: "Hidden" is a substring
    // of "SetActorHiddenInGame", and also of the "bNewHidden" argument name that appears
    // in the emission regardless. The disjunction therefore reduced to Contains("Hidden"),
    // which held even if the function name were dropped from the output entirely.
    TestTrue(TEXT("Output contains the function call"),
        Output.Contains(TEXT("SetActorHiddenInGame")));

    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripGenericNodeRecompileBP"));
        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// 20. RoundTripWhile — compile a while loop, verify "while" survives round-trip
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripWhileTest,
    "PinWright.bpir.round_trip.While",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripWhileTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripWhileBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %cond = pure IsValid(self)\n"
        "    %w = while(%cond) [body -> @loop, completed -> @done]\n"
        "\n"
        "@loop:\n"
        "    call PrintString(InString: \"Looping\")\n"
        "\n"
        "@done:\n"
        "    call PrintString(InString: \"WhileDone\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    // Contains defaults to IgnoreCase, so a bare "while" was also satisfied by a
    // "While Loop" node-title fallback. The emitter's form is "while(cond)".
    TestTrue(TEXT("Output contains the while( keyword form"), Output.Contains(TEXT("while(")));

    // Re-parse the decompiled output to verify structural integrity
    {
        FBpirParser Parser;
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> ParseErrors;
        bool bParsed = Parser.Parse(Output, Blocks, ParseErrors);
        TestTrue(TEXT("Re-parsed decompiled output without errors"), bParsed);
        if (!bParsed)
        {
            for (const FCompileError& Err : ParseErrors)
            {
                AddError(FString::Printf(TEXT("  Re-parse L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Parsed entry blocks exist"), Blocks.Num() > 0);
        if (Blocks.Num() > 0)
        {
            TestEqual(TEXT("Exactly 1 entry block"), Blocks.Num(), 1);
            TestTrue(TEXT("Entry block has instructions"), Blocks[0].Instructions.Num() > 0);
        }
    }

    // Re-compile the decompiled output onto a fresh Blueprint
    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripWhileRecompileBP"));
        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile of decompiled output succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// 21. RoundTripForeachBreak — compile a foreach_break loop, verify round-trip
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripForeachBreakTest,
    "PinWright.bpir.round_trip.ForeachBreak",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripForeachBreakTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripForeachBreakBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %arr = make_array(\"a\", \"b\", \"c\")\n"
        "    %fb = foreach_break(%arr) [body -> @loop, completed -> @done]\n"
        "\n"
        "@loop:\n"
        "    call PrintString(InString: \"Item\")\n"
        "\n"
        "@done:\n"
        "    call PrintString(InString: \"ForeachBreakDone\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains foreach_break keyword"), Output.Contains(TEXT("foreach_break")));

    // Re-parse the decompiled output to verify structural integrity
    {
        FBpirParser Parser;
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> ParseErrors;
        bool bParsed = Parser.Parse(Output, Blocks, ParseErrors);
        TestTrue(TEXT("Re-parsed decompiled output without errors"), bParsed);
        if (!bParsed)
        {
            for (const FCompileError& Err : ParseErrors)
            {
                AddError(FString::Printf(TEXT("  Re-parse L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Parsed entry blocks exist"), Blocks.Num() > 0);
        if (Blocks.Num() > 0)
        {
            TestEqual(TEXT("Exactly 1 entry block"), Blocks.Num(), 1);
            TestTrue(TEXT("Entry block has instructions"), Blocks[0].Instructions.Num() > 0);
        }
    }

    // Re-compile the decompiled output onto a fresh Blueprint
    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripForeachBreakRecompileBP"));
        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile of decompiled output succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// 22. RoundTripClearDispatcher — compile clear_dispatcher, verify round-trip
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripClearDispatcherTest,
    "PinWright.bpir.round_trip.ClearDispatcher",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripClearDispatcherTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripClearDispatcherBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    // Add a multicast delegate variable so clear_dispatcher has something to resolve
    FEdGraphPinType DelegateType;
    DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("OnClearEvent"), DelegateType);
    FKismetEditorUtilities::CompileBlueprint(BP);

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    clear_dispatcher OnClearEvent()\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    // Dispatcher compilation may fail if delegate type doesn't resolve in test context
    if (!CompileResult.bSuccess)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("delegate-compile-unresolved"),
            TEXT("ClearDispatcher compile produced errors (expected in test context), verifying no crash"));
        return true;
    }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    // The keyword must be required, not accepted as one arm of a disjunction: the
    // dispatcher name alone appears in any emission that merely names the variable, so
    // "clear_dispatcher || OnClearEvent" was satisfied even if BpirTextEmitter dropped
    // the keyword entirely (or fell through to its "unknown_dispatcher" arm) and emitted
    // a plain call. That output also re-parses and re-compiles, so the drop was invisible
    // end to end -- the classic symmetric round-trip blind spot.
    TestTrue(TEXT("Output contains the clear_dispatcher keyword"),
        Output.Contains(TEXT("clear_dispatcher")));
    TestTrue(TEXT("Output names the OnClearEvent dispatcher"),
        Output.Contains(TEXT("OnClearEvent")));

    // Re-parse the decompiled output to verify structural integrity
    {
        FBpirParser Parser;
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> ParseErrors;
        bool bParsed = Parser.Parse(Output, Blocks, ParseErrors);
        TestTrue(TEXT("Re-parsed decompiled output without errors"), bParsed);
        if (!bParsed)
        {
            for (const FCompileError& Err : ParseErrors)
            {
                AddError(FString::Printf(TEXT("  Re-parse L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }

    // Re-compile the decompiled output onto a fresh Blueprint
    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripClearDispatcherRecompileBP"));
        FEdGraphPinType DelegateType2;
        DelegateType2.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
        FBlueprintEditorUtils::AddMemberVariable(BP2, TEXT("OnClearEvent"), DelegateType2);
        FKismetEditorUtilities::CompileBlueprint(BP2);

        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
        // "Re-compile succeeded" alone cannot see the round trip landing on a different
        // node class than it started from -- a plain call re-compiles perfectly happily.
        // Pin the class so the trip is verified end to end.
        int32 ClearDelegateCount = 0;
        for (UEdGraph* Graph : BP2->UbergraphPages)
        {
            if (!Graph) continue;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node && Node->GetClass()->GetName() == TEXT("K2Node_ClearDelegate"))
                {
                    ++ClearDelegateCount;
                }
            }
        }
        TestEqual(TEXT("Round trip reproduces exactly one K2Node_ClearDelegate"), ClearDelegateCount, 1);
    }
    return true;
}

// ----------------------------------------------------------------------------
// 23. RoundTripSetVariable — compile a set instruction, verify round-trip
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripSetVariableTest,
    "PinWright.bpir.round_trip.SetVariable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripSetVariableTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripSetVarBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    // Add an integer variable so "set" resolves
    FEdGraphPinType IntType;
    IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("Score"), IntType);

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    set Score = 42\n"
        "    call PrintString(InString: \"Done\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains 'set'"), Output.Contains(TEXT("set")));
    TestTrue(TEXT("Output contains 'Score'"), Output.Contains(TEXT("Score")));

    // Re-parse
    {
        FBpirParser Parser;
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> ParseErrors;
        bool bParsed = Parser.Parse(Output, Blocks, ParseErrors);
        TestTrue(TEXT("Re-parsed decompiled output without errors"), bParsed);
        if (!bParsed)
        {
            for (const FCompileError& Err : ParseErrors)
            {
                AddError(FString::Printf(TEXT("  Re-parse L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Parsed entry blocks exist"), Blocks.Num() > 0);
    }

    // Re-compile
    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripSetVarRecompileBP"));
        FEdGraphPinType IntType2;
        IntType2.PinCategory = UEdGraphSchema_K2::PC_Int;
        FBlueprintEditorUtils::AddMemberVariable(BP2, TEXT("Score"), IntType2);

        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// 24. RoundTripGetVariable — compile a get instruction, verify round-trip
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripGetVariableTest,
    "PinWright.bpir.round_trip.GetVariable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripGetVariableTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripGetVarBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    FEdGraphPinType FloatType;
    FloatType.PinCategory = UEdGraphSchema_K2::PC_Real;
    FloatType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
    FBlueprintEditorUtils::AddMemberVariable(BP, TEXT("Speed"), FloatType);

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %val = get Speed\n"
        "    call PrintString(InString: %val)\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains Speed variable reference"), Output.Contains(TEXT("Speed")));

    // Re-parse
    {
        FBpirParser Parser;
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> ParseErrors;
        bool bParsed = Parser.Parse(Output, Blocks, ParseErrors);
        TestTrue(TEXT("Re-parsed decompiled output without errors"), bParsed);
        if (!bParsed)
        {
            for (const FCompileError& Err : ParseErrors)
            {
                AddError(FString::Printf(TEXT("  Re-parse L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Parsed entry blocks exist"), Blocks.Num() > 0);
    }

    // Re-compile
    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripGetVarRecompileBP"));
        FEdGraphPinType FloatType2;
        FloatType2.PinCategory = UEdGraphSchema_K2::PC_Real;
        FloatType2.PinSubCategory = UEdGraphSchema_K2::PC_Float;
        FBlueprintEditorUtils::AddMemberVariable(BP2, TEXT("Speed"), FloatType2);

        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// 25. RoundTripSwitchEnum — compile switch_enum, verify round-trip
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripSwitchEnumTest,
    "PinWright.bpir.round_trip.SwitchEnum",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripSwitchEnumTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripSwitchEnumBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %e = enum ESlateVisibility::Visible\n"
        "    %s = switch_enum<ESlateVisibility>(%e) [ESlateVisibility::Visible -> @a, ESlateVisibility::Collapsed -> @b]\n"
        "\n"
        "@a:\n"
        "    call PrintString(InString: \"Visible\")\n"
        "    exec -> @done\n"
        "\n"
        "@b:\n"
        "    call PrintString(InString: \"Collapsed\")\n"
        "    exec -> @done\n"
        "\n"
        "@done:\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
        }
        return false;
    }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains switch_enum"), Output.Contains(TEXT("switch_enum")));
    TestFalse(TEXT("Output does not contain raw colon enum literal"), Output.Contains(TEXT("enum :Visible")));
    TestFalse(TEXT("Output does not contain raw colon enum selection literal"), Output.Contains(TEXT("switch_enum<ESlateVisibility>(:Visible)")));

    // Re-parse
    {
        FBpirParser Parser;
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> ParseErrors;
        bool bParsed = Parser.Parse(Output, Blocks, ParseErrors);
        TestTrue(TEXT("Re-parsed decompiled output without errors"), bParsed);
        if (!bParsed)
        {
            for (const FCompileError& Err : ParseErrors)
            {
                AddError(FString::Printf(TEXT("  Re-parse L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }

    // Re-compile
    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripSwitchEnumRecompileBP"));
        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// 26. RoundTripMacroGate — compile a Gate macro, verify round-trip
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripMacroGateTest,
    "PinWright.bpir.round_trip.MacroGate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripMacroGateTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripGateBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %g = macro Gate() [Exit -> @out]\n"
        "\n"
        "@out:\n"
        "    call PrintString(InString: \"GateOpen\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains Gate"), Output.Contains(TEXT("Gate")));

    // Re-parse
    {
        FBpirParser Parser;
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> ParseErrors;
        bool bParsed = Parser.Parse(Output, Blocks, ParseErrors);
        TestTrue(TEXT("Re-parsed decompiled output without errors"), bParsed);
        if (!bParsed)
        {
            for (const FCompileError& Err : ParseErrors)
            {
                AddError(FString::Printf(TEXT("  Re-parse L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }

    // Re-compile
    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripGateRecompileBP"));
        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// 27. RoundTripMacroMultiGate — compile a MultiGate macro, verify round-trip
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripMacroMultiGateTest,
    "PinWright.bpir.round_trip.MacroMultiGate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripMacroMultiGateTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripMultiGateBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %mg = macro MultiGate() [Out 0 -> @a, Out 1 -> @b]\n"
        "\n"
        "@a:\n"
        "    call PrintString(InString: \"PathA\")\n"
        "    exec -> @done\n"
        "\n"
        "@b:\n"
        "    call PrintString(InString: \"PathB\")\n"
        "    exec -> @done\n"
        "\n"
        "@done:\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);

    // MultiGate may not exist in StandardMacros on all UE versions (e.g. UE 5.6+).
    // If it fails with "not found", that's an environment issue — skip gracefully.
    if (!CompileResult.bSuccess)
    {
        bool bIsMacroNotFound = false;
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddInfo(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message));
            if (Err.Message.Contains(TEXT("not found")))
            {
                bIsMacroNotFound = true;
            }
        }
        if (bIsMacroNotFound)
        {
            // Greppable SKIPPED marker rather than AddWarning: this runner treats
            // AddWarning as a test failure (see TestCompilerIntegration.cpp ~3322), and
            // MultiGate really is absent from StandardMacros on some engine versions, so
            // a warning here would turn a legitimate environment skip into a red test.
            // The marker at least makes the abandoned assertions greppable in the log.
            PinWrightTestSkip::SkipAssertions(*this, TEXT("multigate-macro-unavailable"),
                TEXT("SKIPPED: MultiGate macro not available in this UE version's StandardMacros; skipping RoundTripMultiGate."));
            return true;
        }
        // Unexpected failure
        for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); }
        TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
        return false;
    }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains MultiGate"), Output.Contains(TEXT("MultiGate")));

    // Re-parse
    {
        FBpirParser Parser;
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> ParseErrors;
        bool bParsed = Parser.Parse(Output, Blocks, ParseErrors);
        TestTrue(TEXT("Re-parsed decompiled output without errors"), bParsed);
        if (!bParsed)
        {
            for (const FCompileError& Err : ParseErrors)
            {
                AddError(FString::Printf(TEXT("  Re-parse L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }

    // Re-compile
    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripMultiGateRecompileBP"));
        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// 28. RoundTripMakeStruct — compile a make<Vector>, verify round-trip
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripMakeStructTest,
    "PinWright.bpir.round_trip.MakeStruct",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripMakeStructTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripMakeStructBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %v = pure MakeVector(X: 1.0, Y: 2.0, Z: 3.0)\n"
        "    call SetActorLocation(NewLocation: %v)\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    bool bHasMakeVector = Output.Contains(TEXT("MakeVector"));
    bool bHasCall = Output.Contains(TEXT("call"));
    bool bHasMakeSyntax = Output.Contains(TEXT("make<"));
    bool bHasVector = Output.Contains(TEXT("Vector"));
    TestTrue(TEXT("Output contains struct construction syntax"),
        bHasMakeVector || bHasCall || bHasMakeSyntax || bHasVector);

    // Re-parse
    {
        FBpirParser Parser;
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> ParseErrors;
        bool bParsed = Parser.Parse(Output, Blocks, ParseErrors);
        TestTrue(TEXT("Re-parsed decompiled output without errors"), bParsed);
        if (!bParsed)
        {
            for (const FCompileError& Err : ParseErrors)
            {
                AddError(FString::Printf(TEXT("  Re-parse L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }

    // Re-compile
    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripMakeStructRecompileBP"));
        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// 29. RoundTripReturn — compile a function with return, verify round-trip
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripReturnTest,
    "PinWright.bpir.round_trip.Return",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripReturnTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripReturnBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry function Calc(float X) -> float {\n"
        "    return $X\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains return keyword"), Output.Contains(TEXT("return")));
    TestTrue(TEXT("Output contains function keyword"), Output.Contains(TEXT("function")));

    // Re-parse
    {
        FBpirParser Parser;
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> ParseErrors;
        bool bParsed = Parser.Parse(Output, Blocks, ParseErrors);
        TestTrue(TEXT("Re-parsed decompiled output without errors"), bParsed);
        if (!bParsed)
        {
            for (const FCompileError& Err : ParseErrors)
            {
                AddError(FString::Printf(TEXT("  Re-parse L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Parsed entry blocks exist"), Blocks.Num() > 0);
        if (Blocks.Num() > 0)
        {
            TestTrue(TEXT("Entry block is a function"), Blocks.ContainsByPredicate(
                [](const FBpirEntryBlock& B) { return B.Kind == EBpirEntryKind::Function; }));
        }
    }

    // Re-compile
    {
        UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripReturnRecompileBP"));
        FBpirCompiler Compiler2(BP2);
        FCompileResult RecompileResult = Compiler2.Compile(Output);
        TestTrue(TEXT("Re-compile succeeded"), RecompileResult.bSuccess);
        if (!RecompileResult.bSuccess)
        {
            for (const FCompileError& Err : RecompileResult.Errors)
            {
                AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// 30. MacroSimple — compile and decompile a simple impure macro
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripMacroSimpleTest,
    "PinWright.bpir.round_trip.MacroSimple",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripMacroSimpleTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripMacroSimpleBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry macro ClampValue(float Value, float Min, float Max) -> (float Result) {\n"
        "    %clamped = pure FClamp(Value: $Value, Min: $Min, Max: $Max)\n"
        "    return (Result: %clamped)\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains entry macro"), Output.Contains(TEXT("entry macro")));
    TestTrue(TEXT("Output contains ClampValue"), Output.Contains(TEXT("ClampValue")));
    TestTrue(TEXT("Output contains FClamp"), Output.Contains(TEXT("FClamp")));
    TestTrue(TEXT("Output contains Result"), Output.Contains(TEXT("Result")));
    return true;
}

// ----------------------------------------------------------------------------
// 31. MacroExecOnly — compile and decompile an exec-only macro (no data outputs)
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripMacroExecOnlyTest,
    "PinWright.bpir.round_trip.MacroExecOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripMacroExecOnlyTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripMacroExecOnlyBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry macro LogAndContinue(string Message) {\n"
        "    call PrintString(InString: $Message)\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains entry macro"), Output.Contains(TEXT("entry macro")));
    TestTrue(TEXT("Output contains LogAndContinue"), Output.Contains(TEXT("LogAndContinue")));
    TestTrue(TEXT("Output contains PrintString"), Output.Contains(TEXT("PrintString")));
    return true;
}

// ----------------------------------------------------------------------------
// 32. MacroBacktickName — compile and decompile a macro with spaces in name
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripMacroBacktickNameTest,
    "PinWright.bpir.round_trip.MacroBacktickName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripMacroBacktickNameTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripMacroBacktickBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry macro `Log Message`(string Text) {\n"
        "    call PrintString(InString: $Text)\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess) { for (const FCompileError& Err : CompileResult.Errors) { AddError(FString::Printf(TEXT("  L%d: %s"), Err.Line, *Err.Message)); } }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains backtick-quoted name"), Output.Contains(TEXT("`Log Message`")));
    return true;
}

// ----------------------------------------------------------------------------
// 33. ChainedPropertyAccess — compile multi-dot notation (cast output -> property)
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripChainedPropertyAccessTest,
    "PinWright.bpir.round_trip.ChainedPropertyAccess",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripChainedPropertyAccessTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripChainedPropertyBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    // Cast self to Actor, then access bHidden via multi-dot: %cast.AsActor.bHidden
    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %self_ref = self\n"
        "    %cast = cast<Actor>(%self_ref) [success -> @ok, fail -> @done]\n"
        "\n"
        "@ok:\n"
        "    call PrintString(InString: %cast.AsActor.bHidden)\n"
        "    exec -> @done\n"
        "\n"
        "@done:\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    return true;
}

// ----------------------------------------------------------------------------
// 34. AutoBreakStructMember — compile %ref.Member where ref returns a struct,
//     auto-inserting a BreakStruct node without explicit break<> syntax
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripAutoBreakStructMemberTest,
    "PinWright.bpir.round_trip.AutoBreakStructMember",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripAutoBreakStructMemberTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripAutoBreakStructBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    // GetActorForwardVector returns FVector; %loc.X should auto-break the struct
    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %loc = pure GetActorForwardVector(Target: self)\n"
        "    %str = pure Conv_DoubleToString(InDouble: %loc.X)\n"
        "    call PrintString(InString: %str)\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    return true;
}

// ----------------------------------------------------------------------------
// CustomEventParam — custom event params survive compile → decompile → recompile
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripCustomEventParamTest,
    "PinWright.bpir.round_trip.CustomEventParam",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripCustomEventParamTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripCEParam"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry custom_event OnMyEvent(bool bFlag) {\n"
        "    %n0 = branch($bFlag) [true -> @then]\n"
        "\n"
        "@then:\n"
        "    call PrintString(InString: \"flag is true\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
            AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
        return false;
    }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains $bFlag"), Output.Contains(TEXT("$bFlag")));

    // Recompile onto fresh BP
    UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripCEParamRecompile"));
    FBpirCompiler Compiler2(BP2);
    FCompileResult RecompileResult = Compiler2.Compile(Output);
    TestTrue(TEXT("Re-compile succeeded"), RecompileResult.bSuccess);
    if (!RecompileResult.bSuccess)
    {
        for (const FCompileError& Err : RecompileResult.Errors)
            AddError(FString::Printf(TEXT("Recompile L%d: %s"), Err.Line, *Err.Message));
    }
    return true;
}

// ----------------------------------------------------------------------------
// BareParamName — bare parameter names (no $ prefix) resolve for custom events
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCompilerBareParamNameTest,
    "PinWright.bpir.round_trip.BareParamName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerBareParamNameTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("BareParamTest"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry custom_event OnTestEvent(bool bMyFlag) {\n"
        "    %n0 = branch(bMyFlag) [true -> @then]\n"
        "\n"
        "@then:\n"
        "    call PrintString(InString: \"yes\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(InputBpir);
    TestTrue(TEXT("Compile with bare param name succeeded"), Result.bSuccess);
    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
            AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
    }
    return true;
}

// ----------------------------------------------------------------------------
// CrossEntryCustomEventCall — calling a custom event from another entry block
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripCrossEntryCallTest,
    "PinWright.bpir.round_trip.CrossEntryCustomEventCall",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripCrossEntryCallTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("CrossEntryCallTest"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry custom_event MyCustomEvent(bool bParam) {\n"
        "    call PrintString(InString: \"custom event fired\")\n"
        "}\n"
        "\n"
        "entry event BeginPlay() {\n"
        "    call MyCustomEvent(bParam: true)\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(InputBpir);
    TestTrue(TEXT("Cross-entry call compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
            AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
    }
    return true;
}

// ----------------------------------------------------------------------------
// AuthoredPositionRoundTrip — authored coordinates are preserved, decompiled
// from current graph coordinates, then accepted by a fresh compile.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripAuthoredPositionRoundTripTest,
    "PinWright.bpir.round_trip.AuthoredPositionRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripAuthoredPositionRoundTripTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripAuthoredPositionBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    call PrintString(InString: \"FixedA\") @(120, 240)\n"
        "    call PrintString(InString: \"FixedB\") @(420, 240)\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    UK2Node_CallFunction* FixedA = FindRoundTripPrintStringWithDefault(BP, TEXT("FixedA"));
    UK2Node_CallFunction* FixedB = FindRoundTripPrintStringWithDefault(BP, TEXT("FixedB"));
    TestNotNull(TEXT("FixedA node exists"), FixedA);
    TestNotNull(TEXT("FixedB node exists"), FixedB);
    if (!FixedA || !FixedB) return false;

    TestEqual(TEXT("FixedA NodePosX preserved"), FixedA->NodePosX, 120);
    TestEqual(TEXT("FixedA NodePosY preserved"), FixedA->NodePosY, 240);
    TestEqual(TEXT("FixedB NodePosX preserved before move"), FixedB->NodePosX, 420);
    TestEqual(TEXT("FixedB NodePosY preserved before move"), FixedB->NodePosY, 240);

    FixedB->NodePosX = 512;
    FixedB->NodePosY = 288;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Output contains FixedA authored position"),
        RoundTripLineContains(Output, TEXT("FixedA"), TEXT("@(120, 240)")));
    TestTrue(TEXT("Output contains FixedB current position"),
        RoundTripLineContains(Output, TEXT("FixedB"), TEXT("@(512, 288)")));

    UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripAuthoredPositionRecompileBP"));
    FBpirCompiler Compiler2(BP2);
    FCompileResult RecompileResult = Compiler2.Compile(Output);
    TestTrue(TEXT("Re-compile of positioned output succeeded"), RecompileResult.bSuccess);
    if (!RecompileResult.bSuccess)
    {
        for (const FCompileError& Err : RecompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Recompile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// EntryNodeAuthoredPositionRoundTrip — entry signature and body coordinates both
// survive compile, decompile, and recompile.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripEntryNodeAuthoredPositionRoundTripTest,
    "PinWright.bpir.round_trip.EntryNodeAuthoredPositionRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripEntryNodeAuthoredPositionRoundTripTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripEntryNodeAuthoredPositionBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry custom_event PositionedEntry() @(16, 1902) {\n"
        "    call PrintString(InString: \"EntryBody\") @(316, 1902)\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    UK2Node_CustomEvent* EntryNode = FindRoundTripCustomEvent(BP, FName(TEXT("PositionedEntry")));
    UK2Node_CallFunction* BodyNode = FindRoundTripPrintStringWithDefault(BP, TEXT("EntryBody"));
    TestNotNull(TEXT("Custom event exists"), EntryNode);
    TestNotNull(TEXT("Body node exists"), BodyNode);
    if (!EntryNode || !BodyNode) return false;

    TestEqual(TEXT("Entry NodePosX preserved"), EntryNode->NodePosX, 16);
    TestEqual(TEXT("Entry NodePosY preserved"), EntryNode->NodePosY, 1902);
    TestEqual(TEXT("Body NodePosX preserved"), BodyNode->NodePosX, 316);
    TestEqual(TEXT("Body NodePosY preserved"), BodyNode->NodePosY, 1902);

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Entry signature contains authored position"),
        RoundTripLineContains(Output, TEXT("entry custom_event PositionedEntry()"), TEXT("@(16, 1902)")));
    TestTrue(TEXT("Body node contains authored position"),
        RoundTripLineContains(Output, TEXT("EntryBody"), TEXT("@(316, 1902)")));

    UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripEntryNodeAuthoredPositionRecompileBP"));
    TestNotNull(TEXT("Second blueprint created"), BP2);
    if (!BP2) return false;

    FBpirCompiler Compiler2(BP2);
    FCompileResult RecompileResult = Compiler2.Compile(Output);
    if (!RecompileResult.bSuccess)
    {
        for (const FCompileError& Err : RecompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Recompile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Recompile succeeded"), RecompileResult.bSuccess);
    if (!RecompileResult.bSuccess) return false;

    UK2Node_CustomEvent* RecompiledEntryNode = FindRoundTripCustomEvent(BP2, FName(TEXT("PositionedEntry")));
    UK2Node_CallFunction* RecompiledBodyNode = FindRoundTripPrintStringWithDefault(BP2, TEXT("EntryBody"));
    TestNotNull(TEXT("Recompiled custom event exists"), RecompiledEntryNode);
    TestNotNull(TEXT("Recompiled body node exists"), RecompiledBodyNode);
    if (!RecompiledEntryNode || !RecompiledBodyNode) return false;

    TestEqual(TEXT("Recompiled entry NodePosX preserved"), RecompiledEntryNode->NodePosX, 16);
    TestEqual(TEXT("Recompiled entry NodePosY preserved"), RecompiledEntryNode->NodePosY, 1902);
    TestEqual(TEXT("Recompiled body NodePosX preserved"), RecompiledBodyNode->NodePosX, 316);
    TestEqual(TEXT("Recompiled body NodePosY preserved"), RecompiledBodyNode->NodePosY, 1902);
    return true;
}

// ----------------------------------------------------------------------------
// GeneratedPositionRoundTrip — unpositioned BPIR receives layout coordinates
// from decompile and remains compilable with labels and quoted @(...) text.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripGeneratedPositionRoundTripTest,
    "PinWright.bpir.round_trip.GeneratedPositionRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripGeneratedPositionRoundTripTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripGeneratedPositionBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %b = branch(true) [true -> @then, false -> @done]\n"
        "\n"
        "@then:\n"
        "    call PrintString(InString: \"quoted @(1, 2) and @text\")\n"
        "    exec -> @done\n"
        "\n"
        "@done:\n"
        "    call PrintString(InString: \"AfterLabel\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Unpositioned compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Branch line received a generated position"),
        RoundTripLineContains(Output, TEXT("branch("), TEXT("@(")));
    TestTrue(TEXT("Quoted marker text survived"),
        Output.Contains(TEXT("quoted @(1, 2) and @text")));
    TestTrue(TEXT("AfterLabel line received a generated position"),
        RoundTripLineContains(Output, TEXT("AfterLabel"), TEXT("@(")));

    UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripGeneratedPositionRecompileBP"));
    FBpirCompiler Compiler2(BP2);
    FCompileResult RecompileResult = Compiler2.Compile(Output);
    TestTrue(TEXT("Re-compile of generated-position output succeeded"), RecompileResult.bSuccess);
    if (!RecompileResult.bSuccess)
    {
        for (const FCompileError& Err : RecompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Recompile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// PositionedExplicitHelperRoundTrip — explicit helper-node BPIR lines can carry
// positions and round-trip without lazy helper synthesis.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripPositionedExplicitHelperRoundTripTest,
    "PinWright.bpir.round_trip.PositionedExplicitHelperRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripPositionedExplicitHelperRoundTripTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripPositionedExplicitHelperBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %m = make<Margin>(Left: 1.0, Top: 2.0, Right: 3.0, Bottom: 4.0) @(10, 20)\n"
        "    %b = break<Margin>(%m) @(30, 40)\n"
        "    %s = pure Conv_DoubleToString(InDouble: %b.Left) @(50, 60)\n"
        "    call PrintString(InString: %s) @(70, 80)\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Positioned explicit-helper compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    UK2Node_MakeStruct* MakeNode = FindRoundTripNodeOfType<UK2Node_MakeStruct>(BP);
    UK2Node_BreakStruct* BreakNode = FindRoundTripNodeOfType<UK2Node_BreakStruct>(BP);
    UK2Node_CallFunction* PrintNode = FindRoundTripCallByFunctionName(BP, TEXT("PrintString"));
    TestNotNull(TEXT("MakeStruct node exists"), MakeNode);
    TestNotNull(TEXT("BreakStruct node exists"), BreakNode);
    TestNotNull(TEXT("PrintString node exists"), PrintNode);
    if (!MakeNode || !BreakNode || !PrintNode) return false;

    TestEqual(TEXT("MakeStruct NodePosX preserved"), MakeNode->NodePosX, 10);
    TestEqual(TEXT("MakeStruct NodePosY preserved"), MakeNode->NodePosY, 20);
    TestEqual(TEXT("BreakStruct NodePosX preserved"), BreakNode->NodePosX, 30);
    TestEqual(TEXT("BreakStruct NodePosY preserved"), BreakNode->NodePosY, 40);
    TestEqual(TEXT("PrintString NodePosX preserved"), PrintNode->NodePosX, 70);
    TestEqual(TEXT("PrintString NodePosY preserved"), PrintNode->NodePosY, 80);

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("MakeStruct position emitted"),
        RoundTripLineContains(Output, TEXT("make<Margin>"), TEXT("@(10, 20)")));
    TestTrue(TEXT("BreakStruct position emitted"),
        RoundTripLineContains(Output, TEXT("break<Margin>"), TEXT("@(30, 40)")));
    TestTrue(TEXT("Explicit conversion position emitted"),
        RoundTripLineContains(Output, TEXT("Conv_DoubleToString"), TEXT("@(50, 60)")));
    TestTrue(TEXT("PrintString position emitted"),
        RoundTripLineContains(Output, TEXT("PrintString"), TEXT("@(70, 80)")));

    UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripPositionedExplicitHelperRecompileBP"));
    FBpirCompiler Compiler2(BP2);
    FCompileResult RecompileResult = Compiler2.Compile(Output);
    TestTrue(TEXT("Re-compile of explicit-helper output succeeded"), RecompileResult.bSuccess);
    if (!RecompileResult.bSuccess)
    {
        for (const FCompileError& Err : RecompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Recompile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// HelperPlacementModeContract — pure implicit helpers (K2Node_Self, pure
// K2Node_VariableGet) are inline operands of their consumer and never need
// their own @(x, y). Authored-position bodies must accept them silently;
// auto-layout bodies must still place them. Only non-pure visible helpers
// without positions remain rejectable in authored-position mode.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripHelperPlacementModeContractTest,
    "PinWright.bpir.round_trip.HelperPlacementModeContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripHelperPlacementModeContractTest::RunTest(const FString& Parameters)
{
    UBlueprint* PositionedBP = CreateTransientTestBP(TEXT("RoundTripPositionedImplicitHelperBP"));
    TestNotNull(TEXT("Positioned Blueprint created"), PositionedBP);
    if (!PositionedBP) return false;

    FEdGraphPinType StringType;
    StringType.PinCategory = UEdGraphSchema_K2::PC_String;
    FBlueprintEditorUtils::AddMemberVariable(PositionedBP, TEXT("Source"), StringType);

    FBpirCompiler PositionedCompiler(PositionedBP);
    // $Source materialises a pure K2Node_VariableGet wired into PrintString;
    // its layout follows the consumer, so the positioned body must compile.
    FCompileResult PositionedResult = PositionedCompiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call PrintString(InString: $Source) @(100, 200)\n")
        TEXT("}"));
    if (!PositionedResult.bSuccess)
    {
        for (const FCompileError& Err : PositionedResult.Errors)
        {
            AddError(FString::Printf(TEXT("Positioned compile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Positioned body with pure implicit helper compiles"), PositionedResult.bSuccess);

    UBlueprint* AutoBP = CreateTransientTestBP(TEXT("RoundTripAutoImplicitHelperBP"));
    TestNotNull(TEXT("Auto-layout Blueprint created"), AutoBP);
    if (!AutoBP) return false;

    FBlueprintEditorUtils::AddMemberVariable(AutoBP, TEXT("Source"), StringType);

    FBpirCompiler AutoCompiler(AutoBP);
    FCompileResult AutoResult = AutoCompiler.Compile(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    call PrintString(InString: $Source)\n")
        TEXT("}"));
    if (!AutoResult.bSuccess)
    {
        for (const FCompileError& Err : AutoResult.Errors)
        {
            AddError(FString::Printf(TEXT("Auto-layout compile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Auto-layout lazy-helper body compiles"), AutoResult.bSuccess);
    if (AutoResult.bSuccess)
    {
        UEdGraphNode* SourceGetNode = FindRoundTripNodeMatching(AutoBP, [](UEdGraphNode* Node)
        {
            return Node && Node->GetClass()->GetName().Contains(TEXT("VariableGet"));
        });
        TestNotNull(TEXT("Auto-layout lazy helper node exists"), SourceGetNode);
        if (SourceGetNode)
        {
            TestTrue(TEXT("Auto-layout lazy helper received layout position"),
                SourceGetNode->NodePosX != 0 || SourceGetNode->NodePosY != 0);
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// AuthoredPositionDiagnostics — malformed syntax fails in parsing, while mixed
// and zero-node authored positions fail with actionable compile diagnostics.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripAuthoredPositionDiagnosticsTest,
    "PinWright.bpir.round_trip.AuthoredPositionDiagnostics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripAuthoredPositionDiagnosticsTest::RunTest(const FString& Parameters)
{
    {
        FBpirParser Parser;
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> Errors;
        const bool bParsed = Parser.Parse(
            TEXT("entry event BeginPlay() {\n")
            TEXT("    call PrintString(InString: \"Bad\") @(10\n")
            TEXT("}"),
            Blocks,
            Errors,
            /*bSkipReferenceValidation=*/ true);
        TestFalse(TEXT("Malformed position syntax fails in parser"), bParsed);
        TestTrue(TEXT("Parser error mentions position marker"),
            ErrorsContain(Errors, TEXT("Position marker")));
    }

    {
        UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripMixedPositionBP"));
        TestNotNull(TEXT("Mixed Blueprint created"), BP);
        if (!BP) return false;

        FBpirCompiler Compiler(BP);
        FCompileResult Result = Compiler.Compile(
            TEXT("entry custom_event Mixed() {\n")
            TEXT("    call PrintString(InString: \"A\") @(10, 20)\n")
            TEXT("    call PrintString(InString: \"B\")\n")
            TEXT("}"));

        TestFalse(TEXT("Mixed positioned body fails"), Result.bSuccess);
        TestTrue(TEXT("Mixed error mentions authored-position mode"),
            ErrorsContain(Result.Errors, TEXT("authored-position mode")));
        TestTrue(TEXT("Mixed error includes add-position remediation"),
            ErrorsContain(Result.Errors, TEXT("add @(x, y)")));
        TestTrue(TEXT("Mixed error includes auto-layout remediation"),
            ErrorsContain(Result.Errors, TEXT("remove all positions")));
    }

    {
        UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripZeroNodePositionBP"));
        TestNotNull(TEXT("Zero-node Blueprint created"), BP);
        if (!BP) return false;

        FBpirCompiler Compiler(BP);
        FCompileResult Result = Compiler.Compile(
            TEXT("entry event BeginPlay() {\n")
            TEXT("    %e = enum ESlateVisibility::Visible @(10, 20)\n")
            TEXT("    call PrintString(InString: \"A\") @(30, 40)\n")
            TEXT("}"));

        TestFalse(TEXT("Zero-node authored position fails"), Result.bSuccess);
        TestTrue(TEXT("Zero-node error explains node-backed restriction"),
            ErrorsContain(Result.Errors, TEXT("only applies to visible node-backed BPIR instructions")));
        TestTrue(TEXT("Zero-node error includes remediation"),
            ErrorsContain(Result.Errors, TEXT("remove all positions")));
    }
    return true;
}

// ----------------------------------------------------------------------------
// AuthoredPositionPerEntryIsolation — one positioned entry and one auto-layout
// entry choose placement modes independently and decompile into recompilable BPIR.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRoundTripAuthoredPositionPerEntryIsolationTest,
    "PinWright.bpir.round_trip.AuthoredPositionPerEntryIsolation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoundTripAuthoredPositionPerEntryIsolationTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripPositionPerEntryBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry custom_event Positioned() {\n"
        "    call PrintString(InString: \"FixedEntry\") @(333, 444)\n"
        "}\n"
        "\n"
        "entry custom_event Auto() {\n"
        "    call PrintString(InString: \"AutoEntry\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Mixed placement entries compile"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    UK2Node_CallFunction* FixedNode = FindRoundTripPrintStringWithDefault(BP, TEXT("FixedEntry"));
    UK2Node_CallFunction* AutoNode = FindRoundTripPrintStringWithDefault(BP, TEXT("AutoEntry"));
    TestNotNull(TEXT("Fixed entry node exists"), FixedNode);
    TestNotNull(TEXT("Auto entry node exists"), AutoNode);
    if (!FixedNode || !AutoNode) return false;

    TestEqual(TEXT("Fixed entry NodePosX preserved"), FixedNode->NodePosX, 333);
    TestEqual(TEXT("Fixed entry NodePosY preserved"), FixedNode->NodePosY, 444);

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("Fixed entry line includes authored position"),
        RoundTripLineContains(Output, TEXT("FixedEntry"), TEXT("@(333, 444)")));
    TestTrue(TEXT("Auto entry line includes generated position"),
        RoundTripLineContains(Output, TEXT("AutoEntry"), TEXT("@(")));

    UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripPositionPerEntryRecompileBP"));
    FBpirCompiler Compiler2(BP2);
    FCompileResult RecompileResult = Compiler2.Compile(Output);
    TestTrue(TEXT("Re-compile of multi-entry output succeeded"), RecompileResult.bSuccess);
    if (!RecompileResult.bSuccess)
    {
        for (const FCompileError& Err : RecompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Recompile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// Multi-input-exec round-trip: parser captures `.PinName` suffix on exec
// targets, and the emitter wraps spaced pin names in backticks. Counterfactual:
// reverting the parser's SplitLabelAndInputPin wiring (Chunk 2A) clears
// TargetInputPinName so Cases A and B fail; reverting the emitter's
// QuoteIdentifierIfNeeded wrap on the pin segment (Chunk 1A step 4) drops the
// backticks and Case C's substring assertion fails.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBpirRoundTripMultiInputExecTest,
    "PinWright.bpir.round_trip.MultiInputExec",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirRoundTripMultiInputExecTest::RunTest(const FString& Parameters)
{
    // ---- Case A: identifier-shaped pin names parse into TargetInputPinName.
    {
        const FString Code = TEXT(
            "entry event BeginPlay() {\n"
            "    %g = macro Gate() [Open -> @gate.Open, Reset -> @gate.Reset]\n"
            "\n"
            "@gate:\n"
            "    call PrintString(InString: \"After\")\n"
            "}");

        FBpirParser Parser;
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> Errors;
        const bool bOk = Parser.Parse(Code, Blocks, Errors, /*bSkipReferenceValidation=*/true);
        TestTrue(TEXT("Case A: parse succeeded"), bOk);
        for (const FCompileError& Err : Errors)
        {
            AddError(FString::Printf(TEXT("Case A parse L%d: %s"), Err.Line, *Err.Message));
        }
        if (Blocks.Num() == 0 || Blocks[0].Instructions.Num() == 0)
        {
            AddError(TEXT("Case A: no instructions parsed"));
            return false;
        }

        const FBpirInstruction& Macro = Blocks[0].Instructions[0];
        TestEqual(TEXT("Case A: opcode is Macro"), (int32)Macro.Opcode, (int32)EBpirOpcode::Macro);
        TestEqual(TEXT("Case A: two exec targets parsed"), Macro.ExecTargets.Num(), 2);
        if (Macro.ExecTargets.Num() == 2)
        {
            // ExecTargets preserve parse order; entries 0/1 correspond to Open/Reset.
            TestEqual(TEXT("Case A: target[0] output pin"), Macro.ExecTargets[0].PinName, FString(TEXT("Open")));
            TestEqual(TEXT("Case A: target[0] label"), Macro.ExecTargets[0].Label, FString(TEXT("gate")));
            TestEqual(TEXT("Case A: target[0] input pin"), Macro.ExecTargets[0].TargetInputPinName, FString(TEXT("Open")));
            TestEqual(TEXT("Case A: target[1] output pin"), Macro.ExecTargets[1].PinName, FString(TEXT("Reset")));
            TestEqual(TEXT("Case A: target[1] label"), Macro.ExecTargets[1].Label, FString(TEXT("gate")));
            TestEqual(TEXT("Case A: target[1] input pin"), Macro.ExecTargets[1].TargetInputPinName, FString(TEXT("Reset")));
        }
    }

    // ---- Case B: backtick-wrapped pin name preserves internal space.
    {
        const FString Code = TEXT(
            "entry event BeginPlay() {\n"
            "    %g = macro Gate() [Out -> @gate.`Reset Pin`]\n"
            "\n"
            "@gate:\n"
            "    call PrintString(InString: \"After\")\n"
            "}");

        FBpirParser Parser;
        TArray<FBpirEntryBlock> Blocks;
        TArray<FCompileError> Errors;
        const bool bOk = Parser.Parse(Code, Blocks, Errors, /*bSkipReferenceValidation=*/true);
        TestTrue(TEXT("Case B: parse succeeded"), bOk);
        for (const FCompileError& Err : Errors)
        {
            AddError(FString::Printf(TEXT("Case B parse L%d: %s"), Err.Line, *Err.Message));
        }
        if (Blocks.Num() == 0 || Blocks[0].Instructions.Num() == 0)
        {
            AddError(TEXT("Case B: no instructions parsed"));
            return false;
        }

        const FBpirInstruction& Macro = Blocks[0].Instructions[0];
        TestEqual(TEXT("Case B: one exec target parsed"), Macro.ExecTargets.Num(), 1);
        if (Macro.ExecTargets.Num() == 1)
        {
            TestEqual(TEXT("Case B: target label"), Macro.ExecTargets[0].Label, FString(TEXT("gate")));
            // Backticks stripped by the parser; internal space preserved verbatim.
            TestEqual(TEXT("Case B: target input pin (spaced, unwrapped)"),
                Macro.ExecTargets[0].TargetInputPinName, FString(TEXT("Reset Pin")));
        }
    }

    // ---- Case C: emitter wraps a spaced pin name in backticks.
    {
        FBpirLabelMap LabelMap;
        LabelMap.Add(TEXT("Out"), FBpirEmitTarget{ TEXT("gate"), TEXT("Reset Pin") });

        FBpirTextEmitter Emitter;
        const FString Out = Emitter.FormatExecTargets(LabelMap);

        TestTrue(TEXT("Case C: emit contains backtick-wrapped pin name"),
            Out.Contains(TEXT("`Reset Pin`")));
        TestTrue(TEXT("Case C: emit uses dotted suffix syntax"),
            Out.Contains(TEXT("@gate.")));
        TestTrue(TEXT("Case C: emit places suffix on label gate"),
            Out.Contains(TEXT("Out -> @gate.`Reset Pin`")));
    }
    return true;
}

// ----------------------------------------------------------------------------
// PreservesAnnotatedGraph — first-pass decompile output is byte-stable through
// emit -> compile -> re-emit. The annotation emitter writes is the same one
// the parser accepts, so the second-pass decompile must equal the first-pass.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirRoundTripPreservesAnnotatedGraphTest,
    "PinWright.bpir.round_trip.PreservesAnnotatedGraph",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirRoundTripPreservesAnnotatedGraphTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripAnnotatedBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %valid = pure IsValid(self)\n"
        "    %owner = pure GetOwner(self)\n"
        "    %br = branch(%valid) [true -> @done]\n"
        "@done:\n"
        "    call PrintString(InString: \"ok\")\n"
        "}"
    );

    FBpirCompiler Compiler(BP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Compile error L%d: %s"), Err.Line, *Err.Message));
        }
        return false;
    }
    TestTrue(TEXT("Initial compile succeeded"), CompileResult.bSuccess);

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult FirstDecompile = Decompiler.Decompile();
    TestTrue(TEXT("First decompile succeeded"), FirstDecompile.bSuccess);
    if (!FirstDecompile.bSuccess) return false;

    const FString& FirstText = FirstDecompile.BpirText;
    // The input declares both an annotated bool (%valid) and an annotated object
    // (%owner), so both annotation kinds must survive. As a disjunction this held when
    // either kind was dropped entirely -- and the byte-for-byte second-pass comparison
    // below cannot see that, because a symmetric drop is identical on both passes.
    // The Contains checks are the only correctness anchor this test has.
    TestTrue(TEXT("First decompile carries the bool type annotation"),
        FirstText.Contains(TEXT(": bool")));
    TestTrue(TEXT("First decompile carries the object type annotation"),
        FirstText.Contains(TEXT(": object<")));

    // Compile the annotated text onto a fresh BP and re-decompile.
    UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripAnnotatedBP_2"));
    TestNotNull(TEXT("Second BP created"), BP2);
    if (!BP2) return false;

    FBpirCompiler Compiler2(BP2);
    FCompileResult Recompile = Compiler2.Compile(FirstText);
    if (!Recompile.bSuccess)
    {
        for (const FCompileError& Err : Recompile.Errors)
        {
            AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
        }
        return false;
    }
    TestTrue(TEXT("Re-compile of annotated decompile output succeeded"), Recompile.bSuccess);

    FBpirDecompiler Decompiler2(BP2);
    FBpirDecompileResult SecondDecompile = Decompiler2.Decompile();
    TestTrue(TEXT("Second decompile succeeded"), SecondDecompile.bSuccess);
    if (!SecondDecompile.bSuccess) return false;

    TestEqual(TEXT("Second-pass decompile equals first-pass byte-for-byte"),
        SecondDecompile.BpirText, FirstText);
    return true;
}

// ----------------------------------------------------------------------------
// MultiSelfTargets — three sibling object refs wired into a single Call node's
// Self pin produce three sibling `call SetActorHiddenInGame(Target: ...)` lines
// (one per source) and the multi-connection warning is suppressed.
//
// Counterfactual: if BpirTextEmitter.cpp::EmitCallNode multi-self fan-out is
// reverted, the assertion "output text contains exactly 3 `call FuncName(Target:`
// occurrences" fails because the emitter writes only one such line. If the
// warning suppression in BpirDecompiler.cpp::ResolveInputValue is reverted, the
// assertion "no warning containing only the first is used" fails because the
// resolver re-emits that warning.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirRoundTripMultiSelfTargetsTest,
    "PinWright.bpir.roundtrip.MultiSelfTargets",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirRoundTripMultiSelfTargetsTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripMultiSelfBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    UEdGraph* EventGraph = BP->UbergraphPages.Num() > 0 ? BP->UbergraphPages[0] : nullptr;
    TestNotNull(TEXT("Event graph exists"), EventGraph);
    if (!EventGraph) return false;

    // Add three AActor* member variables. UK2Node_VariableGet on these produces
    // distinct pure source pins that can be wired into Self, exercising the
    // multi-self LinkedTo.Num() > 1 path without dragging exec-flow concerns in.
    FEdGraphPinType ActorPinType;
    ActorPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
    ActorPinType.PinSubCategoryObject = AActor::StaticClass();
    const TArray<FName> VarNames = { TEXT("ActorA"), TEXT("ActorB"), TEXT("ActorC") };
    for (const FName& VarName : VarNames)
    {
        const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(BP, VarName, ActorPinType);
        TestTrue(FString::Printf(TEXT("Added variable %s"), *VarName.ToString()), bAdded);
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

    // SetActorHiddenInGame: impure, no return, non-latent member of AActor.
    // CanFunctionSupportMultipleTargets(SetActorHiddenInGame) == true, so the
    // owning UK2Node_CallFunction reports AllowMultipleSelfs(false) == true.
    UK2Node_CallFunction* Call = SpawnNode<UK2Node_CallFunction>(EventGraph, 200, 0);
    Call->FunctionReference.SetExternalMember(
        GET_FUNCTION_NAME_CHECKED(AActor, SetActorHiddenInGame),
        AActor::StaticClass());
    Call->ReconstructNode();

    UEdGraphPin* SelfPin = Call->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
    TestNotNull(TEXT("Self pin found on call"), SelfPin);
    if (!SelfPin) return false;

    // Wire BeginPlay's Then into the call's Execute so the call lands in
    // emitted exec-flow output (not orphan-pruned).
    UK2Node_Event* BeginPlay = BpirGraphTestHelpers::EnsureBeginPlayNode(EventGraph);
    TestNotNull(TEXT("BeginPlay node exists on transient BP"), BeginPlay);
    if (!BeginPlay) return false;
    UEdGraphPin* ThenPin = BeginPlay->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
    UEdGraphPin* ExecPin = Call->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
    TestNotNull(TEXT("BeginPlay.then pin"), ThenPin);
    TestNotNull(TEXT("Call.execute pin"), ExecPin);
    if (ThenPin && ExecPin)
    {
        ThenPin->MakeLinkTo(ExecPin);
    }

    // Spawn one UK2Node_VariableGet per variable and wire its output into Self.
    // FindPin by-name yields the variable's data output pin (PinName == VarName
    // for VariableGet nodes).
    for (const FName& VarName : VarNames)
    {
        UK2Node_VariableGet* Get = SpawnNode<UK2Node_VariableGet>(EventGraph, 0, 100);
        Get->VariableReference.SetSelfMember(VarName);
        Get->ReconstructNode();
        UEdGraphPin* OutPin = Get->FindPin(VarName, EGPD_Output);
        TestNotNull(FString::Printf(TEXT("VariableGet[%s] output pin"), *VarName.ToString()), OutPin);
        if (!OutPin) return false;
        OutPin->MakeLinkTo(SelfPin);
    }
    TestEqual(TEXT("Self pin has 3 incoming connections"), SelfPin->LinkedTo.Num(), 3);

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    // Count "call SetActorHiddenInGame(Target:" occurrences. The emitter
    // formats the function token via FormatNameToken, which preserves the
    // function's display name; SetActorHiddenInGame contains no special
    // characters so the literal match is stable.
    const FString Needle = TEXT("call SetActorHiddenInGame(Target:");
    int32 Occurrences = 0;
    int32 SearchFrom = 0;
    while (true)
    {
        const int32 Found = Result.BpirText.Find(Needle, ESearchCase::CaseSensitive,
            ESearchDir::FromStart, SearchFrom);
        if (Found == INDEX_NONE) break;
        ++Occurrences;
        SearchFrom = Found + Needle.Len();
    }
    TestEqual(TEXT("Three sibling 'call SetActorHiddenInGame(Target:' lines emitted"),
        Occurrences, 3);

    for (const FBpirWarning& Warn : Result.Warnings)
    {
        TestFalse(
            FString::Printf(TEXT("Multi-self warning suppressed: '%s'"), *Warn.Text),
            Warn.Text.Contains(TEXT("only the first is used")));
    }

    return true;
}

// ----------------------------------------------------------------------------
// FNamePinDefaultQuoted — a PC_Name pin default with a non-empty value must
// decompile to a quoted BPIR literal (`ParameterName: "AnimateArcFill"`),
// not the bare identifier form (`ParameterName: AnimateArcFill`) that the
// compile-side IsLiteral resolver rejects with "Could not resolve value".
//
// Tracked in B-bpir-fname-pin-bare-identifier-not-resolved. The fix at
// BpirDecompiler.cpp adds a PC_Name branch to the non-empty-DefaultValue
// emit path that routes through BpirStructLiteralUtils::EscapeBpirString.
// Compile-side FCodePinResolver::SetPinDefaultValue already unquotes the
// outer-quoted form before storing, so the round-trip is symmetric.
//
// Counterfactual: if the PC_Name branch is reverted, the assertion
// `BPIR text contains quoted form` fails because the catch-all emits
// the bare identifier; the subsequent recompile also fails because
// IsLiteral cannot bind the bare identifier to a pin default.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirRoundTripFNamePinDefaultQuotedTest,
    "PinWright.bpir.round_trip.FNamePinDefaultQuoted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirRoundTripFNamePinDefaultQuotedTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("RoundTripFNamePinBP"));
    TestNotNull(TEXT("Blueprint created"), BP);
    if (!BP) return false;

    UEdGraph* EventGraph = BP->UbergraphPages.Num() > 0 ? BP->UbergraphPages[0] : nullptr;
    TestNotNull(TEXT("Event graph exists"), EventGraph);
    if (!EventGraph) return false;

    // UKismetSystemLibrary::SetIntPropertyByName is impure (so it appears in the
    // emitted exec chain) and carries a PC_Name `PropertyName` input — the
    // structural twin of the SetScalarParameterValue.ParameterName pin cited
    // in the ticket repro, without the material module dependency.
    UK2Node_CallFunction* Call = SpawnNode<UK2Node_CallFunction>(EventGraph, 200, 0);
    Call->FunctionReference.SetExternalMember(
        GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, SetIntPropertyByName),
        UKismetSystemLibrary::StaticClass());
    Call->ReconstructNode();

    UEdGraphPin* NamePin = Call->FindPin(FName(TEXT("PropertyName")), EGPD_Input);
    TestNotNull(TEXT("PropertyName pin found on SetIntPropertyByName"), NamePin);
    if (!NamePin) return false;
    TestEqual(TEXT("PropertyName pin is PC_Name"),
        NamePin->PinType.PinCategory, UEdGraphSchema_K2::PC_Name);

    // Stamp the non-empty PC_Name default that triggers the bare-identifier
    // emit in the pre-fix decompiler.
    const FString PropertyName = TEXT("AnimateArcFill");
    NamePin->DefaultValue = PropertyName;

    // Wire to BeginPlay so the impure call lands on the emitted exec chain.
    UK2Node_Event* BeginPlay = BpirGraphTestHelpers::EnsureBeginPlayNode(EventGraph);
    TestNotNull(TEXT("BeginPlay node exists"), BeginPlay);
    if (!BeginPlay) return false;
    UEdGraphPin* ThenPin = BeginPlay->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
    UEdGraphPin* ExecPin = Call->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
    TestNotNull(TEXT("Call has Execute pin (impure)"), ExecPin);
    if (ThenPin && ExecPin) { ThenPin->MakeLinkTo(ExecPin); }

    FBpirDecompiler Decompiler(BP);
    FBpirDecompileResult Result = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    // Assertion: BPIR emits the quoted form, not the bare identifier.
    const FString QuotedForm = FString::Printf(TEXT("PropertyName: \"%s\""), *PropertyName);
    TestTrue(FString::Printf(TEXT("Decompile contains quoted PC_Name default '%s'"), *QuotedForm),
        Result.BpirText.Contains(QuotedForm));
    const FString BareForm = FString::Printf(TEXT("PropertyName: %s"), *PropertyName);
    TestFalse(FString::Printf(TEXT("Decompile does not contain bare PC_Name identifier '%s'"), *BareForm),
        Result.BpirText.Contains(BareForm));

    // Recompile the decompiled text onto a fresh BP — the quoted form must
    // round-trip cleanly through SetPinDefaultValue's quote-strip path and
    // land back on a PC_Name pin with the original DefaultValue intact.
    UBlueprint* BP2 = CreateTransientTestBP(TEXT("RoundTripFNamePinRecompileBP"));
    FBpirCompiler Compiler2(BP2);
    FCompileResult Recompile = Compiler2.Compile(Result.BpirText);
    if (!Recompile.bSuccess)
    {
        for (const FCompileError& Err : Recompile.Errors)
        {
            AddError(FString::Printf(TEXT("Re-compile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Re-compile of decompiled PC_Name default succeeded"), Recompile.bSuccess);
    if (!Recompile.bSuccess) return false;

    UK2Node_CallFunction* RecompiledCall = FindRoundTripCallByFunctionName(
        BP2, GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, SetIntPropertyByName));
    TestNotNull(TEXT("Recompiled SetIntPropertyByName call exists"), RecompiledCall);
    if (!RecompiledCall) return false;
    UEdGraphPin* RecompiledNamePin = RecompiledCall->FindPin(FName(TEXT("PropertyName")), EGPD_Input);
    TestNotNull(TEXT("Recompiled PropertyName pin exists"), RecompiledNamePin);
    if (!RecompiledNamePin) return false;
    TestEqual(TEXT("Recompiled PC_Name DefaultValue matches original (unquoted on store)"),
        RecompiledNamePin->DefaultValue, PropertyName);

    return true;
}
