// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for board ticket B-performance-typed-verbs-pin-scalability-cvars.
//
// THE DEFECT. `performance.apply_baseline_settings` and `performance.configure_texture_streaming`
// wrote scalability CVars through a bare `CVar->Set(x)`. The templated helper
// `Set(T Value, EConsoleVariableFlags Flags = ECVF_SetByCode, FName Tag = NAME_None)`
// (IConsoleManager.h:766) defaults to ECVF_SetByCode = 0x0E000000 (:183), six levels above the
// ECVF_SetByScalability = 0x02000000 (:159) that Scalability::SetQualityLevels — and therefore the
// editor's own Settings > Engine Scalability Settings panel — writes at (Scalability.cpp:907).
// FConsoleVariableBase::CanChange is `NewPri >= OldPri` (ConsoleManager.cpp:275-311, compare at
// :280), so after one such call every later change the HUMAN USER makes to the owning group was
// discarded for the rest of the editor session. Seven ECVF_Scalability CVars across the two verbs
// were affected, spanning PostProcessQuality, ShadowQuality, TextureQuality and — via r.VSync,
// which carries the flag with no BaseScalability.ini row — nothing an ini scan can see.
//
// This is the THIRD door to the same pin. The other two are console lines and are guarded by
// Handlers/ScalabilityConsoleGuard.h (tests in TestScalabilityConsoleGuard.cpp beside this file);
// these two verbs pass no console string at all, so no widening of that predicate reaches them.
// They are also the verbs that guard's own refusal text steers callers ONTO, which is what makes
// this the worst of the three.
//
// WHAT THESE TESTS PIN. The two verbs must not RAISE the SetBy priority of any CVar they write.
// Asserted as "the SetBy after the call equals the SetBy before it" rather than as "the SetBy is
// ECVF_SetByScalability", because writing at a fixed Scalability priority is the wrong fix and
// would be discarded outright for r.VSync, which UGameUserSettings::ApplyNonResolutionSettings
// writes at ECVF_SetByGameSetting (GameUserSettings.cpp:529). See
// Handlers/CVarPriorityPreservingSet.h for the full argument.
//
// TEST HYGIENE, WHICH IS THE DEFECT ITSELF AND THEREFORE NOT OPTIONAL. These tests call the real
// verbs, so they really do write real scalability CVars in a shared editor. Three rules keep that
// from leaving damage behind:
//   - every CVar written is snapshotted (value AND SetBy) before the call and restored after it;
//   - the restore is issued at the CVar's CURRENT priority, not the snapshotted one, so it lands
//     unconditionally — including on a REVERTED build where the verb has just stamped the CVar at
//     ECVF_SetByCode and a Scalability-priority restore would be silently discarded;
//   - the configure_texture_streaming case passes each CVar the value it ALREADY holds, so on a
//     fixed build that call is a complete no-op and only the priority stamp is under test.
// What a reverted build still leaves behind is the priority stamp itself, which no test can undo:
// `Unset` is not reachable through IConsoleVariable's public surface for this purpose and
// re-setting lower is exactly what CanChange refuses. That residue is the defect under repair,
// not something these tests introduce.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/IConsoleManager.h"

#include "Handlers/CVarPriorityPreservingSet.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

// Named (not anonymous) and prefixed: this file joins a unity blob with every other test in the
// bucket, so a bare `FCVarSnapshot` would collide on the next author who reaches for it.
namespace PerformanceCVarPriorityTestSupport
{
    // One CVar's state before a verb touched it, plus the restore that puts it back.
    struct FPerformanceCVarSnapshot
    {
        FString Name;
        IConsoleVariable* CVar = nullptr;
        FString Value;
        EConsoleVariableFlags SetBy = ECVF_SetByConstructor;

        bool IsValid() const { return CVar != nullptr; }

        static FPerformanceCVarSnapshot Take(const TCHAR* InName)
        {
            FPerformanceCVarSnapshot Snapshot;
            Snapshot.Name = InName;
            Snapshot.CVar = IConsoleManager::Get().FindConsoleVariable(InName);
            if (Snapshot.CVar)
            {
                Snapshot.Value = Snapshot.CVar->GetString();
                Snapshot.SetBy = CVarPriorityPreservingSet::ReadCurrentSetByPriority(Snapshot.CVar);
            }
            return Snapshot;
        }

        EConsoleVariableFlags ReadSetByNow() const
        {
            return CVarPriorityPreservingSet::ReadCurrentSetByPriority(CVar);
        }

        // Restores the VALUE at whatever priority the CVar carries right now. Deliberately not at
        // the snapshotted priority: on a build where the verb raised it, a snapshot-priority write
        // would be discarded by CanChange and the host would keep the verb's value on top of the
        // wrong priority. Restoring at the current priority always lands, so the value is put back
        // in both worlds and only the (undoable-by-nobody) priority stamp can differ.
        void RestoreValue() const
        {
            if (CVar)
            {
                CVar->Set(*Value, ReadSetByNow());
            }
        }
    };

    // The seven CVars performance.apply_baseline_settings writes, in registration order. Six carry
    // ECVF_Scalability; r.AllowHDR does not (RenderCore.cpp:322, flags :327 are ECVF_ReadOnly) and
    // is included because the verb writes it and a priority raise on it is just as durable.
    const TCHAR* const BaselineSettingsCVars[] = {
        TEXT("r.VSync"),
        TEXT("r.AllowHDR"),
        TEXT("r.MotionBlurQuality"),
        TEXT("r.DepthOfFieldQuality"),
        TEXT("r.BloomQuality"),
        TEXT("r.ShadowQuality"),
        TEXT("r.MaxAnisotropy"),
    };

    // The priority the editor's own Scalability panel writes at. A CVar sitting at or below this
    // can still be changed by the user; one sitting above it cannot.
    constexpr uint32 EditorPanelPriority = static_cast<uint32>(ECVF_SetByScalability);

    // Asserts the two properties that matter, given a before/after pair.
    void AssertPriorityNotRaised(FAutomationTestBase& Test,
        const FPerformanceCVarSnapshot& Before, EConsoleVariableFlags After)
    {
        // int64 rather than uint32 on the assertion: FAutomationTestBase::TestEqual has no uint32
        // overload and a uint32 argument is ambiguous across its int32/int64/SIZE_T/float/double
        // set (AutomationTest.h:1985-1989). The priority values top out at 0x10000000, so the
        // widening is lossless.
        Test.TestEqual(
            *FString::Printf(TEXT("%s keeps the SetBy priority it had before the call"), *Before.Name),
            static_cast<int64>(After), static_cast<int64>(Before.SetBy));

        // Stated separately because it is the user-facing property rather than the mechanism: a
        // CVar the panel could drive before the call must still be one the panel can drive after.
        if (static_cast<uint32>(Before.SetBy) <= EditorPanelPriority)
        {
            Test.TestTrue(
                *FString::Printf(
                    TEXT("%s is still at or below ECVF_SetByScalability, so the editor's "
                         "Scalability panel can still override it"), *Before.Name),
                static_cast<uint32>(After) <= EditorPanelPriority);
        }
    }
}

// ============================================================================
// The helper itself. Pinned once here so the two verb tests below only have to prove each verb
// goes through it.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCVarPriorityPreservingSetTest,
    "PinWright.core.cvar_priority_preserving_set.KeepsExistingSetBy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCVarPriorityPreservingSetTest::RunTest(const FString& Parameters)
{
    using namespace PerformanceCVarPriorityTestSupport;

    const FPerformanceCVarSnapshot Before = FPerformanceCVarSnapshot::Take(TEXT("r.MaxAnisotropy"));
    if (!Before.IsValid())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-not-registered"),
            TEXT("r.MaxAnisotropy is not registered on this host, so the write cannot be exercised "
                 "against a real ECVF_Scalability variable"));
        return true;
    }

    // A DIFFERENT value, so the write is a real one rather than a no-op the engine might elide.
    const int32 Original = Before.CVar->GetInt();
    const int32 Different = (Original == 8) ? 4 : 8;
    CVarPriorityPreservingSet::SetPreservingPriority(Before.CVar, Different);

    TestEqual(TEXT("the write landed - equal priority is accepted, CanChange is >= not >"),
        Before.CVar->GetInt(), Different);
    AssertPriorityNotRaised(*this, Before, Before.ReadSetByNow());

    Before.RestoreValue();
    TestEqual(TEXT("the test restored r.MaxAnisotropy's value"), Before.CVar->GetInt(), Original);
    TestEqual(TEXT("the test restored r.MaxAnisotropy's priority"),
        static_cast<int64>(Before.ReadSetByNow()), static_cast<int64>(Before.SetBy));

    // The reader used everywhere above is itself part of the contract, including its null case.
    TestEqual(TEXT("a null CVar reads as the lowest priority rather than crashing"),
        static_cast<int64>(CVarPriorityPreservingSet::ReadCurrentSetByPriority(nullptr)),
        static_cast<int64>(ECVF_SetByConstructor));
    return true;
}

// ============================================================================
// performance.configure_texture_streaming — writes r.Streaming.PoolSize, an ECVF_Scalability
// TextureQuality member (TextureStreamingHelpers.cpp:119-123).
//
// Every value passed in is the value the CVar already holds, so on a fixed build this whole test
// changes nothing at all and only the priority stamp is under test.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfConfigureTextureStreamingPriorityTest,
    "PinWright.performance.configure_texture_streaming.PreservesCVarSetByPriority",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfConfigureTextureStreamingPriorityTest::RunTest(const FString& Parameters)
{
    using namespace PerformanceCVarPriorityTestSupport;

    const FPerformanceCVarSnapshot PoolSize =
        FPerformanceCVarSnapshot::Take(TEXT("r.Streaming.PoolSize"));
    const FPerformanceCVarSnapshot Streaming =
        FPerformanceCVarSnapshot::Take(TEXT("r.TextureStreaming"));
    if (!PoolSize.IsValid() || !Streaming.IsValid())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("streaming-cvars-not-registered"),
            FString::Printf(TEXT("r.Streaming.PoolSize registered=%d, r.TextureStreaming "
                                 "registered=%d - PLATFORM_SUPPORTS_TEXTURE_STREAMING gates the "
                                 "second (TextureStreamingHelpers.cpp:103)"),
                PoolSize.IsValid() ? 1 : 0, Streaming.IsValid() ? 1 : 0));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetBoolField(TEXT("enabled"), Streaming.CVar->GetInt() != 0);
    Payload->SetNumberField(TEXT("poolSize"), PoolSize.CVar->GetInt());
    // boostPlayerLocation stays off: it is the only branch that touches GEditor and a streaming
    // manager, neither of which this assertion needs.
    Payload->SetBoolField(TEXT("boostPlayerLocation"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("performance.configure_texture_streaming is registered"),
        InvokeHandlerWithCapture(TEXT("performance.configure_texture_streaming"), Payload, Capture));
    TestTrue(TEXT("performance.configure_texture_streaming reported success"), Capture.bSuccess);

    AssertPriorityNotRaised(*this, PoolSize, PoolSize.ReadSetByNow());
    AssertPriorityNotRaised(*this, Streaming, Streaming.ReadSetByNow());

    PoolSize.RestoreValue();
    Streaming.RestoreValue();
    return true;
}

// ============================================================================
// performance.apply_baseline_settings — six ECVF_Scalability CVars across three groups, plus the
// response field that used to assert the absence of the side effect it was causing.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPerfApplyBaselineSettingsPriorityTest,
    "PinWright.performance.apply_baseline_settings.PreservesCVarSetByPriority",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPerfApplyBaselineSettingsPriorityTest::RunTest(const FString& Parameters)
{
    using namespace PerformanceCVarPriorityTestSupport;

    TArray<FPerformanceCVarSnapshot> Snapshots;
    for (const TCHAR* Name : BaselineSettingsCVars)
    {
        FPerformanceCVarSnapshot Snapshot = FPerformanceCVarSnapshot::Take(Name);
        if (Snapshot.IsValid())
        {
            Snapshots.Add(MoveTemp(Snapshot));
        }
    }
    if (Snapshots.Num() == 0)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("baseline-cvars-not-registered"),
            TEXT("none of the seven r.* CVars performance.apply_baseline_settings writes is "
                 "registered on this host"));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("profile"), TEXT("balanced"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("performance.apply_baseline_settings is registered"),
        InvokeHandlerWithCapture(TEXT("performance.apply_baseline_settings"), Payload, Capture));
    TestTrue(TEXT("performance.apply_baseline_settings reported success"), Capture.bSuccess);

    // The whole ticket, asserted per CVar. Before the fix this failed on every one of the six
    // scalability members: each came back stamped ECVF_SetByCode.
    for (const FPerformanceCVarSnapshot& Snapshot : Snapshots)
    {
        AssertPriorityNotRaised(*this, Snapshot, Snapshot.ReadSetByNow());
    }

    if (Capture.Result.IsValid())
    {
        // The response used to publish scalabilityGroupsChanged:false and nothing else — literally
        // true (no sg.* value moved) and materially false (six scalability CVars had just been
        // pinned above the panel). This is the field that answers the question the caller was
        // actually asking.
        bool bPinned = true;
        TestTrue(TEXT("the response carries scalabilityCVarsPinned"),
            Capture.Result->TryGetBoolField(TEXT("scalabilityCVarsPinned"), bPinned));
        TestFalse(TEXT("scalabilityCVarsPinned is false - the verb cannot raise a priority"),
            bPinned);

        // appliedCVars must report the value READ BACK from the CVar, not the requested one. A
        // clamp, or a write CanChange discarded because something already sits higher, would
        // otherwise be reported as applied.
        const TArray<TSharedPtr<FJsonValue>>* AppliedArr = nullptr;
        if (Capture.Result->TryGetArrayField(TEXT("appliedCVars"), AppliedArr) && AppliedArr)
        {
            for (const TSharedPtr<FJsonValue>& Entry : *AppliedArr)
            {
                const TSharedPtr<FJsonObject>* Obj = nullptr;
                if (!Entry.IsValid() || !Entry->TryGetObject(Obj) || !Obj || !Obj->IsValid())
                {
                    continue;
                }
                FString ReportedName;
                double ReportedValue = 0.0;
                if (!(*Obj)->TryGetStringField(TEXT("cvar"), ReportedName)
                    || !(*Obj)->TryGetNumberField(TEXT("value"), ReportedValue))
                {
                    continue;
                }
                if (IConsoleVariable* Live =
                        IConsoleManager::Get().FindConsoleVariable(*ReportedName))
                {
                    TestEqual(*FString::Printf(
                            TEXT("appliedCVars reports %s as the CVar actually reads, not as "
                                 "requested"), *ReportedName),
                        static_cast<int32>(ReportedValue), Live->GetInt());
                }
            }
        }
    }

    for (const FPerformanceCVarSnapshot& Snapshot : Snapshots)
    {
        Snapshot.RestoreValue();
        TestEqual(*FString::Printf(TEXT("the test restored %s"), *Snapshot.Name),
            Snapshot.CVar->GetString(), Snapshot.Value);
    }
    return true;
}
