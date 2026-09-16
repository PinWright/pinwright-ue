// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBpirSharedTopologyCastSuccessWired.cpp
// Regression test for B-bpir-statement-cast-success-unwired-replace-shared-topology
// (manifestation #1: statement-form cast's success exec link silently unwired when
// compile_bpir replaces entries on assets with shared K2Node topology across entries).
//
// Pre-fix: a multi-entry compile in Replace mode where (a) the target asset already
// has shared K2Node topology between entries (two entries' bodies reference the same
// physical UK2Node_VariableSet), and (b) the new submission adds a fresh
// statement-form cast inside a latent-continuation block, returns success=true /
// errors=[] but leaves the cast node's PN_CastSucceeded exec pin with zero linked
// downstream entries. The body following the [success -> @label] clause is created
// but orphaned.
//
// This synthetic fixture sets up the shared-topology precondition by compiling a
// first BPIR that plants two custom_event entries whose bodies both write the same
// member variable. The second compile (Replace mode) then submits three entries —
// the two pre-existing custom_events plus a new custom_event with a statement-form
// cast inside a latent Delay's continuation block. After the second compile, the
// new cast's success exec pin must have at least one downstream link.
//
// Counterfactual: if WireExecPins Step 3 reverts to the unwired-then-no-error
// behaviour, the test FAILS at the GetValidCastPin()->LinkedTo.Num() > 0 assertion.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "GameFramework/Character.h"
#include "K2Node_DynamicCast.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

using namespace CompilerTestUtils;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompilerIntegrationSharedTopologyCastSuccessWiredTest,
    "PinWright.bpir.compiler.integration.SharedTopologyCastSuccessWired",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompilerIntegrationSharedTopologyCastSuccessWiredTest::RunTest(const FString& Parameters)
{
    // 1. Create an AActor-derived transient BP with an int member 'Counter'. Both
    //    entries below will write Counter, planting shared-topology state.
    UBlueprint* BP = CreateTransientTestBP(TEXT("SharedTopologyCastSuccessBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FEdGraphPinType IntType;
    IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
    const bool bVarAdded = FBlueprintEditorUtils::AddMemberVariable(
        BP, TEXT("Counter"), IntType);
    TestTrue(TEXT("Counter member variable added"), bVarAdded);
    if (!bVarAdded) return false;
    FKismetEditorUtilities::CompileBlueprint(BP);

    // 2. First compile (default/append mode): plant two custom_event entries that
    //    both write Counter. The repeated `set Counter` references across the two
    //    entries plant shared K2Node topology — both bodies are emitted against the
    //    same physical UK2Node_VariableSet identity for Counter.
    {
        FBpirCompiler Setup(BP);
        FCompileResult SetupResult = Setup.Compile(
            TEXT("entry custom_event SharedWriterA() {\n")
            TEXT("    set Counter = 1\n")
            TEXT("}\n")
            TEXT("entry custom_event SharedWriterB() {\n")
            TEXT("    set Counter = 2\n")
            TEXT("}"));
        if (!SetupResult.bSuccess)
        {
            for (const FCompileError& Err : SetupResult.Errors)
            {
                AddError(FString::Printf(TEXT("Setup compile L%d: %s"), Err.Line, *Err.Message));
            }
        }
        TestTrue(TEXT("Setup compile (two custom_events sharing $Counter) succeeded"),
            SetupResult.bSuccess);
        if (!SetupResult.bSuccess) return false;
    }

    // 3. Second compile (Replace mode): submit three entries — the existing
    //    SharedWriterA (matching the entry that previously shared K2Nodes),
    //    SharedWriterB (also previously sharing), and a new custom_event with a
    //    statement-form cast inside a latent Delay's continuation block. This
    //    mirrors the ticket's repro shape: multi-entry Replace mode with a fresh
    //    statement-form cast in a latent-continuation block on an asset that
    //    already has shared topology across entries.
    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry custom_event SharedWriterA() {\n")
        TEXT("    set Counter = 3\n")
        TEXT("}\n")
        TEXT("entry custom_event SharedWriterB() {\n")
        TEXT("    set Counter = 4\n")
        TEXT("}\n")
        TEXT("entry custom_event LatentCastEntry() {\n")
        TEXT("    %d = latent Delay(Duration: 0.2) [completed -> @after]\n")
        TEXT("@after:\n")
        TEXT("    %pawn = call GetPlayerPawn(PlayerIndex: 0)\n")
        TEXT("    %c = cast<Character>(%pawn) [success -> @hit]\n")
        TEXT("@hit:\n")
        TEXT("    set Counter = 5\n")
        TEXT("}"),
        /*bReplaceMode=*/true);

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("Replace compile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Replace compile (multi-entry with statement-form cast in latent continuation) succeeded"),
        Result.bSuccess);
    if (!Result.bSuccess) return false;

    // 4. Locate the new statement-form cast node. It must be impure (bIsPureCast == false)
    //    and target Character.
    UK2Node_DynamicCast* StatementCast = nullptr;
    for (UEdGraph* Graph : BP->UbergraphPages)
    {
        if (!Graph) continue;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_DynamicCast* CastNode = ::Cast<UK2Node_DynamicCast>(Node);
            if (!CastNode) continue;
            if (CastNode->IsNodePure()) continue;
            if (CastNode->TargetType.Get() == ACharacter::StaticClass())
            {
                StatementCast = CastNode;
                break;
            }
        }
        if (StatementCast) break;
    }
    TestNotNull(TEXT("Statement-form Character cast node located in ubergraph"), StatementCast);
    if (!StatementCast) return false;
    TestFalse(TEXT("Cast node is impure (statement form)"), StatementCast->IsNodePure());

    // 5. The cast's success ('valid cast') exec pin must have at least one outgoing
    //    link — i.e. the [success -> @hit] clause was wired. This is the assertion
    //    that fails pre-patch on the live repro asset.
    UEdGraphPin* ValidCastPin = StatementCast->GetValidCastPin();
    TestNotNull(TEXT("Cast 'valid cast' (success) exec pin exists"), ValidCastPin);
    if (!ValidCastPin) return false;
    TestTrue(TEXT("Cast PN_CastSucceeded has at least one downstream exec link"),
        ValidCastPin->LinkedTo.Num() > 0);

    return true;
}
