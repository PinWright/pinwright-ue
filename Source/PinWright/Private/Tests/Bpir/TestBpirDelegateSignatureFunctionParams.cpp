// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Components/Button.h"
#include "Components/Widget.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/MemberReference.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/UnrealType.h"


using namespace CompilerTestUtils;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirDelegateSignatureFunctionParamCompilesTest,
    "PinWright.bpir.compiler.DelegateSignatureFunctionParamCompiles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirDelegateSignatureFunctionParamCompilesTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("DelegateSignatureFunctionParamBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry function VerifyDelegates(delegate<Widget, FGetBool__DelegateSignature> Handler, mcdelegate<Button, OnButtonClickedEvent__DelegateSignature> MultiHandler) { }"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("BPIR compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("BPIR compile succeeded"), Result.bSuccess);
    if (!Result.bSuccess) return false;

    UEdGraph* FunctionGraph = nullptr;
    for (UEdGraph* Graph : BP->FunctionGraphs)
    {
        if (Graph && Graph->GetFName() == FName(TEXT("VerifyDelegates")))
        {
            FunctionGraph = Graph;
            break;
        }
    }
    TestNotNull(TEXT("VerifyDelegates function graph exists"), FunctionGraph);
    if (!FunctionGraph) return false;

    UK2Node_FunctionEntry* EntryNode = FindNodeOfType<UK2Node_FunctionEntry>(FunctionGraph);
    TestNotNull(TEXT("VerifyDelegates function entry exists"), EntryNode);
    if (!EntryNode) return false;

    UEdGraphPin* HandlerPin = EntryNode->FindPin(FName(TEXT("Handler")), EGPD_Output);
    UEdGraphPin* MultiHandlerPin = EntryNode->FindPin(FName(TEXT("MultiHandler")), EGPD_Output);
    TestNotNull(TEXT("Handler pin exists"), HandlerPin);
    TestNotNull(TEXT("MultiHandler pin exists"), MultiHandlerPin);
    if (!HandlerPin || !MultiHandlerPin) return false;

    TestEqual(TEXT("Handler pin category is PC_Delegate"),
        HandlerPin->PinType.PinCategory, UEdGraphSchema_K2::PC_Delegate);
    TestEqual(TEXT("MultiHandler pin category is PC_MCDelegate"),
        MultiHandlerPin->PinType.PinCategory, UEdGraphSchema_K2::PC_MCDelegate);

    UFunction* HandlerSignature = FMemberReference::ResolveSimpleMemberReference<UFunction>(
        HandlerPin->PinType.PinSubCategoryMemberReference);
    UFunction* MultiHandlerSignature = FMemberReference::ResolveSimpleMemberReference<UFunction>(
        MultiHandlerPin->PinType.PinSubCategoryMemberReference);
    TestNotNull(TEXT("Handler delegate signature resolves"), HandlerSignature);
    TestNotNull(TEXT("MultiHandler delegate signature resolves"), MultiHandlerSignature);
    if (!HandlerSignature || !MultiHandlerSignature) return false;

    // UHT strips the leading 'F' from dynamic delegate type names: UWidget's
    // DECLARE_DYNAMIC_DELEGATE_RetVal(bool, FGetBool) registers the UFunction as
    // GetBool__DelegateSignature, and UButton's DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnButtonClickedEvent)
    // registers as OnButtonClickedEvent__DelegateSignature. See CodePinResolver.cpp:525-530.
    TestEqual(TEXT("Handler signature name"),
        HandlerSignature->GetFName(), FName(TEXT("GetBool__DelegateSignature")));
    TestEqual(TEXT("MultiHandler signature name"),
        MultiHandlerSignature->GetFName(), FName(TEXT("OnButtonClickedEvent__DelegateSignature")));

    FKismetEditorUtilities::CompileBlueprint(BP);
    TestFalse(TEXT("Full Blueprint compile did not leave the BP in error status"), BP->Status == BS_Error);
    UClass* GeneratedClass = BP->GeneratedClass.Get();
    TestNotNull(TEXT("GeneratedClass exists after full compile"), GeneratedClass);
    if (BP->Status == BS_Error || !GeneratedClass) return false;

    UFunction* GeneratedFunction = GeneratedClass->FindFunctionByName(
        FName(TEXT("VerifyDelegates")),
        EIncludeSuperFlag::ExcludeSuper);
    TestNotNull(TEXT("VerifyDelegates generated UFunction exists"), GeneratedFunction);
    if (!GeneratedFunction) return false;

    FDelegateProperty* HandlerProperty = CastField<FDelegateProperty>(
        GeneratedFunction->FindPropertyByName(FName(TEXT("Handler"))));
    FMulticastDelegateProperty* MultiHandlerProperty = CastField<FMulticastDelegateProperty>(
        GeneratedFunction->FindPropertyByName(FName(TEXT("MultiHandler"))));
    TestNotNull(TEXT("Handler generated as FDelegateProperty"), HandlerProperty);
    TestNotNull(TEXT("MultiHandler generated as FMulticastDelegateProperty"), MultiHandlerProperty);
    if (!HandlerProperty || !MultiHandlerProperty) return false;

    TestNotNull(TEXT("Handler SignatureFunction is non-null"), HandlerProperty->SignatureFunction.Get());
    TestNotNull(TEXT("MultiHandler SignatureFunction is non-null"), MultiHandlerProperty->SignatureFunction.Get());

    return true;
}
