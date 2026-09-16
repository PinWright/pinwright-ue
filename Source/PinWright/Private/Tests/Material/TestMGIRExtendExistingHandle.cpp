// Copyright (c) 2026 Alexander Penkin. MIT License.

// Extend mode has no update-by-handle path: FMGIRExpressionEmitter's entire API is Emit*,
// and every one of those creates. A document naming an expression that already exists was
// therefore silently duplicated -- the caller's new value landed on a fresh orphan while the
// handle they named kept its old value and stayed wired to the output, and the call still
// reported blocksCompiled/expressionsCreated/consumerRefresh.complete as a success. It was
// caught in production only by a destructive control; every returned field read as success.
//
// These tests assert the EXPRESSION GRAPH, not the return value. A test that asserted only
// bSuccess would have passed against the broken compiler.
#include "Misc/AutomationTest.h"

#include "MGIR/MGIRCompiler.h"
#include "MGIR/MGIRDecompiler.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Tests/IrCore/IrTestFixture.h"

namespace
{
int32 CountExpressions(UMaterial* Material)
{
    int32 Count = 0;
    if (Material)
    {
        for (UMaterialExpression* Expression : Material->GetExpressions())
        {
            if (Expression)
            {
                ++Count;
            }
        }
    }
    return Count;
}

UMaterialExpressionConstant3Vector* FindOnlyConstant3(UMaterial* Material)
{
    UMaterialExpressionConstant3Vector* Found = nullptr;
    if (!Material)
    {
        return nullptr;
    }

    for (UMaterialExpression* Expression : Material->GetExpressions())
    {
        if (UMaterialExpressionConstant3Vector* Constant =
            Cast<UMaterialExpressionConstant3Vector>(Expression))
        {
            if (Found)
            {
                // A second one means the duplicate this guard exists to prevent.
                return nullptr;
            }
            Found = Constant;
        }
    }
    return Found;
}

// Builds a one-constant material through the compiler itself, then decompiles it so the
// test uses the same GUID-derived handle a real caller would have edited.
bool BuildSeedMaterial(
    const IrTest::FScratchAsset& Scratch,
    UMaterial*& OutMaterial,
    FString& OutDecompiledText,
    FString& OutError)
{
    const FString SeedText = FString::Printf(
        TEXT("entry material `%s` {\n")
        TEXT("    %%seed = constant Float3(0.2, 0.4, 1.0) @(0, 0)\n")
        TEXT("    output BaseColor: %%seed\n")
        TEXT("}\n"),
        *Scratch.PackagePath);

    FMGIRCompileOptions SeedOptions;
    SeedOptions.bRunLayout = false;
    SeedOptions.bSave = false;
    const FMGIRCompileResult SeedResult = FMGIRCompiler::Compile(SeedText, SeedOptions);
    if (!SeedResult.bSuccess)
    {
        OutError = FString::Printf(TEXT("seed compile failed: %s %s"),
            *SeedResult.ErrorCode, *SeedResult.ErrorMessage);
        return false;
    }

    OutMaterial = LoadObject<UMaterial>(nullptr, *Scratch.PackagePath);
    if (!OutMaterial)
    {
        OutError = TEXT("seed material did not load");
        return false;
    }

    const FMGIRDecompileResult Decompiled = FMGIRDecompiler::DecompileMaterial(OutMaterial);
    if (!Decompiled.bSuccess)
    {
        OutError = TEXT("seed decompile failed");
        return false;
    }

    OutDecompiledText = Decompiled.MGIRText;
    return true;
}
}

// The production failure, reduced: re-compile a decompiled document in Extend mode with one
// constant value changed. The handle names an expression that already exists, so the compile
// must be refused and the graph must be left exactly as it was.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMGIRExtendRefusesExistingHandleTest,
    "PinWright.material.mgir.Extend.RefusesExistingHandle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMGIRExtendRefusesExistingHandleTest::RunTest(const FString& Parameters)
{
    IrTest::FScratchAsset Scratch(TEXT("M_MGIRExtendExistingHandle"));

    UMaterial* Material = nullptr;
    FString DecompiledText;
    FString SeedError;
    if (!BuildSeedMaterial(Scratch, Material, DecompiledText, SeedError))
    {
        AddError(SeedError);
        return false;
    }

    TestEqual(TEXT("seed graph has exactly one expression"), CountExpressions(Material), 1);

    UMaterialExpressionConstant3Vector* SeedConstant = FindOnlyConstant3(Material);
    TestNotNull(TEXT("seed constant exists"), SeedConstant);
    if (!SeedConstant)
    {
        return false;
    }

    // The decompiled text names the constant by its persisted MaterialExpressionGuid, which
    // is precisely the handle a caller edits in a decompile -> edit -> compile round trip.
    const FString EditedText = DecompiledText.Replace(
        TEXT("0.2, 0.4, 1.0"),
        TEXT("0.9, 0.1, 0.1"));
    TestNotEqual(TEXT("edited document actually differs from the decompiled one"),
        EditedText, DecompiledText);

    FMGIRCompileOptions ExtendOptions;
    ExtendOptions.Mode = EMGIRCompileMode::Extend;
    ExtendOptions.bRunLayout = false;
    ExtendOptions.bSave = false;
    const FMGIRCompileResult ExtendResult = FMGIRCompiler::Compile(EditedText, ExtendOptions);

    TestFalse(TEXT("Extend refuses a document naming an existing handle"), ExtendResult.bSuccess);
    TestEqual(TEXT("refusal carries the Extend-cannot-update code"),
        ExtendResult.ErrorCode, FString(TEXT("MGIR_EXTEND_CANNOT_UPDATE")));

    // The graph assertions are the point. Before the guard these three all failed: the count
    // went to 2, the named handle kept 0.2/0.4/1.0, and the intended value sat on an orphan.
    TestEqual(TEXT("no duplicate expression was created"), CountExpressions(Material), 1);

    UMaterialExpressionConstant3Vector* AfterConstant = FindOnlyConstant3(Material);
    TestNotNull(TEXT("still exactly one constant in the graph"), AfterConstant);
    if (!AfterConstant)
    {
        return false;
    }

    TestEqual(TEXT("refused compile left R untouched"), AfterConstant->Constant.R, 0.2f);
    TestEqual(TEXT("refused compile left G untouched"), AfterConstant->Constant.G, 0.4f);
    TestEqual(TEXT("refused compile left B untouched"), AfterConstant->Constant.B, 1.0f);
    return true;
}

// The guard must not cost Extend its actual documented job: adding NEW expressions to an
// existing graph. Without this, refusing every handle would look like a fix and quietly
// make the mode useless.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMGIRExtendStillAddsNewExpressionsTest,
    "PinWright.material.mgir.Extend.StillAddsNewExpressions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMGIRExtendStillAddsNewExpressionsTest::RunTest(const FString& Parameters)
{
    IrTest::FScratchAsset Scratch(TEXT("M_MGIRExtendAddsNew"));

    UMaterial* Material = nullptr;
    FString DecompiledText;
    FString SeedError;
    if (!BuildSeedMaterial(Scratch, Material, DecompiledText, SeedError))
    {
        AddError(SeedError);
        return false;
    }

    TestEqual(TEXT("seed graph has exactly one expression"), CountExpressions(Material), 1);

    // A symbol that names nothing in the target graph: this is what Extend is for.
    const FString AddText = FString::Printf(
        TEXT("entry material `%s` {\n")
        TEXT("    %%freshlyadded = constant Float3(0.0, 1.0, 0.0) @(0, 200)\n")
        TEXT("}\n"),
        *Scratch.PackagePath);

    FMGIRCompileOptions ExtendOptions;
    ExtendOptions.Mode = EMGIRCompileMode::Extend;
    ExtendOptions.bRunLayout = false;
    ExtendOptions.bSave = false;
    const FMGIRCompileResult ExtendResult = FMGIRCompiler::Compile(AddText, ExtendOptions);

    TestTrue(FString::Printf(TEXT("Extend still adds a new expression (code='%s' msg='%s')"),
        *ExtendResult.ErrorCode, *ExtendResult.ErrorMessage),
        ExtendResult.bSuccess);
    TestEqual(TEXT("the added expression reached the graph"), CountExpressions(Material), 2);
    return true;
}
