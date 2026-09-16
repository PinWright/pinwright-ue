// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBpirComponentBoundEventIdentity.cpp

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Decompiler/BpirDecompiler.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Components/CheckBox.h"
#include "EdGraphSchema_K2.h"
#include "WidgetBlueprint.h"

using namespace CompilerTestUtils;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirComponentBoundWidgetEventsDecompileIdentityTest,
    "PinWright.bpir.decompiler.ComponentBoundWidgetEventIdentity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirComponentBoundWidgetEventsDecompileIdentityTest::RunTest(const FString& Parameters)
{
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
        UUserWidget::StaticClass(),
        GetTransientPackage(),
        FName(*FString::Printf(TEXT("ComponentBoundWidgetIdentityBP_%d"), FMath::Rand())),
        BPTYPE_Normal,
        UWidgetBlueprint::StaticClass(),
        UWidgetBlueprintGeneratedClass::StaticClass()));

    TestNotNull(TEXT("Widget blueprint was created"), WBP);
    if (!WBP) return false;

    FEdGraphPinType CheckBoxType;
    CheckBoxType.PinCategory = UEdGraphSchema_K2::PC_Object;
    CheckBoxType.PinSubCategoryObject = UCheckBox::StaticClass();
    FBlueprintEditorUtils::AddMemberVariable(WBP, TEXT("FirstCheck"), CheckBoxType);
    FBlueprintEditorUtils::AddMemberVariable(WBP, TEXT("SecondCheck"), CheckBoxType);

    FKismetEditorUtilities::CompileBlueprint(WBP);

    FBpirCompiler Compiler(WBP);
    FCompileResult CompileResult = Compiler.Compile(
        TEXT("entry widget_event FirstCheck.OnCheckStateChanged() {\n")
        TEXT("}\n")
        TEXT("entry widget_event SecondCheck.OnCheckStateChanged() {\n")
        TEXT("}"));

    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("BPIR compile error L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Widget component-bound BPIR compile succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(WBP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess) return false;

    const FString& Output = DecompileResult.BpirText;
    TestTrue(TEXT("First widget-bound event keeps component identity"),
        Output.Contains(TEXT("entry widget_event FirstCheck.OnCheckStateChanged(")));
    TestTrue(TEXT("Second widget-bound event keeps component identity"),
        Output.Contains(TEXT("entry widget_event SecondCheck.OnCheckStateChanged(")));
    TestFalse(TEXT("Widget-bound delegates do not flatten to duplicate plain delegate-signature events"),
        Output.Contains(TEXT("entry event OnCheckStateChangedEvent__DelegateSignature(")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirDuplicatePlainDelegateSignatureEventRejectedTest,
    "PinWright.bpir.compiler.DuplicatePlainDelegateSignatureEventRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirDuplicatePlainDelegateSignatureEventRejectedTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CreateTransientTestBP(TEXT("DuplicatePlainDelegateSignatureBP"));
    TestNotNull(TEXT("Blueprint was created"), BP);
    if (!BP) return false;

    FBpirCompiler Compiler(BP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry event OnCheckStateChangedEvent__DelegateSignature(bool bIsChecked) {\n")
        TEXT("}\n")
        TEXT("entry event OnCheckStateChangedEvent__DelegateSignature(bool bIsChecked) {\n")
        TEXT("}"));

    TestFalse(TEXT("Duplicate plain delegate-signature events are rejected"), Result.bSuccess);
    TestTrue(TEXT("Diagnostic mentions duplicate plain entry events"),
        ErrorsContain(Result.Errors, TEXT("Duplicate plain entry events")));
    TestTrue(TEXT("Diagnostic points to widget_event or component_event"),
        ErrorsContain(Result.Errors, TEXT("widget_event"))
        || ErrorsContain(Result.Errors, TEXT("component_event")));
    return true;
}
