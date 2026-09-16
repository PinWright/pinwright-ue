// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "MGIR/MGIRCompiler.h"
#include "MGIR/MGIRDecompiler.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Tests/IrCore/IrTestFixture.h"
#include "UObject/Package.h"

// B-mgir-custom-node-inputs-and-newlines-lost. Two defects met in one node: MGIR never
// created the FCustomInput entries a Custom HLSL node's pins live in (so the translator
// emitted a function with no parameters and the shader failed on the first identifier the
// code names), and the compiler's string-literal decoder reversed only two of the escapes
// the decompiler writes (so every `\n` in the program reached the asset as a literal
// backslash-n and the HLSL collapsed onto one line). Both reported a successful compile.

namespace
{
// What the node should hold after compiling: real newlines, a real tab, real quotes and one
// real backslash. Covers the whole MGIR escape set except `\uNNNN`, which no HLSL needs.
const TCHAR* const MGIRCustomDecodedCode =
    TEXT("float2 d = UV.xy - 0.5;\n\tfloat2 p = float2(d.x, d.y);\n\tfloat r = length(p) * Scale; // \"tuned\" \\ ok\nreturn r;");

// The same program as it appears inside the document's double-quoted literal. Written out
// rather than produced with the encoder under test, so the test pins the wire format itself.
const TCHAR* const MGIRCustomEncodedCode =
    TEXT("float2 d = UV.xy - 0.5;\\n\\tfloat2 p = float2(d.x, d.y);\\n\\tfloat r = length(p) * Scale; // \\\"tuned\\\" \\\\ ok\\nreturn r;");

UMaterial* CreateCustomScratchMaterial(const IrTest::FScratchAsset& Scratch)
{
    UPackage* Package = CreatePackage(*Scratch.PackagePath);
    if (!Package)
    {
        return nullptr;
    }

    UMaterial* Material = NewObject<UMaterial>(
        Package,
        FName(*Scratch.AssetName),
        RF_Public | RF_Standalone);
    if (Material)
    {
        FAssetRegistryModule::AssetCreated(Material);
    }
    return Material;
}

template <typename TExpression>
TExpression* AddCustomTestExpression(UMaterial* Material)
{
    if (!Material || !Material->GetEditorOnlyData())
    {
        return nullptr;
    }

    TExpression* Expression = NewObject<TExpression>(Material, NAME_None, RF_Transactional);
    if (!Expression)
    {
        return nullptr;
    }

    Expression->MaterialExpressionGuid = FGuid::NewGuid();
    Material->GetEditorOnlyData()->ExpressionCollection.AddExpression(Expression);
    return Expression;
}

UMaterialExpressionCustom* FindCustomExpression(UMaterial* Material)
{
    if (!Material)
    {
        return nullptr;
    }

    for (UMaterialExpression* Expression : Material->GetExpressions())
    {
        if (UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Expression))
        {
            return Custom;
        }
    }
    return nullptr;
}

// Replaces every `%handle` outside a string literal with a fixed token. Two materials
// compiled from the same document carry different MaterialExpressionGuids, so the handles
// differ by construction; everything else on the line must not.
FString StripPinHandles(const FString& Line)
{
    FString Result;
    Result.Reserve(Line.Len());

    bool bInQuote = false;
    for (int32 Index = 0; Index < Line.Len(); ++Index)
    {
        const TCHAR Ch = Line[Index];
        if (Ch == TEXT('"') && (Index == 0 || Line[Index - 1] != TEXT('\\')))
        {
            bInQuote = !bInQuote;
        }

        if (!bInQuote && Ch == TEXT('%'))
        {
            Result += TEXT("%handle");
            while (Index + 1 < Line.Len()
                && (FChar::IsAlnum(Line[Index + 1]) || Line[Index + 1] == TEXT('_')))
            {
                ++Index;
            }
            continue;
        }

        Result.AppendChar(Ch);
    }

    return Result;
}

FString ExtractNormalizedCustomCallLine(const FString& MGIRText)
{
    TArray<FString> Lines;
    MGIRText.ParseIntoArrayLines(Lines, false);
    for (const FString& Line : Lines)
    {
        if (Line.Contains(TEXT("MaterialExpressionCustom")))
        {
            return StripPinHandles(Line.TrimStartAndEnd());
        }
    }
    return FString();
}
}

// The compile direction of both defects, on the shape the ticket reports: two named inputs
// the class default does not have, and a program that only works if its newlines survive.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMGIRCustomNamedInputsAndMultilineCodeTest,
    "PinWright.material.mgir.Custom.NamedInputsAndMultilineCodeCompile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMGIRCustomNamedInputsAndMultilineCodeTest::RunTest(const FString& Parameters)
{
    IrTest::FScratchAsset Scratch(TEXT("M_MGIRCustomCompile"));

    const FString MGIRText = FString::Printf(
        TEXT("entry material `%s` {\n")
        TEXT("    %%uv = call `/Script/Engine.MaterialExpressionTextureCoordinate`() @(-900, 0)\n")
        TEXT("    %%scale = constant Float1(2) @(-900, 200)\n")
        TEXT("    %%c = call `/Script/Engine.MaterialExpressionCustom`(UV: %%uv, Scale: %%scale, Code: \"%s\", OutputType: \"CMOT_Float1\") @(-500, 0)\n")
        TEXT("    output Opacity: %%c\n")
        TEXT("}\n"),
        *Scratch.PackagePath,
        MGIRCustomEncodedCode);

    FMGIRCompileOptions CompileOptions;
    CompileOptions.bRunLayout = false;
    CompileOptions.bSave = false;
    const FMGIRCompileResult CompileResult = FMGIRCompiler::Compile(MGIRText, CompileOptions);
    TestTrue(FString::Printf(TEXT("compile succeeds (code='%s' msg='%s')"),
        *CompileResult.ErrorCode,
        *CompileResult.ErrorMessage),
        CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        return false;
    }

    UMaterial* Material = LoadObject<UMaterial>(nullptr, *Scratch.PackagePath);
    UMaterialExpressionCustom* Custom = FindCustomExpression(Material);
    TestNotNull(TEXT("compiled Custom expression exists"), Custom);
    if (!Custom)
    {
        return false;
    }

    // The pins the document declared, in the order it declared them. The class default is a
    // single input whose name is empty, and the translator skips every unnamed input -- which
    // is how a "successful" compile used to produce a Custom function with no parameters.
    TestEqual(TEXT("declared input count"), Custom->Inputs.Num(), 2);
    if (Custom->Inputs.Num() != 2)
    {
        return false;
    }

    TestEqual(TEXT("first input is named UV"), Custom->Inputs[0].InputName, FName(TEXT("UV")));
    TestEqual(TEXT("second input is named Scale"), Custom->Inputs[1].InputName, FName(TEXT("Scale")));
    TestNotNull(TEXT("UV input is wired"), Custom->Inputs[0].Input.Expression);
    TestNotNull(TEXT("Scale input is wired"), Custom->Inputs[1].Input.Expression);
    TestTrue(TEXT("UV is wired to the texture coordinate"),
        Custom->Inputs[0].Input.Expression != nullptr
            && Custom->Inputs[0].Input.Expression->IsA<UMaterialExpressionTextureCoordinate>());
    TestTrue(TEXT("Scale is wired to the constant"),
        Custom->Inputs[1].Input.Expression != nullptr
            && Custom->Inputs[1].Input.Expression->IsA<UMaterialExpressionConstant>());

    TestEqual(TEXT("HLSL survives the escape decode"), Custom->Code, FString(MGIRCustomDecodedCode));
    TestTrue(TEXT("HLSL carries real newlines, not backslash-n"),
        Custom->Code.Contains(TEXT("\n")) && !Custom->Code.Contains(TEXT("\\n")));
    TestEqual(TEXT("output type round-trips"), static_cast<int32>(Custom->OutputType.GetValue()),
        static_cast<int32>(CMOT_Float1));

    return true;
}

// Decompile emits the same declaration it now reads: named pin arguments, and no restated
// `Inputs: [...]` array. The second recompile is what proves the document is readable by the
// compiler that wrote it -- the property the ticket's report failed at.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMGIRCustomDecompileRoundTripTest,
    "PinWright.material.mgir.Custom.DecompileRoundTripsInputsAndCode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMGIRCustomDecompileRoundTripTest::RunTest(const FString& Parameters)
{
    IrTest::FScratchAsset SourceScratch(TEXT("M_MGIRCustomSource"));
    UMaterial* SourceMaterial = CreateCustomScratchMaterial(SourceScratch);
    TestNotNull(TEXT("source material created"), SourceMaterial);
    if (!SourceMaterial || !SourceMaterial->GetEditorOnlyData())
    {
        return false;
    }

    UMaterialExpressionTextureCoordinate* Coordinate =
        AddCustomTestExpression<UMaterialExpressionTextureCoordinate>(SourceMaterial);
    UMaterialExpressionConstant* Constant = AddCustomTestExpression<UMaterialExpressionConstant>(SourceMaterial);
    UMaterialExpressionCustom* SourceCustom = AddCustomTestExpression<UMaterialExpressionCustom>(SourceMaterial);
    TestNotNull(TEXT("texture coordinate created"), Coordinate);
    TestNotNull(TEXT("constant created"), Constant);
    TestNotNull(TEXT("source Custom created"), SourceCustom);
    if (!Coordinate || !Constant || !SourceCustom)
    {
        return false;
    }

    Constant->R = 2.0f;
    SourceCustom->Code = MGIRCustomDecodedCode;
    SourceCustom->OutputType = CMOT_Float1;
    SourceCustom->Inputs.Reset(2);
    SourceCustom->Inputs.AddDefaulted_GetRef().InputName = FName(TEXT("UV"));
    SourceCustom->Inputs.AddDefaulted_GetRef().InputName = FName(TEXT("Scale"));
    SourceCustom->Inputs[0].Input.Expression = Coordinate;
    SourceCustom->Inputs[1].Input.Expression = Constant;

    const FMGIRDecompileResult DecompileResult = FMGIRDecompiler::DecompileMaterial(SourceMaterial);
    TestTrue(TEXT("decompile succeeds"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess)
    {
        return false;
    }

    TestTrue(TEXT("decompile names the UV pin as a call argument"),
        DecompileResult.MGIRText.Contains(TEXT("UV: %")));
    TestTrue(TEXT("decompile names the Scale pin as a call argument"),
        DecompileResult.MGIRText.Contains(TEXT("Scale: %")));
    TestTrue(TEXT("decompile escapes the newlines in the HLSL"),
        DecompileResult.MGIRText.Contains(MGIRCustomEncodedCode));
    // The reflected array restates the same names and connections in a struct-literal form
    // the compiler cannot read, which is what made a decompiled Custom node fail its own
    // recompile at MGIR_INPUT_NOT_FOUND.
    TestFalse(TEXT("decompile suppresses the raw Inputs array"),
        DecompileResult.MGIRText.Contains(TEXT("Inputs:")));

    IrTest::FScratchAsset TargetScratch(TEXT("M_MGIRCustomTarget"));
    const FString TargetMGIR = IrTest::ReplaceScratchAssetName(
        DecompileResult.MGIRText,
        SourceScratch,
        TargetScratch);

    FMGIRCompileOptions CompileOptions;
    CompileOptions.bRunLayout = false;
    CompileOptions.bSave = false;
    const FMGIRCompileResult CompileResult = FMGIRCompiler::Compile(TargetMGIR, CompileOptions);
    TestTrue(FString::Printf(TEXT("recompile succeeds (code='%s' msg='%s')"),
        *CompileResult.ErrorCode,
        *CompileResult.ErrorMessage),
        CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        return false;
    }

    UMaterial* TargetMaterial = LoadObject<UMaterial>(nullptr, *TargetScratch.PackagePath);
    UMaterialExpressionCustom* TargetCustom = FindCustomExpression(TargetMaterial);
    TestNotNull(TEXT("recompiled Custom expression exists"), TargetCustom);
    if (!TargetCustom)
    {
        return false;
    }

    TestEqual(TEXT("recompiled input count"), TargetCustom->Inputs.Num(), 2);
    if (TargetCustom->Inputs.Num() == 2)
    {
        TestEqual(TEXT("recompiled first input name"), TargetCustom->Inputs[0].InputName, FName(TEXT("UV")));
        TestEqual(TEXT("recompiled second input name"), TargetCustom->Inputs[1].InputName, FName(TEXT("Scale")));
        TestTrue(TEXT("recompiled UV stays wired to the texture coordinate"),
            TargetCustom->Inputs[0].Input.Expression != nullptr
                && TargetCustom->Inputs[0].Input.Expression->IsA<UMaterialExpressionTextureCoordinate>());
        TestTrue(TEXT("recompiled Scale stays wired to the constant"),
            TargetCustom->Inputs[1].Input.Expression != nullptr
                && TargetCustom->Inputs[1].Input.Expression->IsA<UMaterialExpressionConstant>());
    }
    TestEqual(TEXT("recompiled HLSL matches the source"), TargetCustom->Code, FString(MGIRCustomDecodedCode));

    const FMGIRDecompileResult SecondDecompile = FMGIRDecompiler::DecompileMaterial(TargetMaterial);
    TestTrue(TEXT("second decompile succeeds"), SecondDecompile.bSuccess);
    if (!SecondDecompile.bSuccess)
    {
        return false;
    }

    const FString FirstCall = ExtractNormalizedCustomCallLine(DecompileResult.MGIRText);
    const FString SecondCall = ExtractNormalizedCustomCallLine(SecondDecompile.MGIRText);
    TestTrue(TEXT("first decompile carries a Custom call line"), !FirstCall.IsEmpty());
    TestEqual(TEXT("the Custom node decompiles identically after a round trip"), SecondCall, FirstCall);

    return true;
}

// The escape set is a property of MGIR string literals, not of the Custom node: the same
// decoder reads every literal in the document.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMGIRStringLiteralEscapeSetTest,
    "PinWright.material.mgir.StringLiteral.EscapeSetDecodes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMGIRStringLiteralEscapeSetTest::RunTest(const FString& Parameters)
{
    IrTest::FScratchAsset Scratch(TEXT("M_MGIRStringLiteral"));

    const FString EncodedDesc = TEXT("line1\\nline2\\tcol \\\"quoted\\\" back\\\\slash");
    const FString DecodedDesc = TEXT("line1\nline2\tcol \"quoted\" back\\slash");

    const FString MGIRText = FString::Printf(
        TEXT("entry material `%s` {\n")
        TEXT("    %%a = call `/Script/Engine.MaterialExpressionAdd`(Desc: \"%s\") @(0, 0)\n")
        TEXT("}\n"),
        *Scratch.PackagePath,
        *EncodedDesc);

    FMGIRCompileOptions CompileOptions;
    CompileOptions.bRunLayout = false;
    CompileOptions.bSave = false;
    const FMGIRCompileResult CompileResult = FMGIRCompiler::Compile(MGIRText, CompileOptions);
    TestTrue(FString::Printf(TEXT("compile succeeds (code='%s' msg='%s')"),
        *CompileResult.ErrorCode,
        *CompileResult.ErrorMessage),
        CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        return false;
    }

    UMaterial* Material = LoadObject<UMaterial>(nullptr, *Scratch.PackagePath);
    TestNotNull(TEXT("compiled material loads"), Material);
    if (!Material)
    {
        return false;
    }

    UMaterialExpression* Added = nullptr;
    for (UMaterialExpression* Expression : Material->GetExpressions())
    {
        if (Expression && Expression->IsA<UMaterialExpressionAdd>())
        {
            Added = Expression;
            break;
        }
    }
    TestNotNull(TEXT("compiled expression exists"), Added);
    if (!Added)
    {
        return false;
    }

    TestEqual(TEXT("every escape in the literal decodes"), Added->Desc, DecodedDesc);

    // ... and the decompiler writes back exactly what the compiler just read.
    const FMGIRDecompileResult DecompileResult = FMGIRDecompiler::DecompileMaterial(Material);
    TestTrue(TEXT("decompile succeeds"), DecompileResult.bSuccess);
    TestTrue(TEXT("decompile re-emits the same escaped literal"),
        DecompileResult.MGIRText.Contains(FString::Printf(TEXT("Desc: \"%s\""), *EncodedDesc)));

    return true;
}
