// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

// UMaterialExpressionSubstrateSlabBSDF (and Materials/MaterialExpressionSubstrate.h) are UE 5.4+;
// Substrate material expressions do not exist on 5.3. The whole test is compiled out there.
#if __has_include("Materials/MaterialExpressionSubstrate.h")

#include "MGIR/MGIRCompiler.h"
#include "MGIR/MGIRDecompiler.h"
#include "MGIR/MGIRParser.h"
#include "MGIR/MGIRSubstrateSugar.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionSubstrate.h"
#include "HAL/IConsoleManager.h"
#include "Tests/IrCore/IrTestFixture.h"
#include "UObject/Package.h"

namespace
{
UMaterialExpressionSubstrateSlabBSDF* AddTransientSlabExpression(UMaterial* Material)
{
    if (!Material)
    {
        return nullptr;
    }

    UMaterialExpressionSubstrateSlabBSDF* Slab =
        NewObject<UMaterialExpressionSubstrateSlabBSDF>(Material, NAME_None, RF_Transactional);
    if (!Slab)
    {
        return nullptr;
    }

    Slab->MaterialExpressionGuid = FGuid::NewGuid();
    if (UMaterialEditorOnlyData* EditorOnly = Material->GetEditorOnlyData())
    {
        EditorOnly->ExpressionCollection.AddExpression(Slab);
    }
    return Slab;
}

bool ContainsSlabExpression(UMaterial* Material)
{
    if (!Material)
    {
        return false;
    }

    for (UMaterialExpression* Expression : Material->GetExpressions())
    {
        if (Expression && Expression->IsA<UMaterialExpressionSubstrateSlabBSDF>())
        {
            return true;
        }
    }
    return false;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMGIRSubstrateSugar_DecompileSlabAlias,
    "PinWright.material.mgir.SubstrateSugar.DecompileSlab",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMGIRSubstrateSugar_DecompileSlabAlias::RunTest(const FString& Parameters)
{
    UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
    TestNotNull(TEXT("Transient material created"), Material);
    if (!Material)
    {
        return false;
    }

    UMaterialExpressionSubstrateSlabBSDF* Slab = AddTransientSlabExpression(Material);
    TestNotNull(TEXT("Substrate slab expression created"), Slab);
    if (!Slab)
    {
        return false;
    }

    FMGIRDecompileOptions Options;
    Options.bEmitSubstrateSugar = true;
    const FMGIRDecompileResult Result = FMGIRDecompiler::DecompileMaterial(Material, Options);
    TestTrue(TEXT("decompile succeeds"), Result.bSuccess);
    if (!Result.bSuccess)
    {
        return false;
    }

    TestTrue(TEXT("sugar output contains slab alias"),
        Result.MGIRText.Contains(TEXT(" = slab(")));
    TestFalse(TEXT("sugar output omits canonical slab class call"),
        Result.MGIRText.Contains(TEXT("call `/Script/Engine.MaterialExpressionSubstrateSlabBSDF`(")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMGIRSubstrateSugar_CompileSlabAlias,
    "PinWright.material.mgir.SubstrateSugar.CompileSlab",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMGIRSubstrateSugar_CompileSlabAlias::RunTest(const FString& Parameters)
{
    IrTest::FScratchAsset Scratch(TEXT("M_MGIRSubstrateSugarCompile"));
    const FString Text = FString::Printf(
        TEXT("entry material `%s` {\n")
        TEXT("    %%slab = slab()\n")
        TEXT("}\n"),
        *Scratch.PackagePath);

    FMGIRCompileOptions Options;
    Options.bRunLayout = false;
    Options.bSave = false;
    const FMGIRCompileResult Result = FMGIRCompiler::Compile(Text, Options);
    TestTrue(FString::Printf(TEXT("compile succeeds (code='%s' msg='%s')"),
        *Result.ErrorCode,
        *Result.ErrorMessage),
        Result.bSuccess);
    if (!Result.bSuccess)
    {
        return false;
    }

    UMaterial* Material = LoadObject<UMaterial>(nullptr, *Scratch.PackagePath);
    TestNotNull(TEXT("compiled material loads"), Material);
    if (!Material)
    {
        return false;
    }

    TestTrue(TEXT("compiled material contains a Substrate slab expression"),
        ContainsSlabExpression(Material));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMGIRSubstrateSugar_ParserAliasesAndCanonicalSubstrateCall,
    "PinWright.material.mgir.SubstrateSugar.ParserAliasesAndCanonicalSubstrateCall",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMGIRSubstrateSugar_ParserAliasesAndCanonicalSubstrateCall::RunTest(const FString& Parameters)
{
    FMGIRParser Parser;
    TArray<FMGIREntryBlock> Blocks;
    TArray<FMGIRParseError> Errors;

    for (const FMGIRSubstrateSugarMapping& Mapping : FMGIRSubstrateSugar::GetMappings())
    {
        const FString Text = FString::Printf(
            TEXT("entry material `/Game/Test/M_Test.M_Test` {\n")
            TEXT("    %%node = %s()\n")
            TEXT("}\n"),
            Mapping.Alias);

        Blocks.Reset();
        Errors.Reset();
        TestTrue(FString::Printf(TEXT("parser accepts Substrate alias '%s'"), Mapping.Alias),
            Parser.Parse(Text, Blocks, Errors));
        if (Blocks.Num() == 1 && Blocks[0].Instructions.Num() == 1)
        {
            TestEqual(TEXT("alias lowers to canonical class path"),
                Blocks[0].Instructions[0].SymbolName,
                FString(Mapping.ClassPath));
        }
    }

    Blocks.Reset();
    Errors.Reset();
    const FString CanonicalShortText =
        TEXT("entry material `/Game/Test/M_Test.M_Test` {\n")
        TEXT("    %slab = call SubstrateSlabBSDF()\n")
        TEXT("}\n");
    TestTrue(TEXT("parser accepts short canonical Substrate class names"),
        Parser.Parse(CanonicalShortText, Blocks, Errors));
    if (Blocks.Num() == 1 && Blocks[0].Instructions.Num() == 1)
    {
        TestEqual(TEXT("short canonical Substrate call expands to qualified class path"),
            Blocks[0].Instructions[0].SymbolName,
            FString(TEXT("/Script/Engine.MaterialExpressionSubstrateSlabBSDF")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMGIRSubstrateSugar_DisabledCVarWarning,
    "PinWright.material.mgir.SubstrateSugar.DisabledCVarWarning",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMGIRSubstrateSugar_DisabledCVarWarning::RunTest(const FString& Parameters)
{
    UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
    TestNotNull(TEXT("Transient material created"), Material);
    if (!Material)
    {
        return false;
    }

    UMaterialExpressionSubstrateSlabBSDF* Slab = AddTransientSlabExpression(Material);
    TestNotNull(TEXT("Substrate slab expression created"), Slab);
    if (!Slab)
    {
        return false;
    }

    IConsoleVariable* SubstrateCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Substrate"));
    TestNotNull(TEXT("r.Substrate cvar exists"), SubstrateCVar);
    if (!SubstrateCVar)
    {
        return false;
    }

    const int32 PreviousValue = SubstrateCVar->GetInt();
    SubstrateCVar->Set(0, ECVF_SetByCode);
    const FMGIRDecompileResult Result = FMGIRDecompiler::DecompileMaterial(Material);
    SubstrateCVar->Set(PreviousValue, ECVF_SetByCode);

    TestTrue(TEXT("decompile succeeds while r.Substrate=0"), Result.bSuccess);
    bool bFoundWarning = false;
    for (const FString& Warning : Result.Warnings)
    {
        if (Warning.Contains(TEXT("r.Substrate=0")))
        {
            bFoundWarning = true;
            break;
        }
    }
    TestTrue(TEXT("warning mentions r.Substrate=0"), bFoundWarning);

    return Result.bSuccess && bFoundWarning;
}

#endif // __has_include("Materials/MaterialExpressionSubstrate.h")
