// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBpirSetCastRhs.cpp
// Regression test for E-bpir-cast-as-set-rhs:
// `set $TypedRef = cast<Actor>($Source)` — i.e. an inline cast<T>(...) used as the
// RHS of a `set` statement — must compile into a single PURE UK2Node_DynamicCast
// whose source pin is the entry parameter and whose result pin feeds the variable
// set. Counterfactual: if ResolveCastExpression reverts to raw MakeLinkTo, the
// link-count assertions still pass, but the cast source pin remains PC_Wildcard and
// full Blueprint compile fails with the undetermined Object pin error.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

using namespace CompilerTestUtils;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirSetCastRhsTest,
    "PinWright.bpir.compile.SetCastRhs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirSetCastRhsTest::RunTest(const FString& Parameters)
{
    // 1. Create a transient AActor-based Blueprint with a TObjectPtr<AActor> member.
    UBlueprint* BP = CreateTransientTestBP(TEXT("BpirSetCastRhsBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FEdGraphPinType ActorRefType;
    ActorRefType.PinCategory = UEdGraphSchema_K2::PC_Object;
    ActorRefType.PinSubCategoryObject = AActor::StaticClass();
    const bool bVarAdded = FBlueprintEditorUtils::AddMemberVariable(
        BP, TEXT("TypedRef"), ActorRefType);
    TestTrue(TEXT("TypedRef member variable added"), bVarAdded);
    if (!bVarAdded) return false;

    FKismetEditorUtilities::CompileBlueprint(BP);

    // 2. Compile the inline-cast BPIR body. The entry function takes an Object
    //    parameter and assigns a downcast of it into the TypedRef member.
    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry function Apply(object<Object> Source) {\n")
        TEXT("    set $TypedRef = cast<Actor>($Source)\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("BPIR compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("BPIR compiled successfully (set $TypedRef = cast<Actor>($Source))"),
        Result.bSuccess);
    TestEqual(TEXT("No compile errors"), Result.Errors.Num(), 0);
    if (!Result.bSuccess) return false;

    // 3. Locate the function graph and the cast node within it.
    UEdGraph* FunctionGraph = nullptr;
    for (UEdGraph* Graph : BP->FunctionGraphs)
    {
        if (Graph && Graph->GetFName() == FName(TEXT("Apply")))
        {
            FunctionGraph = Graph;
            break;
        }
    }
    TestNotNull(TEXT("Apply function graph exists"), FunctionGraph);
    if (!FunctionGraph) return false;

    int32 CastCount = 0;
    UK2Node_DynamicCast* CastNode = nullptr;
    for (UEdGraphNode* Node : FunctionGraph->Nodes)
    {
        if (UK2Node_DynamicCast* Cast = ::Cast<UK2Node_DynamicCast>(Node))
        {
            ++CastCount;
            CastNode = Cast;
        }
    }
    TestEqual(TEXT("Exactly one UK2Node_DynamicCast in the function graph"), CastCount, 1);
    TestNotNull(TEXT("Cast node located"), CastNode);
    if (!CastNode) return false;

    // 4. The cast must be pure and target AActor.
    TestTrue(TEXT("Cast node is pure (bIsPureCast == true)"), CastNode->IsNodePure());
    TestEqual(TEXT("Cast TargetType == AActor::StaticClass()"),
        CastNode->TargetType.Get(), AActor::StaticClass());

    // 5. The cast result pin must drive exactly one UK2Node_VariableSet for "TypedRef".
    UEdGraphPin* ResultPin = CastNode->GetCastResultPin();
    TestNotNull(TEXT("Cast result pin exists"), ResultPin);
    if (!ResultPin) return false;
    TestEqual(TEXT("Cast result pin has exactly one outgoing link"), ResultPin->LinkedTo.Num(), 1);
    if (ResultPin->LinkedTo.Num() != 1) return false;

    UK2Node_VariableSet* VarSetNode = ::Cast<UK2Node_VariableSet>(ResultPin->LinkedTo[0]->GetOwningNode());
    TestNotNull(TEXT("Cast result pin feeds a UK2Node_VariableSet"), VarSetNode);
    if (!VarSetNode) return false;
    TestEqual(TEXT("Variable set name is TypedRef"),
        VarSetNode->GetVarName(), FName(TEXT("TypedRef")));

    // 6. The cast source pin must receive exactly one link from the Apply entry's
    //    "Source" output pin.
    UEdGraphPin* SourcePin = CastNode->GetCastSourcePin();
    TestNotNull(TEXT("Cast source pin exists"), SourcePin);
    if (!SourcePin) return false;
    TestEqual(TEXT("Cast source pin has exactly one incoming link"), SourcePin->LinkedTo.Num(), 1);
    if (SourcePin->LinkedTo.Num() != 1) return false;
    TestTrue(TEXT("Cast source pin category resolved from wildcard"),
        SourcePin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard);
    TestEqual(TEXT("Cast source pin category is object"),
        SourcePin->PinType.PinCategory, UEdGraphSchema_K2::PC_Object);

    UK2Node_FunctionEntry* EntryNode =
        ::Cast<UK2Node_FunctionEntry>(SourcePin->LinkedTo[0]->GetOwningNode());
    TestNotNull(TEXT("Cast source connects to the function entry node"), EntryNode);
    if (!EntryNode) return false;
    TestEqual(TEXT("Cast source connects to the 'Source' entry pin"),
        SourcePin->LinkedTo[0]->PinName, FName(TEXT("Source")));

    BlueprintHandlerUtils::FBlueprintCompileDiagnostics Diagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(BP);
    if (!Diagnostics.bCompiled || Diagnostics.Errors.Num() > 0)
    {
        for (const FString& Err : Diagnostics.Errors)
        {
            AddError(FString::Printf(TEXT("Post-BPIR full compile error: %s"), *Err));
        }
    }
    TestTrue(TEXT("Post-BPIR full compile succeeded"), Diagnostics.bCompiled);
    TestEqual(TEXT("Post-BPIR full compile diagnostics has no errors"), Diagnostics.Errors.Num(), 0);

    return Diagnostics.bCompiled && Diagnostics.Errors.Num() == 0;
}
