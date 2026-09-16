// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for the original widget-flavored B-bpir-override-param-not-resolvable repro.
//
// A real UWidgetBlueprint override of a BlueprintImplementableEvent with a
// const-ref struct parameter must be able to read `$InHit.bBlockingHit` from
// the body. Before the resolver fix, the entry param was registered correctly
// but `$InHit.bBlockingHit` still failed because PreEmitExternalGet never
// cached struct member access for `$target.Property`.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "WidgetBlueprint.h"
#include "TestWidgetWithStructBIE.h"


using namespace CompilerTestUtils;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirWidgetStructOverrideTest,
    "PinWright.bpir.compiler.integration.EntryOverrideWidgetBIEStructParamResolves",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirWidgetStructOverrideTest::RunTest(const FString& Parameters)
{
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
        UTestWidgetWithStructBIE::StaticClass(),
        GetTransientPackage(),
        FName(*FString::Printf(TEXT("WidgetStructOverrideBP_%d"), FMath::Rand())),
        BPTYPE_Normal,
        UWidgetBlueprint::StaticClass(),
        UWidgetBlueprintGeneratedClass::StaticClass()));

    TestNotNull(TEXT("Widget blueprint was created"), WBP);
    if (!WBP)
    {
        return false;
    }

    FEdGraphPinType BoolType;
    BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    FBlueprintEditorUtils::AddMemberVariable(WBP, TEXT("StoredBlockingHit"), BoolType);

    // Mirror the widget-BP pre-compile path so generated variables and override
    // metadata are populated before BPIR compiles the override body.
    FKismetEditorUtilities::CompileBlueprint(WBP);

    FBpirCompiler Compiler(WBP);
    FCompileResult Result = Compiler.Compile(
        TEXT("entry override TestStructBIE(const struct<FHitResult>& InHit) {\n")
        TEXT("    set StoredBlockingHit = $InHit.bBlockingHit\n")
        TEXT("}"));

    if (!Result.bSuccess)
    {
        for (const FCompileError& Err : Result.Errors)
        {
            AddError(FString::Printf(TEXT("BPIR compile error  L%d: %s"), Err.Line, *Err.Message));
        }
        return false;
    }

    TestTrue(TEXT("BPIR compile succeeded for widget override struct param access"), Result.bSuccess);
    TestEqual(TEXT("No compile errors"), Result.Errors.Num(), 0);

    BlueprintHandlerUtils::FBlueprintCompileDiagnostics Diagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(WBP);
    if (!Diagnostics.bCompiled)
    {
        for (const FString& Err : Diagnostics.Errors)
        {
            AddError(FString::Printf(TEXT("Post-BPIR full compile error: %s"), *Err));
        }
        return false;
    }

    BlueprintHandlerUtils::RefreshBpirDelegateNodes(WBP, Result.CreatedNodeGUIDs);

    TArray<BlueprintHandlerUtils::FBlueprintIntegrityFailure> Failures;
    const bool bIntegrityOk = BlueprintHandlerUtils::ValidateBlueprintGraphIntegrity(WBP, Failures);
    if (!bIntegrityOk)
    {
        for (const BlueprintHandlerUtils::FBlueprintIntegrityFailure& Fail : Failures)
        {
            AddError(FString::Printf(
                TEXT("IntegrityFailure nodeKind=%s graphName=%s reason=%s"),
                *Fail.NodeKind, *Fail.GraphName, *Fail.Reason));
        }
        return false;
    }

    TestTrue(TEXT("ValidateBlueprintGraphIntegrity returns true for widget override struct access"), bIntegrityOk);
    TestEqual(TEXT("No integrity failures recorded"), Failures.Num(), 0);
    return true;
}
