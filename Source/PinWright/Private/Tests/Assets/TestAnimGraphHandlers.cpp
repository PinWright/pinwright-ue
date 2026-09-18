// Copyright (c) 2026 Alexander Penkin. MIT License.

// Phase 2 round-trip + decompile tests for AGIR (AnimGraph IR).
//
// Mirrors TestMGIRCompositeInline / TestMaterialHandlers patterns:
// - Lyra mannequin AnimBP fixture for end-to-end decompile / dump aspect tests
// - Tokenizer + grammar smoke test exercises every AGIR opcode keyword
// - Parser smoke test confirms that decompile text round-trips through the parser
// - Dispatcher round-trip exercises the `anim.decompile_agir` RPC contract
// - Phase 3 round-trip tests exercise FAGIRCompiler against fresh target assets,
//   plus the explicit Phase 3 cliff codes (AGIR_SUBGRAPH_NOT_SUPPORTED,
//   AGIR_TARGET_NOT_FOUND).
#include "Misc/AutomationTest.h"
#include "Compat/EngineVersionCompat.h"


#include "AGIR/AGIRCompiler.h"
#include "AGIR/AGIRDecompiler.h"
#include "AGIR/AGIRGrammar.h"
#include "AGIR/AGIROpcodes.h"
#include "AGIR/AGIRParser.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_BlendListByBool.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimGraphNode_ModifyBone.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_TwoBoneIK.h"
#include "AnimNodes/AnimNode_LayeredBoneBlend.h"
#include "Animation/AnimData/BoneMaskFilter.h"
#include "BoneControllers/AnimNode_ModifyBone.h"
#include "BoneControllers/AnimNode_TwoBoneIK.h"
#include "AnimStateAliasNode.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNode.h"
#include "AnimStateNodeBase.h"
#include "AnimStateTransitionNode.h"
#include "Animation/AnimStateMachineTypes.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimationAsset.h"
#include "AnimationGraph.h"
#include "AnimationStateMachineGraph.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dispatch/RpcDispatcher.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/SkeletalMesh.h"
#include "Factories/AnimBlueprintFactory.h"
#include "HAL/FileManager.h"
#include "Handlers/Animation/AnimGraphConstructionUtils.h"
#include "Handlers/Animation/AnimGraphDumpBuilder.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "IrCore/IrToken.h"
#include "IrCore/IrTokenizer.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"

namespace
{
// Host Mannequin locomotion AnimBP — Lyra mannequin content that not every host
// project ships (many hosts delete it), so every consuming test gates on
// PINWRIGHT_SKIP_IF_FIXTURE_MISSING before loading. Where present it carries a
// locomotion state machine, so positive assertions on `state_machine` content
// are safe. (Constant name kept for source compatibility with other test files.)
const TCHAR* LyraMannequinAnimBPPath =
    TEXT("/Game/Characters/Mannequins/Animations/ABP_Manny.ABP_Manny");

UAnimBlueprint* LoadLyraMannequinAnimBP()
{
    return LoadObject<UAnimBlueprint>(nullptr, LyraMannequinAnimBPPath);
}

struct FAgirDispatchResult
{
    bool bCompletionFired = false;
    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
};

FAgirDispatchResult DispatchAnimDecompileViaDispatcher(
    const FString& RequestId,
    const TSharedPtr<FJsonObject>& Payload)
{
    FAgirDispatchResult Out;
    FRpcDispatcher Dispatcher;
    // Initialize BEFORE draining so the bridge lambdas capture a live sink.
    Dispatcher.Initialize(FResponseSink(
        [&Out](const FString&, bool bSuccess, const FString&,
               const TSharedPtr<FJsonObject>& Result, const FString& ErrorCode)
        {
            Out.bCompletionFired = true;
            Out.bSuccess = bSuccess;
            Out.ErrorCode = ErrorCode;
            Out.Result = Result;
        }));
    // Register the production handlers into this fresh dispatcher; Initialize only
    // installs the sink. Without this, every dispatch resolves to UNKNOWN_ACTION.
    Dispatcher.DrainAutoRegistrations(nullptr);

    Dispatcher.ProcessRequest(RequestId, TEXT("anim.decompile_agir"),
        Payload.IsValid() ? Payload : MakeShared<FJsonObject>());
    return Out;
}

// Generic dispatcher entry-point used by anim authoring / discovery tests.
// Mirrors DispatchAnimDecompileViaDispatcher but parameterises the method name
// so subsequent sprint tests can reuse this single helper.
FAgirDispatchResult DispatchAnimRpcViaDispatcher(
    const FString& Method,
    const FString& RequestId,
    const TSharedPtr<FJsonObject>& Payload)
{
    FAgirDispatchResult Out;
    FRpcDispatcher Dispatcher;
    Dispatcher.Initialize(FResponseSink(
        [&Out](const FString&, bool bSuccess, const FString&,
               const TSharedPtr<FJsonObject>& Result, const FString& ErrorCode)
        {
            Out.bCompletionFired = true;
            Out.bSuccess = bSuccess;
            Out.ErrorCode = ErrorCode;
            Out.Result = Result;
        }));
    // Register the production handlers into this fresh dispatcher; Initialize only
    // installs the sink. Without this, every dispatch resolves to UNKNOWN_ACTION.
    Dispatcher.DrainAutoRegistrations(nullptr);

    Dispatcher.ProcessRequest(RequestId, Method,
        Payload.IsValid() ? Payload : MakeShared<FJsonObject>());
    return Out;
}

// Create a fresh empty UAnimBlueprint at PackagePath bound to the supplied
// skeleton. Used by Phase 3 round-trip tests as the compile target so the
// source asset is never mutated.
UAnimBlueprint* CreateFreshAnimBlueprint(const FString& PackagePath, USkeleton* Skeleton)
{
    FString FolderPath;
    FString AssetName;
    PackagePath.Split(TEXT("/"), &FolderPath, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        return nullptr;
    }

    UAnimBlueprintFactory* Factory = NewObject<UAnimBlueprintFactory>();
    Factory->TargetSkeleton = Skeleton;
    Factory->ParentClass = UAnimInstance::StaticClass();
    UAnimBlueprint* NewBP = Cast<UAnimBlueprint>(
        Factory->FactoryCreateNew(UAnimBlueprint::StaticClass(), Package,
            FName(*AssetName), RF_Public | RF_Standalone, nullptr, GWarn));
    if (!NewBP)
    {
        return nullptr;
    }

    FAssetRegistryModule::AssetCreated(NewBP);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(NewBP);
    return NewBP;
}

UAnimSequence* FindCompatibleAnimSequenceForSkeleton(USkeleton* Skeleton, FString& OutSequencePath)
{
    if (!Skeleton)
    {
        return nullptr;
    }

    static const TCHAR* const Candidates[] = {
        TEXT("/Game/Characters/Mannequins/Animations/Manny/MM_Idle.MM_Idle"),
        TEXT("/Game/Characters/Mannequins/Animations/Manny/MM_Run_Fwd.MM_Run_Fwd"),
    };
    for (const TCHAR* Candidate : Candidates)
    {
        UAnimSequence* CandidateSequence = LoadObject<UAnimSequence>(nullptr, Candidate);
        if (CandidateSequence && CandidateSequence->GetSkeleton() &&
            CandidateSequence->GetSkeleton()->IsCompatibleForEditor(Skeleton))
        {
            OutSequencePath = Candidate;
            return CandidateSequence;
        }
    }

    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    FARFilter Filter;
    Filter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());
    Filter.PackagePaths.Add(FName(TEXT("/Game")));
    Filter.bRecursivePaths = true;
    TArray<FAssetData> Found;
    ARM.Get().GetAssets(Filter, Found);
    for (const FAssetData& AD : Found)
    {
        UAnimSequence* CandidateSequence = Cast<UAnimSequence>(AD.GetAsset());
        if (CandidateSequence && CandidateSequence->GetSkeleton() &&
            CandidateSequence->GetSkeleton()->IsCompatibleForEditor(Skeleton))
        {
            OutSequencePath = AD.GetObjectPathString();
            return CandidateSequence;
        }
    }

    return nullptr;
}
} // namespace

// ============================================================================
// FAGIRDecompiler smoke: Lyra mannequin AnimBP decompiles to non-empty AGIR
// text containing at least one `state_machine` block.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAGIRDecompilerLyraMannequinBaseTest,
    "PinWright.anim.agir.DecompilerLyraMannequinBase",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAGIRDecompilerLyraMannequinBaseTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(LyraMannequinAnimBPPath);
    UAnimBlueprint* AnimBP = LoadLyraMannequinAnimBP();
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), AnimBP))
    {
        return false;
    }

    FAGIRDecompiler Decompiler(AnimBP);
    FAGIRDecompileResult Result = Decompiler.Decompile();

    TestTrue(TEXT("decompile reports success"), Result.bSuccess);
    if (!Result.bSuccess)
    {
        return false;
    }

    TestFalse(TEXT("decompile text is non-empty"), Result.AGIRText.IsEmpty());
    TestTrue(TEXT("decompile text contains a state_machine block"),
        Result.AGIRText.Contains(TEXT("state_machine ")));
    return true;
}

// ============================================================================
// FAGIRParser round-trip: parse the text emitted by FAGIRDecompiler.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAGIRParserParsesEmittedTextTest,
    "PinWright.anim.agir.ParserParsesEmittedText",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAGIRParserParsesEmittedTextTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(LyraMannequinAnimBPPath);
    UAnimBlueprint* AnimBP = LoadLyraMannequinAnimBP();
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), AnimBP))
    {
        return false;
    }

    FAGIRDecompileResult DecompileResult = FAGIRDecompiler(AnimBP).Decompile();
    TestTrue(TEXT("decompile reports success"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess || DecompileResult.AGIRText.IsEmpty())
    {
        return false;
    }

    TArray<FAGIREntryBlock> Blocks;
    TArray<FAGIRParseError> Errors;
    const bool bParsed = FAGIRParser::Parse(DecompileResult.AGIRText, Blocks, Errors,
        /*bSkipReferenceValidation=*/true);

    if (!bParsed)
    {
        for (const FAGIRParseError& Err : Errors)
        {
            AddError(FString::Printf(TEXT("AGIR parse error (line %d, code='%s'): %s"),
                Err.Line, *Err.Code, *Err.Message));
        }
    }
    TestTrue(TEXT("parser accepts decompile output"), bParsed);
    TestTrue(TEXT("parser produced at least one entry block"), Blocks.Num() > 0);
    return true;
}

// ============================================================================
// FAGIRGrammar + FIrTokenizer: every opcode keyword tokenizes as Keyword and
// resolves through the grammar's TryGetOpcode.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAGIRGrammarOpcodeKeywordsTest,
    "PinWright.anim.agir.GrammarOpcodeKeywords",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAGIRGrammarOpcodeKeywordsTest::RunTest(const FString& Parameters)
{
    const FAGIRGrammar& Grammar = FAGIRGrammar::Get();

    static const EAGIROpcode AllOpcodes[] = {
        EAGIROpcode::Call,
        EAGIROpcode::StateMachine,
        EAGIROpcode::BlendSpace,
        EAGIROpcode::LayeredBlend,
        EAGIROpcode::LinkedAnim,
        EAGIROpcode::LinkedInputPose,
        EAGIROpcode::SaveCachedPose,
        EAGIROpcode::UseCachedPose,
        EAGIROpcode::Output,
        EAGIROpcode::State,
        EAGIROpcode::Transition,
        EAGIROpcode::Conduit,
    };

    for (EAGIROpcode Opcode : AllOpcodes)
    {
        const FString Keyword = FAGIRGrammar::OpcodeToText(Opcode);
        TestFalse(FString::Printf(TEXT("opcode %d has non-empty keyword"), static_cast<int32>(Opcode)),
            Keyword.IsEmpty());

        TestTrue(FString::Printf(TEXT("grammar treats '%s' as a keyword"), *Keyword),
            Grammar.IsKeyword(Keyword));

        int32 ResolvedOpcode = -1;
        TestTrue(FString::Printf(TEXT("grammar resolves '%s' to an opcode"), *Keyword),
            Grammar.TryGetOpcode(Keyword, ResolvedOpcode));
        TestEqual(FString::Printf(TEXT("opcode round-trips via keyword '%s'"), *Keyword),
            ResolvedOpcode, static_cast<int32>(Opcode));

        // Tokenizer should classify the bare keyword as EIrTokenType::Keyword.
        const TArray<FIrToken> Tokens = FIrTokenizer::Tokenize(Keyword, Grammar);
        TestEqual(FString::Printf(TEXT("'%s' tokenizes as one token"), *Keyword),
            Tokens.Num(), 1);
        if (Tokens.Num() == 1)
        {
            TestEqual(FString::Printf(TEXT("'%s' tokenizes as Keyword"), *Keyword),
                static_cast<int32>(Tokens[0].Type), static_cast<int32>(EIrTokenType::Keyword));
            TestEqual(FString::Printf(TEXT("'%s' token text matches"), *Keyword),
                Tokens[0].Text, Keyword);
        }
    }
    return true;
}

// ============================================================================
// AssetDumpHandler.AnimBlueprint dump aspect: dumping Lyra mannequin AnimBP
// produces both `anim_graph.json` and `agir.txt`, both with non-empty content.
//
// Counterfactual: reverting `AddStringFile(DumpFileNames::Agir, ...)` in
// Chunk 5C drops `agir.txt` from WrittenPaths and this test fails.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAGIRDumpAspectIntegrationTest,
    "PinWright.anim.agir.DumpAspectIntegration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAGIRDumpAspectIntegrationTest::RunTest(const FString& Parameters)
{
    const FString TestRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir())
        / TEXT("AssetDumpAGIRTests")
        / FGuid::NewGuid().ToString();

    ON_SCOPE_EXIT
    {
        IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
    };

    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(LyraMannequinAnimBPPath);
    AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(LyraMannequinAnimBPPath, TestRoot, /*bDiff=*/false);

    if (!TestTrue(FString::Printf(
            TEXT("Mannequin AnimBP dump succeeds (errorCode='%s': %s)"),
            *Result.ErrorCode, *Result.ErrorMessage),
            Result.ErrorCode.IsEmpty()))
    {
        return false;
    }

    FString AnimGraphPath;
    FString AgirPath;
    for (const FString& Path : Result.WrittenPaths)
    {
        if (Path.EndsWith(DumpFileNames::AnimGraph))
        {
            AnimGraphPath = Path;
        }
        else if (Path.EndsWith(DumpFileNames::Agir))
        {
            AgirPath = Path;
        }
    }

    TestFalse(TEXT("anim_graph.json is in WrittenPaths"), AnimGraphPath.IsEmpty());
    TestFalse(TEXT("agir.txt is in WrittenPaths"), AgirPath.IsEmpty());
    if (AnimGraphPath.IsEmpty() || AgirPath.IsEmpty())
    {
        return false;
    }

    TestTrue(FString::Printf(TEXT("anim_graph.json exists on disk: %s"), *AnimGraphPath),
        IFileManager::Get().FileExists(*AnimGraphPath));
    TestTrue(FString::Printf(TEXT("agir.txt exists on disk: %s"), *AgirPath),
        IFileManager::Get().FileExists(*AgirPath));

    FString AnimGraphRaw;
    TestTrue(TEXT("anim_graph.json is readable"),
        FFileHelper::LoadFileToString(AnimGraphRaw, *AnimGraphPath));
    TestFalse(TEXT("anim_graph.json is non-empty"), AnimGraphRaw.IsEmpty());

    FString AgirRaw;
    TestTrue(TEXT("agir.txt is readable"),
        FFileHelper::LoadFileToString(AgirRaw, *AgirPath));
    TestFalse(TEXT("agir.txt is non-empty"), AgirRaw.IsEmpty());
    return true;
}

// ============================================================================
// anim.decompile_agir RPC: dispatcher round-trip returns a JSON object whose
// top-level fields are { assetPath, text, warnings }.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAGIRDecompileRPCHandlerTest,
    "PinWright.anim.decompile_agir.DispatcherRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAGIRDecompileRPCHandlerTest::RunTest(const FString& Parameters)
{
    if (!IsHandlerRegistered(TEXT("anim.decompile_agir")))
    {
        AddError(TEXT("anim.decompile_agir handler is not registered."));
        return false;
    }

    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(LyraMannequinAnimBPPath);
    UAnimBlueprint* AnimBP = LoadLyraMannequinAnimBP();
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), AnimBP))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), LyraMannequinAnimBPPath);

    const FAgirDispatchResult Dispatch = DispatchAnimDecompileViaDispatcher(
        TEXT("req-agir-decompile-ok"), Payload);

    TestTrue(TEXT("completion fired"), Dispatch.bCompletionFired);
    TestTrue(FString::Printf(TEXT("dispatch reports success (errorCode='%s')"), *Dispatch.ErrorCode),
        Dispatch.bSuccess);
    if (!Dispatch.bSuccess || !Dispatch.Result.IsValid())
    {
        return false;
    }

    FString EchoedAssetPath;
    TestTrue(TEXT("response contains 'assetPath'"),
        Dispatch.Result->TryGetStringField(TEXT("assetPath"), EchoedAssetPath));
    TestEqual(TEXT("response echoes input assetPath"), EchoedAssetPath, FString(LyraMannequinAnimBPPath));

    FString DecompiledText;
    TestTrue(TEXT("response contains 'text'"),
        Dispatch.Result->TryGetStringField(TEXT("text"), DecompiledText));
    TestFalse(TEXT("response 'text' is non-empty"), DecompiledText.IsEmpty());

    const TArray<TSharedPtr<FJsonValue>>* WarningsArray = nullptr;
    TestTrue(TEXT("response contains 'warnings' array"),
        Dispatch.Result->TryGetArrayField(TEXT("warnings"), WarningsArray));
    return true;
}

// ============================================================================
// Phase 3 round-trip: tiny flat AnimBlueprint authored programmatically (two
// SequencePlayers + a BlendListByBool), decompiled to AGIR, then compiled to a
// fresh target. Asserts node-class set and pose-link count round-trip. Skips
// gracefully if the source AnimBP cannot be authored (no skeleton available).
// ============================================================================

namespace
{
// Counts pose-typed input-pin link edges across all anim graph nodes in the
// supplied target graph. Used by the flat round-trip test as a structural
// equivalence check that doesn't depend on specific field values. Pose pins
// are PC_Struct pins whose subcategory is FPoseLink (or component-space pose
// link); we check for the "PoseLink" suffix on the struct name to cover both.
int32 CountPoseLinksOnAnimGraph(UEdGraph* Graph)
{
    if (!Graph)
    {
        return 0;
    }
    int32 LinkCount = 0;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node)
        {
            continue;
        }
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Input)
            {
                continue;
            }
            const UScriptStruct* SubStruct = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get());
            if (SubStruct && SubStruct->GetName().Contains(TEXT("PoseLink")))
            {
                LinkCount += Pin->LinkedTo.Num();
            }
        }
    }
    return LinkCount;
}

// Collects the FName of every UAnimGraphNode_Base subclass present in Graph.
TSet<FName> CollectAnimNodeClassNames(UEdGraph* Graph)
{
    TSet<FName> Names;
    if (!Graph)
    {
        return Names;
    }
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node))
        {
            Names.Add(AnimNode->GetClass()->GetFName());
        }
    }
    return Names;
}

// Returns the AnimGraph's Output (Root) node, or nullptr if absent. Mirrors the
// compiler's FindRootNode (AGIRCompiler.cpp), kept file-local because that one
// lives in an anonymous namespace.
UAnimGraphNode_Root* FindAnimGraphRoot(UEdGraph* Graph)
{
    if (!Graph)
    {
        return nullptr;
    }
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (UAnimGraphNode_Root* RootNode = Cast<UAnimGraphNode_Root>(Node))
        {
            return RootNode;
        }
    }
    return nullptr;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAGIRRoundTripFlatGraphTest,
    "PinWright.anim.agir.RoundTripFlatGraph",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAGIRRoundTripFlatGraphTest::RunTest(const FString& Parameters)
{
    // Borrow the Mannequin AnimBP's skeleton so the fresh source/target AnimBPs
    // are valid. Host absence is gated below; a package that exists but fails
    // to load is still a hard failure, not a skip.
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(LyraMannequinAnimBPPath);
    UAnimBlueprint* Fixture = LoadLyraMannequinAnimBP();
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), Fixture))
    {
        return false;
    }
    if (!TestNotNull(TEXT("fixture has TargetSkeleton"), Fixture->TargetSkeleton.Get()))
    {
        return false;
    }
    USkeleton* Skeleton = Fixture->TargetSkeleton;

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePath = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_AGIR_Src_%s"), *Guid);
    const FString TargetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_AGIR_%s"), *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePath);
        CleanupTestAsset(TargetPath);
    };

    UAnimBlueprint* SourceBP = CreateFreshAnimBlueprint(SourcePath, Skeleton);
    if (!TestNotNull(TEXT("source AnimBlueprint created"), SourceBP))
    {
        return false;
    }

    UEdGraph* SourceAnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(SourceBP);
    if (!TestNotNull(TEXT("source AnimBP has a default AnimGraph page"), SourceAnimGraph))
    {
        return false;
    }

    // Author two SequencePlayer nodes + one BlendListByBool node on the source
    // AnimGraph. Pose connections are not strictly necessary for the structural
    // round-trip assertions; the decompile path emits whatever pose links exist.
    UAnimGraphNode_Base* SeqA = AnimGraphConstructionUtils::CreateAnimNode(
        SourceAnimGraph, UAnimGraphNode_SequencePlayer::StaticClass(), FVector2D(0, 0));
    UAnimGraphNode_Base* SeqB = AnimGraphConstructionUtils::CreateAnimNode(
        SourceAnimGraph, UAnimGraphNode_SequencePlayer::StaticClass(), FVector2D(0, 200));
    UAnimGraphNode_Base* Blend = AnimGraphConstructionUtils::CreateAnimNode(
        SourceAnimGraph, UAnimGraphNode_BlendListByBool::StaticClass(), FVector2D(300, 100));

    TestNotNull(TEXT("source SequencePlayer A authored"), SeqA);
    TestNotNull(TEXT("source SequencePlayer B authored"), SeqB);
    TestNotNull(TEXT("source BlendListByBool authored"), Blend);
    if (!SeqA || !SeqB || !Blend)
    {
        return false;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(SourceBP);

    const TSet<FName> SourceClasses = CollectAnimNodeClassNames(SourceAnimGraph);

    // Decompile source -> AGIR text.
    FAGIRDecompileResult DecompileResult = FAGIRDecompiler(SourceBP).Decompile();
    TestTrue(TEXT("decompile of source succeeds"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess || DecompileResult.AGIRText.IsEmpty())
    {
        return false;
    }

    // Compile -> fresh target.
    UAnimBlueprint* TargetBP = CreateFreshAnimBlueprint(TargetPath, Skeleton);
    TestNotNull(TEXT("target AnimBP created"), TargetBP);
    if (!TargetBP)
    {
        return false;
    }

    FAGIRCompileOptions Options;
    Options.Context = TargetBP->GetPathName();
    Options.Mode = EAGIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    FAGIRCompileResult CompileResult = FAGIRCompiler::Compile(DecompileResult.AGIRText, Options);
    TestTrue(FString::Printf(TEXT("compile to target succeeds (errorCode='%s', message='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        return false;
    }

    UEdGraph* TargetAnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(TargetBP);
    TestNotNull(TEXT("target AnimGraph present"), TargetAnimGraph);
    if (!TargetAnimGraph)
    {
        return false;
    }

    const TSet<FName> TargetClasses = CollectAnimNodeClassNames(TargetAnimGraph);
    for (const FName& Name : SourceClasses)
    {
        TestTrue(FString::Printf(TEXT("target contains source class '%s'"), *Name.ToString()),
            TargetClasses.Contains(Name));
    }

    const int32 SourceLinks = CountPoseLinksOnAnimGraph(SourceAnimGraph);
    const int32 TargetLinks = CountPoseLinksOnAnimGraph(TargetAnimGraph);
    TestEqual(TEXT("pose-link count round-trips"), TargetLinks, SourceLinks);
    return true;
}

// ============================================================================
// Phase 3 round-trip: Lyra mannequin AnimBP -> AGIR -> fresh target with the
// same skeleton. Assert state-machine count and total transition count
// round-trip. Skips if the Lyra fixture is not loadable.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAGIRRoundTripStateMachineTest,
    "PinWright.anim.agir.RoundTripStateMachine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAGIRRoundTripStateMachineTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(LyraMannequinAnimBPPath);
    UAnimBlueprint* SourceBP = LoadLyraMannequinAnimBP();
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), SourceBP))
    {
        return false;
    }
    if (!TestNotNull(TEXT("fixture has TargetSkeleton"), SourceBP->TargetSkeleton.Get()))
    {
        return false;
    }

    auto CountStateMachinesAndTransitions = [](UAnimBlueprint* BP, int32& OutMachines, int32& OutTransitions)
    {
        OutMachines = 0;
        OutTransitions = 0;
        if (!BP)
        {
            return;
        }
        TArray<UAnimGraphNode_StateMachineBase*> StateMachineNodes;
        FBlueprintEditorUtils::GetAllNodesOfClass(BP, StateMachineNodes);
        OutMachines = StateMachineNodes.Num();
        for (UAnimGraphNode_StateMachineBase* SMNode : StateMachineNodes)
        {
            if (!SMNode || !SMNode->EditorStateMachineGraph)
            {
                continue;
            }
            for (UEdGraphNode* InnerNode : SMNode->EditorStateMachineGraph->Nodes)
            {
                if (Cast<UAnimStateTransitionNode>(InnerNode))
                {
                    ++OutTransitions;
                }
            }
        }
    };

    int32 SourceMachines = 0;
    int32 SourceTransitions = 0;
    CountStateMachinesAndTransitions(SourceBP, SourceMachines, SourceTransitions);

    // ABP_Manny carries a locomotion state machine, so a zero count is a real
    // regression (either the fixture changed or state-machine discovery broke).
    if (!TestTrue(TEXT("Mannequin AnimBP has at least one state machine"), SourceMachines > 0))
    {
        return false;
    }

    FAGIRDecompileResult DecompileResult = FAGIRDecompiler(SourceBP).Decompile();
    TestTrue(TEXT("decompile of Lyra mannequin succeeds"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess || DecompileResult.AGIRText.IsEmpty())
    {
        return false;
    }

    const FString TargetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_AGIR_SM_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(TargetPath);
    };

    UAnimBlueprint* TargetBP = CreateFreshAnimBlueprint(TargetPath, SourceBP->TargetSkeleton);
    TestNotNull(TEXT("target AnimBP created"), TargetBP);
    if (!TargetBP)
    {
        return false;
    }

    FAGIRCompileOptions Options;
    Options.Context = TargetBP->GetPathName();
    Options.Mode = EAGIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    // All seven Phase 3 cliff families (blend_space, layered_blend, custom
    // transitions, linked_anim, linked_input_pose, save/use_cached_pose) now
    // round-trip, so the previous AGIR_SUBGRAPH_NOT_SUPPORTED tolerance is gone:
    // any compile failure against the Lyra mannequin fixture is a real regression.
    FAGIRCompileResult CompileResult = FAGIRCompiler::Compile(DecompileResult.AGIRText, Options);
    if (!CompileResult.bSuccess)
    {
        AddError(FString::Printf(TEXT("compile failed unexpectedly: %s: %s"),
            *CompileResult.ErrorCode, *CompileResult.ErrorMessage));
        return false;
    }

    int32 TargetMachines = 0;
    int32 TargetTransitions = 0;
    CountStateMachinesAndTransitions(TargetBP, TargetMachines, TargetTransitions);

    TestEqual(TEXT("state machine count round-trips"), TargetMachines, SourceMachines);
    TestEqual(TEXT("transition count round-trips"), TargetTransitions, SourceTransitions);
    return true;
}

// ============================================================================
// Regression (B-agir-state-machine-output-pose-unbound): an AnimBP whose
// top-level AnimGraph output IS a single state machine — the most ordinary
// locomotion-AnimBP shape — must survive the verbatim decompile -> compile
// round-trip. The Lyra-mannequin round-trip above never exercises this: its
// output is fronted by cached poses, so its top-level `output` ref points at a
// `%save_cached_pose_*` symbol (which carries an explicit `%n = ` binding),
// never at a bare `%state_machine_0`. The decompiler emits the state machine
// with the unassigned name-token opener (no `%n = `), so before the fix the
// compiler never registered it in Symbols and the consuming `output
// %state_machine_0` line failed with AGIR_SYMBOL_NOT_FOUND. This test fails if
// the positional-id registration in AGIRCompiler.cpp's StateMachine case is
// reverted.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAGIRRoundTripTopLevelStateMachineOutputTest,
    "PinWright.anim.agir.RoundTripTopLevelStateMachineOutput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAGIRRoundTripTopLevelStateMachineOutputTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(LyraMannequinAnimBPPath);
    UAnimBlueprint* Fixture = LoadLyraMannequinAnimBP();
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), Fixture))
    {
        return false;
    }
    if (!TestNotNull(TEXT("fixture has TargetSkeleton"), Fixture->TargetSkeleton.Get()))
    {
        return false;
    }
    USkeleton* Skeleton = Fixture->TargetSkeleton;

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePath = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_AGIR_SMOut_Src_%s"), *Guid);
    const FString TargetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_AGIR_SMOut_%s"), *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePath);
        CleanupTestAsset(TargetPath);
    };

    UAnimBlueprint* SourceBP = CreateFreshAnimBlueprint(SourcePath, Skeleton);
    if (!TestNotNull(TEXT("source AnimBlueprint created"), SourceBP))
    {
        return false;
    }

    UEdGraph* SourceAnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(SourceBP);
    if (!TestNotNull(TEXT("source AnimBP has a default AnimGraph page"), SourceAnimGraph))
    {
        return false;
    }

    // Author a single state machine directly on the AnimGraph and wire its Pose
    // output straight into the Output (Result) node — the bare "AnimGraph output
    // IS the state machine" topology, with no cached-pose / layered-blend node
    // in between.
    UAnimGraphNode_StateMachine* MachineNode = AnimGraphConstructionUtils::CreateStateMachine(
        SourceBP, SourceAnimGraph, FName(TEXT("Locomotion")), FVector2D(-256, 0));
    if (!TestNotNull(TEXT("state machine node authored"), MachineNode))
    {
        return false;
    }
    if (!TestNotNull(TEXT("authored state machine has an editor graph"), MachineNode->EditorStateMachineGraph.Get()))
    {
        return false;
    }
    // One state so the machine is non-degenerate (mirrors a real locomotion SM).
    AnimGraphConstructionUtils::CreateState(
        MachineNode->EditorStateMachineGraph, FName(TEXT("Idle")), FVector2D(0, 0));

    UAnimGraphNode_Root* RootNode = FindAnimGraphRoot(SourceAnimGraph);
    TestNotNull(TEXT("source AnimGraph has an Output (Root) node"), RootNode);
    if (!RootNode)
    {
        return false;
    }

    const bool bWired = AnimGraphConstructionUtils::WirePoseLink(
        MachineNode, FName(TEXT("Pose")), RootNode, FName(TEXT("Result")));
    TestTrue(TEXT("state machine Pose output wired to AnimGraph Result"), bWired);
    if (!bWired)
    {
        return false;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(SourceBP);

    // Decompile -> AGIR text. Expect the bare single-state-machine topology:
    // `output %state_machine_0` plus a name-token `state_machine Locomotion {`
    // opener with no `%state_machine_0 = ` binding.
    FAGIRDecompileResult DecompileResult = FAGIRDecompiler(SourceBP).Decompile();
    TestTrue(TEXT("decompile of single-top-level-state-machine source succeeds"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess || DecompileResult.AGIRText.IsEmpty())
    {
        return false;
    }
    // Guard the topology assumption: if the emitter ever stopped referencing the
    // machine via the bare positional id, this test would silently stop covering
    // the bug. Assert the exact unbound-ref shape the defect is about.
    TestTrue(TEXT("decompiler output references the state machine by %state_machine_0"),
        DecompileResult.AGIRText.Contains(TEXT("output %state_machine_0")));

    // Feed the UNMODIFIED decompiler output back into the compiler against a
    // fresh target — the canonical decompile -> recompile workflow.
    UAnimBlueprint* TargetBP = CreateFreshAnimBlueprint(TargetPath, Skeleton);
    TestNotNull(TEXT("target AnimBP created"), TargetBP);
    if (!TargetBP)
    {
        return false;
    }

    FAGIRCompileOptions Options;
    Options.Context = TargetBP->GetPathName();
    Options.Mode = EAGIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    FAGIRCompileResult CompileResult = FAGIRCompiler::Compile(DecompileResult.AGIRText, Options);
    // Pre-fix this returns AGIR_SYMBOL_NOT_FOUND on the `output %state_machine_0`
    // wire; with the positional-id registration it must compile cleanly.
    TestTrue(FString::Printf(
        TEXT("verbatim decompiler output recompiles (errorCode='%s', message='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        return false;
    }

    // The state machine landed in the target and its output pose is wired to the
    // Result node (the wire the unbound ref previously failed to create).
    UEdGraph* TargetAnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(TargetBP);
    TestNotNull(TEXT("target AnimGraph present"), TargetAnimGraph);
    if (!TargetAnimGraph)
    {
        return false;
    }

    TArray<UAnimGraphNode_StateMachineBase*> TargetMachines;
    FBlueprintEditorUtils::GetAllNodesOfClass(TargetBP, TargetMachines);
    TestEqual(TEXT("target has exactly one state machine"), TargetMachines.Num(), 1);

    UAnimGraphNode_Root* TargetRoot = FindAnimGraphRoot(TargetAnimGraph);
    TestNotNull(TEXT("target AnimGraph has an Output (Root) node"), TargetRoot);
    if (TargetRoot)
    {
        UEdGraphPin* ResultPin = TargetRoot->FindPin(FName(TEXT("Result")));
        const bool bResultLinked = ResultPin && ResultPin->LinkedTo.Num() > 0 && ResultPin->LinkedTo[0];
        TestTrue(TEXT("target Result pin is wired to the state machine"), bResultLinked);
    }
    return true;
}

// ============================================================================
// Diagnosability: an unresolved %-pose-ref must carry a round-trip diagnostic
// Hint on the compile result so anim.compile_agir can attribute the failure
// (tool round-trip gap vs. user-authored bad AGIR) on the tool surface — the
// caller should not have to read plugin C++ to diagnose. Covers
// E-agir-roundtrip-symbol-not-found-undiagnosable.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAGIRSymbolNotFoundCarriesRoundTripHintTest,
    "PinWright.anim.agir.SymbolNotFoundCarriesRoundTripHint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAGIRSymbolNotFoundCarriesRoundTripHintTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(LyraMannequinAnimBPPath);
    UAnimBlueprint* Fixture = LoadLyraMannequinAnimBP();
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), Fixture))
    {
        return false;
    }
    if (!TestNotNull(TEXT("fixture has TargetSkeleton"), Fixture->TargetSkeleton.Get()))
    {
        return false;
    }
    USkeleton* Skeleton = Fixture->TargetSkeleton;

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString TargetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_AGIR_HintTarget_%s"), *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(TargetPath);
    };

    // A valid target is required so compile gets past AGIR_TARGET_NOT_FOUND and
    // reaches pose-wire resolution, where the unresolved-symbol error fires.
    UAnimBlueprint* TargetBP = CreateFreshAnimBlueprint(TargetPath, Skeleton);
    if (!TestNotNull(TEXT("target AnimBlueprint created"), TargetBP))
    {
        return false;
    }

    // AnimGraph whose output references a %-pose-ref symbol that is never bound —
    // the exact failure shape (an unresolved %-pose-ref) the diagnostic hint
    // covers. This stands in for "decompiler emitted a %-ref the compiler can't
    // reconsume" without depending on any one (possibly-fixed) emitter path.
    const FString AGIRText = TEXT(
        "entry anim_graph \"AnimGraph\" {\n"
        "    output %never_defined_0\n"
        "}\n");

    FAGIRCompileOptions Options;
    Options.Context = TargetBP->GetPathName();
    Options.Mode = EAGIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    FAGIRCompileResult CompileResult = FAGIRCompiler::Compile(AGIRText, Options);

    TestFalse(TEXT("compile of an unresolved %-pose-ref fails"), CompileResult.bSuccess);
    TestEqual(TEXT("error code is AGIR_SYMBOL_NOT_FOUND"),
        CompileResult.ErrorCode, FString(TEXT("AGIR_SYMBOL_NOT_FOUND")));
    // The load-bearing assertion: the result carries the round-trip diagnostic
    // hint. Reverting the .WithHint(...) attach leaves this empty and fails here.
    TestFalse(TEXT("AGIR_SYMBOL_NOT_FOUND result carries a diagnostic hint"),
        CompileResult.Hint.IsEmpty());
    TestTrue(TEXT("hint mentions the decompile round-trip gap"),
        CompileResult.Hint.Contains(TEXT("round-trip")));
    return true;
}

// ============================================================================
// Phase 3: missing target anim BP must surface as AGIR_TARGET_NOT_FOUND.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAGIRCompileRejectsMissingTargetTest,
    "PinWright.anim.agir.CompileRejectsMissingTarget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAGIRCompileRejectsMissingTargetTest::RunTest(const FString& Parameters)
{
    const FString AGIRText = TEXT(
        "entry anim_graph \"AnimGraph\" {\n"
        "    output Result\n"
        "}\n");

    FAGIRCompileOptions Options;
    Options.Context = TEXT("/Game/Nonexistent/DoesNotExist.DoesNotExist");
    Options.Mode = EAGIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    FAGIRCompileResult CompileResult = FAGIRCompiler::Compile(AGIRText, Options);
    TestFalse(TEXT("compile against missing target fails"), CompileResult.bSuccess);
    TestEqual(TEXT("error code is AGIR_TARGET_NOT_FOUND"),
        CompileResult.ErrorCode, FString(TEXT("AGIR_TARGET_NOT_FOUND")));
    return true;
}

// ============================================================================
// animation.authoring.set_anim_graph_pin_exposed: toggles ShowPinForProperties
// bShowPin on a SequencePlayer's PlayRate and confirms a graph pin appears.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimAuthoringSetGraphPinExposedTest,
    "PinWright.anim.authoring.SetGraphPinExposed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimAuthoringSetGraphPinExposedTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(LyraMannequinAnimBPPath);
    UAnimBlueprint* Fixture = LoadLyraMannequinAnimBP();
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), Fixture))
    {
        return false;
    }
    if (!TestNotNull(TEXT("fixture has TargetSkeleton"), Fixture->TargetSkeleton.Get()))
    {
        return false;
    }
    USkeleton* Skeleton = Fixture->TargetSkeleton;

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Path = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_ExposePin_%s"), *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(Path);
    };

    UAnimBlueprint* AnimBP = CreateFreshAnimBlueprint(Path, Skeleton);
    if (!TestNotNull(TEXT("AnimBlueprint created"), AnimBP))
    {
        return false;
    }

    UEdGraph* AnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
    TestNotNull(TEXT("AnimGraph present"), AnimGraph);
    if (!AnimGraph)
    {
        return false;
    }

    UAnimGraphNode_Base* SeqNode = AnimGraphConstructionUtils::CreateAnimNode(
        AnimGraph, UAnimGraphNode_SequencePlayer::StaticClass(), FVector2D::ZeroVector);
    TestNotNull(TEXT("SequencePlayer node authored"), SeqNode);
    if (!SeqNode)
    {
        return false;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    // Pre-assert: PlayRate is hidden by default on a fresh SequencePlayer.
    bool bFoundPreEntry = false;
    bool bPreShowPin = true;
    for (const FOptionalPinFromProperty& Entry : SeqNode->ShowPinForProperties)
    {
        if (Entry.PropertyName == FName(TEXT("PlayRate")))
        {
            bFoundPreEntry = true;
            bPreShowPin = Entry.bShowPin;
            break;
        }
    }
    TestTrue(TEXT("PlayRate is in ShowPinForProperties"), bFoundPreEntry);
    TestFalse(TEXT("PlayRate starts hidden (bShowPin == false)"), bPreShowPin);

    // Dispatch the new RPC.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), Path);
    Payload->SetStringField(TEXT("nodeName"), SeqNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
    Payload->SetStringField(TEXT("propertyName"), TEXT("PlayRate"));
    Payload->SetBoolField(TEXT("exposed"), true);
    Payload->SetBoolField(TEXT("save"), false);

    FAgirDispatchResult Dispatch = DispatchAnimRpcViaDispatcher(
        TEXT("animation.authoring.set_anim_graph_pin_exposed"),
        TEXT("expose-pin-1"),
        Payload);
    TestTrue(FString::Printf(TEXT("dispatch success (errorCode='%s')"), *Dispatch.ErrorCode),
        Dispatch.bSuccess);
    if (!Dispatch.bSuccess)
    {
        return false;
    }

    // Post-assert: PlayRate now shows, and a graph input pin exists for it.
    bool bFoundPostEntry = false;
    bool bPostShowPin = false;
    for (const FOptionalPinFromProperty& Entry : SeqNode->ShowPinForProperties)
    {
        if (Entry.PropertyName == FName(TEXT("PlayRate")))
        {
            bFoundPostEntry = true;
            bPostShowPin = Entry.bShowPin;
            break;
        }
    }
    TestTrue(TEXT("PlayRate entry still present after toggle"), bFoundPostEntry);
    TestTrue(TEXT("PlayRate is now exposed (bShowPin == true)"), bPostShowPin);

    const bool bHasPlayRateInputPin = SeqNode->Pins.ContainsByPredicate(
        [](const UEdGraphPin* Pin)
        {
            return Pin && Pin->Direction == EGPD_Input && Pin->PinName == FName(TEXT("PlayRate"));
        });
    TestTrue(TEXT("PlayRate input pin materialised on node"), bHasPlayRateInputPin);
    return true;
}

// ============================================================================
// animation.authoring.bind_player_asset round-trip on a SequencePlayer node.
// Counterfactual: if the new handler is reverted, dispatcher returns
// METHOD_NOT_FOUND, Node.Sequence stays null, and Node.Sequence == LoadedSequence
// fails.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimAuthoringBindPlayerAssetTest,
    "PinWright.anim.authoring.BindPlayerAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimAuthoringBindPlayerAssetTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(LyraMannequinAnimBPPath);
    UAnimBlueprint* Fixture = LoadLyraMannequinAnimBP();
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), Fixture))
    {
        return false;
    }
    if (!TestNotNull(TEXT("fixture has TargetSkeleton"), Fixture->TargetSkeleton.Get()))
    {
        return false;
    }
    USkeleton* Skeleton = Fixture->TargetSkeleton;

    FString SequencePath;
    UAnimSequence* LoadedSequence = FindCompatibleAnimSequenceForSkeleton(Skeleton, SequencePath);
    if (!TestNotNull(TEXT("a compatible UAnimSequence is present for the skeleton"), LoadedSequence))
    {
        return false;
    }

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Path = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_BindAsset_%s"), *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(Path);
    };

    UAnimBlueprint* AnimBP = CreateFreshAnimBlueprint(Path, Skeleton);
    if (!TestNotNull(TEXT("AnimBlueprint created"), AnimBP))
    {
        return false;
    }

    UEdGraph* AnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
    TestNotNull(TEXT("AnimGraph present"), AnimGraph);
    if (!AnimGraph)
    {
        return false;
    }

    UAnimGraphNode_Base* CreatedNode = AnimGraphConstructionUtils::CreateAnimNode(
        AnimGraph, UAnimGraphNode_SequencePlayer::StaticClass(), FVector2D::ZeroVector);
    TestNotNull(TEXT("SequencePlayer node authored"), CreatedNode);
    UAnimGraphNode_SequencePlayer* SeqNode = Cast<UAnimGraphNode_SequencePlayer>(CreatedNode);
    TestNotNull(TEXT("SequencePlayer cast"), SeqNode);
    if (!SeqNode)
    {
        return false;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    TSharedPtr<FJsonObject> Options = MakeShared<FJsonObject>();
    Options->SetBoolField(TEXT("loop"), false);
    Options->SetNumberField(TEXT("playRate"), 1.5);
    Options->SetNumberField(TEXT("startPosition"), 0.25);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), Path);
    Payload->SetStringField(TEXT("nodeName"), SeqNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
    Payload->SetStringField(TEXT("assetPath"), SequencePath);
    Payload->SetObjectField(TEXT("options"), Options);
    Payload->SetBoolField(TEXT("save"), false);

    FAgirDispatchResult Dispatch = DispatchAnimRpcViaDispatcher(
        TEXT("animation.authoring.bind_player_asset"),
        TEXT("bind-player-asset-1"),
        Payload);
    TestTrue(FString::Printf(TEXT("bind_player_asset success (errorCode='%s')"), *Dispatch.ErrorCode),
        Dispatch.bSuccess);
    if (!Dispatch.bSuccess)
    {
        return false;
    }

    TestEqual(TEXT("GetAnimationAsset() returns the bound sequence"),
        SeqNode->GetAnimationAsset(), static_cast<UAnimationAsset*>(LoadedSequence));
    TestEqual(TEXT("Node.GetSequence() equals the bound sequence"),
        SeqNode->Node.GetSequence(), static_cast<UAnimSequenceBase*>(LoadedSequence));
    TestTrue(TEXT("Node.GetPlayRate() ~= 1.5"),
        FMath::IsNearlyEqual(SeqNode->Node.GetPlayRate(), 1.5f));
    TestFalse(TEXT("Node.IsLooping() == false"),
        SeqNode->Node.IsLooping());
    TestTrue(TEXT("Node.GetStartPosition() ~= 0.25"),
        FMath::IsNearlyEqual(SeqNode->Node.GetStartPosition(), 0.25f));
    return true;
}

// ============================================================================
// animation.authoring.set_sync_group round-trip on a SequencePlayer node.
// Counterfactual: if the handler or inner FAnimNode_AssetPlayerBase write is
// reverted, dispatcher returns METHOD_NOT_FOUND or the SequencePlayer remains
// NAME_None / CanBeLeader.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimAuthoringSetSyncGroupTest,
    "PinWright.anim.authoring.SetSyncGroup",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimAuthoringSetSyncGroupTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(LyraMannequinAnimBPPath);
    UAnimBlueprint* Fixture = LoadLyraMannequinAnimBP();
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), Fixture))
    {
        return false;
    }
    if (!TestNotNull(TEXT("fixture has TargetSkeleton"), Fixture->TargetSkeleton.Get()))
    {
        return false;
    }
    USkeleton* Skeleton = Fixture->TargetSkeleton;

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Path = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_SetSyncGroup_%s"), *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(Path);
    };

    UAnimBlueprint* AnimBP = CreateFreshAnimBlueprint(Path, Skeleton);
    if (!TestNotNull(TEXT("AnimBlueprint created"), AnimBP))
    {
        return false;
    }

    UEdGraph* AnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
    TestNotNull(TEXT("AnimGraph present"), AnimGraph);
    if (!AnimGraph)
    {
        return false;
    }

    UAnimGraphNode_Base* CreatedNode = AnimGraphConstructionUtils::CreateAnimNode(
        AnimGraph, UAnimGraphNode_SequencePlayer::StaticClass(), FVector2D::ZeroVector);
    UAnimGraphNode_SequencePlayer* SeqNode = Cast<UAnimGraphNode_SequencePlayer>(CreatedNode);
    TestNotNull(TEXT("SequencePlayer node authored"), SeqNode);
    if (!SeqNode)
    {
        return false;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), Path);
    Payload->SetStringField(TEXT("nodeName"), SeqNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
    Payload->SetStringField(TEXT("groupName"), TEXT("Locomotion"));
    Payload->SetStringField(TEXT("role"), TEXT("AlwaysFollower"));
    Payload->SetBoolField(TEXT("save"), false);

    FAgirDispatchResult Dispatch = DispatchAnimRpcViaDispatcher(
        TEXT("animation.authoring.set_sync_group"),
        TEXT("set-sync-group-1"),
        Payload);
    TestTrue(FString::Printf(TEXT("set_sync_group success (errorCode='%s')"), *Dispatch.ErrorCode),
        Dispatch.bSuccess);
    if (!Dispatch.bSuccess)
    {
        return false;
    }

    TestEqual(TEXT("Node.GetGroupName() == Locomotion"),
        SeqNode->Node.GetGroupName(), FName(TEXT("Locomotion")));
    TestEqual(TEXT("Node.GetGroupRole() == AlwaysFollower"),
        (int32)SeqNode->Node.GetGroupRole(), (int32)EAnimGroupRole::AlwaysFollower);
    TestTrue(TEXT("Node.GetGroupMethod() == SyncGroup"),
        SeqNode->Node.GetGroupMethod() == EAnimSyncMethod::SyncGroup);

    TSharedPtr<FJsonObject> StandalonePayload = MakeShared<FJsonObject>();
    StandalonePayload->SetStringField(TEXT("blueprintPath"), Path);
    StandalonePayload->SetStringField(TEXT("nodeName"), SeqNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
    StandalonePayload->SetStringField(TEXT("groupName"), TEXT(""));
    StandalonePayload->SetStringField(TEXT("role"), TEXT("Standalone"));
    StandalonePayload->SetBoolField(TEXT("save"), false);

    FAgirDispatchResult StandaloneDispatch = DispatchAnimRpcViaDispatcher(
        TEXT("animation.authoring.set_sync_group"),
        TEXT("set-sync-group-standalone"),
        StandalonePayload);
    TestTrue(FString::Printf(TEXT("Standalone set_sync_group success (errorCode='%s')"), *StandaloneDispatch.ErrorCode),
        StandaloneDispatch.bSuccess);
    if (!StandaloneDispatch.bSuccess)
    {
        return false;
    }

    TestEqual(TEXT("Standalone clears Node.GetGroupName()"),
        SeqNode->Node.GetGroupName(), FName(NAME_None));
    TestEqual(TEXT("Standalone resets Node.GetGroupRole()"),
        (int32)SeqNode->Node.GetGroupRole(), (int32)EAnimGroupRole::CanBeLeader);
    TestTrue(TEXT("Standalone resets Node.GetGroupMethod()"),
        SeqNode->Node.GetGroupMethod() == EAnimSyncMethod::DoNotSync);
    return true;
}

// ============================================================================
// Regression (E-anim-node-name-substring-ambiguous): a `nodeName` substring that
// matches more than one AnimGraph node must NOT silently mutate an arbitrary one
// and report success — it must surface AMBIGUOUS_NODE with the candidate titles.
// Drives both the shared resolver (ResolveAnimGraphNodeByTitle) and the
// production set_sync_group handler with two unbound SequencePlayer nodes whose
// list-view titles are both exactly "Sequence Player" (literally
// indistinguishable by substring). Pre-fix: the handlers resolved a `nodeName`
// via a first-match substring scan that returned the first match, and
// set_sync_group reported success:true echoing the raw input substring; the
// fix routes them through ResolveAnimGraphNodeByTitle, which reports the
// multi-match instead of swallowing it. Reverting either the resolver or the handler wiring fails
// this test. Also asserts that an unambiguous resolve echoes the resolved node's
// ACTUAL list-view title in the response nodeName, not the raw input substring.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimAuthoringNodeNameAmbiguityTest,
    "PinWright.anim.authoring.NodeNameAmbiguity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimAuthoringNodeNameAmbiguityTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(LyraMannequinAnimBPPath);
    UAnimBlueprint* Fixture = LoadLyraMannequinAnimBP();
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), Fixture))
    {
        return false;
    }
    if (!TestNotNull(TEXT("fixture has TargetSkeleton"), Fixture->TargetSkeleton.Get()))
    {
        return false;
    }
    USkeleton* Skeleton = Fixture->TargetSkeleton;

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Path = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_NodeNameAmbiguity_%s"), *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(Path);
    };

    UAnimBlueprint* AnimBP = CreateFreshAnimBlueprint(Path, Skeleton);
    if (!TestNotNull(TEXT("AnimBlueprint created"), AnimBP))
    {
        return false;
    }

    UEdGraph* AnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
    TestNotNull(TEXT("AnimGraph present"), AnimGraph);
    if (!AnimGraph)
    {
        return false;
    }

    // Two unbound SequencePlayer nodes. A fresh, unbound SequencePlayer is titled
    // exactly "Sequence Player", so both carry that identical title — the literal
    // worst case the ticket describes.
    UAnimGraphNode_Base* SeqA = AnimGraphConstructionUtils::CreateAnimNode(
        AnimGraph, UAnimGraphNode_SequencePlayer::StaticClass(), FVector2D(0, 0));
    UAnimGraphNode_Base* SeqB = AnimGraphConstructionUtils::CreateAnimNode(
        AnimGraph, UAnimGraphNode_SequencePlayer::StaticClass(), FVector2D(0, 200));
    TestNotNull(TEXT("first SequencePlayer authored"), SeqA);
    TestNotNull(TEXT("second SequencePlayer authored"), SeqB);
    if (!SeqA || !SeqB)
    {
        return false;
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    const FString TitleA = SeqA->GetNodeTitle(ENodeTitleType::ListView).ToString();
    const FString TitleB = SeqB->GetNodeTitle(ENodeTitleType::ListView).ToString();

    // Two fresh, unbound SequencePlayer nodes are both titled exactly "Sequence
    // Player" — deterministic engine behavior — so both must share this substring.
    // A mismatch is a real failure, not a reason to skip.
    const TCHAR* SharedSubstring = TEXT("Sequence Player");
    if (!TestTrue(FString::Printf(
            TEXT("both fresh SequencePlayer titles ('%s' / '%s') contain '%s'"),
            *TitleA, *TitleB, SharedSubstring),
            TitleA.Contains(SharedSubstring) && TitleB.Contains(SharedSubstring)))
    {
        return false;
    }

    // --- Unit-level: the shared resolver reports Ambiguous with both candidates.
    const AnimGraphConstructionUtils::FAnimNodeResolveResult Ambiguous =
        AnimGraphConstructionUtils::ResolveAnimGraphNodeByTitle(AnimGraph, SharedSubstring);
    TestTrue(TEXT("resolver reports Ambiguous on a multi-match substring"),
        Ambiguous.Status == AnimGraphConstructionUtils::EAnimNodeResolveStatus::Ambiguous);
    TestNull(TEXT("ambiguous resolve returns no node"), Ambiguous.Node);
    TestEqual(TEXT("ambiguous resolve lists both candidates"), Ambiguous.Candidates.Num(), 2);

    // --- Handler-level: set_sync_group rejects the ambiguous nodeName.
    TSharedPtr<FJsonObject> AmbiguousPayload = MakeShared<FJsonObject>();
    AmbiguousPayload->SetStringField(TEXT("blueprintPath"), Path);
    AmbiguousPayload->SetStringField(TEXT("nodeName"), SharedSubstring);
    AmbiguousPayload->SetStringField(TEXT("groupName"), TEXT("Locomotion"));
    AmbiguousPayload->SetStringField(TEXT("role"), TEXT("AlwaysFollower"));
    AmbiguousPayload->SetBoolField(TEXT("save"), false);

    FAgirDispatchResult AmbiguousDispatch = DispatchAnimRpcViaDispatcher(
        TEXT("animation.authoring.set_sync_group"),
        TEXT("set-sync-group-ambiguous"),
        AmbiguousPayload);
    TestTrue(TEXT("ambiguous set_sync_group completion fired"), AmbiguousDispatch.bCompletionFired);
    TestFalse(TEXT("ambiguous set_sync_group does NOT report success"), AmbiguousDispatch.bSuccess);
    TestEqual(TEXT("ambiguous set_sync_group error is AMBIGUOUS_NODE"),
        AmbiguousDispatch.ErrorCode, FString(TEXT("AMBIGUOUS_NODE")));

    // The ambiguous mutation must NOT have silently touched either node.
    UAnimGraphNode_SequencePlayer* SeqAPlayer = Cast<UAnimGraphNode_SequencePlayer>(SeqA);
    UAnimGraphNode_SequencePlayer* SeqBPlayer = Cast<UAnimGraphNode_SequencePlayer>(SeqB);
    if (SeqAPlayer && SeqBPlayer)
    {
        TestEqual(TEXT("first node sync group untouched after ambiguous reject"),
            SeqAPlayer->Node.GetGroupName(), FName(NAME_None));
        TestEqual(TEXT("second node sync group untouched after ambiguous reject"),
            SeqBPlayer->Node.GetGroupName(), FName(NAME_None));
    }

    // --- Echo contract: an UNAMBIGUOUS resolve must echo the resolved node's
    // ACTUAL list-view title, not the raw input substring. To make this
    // load-bearing (input substring != resolved title), bind SeqA to a real
    // asset so its title grows to "Sequence Player '<asset>'", remove SeqB, then
    // resolve by the bare "Sequence Player" prefix — a strict substring of the
    // resolved title. Pre-fix the handler echoed the input prefix verbatim; the
    // fix echoes the full bound-asset title.
    AnimGraph->RemoveNode(SeqB);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    FString SequencePath;
    UAnimSequence* BoundSeq = FindCompatibleAnimSequenceForSkeleton(Skeleton, SequencePath);
    if (BoundSeq && SeqAPlayer)
    {
        // Bind via the production RPC so the title grows to "Sequence Player
        // '<asset>'". SeqB is gone, so the bare-title resolve is unambiguous here.
        TSharedPtr<FJsonObject> BindPayload = MakeShared<FJsonObject>();
        BindPayload->SetStringField(TEXT("blueprintPath"), Path);
        BindPayload->SetStringField(TEXT("nodeName"), TitleA);
        BindPayload->SetStringField(TEXT("assetPath"), SequencePath);
        BindPayload->SetBoolField(TEXT("save"), false);
        DispatchAnimRpcViaDispatcher(
            TEXT("animation.authoring.bind_player_asset"), TEXT("bind-for-echo"), BindPayload);

        const FString BoundTitle = SeqA->GetNodeTitle(ENodeTitleType::ListView).ToString();
        // Only meaningful if binding actually lengthened the title beyond the prefix.
        if (BoundTitle.Len() > FCString::Strlen(SharedSubstring) && BoundTitle.Contains(SharedSubstring))
        {
            const AnimGraphConstructionUtils::FAnimNodeResolveResult Solo =
                AnimGraphConstructionUtils::ResolveAnimGraphNodeByTitle(AnimGraph, SharedSubstring);
            TestTrue(TEXT("resolver reports Found once the match is unique"),
                Solo.Status == AnimGraphConstructionUtils::EAnimNodeResolveStatus::Found);
            TestTrue(TEXT("solo resolve returns the surviving node"), Solo.Node == SeqA);
            if (Solo.Candidates.Num() == 1)
            {
                TestEqual(TEXT("resolved candidate is the node's actual full title"),
                    Solo.Candidates[0], BoundTitle);
            }

            TSharedPtr<FJsonObject> SoloPayload = MakeShared<FJsonObject>();
            SoloPayload->SetStringField(TEXT("blueprintPath"), Path);
            SoloPayload->SetStringField(TEXT("nodeName"), SharedSubstring);
            SoloPayload->SetStringField(TEXT("groupName"), TEXT("Locomotion"));
            SoloPayload->SetStringField(TEXT("role"), TEXT("AlwaysFollower"));
            SoloPayload->SetBoolField(TEXT("save"), false);

            FAgirDispatchResult SoloDispatch = DispatchAnimRpcViaDispatcher(
                TEXT("animation.authoring.set_sync_group"),
                TEXT("set-sync-group-solo"),
                SoloPayload);
            TestTrue(FString::Printf(TEXT("solo set_sync_group success (errorCode='%s')"), *SoloDispatch.ErrorCode),
                SoloDispatch.bSuccess);
            if (SoloDispatch.bSuccess && SoloDispatch.Result.IsValid())
            {
                FString EchoedNodeName;
                TestTrue(TEXT("response carries nodeName"),
                    SoloDispatch.Result->TryGetStringField(TEXT("nodeName"), EchoedNodeName));
                // The load-bearing assertion: the echo is the resolved node's
                // full title, NOT the raw "Sequence Player" input substring.
                TestEqual(TEXT("response echoes the resolved node's full title, not the input substring"),
                    EchoedNodeName, BoundTitle);
                TestNotEqual(TEXT("echoed nodeName differs from the input substring"),
                    EchoedNodeName, FString(SharedSubstring));
            }
        }
    }
    return true;
}

// ============================================================================
// animation.authoring.add_two_bone_ik / add_modify_bone round-trip: typed
// skeletal-control helpers create real AnimGraph nodes and write the nested
// bone-reference/runtime fields directly.
// Counterfactual: if the production handlers or direct skeletal-control field
// writes are reverted, dispatch returns METHOD_NOT_FOUND or the created nodes
// keep default bone, mode, space, transform, or alpha values.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimAuthoringAddSkeletalControlIkHelpersTest,
    "PinWright.anim.authoring.AddSkeletalControlIkHelpers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimAuthoringAddSkeletalControlIkHelpersTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(LyraMannequinAnimBPPath);
    UAnimBlueprint* Fixture = LoadLyraMannequinAnimBP();
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), Fixture))
    {
        return false;
    }
    if (!TestNotNull(TEXT("fixture has TargetSkeleton"), Fixture->TargetSkeleton.Get()))
    {
        return false;
    }
    USkeleton* Skeleton = Fixture->TargetSkeleton;

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Path = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_SkeletalControlIk_%s"), *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(Path);
    };

    UAnimBlueprint* AnimBP = CreateFreshAnimBlueprint(Path, Skeleton);
    if (!TestNotNull(TEXT("AnimBlueprint created"), AnimBP))
    {
        return false;
    }

    TSharedPtr<FJsonObject> TwoBonePayload = MakeShared<FJsonObject>();
    TwoBonePayload->SetStringField(TEXT("blueprintPath"), Path);
    TwoBonePayload->SetStringField(TEXT("ikBone"), TEXT("foot_l"));
    TwoBonePayload->SetStringField(TEXT("effectorBone"), TEXT("foot_l"));
    TwoBonePayload->SetStringField(TEXT("jointTargetBone"), TEXT("calf_l"));
    TwoBonePayload->SetStringField(TEXT("effectorLocationSpace"), TEXT("BCS_BoneSpace"));
    TwoBonePayload->SetStringField(TEXT("jointTargetLocationSpace"), TEXT("BCS_ComponentSpace"));
    TwoBonePayload->SetNumberField(TEXT("x"), 200);
    TwoBonePayload->SetNumberField(TEXT("y"), 100);
    TwoBonePayload->SetNumberField(TEXT("alpha"), 0.75);
    TwoBonePayload->SetBoolField(TEXT("save"), false);

    FAgirDispatchResult TwoBoneDispatch = DispatchAnimRpcViaDispatcher(
        TEXT("animation.authoring.add_two_bone_ik"),
        TEXT("add-two-bone-ik-1"),
        TwoBonePayload);
    TestTrue(FString::Printf(TEXT("add_two_bone_ik success (errorCode='%s')"), *TwoBoneDispatch.ErrorCode),
        TwoBoneDispatch.bSuccess);
    if (!TwoBoneDispatch.bSuccess)
    {
        return false;
    }

    TSharedPtr<FJsonObject> ModifyPayload = MakeShared<FJsonObject>();
    ModifyPayload->SetStringField(TEXT("blueprintPath"), Path);
    ModifyPayload->SetStringField(TEXT("boneName"), TEXT("spine_01"));
    TSharedPtr<FJsonObject> Translation = MakeShared<FJsonObject>();
    Translation->SetNumberField(TEXT("x"), 1.0);
    Translation->SetNumberField(TEXT("y"), 2.0);
    Translation->SetNumberField(TEXT("z"), 3.0);
    ModifyPayload->SetObjectField(TEXT("translation"), Translation);
    TSharedPtr<FJsonObject> Rotation = MakeShared<FJsonObject>();
    Rotation->SetNumberField(TEXT("pitch"), 10.0);
    Rotation->SetNumberField(TEXT("yaw"), 20.0);
    Rotation->SetNumberField(TEXT("roll"), 30.0);
    ModifyPayload->SetObjectField(TEXT("rotation"), Rotation);
    TSharedPtr<FJsonObject> Scale = MakeShared<FJsonObject>();
    Scale->SetNumberField(TEXT("x"), 1.25);
    Scale->SetNumberField(TEXT("y"), 1.5);
    Scale->SetNumberField(TEXT("z"), 1.75);
    ModifyPayload->SetObjectField(TEXT("scale"), Scale);
    ModifyPayload->SetStringField(TEXT("translationSpace"), TEXT("BCS_ComponentSpace"));
    ModifyPayload->SetStringField(TEXT("rotationSpace"), TEXT("BCS_ParentBoneSpace"));
    ModifyPayload->SetStringField(TEXT("scaleSpace"), TEXT("BCS_BoneSpace"));
    ModifyPayload->SetStringField(TEXT("translationMode"), TEXT("Replace"));
    ModifyPayload->SetStringField(TEXT("rotationMode"), TEXT("Additive"));
    ModifyPayload->SetNumberField(TEXT("x"), 420);
    ModifyPayload->SetNumberField(TEXT("y"), 160);
    ModifyPayload->SetNumberField(TEXT("alpha"), 0.5);
    ModifyPayload->SetBoolField(TEXT("save"), false);

    FAgirDispatchResult ModifyDispatch = DispatchAnimRpcViaDispatcher(
        TEXT("animation.authoring.add_modify_bone"),
        TEXT("add-modify-bone-1"),
        ModifyPayload);
    TestTrue(FString::Printf(TEXT("add_modify_bone success (errorCode='%s')"), *ModifyDispatch.ErrorCode),
        ModifyDispatch.bSuccess);
    if (!ModifyDispatch.bSuccess)
    {
        return false;
    }

    UEdGraph* AnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
    TestNotNull(TEXT("AnimGraph present after skeletal-control dispatches"), AnimGraph);
    if (!AnimGraph)
    {
        return false;
    }

    int32 TwoBoneIkCount = 0;
    UAnimGraphNode_TwoBoneIK* TwoBoneNode = nullptr;
    int32 ModifyBoneCount = 0;
    UAnimGraphNode_ModifyBone* ModifyNode = nullptr;
    for (UEdGraphNode* Node : AnimGraph->Nodes)
    {
        if (UAnimGraphNode_TwoBoneIK* IK = Cast<UAnimGraphNode_TwoBoneIK>(Node))
        {
            ++TwoBoneIkCount;
            TwoBoneNode = IK;
        }
        if (UAnimGraphNode_ModifyBone* MB = Cast<UAnimGraphNode_ModifyBone>(Node))
        {
            ++ModifyBoneCount;
            ModifyNode = MB;
        }
    }

    TestEqual(TEXT("exactly one TwoBoneIK authored"), TwoBoneIkCount, 1);
    TestEqual(TEXT("exactly one ModifyBone authored"), ModifyBoneCount, 1);
    if (!TwoBoneNode || !ModifyNode)
    {
        return false;
    }

    TestEqual(TEXT("TwoBoneIK IKBone"), TwoBoneNode->Node.IKBone.BoneName, FName(TEXT("foot_l")));
    TestFalse(TEXT("TwoBoneIK effector targets a bone"), TwoBoneNode->Node.EffectorTarget.bUseSocket);
    TestEqual(TEXT("TwoBoneIK EffectorTarget bone"), TwoBoneNode->Node.EffectorTarget.BoneReference.BoneName, FName(TEXT("foot_l")));
    TestFalse(TEXT("TwoBoneIK joint target targets a bone"), TwoBoneNode->Node.JointTarget.bUseSocket);
    TestEqual(TEXT("TwoBoneIK JointTarget bone"), TwoBoneNode->Node.JointTarget.BoneReference.BoneName, FName(TEXT("calf_l")));
    TestEqual(TEXT("TwoBoneIK effector space"), TwoBoneNode->Node.EffectorLocationSpace.GetValue(), BCS_BoneSpace);
    TestEqual(TEXT("TwoBoneIK joint target space"), TwoBoneNode->Node.JointTargetLocationSpace.GetValue(), BCS_ComponentSpace);
    TestTrue(TEXT("TwoBoneIK alpha"), FMath::IsNearlyEqual(TwoBoneNode->Node.Alpha, 0.75f));

    TestEqual(TEXT("ModifyBone target bone"), ModifyNode->Node.BoneToModify.BoneName, FName(TEXT("spine_01")));
    TestEqual(TEXT("ModifyBone translation"), ModifyNode->Node.Translation, FVector(1.0, 2.0, 3.0));
    TestEqual(TEXT("ModifyBone rotation pitch"), ModifyNode->Node.Rotation.Pitch, 10.0);
    TestEqual(TEXT("ModifyBone rotation yaw"), ModifyNode->Node.Rotation.Yaw, 20.0);
    TestEqual(TEXT("ModifyBone rotation roll"), ModifyNode->Node.Rotation.Roll, 30.0);
    TestEqual(TEXT("ModifyBone scale"), ModifyNode->Node.Scale, FVector(1.25, 1.5, 1.75));
    TestEqual(TEXT("ModifyBone translation space"), ModifyNode->Node.TranslationSpace.GetValue(), BCS_ComponentSpace);
    TestEqual(TEXT("ModifyBone rotation space"), ModifyNode->Node.RotationSpace.GetValue(), BCS_ParentBoneSpace);
    TestEqual(TEXT("ModifyBone scale space"), ModifyNode->Node.ScaleSpace.GetValue(), BCS_BoneSpace);
    TestEqual(TEXT("ModifyBone translation mode alias"), ModifyNode->Node.TranslationMode.GetValue(), BMM_Replace);
    TestEqual(TEXT("ModifyBone rotation mode alias"), ModifyNode->Node.RotationMode.GetValue(), BMM_Additive);
    TestEqual(TEXT("ModifyBone scale mode defaults to Replace with scale payload"), ModifyNode->Node.ScaleMode.GetValue(), BMM_Replace);
    TestTrue(TEXT("ModifyBone alpha"), FMath::IsNearlyEqual(ModifyNode->Node.Alpha, 0.5f));

    TSharedPtr<FJsonObject> BadTwoBonePayload = MakeShared<FJsonObject>();
    BadTwoBonePayload->SetStringField(TEXT("blueprintPath"), Path);
    BadTwoBonePayload->SetStringField(TEXT("ikBone"), TEXT("not_a_real_bone"));
    BadTwoBonePayload->SetStringField(TEXT("effectorBone"), TEXT("foot_l"));
    BadTwoBonePayload->SetStringField(TEXT("jointTargetBone"), TEXT("calf_l"));
    BadTwoBonePayload->SetNumberField(TEXT("x"), 0);
    BadTwoBonePayload->SetNumberField(TEXT("y"), 0);
    BadTwoBonePayload->SetBoolField(TEXT("save"), false);
    FAgirDispatchResult BadTwoBoneDispatch = DispatchAnimRpcViaDispatcher(
        TEXT("animation.authoring.add_two_bone_ik"),
        TEXT("add-two-bone-ik-bad-bone"),
        BadTwoBonePayload);
    TestFalse(TEXT("bad TwoBoneIK bone rejected"), BadTwoBoneDispatch.bSuccess);
    TestEqual(TEXT("bad TwoBoneIK error code"), BadTwoBoneDispatch.ErrorCode, FString(TEXT("BONE_NOT_FOUND")));

    TSharedPtr<FJsonObject> BadModifyPayload = MakeShared<FJsonObject>();
    BadModifyPayload->SetStringField(TEXT("blueprintPath"), Path);
    BadModifyPayload->SetStringField(TEXT("boneName"), TEXT("not_a_real_bone"));
    BadModifyPayload->SetNumberField(TEXT("x"), 0);
    BadModifyPayload->SetNumberField(TEXT("y"), 0);
    BadModifyPayload->SetBoolField(TEXT("save"), false);
    FAgirDispatchResult BadModifyDispatch = DispatchAnimRpcViaDispatcher(
        TEXT("animation.authoring.add_modify_bone"),
        TEXT("add-modify-bone-bad-bone"),
        BadModifyPayload);
    TestFalse(TEXT("bad ModifyBone bone rejected"), BadModifyDispatch.bSuccess);
    TestEqual(TEXT("bad ModifyBone error code"), BadModifyDispatch.ErrorCode, FString(TEXT("BONE_NOT_FOUND")));
    return true;
}

// ============================================================================
// Regression (B-set-anim-graph-node-value-nested-fanimnode-field):
// animation.authoring.set_anim_graph_node_value must be able to mutate a field
// that lives on the wrapped runtime FAnimNode_* struct (under the node's public
// `Node` member), not just direct UPROPERTYs on the outer UAnimGraphNode_*.
// FAnimNode_TwoBoneIK::Alpha is the canonical case: add_two_bone_ik writes it at
// creation via Node->Node.Alpha, but it is NOT a direct property on
// UAnimGraphNode_TwoBoneIK, so the handler's flat
// FoundNode->GetClass()->FindPropertyByName(...) lookup misses it.
//
// Pre-fix the handler bailed with PROPERTY_NOT_FOUND for both "Alpha" and
// "Node.Alpha", making the generic post-hoc setter unable to re-set Alpha (there
// is no set_two_bone_ik mutator). The fix adds the same runtime-FAnimNode-field
// fallback the sibling add_graph_node already uses
// (AnimGraphConstructionUtils::ApplyJsonValueToAnimNodeFieldByName over
// ResolveAnimNodeFieldByName). This test creates a TwoBoneIK node with alpha 0.75,
// then re-sets Alpha to 0.8 via set_anim_graph_node_value and confirms the runtime
// field actually changed. Reverting the fallback makes the set dispatch fail with
// PROPERTY_NOT_FOUND and the runtime Alpha stay 0.75, failing this test.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimAuthoringSetGraphNodeValueNestedFieldTest,
    "PinWright.anim.authoring.SetGraphNodeValueNestedField",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimAuthoringSetGraphNodeValueNestedFieldTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(LyraMannequinAnimBPPath);
    UAnimBlueprint* Fixture = LoadLyraMannequinAnimBP();
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), Fixture))
    {
        return false;
    }
    if (!TestNotNull(TEXT("fixture has TargetSkeleton"), Fixture->TargetSkeleton.Get()))
    {
        return false;
    }
    USkeleton* Skeleton = Fixture->TargetSkeleton;

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Path = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_SetNodeValueNested_%s"), *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(Path);
    };

    UAnimBlueprint* AnimBP = CreateFreshAnimBlueprint(Path, Skeleton);
    if (!TestNotNull(TEXT("AnimBlueprint created"), AnimBP))
    {
        return false;
    }

    // Create a TwoBoneIK node with a known non-default alpha (0.75) at creation.
    TSharedPtr<FJsonObject> TwoBonePayload = MakeShared<FJsonObject>();
    TwoBonePayload->SetStringField(TEXT("blueprintPath"), Path);
    TwoBonePayload->SetStringField(TEXT("ikBone"), TEXT("foot_l"));
    TwoBonePayload->SetStringField(TEXT("effectorBone"), TEXT("foot_l"));
    TwoBonePayload->SetStringField(TEXT("jointTargetBone"), TEXT("calf_l"));
    TwoBonePayload->SetNumberField(TEXT("x"), 200);
    TwoBonePayload->SetNumberField(TEXT("y"), 100);
    TwoBonePayload->SetNumberField(TEXT("alpha"), 0.75);
    TwoBonePayload->SetBoolField(TEXT("save"), false);

    FAgirDispatchResult CreateDispatch = DispatchAnimRpcViaDispatcher(
        TEXT("animation.authoring.add_two_bone_ik"),
        TEXT("set-nested-create-two-bone-ik"),
        TwoBonePayload);
    TestTrue(FString::Printf(TEXT("add_two_bone_ik success (errorCode='%s')"), *CreateDispatch.ErrorCode),
        CreateDispatch.bSuccess);
    if (!CreateDispatch.bSuccess)
    {
        return false;
    }

    // Resolve the TwoBoneIK node and confirm the create-time alpha landed.
    UEdGraph* AnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
    TestNotNull(TEXT("AnimGraph present after add_two_bone_ik"), AnimGraph);
    if (!AnimGraph)
    {
        return false;
    }

    UAnimGraphNode_TwoBoneIK* TwoBoneNode = nullptr;
    for (UEdGraphNode* Node : AnimGraph->Nodes)
    {
        if (UAnimGraphNode_TwoBoneIK* IK = Cast<UAnimGraphNode_TwoBoneIK>(Node))
        {
            TwoBoneNode = IK;
            break;
        }
    }
    TestNotNull(TEXT("TwoBoneIK node authored"), TwoBoneNode);
    if (!TwoBoneNode)
    {
        return false;
    }
    TestTrue(TEXT("create-time alpha is 0.75"),
        FMath::IsNearlyEqual(TwoBoneNode->Node.Alpha, 0.75f));

    // The node title contains the IK bone name ("foot_l"); set_anim_graph_node_value
    // matches nodeName as a substring of the title. Alpha is NOT a direct property
    // on UAnimGraphNode_TwoBoneIK — it lives on the FAnimNode_TwoBoneIK struct under
    // the `Node` member, so this only succeeds via the nested-FAnimNode fallback.
    TSharedPtr<FJsonObject> SetPayload = MakeShared<FJsonObject>();
    SetPayload->SetStringField(TEXT("blueprintPath"), Path);
    SetPayload->SetStringField(TEXT("nodeName"), TEXT("foot_l"));
    SetPayload->SetStringField(TEXT("propertyName"), TEXT("Alpha"));
    SetPayload->SetStringField(TEXT("value"), TEXT("0.8"));
    SetPayload->SetBoolField(TEXT("save"), false);

    FAgirDispatchResult SetDispatch = DispatchAnimRpcViaDispatcher(
        TEXT("animation.authoring.set_anim_graph_node_value"),
        TEXT("set-nested-alpha"),
        SetPayload);

    // The load-bearing assertion: pre-fix this returned PROPERTY_NOT_FOUND because
    // the handler only looked up Alpha on the outer node class.
    TestTrue(FString::Printf(
        TEXT("set_anim_graph_node_value reaches nested FAnimNode field 'Alpha' (errorCode='%s')"),
        *SetDispatch.ErrorCode), SetDispatch.bSuccess);
    if (!SetDispatch.bSuccess)
    {
        TestNotEqual(TEXT("failure is not the pre-fix PROPERTY_NOT_FOUND"),
            SetDispatch.ErrorCode, FString(TEXT("PROPERTY_NOT_FOUND")));
        return false;
    }

    // The runtime field actually changed — the value was applied, not just accepted.
    TestTrue(TEXT("runtime Alpha updated to 0.8 via the nested-field fallback"),
        FMath::IsNearlyEqual(TwoBoneNode->Node.Alpha, 0.8f));

    return true;
}

// ============================================================================
// animation.authoring.add_graph_node round-trip: generic typed creator spawns
// a UAnimGraphNode_SequencePlayer at (100, 200) on the AnimGraph with typed
// syncGroup/syncRole properties, exposed PlayRate, and a bound sequence asset.
// Counterfactual: if the runtime-node property fallback is reverted, PlayRate
// lands in propertiesFailed and the runtime play rate stays 1.0.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimAuthoringAddGraphNodeGenericTest,
    "PinWright.anim.authoring.AddGraphNodeGeneric",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimAuthoringAddGraphNodeGenericTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(LyraMannequinAnimBPPath);
    UAnimBlueprint* Fixture = LoadLyraMannequinAnimBP();
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), Fixture))
    {
        return false;
    }
    if (!TestNotNull(TEXT("fixture has TargetSkeleton"), Fixture->TargetSkeleton.Get()))
    {
        return false;
    }
    USkeleton* Skeleton = Fixture->TargetSkeleton;

    FString SequencePath;
    UAnimSequence* LoadedSequence = FindCompatibleAnimSequenceForSkeleton(Skeleton, SequencePath);
    if (!TestNotNull(TEXT("a compatible UAnimSequence is present for the skeleton"), LoadedSequence))
    {
        return false;
    }

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Path = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_AddGraphNodeGeneric_%s"), *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(Path);
    };

    UAnimBlueprint* AnimBP = CreateFreshAnimBlueprint(Path, Skeleton);
    if (!TestNotNull(TEXT("AnimBlueprint created"), AnimBP))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), Path);
    Payload->SetStringField(TEXT("nodeClass"), TEXT("AnimGraphNode_SequencePlayer"));
    Payload->SetNumberField(TEXT("x"), 100);
    Payload->SetNumberField(TEXT("y"), 200);
    TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetStringField(TEXT("syncGroup"), TEXT("Locomotion"));
    Properties->SetStringField(TEXT("syncRole"), TEXT("AlwaysLeader"));
    Properties->SetNumberField(TEXT("PlayRate"), 1.25);
    Payload->SetObjectField(TEXT("properties"), Properties);
    TArray<TSharedPtr<FJsonValue>> ExposePins;
    ExposePins.Add(MakeShared<FJsonValueString>(TEXT("PlayRate")));
    Payload->SetArrayField(TEXT("exposePins"), ExposePins);
    Payload->SetStringField(TEXT("bindAsset"), SequencePath);
    Payload->SetBoolField(TEXT("save"), false);

    FAgirDispatchResult Dispatch = DispatchAnimRpcViaDispatcher(
        TEXT("animation.authoring.add_graph_node"),
        TEXT("add-graph-node-generic-1"),
        Payload);
    TestTrue(FString::Printf(TEXT("add_graph_node success (errorCode='%s')"), *Dispatch.ErrorCode),
        Dispatch.bSuccess);
    if (!Dispatch.bSuccess)
    {
        return false;
    }
    TestTrue(TEXT("add_graph_node response object is valid"), Dispatch.Result.IsValid());
    if (!Dispatch.Result.IsValid())
    {
        return false;
    }

    auto StringArrayContains = [](const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, const FString& Expected)
    {
        const TArray<TSharedPtr<FJsonValue>>* ArrayPtr = nullptr;
        if (!Obj.IsValid() || !Obj->TryGetArrayField(FieldName, ArrayPtr) || !ArrayPtr)
        {
            return false;
        }
        for (const TSharedPtr<FJsonValue>& Value : *ArrayPtr)
        {
            FString Actual;
            if (Value.IsValid() && Value->TryGetString(Actual) && Actual == Expected)
            {
                return true;
            }
        }
        return false;
    };

    auto ObjectArrayContainsName = [](const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, const FString& ExpectedName)
    {
        const TArray<TSharedPtr<FJsonValue>>* ArrayPtr = nullptr;
        if (!Obj.IsValid() || !Obj->TryGetArrayField(FieldName, ArrayPtr) || !ArrayPtr)
        {
            return false;
        }
        for (const TSharedPtr<FJsonValue>& Value : *ArrayPtr)
        {
            const TSharedPtr<FJsonObject>* Entry = nullptr;
            FString ActualName;
            if (Value.IsValid() && Value->TryGetObject(Entry) && Entry && Entry->IsValid() &&
                (*Entry)->TryGetStringField(TEXT("name"), ActualName) && ActualName == ExpectedName)
            {
                return true;
            }
        }
        return false;
    };

    TestTrue(TEXT("propertiesApplied contains PlayRate"),
        StringArrayContains(Dispatch.Result, TEXT("propertiesApplied"), TEXT("PlayRate")));
    TestFalse(TEXT("propertiesFailed does not contain PlayRate"),
        ObjectArrayContainsName(Dispatch.Result, TEXT("propertiesFailed"), TEXT("PlayRate")));
    TestTrue(TEXT("pinsExposed contains PlayRate"),
        StringArrayContains(Dispatch.Result, TEXT("pinsExposed"), TEXT("PlayRate")));
    bool bAssetBound = false;
    TestTrue(TEXT("response contains assetBound"),
        Dispatch.Result->TryGetBoolField(TEXT("assetBound"), bAssetBound));
    TestTrue(TEXT("assetBound true"), bAssetBound);

    UEdGraph* AnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
    TestNotNull(TEXT("AnimGraph present after dispatch"), AnimGraph);
    if (!AnimGraph)
    {
        return false;
    }

    int32 SeqPlayerCount = 0;
    UAnimGraphNode_SequencePlayer* Created = nullptr;
    for (UEdGraphNode* Node : AnimGraph->Nodes)
    {
        if (UAnimGraphNode_SequencePlayer* SP = Cast<UAnimGraphNode_SequencePlayer>(Node))
        {
            ++SeqPlayerCount;
            Created = SP;
        }
    }
    TestEqual(TEXT("exactly one SequencePlayer authored"), SeqPlayerCount, 1);
    if (!Created)
    {
        return false;
    }
    TestEqual(TEXT("NodePosX == 100"), Created->NodePosX, 100);
    TestEqual(TEXT("NodePosY == 200"), Created->NodePosY, 200);
    TestEqual(TEXT("add_graph_node syncGroup applied"),
        Created->Node.GetGroupName(), FName(TEXT("Locomotion")));
    TestEqual(TEXT("add_graph_node syncRole applied"),
        (int32)Created->Node.GetGroupRole(), (int32)EAnimGroupRole::AlwaysLeader);
    TestTrue(TEXT("Node.GetPlayRate() ~= 1.25"),
        FMath::IsNearlyEqual(Created->Node.GetPlayRate(), 1.25f));
    TestEqual(TEXT("Node.GetSequence() equals the bound sequence"),
        Created->Node.GetSequence(), static_cast<UAnimSequenceBase*>(LoadedSequence));
    return true;
}

// ============================================================================
// animation.authoring.set_layered_blend_layers round-trip: authors a
// UAnimGraphNode_LayeredBoneBlend, then writes a 2-layer mask (spine_01/-1 +
// clavicle_l/0; neck_01/1) and asserts LayerSetup is rebuilt accordingly.
// Counterfactual: if set_layered_blend_layers is reverted, Node->Node.LayerSetup.Num()
// stays at 1 (the default ctor's AddFirstPose entry) with empty BranchFilters —
// the 2-entry assertion fails.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimAuthoringSetLayeredBlendLayersTest,
    "PinWright.anim.authoring.SetLayeredBlendLayers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimAuthoringSetLayeredBlendLayersTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(LyraMannequinAnimBPPath);
    UAnimBlueprint* Fixture = LoadLyraMannequinAnimBP();
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), Fixture))
    {
        return false;
    }
    if (!TestNotNull(TEXT("fixture has TargetSkeleton"), Fixture->TargetSkeleton.Get()))
    {
        return false;
    }
    USkeleton* Skeleton = Fixture->TargetSkeleton;

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Path = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_LayeredBlend_%s"), *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(Path);
    };

    UAnimBlueprint* AnimBP = CreateFreshAnimBlueprint(Path, Skeleton);
    if (!TestNotNull(TEXT("AnimBlueprint created"), AnimBP))
    {
        return false;
    }

    UEdGraph* AnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
    TestNotNull(TEXT("AnimGraph present"), AnimGraph);
    if (!AnimGraph)
    {
        return false;
    }

    UAnimGraphNode_Base* CreatedNode = AnimGraphConstructionUtils::CreateAnimNode(
        AnimGraph, UAnimGraphNode_LayeredBoneBlend::StaticClass(), FVector2D::ZeroVector);
    UAnimGraphNode_LayeredBoneBlend* LayeredNode = Cast<UAnimGraphNode_LayeredBoneBlend>(CreatedNode);
    TestNotNull(TEXT("LayeredBoneBlend node authored"), LayeredNode);
    if (!LayeredNode)
    {
        return false;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    const FString NodeTitle = LayeredNode->GetNodeTitle(ENodeTitleType::ListView).ToString();

    // Build the two-layer payload.
    auto MakeFilter = [](const TCHAR* BoneName, int32 Depth) -> TSharedPtr<FJsonValue>
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("boneName"), BoneName);
        Obj->SetNumberField(TEXT("blendDepth"), Depth);
        return MakeShared<FJsonValueObject>(Obj);
    };

    TArray<TSharedPtr<FJsonValue>> Layer0Filters;
    Layer0Filters.Add(MakeFilter(TEXT("spine_01"), -1));
    Layer0Filters.Add(MakeFilter(TEXT("clavicle_l"), 0));
    TSharedPtr<FJsonObject> Layer0 = MakeShared<FJsonObject>();
    Layer0->SetArrayField(TEXT("branchFilters"), Layer0Filters);

    TArray<TSharedPtr<FJsonValue>> Layer1Filters;
    Layer1Filters.Add(MakeFilter(TEXT("neck_01"), 1));
    TSharedPtr<FJsonObject> Layer1 = MakeShared<FJsonObject>();
    Layer1->SetArrayField(TEXT("branchFilters"), Layer1Filters);

    TArray<TSharedPtr<FJsonValue>> Layers;
    Layers.Add(MakeShared<FJsonValueObject>(Layer0));
    Layers.Add(MakeShared<FJsonValueObject>(Layer1));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), Path);
    Payload->SetStringField(TEXT("nodeName"), NodeTitle);
    Payload->SetArrayField(TEXT("layers"), Layers);
    Payload->SetBoolField(TEXT("save"), false);

    FAgirDispatchResult Dispatch = DispatchAnimRpcViaDispatcher(
        TEXT("animation.authoring.set_layered_blend_layers"),
        TEXT("set-layered-blend-layers-1"),
        Payload);
    TestTrue(FString::Printf(TEXT("set_layered_blend_layers success (errorCode='%s')"), *Dispatch.ErrorCode),
        Dispatch.bSuccess);
    if (!Dispatch.bSuccess)
    {
        return false;
    }

    TestEqual(TEXT("LayerSetup has 2 entries"), LayeredNode->Node.LayerSetup.Num(), 2);
    TestEqual(TEXT("BlendPoses has 2 entries"), LayeredNode->Node.BlendPoses.Num(), 2);
    TestEqual(TEXT("BlendWeights has 2 entries"), LayeredNode->Node.BlendWeights.Num(), 2);
    if (LayeredNode->Node.LayerSetup.Num() < 2)
    {
        return false;
    }

    const FInputBlendPose& L0 = LayeredNode->Node.LayerSetup[0];
    TestEqual(TEXT("Layer[0] has 2 branch filters"), L0.BranchFilters.Num(), 2);
    if (L0.BranchFilters.Num() >= 2)
    {
        TestEqual(TEXT("Layer[0][0].BoneName == spine_01"),
            L0.BranchFilters[0].BoneName, FName(TEXT("spine_01")));
        TestEqual(TEXT("Layer[0][0].BlendDepth == -1"),
            L0.BranchFilters[0].BlendDepth, -1);
        TestEqual(TEXT("Layer[0][1].BoneName == clavicle_l"),
            L0.BranchFilters[1].BoneName, FName(TEXT("clavicle_l")));
        TestEqual(TEXT("Layer[0][1].BlendDepth == 0"),
            L0.BranchFilters[1].BlendDepth, 0);
    }

    const FInputBlendPose& L1 = LayeredNode->Node.LayerSetup[1];
    TestEqual(TEXT("Layer[1] has 1 branch filter"), L1.BranchFilters.Num(), 1);
    if (L1.BranchFilters.Num() >= 1)
    {
        TestEqual(TEXT("Layer[1][0].BoneName == neck_01"),
            L1.BranchFilters[0].BoneName, FName(TEXT("neck_01")));
        TestEqual(TEXT("Layer[1][0].BlendDepth == 1"),
            L1.BranchFilters[0].BlendDepth, 1);
    }
    return true;
}

// ============================================================================
// State-machine internals: set_state_machine_entry / set_transition_settings /
// add_state_alias round-trip via dispatcher.
// Counterfactual: if any of the three new handlers is reverted, dispatcher
// returns METHOD_NOT_FOUND for that subsection and TestTrue(bSuccess) for that
// branch fails.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimAuthoringStateMachineInternalsTest,
    "PinWright.anim.authoring.StateMachineInternals",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimAuthoringStateMachineInternalsTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(LyraMannequinAnimBPPath);
    UAnimBlueprint* Fixture = LoadLyraMannequinAnimBP();
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), Fixture))
    {
        return false;
    }
    if (!TestNotNull(TEXT("fixture has TargetSkeleton"), Fixture->TargetSkeleton.Get()))
    {
        return false;
    }
    USkeleton* Skeleton = Fixture->TargetSkeleton;

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Path = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_SMInternals_%s"), *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(Path);
    };

    UAnimBlueprint* AnimBP = CreateFreshAnimBlueprint(Path, Skeleton);
    if (!TestNotNull(TEXT("AnimBlueprint created"), AnimBP))
    {
        return false;
    }

    // Build topology via dispatched RPCs to exercise the full surface.
    auto MakePayload = [&Path]() -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("blueprintPath"), Path);
        P->SetBoolField(TEXT("save"), false);
        return P;
    };

    {
        TSharedPtr<FJsonObject> P = MakePayload();
        P->SetStringField(TEXT("stateMachineName"), TEXT("SM"));
        P->SetNumberField(TEXT("x"), 0);
        P->SetNumberField(TEXT("y"), 0);
        FAgirDispatchResult R = DispatchAnimRpcViaDispatcher(
            TEXT("animation.authoring.add_state_machine"), TEXT("sm-internals-add-sm"), P);
        TestTrue(FString::Printf(TEXT("add_state_machine success (errorCode='%s')"), *R.ErrorCode), R.bSuccess);
        if (!R.bSuccess) return false;
    }
    for (const TCHAR* StateName : {TEXT("Idle"), TEXT("Run")})
    {
        TSharedPtr<FJsonObject> P = MakePayload();
        P->SetStringField(TEXT("stateMachineName"), TEXT("SM"));
        P->SetStringField(TEXT("stateName"), StateName);
        P->SetNumberField(TEXT("x"), FCString::Strcmp(StateName, TEXT("Idle")) == 0 ? 100 : 300);
        P->SetNumberField(TEXT("y"), 0);
        FAgirDispatchResult R = DispatchAnimRpcViaDispatcher(
            TEXT("animation.authoring.add_state"), TEXT("sm-internals-add-state"), P);
        TestTrue(FString::Printf(TEXT("add_state(%s) success (errorCode='%s')"), StateName, *R.ErrorCode), R.bSuccess);
        if (!R.bSuccess) return false;
    }
    {
        TSharedPtr<FJsonObject> P = MakePayload();
        P->SetStringField(TEXT("stateMachineName"), TEXT("SM"));
        P->SetStringField(TEXT("fromState"), TEXT("Idle"));
        P->SetStringField(TEXT("toState"), TEXT("Run"));
        FAgirDispatchResult R = DispatchAnimRpcViaDispatcher(
            TEXT("animation.authoring.add_transition"), TEXT("sm-internals-add-trans"), P);
        TestTrue(FString::Printf(TEXT("add_transition success (errorCode='%s')"), *R.ErrorCode), R.bSuccess);
        if (!R.bSuccess) return false;
    }

    // Locate the SM graph for direct-assertion access.
    UEdGraph* AnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
    TestNotNull(TEXT("AnimGraph present"), AnimGraph);
    if (!AnimGraph) return false;
    UAnimGraphNode_StateMachine* SMNode = AnimGraphConstructionUtils::FindStateMachineNode(AnimGraph, TEXT("SM"));
    TestNotNull(TEXT("SM node present"), SMNode);
    if (!SMNode || !SMNode->EditorStateMachineGraph) return false;
    UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
    TestNotNull(TEXT("SM graph castable"), SMGraph);
    if (!SMGraph) return false;

    // Both anim_graph.json readback guards below (transition logic/blend in
    // sub-assertion 1, entry_state in sub-assertion 3) navigate to the first
    // state_machines[] object of a freshly built dump before asserting their own
    // field. A fresh dump is rebuilt per call on purpose: set_state_machine_entry
    // mutates the graph between the guards, so each needs its own dump. Returns
    // null (having failed the test) when the dump or state_machines[0] is absent.
    auto FirstStateMachineDumpObj = [&]() -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Dump = AnimGraphDumpBuilder::BuildAnimGraphJson(AnimBP);
        TestNotNull(TEXT("BuildAnimGraphJson returned an object"), Dump.Get());
        if (!Dump.IsValid())
        {
            return nullptr;
        }
        const TArray<TSharedPtr<FJsonValue>>* Machines = nullptr;
        if (!Dump->TryGetArrayField(TEXT("state_machines"), Machines) || !Machines || Machines->Num() == 0)
        {
            TestTrue(TEXT("dump state_machines[] is non-empty"), false);
            return nullptr;
        }
        const TSharedPtr<FJsonObject> MObj = (*Machines)[0]->AsObject();
        TestNotNull(TEXT("state_machines[0] is an object"), MObj.Get());
        return MObj;
    };

    // Sub-assertion 1: set_transition_settings.
    {
        TSharedPtr<FJsonObject> P = MakePayload();
        P->SetStringField(TEXT("stateMachineName"), TEXT("SM"));
        P->SetStringField(TEXT("fromState"), TEXT("Idle"));
        P->SetStringField(TEXT("toState"), TEXT("Run"));
        P->SetStringField(TEXT("logicType"), TEXT("Inertialization"));
        P->SetStringField(TEXT("blendMode"), TEXT("Cubic"));
        P->SetBoolField(TEXT("disabled"), true);
        FAgirDispatchResult R = DispatchAnimRpcViaDispatcher(
            TEXT("animation.authoring.set_transition_settings"), TEXT("sm-internals-set-ts"), P);
        TestTrue(FString::Printf(TEXT("set_transition_settings success (errorCode='%s')"), *R.ErrorCode), R.bSuccess);
        if (!R.bSuccess) return false;

        UAnimStateTransitionNode* TransNode = nullptr;
        for (UEdGraphNode* Node : SMGraph->Nodes)
        {
            if (UAnimStateTransitionNode* Trans = Cast<UAnimStateTransitionNode>(Node))
            {
                UAnimStateNodeBase* Prev = Trans->GetPreviousState();
                UAnimStateNodeBase* Next = Trans->GetNextState();
                if (Prev && Next && Prev->GetStateName() == TEXT("Idle") && Next->GetStateName() == TEXT("Run"))
                {
                    TransNode = Trans;
                    break;
                }
            }
        }
        TestNotNull(TEXT("Transition node located"), TransNode);
        if (!TransNode) return false;
        TestEqual(TEXT("LogicType == Inertialization"),
            static_cast<int32>(TransNode->LogicType.GetValue()),
            static_cast<int32>(ETransitionLogicType::TLT_Inertialization));
        TestEqual(TEXT("BlendMode == Cubic"),
            static_cast<int32>(TransNode->BlendMode),
            static_cast<int32>(EAlphaBlendOption::Cubic));
        // bDisabled was added to UAnimStateTransitionNode in UE 5.6 (present in 5.6 and 5.7).
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        TestTrue(TEXT("bDisabled == true"), TransNode->bDisabled);
#endif

        // Regression guard for E-anim-graph-json-omits-transition-logic-blend:
        // the anim_graph.json dump must surface the LogicType/BlendMode that
        // set_transition_settings just wrote, as the same string vocabulary the
        // RPC accepts. Exercises the production dump builder directly. Before the
        // fix, transitions[] carried only from/to/priority/rule_graph/bidirectional/
        // disabled and these TestEqual assertions on logicType/blendMode would fail
        // (the fields were absent → TryGetStringField returns false / empty).
        {
            TSharedPtr<FJsonObject> MObj = FirstStateMachineDumpObj();
            if (MObj.IsValid())
            {
                // The fixture builds exactly one state machine; run a single loop
                // over its transitions to locate Idle->Run.
                TSharedPtr<FJsonObject> DumpedTrans;
                const TArray<TSharedPtr<FJsonValue>>* Transitions = nullptr;
                if (MObj->TryGetArrayField(TEXT("transitions"), Transitions) && Transitions)
                {
                    for (const TSharedPtr<FJsonValue>& TV : *Transitions)
                    {
                        const TSharedPtr<FJsonObject> TObj = TV->AsObject();
                        if (TObj.IsValid() &&
                            TObj->GetStringField(TEXT("from")) == TEXT("Idle") &&
                            TObj->GetStringField(TEXT("to")) == TEXT("Run"))
                        {
                            DumpedTrans = TObj;
                            break;
                        }
                    }
                }

                TestNotNull(TEXT("dump contains the Idle->Run transition"), DumpedTrans.Get());
                if (DumpedTrans.IsValid())
                {
                    TestEqual(TEXT("dump logic_type == Inertialization"),
                        DumpedTrans->GetStringField(TEXT("logic_type")), FString(TEXT("Inertialization")));
                    TestEqual(TEXT("dump blend_mode == Cubic"),
                        DumpedTrans->GetStringField(TEXT("blend_mode")), FString(TEXT("Cubic")));
                }
            }
        }
    }

    // Sub-assertion 2: add_state_alias.
    {
        TArray<TSharedPtr<FJsonValue>> Aliases;
        Aliases.Add(MakeShared<FJsonValueString>(TEXT("Idle")));
        TSharedPtr<FJsonObject> P = MakePayload();
        P->SetStringField(TEXT("stateMachineName"), TEXT("SM"));
        P->SetStringField(TEXT("aliasName"), TEXT("Alias_Idle"));
        P->SetArrayField(TEXT("aliases"), Aliases);
        P->SetNumberField(TEXT("x"), 200);
        P->SetNumberField(TEXT("y"), 100);
        FAgirDispatchResult R = DispatchAnimRpcViaDispatcher(
            TEXT("animation.authoring.add_state_alias"), TEXT("sm-internals-add-alias"), P);
        TestTrue(FString::Printf(TEXT("add_state_alias success (errorCode='%s')"), *R.ErrorCode), R.bSuccess);
        if (!R.bSuccess) return false;

        UAnimStateAliasNode* AliasNode = nullptr;
        for (UEdGraphNode* Node : SMGraph->Nodes)
        {
            if (UAnimStateAliasNode* Alias = Cast<UAnimStateAliasNode>(Node))
            {
                if (Alias->GetStateName() == TEXT("Alias_Idle"))
                {
                    AliasNode = Alias;
                    break;
                }
            }
        }
        TestNotNull(TEXT("Alias node located"), AliasNode);
        if (!AliasNode) return false;
        TestEqual(TEXT("StateAliasName == Alias_Idle"),
            AliasNode->GetStateName(), FString(TEXT("Alias_Idle")));
        TestFalse(TEXT("bGlobalAlias == false"), AliasNode->bGlobalAlias);
        TestEqual(TEXT("Aliased states count == 1"),
            AliasNode->GetAliasedStates().Num(), 1);
        UAnimStateNode* IdleState = AnimGraphConstructionUtils::FindStateNode(SMGraph, TEXT("Idle"));
        if (AliasNode->GetAliasedStates().Num() == 1 && IdleState)
        {
            const TWeakObjectPtr<UAnimStateNodeBase>& Weak = *AliasNode->GetAliasedStates().CreateIterator();
            TestEqual(TEXT("Alias points at Idle state"),
                Weak.Get(), static_cast<UAnimStateNodeBase*>(IdleState));
        }
    }

    // Sub-assertion 3: set_state_machine_entry.
    {
        TSharedPtr<FJsonObject> P = MakePayload();
        P->SetStringField(TEXT("stateMachineName"), TEXT("SM"));
        P->SetStringField(TEXT("stateName"), TEXT("Run"));
        FAgirDispatchResult R = DispatchAnimRpcViaDispatcher(
            TEXT("animation.authoring.set_state_machine_entry"), TEXT("sm-internals-set-entry"), P);
        TestTrue(FString::Printf(TEXT("set_state_machine_entry success (errorCode='%s')"), *R.ErrorCode), R.bSuccess);
        if (!R.bSuccess) return false;

        UAnimStateEntryNode* EntryNode = SMGraph->EntryNode.Get();
        TestNotNull(TEXT("EntryNode present"), EntryNode);
        if (!EntryNode || EntryNode->Pins.Num() == 0) return false;
        UEdGraphPin* EntryPin = EntryNode->Pins[0];
        TestEqual(TEXT("EntryPin has 1 linked connection"), EntryPin->LinkedTo.Num(), 1);
        UAnimStateNode* RunState = AnimGraphConstructionUtils::FindStateNode(SMGraph, TEXT("Run"));
        TestNotNull(TEXT("Run state located"), RunState);
        if (EntryPin->LinkedTo.Num() == 1 && RunState)
        {
            TestEqual(TEXT("Entry connects to Run state"),
                EntryPin->LinkedTo[0]->GetOwningNode(),
                static_cast<UEdGraphNode*>(RunState));
        }

        // Regression guard for E-anim-graph-omits-state-machine-entry-state:
        // after set_state_machine_entry rewired the entry to "Run", the
        // anim_graph.json dump must report which state is the entry so the
        // structured readback can observe the rewire (previously the readback was
        // byte-identical whether the entry was Idle or Run). Exercises the
        // production dump builder directly. Before the fix, state_machines[]
        // carried only name/page/states/transitions/conduits and had no
        // entry_state field, so TryGetStringField returns false here and this
        // fails.
        {
            TSharedPtr<FJsonObject> MObj = FirstStateMachineDumpObj();
            if (MObj.IsValid())
            {
                FString EntryState;
                const bool bHasEntry = MObj->TryGetStringField(TEXT("entry_state"), EntryState);
                TestTrue(TEXT("state_machines[] carries an entry_state field"), bHasEntry);
                TestEqual(TEXT("dump entry_state == Run"), EntryState, FString(TEXT("Run")));
            }
        }
    }
    return true;
}

// ============================================================================
// animation.list_graph_nodes / animation.search_graph_nodes catalog discovery.
// Verifies that the live TObjectIterator walk surfaces well-known UAnimGraphNode_*
// classes with the expected runtimeNode + requiredAssets metadata, and that the
// search handler ranks substring hits and rejects an empty query.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimSearchGraphNodesCatalogTest,
    "PinWright.anim.discovery.SearchGraphNodesCatalog",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimSearchGraphNodesCatalogTest::RunTest(const FString& Parameters)
{
    // 1. animation.list_graph_nodes — empty payload, full catalog.
    {
        FAgirDispatchResult Dispatch = DispatchAnimRpcViaDispatcher(
            TEXT("animation.list_graph_nodes"), TEXT("anim-list-graph-nodes"),
            MakeShared<FJsonObject>());
        TestTrue(FString::Printf(TEXT("list success (errorCode='%s')"), *Dispatch.ErrorCode),
            Dispatch.bSuccess);
        if (!Dispatch.bSuccess || !Dispatch.Result.IsValid()) return false;

        const TArray<TSharedPtr<FJsonValue>>* ResultsPtr = nullptr;
        TestTrue(TEXT("list result has results array"),
            Dispatch.Result->TryGetArrayField(TEXT("results"), ResultsPtr));
        if (!ResultsPtr) return false;

        TSet<FString> ClassNames;
        TSharedPtr<FJsonObject> SequencePlayerEntry;
        for (const TSharedPtr<FJsonValue>& V : *ResultsPtr)
        {
            const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
            if (!V->TryGetObject(ObjPtr) || !ObjPtr) continue;
            FString CN;
            if ((*ObjPtr)->TryGetStringField(TEXT("className"), CN))
            {
                ClassNames.Add(CN);
                if (CN == TEXT("AnimGraphNode_SequencePlayer"))
                {
                    SequencePlayerEntry = *ObjPtr;
                }
            }
        }

        TestTrue(TEXT("catalog contains AnimGraphNode_SequencePlayer"),
            ClassNames.Contains(TEXT("AnimGraphNode_SequencePlayer")));
        TestTrue(TEXT("catalog contains AnimGraphNode_StateMachine"),
            ClassNames.Contains(TEXT("AnimGraphNode_StateMachine")));
        TestTrue(TEXT("catalog contains AnimGraphNode_LayeredBoneBlend"),
            ClassNames.Contains(TEXT("AnimGraphNode_LayeredBoneBlend")));

        TestTrue(TEXT("SequencePlayer entry located"), SequencePlayerEntry.IsValid());
        if (SequencePlayerEntry.IsValid())
        {
            FString RuntimeNode;
            SequencePlayerEntry->TryGetStringField(TEXT("runtimeNode"), RuntimeNode);
            TestEqual(TEXT("SequencePlayer runtimeNode is FAnimNode_SequencePlayer"),
                RuntimeNode, FString(TEXT("FAnimNode_SequencePlayer")));

            const TArray<TSharedPtr<FJsonValue>>* RequiredAssetsPtr = nullptr;
            SequencePlayerEntry->TryGetArrayField(TEXT("requiredAssets"), RequiredAssetsPtr);
            bool bHasAnimSequenceClass = false;
            if (RequiredAssetsPtr)
            {
                for (const TSharedPtr<FJsonValue>& Val : *RequiredAssetsPtr)
                {
                    FString S;
                    if (Val->TryGetString(S) && S.Contains(TEXT("AnimSequence")))
                    {
                        bHasAnimSequenceClass = true;
                        break;
                    }
                }
            }
            TestTrue(TEXT("SequencePlayer requiredAssets contains an AnimSequence-family class"),
                bHasAnimSequenceClass);
        }
    }

    // 2. animation.search_graph_nodes — non-empty query returns SequencePlayer with score > 0.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("query"), TEXT("Sequence"));
        FAgirDispatchResult Dispatch = DispatchAnimRpcViaDispatcher(
            TEXT("animation.search_graph_nodes"), TEXT("anim-search-graph-nodes"), Payload);
        TestTrue(FString::Printf(TEXT("search success (errorCode='%s')"), *Dispatch.ErrorCode),
            Dispatch.bSuccess);
        if (!Dispatch.bSuccess || !Dispatch.Result.IsValid()) return false;

        const TArray<TSharedPtr<FJsonValue>>* ResultsPtr = nullptr;
        Dispatch.Result->TryGetArrayField(TEXT("results"), ResultsPtr);
        bool bSequencePlayerHit = false;
        if (ResultsPtr)
        {
            for (const TSharedPtr<FJsonValue>& V : *ResultsPtr)
            {
                const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
                if (!V->TryGetObject(ObjPtr) || !ObjPtr) continue;
                FString CN;
                (*ObjPtr)->TryGetStringField(TEXT("className"), CN);
                if (CN == TEXT("AnimGraphNode_SequencePlayer"))
                {
                    double Score = 0.0;
                    (*ObjPtr)->TryGetNumberField(TEXT("score"), Score);
                    if (Score > 0.0) { bSequencePlayerHit = true; }
                    break;
                }
            }
        }
        TestTrue(TEXT("search results contain SequencePlayer with score > 0"), bSequencePlayerHit);
    }

    // 3. animation.search_graph_nodes — empty query is rejected with INVALID_QUERY.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("query"), TEXT(""));
        FAgirDispatchResult Dispatch = DispatchAnimRpcViaDispatcher(
            TEXT("animation.search_graph_nodes"), TEXT("anim-search-empty-query"), Payload);
        TestFalse(TEXT("empty query is rejected"), Dispatch.bSuccess);
        TestEqual(TEXT("empty query yields INVALID_QUERY"),
            Dispatch.ErrorCode, FString(TEXT("INVALID_QUERY")));
    }

    return true;
}

// ============================================================================
// Regression (B-anim-graph-search-cdo-ensure): the discovery catalog must read a
// *graph-independent* title for every UAnimGraphNode_* CDO.
//
// AnimGraphSearchHandler::BuildEntry read GetNodeTitle(ENodeTitleType::ListView)
// on each class-default object. For UAnimGraphNode_SaveCachedPose the ListView
// branch never short-circuits: it routes through FNodeTextCache ->
// UEdGraphNode::GetGraph(), which ensures ("does not have a UEdGraph as an
// Outer") on a CDO (outered to its class, not a graph) and writes a crash dump
// on every call. The ensure is non-fatal, so the suite stays green and the
// description field is still populated correctly — meaning neither "no ensure"
// nor "non-empty description" distinguishes fixed from broken.
//
// The fix reads ENodeTitleType::MenuTitle, whose SaveCachedPose branch returns a
// fixed graph-independent label ("New Save cached pose...") for the empty-
// CacheName CDO without ever touching the graph. The deterministic signal this
// test asserts is the *value*: the handler's SaveCachedPose description must
// equal the graph-safe MenuTitle (computed live below). Reverting BuildEntry to
// ListView makes the description "Save cached pose ''" instead -> TestEqual
// fails. The expected value is computed via MenuTitle only, so this test never
// itself calls the ensure-prone ListView path.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimSearchGraphNodesCdoTitleNoEnsureTest,
    "PinWright.anim.discovery.SearchGraphNodesCdoTitleNoEnsure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimSearchGraphNodesCdoTitleNoEnsureTest::RunTest(const FString& Parameters)
{
    // Graph-safe label the fixed handler must report for the SaveCachedPose CDO.
    // MenuTitle short-circuits on the empty-CacheName CDO (returns "New Save
    // cached pose..."), so this call never reaches UEdGraphNode::GetGraph().
    const UAnimGraphNode_SaveCachedPose* SaveCachedCDO =
        GetDefault<UAnimGraphNode_SaveCachedPose>();
    TestNotNull(TEXT("SaveCachedPose CDO resolvable"), SaveCachedCDO);
    if (!SaveCachedCDO) return false;
    const FString ExpectedSafeTitle =
        SaveCachedCDO->GetNodeTitle(ENodeTitleType::MenuTitle).ToString();
    TestFalse(TEXT("expected graph-safe MenuTitle is non-empty"), ExpectedSafeTitle.IsEmpty());

    // Drive the real discovery RPC over the full UAnimGraphNode_Base catalog.
    FAgirDispatchResult Dispatch = DispatchAnimRpcViaDispatcher(
        TEXT("animation.list_graph_nodes"), TEXT("anim-cdo-title-no-ensure"),
        MakeShared<FJsonObject>());
    TestTrue(FString::Printf(TEXT("list success (errorCode='%s')"), *Dispatch.ErrorCode),
        Dispatch.bSuccess);
    if (!Dispatch.bSuccess || !Dispatch.Result.IsValid()) return false;

    TSharedPtr<FJsonObject> SaveCachedEntry = JsonArrayFindObjectByStringField(
        Dispatch.Result, TEXT("results"), TEXT("className"), TEXT("AnimGraphNode_SaveCachedPose"));
    TestTrue(TEXT("catalog contains AnimGraphNode_SaveCachedPose"), SaveCachedEntry.IsValid());
    if (!SaveCachedEntry.IsValid()) return false;

    FString Description;
    SaveCachedEntry->TryGetStringField(TEXT("description"), Description);
    // Load-bearing assertion: the handler must report the graph-safe MenuTitle,
    // not the ListView title produced by the (reverted) ensure path.
    TestEqual(TEXT("SaveCachedPose description is the graph-safe menu title (not the ListView/ensure title)"),
        Description, ExpectedSafeTitle);

    return true;
}

// ============================================================================
// Regression (B-create-anim-blueprint-duplicate-name-crash): calling
// animation.authoring.create_anim_blueprint twice with the same name+path must
// NOT crash the editor. Before the fix, the second call went straight to
// UAnimBlueprintFactory::FactoryCreateNew, where FKismetEditorUtilities::
// CreateBlueprint fires a fatal check(FindObject<UBlueprint>(Outer, ...) == 0)
// and brings down the host (taking this whole automation run with it). With the
// pre-existence guard the second call returns a clean ASSET_EXISTS error.
//
// Counterfactual: revert the FindObject/DoesAssetExist guard added to the
// create_anim_blueprint handler and the second dispatch below crashes the editor
// (the suite dies) instead of returning ASSET_EXISTS — this test fails either by
// crash or by the missing error code.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimAuthoringCreateAnimBlueprintDuplicateNameNoCrashTest,
    "PinWright.anim.authoring.CreateAnimBlueprintDuplicateNameNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimAuthoringCreateAnimBlueprintDuplicateNameNoCrashTest::RunTest(const FString& Parameters)
{
    if (!IsHandlerRegistered(TEXT("animation.authoring.create_anim_blueprint")))
    {
        AddError(TEXT("animation.authoring.create_anim_blueprint handler is not registered."));
        return false;
    }

    // Borrow the Mannequin AnimBP's skeleton so the create call gets past
    // SKELETON_NOT_FOUND and reaches the factory path the guard protects.
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(LyraMannequinAnimBPPath);
    UAnimBlueprint* Fixture = LoadLyraMannequinAnimBP();
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), Fixture))
    {
        return false;
    }
    if (!TestNotNull(TEXT("fixture has TargetSkeleton"), Fixture->TargetSkeleton.Get()))
    {
        return false;
    }
    const FString SkeletonPath = Fixture->TargetSkeleton->GetPathName();

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString FolderPath = TEXT("/Game/PinWrightTests");
    const FString AssetName = FString::Printf(TEXT("ABP_DupNameGuard_%s"), *Guid);
    const FString PackagePath = FolderPath / AssetName;

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackagePath);
    };

    auto BuildPayload = [&]()
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), AssetName);
        Payload->SetStringField(TEXT("path"), FolderPath);
        Payload->SetStringField(TEXT("skeletonPath"), SkeletonPath);
        // Keep the asset in memory only; no need to write to disk for the
        // collision check (the second call's FindObject<UBlueprint> hit is what
        // matters).
        Payload->SetBoolField(TEXT("save"), false);
        return Payload;
    };

    // First create — must succeed and leave the UAnimBlueprint loaded in memory.
    const FAgirDispatchResult First = DispatchAnimRpcViaDispatcher(
        TEXT("animation.authoring.create_anim_blueprint"),
        TEXT("req-create-anim-bp-dup-1"), BuildPayload());
    TestTrue(TEXT("first create completion fired"), First.bCompletionFired);
    TestTrue(FString::Printf(TEXT("first create succeeds (errorCode='%s')"), *First.ErrorCode),
        First.bSuccess);
    if (!First.bSuccess)
    {
        return false;
    }

    // Second create with the EXACT same name+path. Pre-fix this crashes the
    // editor inside CreateBlueprint's check(); post-fix it must return a clean
    // ASSET_EXISTS error rather than killing the host.
    const FAgirDispatchResult Second = DispatchAnimRpcViaDispatcher(
        TEXT("animation.authoring.create_anim_blueprint"),
        TEXT("req-create-anim-bp-dup-2"), BuildPayload());
    TestTrue(TEXT("duplicate create completion fired (no crash)"), Second.bCompletionFired);
    TestFalse(TEXT("duplicate create is rejected, not a fake success"), Second.bSuccess);
    TestEqual(TEXT("duplicate create yields ASSET_EXISTS"),
        Second.ErrorCode, FString(TEXT("ASSET_EXISTS")));
    return true;
}
