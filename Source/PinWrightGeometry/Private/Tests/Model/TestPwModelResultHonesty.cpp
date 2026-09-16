// Copyright (c) 2026 Alexander Penkin. MIT License.

// Three places the .pwmodel result reported something it did not know, and one it never reported
// at all. Each is a failure of HONESTY rather than of geometry, which is why they share a file:
// the compile is correct in every one of these cases and the response is not.
//
//  - THE ASSET CLASS ON A FAILED PARSE. `assetClass` and `skeletal` were written off a bool that
//    defaults to false, so every failed parse answered "UStaticMesh, skeletal: false" - including
//    for a source that declares `use skeleton from`, which the recovering parser usually still
//    has in hand. A caller cannot tell that answer from a real one. These pin the three cases
//    apart: a recovered declaration is the answer whatever else failed, a complete parse with no
//    declaration IS the static answer, and an incomplete parse with neither knows nothing and
//    must say so by omission.
//
//  - UNBOUND SLOTS. Both asset creators have always computed which slots ended up on the default
//    surface material; the model pipeline copied ClearedFeatures beside it and dropped this. A
//    section shipped grey with `success: true` and a slot count that looked exactly right, and
//    no field in the response moved. The validate-path test is the other half: an empty array
//    there would assert that every slot is bound, on a run that never resolved one.
//
//  - PARTS THAT INTERPENETRATE. The op-level overlap check is scoped to one mesh by RunOps, so
//    parts were never compared with each other at all. That is not an edge case in this format:
//    `harmonic_deform` carries one center and phase, so deforming pieces differently forces one
//    part per piece, and the check went blind exactly where an author had most reason to want
//    it. The count assertions are the load-bearing ones - one diagnostic per MODEL, not per
//    pair, because interpenetrating parts are how an organic model is built here and a warning
//    that fires dozens of times on correct work trains the author to ignore the code.
#include "Misc/AutomationTest.h"

#include "Model/PwModelAst.h"
#include "Model/PwModelCompiler.h"
#include "Model/PwModelDiagnostic.h"
#include "Tests/TestUtils.h"

#include "EditorAssetLibrary.h"

namespace
{
// Prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper with a
// common name would collide with a sibling test TU once Unity merges them.
const TCHAR* const PwModelReportTest_OutputRoot = TEXT("/Game/PinWrightTests/PwModelReport");

FPwModelCompileResult PwModelReportTest_Validate(const TCHAR* Source)
{
    FPwModelCompileOptions Options;
    Options.bValidateOnly = true;
    Options.bSave = false;
    return FPwModelCompiler::Compile(Source, Options);
}

FPwModelCompileResult PwModelReportTest_CompileTo(const TCHAR* Source, const FString& AssetPath)
{
    FPwModelCompileOptions Options;
    Options.OutputAssetPath = AssetPath;
    Options.SourcePath = TEXT("Tests/PwModelReport.pwmodel");
    Options.bOverwrite = true;
    Options.bSave = true;
    return FPwModelCompiler::Compile(Source, Options);
}

FString PwModelReportTest_Describe(const FPwModelCompileResult& Result)
{
    return FString::Printf(
        TEXT("success=%d classKnown=%d skeletal=%d slots=%d unbound=%d asset='%s' diagnostics=[%s]"),
        Result.bSuccess ? 1 : 0, Result.bAssetClassKnown ? 1 : 0, Result.bSkeletal ? 1 : 0,
        Result.MaterialSlots, Result.UnboundSlots.Num(), *Result.AssetPath,
        *JoinPwDiagnostics(Result.Diagnostics));
}

int32 PwModelReportTest_CountCode(const TArray<FPwDiagnostic>& Diagnostics, const TCHAR* Code)
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

// CleanupTestAsset, never UEditorAssetLibrary::DeleteAsset: that routes through
// ObjectTools::ForceDeleteObjects, whose full-object-graph reference walk costs seconds per call
// against a live editor's ~300k UObjects. The discard renames the asset AND its package into
// /Transient, removes the registry entry and deletes the .uasset, which is everything the
// pre-compile callers need to leave AssetCreatePolicy::Resolve on its Create branch.
void PwModelReportTest_DeleteIfPresent(const FString& AssetPath)
{
    if (UEditorAssetLibrary::DoesAssetExist(AssetPath))
    {
        CleanupTestAsset(AssetPath);
    }
}
}

// ============================================================================
// What class a failed run was going to produce
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelReportSkeletalIntentSurvivesAParseFailureTest,
    "PinWright.Model.Report.AFailedParseStillReportsADeclaredSkeleton",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelReportSkeletalIntentSurvivesAParseFailureTest::RunTest(const FString& Parameters)
{
    // The declaration is on line 2 and the mistake is on line 4, which is the ordinary case: the
    // parser recovers rather than aborting, so the source's own answer is right there. Reporting
    // "UStaticMesh" for this is not a missing feature, it is a wrong statement about a document
    // the run had already read.
    const FPwModelCompileResult Result = PwModelReportTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("use skeleton from \"/Game/PinWrightTests/SK_NotThere\"\n")
        TEXT("part a {\n")
        TEXT("    box size=(10, 10, 10\n")
        TEXT("}\n"));

    TestFalse(*FString::Printf(TEXT("the run failed. %s"), *PwModelReportTest_Describe(Result)),
        Result.bSuccess);
    TestTrue(*FString::Printf(TEXT("the class is known, because the source declares it. %s"),
        *PwModelReportTest_Describe(Result)), Result.bAssetClassKnown);
    TestTrue(*FString::Printf(TEXT("and the class is the skeletal one. %s"),
        *PwModelReportTest_Describe(Result)), Result.bSkeletal);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelReportUnknownClassIsOmittedTest,
    "PinWright.Model.Report.AFailedParseWithNoDeclarationClaimsNoClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelReportUnknownClassIsOmittedTest::RunTest(const FString& Parameters)
{
    // The same broken document without the declaration. Nothing here says static: the parse may
    // simply have died before reaching a `use` line, and a document is not a static model merely
    // because the reader never got far enough to find out. Unknown is the honest answer, and the
    // RPC omits both fields on it.
    const FPwModelCompileResult Result = PwModelReportTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part a {\n")
        TEXT("    box size=(10, 10, 10\n")
        TEXT("}\n"));

    TestFalse(*FString::Printf(TEXT("the run failed. %s"), *PwModelReportTest_Describe(Result)),
        Result.bSuccess);
    TestFalse(*FString::Printf(TEXT("the class is NOT claimed. %s"), *PwModelReportTest_Describe(Result)),
        Result.bAssetClassKnown);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelReportCompleteParseKnowsItsClassTest,
    "PinWright.Model.Report.ACompleteParseWithNoSkeletonIsTheStaticAnswer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelReportCompleteParseKnowsItsClassTest::RunTest(const FString& Parameters)
{
    // The other side of the rule, and the reason "unknown" cannot simply mean "did not succeed":
    // a document that parsed completely has been read to the end, so the absence of a skeleton
    // declaration IS the static answer - and stays the answer even if a later stage fails,
    // because nothing after the parse can turn a static document into a skeletal one.
    const FPwModelCompileResult Clean = PwModelReportTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part a { box size=(10, 10, 10) }\n"));

    TestTrue(*FString::Printf(TEXT("the run succeeded. %s"), *PwModelReportTest_Describe(Clean)),
        Clean.bSuccess);
    TestTrue(*FString::Printf(TEXT("the class is known. %s"), *PwModelReportTest_Describe(Clean)),
        Clean.bAssetClassKnown);
    TestFalse(*FString::Printf(TEXT("and it is the static one. %s"), *PwModelReportTest_Describe(Clean)),
        Clean.bSkeletal);

    // Parses completely, then fails in the compiler on an op the parser cannot judge. The class
    // was decided at the parse and must not become unknown again.
    const FPwModelCompileResult LateFailure = PwModelReportTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part a {\n")
        TEXT("    box size=(10, 10, 10)\n")
        TEXT("    subtract { box size=(4, 4, 4) at=(500, 0, 0) }\n")
        TEXT("}\n"));

    TestFalse(*FString::Printf(TEXT("the disjoint boolean failed the run. %s"),
        *PwModelReportTest_Describe(LateFailure)), LateFailure.bSuccess);
    TestTrue(*FString::Printf(TEXT("a stage failure does not un-know the class. %s"),
        *PwModelReportTest_Describe(LateFailure)), LateFailure.bAssetClassKnown);

    return true;
}

// ============================================================================
// Slots that shipped on the default material
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelReportUnboundSlotsReachTheResultTest,
    "PinWright.Model.Report.SlotsLeftOnTheDefaultMaterialAreNamed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwModelReportUnboundSlotsReachTheResultTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = FString::Printf(TEXT("%s/UnboundSlots"), PwModelReportTest_OutputRoot);
    PwModelReportTest_DeleteIfPresent(AssetPath);

    // Two slots the geometry uses. One is bound to nothing at all; the other is bound to a path
    // that does not resolve. Both end up on the default surface material, and the second is the
    // one no other field can see - it is bound in the SOURCE and unbound in the MESH, so the slot
    // table reports a path while the asset renders grey.
    const FPwModelCompileResult Result = PwModelReportTest_CompileTo(
        TEXT("pwmodel 0\n")
        TEXT("materials {\n")
        TEXT("    Bound = \"/Game/PinWrightTests/M_NotThere\"\n")
        TEXT("}\n")
        TEXT("part a {\n")
        TEXT("    box size=(20, 20, 20) material=\"Bound\"\n")
        TEXT("    box size=(20, 20, 20) at=(60, 0, 0) material=\"Loose\"\n")
        TEXT("}\n"),
        AssetPath);

    TestTrue(*FString::Printf(TEXT("the compile succeeded - an unloadable binding is not fatal. %s"),
        *PwModelReportTest_Describe(Result)), Result.bSuccess);
    TestEqual(*FString::Printf(TEXT("both slots exist. %s"), *PwModelReportTest_Describe(Result)),
        Result.MaterialSlots, 2);

    TestTrue(*FString::Printf(TEXT("the slot nothing bound is named. %s"), *PwModelReportTest_Describe(Result)),
        Result.UnboundSlots.Contains(TEXT("Loose")));
    TestTrue(*FString::Printf(TEXT("the slot whose binding would not load is named too. %s"),
        *PwModelReportTest_Describe(Result)), Result.UnboundSlots.Contains(TEXT("Bound")));

    PwModelReportTest_DeleteIfPresent(AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelReportValidateMeasuresNoSlotBindingsTest,
    "PinWright.Model.Report.ValidateMeasuresNoSlotBindingsAndSaysSoByOmission",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelReportValidateMeasuresNoSlotBindingsTest::RunTest(const FString& Parameters)
{
    // The same document, validated. No creator runs, so nothing resolved a binding - and an empty
    // list here would be a claim that every slot is bound, which is the same false confidence the
    // field exists to remove. It stays empty AND the run writes no asset path, which is what the
    // RPC keys its omission on.
    const FPwModelCompileResult Result = PwModelReportTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part a {\n")
        TEXT("    box size=(20, 20, 20) material=\"Loose\"\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("validate succeeded. %s"), *PwModelReportTest_Describe(Result)),
        Result.bSuccess);
    TestEqual(*FString::Printf(TEXT("the slot exists. %s"), *PwModelReportTest_Describe(Result)),
        Result.MaterialSlots, 1);
    TestEqual(*FString::Printf(TEXT("nothing was measured, so nothing is reported. %s"),
        *PwModelReportTest_Describe(Result)), Result.UnboundSlots.Num(), 0);
    TestTrue(*FString::Printf(TEXT("and no asset path, which is what makes the omission detectable. %s"),
        *PwModelReportTest_Describe(Result)), Result.AssetPath.IsEmpty());

    return true;
}

// ============================================================================
// Parts that interpenetrate
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelReportInterpenetratingPartsAreSeenTest,
    "PinWright.Model.Report.PartsThatInterpenetrateAreReportedOncePerModel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelReportInterpenetratingPartsAreSeenTest::RunTest(const FString& Parameters)
{
    // Two solids, one per part, overlapping by 20 uu on every axis. The op-level check cannot see
    // this at all: its footprint list is scoped to one mesh, so each part is compared against an
    // empty list and reports nothing.
    const FPwModelCompileResult Pair = PwModelReportTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part left  { box size=(40, 40, 40) }\n")
        TEXT("part right { box size=(40, 40, 40) at=(20, 0, 0) }\n"));

    TestTrue(*FString::Printf(TEXT("a warning does not fail the compile. %s"),
        *PwModelReportTest_Describe(Pair)), Pair.bSuccess);
    TestEqual(*FString::Printf(TEXT("the pair is reported exactly once. %s"),
        *PwModelReportTest_Describe(Pair)),
        PwModelReportTest_CountCode(Pair.Diagnostics,
            PwModelDiagnosticCodes::PWMODEL_UNUNIONED_OVERLAP_PARTS), 1);

    // And it does NOT borrow the op-level code, which would fold it into whatever op-level group
    // the same part already had and hide the fact behind that group's first message.
    TestEqual(*FString::Printf(TEXT("the op-level code is not reused across parts. %s"),
        *PwModelReportTest_Describe(Pair)),
        PwModelReportTest_CountCode(Pair.Diagnostics,
            PwModelDiagnosticCodes::PWMODEL_UNUNIONED_OVERLAP), 0);

    // Five mutually overlapping parts are ten pairs. Still one diagnostic: this format builds an
    // organic model as a shape per lobe, so a per-pair warning runs to dozens on work that is
    // entirely correct, and a code that fires dozens of times is one an author learns to skip.
    const FPwModelCompileResult Many = PwModelReportTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part a { sphere radius=30 }\n")
        TEXT("part b { sphere radius=30 at=(15, 0, 0) }\n")
        TEXT("part c { sphere radius=30 at=(0, 15, 0) }\n")
        TEXT("part d { sphere radius=30 at=(0, 0, 15) }\n")
        TEXT("part e { sphere radius=30 at=(10, 10, 10) }\n"));

    TestTrue(*FString::Printf(TEXT("the many-part model compiles. %s"), *PwModelReportTest_Describe(Many)),
        Many.bSuccess);
    TestEqual(*FString::Printf(TEXT("ten overlapping pairs are still one diagnostic. %s"),
        *PwModelReportTest_Describe(Many)),
        PwModelReportTest_CountCode(Many.Diagnostics,
            PwModelDiagnosticCodes::PWMODEL_UNUNIONED_OVERLAP_PARTS), 1);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelReportSeparatePartsAreQuietTest,
    "PinWright.Model.Report.PartsThatOnlyTouchOrMissAreNotReported",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelReportSeparatePartsAreQuietTest::RunTest(const FString& Parameters)
{
    // Disjoint. The baseline: if this ever warns, the diagnostic is worthless.
    const FPwModelCompileResult Apart = PwModelReportTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part left  { box size=(40, 40, 40) }\n")
        TEXT("part right { box size=(40, 40, 40) at=(200, 0, 0) }\n"));

    TestEqual(*FString::Printf(TEXT("separated parts raise nothing. %s"),
        *PwModelReportTest_Describe(Apart)),
        PwModelReportTest_CountCode(Apart.Diagnostics,
            PwModelDiagnosticCodes::PWMODEL_UNUNIONED_OVERLAP_PARTS), 0);

    // FLUSH-ADJACENT, sharing a face exactly. A column on a base, a floor meeting a wall: the
    // single most common correct arrangement of two parts there is, and a zero-thickness
    // intersection. The positive-thickness rule exists for this, and without it the warning
    // would fire on almost every correct multi-part model.
    const FPwModelCompileResult Flush = PwModelReportTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part base   { box size=(40, 40, 40) }\n")
        TEXT("part column { box size=(40, 40, 40) at=(40, 0, 0) }\n"));

    TestTrue(*FString::Printf(TEXT("the flush model compiles. %s"), *PwModelReportTest_Describe(Flush)),
        Flush.bSuccess);
    TestEqual(*FString::Printf(TEXT("touching is not overlapping. %s"), *PwModelReportTest_Describe(Flush)),
        PwModelReportTest_CountCode(Flush.Diagnostics,
            PwModelDiagnosticCodes::PWMODEL_UNUNIONED_OVERLAP_PARTS), 0);

    // An OPEN part bounds no interior, so "these interpenetrate" says nothing about buried faces
    // and there is no shared volume to bury them in. Same exclusion the op-level check makes, for
    // the same reason - without it a flat card crossing a solid warns about a condition that does
    // not apply to it.
    const FPwModelCompileResult Open = PwModelReportTest_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part solid { box size=(40, 40, 40) }\n")
        TEXT("part card  { plane size=(80, 80) }\n"));

    TestTrue(*FString::Printf(TEXT("the open model compiles. %s"), *PwModelReportTest_Describe(Open)),
        Open.bSuccess);
    TestEqual(*FString::Printf(TEXT("an open part is not compared. %s"), *PwModelReportTest_Describe(Open)),
        PwModelReportTest_CountCode(Open.Diagnostics,
            PwModelDiagnosticCodes::PWMODEL_UNUNIONED_OVERLAP_PARTS), 0);

    return true;
}
