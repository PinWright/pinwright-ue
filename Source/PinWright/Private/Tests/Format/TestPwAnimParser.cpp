// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "PwAnim/PwAnimAst.h"
#include "PwAnim/PwAnimDiagnostic.h"
#include "PwAnim/PwAnimParser.h"

namespace
{
    bool PwAnimParserTest_HasCode(const TArray<FPwDiagnostic>& Diagnostics, const TCHAR* Code)
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

    const FPwDiagnostic* PwAnimParserTest_FindCode(
        const TArray<FPwDiagnostic>& Diagnostics, const TCHAR* Code)
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

    void PwAnimParserTest_ExpectCode(FAutomationTestBase& Test,
                                     const TArray<FPwDiagnostic>& Diagnostics,
                                     const TCHAR* Code)
    {
        Test.TestTrue(*FString::Printf(TEXT("%s was reported. Diagnostics: [%s]"), Code,
            *JoinPwDiagnostics(Diagnostics)), PwAnimParserTest_HasCode(Diagnostics, Code));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAnimParserGrammarTest,
    "PinWright.Animation.Parser.ParsesStandaloneGrammar",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwAnimParserGrammarTest::RunTest(const FString& Parameters)
{
    FPwAnimDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bParsed = FPwAnimParser::Parse(
        TEXTVIEW("pwanim 0\n")
        TEXT("use skeleton from \"/Game/Chars/SK_Hero\"\n")
        TEXT("timebase rate=(30, 1) frames=48 loop=true\n")
        TEXT("sync_marker \"L\" frame=0\n")
        TEXT("sync_marker \"R\" frame=24\n")
        TEXT("sync_marker \"L\" frame=48\n")
        TEXT("bone \"pelvis\" ease=ease_out {\n")
        TEXT("    key frame=0 at=(0, 0, 0) ease=ease_out\n")
        TEXT("    key frame=24 at=(0, 0, 24)\n")
        TEXT("    key frame=48 at=(0, 0, 0)\n")
        TEXT("}\n")
        TEXT("bone \"spine 01\" {\n")
        TEXT("    key frame=0 rotate=(0, 0, 0)\n")
        TEXT("    key frame=48 rotate=(0, -6, 0)\n")
        TEXT("}\n"),
        Document, Diagnostics);

    TestTrue(*FString::Printf(TEXT("standalone .pwanim grammar parses. [%s]"),
        *JoinPwDiagnostics(Diagnostics)), bParsed);
    TestEqual(TEXT("no diagnostics"), Diagnostics.Num(), 0);
    TestEqual(TEXT("format keyword"), Document.Header.FormatKeyword, FString(TEXT("pwanim")));
    TestEqual(TEXT("version"), Document.Header.Version, 0);
    TestEqual(TEXT("one skeleton use"), Document.Uses.Num(), 1);
    TestEqual(TEXT("asset path is preserved exactly"), Document.Uses[0].Path,
        FString(TEXT("/Game/Chars/SK_Hero")));
    TestTrue(TEXT("timebase is present"), Document.Timebase.IsSet());
    TestTrue(TEXT("sync marker statements are present"), Document.SyncMarkers.IsSet());
    if (Document.SyncMarkers.IsSet())
    {
        TestEqual(TEXT("three sync marker statements are retained"),
            Document.SyncMarkers->Num(), 3);
        if (Document.SyncMarkers->Num() == 3)
        {
            TestEqual(TEXT("first marker name"), (*Document.SyncMarkers)[0].MarkerName,
                FString(TEXT("L")));
            TestEqual(TEXT("second marker frame"), (*Document.SyncMarkers)[1].Frame, 24);
            TestEqual(TEXT("duplicate marker name is legal"), (*Document.SyncMarkers)[2].MarkerName,
                FString(TEXT("L")));
        }
    }
    TestEqual(TEXT("two bone tracks"), Document.Bones.Num(), 2);

    if (Document.Timebase.IsSet())
    {
        const FPwOp& Timebase = Document.Timebase.GetValue();
        const FPwValue* Rate = Timebase.Params.Find(TEXT("rate"));
        TestNotNull(TEXT("timebase rate is stored"), Rate);
        if (Rate)
        {
            TestEqual(TEXT("rate is a tuple"), Rate->Type, EPwValueType::Tuple);
            TestEqual(TEXT("rate has numerator and denominator"), Rate->Tuple.Num(), 2);
            TestEqual(TEXT("rate numerator"), Rate->Tuple[0], 30.0);
            TestEqual(TEXT("rate denominator"), Rate->Tuple[1], 1.0);
        }
        const FPwValue* Frames = Timebase.Params.Find(TEXT("frames"));
        TestNotNull(TEXT("timebase frame count is stored"), Frames);
        if (Frames)
        {
            TestEqual(TEXT("frame count"), Frames->Number, 48.0);
        }
    }

    if (Document.Bones.Num() == 2)
    {
        TestEqual(TEXT("first bone keeps string name"), Document.Bones[0].BoneName,
            FString(TEXT("pelvis")));
        TestEqual(TEXT("first bone keeps three keys"), Document.Bones[0].Keys.Num(), 3);
        TestEqual(TEXT("second bone keeps spaces in its string name"), Document.Bones[1].BoneName,
            FString(TEXT("spine 01")));
        TestEqual(TEXT("second bone keeps two keys"), Document.Bones[1].Keys.Num(), 2);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAnimParserTimeIsIntegerFramesTest,
    "PinWright.Animation.Parser.TimeIsIntegerFramesOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwAnimParserTimeIsIntegerFramesTest::RunTest(const FString& Parameters)
{
    {
        FPwAnimDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        const bool bParsed = FPwAnimParser::Parse(
            TEXTVIEW("pwanim 0\n")
            TEXT("use skeleton from \"/Game/Chars/SK_Hero\"\n")
            TEXT("timebase rate=(30, 1) frames=48\n")
            TEXT("bone \"root\" { key frame=1.6 at=(0, 0, 0) }\n"),
            Document, Diagnostics);

        TestFalse(TEXT("fractional frame is rejected"), bParsed);
        PwAnimParserTest_ExpectCode(*this, Diagnostics,
            PwSourceDiagnosticCodes::PWSRC_BAD_VALUE);
        if (const FPwDiagnostic* Diagnostic = PwAnimParserTest_FindCode(Diagnostics,
                PwSourceDiagnosticCodes::PWSRC_BAD_VALUE))
        {
            TestEqual(TEXT("fractional frame is reported on its source line"), Diagnostic->Line, 4);
        }
    }

    {
        FPwAnimDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        const bool bParsed = FPwAnimParser::Parse(
            TEXTVIEW("pwanim 0\n")
            TEXT("use skeleton from \"/Game/Chars/SK_Hero\"\n")
            TEXT("timebase rate=(30, 1) frames=48\n")
            TEXT("bone \"root\" { key time=1.6 at=(0, 0, 0) }\n"),
            Document, Diagnostics);

        TestFalse(TEXT("seconds are not a key parameter"), bParsed);
        PwAnimParserTest_ExpectCode(*this, Diagnostics,
            PwSourceDiagnosticCodes::PWSRC_UNKNOWN_PARAM);
        if (const FPwDiagnostic* Diagnostic = PwAnimParserTest_FindCode(Diagnostics,
                PwSourceDiagnosticCodes::PWSRC_UNKNOWN_PARAM))
        {
            TestTrue(TEXT("unknown time parameter suggests frame"),
                Diagnostic->Suggestions.Contains(TEXT("frame")));
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAnimParserStructuralDiagnosticsTest,
    "PinWright.Animation.Parser.ReportsFormatStructureErrors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwAnimParserStructuralDiagnosticsTest::RunTest(const FString& Parameters)
{
    {
        FPwAnimDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        const bool bParsed = FPwAnimParser::Parse(TEXTVIEW("pwanim 0\n"), Document, Diagnostics);
        TestFalse(TEXT("an empty animation does not parse"), bParsed);
        PwAnimParserTest_ExpectCode(*this, Diagnostics,
            PwAnimDiagnosticCodes::PWANIM_MISSING_TIMEBASE);
        PwAnimParserTest_ExpectCode(*this, Diagnostics,
            PwAnimDiagnosticCodes::PWANIM_MISSING_SKELETON);
        PwAnimParserTest_ExpectCode(*this, Diagnostics,
            PwAnimDiagnosticCodes::PWANIM_NO_BONES);
    }

    {
        FPwAnimDocument Document;
        TArray<FPwDiagnostic> Diagnostics;
        const bool bParsed = FPwAnimParser::Parse(
            TEXTVIEW("pwanim 0\n")
            TEXT("use skeleton from \"/Game/Chars/SK_Hero\"\n")
            TEXT("use skeleton from \"/Game/Chars/SK_Other\"\n")
            TEXT("timebase rate=(30, 1) frames=4\n")
            TEXT("timebase rate=(30, 1) frames=4\n")
            TEXT("bone \"root\" {\n")
            TEXT("    key frame=2 at=(0, 0, 0)\n")
            TEXT("    key frame=1 at=(0, 0, 0)\n")
            TEXT("    key frame=1 at=(0, 0, 0) ease=step\n")
            TEXT("}\n")
            TEXT("bone \"root\" { }\n"),
            Document, Diagnostics);

        TestFalse(TEXT("duplicate and unordered declarations do not parse"), bParsed);
        PwAnimParserTest_ExpectCode(*this, Diagnostics,
            PwAnimDiagnosticCodes::PWANIM_DUPLICATE_SKELETON);
        PwAnimParserTest_ExpectCode(*this, Diagnostics,
            PwAnimDiagnosticCodes::PWANIM_DUPLICATE_TIMEBASE);
        PwAnimParserTest_ExpectCode(*this, Diagnostics,
            PwAnimDiagnosticCodes::PWANIM_KEYS_OUT_OF_ORDER);
        PwAnimParserTest_ExpectCode(*this, Diagnostics,
            PwAnimDiagnosticCodes::PWANIM_DUPLICATE_KEY);
        PwAnimParserTest_ExpectCode(*this, Diagnostics,
            PwAnimDiagnosticCodes::PWANIM_DUPLICATE_BONE);
        PwAnimParserTest_ExpectCode(*this, Diagnostics,
            PwAnimDiagnosticCodes::PWANIM_EMPTY_BONE);
        PwAnimParserTest_ExpectCode(*this, Diagnostics,
            PwAnimDiagnosticCodes::PWANIM_TRAILING_EASE);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAnimParserSyncMarkerRangeTest,
    "PinWright.Animation.Parser.SyncMarkersRequireInclusiveFrameRange",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwAnimParserSyncMarkerRangeTest::RunTest(const FString& Parameters)
{
    FPwAnimDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bParsed = FPwAnimParser::Parse(
        TEXTVIEW("pwanim 0\n")
        TEXT("use skeleton from \"/Game/Chars/SK_Hero\"\n")
        TEXT("timebase rate=(30, 1) frames=4\n")
        TEXT("sync_marker \"Late\" frame=5\n")
        TEXT("bone \"root\" { key frame=0 at=(0, 0, 0) }\n"),
        Document, Diagnostics);

    TestFalse(TEXT("marker past the final frame is rejected"), bParsed);
    const FPwDiagnostic* BadValue = PwAnimParserTest_FindCode(
        Diagnostics, PwSourceDiagnosticCodes::PWSRC_BAD_VALUE);
    TestNotNull(TEXT("out-of-range marker uses PWSRC_BAD_VALUE"), BadValue);
    if (BadValue)
    {
        TestEqual(TEXT("out-of-range marker diagnostic points to its statement"), BadValue->Line, 4);
        TestTrue(TEXT("out-of-range marker message names the inclusive limit"),
            BadValue->Message.Contains(TEXT("0 through 4")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAnimParserWrongFormatTest,
    "PinWright.Animation.Parser.WrongFormatFailsOnLineOne",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwAnimParserWrongFormatTest::RunTest(const FString& Parameters)
{
    FPwAnimDocument Document;
    TArray<FPwDiagnostic> Diagnostics;
    const bool bParsed = FPwAnimParser::Parse(
        TEXTVIEW("pwmodel 0\n")
        TEXT("part \"body\" { sphere radius=1 }\n"),
        Document, Diagnostics);

    TestFalse(TEXT("a pwmodel document is rejected by the animation parser"), bParsed);
    TestEqual(TEXT("wrong format is one diagnostic, not a cascade"), Diagnostics.Num(), 1);
    PwAnimParserTest_ExpectCode(*this, Diagnostics,
        PwAnimDiagnosticCodes::PWANIM_WRONG_FORMAT);
    if (Diagnostics.Num() == 1)
    {
        TestEqual(TEXT("wrong format is anchored on line one"), Diagnostics[0].Line, 1);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAnimOpTableContractTest,
    "PinWright.Animation.Parser.OpTablePublishesGrammar",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwAnimOpTableContractTest::RunTest(const FString& Parameters)
{
    const TArray<FPwAnimOpSpec>& Specs = PwAnimOpTable::Get();
    TestEqual(TEXT("the animation statement vocabulary has one op"), Specs.Num(), 1);
    if (Specs.Num() == 1)
    {
        TestEqual(TEXT("the published op is key"), Specs[0].Name, FString(TEXT("key")));
        TestFalse(TEXT("key does not accept a block"), Specs[0].bAcceptsBlock);
        TestNotNull(TEXT("key publishes its frame parameter"), Specs[0].FindParam(TEXT("frame")));
        if (const FPwParamSpec* Frame = Specs[0].FindParam(TEXT("frame")))
        {
            TestTrue(TEXT("frame is required"), Frame->bRequired);
            TestEqual(TEXT("frame is an integer"), Frame->Type, EPwParamType::Integer);
        }
    }

    const TArrayView<const FPwParamSpec> Timebase = PwAnimOpTable::TimebaseParams();
    TestTrue(TEXT("timebase publishes rate"), Timebase.FindByPredicate(
        [](const FPwParamSpec& Spec) { return Spec.Name == TEXT("rate"); }) != nullptr);
    TestTrue(TEXT("timebase publishes frames"), Timebase.FindByPredicate(
        [](const FPwParamSpec& Spec) { return Spec.Name == TEXT("frames"); }) != nullptr);
    TestTrue(TEXT("timebase publishes loop"), Timebase.FindByPredicate(
        [](const FPwParamSpec& Spec) { return Spec.Name == TEXT("loop"); }) != nullptr);

    const TArrayView<const FPwParamSpec> SyncMarker = PwAnimOpTable::SyncMarkerParams();
    TestEqual(TEXT("sync_marker publishes one parameter"), SyncMarker.Num(), 1);
    if (SyncMarker.Num() == 1)
    {
        TestEqual(TEXT("sync_marker parameter is frame"), SyncMarker[0].Name,
            FString(TEXT("frame")));
        TestTrue(TEXT("sync_marker frame is required"), SyncMarker[0].bRequired);
        TestEqual(TEXT("sync_marker frame is an integer"), SyncMarker[0].Type,
            EPwParamType::Integer);
    }

    TestTrue(TEXT("sync_marker is published as a top-level construct"),
        PwAnimOpTable::TopLevelNames().Contains(TEXT("sync_marker")));
    return true;
}
