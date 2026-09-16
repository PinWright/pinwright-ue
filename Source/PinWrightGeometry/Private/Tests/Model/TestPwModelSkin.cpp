// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "Model/PwModelAst.h"
#include "Model/PwModelParser.h"
#include "PwSource/PwDiagnostic.h"

namespace
{
const FPwDiagnostic* PwModelSkinTest_FindCode(const TArray<FPwDiagnostic>& Diagnostics,
                                              const TCHAR* Code)
{
    for (const FPwDiagnostic& Diagnostic : Diagnostics)
    {
        if (Diagnostic.Code == Code)
        {
            return &Diagnostic;
        }
    }
    return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserSkinBlockTest,
    "PinWright.Model.Parser.SkinBlockStoresTypedRulesAndPartBoneBinding",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserSkinBlockTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bParsed = FPwModelParser::Parse(
        TEXT("pwmodel 0\n")
        TEXT("use skeleton from \"/Game/Rigs/Hero.Hero\"\n")
        TEXT("part torso bone=\"spine_02\" at=(0, 0, 110) {\n")
        TEXT("    box size=(30, 20, 40)\n")
        TEXT("}\n")
        TEXT("skin {\n")
        TEXT("    smooth max_influences=12 stiffness=1 method=geodesic_voxel voxel_resolution=1024\n")
        TEXT("}\n"),
        Document, Diagnostics);

    TestTrue(TEXT("a valid bone binding and smooth skin rule parse"), bParsed);
    TestEqual(TEXT("the valid skin fixture has no diagnostics"), Diagnostics.Num(), 0);
    TestEqual(TEXT("one bound part is captured"), Document.Parts.Num(), 1);

    if (Document.Parts.Num() == 1)
    {
        const FPwModelPart& Part = Document.Parts[0];
        TestEqual(TEXT("bone= is stored as the decoded binding name"),
            Part.BoneBinding, FString(TEXT("spine_02")));
        TestFalse(TEXT("bone= is not left in the transform map"),
            Part.Transform.Contains(TEXT("bone")));
        TestTrue(TEXT("the part transform still carries at="),
            Part.Transform.Contains(TEXT("at")));
    }

    TestTrue(TEXT("the model stores a skin block"), Document.Skin.IsSet());
    if (Document.Skin.IsSet())
    {
        const FPwOp& Skin = Document.Skin.GetValue();
        TestEqual(TEXT("the outer skin node keeps its model-level name"),
            Skin.OpName, FString(TEXT("skin")));
        TestEqual(TEXT("the skin block stores one smooth rule"), Skin.Children.Num(), 1);
        if (Skin.Children.Num() == 1)
        {
            const FPwOp& Smooth = Skin.Children[0];
            TestEqual(TEXT("the rule name is smooth"), Smooth.OpName, FString(TEXT("smooth")));
            const FPwValue* Method = Smooth.Params.Find(TEXT("method"));
            TestNotNull(TEXT("smooth stores method"), Method);
            if (Method)
            {
                TestEqual(TEXT("method remains an identifier"), Method->Type, EPwValueType::Identifier);
                TestEqual(TEXT("method keeps its authored enum"), Method->Text,
                    FString(TEXT("geodesic_voxel")));
            }
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserBoneBindingLiteralTest,
    "PinWright.Model.Parser.BoneBindingRequiresStringLiteral",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserBoneBindingLiteralTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bParsed = FPwModelParser::Parse(
        TEXT("pwmodel 0\n")
        TEXT("part torso bone=spine_02 {\n")
        TEXT("    box size=(30, 20, 40)\n")
        TEXT("}\n"),
        Document, Diagnostics);

    TestFalse(TEXT("a bare identifier is not a bone binding literal"), bParsed);
    TestNotNull(TEXT("the bare bone name reports a bad value"),
        PwModelSkinTest_FindCode(Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_VALUE));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserSkinUseKindsTest,
    "PinWright.Model.Parser.UseKindsOnlyPermitSkeleton",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserSkinUseKindsTest::RunTest(const FString& Parameters)
{
    const TCHAR* const InvalidKinds[] = { TEXT("skin"), TEXT("part") };
    for (const TCHAR* InvalidKind : InvalidKinds)
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        const FString Source = FString::Printf(
            TEXT("pwmodel 0\nuse %s from \"/Game/Rigs/Hero.Hero\"\n")
            TEXT("part body {\n    box size=(1, 1, 1)\n}\n"),
            InvalidKind);
        FPwModelParser::Parse(Source, Document, Diagnostics);

        TestNotNull(*FString::Printf(TEXT("'use %s' is rejected as a kind"), InvalidKind),
            PwModelSkinTest_FindCode(Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_VALUE));
        TestEqual(*FString::Printf(TEXT("'use %s' is not recorded"), InvalidKind),
            Document.Uses.Num(), 0);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserSkinOpTableTest,
    "PinWright.Model.Parser.SkinSmoothIsPublishedInItsOwnContext",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserSkinOpTableTest::RunTest(const FString& Parameters)
{
    const FPwModelOpSpec* Smooth = PwModelOpTable::Find(TEXT("smooth"), EPwModelOpContext::Skin);
    TestNotNull(TEXT("smooth is in the skin op context"), Smooth);
    if (Smooth)
    {
        TestEqual(TEXT("smooth publishes four parameters"), Smooth->Params.Num(), 4);

        const FPwModelParamSpec* MaxInfluences = Smooth->FindParam(TEXT("max_influences"));
        TestNotNull(TEXT("smooth publishes max_influences"), MaxInfluences);
        if (MaxInfluences)
        {
            TestTrue(TEXT("max_influences has the engine's lower bound"), MaxInfluences->bHasRange);
            TestEqual(TEXT("max_influences starts at one"), MaxInfluences->MinValue, 1.0);
            TestEqual(TEXT("max_influences ends at twelve"), MaxInfluences->MaxValue, 12.0);
        }

        const FPwModelParamSpec* Method = Smooth->FindParam(TEXT("method"));
        TestNotNull(TEXT("smooth publishes method"), Method);
        if (Method)
        {
            TestEqual(TEXT("method has two solve choices"), Method->AllowedValues.Num(), 2);
            TestTrue(TEXT("direct_distance is accepted"), Method->AllowedValues.Contains(TEXT("direct_distance")));
            TestTrue(TEXT("geodesic_voxel is accepted"), Method->AllowedValues.Contains(TEXT("geodesic_voxel")));
        }
    }
    return true;
}
