// Copyright (c) 2026 Alexander Penkin. MIT License.

// C5 acceptance tests for the Niagara capture subject. Every assertion here runs against the
// mechanism in CaptureSubjectProviders_Niagara.h directly, with no asset editor open and no
// capture taken, because the two things that can be measured about a particle subject -- the
// instant the simulation actually reached, and what the response is allowed to claim about it --
// are both invisible in a PNG. Pixel assertions are deliberately absent for that reason alone.
//
// This comment used to give a SECOND reason that is now RETRACTED: that FAdvancedPreviewScene is
// not lit under `UnrealEditor-Cmd -unattended`, so a frame-based test here would measure the host
// rather than the code. It was believed and acted on for weeks. Commit bcc334e8 disproved it - the
// captures the claim rested on were three shots overwriting ONE file, because the auto-generated
// screenshot filename carries a one-second timestamp while a capture takes ~60 ms. Re-measured,
// the same fixture reads meanLuminance 0.3644 headless and 0.3644 interactively; the headless
// preview scene is lit exactly as the interactive one is. See the bullet marked
// `RETRACTED 2026-08-21` in docs/lessons.md, which keeps the withdrawn text verbatim. The absence
// of pixel assertions here does NOT depend on that claim and is unchanged by its withdrawal.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"

#include "Handlers/Render/CaptureSubjectProviders_Niagara.h"
#include "Handlers/Render/CaptureSubject.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "SEditorViewport.h"
#include "Templates/UnrealTypeTraits.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#include "NiagaraCommon.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraSystem.h"

// Upgrades one allow-list entry from human-verified to compiler-checked.
//
// C1's GetEditorViewportTypeAllowList() carries SNiagaraSystemViewport with
// bCompileTimeVerified=false, on the reasoning that a type in a private engine header cannot be
// included from a plugin. That is true in general and false for this one: PinWright.Build.cs:41-44
// already puts NiagaraEditor/Private on this module's private include path (it was added for
// NiagaraNodeStaticSwitch.h), and TIsDerivedFrom needs only the class definition -- no symbol and
// no linkage, so UNiagaraComponent's MinimalAPI is irrelevant to it. The claim behind that entry
// can therefore be checked by the compiler after all, and this is where it is checked: if Epic ever
// reparents the widget, the build fails here rather than the StaticCastSharedRef inside
// FindEditorViewportInWidgetTree becoming undefined behaviour in the field.
#if __has_include("Widgets/SNiagaraSystemViewport.h")
#include "Widgets/SNiagaraSystemViewport.h"
#define PINWRIGHT_HAS_NIAGARA_SYSTEM_VIEWPORT_HEADER 1
static_assert(TIsDerivedFrom<SNiagaraSystemViewport, SEditorViewport>::Value,
    "SNiagaraSystemViewport is on the capture-subject viewport allow-list, which casts a widget of "
    "that exact type to SEditorViewport. SEditorViewport carries no SLATE_DECLARE_WIDGET, so there "
    "is no runtime hierarchy check behind that cast -- if this class no longer derives from it, "
    "remove the allow-list entry rather than keeping an undefined-behaviour cast.");
#else
#define PINWRIGHT_HAS_NIAGARA_SYSTEM_VIEWPORT_HEADER 0
#endif

namespace
{
    // Every key in the object whose name carries "reproducib", other than the one warning field
    // that is allowed to. This is the grep-style check the plan asks for: it fails on `reproducible`
    // and equally on a `timeReproducible` or `isReproducible` mirror somebody adds later, which a
    // HasField("reproducible") test would sail straight past.
    TArray<FString> FindReproducibilityClaimKeys(const TSharedPtr<FJsonObject>& Object)
    {
        TArray<FString> Offenders;
        if (!Object.IsValid())
        {
            return Offenders;
        }
        for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : Object->Values)
        {
            if (Pair.Key.Contains(TEXT("reproducib"), ESearchCase::IgnoreCase) &&
                !Pair.Key.Equals(TEXT("reproducibilityWarning"), ESearchCase::CaseSensitive))
            {
                Offenders.Add(Pair.Key);
            }
        }
        return Offenders;
    }

    // A live, registered, unowned Niagara component running the stock template system, torn down
    // by the destructor. Unowned and RF_Transient so nothing enters the loaded level; the level
    // package's dirty flag is snapshotted and restored anyway, because an automation run that
    // dirties the open map eventually saves it.
    struct FLiveNiagaraComponentFixture
    {
        UNiagaraComponent* Component = nullptr;
        UNiagaraSystem* System = nullptr;
        FString FailureDetail;

        FLiveNiagaraComponentFixture()
        {
            UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
            if (!World)
            {
                FailureDetail = TEXT("No editor world is available on this host.");
                return;
            }
            if (UPackage* LevelPackage = World->PersistentLevel ? World->PersistentLevel->GetPackage() : nullptr)
            {
                DirtyGuardPackage = LevelPackage;
                bLevelWasDirty = LevelPackage->IsDirty();
            }

            // The stock template system, used unmodified rather than duplicated: it ships with
            // baked scripts, so it can actually run. A NewObject<UNiagaraSystem> has nothing to
            // simulate and would make every assertion below vacuous.
            System = LoadObject<UNiagaraSystem>(nullptr, NiagaraEditTestUtils::FixtureSystemAssetPath);
            if (!System)
            {
                FailureDetail = FString::Printf(TEXT("Fixture system %s did not load."),
                    NiagaraEditTestUtils::FixtureSystemAssetPath);
                return;
            }

            Component = NewObject<UNiagaraComponent>(GetTransientPackage(), NAME_None, RF_Transient);
            if (!Component)
            {
                FailureDetail = TEXT("UNiagaraComponent could not be constructed.");
                return;
            }
            Component->SetAutoActivate(false);
            Component->SetAsset(System);
            Component->RegisterComponentWithWorld(World);
            // Solo so the instance owns its own simulation and ManualTick reaches it directly,
            // which is the mode AdvanceSimulation forces anyway (NiagaraSystemInstance.cpp:1000).
            Component->SetForceSolo(true);
            Component->ResetSystem();
        }

        ~FLiveNiagaraComponentFixture()
        {
            if (Component)
            {
                Component->DeactivateImmediate();
                if (Component->IsRegistered())
                {
                    Component->UnregisterComponent();
                }
                Component->DestroyComponent();
                Component = nullptr;
            }
            if (DirtyGuardPackage.IsValid() && !bLevelWasDirty)
            {
                DirtyGuardPackage->SetDirtyFlag(false);
            }
        }

        FLiveNiagaraComponentFixture(const FLiveNiagaraComponentFixture&) = delete;
        FLiveNiagaraComponentFixture& operator=(const FLiveNiagaraComponentFixture&) = delete;

        // True only when a system instance actually came up. Anything less and the age readings
        // below are not measurements of anything.
        bool IsSimulating() const
        {
            double Age = 0.0;
            return Component != nullptr && PinWrightCaptureSubjectNiagara::TryReadSimulatedAge(*Component, Age);
        }

    private:
        TWeakObjectPtr<UPackage> DirtyGuardPackage;
        bool bLevelWasDirty = false;
    };

    // First emitter data on the fixture system, or nullptr.
    FVersionedNiagaraEmitterData* FirstEmitterData(UNiagaraSystem& System, FString& OutEmitterName)
    {
        for (FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
        {
            if (FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData())
            {
                // Read the name back from the handle rather than echoing the one passed to
                // AddEmitterHandle: the system uniquifies names, and a warning that names the
                // requested name instead of the real one is the bug these assertions catch.
                OutEmitterName = Handle.GetName().ToString();
                return EmitterData;
            }
        }
        return nullptr;
    }

    // UNiagaraSystem::bDeterminism is protected (NiagaraSystem.h:1078, inside the protected block
    // opened at :953) and has no setter, so reflection is the only way to author EITHER direction
    // of the reproducibility test - including the off direction, which a fixture duplicated from a
    // shipped template does not necessarily start in. NeedsDeterminism() is asserted after every
    // write, so a reflection failure fails the test rather than silently testing one direction
    // twice.
    bool SetSystemDeterminism(UNiagaraSystem& System, bool bValue)
    {
        FBoolProperty* Property =
            FindFProperty<FBoolProperty>(UNiagaraSystem::StaticClass(), TEXT("bDeterminism"));
        if (!Property)
        {
            return false;
        }
        Property->SetPropertyValue_InContainer(&System, bValue);
        return true;
    }
}

// ---------------------------------------------------------------------------------------------
// The instant a particle frame shows comes from the tick count and the fixed delta, never from how
// long the editor happened to spend between calls.
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCaptureSubjectNiagaraAdvancesByTickCountTest,
    "PinWright.render.capture_subject_niagara.AdvancesByTickCountNotWallClock",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectNiagaraAdvancesByTickCountTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubjectNiagara;

    FLiveNiagaraComponentFixture Fixture;
    if (!Fixture.IsSimulating())
    {
        // Loud, not silent. Nothing below can be measured without a running system instance, and
        // a green tick on a test that measured nothing is the exact failure mode board ticket
        // B-test-skips-assertions-silently exists for.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-niagara-system-instance"),
            Fixture.FailureDetail.IsEmpty()
                ? FString(TEXT("The fixture component registered but no FNiagaraSystemInstance came "
                               "up, so no simulated age could be read."))
                : Fixture.FailureDetail);
        return true;
    }

    const float TickDelta = DefaultTickDeltaSeconds;

    FTimeStep ShortStep;
    FString ErrorCode;
    FString ErrorMessage;
    const double WallBefore = FPlatformTime::Seconds();
    if (!TestTrue(TEXT("Advancing to 1.0 s succeeded"),
            AdvanceToTime(*Fixture.Component, 1.0, TickDelta, ShortStep, ErrorCode, ErrorMessage)))
    {
        AddError(FString::Printf(TEXT("AdvanceToTime failed: %s %s"), *ErrorCode, *ErrorMessage));
        return false;
    }

    FTimeStep LongStep;
    if (!TestTrue(TEXT("Advancing to 2.0 s succeeded"),
            AdvanceToTime(*Fixture.Component, 2.0, TickDelta, LongStep, ErrorCode, ErrorMessage)))
    {
        AddError(FString::Printf(TEXT("AdvanceToTime failed: %s %s"), *ErrorCode, *ErrorMessage));
        return false;
    }
    const double WallElapsed = FPlatformTime::Seconds() - WallBefore;

    TestEqual(TEXT("1.0 s at 1/30 is 30 fixed ticks"), ShortStep.TickCount, 30);
    TestEqual(TEXT("2.0 s at 1/30 is 60 fixed ticks"), LongStep.TickCount, 60);

    // The count function on its own, at the three shapes that pin its contract. A float cannot
    // hold 1/30, so an exact multiple divides to 29.999998 and a plain floor drops a whole tick;
    // the two non-multiples below are what stops that being "fixed" by rounding to nearest, which
    // would simulate a shot PAST the instant it asked for.
    TestEqual(TEXT("Half a second at 1/30 is 15 fixed ticks"),
        TickCountForTime(0.5, TickDelta), 15);
    TestEqual(TEXT("A non-multiple floors rather than rounding up (1.05 s at 1/30)"),
        TickCountForTime(1.05, TickDelta), 31);
    TestEqual(TEXT("A request just short of a tick boundary still floors (0.999 s at 1/30)"),
        TickCountForTime(0.999, TickDelta), 29);
    TestEqual(TEXT("Age 0 is no ticks"), TickCountForTime(0.0, TickDelta), 0);

    if (!TestTrue(TEXT("The simulated age was measured, not assumed"),
            ShortStep.bAgeMeasured && LongStep.bAgeMeasured))
    {
        return false;
    }

    // The plan's criterion, read off the component rather than off the value that was passed in.
    // GetDesiredAge() holds the age the simulation ACTUALLY reached (AdvanceToTime sets it from
    // the system instance, not from the request), so this fails if AdvanceSimulation no-ops.
    const double ShortTarget = ShortStep.TickCount * static_cast<double>(TickDelta);
    const double LongTarget = LongStep.TickCount * static_cast<double>(TickDelta);
    const double HeldAge = static_cast<double>(Fixture.Component->GetDesiredAge());
    TestTrue(FString::Printf(TEXT("Component age %g is within one tick of %g"), HeldAge, LongTarget),
        FMath::Abs(HeldAge - LongTarget) <= TickDelta);
    TestTrue(TEXT("The held age is the age that was measured, not a re-echo of the request"),
        FMath::Abs(HeldAge - LongStep.AchievedAgeSeconds) <= UE_KINDA_SMALL_NUMBER);

    // Proof it advanced at all: a system that never ticked reports age 0 for both requests, which
    // would pass a "within one tick of the target" check only for a target of 0.
    TestTrue(TEXT("The 1.0 s request actually advanced the simulation"), ShortStep.AchievedAgeSeconds > 0.0);
    TestTrue(FString::Printf(TEXT("Measured age %g is within one tick of %g"),
                 ShortStep.AchievedAgeSeconds, ShortTarget),
        FMath::Abs(ShortStep.AchievedAgeSeconds - ShortTarget) <= TickDelta);

    // The wall-clock direction. The two requests are one simulated second apart while the real
    // time spent making them is milliseconds, so an implementation that let real elapsed time
    // drive the age -- SeekToDesiredAge spread over frames, or a plain SetDesiredAge waiting on
    // TickComponent -- cannot produce this gap.
    const double SimulatedGap = LongStep.AchievedAgeSeconds - ShortStep.AchievedAgeSeconds;
    TestTrue(FString::Printf(
                 TEXT("Simulated gap %g s exceeds the %g s of wall clock that produced it"),
                 SimulatedGap, WallElapsed),
        SimulatedGap > WallElapsed);
    TestTrue(FString::Printf(TEXT("Simulated gap %g s is one second within a tick"), SimulatedGap),
        FMath::Abs(SimulatedGap - 1.0) <= TickDelta);

    // A different fixed delta for the same requested instant lands on the same age: the age
    // tracks TickCount * TickDelta, not the number of calls made.
    FTimeStep FineStep;
    if (TestTrue(TEXT("Advancing to 2.0 s at a finer delta succeeded"),
            AdvanceToTime(*Fixture.Component, 2.0, TickDelta * 0.5f, FineStep, ErrorCode, ErrorMessage)))
    {
        TestEqual(TEXT("Halving the delta doubles the tick count"), FineStep.TickCount, LongStep.TickCount * 2);
        TestTrue(FString::Printf(TEXT("Ages agree across deltas: %g vs %g"),
                     FineStep.AchievedAgeSeconds, LongStep.AchievedAgeSeconds),
            FMath::Abs(FineStep.AchievedAgeSeconds - LongStep.AchievedAgeSeconds) <= TickDelta);
    }

    // A delta the engine would silently swallow is refused rather than reported as ticks that
    // never ran (UNiagaraComponent::AdvanceSimulation gates on TickDeltaSeconds > SMALL_NUMBER).
    FTimeStep RefusedStep;
    TestFalse(TEXT("A zero tick delta is refused"),
        AdvanceToTime(*Fixture.Component, 1.0, 0.0f, RefusedStep, ErrorCode, ErrorMessage));
    TestEqual(TEXT("A zero tick delta refuses with INVALID_ARGUMENT"), ErrorCode,
        FString(ErrorCodes::ERR_INVALID_ARGUMENT));

    return true;
}

// ---------------------------------------------------------------------------------------------
// A GPU emitter without authored fixed bounds is unframeable, and the response says so.
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCaptureSubjectNiagaraGpuBoundsWarningTest,
    "PinWright.render.capture_subject_niagara.GpuEmitterWithoutFixedBoundsWarns",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectNiagaraGpuBoundsWarningTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubjectNiagara;

    FString SystemPath;
    UNiagaraSystem* System = nullptr;
    UNiagaraEmitter* SourceEmitter = nullptr;
    FName EmitterName;
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    const bool bBuilt =
        NiagaraEditTestUtils::MakeAuthorableSystem(SystemPath, System, SourceEmitter, EmitterName);
    Roots.System = System;
    Roots.Emitter = SourceEmitter;
    if (!TestTrue(TEXT("Authorable fixture system built"), bBuilt) ||
        !TestNotNull(TEXT("Fixture system exists"), System))
    {
        return false;
    }

    FString GpuEmitterName;
    FVersionedNiagaraEmitterData* EmitterData = FirstEmitterData(*System, GpuEmitterName);
    if (!TestNotNull(TEXT("Fixture emitter data reachable"), EmitterData))
    {
        return false;
    }
    EmitterData->SimTarget = ENiagaraSimTarget::GPUComputeSim;
    EmitterData->CalculateBoundsMode = ENiagaraEmitterCalculateBoundMode::Dynamic;

    // The preconditions the plan demands be asserted first: a CPU-only fixture, or one with
    // authored fixed bounds, would make the warning's absence look like correct behaviour instead
    // of an untested path.
    if (!TestTrue(TEXT("Fixture emitter really is a GPU sim"),
            EmitterData->SimTarget == ENiagaraSimTarget::GPUComputeSim))
    {
        return false;
    }
    if (!TestTrue(TEXT("Fixture emitter really has no authored fixed bounds"),
            EmitterData->CalculateBoundsMode != ENiagaraEmitterCalculateBoundMode::Fixed))
    {
        return false;
    }

    TArray<FString> WarnedEmitters;
    const FString Warning = MakeGpuBoundsWarning(*System, WarnedEmitters);
    TestTrue(TEXT("A GPU emitter without fixed bounds warns"), !Warning.IsEmpty());
    TestTrue(FString::Printf(TEXT("The warning names the emitter '%s'"), *GpuEmitterName),
        !GpuEmitterName.IsEmpty() && Warning.Contains(GpuEmitterName));
    TestTrue(TEXT("The warned-emitter list carries the emitter"), WarnedEmitters.Contains(GpuEmitterName));

    // Both other directions, so the warning is a measurement and not a constant.
    EmitterData->CalculateBoundsMode = ENiagaraEmitterCalculateBoundMode::Fixed;
    TArray<FString> FixedBoundsEmitters;
    TestTrue(TEXT("Authored fixed bounds on a GPU emitter do not warn"),
        MakeGpuBoundsWarning(*System, FixedBoundsEmitters).IsEmpty());
    TestEqual(TEXT("Nothing is listed once bounds are authored"), FixedBoundsEmitters.Num(), 0);

    EmitterData->CalculateBoundsMode = ENiagaraEmitterCalculateBoundMode::Dynamic;
    EmitterData->SimTarget = ENiagaraSimTarget::CPUSim;
    TArray<FString> CpuEmitters;
    TestTrue(TEXT("A CPU emitter with dynamic bounds does not warn"),
        MakeGpuBoundsWarning(*System, CpuEmitters).IsEmpty());
    TestEqual(TEXT("Nothing is listed for a CPU emitter"), CpuEmitters.Num(), 0);

    return true;
}

// ---------------------------------------------------------------------------------------------
// The response never claims particle captures repeat, in either direction.
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCaptureSubjectNiagaraNoReproducibilityClaimTest,
    "PinWright.render.capture_subject_niagara.NoReproducibilityIsClaimed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectNiagaraNoReproducibilityClaimTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubjectNiagara;

    FString SystemPath;
    UNiagaraSystem* System = nullptr;
    UNiagaraEmitter* SourceEmitter = nullptr;
    FName EmitterName;
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    const bool bBuilt =
        NiagaraEditTestUtils::MakeAuthorableSystem(SystemPath, System, SourceEmitter, EmitterName);
    Roots.System = System;
    Roots.Emitter = SourceEmitter;
    if (!TestTrue(TEXT("Authorable fixture system built"), bBuilt) ||
        !TestNotNull(TEXT("Fixture system exists"), System))
    {
        return false;
    }

    FString FixtureEmitterName;
    FVersionedNiagaraEmitterData* EmitterData = FirstEmitterData(*System, FixtureEmitterName);
    if (!TestNotNull(TEXT("Fixture emitter data reachable"), EmitterData))
    {
        return false;
    }
    // Keep the CPU/GPU axis out of this test: a GPU emitter warns on its own and would mask
    // whether the determinism flags are being read at all.
    EmitterData->SimTarget = ENiagaraSimTarget::CPUSim;

    const FTimeStep Step;
    const FBoundsPlan Bounds;

    // ---- direction one: determinism off at every scope this test controls ----
    //
    // AUTHORED, not assumed. The class default of UNiagaraSystem::bDeterminism is false
    // (NiagaraSystem.h:1078), but this fixture is a DuplicateObject of a shipped template
    // (NiagaraEditTestUtils::FixtureSystemAssetPath) and the duplicate inherits whatever the
    // template LOADED with - which is not the class default: UNiagaraSystem::Serialize forces
    // bDeterminism = true on any system package saved before
    // FNiagaraCustomVersion::ChangeSystemDeterministicDefault (NiagaraSystem.cpp:1243-1249),
    // because that was the older default, and the stock templates are that old. Asserting the
    // class default here measured how old the template asset is instead of establishing the state
    // the assertions below need. The write is still checked: NeedsDeterminism() must read back
    // false, so a reflected write that lands on the wrong property or the wrong object fails the
    // test rather than quietly testing the other direction twice.
    EmitterData->bDeterminism = false;
    if (!TestTrue(TEXT("System determinism could be cleared through reflection"),
            SetSystemDeterminism(*System, false)))
    {
        return false;
    }
    if (!TestFalse(TEXT("Fixture system determinism is off"), System->NeedsDeterminism()))
    {
        return false;
    }
    const FDeterminismReport OffReport = ReadDeterminism(*System, nullptr);
    const FString OffWarning = MakeReproducibilityWarning(OffReport);
    TestTrue(TEXT("Determinism off warns"), !OffWarning.IsEmpty());
    TestTrue(TEXT("The warning names the system scope that is off"),
        OffWarning.Contains(TEXT("system determinism")));
    TestTrue(FString::Printf(TEXT("The warning names the emitter scope that is off ('%s')"),
                 *FixtureEmitterName),
        OffWarning.Contains(FixtureEmitterName));

    // The `subject` block a verb actually returns, assembled the way the provider assembles it.
    PinWrightCaptureSubject::FResolvedSubject OffSubject;
    OffSubject.Kind = PinWrightCaptureSubject::ESubjectKind::Niagara;
    OffSubject.AssetPath = SystemPath;
    OffSubject.CaptureSource = TEXT("niagaraSystemEditorPreview");
    OffSubject.bTimeSupported = true;
    OffSubject.bTimeReproducible = false;
    OffSubject.ReproducibilityWarning = OffWarning;
    const TSharedPtr<FJsonObject> OffInfo = PinWrightCaptureSubject::MakeSubjectInfoObject(OffSubject);
    if (!TestTrue(TEXT("A subject block was produced"), OffInfo.IsValid()))
    {
        return false;
    }
    TestTrue(TEXT("The subject block carries the reproducibility warning"),
        OffInfo->HasField(TEXT("reproducibilityWarning")));
    const TArray<FString> OffOffenders = FindReproducibilityClaimKeys(OffInfo);
    TestEqual(FString::Printf(TEXT("The subject block claims no reproducibility (offending keys: %s)"),
                  *FString::Join(OffOffenders, TEXT(", "))),
        OffOffenders.Num(), 0);

    const TSharedPtr<FJsonObject> OffDetail = MakeNiagaraSubjectDetailObject(Step, Bounds, OffReport);
    if (!TestTrue(TEXT("A niagara detail block was produced"), OffDetail.IsValid()))
    {
        return false;
    }
    const TArray<FString> OffDetailOffenders = FindReproducibilityClaimKeys(OffDetail);
    TestEqual(FString::Printf(TEXT("The niagara detail claims no reproducibility (offending keys: %s)"),
                  *FString::Join(OffDetailOffenders, TEXT(", "))),
        OffDetailOffenders.Num(), 0);
    // What it reports INSTEAD of a promise: the run described exactly.
    TestTrue(TEXT("The detail publishes the tick delta"), OffDetail->HasField(TEXT("tickDeltaSeconds")));
    TestTrue(TEXT("The detail publishes the tick count"), OffDetail->HasField(TEXT("tickCount")));
    TestTrue(TEXT("The detail publishes the determinism it measured"),
        OffDetail->HasField(TEXT("determinism")));

    // ---- direction two: every scope that can be turned on, turned on ----
    if (!TestTrue(TEXT("System determinism could be set through reflection"),
            SetSystemDeterminism(*System, true)))
    {
        return false;
    }
    EmitterData->bDeterminism = true;
    if (!TestTrue(TEXT("Fixture system determinism is now on"), System->NeedsDeterminism()))
    {
        return false;
    }
    const FDeterminismReport OnReport = ReadDeterminism(*System, nullptr);
    TestTrue(TEXT("The report reads the system flag"), OnReport.bSystemDeterminism);
    TestEqual(TEXT("No emitter is listed as non-deterministic"), OnReport.NonDeterministicEmitters.Num(), 0);
    TestEqual(TEXT("No GPU emitter is listed"), OnReport.GpuEmitters.Num(), 0);
    TestTrue(TEXT("Determinism fully on does not warn"), MakeReproducibilityWarning(OnReport).IsEmpty());

    PinWrightCaptureSubject::FResolvedSubject OnSubject;
    OnSubject.Kind = PinWrightCaptureSubject::ESubjectKind::Niagara;
    OnSubject.AssetPath = SystemPath;
    OnSubject.CaptureSource = TEXT("niagaraSystemEditorPreview");
    OnSubject.bTimeSupported = true;
    // The flag the provider sets unconditionally. Even with every scope on, no determinism
    // setting covers data interfaces that read world state, so the answer is still false -- and
    // the block below must still publish no field that says otherwise.
    OnSubject.bTimeReproducible = false;
    const TSharedPtr<FJsonObject> OnInfo = PinWrightCaptureSubject::MakeSubjectInfoObject(OnSubject);
    if (!TestTrue(TEXT("A subject block was produced"), OnInfo.IsValid()))
    {
        return false;
    }
    TestFalse(TEXT("Determinism fully on emits no warning field"),
        OnInfo->HasField(TEXT("reproducibilityWarning")));
    const TArray<FString> OnOffenders = FindReproducibilityClaimKeys(OnInfo);
    TestEqual(FString::Printf(TEXT("Still no reproducibility claim with determinism on (offending keys: %s)"),
                  *FString::Join(OnOffenders, TEXT(", "))),
        OnOffenders.Num(), 0);

    // Put the system back before the roots unpin it, so a later test that reuses the duplicate
    // does not inherit a determinism setting this one authored.
    SetSystemDeterminism(*System, false);
    return true;
}

// ---------------------------------------------------------------------------------------------
// The cross-chunk contract this provider stands on: C1's shared allow-list and toolkit gate carry
// the two Niagara entries, and a provider is registered for the Niagara kind.
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCaptureSubjectNiagaraProviderContractTest,
    "PinWright.render.capture_subject_niagara.ProviderAndAllowListEntriesExist",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectNiagaraProviderContractTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubjectNiagara;

    TestNotNull(TEXT("A provider is registered for the niagara kind"),
        PinWrightCaptureSubject::FindProvider(PinWrightCaptureSubject::ESubjectKind::Niagara));

    // The allow-list entry the old suffix predicate could never match. Without it every Niagara
    // capture is a PREVIEW_VIEWPORT_NOT_FOUND, which is what made this kind look impossible.
    TestTrue(TEXT("SNiagaraSystemViewport is allow-listed"),
        PinWrightCaptureSubject::IsAllowListedEditorViewportType(TEXT("SNiagaraSystemViewport")));
    bool bEntryCarriesEvidence = false;
    for (const PinWrightCaptureSubject::FEditorViewportTypeEntry& Entry :
             PinWrightCaptureSubject::GetEditorViewportTypeAllowList())
    {
        if (Entry.TypeName && FString(Entry.TypeName) == TEXT("SNiagaraSystemViewport"))
        {
            bEntryCarriesEvidence = Entry.EngineEvidence != nullptr && *Entry.EngineEvidence != TEXT('\0');
        }
    }
    TestTrue(TEXT("The Niagara allow-list entry cites its engine evidence"), bEntryCarriesEvidence);

    // The failure the allow-list replaces. A widget whose type name merely ends in
    // "EditorViewport" is not necessarily an SEditorViewport -- stock UE 5.8 ships five that are
    // not -- and the suffix rule cast them anyway.
    TestFalse(TEXT("A look-alike suffix name is not allow-listed"),
        PinWrightCaptureSubject::IsAllowListedEditorViewportType(TEXT("SFakeEditorViewport")));
    TestFalse(TEXT("A near-miss Niagara name is not allow-listed"),
        PinWrightCaptureSubject::IsAllowListedEditorViewportType(TEXT("SNiagaraSystemViewportX")));

    // The toolkit gate that makes the FAssetEditorToolkit downcast defined for this kind.
    TestEqual(TEXT("The Niagara toolkit is matched by its GetToolkitFName"),
        NiagaraSystemToolkitName(), FName(TEXT("Niagara")));
    TestTrue(TEXT("The Niagara toolkit name is accepted by the shared gate"),
        PinWrightCaptureSubject::IsSupportedAssetEditorToolkit(NiagaraSystemToolkitName()));
    TestTrue(TEXT("The local toolkit predicate agrees"), IsNiagaraSystemEditor(FName(TEXT("Niagara"))));
    TestFalse(TEXT("A different toolkit is refused"), IsNiagaraSystemEditor(FName(TEXT("StaticMeshEditor"))));

#if !PINWRIGHT_HAS_NIAGARA_SYSTEM_VIEWPORT_HEADER
    // Not a failure -- the allow-list still works -- but the compile-time guarantee at the top of
    // this file is not in force on this build, and that must be visible rather than assumed.
    PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-viewport-header-absent"),
        TEXT("Widgets/SNiagaraSystemViewport.h was not on the include path, so the "
             "TIsDerivedFrom<SNiagaraSystemViewport, SEditorViewport> assert did not "
             "compile. Check PinWright.Build.cs:41-44."));
#endif

    return true;
}

namespace
{
    // Freezes every Niagara simulation in the process, by the engine's own debug throttle:
    // FNiagaraSystemSimulation::Tick_GameThread_Internal returns without simulating anything for
    // any tick at or below fx.Niagara.SystemSimulation.SkipTickDeltaSeconds
    // (NiagaraSystemSimulation.cpp:1149-1152). The system instance stays valid, active and
    // unpaused throughout - which is the whole point: it is the shape of stall that a
    // controller-validity check cannot see.
    //
    // It is also the only stall in that family a test can hold across an AdvanceToTime call.
    // AdvanceToTime routes through EnsureSystemInstance, whose Activate(bReset=true) unpauses the
    // component and resets a complete instance, so neither "paused" nor "complete" survives long
    // enough to be measured from outside.
    //
    // The variable is process-wide, so the restore is a destructor rather than a line at the end
    // of the test: a failed assertion must not leave Niagara frozen for the rest of the suite.
    struct FScopedNiagaraTickFreeze
    {
        explicit FScopedNiagaraTickFreeze(float SkipAtOrBelowSeconds)
        {
            Variable = IConsoleManager::Get().FindConsoleVariable(
                TEXT("fx.Niagara.SystemSimulation.SkipTickDeltaSeconds"));
            if (Variable)
            {
                SavedValue = Variable->GetFloat();
                Variable->Set(SkipAtOrBelowSeconds, ECVF_SetByCode);
            }
        }

        ~FScopedNiagaraTickFreeze()
        {
            if (Variable)
            {
                Variable->Set(SavedValue, ECVF_SetByCode);
            }
        }

        // False on an engine build that does not register the variable, which is a skip rather
        // than a failure - the fix under test is unaffected, only this way of provoking it is.
        bool IsInForce() const { return Variable != nullptr; }

        FScopedNiagaraTickFreeze(const FScopedNiagaraTickFreeze&) = delete;
        FScopedNiagaraTickFreeze& operator=(const FScopedNiagaraTickFreeze&) = delete;

    private:
        IConsoleVariable* Variable = nullptr;
        float SavedValue = 0.0f;
    };
}

// ---------------------------------------------------------------------------------------------
// `simulated` is a measurement of motion, not a restatement of "a system instance exists".
//
// Board ticket B-niagara-preview-age-zero-after-ticks: a preview capture reported 480 simulated
// ticks and `achievedAgeSeconds: 0` in the same response. Both numbers were correct. The age is
// read straight off FNiagaraSystemInstance::Age, which is the only thing AdvanceSimulation's ticks
// increment, and it is read with nothing able to tick in between - so a present zero after N ticks
// is a true statement that the simulation did not move. What was wrong was the field beside it:
// `simulated` was the return of EnsureSystemInstance, which proves only that a controller exists,
// and the advance ticks nothing with a perfectly valid controller on five separate engine paths.
// The response therefore asserted the ticks had run and measured that they had not, in one object.
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCaptureSubjectNiagaraSimulatedTracksMeasuredAgeTest,
    "PinWright.render.capture_subject_niagara.SimulatedTracksMeasuredAgeNotControllerValidity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectNiagaraSimulatedTracksMeasuredAgeTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubjectNiagara;

    FLiveNiagaraComponentFixture Fixture;
    if (!Fixture.IsSimulating())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-niagara-system-instance"),
            Fixture.FailureDetail.IsEmpty()
                ? FString(TEXT("The fixture component registered but no FNiagaraSystemInstance came "
                               "up, so neither direction of the simulated/stalled pair could be "
                               "produced."))
                : Fixture.FailureDetail);
        return true;
    }

    const float TickDelta = DefaultTickDeltaSeconds;
    FString ErrorCode;
    FString ErrorMessage;

    // ---- direction one: the ticks reach the simulation, and the pair says so ----
    FTimeStep Live;
    if (!TestTrue(TEXT("Advancing to 1.0 s succeeded"),
            AdvanceToTime(*Fixture.Component, 1.0, TickDelta, Live, ErrorCode, ErrorMessage)))
    {
        AddError(FString::Printf(TEXT("AdvanceToTime failed: %s %s"), *ErrorCode, *ErrorMessage));
        return false;
    }
    TestTrue(TEXT("Both ends of the age measurement were read"),
        Live.bStartAgeMeasured && Live.bAgeMeasured);
    TestTrue(FString::Printf(TEXT("The age moved from %g to %g"),
                 Live.StartAgeSeconds, Live.AchievedAgeSeconds),
        Live.AchievedAgeSeconds > Live.StartAgeSeconds);
    TestTrue(TEXT("A drive that moved the simulation reports simulated"), Live.bSimulated);
    TestEqual(TEXT("A drive that moved the simulation carries no stall reason"),
        Live.StallReason, FString());

    // ---- direction two: the same call, the same live instance, every tick swallowed ----
    {
        // Above the 1/30 s delta the drive uses, so every ManualTick lands inside the skip window.
        FScopedNiagaraTickFreeze Freeze(1.0f);
        if (!Freeze.IsInForce())
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("no-niagara-skip-tick-cvar"),
                TEXT("fx.Niagara.SystemSimulation.SkipTickDeltaSeconds is not registered on this "
                     "engine build, so a live-but-frozen Niagara instance could not be produced and "
                     "the stalled direction was not asserted."));
            return true;
        }

        FTimeStep Stalled;
        if (!TestTrue(TEXT("Advancing to 1.0 s against a frozen simulation still returns"),
                AdvanceToTime(*Fixture.Component, 1.0, TickDelta, Stalled, ErrorCode, ErrorMessage)))
        {
            AddError(FString::Printf(TEXT("AdvanceToTime failed: %s %s"), *ErrorCode, *ErrorMessage));
            return false;
        }

        // The ticket's exact shape, reproduced: ticks were asked for, a live instance answered the
        // age question, and the answer had not moved.
        TestEqual(TEXT("1.0 s at 1/30 still asks for 30 ticks"), Stalled.TickCount, 30);
        TestTrue(TEXT("A live instance answered the age question on both sides"),
            Stalled.bStartAgeMeasured && Stalled.bAgeMeasured);
        TestEqual(TEXT("The frozen simulation did not move"),
            Stalled.AchievedAgeSeconds, Stalled.StartAgeSeconds);

        // THE COUNTERFACTUAL. EnsureSystemInstance returns true here - the controller is valid,
        // active and unpaused, only the ticks were swallowed - so the previous implementation
        // published `simulated: true` for this exact state, beside an achieved age of 0.
        TestFalse(TEXT("`simulated` is false when the ticks did not move the age"), Stalled.bSimulated);
        TestTrue(TEXT("A stalled drive says why it stalled"), !Stalled.StallReason.IsEmpty());
        TestTrue(FString::Printf(TEXT("The stall reason names the throttle that swallowed the ticks "
                                      "(got: %s)"), *Stalled.StallReason),
            Stalled.StallReason.Contains(TEXT("SkipTickDeltaSeconds")));

        // And the block a caller actually reads.
        const TSharedPtr<FJsonObject> Detail =
            MakeNiagaraSubjectDetailObject(Stalled, FBoundsPlan(), FDeterminismReport());
        if (TestTrue(TEXT("A niagara detail block was produced"), Detail.IsValid()))
        {
            bool bSimulatedField = true;
            TestTrue(TEXT("The detail publishes `simulated`"),
                Detail->TryGetBoolField(TEXT("simulated"), bSimulatedField));
            TestFalse(TEXT("The detail does not claim ticks that never ran"), bSimulatedField);
            TestTrue(TEXT("The detail still publishes the age it measured"),
                Detail->HasField(TEXT("achievedAgeSeconds")));
            TestTrue(TEXT("The detail publishes the age the drive started from"),
                Detail->HasField(TEXT("startAgeSeconds")));
            TestTrue(TEXT("The detail publishes why nothing was simulated"),
                Detail->HasField(TEXT("simulationStalledReason")));

            // The failure this ticket forbids: answering with the requested age instead of the
            // measured one. 30 ticks were asked for, so a request-echo would read 1.0.
            double ReportedAge = -1.0;
            TestTrue(TEXT("The reported age is readable"),
                Detail->TryGetNumberField(TEXT("achievedAgeSeconds"), ReportedAge));
            TestEqual(TEXT("The reported age is the measured one"),
                ReportedAge, Stalled.AchievedAgeSeconds);
            TestTrue(FString::Printf(TEXT("The reported age %g is not the requested 1.0 s echoed "
                                          "back"), ReportedAge),
                ReportedAge < 1.0);
        }
    }

    // ---- and the freeze leaves nothing behind ----
    FTimeStep Thawed;
    if (TestTrue(TEXT("Advancing to 1.0 s after the throttle is cleared succeeded"),
            AdvanceToTime(*Fixture.Component, 1.0, TickDelta, Thawed, ErrorCode, ErrorMessage)))
    {
        TestTrue(TEXT("The simulation moves again once the throttle is cleared"), Thawed.bSimulated);
        TestTrue(TEXT("A moving simulation carries no stall reason"), Thawed.StallReason.IsEmpty());
    }

    return true;
}
