// Copyright (c) 2026 Alexander Penkin. MIT License.

// Structural guard: no registered automation test id may be a strict dot-prefix of another.
//
// MECHANISM (UE 5.8.1, read from C:\UE_5.8\Engine). The shorter id is not "recognised and
// skipped" - its report node is silently ADOPTED as the parent of the longer id, after which
// every leaf check in the controller stops seeing it.
//
// FAutomationReport::EnsureReportExists
// (Engine/Source/Developer/AutomationController/Private/AutomationReport.cpp:580-676) splits
// FAutomationTestInfo::GetDisplayName() on the FIRST "." (:587), chops the matching prefix off
// FullPath (:597), and looks an existing child up by GetFullTestPath() equality (:611). That
// lookup does not care whether the node it finds is a leaf or a branch. So:
//   1. "A.B" is inserted first and becomes a LEAF report      (:626, bIsParent = false).
//   2. "A.B.C" arrives, chops to "A.B", matches that same leaf at :611, and because the name
//      remainder is non-empty RECURSES INTO IT (:672) - hanging "C" underneath the leaf.
// Order is deterministic, not luck: AutomationControllerManager.cpp:1047 sorts the batch by
// GetDisplayName() before inserting, and a strict prefix always sorts first. The reverse order
// (only reachable across device clusters) is worse: :631-632 would have created a synthetic
// parent whose TestName is TEXT(""), and "A.B" would then bind to it, discarding the real
// command string entirely.
//
// The kill is that the node now has children while every "is this runnable?" test in the
// controller is written as ChildReports.Num() == 0 rather than !IsParent():
//   GetNextReportToExecute  AutomationReport.cpp:683  - only the else branch returns AsShared()
//   GetEnabledTestReports   AutomationReport.cpp:720
//   GetEnabledTestsNum      AutomationReport.cpp:224
//   GetEnabledTestNames     AutomationReport.cpp:133
//   GetFilteredTestNames    AutomationReport.cpp:168
//   SetEnabledTests         AutomationReport.cpp:192
//   FindLeafReport          AutomationReportManager.cpp:261
// Nothing on that path logs, warns or ensures. "A.B" produces no result of any kind - not a
// pass, not a fail, not a skip. Because it never enters the queue it is ABSENT from the
// "<N> tests performed" count (AutomationCommandline.cpp:411 counts filtered leaf names)
// rather than inflating it, which is exactly why the suite-count reconciliation in CLAUDE.md
// cannot see the loss. Note FAutomationTestFramework::StartTestByName is NOT affected - its
// AutomationTestClassNameToInstanceMap is flat and keyed on the C++ class name - so the defect
// lives entirely in the AutomationController report tree.
//
// Only a "." collides. "Foo.BindDispatcher" vs "Foo.BindDispatcher_WithDelegate" is a plain
// string prefix but not a dot-prefix; it never reaches the split at :587 and both members run.
// Hence the explicit TEXT(".") below rather than a bare StartsWith.
//
// WHICH ACCESSOR IS THE ID. FAutomationTestBase::GenerateTestNames
// (Engine/Source/Runtime/Core/Private/Misc/AutomationTest.cpp:2025-2037) constructs every
// FAutomationTestInfo with the beautified name - 'PinWright.infra.foo.Bar' - as BOTH
// DisplayName and FullTestPath, and with the C++ class name as the third argument, TestName.
// So GetTestName() is the StartTestByName command string and is NOT the id; GetFullTestPath()
// is the documented unique dot-separated path and is what EnsureReportExists matches nodes on
// (:611). This walk reads GetFullTestPath(). The self-check below fails loudly if that changes.
//
// WHY EVERY COMPARISON HERE IS CASE-INSENSITIVE. The engine's collision is case-insensitive:
// AutomationReport.cpp:611 compares with FString::operator==, which is
// Equals(Rhs, ESearchCase::IgnoreCase) (UnrealString.h.inl:912-915), and the pre-insert sort at
// AutomationControllerManager.cpp:1047 uses FString::operator<, which is Stricmp
// (UnrealString.h.inl:871-873). 'PinWright.foo' therefore DOES adopt 'PinWright.FOO.Bar'. A
// case-sensitive walk here would miss that pair and report a false green, so this gate folds
// case exactly as the engine does. FString::StartsWith already defaults to IgnoreCase
// (UnrealString.h.inl:1517) but every call passes it explicitly, because FString::Compare and
// FString::Equals default the other way (:1303, :1271) and the mismatch is easy to reintroduce.
//
// LIMITATIONS, stated plainly rather than implied:
//   * FAutomationTestFramework::GetValidTestNames (AutomationTest.cpp:800-889) filters to the
//     tests valid for THIS host right now: application context (Editor/Game/Commandlet),
//     feature flags (RHI, unattended-ness), and the framework's current RequestedTestFilter.
//     The walk therefore proves "nothing this host would run collides", not "nothing in the
//     tree collides". A test excluded by flags on this host is not covered.
//   * The walk deliberately does not widen that filter. SetRequestedTestFilter
//     (AutomationTest.h:1216) has no matching getter, so the previous value cannot be restored,
//     and mutating it mid-run changes log-capture behaviour (AutomationTest.cpp:1302, :1391)
//     for every test queued after this one.
//   * Sub-module tests (PinWrightGeometry and the other integration modules) only register on
//     hosts where the owning engine plugin is enabled, so on a host without it their ids are
//     simply not present in this enumeration and cannot be checked here.
//   * The complementary whole-tree check is static and now exists:
//     Content/Python/check_test_ids.py parses every IMPLEMENT_*AUTOMATION_TEST id literal out of
//     source, needs no editor and runs as its own CI job. It sees the ids the three bullets above
//     hide from this walk - conditionally compiled ones and disabled sub-modules' - because it
//     does not evaluate the preprocessor at all. That costs it the other direction: it can flag a
//     pair no single build would register together, which is the safe way round. This one is
//     host-specific but sees what the framework actually holds. Neither subsumes the other; both
//     are kept.
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAutomationTestIdPrefixCollisionGate,
    "PinWright.infra.automation_registry.NoPrefixCollisions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAutomationTestIdPrefixCollisionGate::RunTest(const FString& Parameters)
{
    TArray<FAutomationTestInfo> AllTests;
    FAutomationTestFramework::Get().GetValidTestNames(AllTests);

    TArray<FString> Ids;
    Ids.Reserve(AllTests.Num());
    for (const FAutomationTestInfo& Info : AllTests)
    {
        Ids.Add(Info.GetFullTestPath());
    }

    // Default TArray<FString>::Sort uses FString::operator<, i.e. Stricmp - the same ordering
    // AutomationControllerManager.cpp:1047 applies before inserting into the report tree. The
    // early-exit in the inner loop below depends on ids that share a prefix being contiguous
    // under this exact ordering, so the sort and the StartsWith must fold case the same way.
    Ids.Sort();

    // Self-check first: this test's own id must appear in the walk. If the accessor above is
    // ever changed to GetTestName() (which yields the C++ class name) or if the enumeration
    // comes back empty on some host, every "no collisions found" result below would be
    // vacuously green - a gate that cannot fail is worse than no gate at all.
    const FString SelfId = TEXT("PinWright.infra.automation_registry.NoPrefixCollisions");
    if (!TestTrue(
            FString::Printf(TEXT("Registry walk is non-vacuous: it contains this test's own id '%s' ")
                            TEXT("(saw %d valid test ids on this host)"), *SelfId, Ids.Num()),
            Ids.Contains(SelfId)))
    {
        AddError(TEXT("GetValidTestNames() did not yield this test's own id, so the prefix walk ")
                 TEXT("proves nothing. Check that FAutomationTestInfo::GetFullTestPath() is still ")
                 TEXT("the beautified id and not the C++ class name (Core/Private/Misc/")
                 TEXT("AutomationTest.cpp, FAutomationTestBase::GenerateTestNames)."));
        return false;
    }

    int32 OwnCollisions = 0;
    int32 ForeignCollisions = 0;

    for (int32 i = 0; i < Ids.Num(); ++i)
    {
        const FString& Shorter = Ids[i];
        const FString DotPrefix = Shorter + TEXT(".");

        // Sorted, so every id beginning with Shorter is contiguous after it. Stop at the first
        // neighbour that does not, which keeps this walk near-linear over ~4k ids.
        for (int32 j = i + 1; j < Ids.Num() && Ids[j].StartsWith(Shorter, ESearchCase::IgnoreCase); ++j)
        {
            if (!Ids[j].StartsWith(DotPrefix, ESearchCase::IgnoreCase))
            {
                // Plain string extension with no "." - harmless, both ids run.
                continue;
            }

            const FString& Longer = Ids[j];

            // Ours vs someone else's. A collision pair always shares a namespace (the longer id
            // starts with the shorter one), so testing the SHORTER id settles both. We FAIL on
            // PinWright ids because we own that source and can rename them. Engine and
            // third-party ids get a WARNING instead: they are outside this plugin's tree, we
            // cannot fix them, and going red for another module's defect would train people to
            // ignore this gate - which is how the five original victims survived for months.
            if (Shorter.StartsWith(TEXT("PinWright."), ESearchCase::IgnoreCase))
            {
                ++OwnCollisions;
                AddError(FString::Printf(
                    TEXT("Automation test id '%s' is a dot-prefix of '%s'. '%s' is therefore adopted ")
                    TEXT("as a branch node by FAutomationReport::EnsureReportExists and NEVER ")
                    TEXT("EXECUTES - no pass, no fail, no skip, and it is absent from the ")
                    TEXT("'<N> tests performed' count rather than failing it. Fix: give '%s' a leaf ")
                    TEXT("suffix naming what it asserts, e.g. '%s.<WhatItAsserts>', and update every ")
                    TEXT("reference to the old id."),
                    *Shorter, *Longer, *Shorter, *Shorter, *Shorter));
            }
            else
            {
                ++ForeignCollisions;
                AddWarning(FString::Printf(
                    TEXT("Non-PinWright automation test id '%s' is a dot-prefix of '%s', so '%s' never ")
                    TEXT("executes on this host. Reported as a warning and not a failure: the id is ")
                    TEXT("outside this plugin's source tree and cannot be renamed from here."),
                    *Shorter, *Longer, *Shorter));
            }
        }
    }

    AddInfo(FString::Printf(
        TEXT("Prefix gate walked %d registered test ids valid on this host: %d PinWright collision(s), ")
        TEXT("%d non-PinWright collision(s)."),
        Ids.Num(), OwnCollisions, ForeignCollisions));

    return OwnCollisions == 0;
}
