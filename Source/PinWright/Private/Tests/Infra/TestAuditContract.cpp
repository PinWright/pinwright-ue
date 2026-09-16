// Copyright (c) 2026 Alexander Penkin. MIT License.

// The rule net for Audit/AuditFramework.h - the ONE definition of what an audit verb's `pass`
// means, now that level.audit, geometry.audit_static_meshes, landscape.audit_shape and
// skeleton.audit_skin_weights all derive their verdict through it.
//
// These assertions are on the rule itself rather than on any verb's output. Each verb keeps its
// own tests for what it measures; what is under test here is the thing all four would otherwise
// have kept a private copy of, and the property that copy exists to hold:
//
//     an audit that could not measure something never reports pass:true.
//
// No world, no asset, no engine subsystem: FVerdict is a plain aggregate on purpose, so the
// rule is drivable from a headless test and a regression in it cannot hide behind a fixture.
#include "Misc/AutomationTest.h"

#include "Audit/AuditFramework.h"
#include "Handlers/Environment/LandscapeShapeMetrics.h"
#include "Handlers/Level/LevelAuditUtils.h"

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper
// with a common name would collide with a sibling test TU once Unity merges them.

// Every failOn the wire accepts, so a rule that must hold "under any failOn" is asserted under
// all of them rather than under the one the author happened to think of.
const TArray<PinWrightAudit::EFailOn>& AuditContractTest_AllFailOns()
{
    static const TArray<PinWrightAudit::EFailOn> All = {
        PinWrightAudit::EFailOn::Error,
        PinWrightAudit::EFailOn::Any,
        PinWrightAudit::EFailOn::None,
    };
    return All;
}

// A stand-in check table with exactly the shape every real one has. Using a synthetic table
// keeps these assertions about the shared helpers rather than about any verb's check list,
// which is free to grow without touching this file.
enum class EAuditContractTestCheck : uint8 { First = 0, Second, Third, Count };

struct FAuditContractTestInfo
{
    EAuditContractTestCheck Check;
    const TCHAR* Id;
    bool bDefaultOn;
    bool bNeedsExtra;
};

const TArray<FAuditContractTestInfo>& AuditContractTest_Table()
{
    static const TArray<FAuditContractTestInfo> Table = {
        {EAuditContractTestCheck::First,  TEXT("first"),  true,  false},
        {EAuditContractTestCheck::Second, TEXT("second"), false, true},
        {EAuditContractTestCheck::Third,  TEXT("third"),  true,  true},
    };
    return Table;
}
}

// ---------------------------------------------------------------------------------------------
// THE assertion. An unrunnable check makes pass false under every failOn the wire accepts,
// including "none" - which is the one a caller reaches for when they want the sweep to stop
// complaining, and the one under which "I measured nothing" would otherwise read as clean.
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuditContractUnrunnableNeverPassesTest,
    "PinWright.infra.audit_contract.UnrunnableNeverYieldsPassUnderAnyFailOn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAuditContractUnrunnableNeverPassesTest::RunTest(const FString& Parameters)
{
    // Nothing flagged at all: the ONLY thing wrong is that a check did not run.
    PinWrightAudit::FVerdict Verdict;
    Verdict.UnrunnableCount = 1;

    for (const PinWrightAudit::EFailOn FailOn : AuditContractTest_AllFailOns())
    {
        TestFalse(*FString::Printf(TEXT("one unrunnable check cannot pass under failOn '%s'"),
                                   PinWrightAudit::FailOnToWire(FailOn)),
            Verdict.DerivePass(FailOn));
    }

    // The control: with that one term cleared and nothing else changed, it passes. Without
    // this, the test above would still hold on a DerivePass that returned false always.
    Verdict.UnrunnableCount = 0;
    for (const PinWrightAudit::EFailOn FailOn : AuditContractTest_AllFailOns())
    {
        TestTrue(*FString::Printf(TEXT("a clean measured sweep passes under failOn '%s'"),
                                  PinWrightAudit::FailOnToWire(FailOn)),
            Verdict.DerivePass(FailOn));
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// The same for truncation. Half a folder audited and reported clean as "the folder is clean" is
// how a sweep once exited 0 having examined nothing; failOn must not be able to buy that back.
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuditContractTruncationNeverPassesTest,
    "PinWright.infra.audit_contract.TruncationNeverYieldsPassUnderAnyFailOn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAuditContractTruncationNeverPassesTest::RunTest(const FString& Parameters)
{
    PinWrightAudit::FVerdict Verdict;
    Verdict.bTruncated = true;

    for (const PinWrightAudit::EFailOn FailOn : AuditContractTest_AllFailOns())
    {
        TestFalse(*FString::Printf(TEXT("a truncated sweep cannot pass under failOn '%s'"),
                                   PinWrightAudit::FailOnToWire(FailOn)),
            Verdict.DerivePass(FailOn));
    }

    // Both non-severity terms together, and then neither: the two are independent, so a rule
    // that ANDed them wrongly would still satisfy the single-term cases above.
    Verdict.UnrunnableCount = 3;
    TestFalse(TEXT("unrunnable and truncated together still cannot pass"),
        Verdict.DerivePass(PinWrightAudit::EFailOn::None));
    Verdict.UnrunnableCount = 0;
    Verdict.bTruncated = false;
    TestTrue(TEXT("with neither term set it passes"),
        Verdict.DerivePass(PinWrightAudit::EFailOn::None));
    return true;
}

// ---------------------------------------------------------------------------------------------
// What failOn IS allowed to move: the severity bar, and only that. Asserted as the full 3x3 of
// (failOn) x (errors, warnings, both clean), because the shape of the bug this guards against is
// one arm of a ternary quietly reading the wrong counter.
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuditContractFailOnMovesOnlySeverityTest,
    "PinWright.infra.audit_contract.FailOnMovesOnlyTheSeverityBar",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAuditContractFailOnMovesOnlySeverityTest::RunTest(const FString& Parameters)
{
    PinWrightAudit::FVerdict ErrorOnly;
    ErrorOnly.ErrorCount = 1;
    PinWrightAudit::FVerdict WarningOnly;
    WarningOnly.WarningCount = 1;
    const PinWrightAudit::FVerdict Clean;

    TestFalse(TEXT("failOn error fails on an error"), ErrorOnly.DerivePass(PinWrightAudit::EFailOn::Error));
    TestTrue(TEXT("failOn error tolerates a warning"), WarningOnly.DerivePass(PinWrightAudit::EFailOn::Error));
    TestTrue(TEXT("failOn error passes a clean sweep"), Clean.DerivePass(PinWrightAudit::EFailOn::Error));

    TestFalse(TEXT("failOn any fails on an error"), ErrorOnly.DerivePass(PinWrightAudit::EFailOn::Any));
    TestFalse(TEXT("failOn any fails on a warning"), WarningOnly.DerivePass(PinWrightAudit::EFailOn::Any));
    TestTrue(TEXT("failOn any passes a clean sweep"), Clean.DerivePass(PinWrightAudit::EFailOn::Any));

    TestTrue(TEXT("failOn none tolerates an error"), ErrorOnly.DerivePass(PinWrightAudit::EFailOn::None));
    TestTrue(TEXT("failOn none tolerates a warning"), WarningOnly.DerivePass(PinWrightAudit::EFailOn::None));
    TestTrue(TEXT("failOn none passes a clean sweep"), Clean.DerivePass(PinWrightAudit::EFailOn::None));

    // SeverityClean is the half failOn owns; DerivePass is that half AND the two terms it does
    // not. Asserting them apart is what pins WHERE the boundary is.
    PinWrightAudit::FVerdict Unmeasured;
    Unmeasured.UnrunnableCount = 1;
    TestTrue(TEXT("an unrunnable check leaves the severity half clean"),
        Unmeasured.SeverityClean(PinWrightAudit::EFailOn::Error));
    TestFalse(TEXT("and still fails the verdict"),
        Unmeasured.DerivePass(PinWrightAudit::EFailOn::Error));
    return true;
}

// ---------------------------------------------------------------------------------------------
// ParseCheckId's contract is its RETURN VALUE: false for an unknown id, so every caller can turn
// it into an error. A typo in `checks` that silently ran nothing is indistinguishable from a
// subject that passed every check - the same false green as the verdict above, one argument
// earlier.
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuditContractParseCheckIdTest,
    "PinWright.infra.audit_contract.UnknownCheckIdIsRejectedAndNotDefaulted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAuditContractParseCheckIdTest::RunTest(const FString& Parameters)
{
    const TArray<FAuditContractTestInfo>& Table = AuditContractTest_Table();

    EAuditContractTestCheck Parsed = EAuditContractTestCheck::Count;
    TestTrue(TEXT("a known id parses"), PinWrightAudit::ParseCheckId(Table, TEXT("second"), Parsed));
    TestTrue(TEXT("and yields that check"), Parsed == EAuditContractTestCheck::Second);

    TestTrue(TEXT("matching is case-insensitive"),
        PinWrightAudit::ParseCheckId(Table, TEXT("SeCoNd"), Parsed));
    TestTrue(TEXT("surrounding whitespace is trimmed"),
        PinWrightAudit::ParseCheckId(Table, TEXT("  second\t"), Parsed));

    // The load-bearing case. The out-param is deliberately pre-set to a REAL check here: a
    // ParseCheckId that returned false but still wrote something would let a caller who ignored
    // the return value run a check the user never asked for.
    Parsed = EAuditContractTestCheck::Third;
    TestFalse(TEXT("an unknown id is rejected"),
        PinWrightAudit::ParseCheckId(Table, TEXT("secnod"), Parsed));
    TestTrue(TEXT("and the out-param is untouched rather than defaulted"),
        Parsed == EAuditContractTestCheck::Third);

    TestFalse(TEXT("an empty id is rejected"), PinWrightAudit::ParseCheckId(Table, FString(), Parsed));
    TestFalse(TEXT("a prefix of a known id is rejected"),
        PinWrightAudit::ParseCheckId(Table, TEXT("sec"), Parsed));
    TestFalse(TEXT("a superstring of a known id is rejected"),
        PinWrightAudit::ParseCheckId(Table, TEXT("second_thoughts"), Parsed));

    // The rejection message names the valid set, derived from the table so a check added to the
    // table cannot go unmentioned in the error that rejects its typo.
    TestEqual(TEXT("the valid-id list is the table, in order"),
        PinWrightAudit::ValidCheckIdList(Table), FString(TEXT("first, second, third")));
    return true;
}

// ---------------------------------------------------------------------------------------------
// The masks are derived from the table's own columns, which is what lets a check join the
// default set by setting one flag instead of by being remembered in a second list.
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuditContractCheckMaskTest,
    "PinWright.infra.audit_contract.CheckMasksAreDerivedFromTheTable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAuditContractCheckMaskTest::RunTest(const FString& Parameters)
{
    const TArray<FAuditContractTestInfo>& Table = AuditContractTest_Table();

    const uint32 Default = PinWrightAudit::DefaultCheckMask(Table);
    TestTrue(TEXT("a bDefaultOn check is in the default mask"),
        PinWrightAudit::HasCheck(Default, EAuditContractTestCheck::First));
    TestFalse(TEXT("an opt-in check is not"),
        PinWrightAudit::HasCheck(Default, EAuditContractTestCheck::Second));
    TestTrue(TEXT("and the third is"),
        PinWrightAudit::HasCheck(Default, EAuditContractTestCheck::Third));

    const uint32 All = PinWrightAudit::AllCheckMask(Table);
    for (const FAuditContractTestInfo& Info : Table)
    {
        TestTrue(TEXT("every check is in the all-mask"), PinWrightAudit::HasCheck(All, Info.Check));
    }
    TestEqual(TEXT("the default mask is a subset of the all-mask"), Default & ~All, 0u);

    // MaskWhere over a per-audit column is how level.audit builds SurfaceCheckMask and
    // PlayAreaCheckMask, so the shape is asserted rather than only the two named masks.
    const uint32 Extra = PinWrightAudit::MaskWhere(Table,
        [](const FAuditContractTestInfo& Info) { return Info.bNeedsExtra; });
    TestFalse(TEXT("MaskWhere excludes a row failing the predicate"),
        PinWrightAudit::HasCheck(Extra, EAuditContractTestCheck::First));
    TestTrue(TEXT("and includes the two that pass it"),
        PinWrightAudit::HasCheck(Extra, EAuditContractTestCheck::Second)
        && PinWrightAudit::HasCheck(Extra, EAuditContractTestCheck::Third));

    TestTrue(TEXT("CheckInfo indexes by the enum value"),
        PinWrightAudit::CheckInfo(Table, EAuditContractTestCheck::Second).Check
            == EAuditContractTestCheck::Second);
    return true;
}

// ---------------------------------------------------------------------------------------------
// The wire spellings and the published passRule sentence. These are SHIPPED CONTRACT: a caller
// branches on "unrunnable" and a reader compares two audits' passRule fields, so a change here
// is a wire change and must fail a test rather than pass quietly through a refactor.
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuditContractWireVocabularyTest,
    "PinWright.infra.audit_contract.WireVocabularyAndPassRuleAreStable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAuditContractWireVocabularyTest::RunTest(const FString& Parameters)
{
    TestEqual(TEXT("severity error"),
        FString(PinWrightAudit::SeverityToWire(PinWrightAudit::ESeverity::Error)), FString(TEXT("error")));
    TestEqual(TEXT("severity warning"),
        FString(PinWrightAudit::SeverityToWire(PinWrightAudit::ESeverity::Warning)), FString(TEXT("warning")));
    TestEqual(TEXT("status flagged"),
        FString(PinWrightAudit::StatusToWire(PinWrightAudit::EFindingStatus::Flagged)), FString(TEXT("flagged")));
    TestEqual(TEXT("status unrunnable"),
        FString(PinWrightAudit::StatusToWire(PinWrightAudit::EFindingStatus::Unrunnable)), FString(TEXT("unrunnable")));

    PinWrightAudit::EFailOn Parsed = PinWrightAudit::EFailOn::Any;
    TestTrue(TEXT("failOn error parses"), PinWrightAudit::ParseFailOn(TEXT("error"), Parsed));
    TestTrue(TEXT("to Error"), Parsed == PinWrightAudit::EFailOn::Error);
    TestTrue(TEXT("failOn ANY parses case-insensitively"), PinWrightAudit::ParseFailOn(TEXT("ANY"), Parsed));
    TestTrue(TEXT("to Any"), Parsed == PinWrightAudit::EFailOn::Any);
    TestTrue(TEXT("failOn none parses"), PinWrightAudit::ParseFailOn(TEXT("none"), Parsed));
    TestTrue(TEXT("to None"), Parsed == PinWrightAudit::EFailOn::None);
    TestFalse(TEXT("an unknown failOn is rejected rather than defaulted"),
        PinWrightAudit::ParseFailOn(TEXT("warning"), Parsed));
    TestFalse(TEXT("and so is an empty one"), PinWrightAudit::ParseFailOn(FString(), Parsed));

    // The three sentences the four verbs publish, verbatim. level.audit takes the bare form;
    // geometry.audit_static_meshes adds its own elaboration; landscape.audit_shape drops the
    // truncation term because its region ceiling refuses rather than truncates.
    TestEqual(TEXT("level.audit's passRule"),
        PinWrightAudit::PassRuleText(true),
        FString(TEXT("pass = no finding at or above failOn, AND zero unrunnable checks, AND the sweep "
                     "was not truncated.")));
    TestEqual(TEXT("geometry.audit_static_meshes' passRule"),
        PinWrightAudit::PassRuleText(true,
            TEXT("An asset that could not be read is not an asset that passed, and a page "
                 "that stopped short of the match set has not audited the set.")),
        FString(TEXT("pass = no finding at or above failOn, AND zero unrunnable checks, AND the sweep "
                     "was not truncated. An asset that could not be read is not an asset that passed, "
                     "and a page that stopped short of the match set has not audited the set.")));
    TestEqual(TEXT("landscape.audit_shape's passRule"),
        PinWrightAudit::PassRuleText(false, TEXT("An unmeasurable region never reads as a pass.")),
        FString(TEXT("pass = no finding at or above failOn, AND zero unrunnable checks. An unmeasurable "
                     "region never reads as a pass.")));

    // Every form names the unrunnable term. A passRule that stopped saying so would be a rule
    // the caller could no longer hold the verb to.
    TestTrue(TEXT("the bare rule still names zero unrunnable checks"),
        PinWrightAudit::PassRuleText(false).Contains(TEXT("zero unrunnable checks")));
    return true;
}

// ---------------------------------------------------------------------------------------------
// The two main-module check tables, against the invariants the shared helpers assume: index i is
// the check whose enum value is i, ids are unique, and nothing is blank. A table that drifts from
// its enum makes CheckInfo return the WRONG check's severity and error code - a finding filed
// under another check's name, which reads as a real finding.
//
// geometry.audit_static_meshes' table lives in another module and is covered by
// PinWright.Geometry.MeshAudit.UnknownCheckIdIsRejectedAndTheTableIsWellFormed.
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuditContractShippedTablesTest,
    "PinWright.infra.audit_contract.ShippedCheckTablesMatchTheirEnums",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAuditContractShippedTablesTest::RunTest(const FString& Parameters)
{
    auto VerifyTable = [this](const TCHAR* Name, auto& Table, int32 ExpectedCount)
    {
        TestEqual(*FString::Printf(TEXT("%s: one row per check"), Name), Table.Num(), ExpectedCount);
        TSet<FString> SeenIds;
        for (int32 Index = 0; Index < Table.Num(); ++Index)
        {
            const auto& Info = Table[Index];
            TestEqual(*FString::Printf(TEXT("%s: row %d is the check with that enum value"), Name, Index),
                static_cast<int32>(Info.Check), Index);
            const FString Id(Info.Id);
            TestFalse(*FString::Printf(TEXT("%s: row %d has a wire id"), Name, Index), Id.IsEmpty());
            TestFalse(*FString::Printf(TEXT("%s: wire id '%s' is unique"), Name, *Id), SeenIds.Contains(Id));
            SeenIds.Add(Id);
            TestFalse(*FString::Printf(TEXT("%s: row %d carries an ERR_ code"), Name, Index),
                FString(Info.Code).IsEmpty());
            TestFalse(*FString::Printf(TEXT("%s: row %d carries a summary"), Name, Index),
                FString(Info.Summary).IsEmpty());
            // Every id in the table must round-trip through the parser that gates the wire.
            decltype(Info.Check) Parsed = Info.Check;
            TestTrue(*FString::Printf(TEXT("%s: '%s' parses back"), Name, *Id),
                PinWrightAudit::ParseCheckId(Table, Id, Parsed) && Parsed == Info.Check);
        }
    };

    VerifyTable(TEXT("LevelAudit"), LevelAudit::AllChecks(), LevelAudit::CheckCount);
    VerifyTable(TEXT("LandscapeShape"), LandscapeShape::AllChecks(), LandscapeShape::CheckCount);

    // The masks are uint32s, so a table past 32 rows would silently drop checks off the end of
    // the selection bitmask rather than failing to build.
    TestTrue(TEXT("LevelAudit fits the selection bitmask"), LevelAudit::CheckCount <= 32);
    TestTrue(TEXT("LandscapeShape fits the selection bitmask"), LandscapeShape::CheckCount <= 32);
    return true;
}
