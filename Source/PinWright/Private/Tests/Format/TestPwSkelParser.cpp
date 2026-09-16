// Copyright (c) 2026 Alexander Penkin. MIT License.

// Parser contracts for the standalone .pwskel format. These tests intentionally stop at the
// source AST: asset creation and save semantics belong to the later skeleton chunks.
#include "Misc/AutomationTest.h"

#include "PwSkel/PwSkelAst.h"
#include "PwSkel/PwSkelDiagnostic.h"
#include "PwSkel/PwSkelParser.h"

namespace
{
    bool PwSkelParserTest_HasCode(const TArray<FPwDiagnostic>& Diagnostics, const TCHAR* Code)
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

    const FPwDiagnostic* PwSkelParserTest_FindCode(const TArray<FPwDiagnostic>& Diagnostics,
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

    int32 PwSkelParserTest_CountCode(const TArray<FPwDiagnostic>& Diagnostics, const TCHAR* Code)
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

    void PwSkelParserTest_ExpectCode(FAutomationTestBase& Test,
                                     const TArray<FPwDiagnostic>& Diagnostics,
                                     const TCHAR* Code)
    {
        Test.TestTrue(*FString::Printf(TEXT("%s was reported. Diagnostics: [%s]"), Code,
            *JoinPwDiagnostics(Diagnostics)), PwSkelParserTest_HasCode(Diagnostics, Code));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSkelParserGrammarTest,
    "PinWright.Skeleton.Parser.StandaloneGrammarParsesNestedBones",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwSkelParserGrammarTest::RunTest(const FString& Parameters)
{
    FPwSkelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bParsed = FPwSkelParser::Parse(
        TEXTVIEW("pwskel 0\n")
        TEXT("bone \"root\" {\n")
        TEXT("    bone \"pelvis\" at=(0, 0, 90) rotate=(10, 20, 30) scale=(1, 2, 3) {\n")
        TEXT("        bone \"hand left\" { }\n")
        TEXT("    }\n")
        TEXT("}\n"),
        Document, Diagnostics);

    TestTrue(*FString::Printf(TEXT("standalone .pwskel grammar parses. [%s]"),
        *JoinPwDiagnostics(Diagnostics)), bParsed);
    TestEqual(TEXT("no diagnostics"), Diagnostics.Num(), 0);
    TestEqual(TEXT("format keyword"), Document.Header.FormatKeyword, FString(TEXT("pwskel")));
    TestEqual(TEXT("version"), Document.Header.Version, 0);
    TestEqual(TEXT("one root"), Document.Roots.Num(), 1);

    if (Document.Roots.Num() == 1)
    {
        const FPwBone& Root = Document.Roots[0];
        TestEqual(TEXT("root name is a decoded string literal"), Root.Name, FString(TEXT("root")));
        TestEqual(TEXT("root has one child"), Root.Children.Num(), 1);
        if (Root.Children.Num() == 1)
        {
            const FPwBone& Pelvis = Root.Children[0];
            TestEqual(TEXT("child name keeps spaces from a string literal"), Pelvis.Name,
                FString(TEXT("pelvis")));
            TestEqual(TEXT("transform has at, rotate and scale"), Pelvis.Transform.Num(), 3);
            TestEqual(TEXT("leaf bone is represented by an empty child list"),
                Pelvis.Children.Num(), 1);
            if (Pelvis.Children.Num() == 1)
            {
                TestEqual(TEXT("leaf name"), Pelvis.Children[0].Name, FString(TEXT("hand left")));
                TestEqual(TEXT("leaf children"), Pelvis.Children[0].Children.Num(), 0);
            }
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSkelParserRejectsAssetWrapperTest,
    "PinWright.Skeleton.Parser.RejectsRemovedSkeletonWrapper",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwSkelParserRejectsAssetWrapperTest::RunTest(const FString& Parameters)
{
    FPwSkelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bParsed = FPwSkelParser::Parse(
        TEXTVIEW("pwskel 0\n")
        TEXT("skeleton \"/Game/Chars/SK_Hero\" {\n")
        TEXT("    bone \"root\" { }\n")
        TEXT("}\n"),
        Document, Diagnostics);

    TestFalse(TEXT("the removed skeleton/path wrapper is not accepted"), bParsed);
    PwSkelParserTest_ExpectCode(*this, Diagnostics,
        PwSourceDiagnosticCodes::PWSRC_UNEXPECTED_TOKEN);

    if (const FPwDiagnostic* Diagnostic = PwSkelParserTest_FindCode(Diagnostics,
            PwSourceDiagnosticCodes::PWSRC_UNEXPECTED_TOKEN))
    {
        TestTrue(TEXT("wrapper diagnostic names the direct-bone spelling"),
            Diagnostic->Message.Contains(TEXT("declare bones directly")));
    }

    TestEqual(TEXT("wrapper does not produce roots"), Document.Roots.Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSkelParserRequiresStringBoneNamesTest,
    "PinWright.Skeleton.Parser.BoneNamesAreStringLiterals",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwSkelParserRequiresStringBoneNamesTest::RunTest(const FString& Parameters)
{
    FPwSkelDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bParsed = FPwSkelParser::Parse(
        TEXTVIEW("pwskel 0\n")
        TEXT("bone root { }\n"),
        Document, Diagnostics);

    TestFalse(TEXT("an identifier is not a bone name"), bParsed);
    PwSkelParserTest_ExpectCode(*this, Diagnostics,
        PwSourceDiagnosticCodes::PWSRC_UNEXPECTED_TOKEN);
    TestEqual(TEXT("invalid positional name does not enter the AST"), Document.Roots.Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSkelParserStructuralDiagnosticsTest,
    "PinWright.Skeleton.Parser.ReportsHierarchyStructuralErrors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwSkelParserStructuralDiagnosticsTest::RunTest(const FString& Parameters)
{
    {
        FPwSkelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        const bool bParsed = FPwSkelParser::Parse(
            TEXTVIEW("pwskel 0\n"), Document, Diagnostics);
        TestFalse(TEXT("a skeleton with no bones does not parse"), bParsed);
        PwSkelParserTest_ExpectCode(*this, Diagnostics,
            PwSkelDiagnosticCodes::PWSKEL_NO_BONES);
    }

    {
        FPwSkelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        const bool bParsed = FPwSkelParser::Parse(
            TEXTVIEW("pwskel 0\n")
            TEXT("bone \"a\" { }\n")
            TEXT("bone \"b\" { }\n"),
            Document, Diagnostics);
        TestFalse(TEXT("two top-level roots do not parse"), bParsed);
        PwSkelParserTest_ExpectCode(*this, Diagnostics,
            PwSkelDiagnosticCodes::PWSKEL_MULTIPLE_ROOTS);
    }

    {
        FPwSkelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        const bool bParsed = FPwSkelParser::Parse(
            TEXTVIEW("pwskel 0\n")
            TEXT("bone \"root\" {\n")
            TEXT("    bone \"arm\" { }\n")
            TEXT("    bone \"arm\" { }\n")
            TEXT("}\n"),
            Document, Diagnostics);
        TestFalse(TEXT("duplicate bone names do not parse"), bParsed);
        PwSkelParserTest_ExpectCode(*this, Diagnostics,
            PwSkelDiagnosticCodes::PWSKEL_DUPLICATE_BONE);
    }

    {
        FPwSkelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        const bool bParsed = FPwSkelParser::Parse(
            TEXTVIEW("pwskel 0\n")
            TEXT("bone \"root\"\n"),
            Document, Diagnostics);
        TestFalse(TEXT("a bone without a block does not parse"), bParsed);
        PwSkelParserTest_ExpectCode(*this, Diagnostics,
            PwSourceDiagnosticCodes::PWSRC_BAD_BLOCK);
    }

    return true;
}

// A bone scale that collapses the rig used to validate completely clean. These tests run both
// directions: the refusal must fire on a value that produces a singular reference pose, AND it
// must stay silent on ordinary non-uniform scale -- either assertion alone is satisfied by a
// one-line change that recreates the other defect.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSkelParserScaleDomainTest,
    "PinWright.Skeleton.Parser.BoneScaleDomainIsChecked",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwSkelParserScaleDomainTest::RunTest(const FString& Parameters)
{
    // The reported defect verbatim: scale=(0,0,0) parsed with zero diagnostics.
    {
        FPwSkelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        const bool bParsed = FPwSkelParser::Parse(
            TEXTVIEW("pwskel 0\n")
            TEXT("bone \"root\" scale=(0, 0, 0) { }\n"),
            Document, Diagnostics);

        TestFalse(TEXT("scale=(0,0,0) is refused"), bParsed);
        PwSkelParserTest_ExpectCode(*this, Diagnostics,
            PwSkelDiagnosticCodes::PWSKEL_DEGENERATE_SCALE);

        if (const FPwDiagnostic* Diagnostic = PwSkelParserTest_FindCode(Diagnostics,
                PwSkelDiagnosticCodes::PWSKEL_DEGENERATE_SCALE))
        {
            TestTrue(TEXT("a collapsing scale is an error, not a warning"),
                Diagnostic->Severity == EPwSeverity::Error);
            TestEqual(TEXT("the diagnostic is scoped to the offending bone"),
                Diagnostic->ScopeName, FString(TEXT("root")));
            TestTrue(TEXT("the message says what a singular reference pose costs"),
                Diagnostic->Message.Contains(TEXT("singular")));
        }
    }

    // The boundary that makes this worth having. A test for exact zero alone would pass this
    // document, and it collapses the arm just as completely as (0,0,0) does.
    {
        FPwSkelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        const bool bParsed = FPwSkelParser::Parse(
            TEXTVIEW("pwskel 0\n")
            TEXT("bone \"root\" {\n")
            TEXT("    bone \"arm\" scale=(0.00001, 1, 1) { }\n")
            TEXT("}\n"),
            Document, Diagnostics);

        TestFalse(TEXT("a near-zero axis is refused, not only an exact zero"), bParsed);
        PwSkelParserTest_ExpectCode(*this, Diagnostics,
            PwSkelDiagnosticCodes::PWSKEL_DEGENERATE_SCALE);

        if (const FPwDiagnostic* Diagnostic = PwSkelParserTest_FindCode(Diagnostics,
                PwSkelDiagnosticCodes::PWSKEL_DEGENERATE_SCALE))
        {
            TestEqual(TEXT("the nested bone is named, not its parent"),
                Diagnostic->ScopeName, FString(TEXT("arm")));
        }
    }

    // The accepting direction. Without this the whole check is satisfied by refusing every
    // scale, and non-uniform scale is ordinary rig authoring.
    {
        FPwSkelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        const bool bParsed = FPwSkelParser::Parse(
            TEXTVIEW("pwskel 0\n")
            TEXT("bone \"root\" scale=(0.5, 1, 2) {\n")
            TEXT("    bone \"arm\" scale=(0.01, 0.01, 0.01) { }\n")
            TEXT("}\n"),
            Document, Diagnostics);

        TestTrue(*FString::Printf(TEXT("ordinary non-uniform scale still parses. [%s]"),
            *JoinPwDiagnostics(Diagnostics)), bParsed);
        TestEqual(*FString::Printf(TEXT("and reports nothing at all. [%s]"),
            *JoinPwDiagnostics(Diagnostics)), Diagnostics.Num(), 0);
    }

    // Omitting scale means the (1,1,1) default and must not be read as a missing value.
    {
        FPwSkelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        const bool bParsed = FPwSkelParser::Parse(
            TEXTVIEW("pwskel 0\n")
            TEXT("bone \"root\" at=(0, 0, 10) { }\n"),
            Document, Diagnostics);

        TestTrue(TEXT("a bone with no scale= parses"), bParsed);
        TestEqual(TEXT("and raises no scale diagnostic"),
            PwSkelParserTest_CountCode(Diagnostics,
                PwSkelDiagnosticCodes::PWSKEL_DEGENERATE_SCALE), 0);
    }

    return true;
}

// A negative axis is a different defect with a different answer: it mirrors the bone's frame
// rather than collapsing it, so the asset is still usable and the compile must still produce
// it. Downgrading this to silence, or promoting it to an error, are both regressions.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSkelParserMirroredScaleTest,
    "PinWright.Skeleton.Parser.NegativeBoneScaleWarnsAndStillParses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwSkelParserMirroredScaleTest::RunTest(const FString& Parameters)
{
    {
        FPwSkelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        const bool bParsed = FPwSkelParser::Parse(
            TEXTVIEW("pwskel 0\n")
            TEXT("bone \"root\" scale=(-1, 1, 1) { }\n"),
            Document, Diagnostics);

        TestTrue(*FString::Printf(TEXT("a mirrored bone still parses. [%s]"),
            *JoinPwDiagnostics(Diagnostics)), bParsed);
        TestEqual(TEXT("the mirrored document still produces its root"), Document.Roots.Num(), 1);
        PwSkelParserTest_ExpectCode(*this, Diagnostics,
            PwSkelDiagnosticCodes::PWSKEL_MIRRORED_SCALE);

        if (const FPwDiagnostic* Diagnostic = PwSkelParserTest_FindCode(Diagnostics,
                PwSkelDiagnosticCodes::PWSKEL_MIRRORED_SCALE))
        {
            TestTrue(TEXT("mirroring is a warning, so the compile is not blocked"),
                Diagnostic->Severity == EPwSeverity::Warning);
            TestTrue(TEXT("the message says handedness flips for descendants too"),
                Diagnostic->Message.Contains(TEXT("descendant")));
        }
    }

    // One axis, one mistake. A negative AND vanishing axis is the singular case; reporting the
    // mirror for it as well would double-count a single bad number.
    {
        FPwSkelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwSkelParser::Parse(
            TEXTVIEW("pwskel 0\n")
            TEXT("bone \"root\" scale=(-0.000001, 1, 1) { }\n"),
            Document, Diagnostics);

        TestEqual(TEXT("a negative vanishing axis reports the singular error"),
            PwSkelParserTest_CountCode(Diagnostics,
                PwSkelDiagnosticCodes::PWSKEL_DEGENERATE_SCALE), 1);
        TestEqual(TEXT("and does not also report the mirror for that same axis"),
            PwSkelParserTest_CountCode(Diagnostics,
                PwSkelDiagnosticCodes::PWSKEL_MIRRORED_SCALE), 0);
    }

    // A bone carrying both faults on DIFFERENT axes reports both, at their own severities.
    {
        FPwSkelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        const bool bParsed = FPwSkelParser::Parse(
            TEXTVIEW("pwskel 0\n")
            TEXT("bone \"root\" scale=(0, -2, 1) { }\n"),
            Document, Diagnostics);

        TestFalse(TEXT("the singular axis still fails the document"), bParsed);
        TestEqual(TEXT("the collapsed axis is reported"),
            PwSkelParserTest_CountCode(Diagnostics,
                PwSkelDiagnosticCodes::PWSKEL_DEGENERATE_SCALE), 1);
        TestEqual(TEXT("and the separately mirrored axis is reported too"),
            PwSkelParserTest_CountCode(Diagnostics,
                PwSkelDiagnosticCodes::PWSKEL_MIRRORED_SCALE), 1);
    }

    return true;
}

// Sockets and virtual bones are deliberately not authorable here, so the refusal has to name
// the tool that does own them. A refusal that only says "not valid" costs the author a round
// trip to find out where the capability actually lives.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSkelParserSteersToOwningVerbTest,
    "PinWright.Skeleton.Parser.RefusalNamesTheVerbThatOwnsTheConstruct",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwSkelParserSteersToOwningVerbTest::RunTest(const FString& Parameters)
{
    // Inside a bone block is where an author would actually write it.
    {
        FPwSkelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        const bool bParsed = FPwSkelParser::Parse(
            TEXTVIEW("pwskel 0\n")
            TEXT("bone \"root\" {\n")
            TEXT("    socket \"attach_point\" { }\n")
            TEXT("}\n"),
            Document, Diagnostics);

        TestFalse(TEXT("a socket declaration is refused"), bParsed);
        if (const FPwDiagnostic* Diagnostic = PwSkelParserTest_FindCode(Diagnostics,
                PwSourceDiagnosticCodes::PWSRC_UNEXPECTED_TOKEN))
        {
            TestTrue(TEXT("and the refusal names the verb that owns sockets"),
                Diagnostic->Message.Contains(TEXT("skeleton.create_socket")));
            TestTrue(TEXT("and says a later recompile refuses the unmanaged state"),
                Diagnostic->Message.Contains(TEXT("refuses")));
            TestTrue(TEXT("and names the explicit permission for discarding it"),
                Diagnostic->Message.Contains(TEXT("overwrite=true")));
        }
        else
        {
            AddError(TEXT("No PWSRC_UNEXPECTED_TOKEN was raised for a socket declaration."));
        }
    }

    {
        FPwSkelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwSkelParser::Parse(
            TEXTVIEW("pwskel 0\n")
            TEXT("bone \"root\" {\n")
            TEXT("    virtual_bone \"vb\" { }\n")
            TEXT("}\n"),
            Document, Diagnostics);

        if (const FPwDiagnostic* Diagnostic = PwSkelParserTest_FindCode(Diagnostics,
                PwSourceDiagnosticCodes::PWSRC_UNEXPECTED_TOKEN))
        {
            TestTrue(TEXT("the virtual-bone refusal names its own verb"),
                Diagnostic->Message.Contains(TEXT("skeleton.create_virtual_bone")));
        }
        else
        {
            AddError(TEXT("No PWSRC_UNEXPECTED_TOKEN was raised for a virtual_bone declaration."));
        }
    }

    // The accepting direction for the steer table itself: an ordinary unknown construct must
    // still get the generic message plus its spelling suggestion. Without this, the steer could
    // swallow every unknown keyword and the suggestion machinery would go silently unused.
    {
        FPwSkelDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        FPwSkelParser::Parse(
            TEXTVIEW("pwskel 0\n")
            TEXT("bne \"root\" { }\n"),
            Document, Diagnostics);

        if (const FPwDiagnostic* Diagnostic = PwSkelParserTest_FindCode(Diagnostics,
                PwSourceDiagnosticCodes::PWSRC_UNEXPECTED_TOKEN))
        {
            TestFalse(TEXT("a plain typo is not steered to a socket verb"),
                Diagnostic->Message.Contains(TEXT("skeleton.create_socket")));
            TestTrue(TEXT("it still gets the generic unknown-construct message"),
                Diagnostic->Message.Contains(TEXT("Unknown .pwskel construct")));
        }
        else
        {
            AddError(TEXT("No PWSRC_UNEXPECTED_TOKEN was raised for an unknown construct."));
        }
    }

    return true;
}
