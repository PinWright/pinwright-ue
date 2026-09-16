// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"


#include "AGIR/AGIRParser.h"
#include "AGIR/AGIRTextEmitter.h"
#include "MGIR/MGIRDecompiler.h"
#include "MGIR/MGIRParser.h"
#include "IrCore/IrTextUtils.h"

#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIRGrammarHarmonization_AGIRColonFieldsAndMnemonicIds,
    "PinWright.core.ir_grammar.AGIRColonFieldsAndMnemonicIds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FIRGrammarHarmonization_AGIRColonFieldsAndMnemonicIds::RunTest(const FString& Parameters)
{
    const FString ValidAgir =
        TEXT("entry anim_graph Main {\n")
        TEXT("    %save_cached_pose_0 = call `/Script/AnimGraph.AnimGraphNode_SaveCachedPose`(Pose: %blend_space_0)\n")
        TEXT("}");

    TArray<FAGIREntryBlock> Blocks;
    TArray<FAGIRParseError> Errors;
    TestTrue(TEXT("AGIR parser accepts colon-separated parenthesized fields"),
        FAGIRParser::Parse(ValidAgir, Blocks, Errors));
    TestEqual(TEXT("One AGIR entry parsed"), Blocks.Num(), 1);
    if (Blocks.Num() > 0 && Blocks[0].Instructions.Num() > 0 && Blocks[0].Instructions[0]->Args.Num() > 0)
    {
        const FAGIRInstruction& Instruction = *Blocks[0].Instructions[0];
        TestEqual(TEXT("Colon field name parsed"), Instruction.Args[0].Name, FString(TEXT("Pose")));
        TestEqual(TEXT("Colon field value parsed"), Instruction.Args[0].Value, FString(TEXT("%blend_space_0")));
    }

    const FString InvalidAgir =
        TEXT("entry anim_graph Main {\n")
        TEXT("    %save_cached_pose_0 = call `/Script/AnimGraph.AnimGraphNode_SaveCachedPose`(Pose=%blend_space_0)\n")
        TEXT("}");
    Blocks.Reset();
    Errors.Reset();
    TestFalse(TEXT("AGIR parser rejects top-level equals in parenthesized fields"),
        FAGIRParser::Parse(InvalidAgir, Blocks, Errors));

    FAGIRTextEmitter Emitter(nullptr);
    UAnimGraphNode_SaveCachedPose* FirstSaveNode = NewObject<UAnimGraphNode_SaveCachedPose>(GetTransientPackage());
    UAnimGraphNode_SaveCachedPose* SecondSaveNode = NewObject<UAnimGraphNode_SaveCachedPose>(GetTransientPackage());
    UAnimGraphNode_UseCachedPose* UseNode = NewObject<UAnimGraphNode_UseCachedPose>(GetTransientPackage());
    TestNotNull(TEXT("First save-cached-pose node created"), FirstSaveNode);
    TestNotNull(TEXT("Second save-cached-pose node created"), SecondSaveNode);
    TestNotNull(TEXT("Use-cached-pose node created"), UseNode);
    if (!FirstSaveNode || !SecondSaveNode || !UseNode)
    {
        return false;
    }

    const FString FirstSaveLine = Emitter.EmitNode(FirstSaveNode);
    const FString SecondSaveLine = Emitter.EmitNode(SecondSaveNode);
    const FString UseLine = Emitter.EmitNode(UseNode);
    TestTrue(TEXT("First save-cached-pose id uses mnemonic counter"),
        FirstSaveLine.Contains(TEXT("%save_cached_pose_0 = save_cached_pose")));
    TestTrue(TEXT("Second save-cached-pose id increments per mnemonic"),
        SecondSaveLine.Contains(TEXT("%save_cached_pose_1 = save_cached_pose")));
    TestTrue(TEXT("Use-cached-pose id has its own mnemonic counter"),
        UseLine.Contains(TEXT("%use_cached_pose_0 = use_cached_pose")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIRGrammarHarmonization_MGIRNameTokensAndQualifiedClasses,
    "PinWright.core.ir_grammar.MGIRNameTokensAndQualifiedClasses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FIRGrammarHarmonization_MGIRNameTokensAndQualifiedClasses::RunTest(const FString& Parameters)
{
    const FName MaterialName(*FString::Printf(
        TEXT("M_IrGrammarHarmonization_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), MaterialName);
    TestNotNull(TEXT("Transient material created"), Material);
    if (!Material)
    {
        return false;
    }

    UMaterialExpressionAdd* AddExpression = NewObject<UMaterialExpressionAdd>(Material, NAME_None, RF_Transactional);
    TestNotNull(TEXT("Add expression created"), AddExpression);
    UMaterialEditorOnlyData* EditorOnly = Material->GetEditorOnlyData();
    TestNotNull(TEXT("Material editor-only data present"), EditorOnly);
    if (!AddExpression || !EditorOnly)
    {
        return false;
    }
    EditorOnly->ExpressionCollection.AddExpression(AddExpression);

    const FMGIRDecompileResult Result = FMGIRDecompiler::DecompileMaterial(Material);
    TestTrue(TEXT("MGIR decompile succeeds"), Result.bSuccess);
    if (!Result.bSuccess)
    {
        return false;
    }

    const FString& MGIRText = Result.MGIRText;
    const FString ExpectedEntryPrefix = FString::Printf(
        TEXT("entry material %s"),
        *FIrTextUtils::FormatNameToken(Material->GetPathName()));
    const FString ExpectedCallClass = FString::Printf(
        TEXT("call %s("),
        *FIrTextUtils::FormatNameToken(UMaterialExpressionAdd::StaticClass()->GetPathName()));
    TestTrue(TEXT("MGIR entry target uses name-token formatting"),
        MGIRText.Contains(ExpectedEntryPrefix));
    TestFalse(TEXT("MGIR entry target is not double-quoted"),
        MGIRText.Contains(TEXT("entry material \"")));
    TestTrue(TEXT("MGIR call emits qualified material expression class name"),
        MGIRText.Contains(ExpectedCallClass));
    TestFalse(TEXT("MGIR call does not emit short Add alias"),
        MGIRText.Contains(TEXT("call Add(")));

    FMGIRParser Parser;
    TArray<FMGIREntryBlock> Blocks;
    TArray<FMGIRParseError> Errors;
    TestTrue(TEXT("MGIR parser accepts decompiler-produced name tokens and qualified classes"),
        Parser.Parse(MGIRText, Blocks, Errors));

    Blocks.Reset();
    Errors.Reset();
    TestFalse(TEXT("MGIR parser rejects double-quoted entry targets"),
        Parser.Parse(TEXT("entry material \"/Game/Test/M_Test.M_Test\" {\n}"), Blocks, Errors));

    Blocks.Reset();
    Errors.Reset();
    TestFalse(TEXT("MGIR parser rejects short call class names"),
        Parser.Parse(TEXT("entry material `/Game/Test/M_Test.M_Test` {\n    %add = call Add()\n}"), Blocks, Errors));

    return true;
}
