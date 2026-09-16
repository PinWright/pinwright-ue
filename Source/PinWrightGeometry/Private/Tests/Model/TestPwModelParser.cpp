// Copyright (c) 2026 Alexander Penkin. MIT License.

// One test per PWMODEL_* code the parser can emit, plus the structural cases that are
// only visible in the AST. Reverting any of them regresses a specific authoring failure:
//
//  - A .pwmodel is written by hand or by an LLM, and the diagnostic IS the authoring UX.
//    A code that stops being emitted turns a precise message into a generic parse failure,
//    and nothing else in the pipeline notices - the compiler simply never runs.
//  - The unclosed-brace case asserts the reported line is where the brace was OPENED. End
//    of file is where the parser noticed; reporting it there sends the author to the wrong
//    end of the file, which is the single most expensive wrong answer a parser can give.
//  - The grammar example is the one in docs/pwmodel-format.md. It parsing with zero
//    diagnostics is what keeps the normative spec and the op table from drifting apart:
//    rename a parameter in the table without updating the spec and this test fails.
#include "Misc/AutomationTest.h"

#include "Handlers/Geometry/GeometryOps_Modeling.h"
#include "Model/PwModelAst.h"
// Read-only here, and the point of the include: the parser's `complexity` and `auto method=`
// vocabularies must BE the collision layer's derived lists, not a second copy that agrees today.
#include "Model/PwModelCollision.h"
#include "Model/PwModelDiagnostic.h"
#include "Model/PwModelParser.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace
// helper with a common name would collide with a sibling test TU when Unity merges them.
bool PwModelParserTest_HasCode(const TArray<FPwDiagnostic>& Diagnostics, const TCHAR* Code)
{
    for (const FPwDiagnostic& Diagnostic : Diagnostics)
    {
        if (Diagnostic.Code == Code)
        {
            return true;
        }
    }
    return false;
}

const FPwDiagnostic* PwModelParserTest_FindCode(const TArray<FPwDiagnostic>& Diagnostics, const TCHAR* Code)
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

int32 PwModelParserTest_CountCode(const TArray<FPwDiagnostic>& Diagnostics, const TCHAR* Code)
{
    int32 Count = 0;
    for (const FPwDiagnostic& Diagnostic : Diagnostics)
    {
        if (Diagnostic.Code == Code)
        {
            ++Count;
        }
    }
    return Count;
}

// Every failure message carries the whole diagnostic list: a parser test that fails with
// "expected true, got false" costs a rerun under a debugger to learn anything.
void PwModelParserTest_ExpectCode(FAutomationTestBase& Test, const TArray<FPwDiagnostic>& Diagnostics,
                                  const TCHAR* Code)
{
    Test.TestTrue(*FString::Printf(TEXT("%s was reported. Diagnostics: [%s]"),
        Code, *JoinPwDiagnostics(Diagnostics)), PwModelParserTest_HasCode(Diagnostics, Code));
}

void PwModelParserTest_ExpectNoErrors(FAutomationTestBase& Test, bool bParsed,
                                      const TArray<FPwDiagnostic>& Diagnostics)
{
    Test.TestTrue(*FString::Printf(TEXT("document parsed without errors. Diagnostics: [%s]"),
        *JoinPwDiagnostics(Diagnostics)), bParsed);
}

// The example from docs/pwmodel-format.md, verbatim.
const TCHAR* PwModelParserTest_GrammarExample =
    TEXT("# comments run to end of line\n")
    TEXT("pwmodel 0\n")
    TEXT("\n")
    TEXT("materials {\n")
    TEXT("    Shell = \"/Game/Materials/M_Metal\"\n")
    TEXT("    Trim  = \"/Game/Materials/M_Rust\"\n")
    TEXT("}\n")
    TEXT("\n")
    TEXT("part body {\n")
    TEXT("    box size=(80, 50, 40) material=\"Shell\" color=(0.8, 0.8, 0.8, 1)\n")
    TEXT("    subtract {\n")
    TEXT("        sphere radius=18 at=(25, 0, 0)\n")
    TEXT("    }\n")
    TEXT("    bevel distance=2.5 segments=3\n")
    TEXT("    uv channel=0 mode=box scale=(1, 1)\n")
    TEXT("}\n")
    TEXT("\n")
    TEXT("part trim at=(0, 0, 22) rotate=(0, 0, 15) {\n")
    TEXT("    torus major_radius=42 minor_radius=3 material=\"Trim\"\n")
    TEXT("    uv channel=0 mode=cylindrical\n")
    TEXT("}\n")
    TEXT("\n")
    TEXT("lightmap channel=1 resolution=256\n")
    TEXT("\n")
    TEXT("collision {\n")
    TEXT("    complexity = simple_and_complex\n")
    TEXT("    box size=(78, 48, 38)\n")
    TEXT("    hull { cylinder radius=20 height=60 }\n")
    TEXT("}\n");

// A minimal well-formed document to hang single-error cases off.
const TCHAR* PwModelParserTest_MinimalPart =
    TEXT("pwmodel 0\n")
    TEXT("part body {\n")
    TEXT("    box size=(10, 10, 10)\n")
    TEXT("}\n");
}

// ============================================================================
// The normative grammar example
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserGrammarExampleTest,
    "PinWright.Model.Parser.GrammarExampleParsesClean",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserGrammarExampleTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bParsed = FPwModelParser::Parse(PwModelParserTest_GrammarExample, Document, Diagnostics);

    PwModelParserTest_ExpectNoErrors(*this, bParsed, Diagnostics);
    TestEqual(*FString::Printf(TEXT("no diagnostics at all. [%s]"), *JoinPwDiagnostics(Diagnostics)),
        Diagnostics.Num(), 0);

    TestEqual(TEXT("format keyword"), Document.Header.FormatKeyword, FString(TEXT("pwmodel")));
    TestEqual(TEXT("version"), Document.Header.Version, 0);
    TestEqual(TEXT("two parts"), Document.Parts.Num(), 2);
    TestEqual(TEXT("two material bindings"), Document.Materials.Num(), 2);
    TestTrue(TEXT("collision block captured"), Document.Collision.IsSet());
    TestTrue(TEXT("lightmap captured"), Document.Lightmap.IsSet());

    if (Document.Parts.Num() == 2)
    {
        TestEqual(TEXT("first part name"), Document.Parts[0].Name, FString(TEXT("body")));
        TestEqual(TEXT("body has four ops"), Document.Parts[0].Ops.Num(), 4);
        TestEqual(TEXT("second part name"), Document.Parts[1].Name, FString(TEXT("trim")));
        TestEqual(TEXT("trim header carries at and rotate"), Document.Parts[1].Transform.Num(), 2);
    }

    if (Document.Materials.Num() == 2)
    {
        TestEqual(TEXT("slot order follows declaration order"), Document.Materials[0].Slot, FString(TEXT("Shell")));
        TestEqual(TEXT("bound asset path"), Document.Materials[1].AssetPath, FString(TEXT("/Game/Materials/M_Rust")));
    }

    return true;
}

// ============================================================================
// Version header
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserMissingVersionTest,
    "PinWright.Model.Parser.MissingVersionHeader",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserMissingVersionTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bParsed = FPwModelParser::Parse(
        TEXTVIEW("part body {\n    box size=(1, 1, 1)\n}\n"), Document, Diagnostics);

    TestFalse(TEXT("a header-less document does not parse"), bParsed);
    PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_MISSING_VERSION);

    if (const FPwDiagnostic* Diagnostic =
            PwModelParserTest_FindCode(Diagnostics, PwSourceDiagnosticCodes::PWSRC_MISSING_VERSION))
    {
        TestTrue(TEXT("the message shows the expected header line"), Diagnostic->Message.Contains(TEXT("pwmodel 0")));
    }

    // Parsing continues past the missing header rather than aborting, so the author sees
    // every other problem in one run.
    TestEqual(TEXT("the rest of the document still parsed"), Document.Parts.Num(), 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserUnsupportedVersionTest,
    "PinWright.Model.Parser.UnsupportedVersion",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserUnsupportedVersionTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bParsed = FPwModelParser::Parse(
        TEXTVIEW("pwmodel 7\npart body {\n    box size=(1, 1, 1)\n}\n"), Document, Diagnostics);

    TestFalse(TEXT("an unknown version does not parse"), bParsed);
    PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_UNSUPPORTED_VERSION);

    if (const FPwDiagnostic* Diagnostic =
            PwModelParserTest_FindCode(Diagnostics, PwSourceDiagnosticCodes::PWSRC_UNSUPPORTED_VERSION))
    {
        // The version list itself, not the boilerplate around it: matching only "accepts"
        // passed with PwModelAcceptedVersions emptied or wrong, which is the one part of
        // the message that tells an author what to write instead.
        TestTrue(*FString::Printf(TEXT("the message names the accepted versions. [%s]"), *Diagnostic->Message),
            Diagnostic->Message.Contains(TEXT("This build accepts: 0")));
    }
    return true;
}

// ============================================================================
// Braces
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserUnclosedBraceTest,
    "PinWright.Model.Parser.UnclosedBraceReportsOpeningLine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserUnclosedBraceTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bParsed = FPwModelParser::Parse(
        TEXT("pwmodel 0\n")          // 1
        TEXT("part body {\n")        // 2 - the brace that is never closed
        TEXT("    box size=(1, 1, 1)\n")  // 3
        TEXT("    bevel distance=1\n")    // 4
        TEXT("    bevel distance=2\n"),   // 5
        Document, Diagnostics);

    TestFalse(TEXT("an unclosed brace does not parse"), bParsed);
    PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_UNCLOSED_BRACE);

    if (const FPwDiagnostic* Diagnostic =
            PwModelParserTest_FindCode(Diagnostics, PwSourceDiagnosticCodes::PWSRC_UNCLOSED_BRACE))
    {
        // Line 2, not line 5: the opening brace is where the author erred.
        TestEqual(TEXT("reported at the line the brace was opened on"), Diagnostic->Line, 2);

        // Column 11 is the '{'. Line alone does not discriminate: `part`, `body` and `{` all
        // sit on line 2, so anchoring on the part keyword's token instead of the brace's -
        // which is the near miss, since ForEachBlockEntry is handed a token by its caller -
        // reads as line 2 either way. The column is the only coordinate that separates them.
        TestEqual(TEXT("and at the column the brace was opened on"), Diagnostic->Column, 11);
    }
    return true;
}

// ============================================================================
// Ops and parameters
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserUnknownOpTest,
    "PinWright.Model.Parser.UnknownOpSuggests",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserUnknownOpTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    FPwModelParser::Parse(
        TEXTVIEW("pwmodel 0\npart body {\n    box size=(1, 1, 1)\n    bevelation distance=1\n}\n"),
        Document, Diagnostics);

    PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_UNKNOWN_OP);

    if (const FPwDiagnostic* Diagnostic =
            PwModelParserTest_FindCode(Diagnostics, PwSourceDiagnosticCodes::PWSRC_UNKNOWN_OP))
    {
        TestEqual(TEXT("anchored to the offending line"), Diagnostic->Line, 4);
        TestEqual(TEXT("carries the owning part"), Diagnostic->ScopeName, FString(TEXT("body")));
        TestTrue(TEXT("offers a did-you-mean"), Diagnostic->Suggestions.Contains(TEXT("bevel")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserCollisionOpOutOfContextTest,
    "PinWright.Model.Parser.CollisionOpRejectedInsidePart",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserCollisionOpOutOfContextTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    FPwModelParser::Parse(
        TEXTVIEW("pwmodel 0\npart body {\n    box size=(1, 1, 1)\n    convex points=[(0,0,0), (1,0,0), (0,1,0), (0,0,1)]\n}\n"),
        Document, Diagnostics);

    PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_UNKNOWN_OP);

    if (const FPwDiagnostic* Diagnostic =
            PwModelParserTest_FindCode(Diagnostics, PwSourceDiagnosticCodes::PWSRC_UNKNOWN_OP))
    {
        // The op exists - saying so is the difference between "you spelled it wrong" and
        // "it belongs in the collision block". Both halves are pinned, not just the word
        // `collision`: the two context names are formatted from the same
        // PwModelOpContextToString call with different arguments, so swapping them produces
        // "not valid in a collision block; it is a part op" - a message that still contains
        // `collision` while sending the author to move the statement in exactly the wrong
        // direction.
        TestTrue(*FString::Printf(TEXT("the message names the context it does belong to. [%s]"),
            *Diagnostic->Message),
            Diagnostic->Message.Contains(TEXT("is not valid in a part block; it is a collision op")));

        // And it must not also read as a typo: `convex` is spelled correctly.
        TestEqual(*FString::Printf(TEXT("no did-you-mean offered. Suggestions: [%s]"),
            *FString::Join(Diagnostic->Suggestions, TEXT(", "))), Diagnostic->Suggestions.Num(), 0);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserUnknownParamTest,
    "PinWright.Model.Parser.UnknownParamListsValidNames",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserUnknownParamTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    FPwModelParser::Parse(
        TEXTVIEW("pwmodel 0\npart body {\n    box width=100\n}\n"), Document, Diagnostics);

    PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_UNKNOWN_PARAM);

    if (const FPwDiagnostic* Diagnostic =
            PwModelParserTest_FindCode(Diagnostics, PwSourceDiagnosticCodes::PWSRC_UNKNOWN_PARAM))
    {
        // width/height/depth is the RPC verb's vocabulary; the format's is size=(x,y,z).
        TestTrue(TEXT("the message lists the valid parameter names"), Diagnostic->Message.Contains(TEXT("size")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserPartHeaderParamTest,
    "PinWright.Model.Parser.PartHeaderRejectsGeometryParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserPartHeaderParamTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    FPwModelParser::Parse(
        TEXTVIEW("pwmodel 0\npart body radius=5 {\n    box size=(1, 1, 1)\n}\n"), Document, Diagnostics);

    PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_UNKNOWN_PARAM);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserMissingParamTest,
    "PinWright.Model.Parser.MissingRequiredParam",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserMissingParamTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    FPwModelParser::Parse(
        TEXTVIEW("pwmodel 0\npart body {\n    box size=(1, 1, 1)\n    uv channel=0\n}\n"),
        Document, Diagnostics);

    PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_MISSING_PARAM);

    if (const FPwDiagnostic* Diagnostic =
            PwModelParserTest_FindCode(Diagnostics, PwSourceDiagnosticCodes::PWSRC_MISSING_PARAM))
    {
        TestTrue(TEXT("names the missing parameter"), Diagnostic->Message.Contains(TEXT("mode")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserBadTupleArityTest,
    "PinWright.Model.Parser.BadTupleArity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserBadTupleArityTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    FPwModelParser::Parse(
        TEXTVIEW("pwmodel 0\npart body {\n    box size=(10, 10)\n}\n"), Document, Diagnostics);

    PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_TUPLE_ARITY);

    // And the list form: an entry of the wrong width is the same class of mistake.
    FPwModelDocument ListDocument;
    TArray<FPwDiagnostic> ListDiagnostics;
    FPwModelParser::Parse(
        TEXTVIEW("pwmodel 0\npart body {\n    revolve profile=[(0, 0), (10, 0, 5)]\n}\n"),
        ListDocument, ListDiagnostics);

    PwModelParserTest_ExpectCode(*this, ListDiagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_TUPLE_ARITY);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserBadValueTest,
    "PinWright.Model.Parser.BadValue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserBadValueTest::RunTest(const FString& Parameters)
{
    // An unknown enum spelling.
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXTVIEW("pwmodel 0\npart body {\n    box size=(1, 1, 1)\n    stretch axis=w factor=2\n}\n"),
            Document, Diagnostics);
        PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_VALUE);
    }

    // A UV channel outside 0-7. SetNumUVSets rejects above 8 and the bake agrees.
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXTVIEW("pwmodel 0\npart body {\n    box size=(1, 1, 1)\n    uv channel=9 mode=box\n}\n"),
            Document, Diagnostics);
        PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_VALUE);
    }

    // The wrong kind of value entirely. The message must name BOTH halves: every caller of
    // BadValue once passed a literal "something else", so `size="80,50,40"` reported "expects
    // vector3, but found something else" and left the author to guess that the quotes are the
    // mistake.
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXTVIEW("pwmodel 0\npart body {\n    sphere radius=\"big\"\n}\n"), Document, Diagnostics);
        PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_VALUE);

        if (const FPwDiagnostic* Diagnostic =
                PwModelParserTest_FindCode(Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_VALUE))
        {
            TestTrue(*FString::Printf(TEXT("names the expected type. [%s]"), *Diagnostic->Message),
                Diagnostic->Message.Contains(TEXT("expects number")));
            TestTrue(*FString::Printf(TEXT("names what was actually written. [%s]"), *Diagnostic->Message),
                Diagnostic->Message.Contains(TEXT("found a quoted string")));
        }
    }

    // The same, one type up: a vector parameter given a string is the mistake the report was
    // useless for, so it is pinned by the exact document from the finding.
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXTVIEW("pwmodel 0\npart body {\n    box size=\"80,50,40\"\n}\n"), Document, Diagnostics);

        if (const FPwDiagnostic* Diagnostic =
                PwModelParserTest_FindCode(Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_VALUE))
        {
            TestTrue(*FString::Printf(TEXT("vector3 vs string is spelled out. [%s]"), *Diagnostic->Message),
                Diagnostic->Message.Contains(TEXT("expects vector3, but found a quoted string")));
        }
        else
        {
            AddError(FString::Printf(TEXT("no PWSRC_BAD_VALUE for size=\"80,50,40\". [%s]"),
                *JoinPwDiagnostics(Diagnostics)));
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserSphericalUvTest,
    "PinWright.Model.Parser.SphericalUvRejectedByName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserSphericalUvTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    FPwModelParser::Parse(
        TEXTVIEW("pwmodel 0\npart body {\n    sphere radius=10\n    uv channel=0 mode=spherical\n}\n"),
        Document, Diagnostics);

    PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_VALUE);

    if (const FPwDiagnostic* Diagnostic =
            PwModelParserTest_FindCode(Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_VALUE))
    {
        // The spelling is right and the engine simply lacks the function; a did-you-mean
        // list would send the author hunting for a typo that is not there.
        TestTrue(TEXT("rejected by name, not as a typo"), Diagnostic->Message.Contains(TEXT("spherical")));
        TestEqual(TEXT("no did-you-mean offered"), Diagnostic->Suggestions.Num(), 0);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserBadBlockTest,
    "PinWright.Model.Parser.BadBlock",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserBadBlockTest::RunTest(const FString& Parameters)
{
    // A block on an op that takes none.
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXTVIEW("pwmodel 0\npart body {\n    box size=(1, 1, 1) {\n        sphere radius=1\n    }\n}\n"),
            Document, Diagnostics);
        PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_BLOCK);
    }

    // A boolean with no block has no tool mesh to operate against.
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXTVIEW("pwmodel 0\npart body {\n    box size=(1, 1, 1)\n    subtract\n}\n"),
            Document, Diagnostics);
        PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_BLOCK);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserOneStatementPerLineTest,
    "PinWright.Model.Parser.OneStatementPerLine",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserOneStatementPerLineTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    FPwModelParser::Parse(
        TEXTVIEW("pwmodel 0\npart body {\n    box size=(1, 1, 1) sphere radius=2\n}\n"),
        Document, Diagnostics);

    PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_UNEXPECTED_TOKEN);
    return true;
}

// ============================================================================
// Parts
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserNoPartsTest,
    "PinWright.Model.Parser.NoParts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserNoPartsTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bParsed = FPwModelParser::Parse(
        TEXTVIEW("pwmodel 0\nlightmap channel=1\n"), Document, Diagnostics);

    TestFalse(TEXT("a partless document does not parse"), bParsed);
    PwModelParserTest_ExpectCode(*this, Diagnostics, PwModelDiagnosticCodes::PWMODEL_NO_PARTS);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserDuplicatePartTest,
    "PinWright.Model.Parser.DuplicatePart",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserDuplicatePartTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    FPwModelParser::Parse(
        TEXTVIEW("pwmodel 0\npart body {\n    box size=(1, 1, 1)\n}\npart body {\n    sphere radius=1\n}\n"),
        Document, Diagnostics);

    PwModelParserTest_ExpectCode(*this, Diagnostics, PwModelDiagnosticCodes::PWMODEL_DUPLICATE_PART);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserEmptyPartTest,
    "PinWright.Model.Parser.EmptyPartWarns",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserEmptyPartTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bParsed = FPwModelParser::Parse(
        TEXTVIEW("pwmodel 0\npart body {\n}\n"), Document, Diagnostics);

    // A warning, so the document still parses: warnings do not fail the parse.
    PwModelParserTest_ExpectNoErrors(*this, bParsed, Diagnostics);
    PwModelParserTest_ExpectCode(*this, Diagnostics, PwModelDiagnosticCodes::PWMODEL_EMPTY_PART);

    if (const FPwDiagnostic* Diagnostic =
            PwModelParserTest_FindCode(Diagnostics, PwModelDiagnosticCodes::PWMODEL_EMPTY_PART))
    {
        TestEqual(TEXT("severity is Warning"), static_cast<int32>(Diagnostic->Severity),
            static_cast<int32>(EPwSeverity::Warning));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserPartNeedsPrimitiveTest,
    "PinWright.Model.Parser.PartNeedsPrimitive",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserPartNeedsPrimitiveTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    FPwModelParser::Parse(
        TEXTVIEW("pwmodel 0\npart body {\n    bevel distance=1\n    box size=(1, 1, 1)\n}\n"),
        Document, Diagnostics);

    PwModelParserTest_ExpectCode(*this, Diagnostics, PwModelDiagnosticCodes::PWMODEL_PART_NEEDS_PRIMITIVE);

    // The same rule inside a boolean block: its tool mesh has to start from something too.
    FPwModelDocument BlockDocument;
    TArray<FPwDiagnostic> BlockDiagnostics;
    FPwModelParser::Parse(
        TEXTVIEW("pwmodel 0\npart body {\n    box size=(1, 1, 1)\n    subtract {\n        bevel distance=1\n    }\n}\n"),
        BlockDocument, BlockDiagnostics);

    PwModelParserTest_ExpectCode(*this, BlockDiagnostics, PwModelDiagnosticCodes::PWMODEL_PART_NEEDS_PRIMITIVE);
    return true;
}

// ============================================================================
// Booleans
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserNestedBooleanTest,
    "PinWright.Model.Parser.NestedBooleanProducesChildOps",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserNestedBooleanTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bParsed = FPwModelParser::Parse(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(10, 10, 10)\n")
        TEXT("    subtract {\n")
        TEXT("        sphere radius=4 at=(1, 0, 0)\n")
        TEXT("        union {\n")
        TEXT("            cylinder radius=1 height=20\n")
        TEXT("        }\n")
        TEXT("    }\n")
        TEXT("}\n"),
        Document, Diagnostics);

    PwModelParserTest_ExpectNoErrors(*this, bParsed, Diagnostics);
    TestEqual(TEXT("one part"), Document.Parts.Num(), 1);

    if (Document.Parts.Num() != 1 || Document.Parts[0].Ops.Num() != 2)
    {
        AddError(FString::Printf(TEXT("expected one part with two top-level ops. [%s]"),
            *JoinPwDiagnostics(Diagnostics)));
        return false;
    }

    const FPwOp& Subtract = Document.Parts[0].Ops[1];
    TestEqual(TEXT("second op is the boolean"), Subtract.OpName, FString(TEXT("subtract")));
    TestEqual(TEXT("boolean carries two children"), Subtract.Children.Num(), 2);

    if (Subtract.Children.Num() == 2)
    {
        TestEqual(TEXT("first child"), Subtract.Children[0].OpName, FString(TEXT("sphere")));
        TestEqual(TEXT("nested boolean"), Subtract.Children[1].OpName, FString(TEXT("union")));
        TestEqual(TEXT("nested boolean has its own child"), Subtract.Children[1].Children.Num(), 1);
    }
    return true;
}

// A boolean takes `material=` - it names the faces the operation CREATES, which is the only
// spelling a cut's walls have - and still refuses `color=`. The asymmetry is the decision: a
// boolean creates faces but no VERTICES, so a scalar colour could only overwrite one a generator
// already wrote.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserColorOnBooleanTest,
    "PinWright.Model.Parser.ColorOnBooleanRejectedButMaterialAccepted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserColorOnBooleanTest::RunTest(const FString& Parameters)
{
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXT("pwmodel 0\n")
            TEXT("part body {\n")
            TEXT("    box size=(10, 10, 10)\n")
            TEXT("    subtract color=(1, 0, 0, 1) {\n")
            TEXT("        sphere radius=4\n")
            TEXT("    }\n")
            TEXT("}\n"),
            Document, Diagnostics);

        PwModelParserTest_ExpectCode(*this, Diagnostics, PwModelDiagnosticCodes::PWMODEL_MATERIAL_ON_BOOLEAN);

        if (const FPwDiagnostic* Diagnostic =
                PwModelParserTest_FindCode(Diagnostics, PwModelDiagnosticCodes::PWMODEL_MATERIAL_ON_BOOLEAN))
        {
            // The remedy is the point of the diagnostic, and it is no longer "model the interior
            // as its own part" - that was the answer while the slot tag was refused too.
            TestTrue(TEXT("the message names set_vertex_color as what was meant"),
                Diagnostic->Message.Contains(TEXT("set_vertex_color")));
        }
    }

    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXT("pwmodel 0\n")
            TEXT("part body {\n")
            TEXT("    box size=(10, 10, 10)\n")
            TEXT("    subtract material=\"Cut\" {\n")
            TEXT("        sphere radius=4\n")
            TEXT("    }\n")
            TEXT("}\n"),
            Document, Diagnostics);

        TestNull(TEXT("material= on a boolean is no longer refused"),
            PwModelParserTest_FindCode(Diagnostics, PwModelDiagnosticCodes::PWMODEL_MATERIAL_ON_BOOLEAN));
    }

    return true;
}

// ============================================================================
// Model-level blocks
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserDuplicateBlocksTest,
    "PinWright.Model.Parser.DuplicateModelLevelBlocks",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserDuplicateBlocksTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    FPwModelParser::Parse(
        TEXT("pwmodel 0\n")
        TEXT("materials {\n    Shell = \"/Game/M_A\"\n}\n")
        TEXT("materials {\n    Trim = \"/Game/M_B\"\n}\n")
        TEXT("part body {\n    box size=(1, 1, 1) material=\"Shell\"\n}\n")
        TEXT("collision {\n    box size=(1, 1, 1)\n}\n")
        TEXT("collision {\n    sphere radius=1\n}\n")
        TEXT("lightmap channel=1\n")
        TEXT("lightmap channel=2\n"),
        Document, Diagnostics);

    PwModelParserTest_ExpectCode(*this, Diagnostics, PwModelDiagnosticCodes::PWMODEL_DUPLICATE_MATERIALS);
    PwModelParserTest_ExpectCode(*this, Diagnostics, PwModelDiagnosticCodes::PWMODEL_DUPLICATE_COLLISION);
    PwModelParserTest_ExpectCode(*this, Diagnostics, PwModelDiagnosticCodes::PWMODEL_DUPLICATE_LIGHTMAP);

    // The first of each wins; the duplicate is parsed for brace balance and discarded.
    TestEqual(TEXT("only the first materials block is stored"), Document.Materials.Num(), 1);
    if (Document.Materials.Num() == 1)
    {
        TestEqual(TEXT("and it is the first one"), Document.Materials[0].Slot, FString(TEXT("Shell")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserDuplicateSlotTest,
    "PinWright.Model.Parser.DuplicateMaterialSlot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserDuplicateSlotTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    FPwModelParser::Parse(
        TEXT("pwmodel 0\n")
        TEXT("materials {\n    Shell = \"/Game/M_A\"\n    Shell = \"/Game/M_B\"\n}\n")
        TEXT("part body {\n    box size=(1, 1, 1) material=\"Shell\"\n}\n"),
        Document, Diagnostics);

    PwModelParserTest_ExpectCode(*this, Diagnostics, PwModelDiagnosticCodes::PWMODEL_DUPLICATE_SLOT);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserMaterialSlotWarningsTest,
    "PinWright.Model.Parser.MaterialSlotWarnings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserMaterialSlotWarningsTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bParsed = FPwModelParser::Parse(
        TEXT("pwmodel 0\n")
        TEXT("materials {\n    Unused = \"/Game/M_A\"\n}\n")
        TEXT("part body {\n    box size=(1, 1, 1) material=\"Shell\"\n}\n"),
        Document, Diagnostics);

    // Both are warnings: an unbound slot still produces an asset, it just has an empty slot.
    PwModelParserTest_ExpectNoErrors(*this, bParsed, Diagnostics);
    PwModelParserTest_ExpectCode(*this, Diagnostics, PwModelDiagnosticCodes::PWMODEL_UNBOUND_MATERIAL);
    PwModelParserTest_ExpectCode(*this, Diagnostics, PwModelDiagnosticCodes::PWMODEL_UNUSED_MATERIAL);

    // Both warnings are raised from ValidateDocument, after the parser has left every part.
    // The unbound one is anchored on a `material=` inside a part, so it must still carry that
    // part's name - with several parts, the name is what says which one to open.
    if (const FPwDiagnostic* Unbound =
            PwModelParserTest_FindCode(Diagnostics, PwModelDiagnosticCodes::PWMODEL_UNBOUND_MATERIAL))
    {
        TestEqual(TEXT("the unbound-slot warning names the part it was tagged in"),
            Unbound->ScopeName, FString(TEXT("body")));
    }

    // The unused one is anchored on a binding line inside `materials { … }`, which is model
    // level: no part name, and that is correct rather than the same omission.
    if (const FPwDiagnostic* Unused =
            PwModelParserTest_FindCode(Diagnostics, PwModelDiagnosticCodes::PWMODEL_UNUSED_MATERIAL))
    {
        TestTrue(TEXT("the unused-binding warning is model level"), Unused->ScopeName.IsEmpty());
    }
    return true;
}

// The implicit `Default` slot is REFERENCED, so binding it is not an unused binding.
//
// PWMODEL_UNUSED_MATERIAL told authors "Slot 'Default' is bound but no geometry tags it; the
// binding is dropped" for exactly this document — and it was false in both halves. The compiler
// allocates a slot literally named Default for every untagged part-level generator
// (PwModelCompiler.cpp RunGenerator), so SlotNames contains it, CreateAsset's
// `SlotNames.Contains(Binding.Slot)` filter passes, and the binding reaches the asset:
// static_mesh.describe on the written .uasset reported the slot present and bound to the
// material the document named. A diagnostic that contradicts the asset it just described is
// worse than no diagnostic, which is why this is pinned by name.
//
// Counterfactual: drop the bUsesImplicitDefaultSlot branch from ValidateMaterialSlots and this
// fails immediately.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserDefaultSlotBindingIsUsedTest,
    "PinWright.Model.Parser.BindingTheImplicitDefaultSlotIsNotUnused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserDefaultSlotBindingIsUsedTest::RunTest(const FString& Parameters)
{
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        const bool bParsed = FPwModelParser::Parse(
            TEXT("pwmodel 0\n")
            TEXT("materials {\n    Default = \"/Engine/BasicShapes/BasicShapeMaterial\"\n}\n")
            TEXT("part body {\n    box size=(1, 1, 1)\n}\n"),
            Document, Diagnostics);

        PwModelParserTest_ExpectNoErrors(*this, bParsed, Diagnostics);
        TestNull(TEXT("binding the implicit Default slot is not reported as unused"),
            PwModelParserTest_FindCode(Diagnostics, PwModelDiagnosticCodes::PWMODEL_UNUSED_MATERIAL));
        // The other half of the pair must stay quiet too: the implicit slot is never TAGGED by
        // a material= the author wrote, so feeding it into the tagged set would fire
        // PWMODEL_UNBOUND_MATERIAL on every ordinary document that has no materials block.
        TestNull(TEXT("and the implicit slot is not reported as unbound either"),
            PwModelParserTest_FindCode(Diagnostics, PwModelDiagnosticCodes::PWMODEL_UNBOUND_MATERIAL));
    }

    {
        // The suppression is scoped to the one name the compiler actually allocates. Any other
        // unreferenced binding is still dropped and still reported.
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXT("pwmodel 0\n")
            TEXT("materials {\n    Paper = \"/Engine/BasicShapes/BasicShapeMaterial\"\n}\n")
            TEXT("part body {\n    box size=(1, 1, 1)\n}\n"),
            Document, Diagnostics);

        PwModelParserTest_ExpectCode(*this, Diagnostics, PwModelDiagnosticCodes::PWMODEL_UNUSED_MATERIAL);
    }

    {
        // Every part-level generator carries a material=, so nothing lands in Default and a
        // Default binding really is dropped. The suppression must not fire on geometry it
        // cannot reach.
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXT("pwmodel 0\n")
            TEXT("materials {\n    Default = \"/Engine/BasicShapes/BasicShapeMaterial\"\n    Shell = \"/Game/M_A\"\n}\n")
            TEXT("part body {\n    box size=(1, 1, 1) material=\"Shell\"\n}\n"),
            Document, Diagnostics);

        PwModelParserTest_ExpectCode(*this, Diagnostics, PwModelDiagnosticCodes::PWMODEL_UNUSED_MATERIAL);
    }

    return true;
}

// A `material=` inside a boolean block IS a reference, and this test used to assert the opposite.
//
// The old premise was "the tool mesh is discarded, so tagging a generator inside a boolean block
// never reaches the asset". That was measurably wrong about where the tool's surface ends up: the
// walls a `subtract` opens ARE the tool's surface, and a `union` keeps the tool's outside outright
// - the ticket that reverted this measured 64 bore-wall triangles carrying the tool's material id.
// The tag now resolves through the model-wide slot table like any other, so a binding for it is
// USED, and reporting it dropped would contradict the asset the same compile writes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserMaterialOnDiscardedToolTest,
    "PinWright.Model.Parser.MaterialInsideBooleanBlockBindsItsSlot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserMaterialOnDiscardedToolTest::RunTest(const FString& Parameters)
{
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXT("pwmodel 0\n")
            TEXT("materials {\n    Cut = \"/Game/M_A\"\n}\n")
            TEXT("part body {\n")
            TEXT("    box size=(10, 10, 10)\n")
            TEXT("    subtract {\n        sphere radius=4 material=\"Cut\"\n    }\n")
            TEXT("}\n"),
            Document, Diagnostics);

        TestNull(TEXT("a binding tagged only inside a boolean block is not reported dropped"),
            PwModelParserTest_FindCode(Diagnostics, PwModelDiagnosticCodes::PWMODEL_UNUSED_MATERIAL));
    }

    // The other direction, which is what makes the first half an assertion about REFERENCES rather
    // than about the warning being switched off: an unbound slot tagged inside a block is still
    // reported, on its own line, exactly as at part level.
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXT("pwmodel 0\n")
            TEXT("materials {\n    Cut = \"/Game/M_A\"\n}\n")
            TEXT("part body {\n")
            TEXT("    box size=(10, 10, 10) material=\"Cut\"\n")
            TEXT("    subtract {\n        sphere radius=4 material=\"Bore\"\n    }\n")
            TEXT("}\n"),
            Document, Diagnostics);

        PwModelParserTest_ExpectCode(*this, Diagnostics, PwModelDiagnosticCodes::PWMODEL_UNBOUND_MATERIAL);
    }

    return true;
}

// `append_buffers` could not be given a material by NAME.
//
// The op table registered it through MakeBareGenerator, which sets bGenerator and nothing else,
// so `material=` hit the parser's unknown-parameter gate and the document did not parse at all.
// The compiler had supported it the whole time - RunGenerator reads a generic `material` off any
// non-nested generator - so the ONLY thing between a raw-buffer part and a named slot was the
// spec. What the author saw instead was PWMODEL_UNUSED_MATERIAL telling them their binding was
// dropped, with `material_id=` (an index into a table they cannot see) as the only alternative.
//
// Counterfactual: remove the AcceptMaterialSlot call from the append_buffers spec and the first
// block fails on PWSRC_UNKNOWN_PARAM before any of the slot assertions are reached.
// `const TCHAR* const`, not `const TCHAR*`: the second gives the pointer external linkage, and
// this module builds with bUseUnity = true.
const TCHAR* const PwModelParserTest_AppendBuffersBody =
    TEXT("    append_buffers vertices=[(0, 0, 0), (100, 0, 0), (50, 100, 0)] triangles=[(0, 1, 2)]");

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserAppendBuffersTakesMaterialSlotTest,
    "PinWright.Model.Parser.AppendBuffersTakesAMaterialSlotByName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserAppendBuffersTakesMaterialSlotTest::RunTest(const FString& Parameters)
{
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        const bool bParsed = FPwModelParser::Parse(
            FString(TEXT("pwmodel 0\n"))
            + TEXT("materials {\n    Quartz = \"/Game/Materials/M_Quartz\"\n}\n")
            + TEXT("part shard {\n")
            + PwModelParserTest_AppendBuffersBody + TEXT(" material=\"Quartz\"\n")
            + TEXT("}\n"),
            Document, Diagnostics);

        PwModelParserTest_ExpectNoErrors(*this, bParsed, Diagnostics);
        TestNull(*FString::Printf(TEXT("material= is a known parameter on append_buffers. Diagnostics: [%s]"),
            *JoinPwDiagnostics(Diagnostics)),
            PwModelParserTest_FindCode(Diagnostics, PwSourceDiagnosticCodes::PWSRC_UNKNOWN_PARAM));

        // The reported symptom. The binding is NOT dropped - the op tags the slot, so the
        // compiler allocates it and CreateAsset copies the binding onto it.
        TestNull(*FString::Printf(TEXT("the binding is not reported as unused. Diagnostics: [%s]"),
            *JoinPwDiagnostics(Diagnostics)),
            PwModelParserTest_FindCode(Diagnostics, PwModelDiagnosticCodes::PWMODEL_UNUSED_MATERIAL));
        TestNull(*FString::Printf(TEXT("and the slot is bound, so it is not reported as unbound. Diagnostics: [%s]"),
            *JoinPwDiagnostics(Diagnostics)),
            PwModelParserTest_FindCode(Diagnostics, PwModelDiagnosticCodes::PWMODEL_UNBOUND_MATERIAL));

        // Parsing without complaint is not enough: the value has to survive into the AST, since
        // that map is the only thing the compiler reads. A parser that accepted the parameter and
        // discarded it would pass every assertion above.
        if (Document.Parts.Num() == 1 && Document.Parts[0].Ops.Num() == 1)
        {
            const FPwOp& Op = Document.Parts[0].Ops[0];
            TestEqual(TEXT("the op is append_buffers"), Op.OpName, FString(TEXT("append_buffers")));
            if (const FPwValue* Material = Op.Params.Find(TEXT("material")))
            {
                TestEqual(TEXT("the parsed op carries the slot name"), Material->Text, FString(TEXT("Quartz")));
            }
            else
            {
                AddError(TEXT("the parsed append_buffers op carries no 'material' value"));
            }
            TestFalse(TEXT("and no material_id, which was not written"), Op.Params.Contains(TEXT("material_id")));
        }
        else
        {
            AddError(FString::Printf(TEXT("expected one part with one op. Diagnostics: [%s]"),
                *JoinPwDiagnostics(Diagnostics)));
        }
    }

    {
        // The other half of the pair still works: an append_buffers slot that `materials` does
        // not bind is reported, and names the part. Without this the first block could pass by
        // the parser treating the value as inert text it never feeds to the slot bookkeeping.
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            FString(TEXT("pwmodel 0\n"))
            + TEXT("part shard {\n")
            + PwModelParserTest_AppendBuffersBody + TEXT(" material=\"Quartz\"\n")
            + TEXT("}\n"),
            Document, Diagnostics);

        PwModelParserTest_ExpectCode(*this, Diagnostics, PwModelDiagnosticCodes::PWMODEL_UNBOUND_MATERIAL);
        if (const FPwDiagnostic* Unbound =
                PwModelParserTest_FindCode(Diagnostics, PwModelDiagnosticCodes::PWMODEL_UNBOUND_MATERIAL))
        {
            TestEqual(TEXT("the unbound-slot warning names the part"), Unbound->ScopeName, FString(TEXT("shard")));
        }
    }

    return true;
}

// The slot tag arrived WITHOUT `color=`, and `append_triangle` took the tag WITHOUT the rest.
//
// `color=` is a scalar the compiler applies with SetVertexColor(bSetAll), so on an op that
// already carries a per-vertex `colors=` buffer it would silently overwrite every value the
// author supplied. Neither bare generator takes it.
//
// `append_triangle` - the op table's other MakeBareGenerator user - DOES now take `material=`,
// deliberately, through its own AcceptMaterialSlot call at registration. Without it a patch op
// that produces geometry could not name a slot at all, and the compiler's untagged path
// allocated a synthetic `Default` for it: twelve patch triangles shipped rendering in
// WorldGridMaterial while the rest of the part rendered in the bound material. That half is
// pinned by PinWright.Model.Compiler (TestPwModelCompiler.cpp).
//
// What is pinned HERE is the boundary the shared helper stops at. AcceptMaterialSlot adds
// exactly one parameter and sets one flag, so `append_triangle` must still reject `color=`
// (never part of that helper) and `material_id=` (an `append_buffers` parameter, not a shared
// one). Either arriving would mean a spec grew a parameter its op never asked for.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserAppendBuffersTakesNoScalarColorTest,
    "PinWright.Model.Parser.AppendBuffersTakesTheSlotTagButNotTheScalarColor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserAppendBuffersTakesNoScalarColorTest::RunTest(const FString& Parameters)
{
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            FString(TEXT("pwmodel 0\n"))
            + TEXT("part shard {\n")
            + PwModelParserTest_AppendBuffersBody + TEXT(" color=(1, 0, 0, 1)\n")
            + TEXT("}\n"),
            Document, Diagnostics);

        PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_UNKNOWN_PARAM);
    }

    {
        // The scalar overlay is withheld from `append_triangle` as well. Nothing has needed it
        // there; what the assertion buys is that AcceptMaterialSlot did not drag it in.
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXT("pwmodel 0\n")
            TEXT("part shard {\n")
            TEXT("    append_triangle v0=(0, 0, 0) v1=(100, 0, 0) v2=(50, 100, 0) color=(1, 0, 0, 1)\n")
            TEXT("}\n"),
            Document, Diagnostics);

        PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_UNKNOWN_PARAM);
    }

    {
        // `material_id=` belongs to `append_buffers` alone - it writes a raw index into a slot
        // table the format never showed the author, which is only defensible for the op that
        // hands over machine-computed IDs. It is also the half of PWMODEL_MATERIAL_ID_CONFLICT
        // that must not spread: an op taking both spellings has a tie-break to explain.
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXT("pwmodel 0\n")
            TEXT("part shard {\n")
            TEXT("    append_triangle v0=(0, 0, 0) v1=(100, 0, 0) v2=(50, 100, 0) material_id=3\n")
            TEXT("}\n"),
            Document, Diagnostics);

        PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_UNKNOWN_PARAM);
    }

    return true;
}

// Both spellings on one op is an error, not a precedence rule.
//
// The compiler's tie-break honours material_id and drops the slot with nothing said, so
// accepting the pair would have replaced "your parameter is unknown" with "your parameter did
// nothing" - a strictly worse failure, because it looks like it worked.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserMaterialIdConflictTest,
    "PinWright.Model.Parser.AppendBuffersRejectsBothMaterialSpellings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserMaterialIdConflictTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bParsed = FPwModelParser::Parse(
        FString(TEXT("pwmodel 0\n"))
        + TEXT("materials {\n    Quartz = \"/Game/Materials/M_Quartz\"\n}\n")
        + TEXT("part shard {\n")
        + PwModelParserTest_AppendBuffersBody + TEXT(" material=\"Quartz\" material_id=3\n")
        + TEXT("}\n"),
        Document, Diagnostics);

    PwModelParserTest_ExpectCode(*this, Diagnostics, PwModelDiagnosticCodes::PWMODEL_MATERIAL_ID_CONFLICT);
    TestFalse(*FString::Printf(TEXT("the conflict fails the parse rather than warning. Diagnostics: [%s]"),
        *JoinPwDiagnostics(Diagnostics)), bParsed);

    if (const FPwDiagnostic* Conflict =
            PwModelParserTest_FindCode(Diagnostics, PwModelDiagnosticCodes::PWMODEL_MATERIAL_ID_CONFLICT))
    {
        TestTrue(TEXT("the conflict is an error"), Conflict->Severity == EPwSeverity::Error);

        // The message has to name BOTH values, because "pick one" is unanswerable without them.
        TestTrue(*FString::Printf(TEXT("the message names the slot. Message: %s"), *Conflict->Message),
            Conflict->Message.Contains(TEXT("Quartz")));
        TestTrue(*FString::Printf(TEXT("the message names the raw id. Message: %s"), *Conflict->Message),
            Conflict->Message.Contains(TEXT("material_id=3")));
        TestTrue(*FString::Printf(TEXT("the message names the op. Message: %s"), *Conflict->Message),
            Conflict->Message.Contains(TEXT("append_buffers")));

        // Anchored on the material_id half - the one to delete - rather than on the op or on the
        // slot tag. Compared against the slot tag's own recorded column, so the assertion does
        // not encode a hand-counted offset into the source text.
        if (Document.Parts.Num() == 1 && Document.Parts[0].Ops.Num() == 1)
        {
            if (const FPwValue* Material = Document.Parts[0].Ops[0].Params.Find(TEXT("material")))
            {
                TestEqual(TEXT("the conflict is on the statement's line"), Conflict->Line, Material->Line);
                TestTrue(*FString::Printf(TEXT("and anchored past the material= value (col %d vs %d)"),
                    Conflict->Column, Material->Column), Conflict->Column > Material->Column);
            }
        }
    }

    // A slot that is named only by a rejected statement must not also be reported as an unused
    // binding: two diagnostics for one mistake sends the author to fix the materials block.
    TestNull(*FString::Printf(TEXT("the conflict is the only report. Diagnostics: [%s]"),
        *JoinPwDiagnostics(Diagnostics)),
        PwModelParserTest_FindCode(Diagnostics, PwModelDiagnosticCodes::PWMODEL_UNUSED_MATERIAL));

    return true;
}

// material_id= alone is unchanged. It exists so a generator that already computed IDs can hand
// them over raw, and closing the conflict above must not have closed that door.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserRawMaterialIdStillParsesTest,
    "PinWright.Model.Parser.AppendBuffersStillTakesARawMaterialIdAlone",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserRawMaterialIdStillParsesTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bParsed = FPwModelParser::Parse(
        FString(TEXT("pwmodel 0\n"))
        + TEXT("part shard {\n")
        + PwModelParserTest_AppendBuffersBody + TEXT(" material_id=3\n")
        + TEXT("}\n"),
        Document, Diagnostics);

    PwModelParserTest_ExpectNoErrors(*this, bParsed, Diagnostics);
    TestNull(*FString::Printf(TEXT("no conflict is reported for material_id alone. Diagnostics: [%s]"),
        *JoinPwDiagnostics(Diagnostics)),
        PwModelParserTest_FindCode(Diagnostics, PwModelDiagnosticCodes::PWMODEL_MATERIAL_ID_CONFLICT));

    if (Document.Parts.Num() == 1 && Document.Parts[0].Ops.Num() == 1)
    {
        const FPwOp& Op = Document.Parts[0].Ops[0];
        if (const FPwValue* MaterialId = Op.Params.Find(TEXT("material_id")))
        {
            TestEqual(TEXT("the raw id survives into the AST"), static_cast<int32>(MaterialId->Number), 3);
        }
        else
        {
            AddError(TEXT("the parsed append_buffers op carries no 'material_id' value"));
        }
        TestFalse(TEXT("and no material slot, which was not written"), Op.Params.Contains(TEXT("material")));
    }
    else
    {
        AddError(FString::Printf(TEXT("expected one part with one op. Diagnostics: [%s]"),
            *JoinPwDiagnostics(Diagnostics)));
    }

    return true;
}

// ============================================================================
// Collision
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserCollisionEntriesTest,
    "PinWright.Model.Parser.CollisionEntriesLandAsElements",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserCollisionEntriesTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bParsed = FPwModelParser::Parse(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n    box size=(10, 10, 10)\n}\n")
        TEXT("collision {\n")
        TEXT("    complexity = simple_as_complex\n")
        TEXT("    box size=(9, 9, 9)\n")
        TEXT("    capsule radius=2 height=10 at=(0, 0, 5)\n")
        TEXT("    hull {\n        cylinder radius=3 height=8\n    }\n")
        TEXT("}\n"),
        Document, Diagnostics);

    PwModelParserTest_ExpectNoErrors(*this, bParsed, Diagnostics);
    TestTrue(TEXT("collision block captured"), Document.Collision.IsSet());

    if (!Document.Collision.IsSet())
    {
        return false;
    }

    const FPwModelCollision& Collision = Document.Collision.GetValue();
    TestEqual(TEXT("complexity recorded as an identifier"),
        static_cast<int32>(Collision.Complexity.Type), static_cast<int32>(EPwValueType::Identifier));
    TestEqual(TEXT("complexity value"), Collision.Complexity.Text, FString(TEXT("simple_as_complex")));
    TestEqual(TEXT("three elements"), Collision.Elements.Num(), 3);

    if (Collision.Elements.Num() == 3)
    {
        TestEqual(TEXT("third element is the hull"), Collision.Elements[2].OpName, FString(TEXT("hull")));
        TestEqual(TEXT("hull body parsed as a nested op list"), Collision.Elements[2].Children.Num(), 1);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserCollisionConflictTest,
    "PinWright.Model.Parser.CollisionConflict",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserCollisionConflictTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    FPwModelParser::Parse(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n    box size=(10, 10, 10)\n}\n")
        TEXT("collision {\n")
        TEXT("    box size=(9, 9, 9)\n")
        TEXT("    auto method=convex_hulls max_hulls=4\n")
        TEXT("}\n"),
        Document, Diagnostics);

    PwModelParserTest_ExpectCode(*this, Diagnostics, PwModelDiagnosticCodes::PWMODEL_COLLISION_CONFLICT);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserDegenerateHullTest,
    "PinWright.Model.Parser.DegenerateHull",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserDegenerateHullTest::RunTest(const FString& Parameters)
{
    // An empty hull body has no geometry to hull, and that is visible without a mesh.
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXT("pwmodel 0\n")
            TEXT("part body {\n    box size=(10, 10, 10)\n}\n")
            TEXT("collision {\n    hull {\n    }\n}\n"),
            Document, Diagnostics);
        PwModelParserTest_ExpectCode(*this, Diagnostics, PwModelDiagnosticCodes::PWMODEL_DEGENERATE_HULL);
    }

    // Fewer than four points cannot bound a volume.
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXT("pwmodel 0\n")
            TEXT("part body {\n    box size=(10, 10, 10)\n}\n")
            TEXT("collision {\n    convex points=[(0,0,0), (1,0,0), (0,1,0)]\n}\n"),
            Document, Diagnostics);
        PwModelParserTest_ExpectCode(*this, Diagnostics, PwModelDiagnosticCodes::PWMODEL_DEGENERATE_HULL);
    }

    return true;
}

// ============================================================================
// Reserved constructs
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserReservedBlocksTest,
    "PinWright.Model.Parser.ReservedBlocksParseWithoutError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserReservedBlocksTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bParsed = FPwModelParser::Parse(
        TEXT("pwmodel 0\n")
        TEXT("use skeleton from \"rig.pwmodel\"\n")
        TEXT("part body {\n    box size=(1, 1, 1)\n}\n")
        TEXT("skeleton {\n    bone root at=(0, 0, 0)\n    bone spine at=(0, 0, 10)\n}\n")
        TEXT("animation walk {\n    key 0 { }\n}\n"),
        Document, Diagnostics);

    // Reserved, not unknown: the rejection is the compiler's, and it says the construct is
    // not implemented.
    // A parse error here would produce PWSRC_UNKNOWN_OP instead, which is the whole
    // reason the keywords are reserved rather than simply absent.
    PwModelParserTest_ExpectNoErrors(*this, bParsed, Diagnostics);

    TestEqual(TEXT("the use statement is recorded"), Document.Uses.Num(), 1);
    if (Document.Uses.Num() == 1)
    {
        TestEqual(TEXT("kind"), Document.Uses[0].Kind, FString(TEXT("skeleton")));
        TestEqual(TEXT("path"), Document.Uses[0].Path, FString(TEXT("rig.pwmodel")));
    }

    TestEqual(TEXT("two reserved blocks"), Document.ReservedBlocks.Num(), 2);
    if (Document.ReservedBlocks.Num() == 2)
    {
        TestEqual(TEXT("skeleton keyword"), Document.ReservedBlocks[0].Keyword, FString(TEXT("skeleton")));
        TestEqual(TEXT("animation keyword"), Document.ReservedBlocks[1].Keyword, FString(TEXT("animation")));
        TestEqual(TEXT("animation name"), Document.ReservedBlocks[1].Name, FString(TEXT("walk")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserUnknownUseKindTest,
    "PinWright.Model.Parser.UnknownUseKind",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserUnknownUseKindTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    FPwModelParser::Parse(
        TEXT("pwmodel 0\n")
        TEXT("use texture from \"albedo.png\"\n")
        TEXT("part body {\n    box size=(1, 1, 1)\n}\n"),
        Document, Diagnostics);

    PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_VALUE);
    return true;
}

// ============================================================================
// Recovery and the op table
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserRecoveryTest,
    "PinWright.Model.Parser.ReportsEveryBadLineInOneRun",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserRecoveryTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    FPwModelParser::Parse(
        TEXT("pwmodel 0\n")
        TEXT("part body {\n")
        TEXT("    box size=(1, 1, 1)\n")
        TEXT("    bevelation distance=1\n")
        TEXT("    twisty angle=10\n")
        TEXT("    smoothe iterations=2\n")
        TEXT("}\n"),
        Document, Diagnostics);

    // Three bad lines, three diagnostics: an author fixing one mistake per run is the
    // failure mode error recovery exists to prevent.
    TestEqual(*FString::Printf(TEXT("one unknown-op diagnostic per bad line. [%s]"),
        *JoinPwDiagnostics(Diagnostics)),
        PwModelParserTest_CountCode(Diagnostics, PwSourceDiagnosticCodes::PWSRC_UNKNOWN_OP), 3);

    // The AST records what was written, diagnostics record what is wrong with it - the
    // parser does not silently drop statements it rejected, because the compiler never
    // runs on a document that parsed false and a half-populated AST would only mislead a
    // later reader of the same tree.
    TestEqual(TEXT("the part survived recovery"), Document.Parts.Num(), 1);
    if (Document.Parts.Num() == 1)
    {
        TestEqual(TEXT("every statement is in the tree"), Document.Parts[0].Ops.Num(), 4);
        TestEqual(TEXT("starting with the valid one"), Document.Parts[0].Ops[0].OpName, FString(TEXT("box")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserOpTableContextTest,
    "PinWright.Model.Parser.OpTableSeparatesContexts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserOpTableContextTest::RunTest(const FString& Parameters)
{
    const FPwModelOpSpec* PartBox = PwModelOpTable::Find(TEXT("box"), EPwModelOpContext::Part);
    const FPwModelOpSpec* CollisionBox = PwModelOpTable::Find(TEXT("box"), EPwModelOpContext::Collision);
    const FPwModelOpSpec* SkinSmooth = PwModelOpTable::Find(TEXT("smooth"), EPwModelOpContext::Skin);

    TestNotNull(TEXT("box exists as a part generator"), PartBox);
    TestNotNull(TEXT("box exists as a collision element"), CollisionBox);
    TestNotNull(TEXT("smooth exists in the skin context"), SkinSmooth);

    if (PartBox && CollisionBox)
    {
        // Same token, different op: the part generator carries a material tag and
        // segments, the collision element carries neither and requires its size.
        TestTrue(TEXT("the part generator can open a part"), PartBox->bGenerator);
        TestTrue(TEXT("the part generator accepts material="), PartBox->bAcceptsMaterial);
        TestNotNull(TEXT("the part generator has segments"), PartBox->FindParam(TEXT("segments")));
        TestFalse(TEXT("the collision element takes no material"), CollisionBox->bAcceptsMaterial);
        TestNull(TEXT("the collision element has no segments"), CollisionBox->FindParam(TEXT("segments")));

        const FPwModelParamSpec* CollisionSize = CollisionBox->FindParam(TEXT("size"));
        TestNotNull(TEXT("the collision element has size"), CollisionSize);
        if (CollisionSize)
        {
            TestTrue(TEXT("and it is required"), CollisionSize->bRequired);
        }
    }

    // The op table is the vocabulary model.describe_ops publishes; an empty one would
    // ship a format with no ops and no test would otherwise notice.
    TestTrue(TEXT("the table is populated"), PwModelOpTable::Get().Num() > 50);
    TestTrue(TEXT("all op contexts are represented"),
        PwModelOpTable::NamesInContext(EPwModelOpContext::Part).Num() > 40
        && PwModelOpTable::NamesInContext(EPwModelOpContext::Collision).Num() >= 6
        && PwModelOpTable::NamesInContext(EPwModelOpContext::Skin).Num() > 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserMinimalDocumentTest,
    "PinWright.Model.Parser.MinimalDocumentParsesClean",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserMinimalDocumentTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bParsed = FPwModelParser::Parse(PwModelParserTest_MinimalPart, Document, Diagnostics);

    PwModelParserTest_ExpectNoErrors(*this, bParsed, Diagnostics);
    TestEqual(*FString::Printf(TEXT("no diagnostics at all. [%s]"), *JoinPwDiagnostics(Diagnostics)),
        Diagnostics.Num(), 0);
    TestEqual(TEXT("one part with one op"), Document.Parts.Num(), 1);

    // Exponent notation survives the parse as one number, which is the tokenizer property
    // the whole Model/ lexer exists for - asserted here at the value level.
    FPwModelDocument ExponentDocument;
    TArray<FPwDiagnostic> ExponentDiagnostics;
    FPwModelParser::Parse(
        TEXTVIEW("pwmodel 0\npart body {\n    box size=(1, 1, 1)\n    weld_vertices tolerance=1e-5\n}\n"),
        ExponentDocument, ExponentDiagnostics);

    TestEqual(*FString::Printf(TEXT("exponent literal parses clean. [%s]"),
        *JoinPwDiagnostics(ExponentDiagnostics)), ExponentDiagnostics.Num(), 0);

    if (ExponentDocument.Parts.Num() == 1 && ExponentDocument.Parts[0].Ops.Num() == 2)
    {
        const FPwValue* Tolerance = ExponentDocument.Parts[0].Ops[1].Params.Find(TEXT("tolerance"));
        TestNotNull(TEXT("tolerance captured"), Tolerance);
        if (Tolerance)
        {
            TestTrue(TEXT("tolerance is 1e-5"), FMath::IsNearlyEqual(Tolerance->Number, 1e-5, 1e-12));
        }
    }
    else
    {
        AddError(TEXT("expected one part with two ops in the exponent document"));
    }
    return true;
}

// ============================================================================
// Did-you-mean, single-table vocabulary, and one mistake costing one diagnostic
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserTypoSuggestionTiersTest,
    "PinWright.Model.Parser.TypoSuggestionsCoverBothTiers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserTypoSuggestionTiersTest::RunTest(const FString& Parameters)
{
    // Tier 1, containment: the misspelling CONTAINS a real op. Longest wins, so `bevelation`
    // resolves to `bevel` rather than to some shorter name that is also a substring.
    // Tier 2, edit distance: a TRANSPOSITION or a DELETION neither contains nor is contained
    // by its target, so a containment-only ranker offered these authors nothing at all.
    struct FCase { const TCHAR* Statement; const TCHAR* Expected; };
    const FCase Cases[] = {
        { TEXT("bevelation distance=1"), TEXT("bevel")    },  // containment
        { TEXT("spehre radius=10"),      TEXT("sphere")   },  // transposition
        { TEXT("cylnder radius=10"),     TEXT("cylinder") },  // deletion
        { TEXT("subtact"),               TEXT("subtract") },  // deletion
    };

    for (const FCase& Case : Cases)
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            FString::Printf(TEXT("pwmodel 0\npart body {\n    box size=(1, 1, 1)\n    %s\n}\n"), Case.Statement),
            Document, Diagnostics);

        const FPwDiagnostic* Diagnostic =
            PwModelParserTest_FindCode(Diagnostics, PwSourceDiagnosticCodes::PWSRC_UNKNOWN_OP);
        if (!Diagnostic)
        {
            AddError(FString::Printf(TEXT("'%s' produced no PWSRC_UNKNOWN_OP. [%s]"),
                Case.Statement, *JoinPwDiagnostics(Diagnostics)));
            continue;
        }

        TestTrue(*FString::Printf(TEXT("'%s' suggests '%s'. Suggestions: [%s]"),
            Case.Statement, Case.Expected, *FString::Join(Diagnostic->Suggestions, TEXT(", "))),
            Diagnostic->Suggestions.Contains(FString(Case.Expected)));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserMalformedPartHeaderTest,
    "PinWright.Model.Parser.MalformedPartHeaderCostsOneDiagnostic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserMalformedPartHeaderTest::RunTest(const FString& Parameters)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    FPwModelParser::Parse(
        TEXT("pwmodel 0\n")
        TEXT("part body at= {\n")
        TEXT("    box size=(10, 10, 10)\n")
        TEXT("    bevel distance=1\n")
        TEXT("}\n"),
        Document, Diagnostics);

    // A part's '{' closes its HEADER rather than opening a line of its own, so the recovery
    // that skips a malformed header line eats the brace with it. Without a resynchronise the
    // parser then reported a spurious "Expected '{' to open the part body", followed by every
    // op line and the closing '}' re-reported at model level: N + 2 diagnostics, with the one
    // real mistake buried at the top of a pile that reads like a broken file.
    TestEqual(*FString::Printf(TEXT("one bad header parameter, one diagnostic. [%s]"),
        *JoinPwDiagnostics(Diagnostics)), Diagnostics.Num(), 1);

    // And the body still parsed - resynchronising on the brace is what keeps the rest of the
    // part meaningful, so a second mistake further down is still found in the same run.
    TestEqual(TEXT("the part survived"), Document.Parts.Num(), 1);
    if (Document.Parts.Num() == 1)
    {
        TestEqual(TEXT("with both of its ops"), Document.Parts[0].Ops.Num(), 2);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserDuplicateParamTest,
    "PinWright.Model.Parser.DuplicateParamHasItsOwnCode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserDuplicateParamTest::RunTest(const FString& Parameters)
{
    // Not PWSRC_UNEXPECTED_TOKEN: every token here is legal and in a legal position, so a
    // token code reported the one thing that was not wrong with the statement.
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXTVIEW("pwmodel 0\npart body {\n    box size=(1, 1, 1) size=(2, 2, 2)\n}\n"),
            Document, Diagnostics);

        PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_DUPLICATE_PARAM);
        TestFalse(TEXT("and is not also reported as a token error"),
            PwModelParserTest_HasCode(Diagnostics, PwSourceDiagnosticCodes::PWSRC_UNEXPECTED_TOKEN));
    }

    // `complexity` twice is the same mistake spelled as an assignment.
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXT("pwmodel 0\n")
            TEXT("part body {\n    box size=(1, 1, 1)\n}\n")
            TEXT("collision {\n")
            TEXT("    complexity = simple_and_complex\n")
            TEXT("    complexity = use_default\n")
            TEXT("}\n"),
            Document, Diagnostics);

        PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_DUPLICATE_PARAM);
    }

    // A non-`complexity` assignment inside collision is an unknown NAME, not a bad token.
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXT("pwmodel 0\n")
            TEXT("part body {\n    box size=(1, 1, 1)\n}\n")
            TEXT("collision {\n    complexety = use_default\n}\n"),
            Document, Diagnostics);

        PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_UNKNOWN_PARAM);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserLightmapUsesOpTableTest,
    "PinWright.Model.Parser.LightmapValidatesAgainstTheOpTable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserLightmapUsesOpTableTest::RunTest(const FString& Parameters)
{
    // `lightmap` used to be validated against an inline ValidNames literal, so its one
    // parameter existed in the parser and in no published vocabulary. Driving it off a spec
    // is what makes the three behaviours below fall out instead of being hand-written.
    const TArrayView<const FPwModelParamSpec> Params = PwModelOpTable::LightmapParams();
    TestEqual(TEXT("lightmap publishes exactly two parameters"), Params.Num(), 2);
    if (Params.Num() == 2)
    {
        TestEqual(TEXT("named channel"), Params[0].Name, FString(TEXT("channel")));
        TestTrue(TEXT("required"), Params[0].bRequired);
        TestTrue(TEXT("and range-checked like every other UV channel"), Params[0].bHasRange);

        TestEqual(TEXT("named resolution"), Params[1].Name, FString(TEXT("resolution")));
        TestFalse(TEXT("optional - a model that omits it keeps the engine default"), Params[1].bRequired);
        // No Default text, so nothing advertises a value this statement does not apply. The
        // absent-stays-absent property that depends on it is asserted in the AST test below.
        TestTrue(TEXT("and carries no advertised default"), Params[1].Default.IsEmpty());
        TestTrue(TEXT("range-checked"), Params[1].bHasRange);
        // The engine's own bounds, not invented ones: EnforceLightmapRestrictions raises
        // anything under 4 back to 4 (StaticMesh.cpp:9732) and the UPROPERTY declares
        // ClampMax = 4096 (StaticMesh.h:1176).
        TestEqual(TEXT("min is the engine's floor of 4"), Params[1].MinValue, 4.0);
        TestEqual(TEXT("max is the UPROPERTY's ClampMax of 4096"), Params[1].MaxValue, 4096.0);
    }

    // Unknown name, with a did-you-mean drawn from the spec, plus the required-parameter
    // report that the same pass produces.
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXT("pwmodel 0\npart body {\n    box size=(1, 1, 1)\n}\nlightmap chanel=1\n"),
            Document, Diagnostics);

        PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_UNKNOWN_PARAM);
        PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_MISSING_PARAM);

        if (const FPwDiagnostic* Diagnostic =
                PwModelParserTest_FindCode(Diagnostics, PwSourceDiagnosticCodes::PWSRC_UNKNOWN_PARAM))
        {
            TestTrue(TEXT("suggests channel"), Diagnostic->Suggestions.Contains(FString(TEXT("channel"))));
        }
    }

    // Out of range, from the spec's bounds rather than from a name-keyed check.
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXT("pwmodel 0\npart body {\n    box size=(1, 1, 1)\n}\nlightmap channel=9\n"),
            Document, Diagnostics);

        PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_VALUE);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserUVLayoutUsesParamTableTest,
    "PinWright.Model.Parser.UVLayoutValidatesAgainstTheParamTable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserUVLayoutUsesParamTableTest::RunTest(const FString& Parameters)
{
    const TArrayView<const FPwModelParamSpec> Params = PwModelOpTable::UVLayoutParams();
    TestEqual(TEXT("uv_layout publishes exactly two parameters"), Params.Num(), 2);
    if (Params.Num() == 2)
    {
        TestEqual(TEXT("channel is first"), Params[0].Name, FString(TEXT("channel")));
        TestTrue(TEXT("channel is required"), Params[0].bRequired);
        TestTrue(TEXT("channel uses the shared 0-7 range"), Params[0].bHasRange);
        TestEqual(TEXT("texture resolution is second"), Params[1].Name,
            FString(TEXT("texture_resolution")));
        TestEqual(TEXT("texture resolution advertises the LayoutUV default"),
            Params[1].Default,
            FString::FromInt(GeometryOps::LayoutUVTextureResolutionDefault));
        TestTrue(TEXT("texture resolution is ranged"), Params[1].bHasRange);
        TestEqual(TEXT("texture resolution uses the packer's minimum"), Params[1].MinValue,
            static_cast<double>(GeometryOps::LayoutUVTextureResolutionMin));
        TestEqual(TEXT("texture resolution uses the packer's maximum"), Params[1].MaxValue,
            static_cast<double>(GeometryOps::LayoutUVTextureResolutionMax));

        const FPwModelOpSpec* UVOp = PwModelOpTable::Find(TEXT("uv"), EPwModelOpContext::Part);
        const FPwModelParamSpec* PartResolution = UVOp
            ? UVOp->FindParam(TEXT("texture_resolution")) : nullptr;
        if (TestNotNull(TEXT("part-level uv publishes texture_resolution"), PartResolution))
        {
            TestEqual(TEXT("both layout surfaces share the production default"),
                PartResolution->Default, Params[1].Default);
            TestEqual(TEXT("both layout surfaces share the minimum"),
                PartResolution->MinValue, Params[1].MinValue);
            TestEqual(TEXT("both layout surfaces share the maximum"),
                PartResolution->MaxValue, Params[1].MaxValue);
        }
    }

    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        const bool bParsed = FPwModelParser::Parse(
            TEXT("pwmodel 0\n")
            TEXT("part body {\n    box size=(1, 1, 1)\n}\n")
            TEXT("uv_layout channel=0 texture_resolution=64\n")
            TEXT("uv_layout channel=1\n"),
            Document, Diagnostics);

        PwModelParserTest_ExpectNoErrors(*this, bParsed, Diagnostics);
        TestEqual(TEXT("multiple distinct channels reach the AST"), Document.UVLayouts.Num(), 2);
        if (Document.UVLayouts.Num() == 2)
        {
            const FPwValue* Resolution =
                Document.UVLayouts[0].Params.Find(TEXT("texture_resolution"));
            TestTrue(TEXT("explicit texture resolution reaches the AST"),
                Resolution != nullptr && Resolution->Number == 64.0);
            TestFalse(TEXT("omitted texture resolution stays absent"),
                Document.UVLayouts[1].Params.Contains(TEXT("texture_resolution")));
        }
    }

    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXT("pwmodel 0\n")
            TEXT("part body {\n    box size=(1, 1, 1)\n}\n")
            TEXT("uv_layout channel=1\n")
            TEXT("uv_layout channel=1 texture_resolution=64\n"),
            Document, Diagnostics);

        const FPwDiagnostic* Duplicate =
            PwModelParserTest_FindCode(Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_BLOCK);
        if (TestNotNull(TEXT("duplicate channel is rejected"), Duplicate))
        {
            TestEqual(TEXT("duplicate anchors to the second statement"), Duplicate->Line, 6);
            TestTrue(TEXT("duplicate message names channel 1"),
                Duplicate->Message.Contains(TEXT("channel 1")));
        }
    }

    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXT("pwmodel 0\npart body {\n    box size=(1, 1, 1)\n}\nuv_layout channel=8\n"),
            Document, Diagnostics);
        PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_VALUE);
    }

    for (const int32 InvalidResolution : {
             -1,
             0,
             GeometryOps::LayoutUVTextureResolutionMin - 1,
             GeometryOps::LayoutUVTextureResolutionMax + 1 })
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            *FString::Printf(
                TEXT("pwmodel 0\npart body {\n    box size=(1, 1, 1)\n}\nuv_layout channel=1 texture_resolution=%d\n"),
                InvalidResolution),
            Document, Diagnostics);
        PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_VALUE);
    }

    return true;
}

// `resolution` in the AST, and - the half that carries the defect - its ABSENCE when omitted.
//
// The compiler reads it with GetInt(Params, "resolution", INDEX_NONE), so "the author said
// nothing" and "the author said 4" have to be distinguishable in this map. If a default were
// ever materialised into FPwOp::Params, every model would start forcing a resolution and
// the INDEX_NONE branch in FCompiler::CreateAsset would become unreachable - silently.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserLightmapResolutionTest,
    "PinWright.Model.Parser.LightmapResolutionReachesTheAst",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserLightmapResolutionTest::RunTest(const FString& Parameters)
{
    // Present: parses clean and lands as a Number in the lightmap statement's params.
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        const bool bParsed = FPwModelParser::Parse(
            TEXT("pwmodel 0\npart body {\n    box size=(1, 1, 1)\n}\nlightmap channel=1 resolution=256\n"),
            Document, Diagnostics);

        PwModelParserTest_ExpectNoErrors(*this, bParsed, Diagnostics);
        if (TestTrue(TEXT("lightmap captured"), Document.Lightmap.IsSet()))
        {
            const TMap<FString, FPwValue>& Params = Document.Lightmap.GetValue().Params;
            const FPwValue* Resolution = Params.Find(TEXT("resolution"));
            if (TestTrue(TEXT("resolution reached the AST"), Resolution != nullptr))
            {
                TestEqual(TEXT("as a number"), static_cast<int32>(Resolution->Type),
                    static_cast<int32>(EPwValueType::Number));
                TestEqual(TEXT("carrying the authored value"), Resolution->Number, 256.0);
            }
            TestTrue(TEXT("and the channel is still there"), Params.Contains(TEXT("channel")));
        }
    }

    // Absent: no key at all, so the compiler's INDEX_NONE fallback is what runs.
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        const bool bParsed = FPwModelParser::Parse(
            TEXT("pwmodel 0\npart body {\n    box size=(1, 1, 1)\n}\nlightmap channel=1\n"),
            Document, Diagnostics);

        PwModelParserTest_ExpectNoErrors(*this, bParsed, Diagnostics);
        if (TestTrue(TEXT("lightmap captured with no resolution"), Document.Lightmap.IsSet()))
        {
            TestFalse(TEXT("an omitted resolution is ABSENT, not defaulted into the map"),
                Document.Lightmap.GetValue().Params.Contains(TEXT("resolution")));
        }
    }

    // Both bounds, from the spec rather than from a name-keyed check.
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXT("pwmodel 0\npart body {\n    box size=(1, 1, 1)\n}\nlightmap channel=1 resolution=2\n"),
            Document, Diagnostics);

        PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_VALUE);
    }
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXT("pwmodel 0\npart body {\n    box size=(1, 1, 1)\n}\nlightmap channel=1 resolution=8192\n"),
            Document, Diagnostics);

        PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_VALUE);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserPartHeaderUsesOpTableTest,
    "PinWright.Model.Parser.PartHeaderValidatesAgainstTheOpTable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserPartHeaderUsesOpTableTest::RunTest(const FString& Parameters)
{
    const TArrayView<const FPwModelParamSpec> Params = PwModelOpTable::PartHeaderParams();
    TestEqual(TEXT("a part header publishes bone, allow_floating and three transform parameters"), Params.Num(), 5);

    struct FHeaderExpectation
    {
        const TCHAR* Name;
        EPwModelParamType Type;
    };
    const FHeaderExpectation HeaderExpectations[] = {
        { TEXT("bone"), EPwModelParamType::String },
        { TEXT("allow_floating"), EPwModelParamType::Bool },
        { TEXT("at"), EPwModelParamType::Vector3 },
        { TEXT("rotate"), EPwModelParamType::Vector3 },
        { TEXT("scale"), EPwModelParamType::Vector3 },
    };
    for (const FHeaderExpectation& Expected : HeaderExpectations)
    {
        const FPwModelParamSpec* Spec = Params.FindByPredicate(
            [&Expected](const FPwModelParamSpec& Candidate) { return Candidate.Name == Expected.Name; });
        TestNotNull(*FString::Printf(TEXT("the header publishes '%s'"), Expected.Name), Spec);
        if (Spec)
        {
            TestEqual(*FString::Printf(TEXT("'%s' has its published type"), Expected.Name),
                static_cast<int32>(Spec->Type), static_cast<int32>(Expected.Type));
        }
    }

    // A header parameter with the right name and the wrong shape is still caught, because the
    // check reads the spec's type instead of a throwaway one fabricated at the call site.
    FPwModelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    FPwModelParser::Parse(
        TEXTVIEW("pwmodel 0\npart body at=(1, 2) {\n    box size=(1, 1, 1)\n}\n"), Document, Diagnostics);

    PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_TUPLE_ARITY);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserChannelRangeIsTableDrivenTest,
    "PinWright.Model.Parser.ChannelRangeIsTableDriven",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserChannelRangeIsTableDrivenTest::RunTest(const FString& Parameters)
{
    // The 0-7 check used to be gated on a hardcoded uv / set_uvs / transform_uvs name triple,
    // so an op that later grew a channel would have lost the check with nothing to fail.
    // Every channel parameter now carries its own bounds, which is also what lets
    // model.describe_ops publish the range instead of leaving it buried in a message.
    int32 ChannelParams = 0;
    for (const FPwModelOpSpec& Op : PwModelOpTable::Get())
    {
        for (const FPwModelParamSpec& Param : Op.Params)
        {
            if (Param.Name != TEXT("channel"))
            {
                continue;
            }
            ++ChannelParams;
            TestTrue(*FString::Printf(TEXT("'%s' channel carries a range"), *Op.Name), Param.bHasRange);
            TestEqual(*FString::Printf(TEXT("'%s' channel starts at 0"), *Op.Name), Param.MinValue, 0.0);
            TestEqual(*FString::Printf(TEXT("'%s' channel ends at 7"), *Op.Name), Param.MaxValue, 7.0);
        }
    }
    TestTrue(TEXT("the table still has channel parameters to check"), ChannelParams >= 3);

    const TCHAR* const OutOfRange[] = {
        TEXT("uv channel=9 mode=box"), TEXT("uv channel=-1 mode=box"), TEXT("transform_uvs channel=8") };
    for (const TCHAR* Statement : OutOfRange)
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            FString::Printf(TEXT("pwmodel 0\npart body {\n    box size=(1, 1, 1)\n    %s\n}\n"), Statement),
            Document, Diagnostics);

        TestTrue(*FString::Printf(TEXT("'%s' is out of range. [%s]"), Statement,
            *JoinPwDiagnostics(Diagnostics)),
            PwModelParserTest_HasCode(Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_VALUE));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserCollisionVocabularyIsDerivedTest,
    "PinWright.Model.Parser.CollisionVocabularyComesFromTheCollisionLayer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserCollisionVocabularyIsDerivedTest::RunTest(const FString& Parameters)
{
    // The parser used to hard-code the four `complexity` spellings and the eight `auto
    // method=` spellings next to a collision layer that derives both from their engine enums.
    // Two lists mean a value added to ECollisionTraceFlag is accepted by one half of the
    // pipeline and rejected by the other, with nothing to fail. Asserting equality is what
    // makes that impossible rather than merely unlikely.
    const FPwModelOpSpec* Auto = PwModelOpTable::Find(TEXT("auto"), EPwModelOpContext::Collision);
    TestNotNull(TEXT("collision auto exists"), Auto);
    if (Auto)
    {
        const FPwModelParamSpec* Method = Auto->FindParam(TEXT("method"));
        TestNotNull(TEXT("with a method parameter"), Method);
        if (Method)
        {
            const TArray<FString>& Derived = PwModelCollisionNames::AutoMethodNames();
            TestTrue(TEXT("the derived method list is not empty"), Derived.Num() > 0);
            TestEqual(TEXT("auto method= publishes as many spellings as the collision layer derives"),
                Method->AllowedValues.Num(), Derived.Num());
            for (const FString& Spelling : Derived)
            {
                TestTrue(*FString::Printf(TEXT("auto method= accepts the derived spelling '%s'"), *Spelling),
                    Method->AllowedValues.Contains(Spelling));
            }
        }
    }

    // `complexity` has no op spec of its own to compare against, so it is pinned through the
    // parse instead: every derived spelling is accepted, and a near-miss is rejected.
    //
    // The literal list below is the INDEPENDENT reference and is not redundant with the loop
    // that follows. That loop feeds ComplexityNames() back into a parser which validates
    // against ComplexityNames(), so it is one list checked against itself: it passes for any
    // content the derivation happens to produce, and it asserts NOTHING AT ALL if the
    // derivation produces an empty list - zero iterations, zero assertions, and the negative
    // case below still reports PWSRC_BAD_VALUE because an empty allowed-set rejects
    // everything. Anchoring on the four spellings docs/pwmodel-format.md publishes is what
    // makes an emptied table, a dropped `use_` rule, or a broken snake_case conversion fail
    // here. The count is 4 because ECollisionTraceFlag has four values on UE 5.8
    // (BodySetupEnums.h) and CTF_MAX is skipped; an engine that adds one must update the
    // format doc's table too, so failing on that is the drift signal, not brittleness.
    const TArray<FString>& ComplexityNames = PwModelCollisionNames::ComplexityNames();
    TestEqual(*FString::Printf(TEXT("the derived complexity list has four spellings. [%s]"),
        *FString::Join(ComplexityNames, TEXT(", "))), ComplexityNames.Num(), 4);

    const TCHAR* const DocumentedComplexity[] = {
        TEXT("simple_and_complex"), TEXT("simple_as_complex"),
        TEXT("complex_as_simple"), TEXT("use_default") };
    for (const TCHAR* Documented : DocumentedComplexity)
    {
        TestTrue(*FString::Printf(TEXT("the derivation still spells '%s' the way the format doc does. [%s]"),
            Documented, *FString::Join(ComplexityNames, TEXT(", "))),
            ComplexityNames.Contains(FString(Documented)));
    }

    for (const FString& Name : ComplexityNames)
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        const bool bParsed = FPwModelParser::Parse(
            FString::Printf(
                TEXT("pwmodel 0\npart body {\n    box size=(1, 1, 1)\n}\ncollision {\n    complexity = %s\n    box size=(1, 1, 1)\n}\n"),
                *Name),
            Document, Diagnostics);

        TestTrue(*FString::Printf(TEXT("complexity = %s is accepted. [%s]"),
            *Name, *JoinPwDiagnostics(Diagnostics)), bParsed);
    }

    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXT("pwmodel 0\n")
            TEXT("part body {\n    box size=(1, 1, 1)\n}\n")
            TEXT("collision {\n    complexity = simple_and_complexx\n    box size=(1, 1, 1)\n}\n"),
            Document, Diagnostics);

        PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_VALUE);
    }

    return true;
}

// ============================================================================
// frame_list
// ============================================================================
//
// The value kind the three path-driven ops are built on. What each assertion holds back:
//
//  - A 6-wide entry parsing clean on all three ops is the whole capability. FPwValue's
//    TupleList has always been ragged, so the entry PARSED before this type existed; what was
//    missing was a constraint that could declare "a list of frames", and nothing else in the
//    format would notice its absence except by accepting a 3-tuple where a frame is required.
//  - The arity message names the ORDERING. Six numbers whose first three are Unreal units and
//    whose last three are degrees cannot be corrected from "expects 6 components" alone, which
//    is the reason this type is not spelled PointList6.
//  - The wire spelling is asserted directly because model.describe_ops publishes it verbatim and
//    docs/pwmodel-format.md's value-kind table names it; a rename that updated neither would
//    leave an author reading a type that does not exist.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelParserFrameListTest,
    "PinWright.Model.Parser.FrameListArity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelParserFrameListTest::RunTest(const FString& Parameters)
{
    TestEqual(TEXT("the frame list type spells itself frame_list on the wire"),
        FString(PwModelParamTypeToString(EPwModelParamType::FrameList)), FString(TEXT("frame_list")));

    // Correct arity, on every op that takes one.
    const TCHAR* const FrameOps[] = {
        TEXT("sweep path=[(0, 0, 0, 0, 0, 0), (0, 0, 100, 0, 15, 0)]"),
        TEXT("extrude_along_spline path=[(0, 0, 0, 0, 0, 0), (30, 0, 180, 0, 15, 0)]"),
        TEXT("array_along_path path=[(0, 0, 0, 0, 0, 0), (0, 0, 100, 0, 0, 90)]"),
    };
    for (const TCHAR* Statement : FrameOps)
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        const bool bParsed = FPwModelParser::Parse(
            FString::Printf(TEXT("pwmodel 0\npart body {\n    box size=(20, 20, 20)\n    %s\n}\n"), Statement),
            Document, Diagnostics);

        TestTrue(*FString::Printf(TEXT("'%s' parses clean. [%s]"),
            Statement, *JoinPwDiagnostics(Diagnostics)), bParsed);
    }

    // A point where a frame is required. The message must carry the arity AND the ordering.
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXT("pwmodel 0\npart body {\n    box size=(20, 20, 20)\n")
            TEXT("    sweep path=[(0, 0, 0, 0, 0, 0), (0, 0, 100)]\n}\n"),
            Document, Diagnostics);

        PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_TUPLE_ARITY);

        const FPwDiagnostic* Arity =
            PwModelParserTest_FindCode(Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_TUPLE_ARITY);
        if (Arity)
        {
            for (const TCHAR* Fragment : { TEXT("Entry 1"), TEXT("'path'"), TEXT("'sweep'"),
                                           TEXT("6 components"), TEXT("(x, y, z, roll, pitch, yaw)"),
                                           TEXT("found 3") })
            {
                TestTrue(*FString::Printf(TEXT("the arity message carries '%s'. Message: %s"),
                    Fragment, *Arity->Message), Arity->Message.Contains(Fragment));
            }
        }
    }

    // A tuple where a LIST of frames is required: a different mistake, and it must name the type
    // rather than the arity.
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXT("pwmodel 0\npart body {\n    box size=(20, 20, 20)\n")
            TEXT("    array_along_path path=(0, 0, 0, 0, 0, 0)\n}\n"),
            Document, Diagnostics);

        PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_VALUE);

        const FPwDiagnostic* Bad =
            PwModelParserTest_FindCode(Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_VALUE);
        if (Bad)
        {
            TestTrue(*FString::Printf(TEXT("the message names the expected type. Message: %s"), *Bad->Message),
                Bad->Message.Contains(TEXT("frame_list")));
            TestTrue(*FString::Printf(TEXT("the message names what was written. Message: %s"), *Bad->Message),
                Bad->Message.Contains(TEXT("a tuple")));
        }
    }

    // `path` is required on the two ops that cannot do anything sensible without one, and
    // OPTIONAL on sweep, whose no-path branch is the documented vertical fallback.
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXTVIEW("pwmodel 0\npart body {\n    box size=(20, 20, 20)\n    extrude_along_spline segments=8\n}\n"),
            Document, Diagnostics);
        PwModelParserTest_ExpectCode(*this, Diagnostics, PwSourceDiagnosticCodes::PWSRC_MISSING_PARAM);
    }
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        const bool bParsed = FPwModelParser::Parse(
            TEXTVIEW("pwmodel 0\npart body {\n    box size=(20, 20, 20)\n    sweep steps=8\n}\n"),
            Document, Diagnostics);
        PwModelParserTest_ExpectNoErrors(*this, bParsed, Diagnostics);
    }

    // None of the three may open a part: without `profile=` the accumulated geometry is what
    // sizes the cross-section, and array_along_path has nothing to place.
    {
        FPwModelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(
            TEXTVIEW("pwmodel 0\npart body {\n    sweep path=[(0, 0, 0, 0, 0, 0), (0, 0, 100, 0, 0, 0)]\n}\n"),
            Document, Diagnostics);
        PwModelParserTest_ExpectCode(*this, Diagnostics, PwModelDiagnosticCodes::PWMODEL_PART_NEEDS_PRIMITIVE);
    }

    return true;
}
