// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for split struct input pins. Split input parents are
// hidden by the Blueprint schema and their visible children are not valid pin
// names on a freshly created node. The emitter must rebuild one value before
// emitting the consuming call or set instruction.

#include "Misc/AutomationTest.h"

#include "BpirGraphTestHelpers.h"
#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Decompiler/BpirDecompiler.h"
#include "Decompiler/BpirTextEmitter.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Select.h"
#include "K2Node_VariableSet.h"
#include "Internationalization/Regex.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

namespace BpirSplitInputPinArgsTest
{
    FString ResolvePinForTest(UEdGraphPin* Pin)
    {
        if (!Pin)
        {
            return FString(TEXT("?"));
        }
        FString PinName = Pin->PinName.ToString();
        PinName.ReplaceInline(TEXT(" "), TEXT("_"));
        return FString::Printf(TEXT("value_%s"), *PinName);
    }

    int32 CountDirectSplitChildren(UEdGraphNode* Node, UEdGraphPin* ParentPin)
    {
        int32 Count = 0;
        if (!Node || !ParentPin)
        {
            return Count;
        }

        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->ParentPin == ParentPin)
            {
                ++Count;
            }
        }
        return Count;
    }

    FString StripAuthoredPositions(FString BpirText)
    {
        const FRegexPattern PositionPattern(TEXT("\\s*@\\(\\s*-?\\d+\\s*,\\s*-?\\d+\\s*\\)"));
        FRegexMatcher Matcher(PositionPattern, BpirText);
        FString Result;
        int32 Cursor = 0;
        while (Matcher.FindNext())
        {
            const int32 Begin = Matcher.GetMatchBeginning();
            const int32 End = Matcher.GetMatchEnding();
            Result.Append(BpirText.Mid(Cursor, Begin - Cursor));
            Cursor = End;
        }
        Result.Append(BpirText.Mid(Cursor));
        return Result;
    }

    void AddCompileErrors(FAutomationTestBase& Test, const TArray<FCompileError>& Errors, const TCHAR* Prefix)
    {
        for (const FCompileError& Error : Errors)
        {
            Test.AddError(FString::Printf(TEXT("%s L%d: %s"), Prefix, Error.Line, *Error.Message));
        }
    }

    int32 CountSubstring(const FString& Text, const TCHAR* Needle)
    {
        int32 Count = 0;
        int32 SearchFrom = 0;
        while ((SearchFrom = Text.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom)) != INDEX_NONE)
        {
            ++Count;
            ++SearchFrom;
        }
        return Count;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirSplitVectorInputFunctionCallTest,
    "PinWright.bpir.split_input.VectorFunctionCall",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirSplitVectorInputFunctionCallTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    TestNotNull(TEXT("Transient Blueprint was created"), Blueprint);
    if (!Blueprint || Blueprint->UbergraphPages.Num() == 0)
    {
        return false;
    }

    UEdGraph* Graph = Blueprint->UbergraphPages[0];
    UK2Node_CallFunction* CallNode =
        BpirGraphTestHelpers::AddNodeToGraph<UK2Node_CallFunction>(Graph);
    CallNode->FunctionReference.SetExternalMember(
        GET_FUNCTION_NAME_CHECKED(AActor, K2_SetActorLocation),
        AActor::StaticClass());
    CallNode->ReconstructNode();
    BpirGraphTestHelpers::WireExec(BpirGraphTestHelpers::EnsureBeginPlayNode(Graph), CallNode);

    UEdGraphPin* LocationPin = CallNode->FindPin(TEXT("NewLocation"), EGPD_Input);
    TestNotNull(TEXT("K2_SetActorLocation exposes NewLocation"), LocationPin);
    if (!LocationPin)
    {
        return false;
    }

    GetDefault<UEdGraphSchema_K2>()->SplitPin(LocationPin, /*bNotify=*/false);
    TestTrue(TEXT("Splitting NewLocation creates visible children"),
        BpirSplitInputPinArgsTest::CountDirectSplitChildren(CallNode, LocationPin) == 3);

    FBpirTextEmitter Emitter;
    const FString Emitted = Emitter.EmitCallNode(
        CallNode,
        FString(),
        BpirSplitInputPinArgsTest::ResolvePinForTest);

    TestTrue(TEXT("Function call emits a standalone FVector make"),
        Emitted.Contains(TEXT("make<Vector>(")));
    TestTrue(TEXT("Function call uses the reconstructed NewLocation value"),
        Emitted.Contains(TEXT("NewLocation: %split_")));
    TestTrue(TEXT("Function call retains the X member value"),
        Emitted.Contains(TEXT("X: value_NewLocation_X")));
    TestTrue(TEXT("Function call retains the Y member value"),
        Emitted.Contains(TEXT("Y: value_NewLocation_Y")));
    TestTrue(TEXT("Function call retains the Z member value"),
        Emitted.Contains(TEXT("Z: value_NewLocation_Z")));
    TestFalse(TEXT("Raw split child names are not emitted as call arguments"),
        Emitted.Contains(TEXT("NewLocation_X:")));

    FBpirDecompiler Decompiler(Blueprint);
    const FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Function-call graph decompiles"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess)
    {
        for (const FBpirWarning& Warning : DecompileResult.Warnings)
        {
            AddError(FString::Printf(TEXT("Decompile warning: %s"), *Warning.Text));
        }
        return false;
    }
    TestTrue(TEXT("Decompiled function call retains split FVector make"),
        DecompileResult.BpirText.Contains(TEXT("make<Vector>("))
        && DecompileResult.BpirText.Contains(TEXT("NewLocation: %split_")));

    UBlueprint* RecompileBlueprint = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    TestNotNull(TEXT("Function-call round-trip Blueprint was created"), RecompileBlueprint);
    if (!RecompileBlueprint)
    {
        return false;
    }
    FBpirCompiler RecompileCompiler(RecompileBlueprint);
    const FCompileResult RecompileResult = RecompileCompiler.Compile(DecompileResult.BpirText);
    if (!RecompileResult.bSuccess)
    {
        BpirSplitInputPinArgsTest::AddCompileErrors(*this, RecompileResult.Errors, TEXT("Function-call recompile"));
    }
    TestTrue(TEXT("Function-call decompiled BPIR recompiles"), RecompileResult.bSuccess);

    UK2Node_CallFunction* RecompiledConsumer = nullptr;
    UK2Node_CallFunction* RecompiledMake = nullptr;
    for (UEdGraph* RecompileGraph : RecompileBlueprint->UbergraphPages)
    {
        if (!RecompileGraph)
        {
            continue;
        }
        for (UEdGraphNode* Candidate : RecompileGraph->Nodes)
        {
            UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Candidate);
            if (!Call)
            {
                continue;
            }
            if (Call->GetFunctionName() == TEXT("K2_SetActorLocation"))
            {
                RecompiledConsumer = Call;
            }
            else if (Call->GetFunctionName() == TEXT("MakeVector"))
            {
                RecompiledMake = Call;
            }
        }
    }
    TestNotNull(TEXT("Positioned round-trip retains the consuming call"), RecompiledConsumer);
    TestNotNull(TEXT("Positioned round-trip creates the generated MakeVector"), RecompiledMake);
    if (RecompiledConsumer && RecompiledMake)
    {
        TestEqual(TEXT("Generated MakeVector is 300 units left of its consumer"),
            RecompiledMake->NodePosX, RecompiledConsumer->NodePosX - 300);
        TestEqual(TEXT("Generated MakeVector starts 80 units below its consumer"),
            RecompiledMake->NodePosY, RecompiledConsumer->NodePosY + 80);
        TestTrue(TEXT("Generated MakeVector position differs from its consumer"),
            RecompiledMake->NodePosX != RecompiledConsumer->NodePosX
            || RecompiledMake->NodePosY != RecompiledConsumer->NodePosY);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirSplitVectorInputVariableSetTest,
    "PinWright.bpir.split_input.VectorVariableSet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirSplitVectorInputVariableSetTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    TestNotNull(TEXT("Transient Blueprint was created"), Blueprint);
    if (!Blueprint || Blueprint->UbergraphPages.Num() == 0)
    {
        return false;
    }

    FEdGraphPinType VectorType;
    VectorType.PinCategory = UEdGraphSchema_K2::PC_Struct;
    VectorType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
    FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("StoredLocation"), VectorType);
    FKismetEditorUtilities::CompileBlueprint(Blueprint);

    UEdGraph* Graph = Blueprint->UbergraphPages[0];
    UK2Node_VariableSet* SetNode =
        BpirGraphTestHelpers::AddNodeToGraph<UK2Node_VariableSet>(Graph);
    SetNode->VariableReference.SetSelfMember(TEXT("StoredLocation"));
    SetNode->ReconstructNode();
    BpirGraphTestHelpers::WireExec(BpirGraphTestHelpers::EnsureBeginPlayNode(Graph), SetNode);

    UEdGraphPin* LocationPin = SetNode->FindPin(TEXT("StoredLocation"), EGPD_Input);
    TestNotNull(TEXT("Variable set exposes StoredLocation"), LocationPin);
    if (!LocationPin)
    {
        return false;
    }

    GetDefault<UEdGraphSchema_K2>()->SplitPin(LocationPin, /*bNotify=*/false);
    TestTrue(TEXT("Splitting StoredLocation creates visible children"),
        BpirSplitInputPinArgsTest::CountDirectSplitChildren(SetNode, LocationPin) == 3);

    FBpirTextEmitter Emitter;
    const FString Emitted = Emitter.EmitVariableSet(
        SetNode,
        BpirSplitInputPinArgsTest::ResolvePinForTest);

    TestTrue(TEXT("Variable set emits a standalone FVector make"),
        Emitted.Contains(TEXT("make<Vector>(")));
    TestTrue(TEXT("Variable set consumes the reconstructed StoredLocation value"),
        Emitted.Contains(TEXT("set StoredLocation = %split_")));
    TestTrue(TEXT("Variable set retains the X member value"),
        Emitted.Contains(TEXT("X: value_StoredLocation_X")));
    TestTrue(TEXT("Variable set retains the Y member value"),
        Emitted.Contains(TEXT("Y: value_StoredLocation_Y")));
    TestTrue(TEXT("Variable set retains the Z member value"),
        Emitted.Contains(TEXT("Z: value_StoredLocation_Z")));
    TestFalse(TEXT("Raw split child names are not emitted as set arguments"),
        Emitted.Contains(TEXT("StoredLocation_X:")));

    FBpirDecompiler Decompiler(Blueprint);
    const FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Variable-set graph decompiles"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess)
    {
        for (const FBpirWarning& Warning : DecompileResult.Warnings)
        {
            AddError(FString::Printf(TEXT("Decompile warning: %s"), *Warning.Text));
        }
        return false;
    }
    TestTrue(TEXT("Decompiled variable set retains split FVector make"),
        DecompileResult.BpirText.Contains(TEXT("make<Vector>("))
        && DecompileResult.BpirText.Contains(TEXT("set StoredLocation = %split_")));

    UBlueprint* RecompileBlueprint = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    TestNotNull(TEXT("Variable-set round-trip Blueprint was created"), RecompileBlueprint);
    if (!RecompileBlueprint)
    {
        return false;
    }
    FBlueprintEditorUtils::AddMemberVariable(RecompileBlueprint, TEXT("StoredLocation"), VectorType);
    FKismetEditorUtilities::CompileBlueprint(RecompileBlueprint);
    FBpirCompiler RecompileCompiler(RecompileBlueprint);
    const FCompileResult RecompileResult = RecompileCompiler.Compile(
        BpirSplitInputPinArgsTest::StripAuthoredPositions(DecompileResult.BpirText));
    if (!RecompileResult.bSuccess)
    {
        BpirSplitInputPinArgsTest::AddCompileErrors(*this, RecompileResult.Errors, TEXT("Variable-set recompile"));
    }
    TestTrue(TEXT("Variable-set decompiled BPIR recompiles"), RecompileResult.bSuccess);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirSplitVectorInputSelectTest,
    "PinWright.bpir.split_input.VectorSelect",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirSplitVectorInputSelectTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    TestNotNull(TEXT("Select Blueprint was created"), Blueprint);
    if (!Blueprint || Blueprint->UbergraphPages.Num() == 0)
    {
        return false;
    }

    const FString InputBpir = TEXT(
        "entry event BeginPlay() {\n"
        "    %left = make<Vector>(X: 1.0, Y: 2.0, Z: 3.0)\n"
        "    %right = make<Vector>(X: 4.0, Y: 5.0, Z: 6.0)\n"
        "    %selected = select(Index: true, true: %left, false: %right)\n"
        "}");
    FBpirCompiler Compiler(Blueprint);
    const FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess)
    {
        BpirSplitInputPinArgsTest::AddCompileErrors(*this, CompileResult.Errors, TEXT("Select setup"));
    }
    TestTrue(TEXT("Select setup compiles"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        return false;
    }

    UK2Node_Select* SelectNode = BpirGraphTestHelpers::FindFirstNodeOfType<UK2Node_Select>(Blueprint);
    TestNotNull(TEXT("Compiled graph contains a Select node"), SelectNode);
    if (!SelectNode)
    {
        return false;
    }

    TArray<UEdGraphPin*> OptionPins;
    SelectNode->GetOptionPins(OptionPins);
    TestEqual(TEXT("Select has two vector options"), OptionPins.Num(), 2);
    if (OptionPins.Num() != 2)
    {
        return false;
    }
    for (UEdGraphPin* OptionPin : OptionPins)
    {
        GetDefault<UEdGraphSchema_K2>()->SplitPin(OptionPin, /*bNotify=*/false);
    }

    FBpirTextEmitter Emitter;
    const FString Emitted = Emitter.EmitPureNode(
        SelectNode,
        TEXT("selected"),
        BpirSplitInputPinArgsTest::ResolvePinForTest);
    TestEqual(TEXT("Each vector Select option emits one make"),
        BpirSplitInputPinArgsTest::CountSubstring(Emitted, TEXT("make<Vector>(")), 2);
    TestTrue(TEXT("Select preserves false option naming"), Emitted.Contains(TEXT("false: %split_")));
    TestTrue(TEXT("Select preserves true option naming"), Emitted.Contains(TEXT("true: %split_")));
    TestTrue(TEXT("Select retains split X values"),
        Emitted.Contains(TEXT("X: value_Option_0_X"))
        && Emitted.Contains(TEXT("X: value_Option_1_X")));
    TestFalse(TEXT("Select does not emit raw split option child names"),
        Emitted.Contains(TEXT("Option 0_X:")) || Emitted.Contains(TEXT("Option 1_X:")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirMalformedSplitInputEmitterTest,
    "PinWright.bpir.split_input.MalformedChild",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirMalformedSplitInputEmitterTest::RunTest(const FString& Parameters)
{
    UBlueprint* Blueprint = BpirGraphTestHelpers::CreateOrphanTestBlueprint();
    TestNotNull(TEXT("Malformed split Blueprint was created"), Blueprint);
    if (!Blueprint || Blueprint->UbergraphPages.Num() == 0)
    {
        return false;
    }

    UEdGraph* Graph = Blueprint->UbergraphPages[0];
    UK2Node_CallFunction* CallNode =
        BpirGraphTestHelpers::AddNodeToGraph<UK2Node_CallFunction>(Graph);
    CallNode->FunctionReference.SetExternalMember(
        GET_FUNCTION_NAME_CHECKED(AActor, K2_SetActorLocation),
        AActor::StaticClass());
    CallNode->ReconstructNode();

    UEdGraphPin* LocationPin = CallNode->FindPin(TEXT("NewLocation"), EGPD_Input);
    TestNotNull(TEXT("Malformed test exposes NewLocation"), LocationPin);
    if (!LocationPin)
    {
        return false;
    }
    GetDefault<UEdGraphSchema_K2>()->SplitPin(LocationPin, /*bNotify=*/false);
    if (LocationPin->SubPins.Num() == 0)
    {
        return false;
    }
    UEdGraphPin* Child = LocationPin->SubPins[0];
    TestNotNull(TEXT("Malformed test has a split child"), Child);
    if (!Child)
    {
        return false;
    }

    FBpirTextEmitter Emitter;
    Child->PinName = TEXT("NotNewLocation_X");
    const FString MalformedEmitted = Emitter.EmitCallNode(
        CallNode,
        FString(),
        BpirSplitInputPinArgsTest::ResolvePinForTest);
    TestTrue(TEXT("Malformed split name preserves the parent as unresolved"),
        MalformedEmitted.Contains(TEXT("NewLocation: <unresolved>")));
    TestFalse(TEXT("Malformed split name emits no partial make"),
        MalformedEmitted.Contains(TEXT("make<Vector>(")));

    Child->PinName = TEXT("NewLocation_X");
    Child->bOrphanedPin = true;
    const FString OrphanedEmitted = Emitter.EmitCallNode(
        CallNode,
        FString(),
        BpirSplitInputPinArgsTest::ResolvePinForTest);
    TestTrue(TEXT("Orphaned split child preserves the parent as unresolved"),
        OrphanedEmitted.Contains(TEXT("NewLocation: <unresolved>")));
    TestFalse(TEXT("Orphaned split child emits no partial make"),
        OrphanedEmitted.Contains(TEXT("make<Vector>(")));

    Child->ParentPin = nullptr;
    const FString DetachedOrphanEmitted = Emitter.EmitCallNode(
        CallNode,
        FString(),
        BpirSplitInputPinArgsTest::ResolvePinForTest);
    TestTrue(TEXT("Detached orphan child emits an unresolved value"),
        DetachedOrphanEmitted.Contains(TEXT("NewLocation_X: <unresolved>")));
    TestFalse(TEXT("Detached orphan child emits no resolved raw value"),
        DetachedOrphanEmitted.Contains(TEXT("NewLocation_X: value_")));
    return true;
}
