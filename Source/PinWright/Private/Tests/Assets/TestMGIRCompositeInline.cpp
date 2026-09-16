// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-mgir-composite-subgraph-compile-fail.
//
// The decompiler used to emit `subgraph "Name" { ... }` for
// UMaterialExpressionComposite ("Collapse Nodes") groups, and the compiler
// hard-rejected that opcode with MGIR_COMPOSITE_NOT_SUPPORTED. Any material
// with a collapsed group therefore produced uncompilable MGIR. The fix
// flattens composite contents at decompile so the inner expressions become
// top-level entries and the `subgraph` keyword never appears in the output.
//
// Counterfactual: if the flatten pre-pass is reverted, decompile re-emits
// `subgraph "Name" { ... }`, and the compile call returns
// MGIR_COMPOSITE_NOT_SUPPORTED.
#include "Misc/AutomationTest.h"


#include "MGIR/MGIRCompiler.h"
#include "MGIR/MGIRDecompiler.h"
#include "IrCore/IrTextUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionComposite.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Factories/MaterialFactoryNew.h"
#include "Tests/IrCore/IrTestFixture.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMGIRCompositeInlineFlattenDecompileTest,
    "PinWright.material.mgir.CompositeInlineFlatten",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMGIRCompositeInlineFlattenDecompileTest::RunTest(const FString& Parameters)
{
    IrTest::FScratchAsset SourceScratch(TEXT("M_MGIRCompositeSrc"));
    IrTest::FScratchAsset TargetScratch(TEXT("M_MGIRCompositeDst"));

    UMaterial* SourceMaterial =
        IrTest::CreateFactoryScratchAsset<UMaterial, UMaterialFactoryNew>(SourceScratch);
    TestNotNull(TEXT("source material created"), SourceMaterial);
    if (!SourceMaterial)
    {
        return false;
    }

    // Inner expressions (Constant + Add) plus a parent Composite. The two
    // inner expressions point their SubgraphExpression at the composite so
    // the decompiler treats them as collapsed-into-the-composite siblings —
    // the same shape produced by UE's "Collapse Nodes" feature.
    UMaterialExpressionComposite* Composite =
        NewObject<UMaterialExpressionComposite>(SourceMaterial, NAME_None, RF_Transactional);
    Composite->MaterialExpressionGuid = FGuid::NewGuid();
    Composite->SubgraphName = TEXT("InlineGroup");
    Composite->MaterialExpressionEditorX = 100;
    Composite->MaterialExpressionEditorY = 200;

    UMaterialExpressionConstant* InnerConstant =
        NewObject<UMaterialExpressionConstant>(SourceMaterial, NAME_None, RF_Transactional);
    InnerConstant->MaterialExpressionGuid = FGuid::NewGuid();
    InnerConstant->R = 0.5f;
    InnerConstant->SubgraphExpression = Composite;
    InnerConstant->MaterialExpressionEditorX = 200;
    InnerConstant->MaterialExpressionEditorY = 200;

    UMaterialExpressionAdd* InnerAdd =
        NewObject<UMaterialExpressionAdd>(SourceMaterial, NAME_None, RF_Transactional);
    InnerAdd->MaterialExpressionGuid = FGuid::NewGuid();
    InnerAdd->ConstA = 0.25f;
    InnerAdd->ConstB = 0.75f;
    InnerAdd->A.Connect(0, InnerConstant);
    InnerAdd->SubgraphExpression = Composite;
    InnerAdd->MaterialExpressionEditorX = 400;
    InnerAdd->MaterialExpressionEditorY = 200;

    UMaterialEditorOnlyData* SourceEditorData = SourceMaterial->GetEditorOnlyData();
    TestNotNull(TEXT("source editor-only data present"), SourceEditorData);
    if (!SourceEditorData)
    {
        return false;
    }

    SourceEditorData->ExpressionCollection.AddExpression(Composite);
    SourceEditorData->ExpressionCollection.AddExpression(InnerConstant);
    SourceEditorData->ExpressionCollection.AddExpression(InnerAdd);

    // Wire the inner Add into a material output pin so the decompiler emits
    // an `output ...` line. The wire crosses the composite boundary; under
    // the flatten fix this becomes a normal cross-expression reference.
    SourceEditorData->EmissiveColor.Connect(0, InnerAdd);

    // Production decompile path.
    FMGIRDecompileResult DecompileResult = FMGIRDecompiler::DecompileMaterial(SourceMaterial);
    TestTrue(TEXT("decompile reports success"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess)
    {
        return false;
    }

    const FString& MGIRText = DecompileResult.MGIRText;

    // The whole point of the fix: no `subgraph ` keyword in the produced
    // text. Trailing space avoids false positives on the word as part of an
    // identifier.
    TestFalse(TEXT("decompile output contains no `subgraph ` keyword"),
        MGIRText.Contains(TEXT("subgraph ")));

    // The inner expressions still appear at the parent level. Constant and
    // Add are emitted as their characteristic MGIR shapes.
    TestTrue(TEXT("inner constant emitted at top level"),
        MGIRText.Contains(TEXT("constant Float1(0.5")));
    const FString ExpectedAddCall = FString::Printf(
        TEXT("call %s("),
        *FIrTextUtils::FormatNameToken(UMaterialExpressionAdd::StaticClass()->GetPathName()));
    TestTrue(TEXT("inner Add emitted at top level"),
        MGIRText.Contains(ExpectedAddCall));

    // Repoint the entry to a fresh asset path so the compile path creates a
    // separate material rather than mutating the source. The helper swaps the
    // GUID-stamped basename, covering both package-path and dotted object-name
    // occurrences in one pass.
    FString CompiledText = IrTest::ReplaceScratchAssetName(MGIRText, SourceScratch, TargetScratch);
    TestTrue(TEXT("substituted target path into MGIR text"),
        CompiledText.Contains(TargetScratch.PackagePath));

    // Production compile path.
    FMGIRCompileOptions CompileOptions;
    CompileOptions.bRunLayout = false;
    CompileOptions.bSave = false;
    FMGIRCompileResult CompileResult = FMGIRCompiler::Compile(CompiledText, CompileOptions);
    TestTrue(FString::Printf(TEXT("compile succeeds (code='%s' msg='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage),
        CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        return false;
    }
    TestNotEqual(TEXT("compile error code is not MGIR_COMPOSITE_NOT_SUPPORTED"),
        CompileResult.ErrorCode, FString(TEXT("MGIR_COMPOSITE_NOT_SUPPORTED")));

    UMaterial* TargetMaterial = LoadObject<UMaterial>(nullptr, *TargetScratch.PackagePath);
    TestNotNull(TEXT("recompiled material asset loaded"), TargetMaterial);
    if (!TargetMaterial)
    {
        return false;
    }

    // The recompiled material has the inner expressions but no Composite —
    // composites disappear at decompile so they cannot reappear at compile.
    int32 CompositeCount = 0;
    int32 ConstantCount = 0;
    int32 AddCount = 0;
    for (UMaterialExpression* Expression : TargetMaterial->GetExpressions())
    {
        if (!Expression)
        {
            continue;
        }
        if (Expression->IsA<UMaterialExpressionComposite>())
        {
            ++CompositeCount;
        }
        else if (Expression->IsA<UMaterialExpressionConstant>())
        {
            ++ConstantCount;
        }
        else if (Expression->IsA<UMaterialExpressionAdd>())
        {
            ++AddCount;
        }
    }
    TestEqual(TEXT("recompiled material has no Composite expressions"), CompositeCount, 0);
    TestTrue(TEXT("recompiled material has Constant inner expression"), ConstantCount >= 1);
    TestTrue(TEXT("recompiled material has Add inner expression"), AddCount >= 1);

    return true;
}
