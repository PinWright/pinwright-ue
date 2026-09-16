// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-effect-niagara-activation-not-measured.
//
// THE DEFECT. effect.activate_niagara wrote SetBoolField("active", true) immediately after
// UNiagaraComponent::Activate(bReset), and effect.deactivate_niagara wrote a hardcoded false the
// same way. Neither read IsActive() back, so `active` was an echo of what the verb was ASKED to do
// rather than a statement about what happened. Activate returns void and ActivateInternal bails
// without ever setting the active flag on several paths (NiagaraComponent.cpp:1301-1375): no asset,
// a host that can never render, an unregistered component, a system not allowed to run, an asset
// with outstanding compilation. Every one of them reported active:true.
//
// THE FIXTURE. A placed, registered UNiagaraComponent with NO asset. Activate takes the
// `Asset == nullptr` early return (NiagaraComponent.cpp:1311-1319) and returns before Super::Activate
// is ever reached, so the component cannot become active however the call is made. That is the
// cheapest honest "cannot become active" case: it needs no content, so it measures the same thing on
// every host, and it does not depend on a compile ever finishing.
//
// The probe actor is deliberately NOT RF_Transient: UEditorActorSubsystem::GetAllLevelActors, which
// both verbs walk to resolve `systemName`, filters transient actors out
// (EditorActorSubsystem.cpp:386), so a transient probe would be invisible to the verb and the run
// would fail as SYSTEM_NOT_FOUND rather than measuring anything. It is spawned under
// FScopedEditorWorldActorGuard, which destroys it and restores the level's dirty flag on scope exit.
//
// Before the fix, the activate test fails on the `active` assertion (the verb reported true for a
// component that never started) and both tests fail on `requestedActive` (the field did not exist).

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "NiagaraComponent.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace PinWrightEffectActivationTest
{
    // A level actor whose root IS an asset-less Niagara component, which is the shape ANiagaraActor
    // places minus the asset. bAutoActivate is cleared before RegisterComponent because
    // registration activates an auto-activate component outside a game world
    // (UActorComponent::RegisterComponentWithWorld), and the verb under test must be the only thing
    // that calls Activate here.
    UNiagaraComponent* SpawnAssetlessNiagaraProbe(UWorld* World, const FString& Label)
    {
        if (!World)
        {
            return nullptr;
        }
        AActor* Actor = World->SpawnActor<AActor>(
            AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
        if (!Actor)
        {
            return nullptr;
        }
        UNiagaraComponent* Component = NewObject<UNiagaraComponent>(
            Actor, TEXT("NiagaraComponent0"), RF_Transient);
        if (!Component)
        {
            return nullptr;
        }
        Component->bAutoActivate = false;
        Actor->SetRootComponent(Component);
        Actor->AddInstanceComponent(Component);
        Component->OnComponentCreated();
        Component->RegisterComponent();
        Actor->SetActorLabel(Label);
        return Component;
    }

    FString MakeProbeLabel()
    {
        return FString::Printf(TEXT("PW_EffectActivationProbe_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
    }

    // fx.Niagara.ComponentWarnNullAsset defaults to 0, so the null-asset path is normally silent —
    // but a host that turned it on logs a LogNiagara warning from inside Activate, and the
    // automation framework elevates a captured log warning to a test error by default
    // (UAutomationControllerSettings::bElevateLogWarningsToErrors). Declaring it expected with a
    // negative occurrence count makes it optional: absent on a default host, tolerated on one that
    // enabled the cvar.
    void AllowNullAssetActivationWarning(FAutomationTestBase& Test)
    {
        Test.AddExpectedMessagePlain(TEXT("Failed to activate Niagara Component due to missing or invalid asset"),
            ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
    }
}

// ============================================================================
// effect.activate_niagara — `active` is read back, not echoed.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEffectActivateNiagaraReportsMeasuredActivationTest,
    "PinWright.effect.activate_niagara.ActiveIsMeasuredNotEchoed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEffectActivateNiagaraReportsMeasuredActivationTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightEffectActivationTest;

    UWorld* const World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world is open; activate_niagara's activation read-back was not asserted"));
        return true;
    }

    AllowNullAssetActivationWarning(*this);

    FScopedEditorWorldActorGuard Guard;
    const FString Label = MakeProbeLabel();
    UNiagaraComponent* const Component = SpawnAssetlessNiagaraProbe(World, Label);
    if (!Component)
    {
        AddError(TEXT("Failed to spawn the asset-less Niagara probe component."));
        return false;
    }
    // The premise of the whole case. If the probe were already running there would be nothing
    // unstartable to measure and the assertions below would be scoring the wrong thing.
    TestFalse(TEXT("the probe component starts inactive"), Component->IsActive());

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("systemName"), Label);
    Payload->SetBoolField(TEXT("reset"), true);

    FTestResponseCapture Capture;
    TestTrue(TEXT("effect.activate_niagara handler found"),
        InvokeHandlerWithCapture(TEXT("effect.activate_niagara"), Payload, Capture));
    // A SYSTEM_NOT_FOUND here would mean the probe never reached the verb, and every assertion
    // below would then be reporting a probe defect as a verb defect.
    TestTrue(TEXT("the verb resolved the probe actor and succeeded"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    // THE REGRESSION. Activate could not have started this component, so the verb must not say it
    // did. The old handler published a constant true here regardless.
    bool bReportedActive = false;
    const bool bHasActive = Capture.Result->TryGetBoolField(TEXT("active"), bReportedActive);
    TestFalse(TEXT("activate does not report a component that cannot start as active"),
        bHasActive && bReportedActive);

    // Whatever is published must be the engine's own reading, not a value the handler chose.
    if (bHasActive)
    {
        TestEqual(TEXT("the reported active matches the component's measured IsActive()"),
            bReportedActive, Component->IsActive());
    }
    else
    {
        // The only legal reason to omit the field is that there was nothing left to read.
        TestFalse(TEXT("active is omitted only when the component no longer exists"),
            IsValid(Component));
    }

    // The request is named separately so it stays legible beside the measurement.
    bool bRequestedActive = false;
    TestTrue(TEXT("the response names the requested state separately"),
        Capture.Result->TryGetBoolField(TEXT("requestedActive"), bRequestedActive));
    TestTrue(TEXT("requestedActive carries what the call asked for"), bRequestedActive);

    // A request that did not take effect is the case the caller most needs to hear about.
    TestTrue(TEXT("a request that did not take effect publishes an activationWarning"),
        Capture.Result->HasField(TEXT("activationWarning")));

    return true;
}

// ============================================================================
// effect.deactivate_niagara — `active` is read back, not hardcoded false.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEffectDeactivateNiagaraReportsMeasuredActivationTest,
    "PinWright.effect.deactivate_niagara.ActiveIsMeasuredNotEchoed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEffectDeactivateNiagaraReportsMeasuredActivationTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightEffectActivationTest;

    UWorld* const World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world is open; deactivate_niagara's activation read-back was not asserted"));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    const FString Label = MakeProbeLabel();
    UNiagaraComponent* const Component = SpawnAssetlessNiagaraProbe(World, Label);
    if (!Component)
    {
        AddError(TEXT("Failed to spawn the asset-less Niagara probe component."));
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("systemName"), Label);

    FTestResponseCapture Capture;
    TestTrue(TEXT("effect.deactivate_niagara handler found"),
        InvokeHandlerWithCapture(TEXT("effect.deactivate_niagara"), Payload, Capture));
    TestTrue(TEXT("the verb resolved the probe actor and succeeded"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    // Deactivate is a request to stop spawning, not a kill — DeactivateInternal ends with
    // SetActiveFlag(!SystemInstanceController->IsComplete()), so `active` genuinely varies and must
    // be read rather than assumed. This probe completes immediately, so the measured answer here is
    // false; what is asserted is that the published value IS the measurement.
    bool bReportedActive = true;
    const bool bHasActive = Capture.Result->TryGetBoolField(TEXT("active"), bReportedActive);
    if (bHasActive)
    {
        TestEqual(TEXT("the reported active matches the component's measured IsActive()"),
            bReportedActive, Component->IsActive());
    }
    else
    {
        TestFalse(TEXT("active is omitted only when the component no longer exists"),
            IsValid(Component));
    }

    bool bRequestedActive = true;
    TestTrue(TEXT("the response names the requested state separately"),
        Capture.Result->TryGetBoolField(TEXT("requestedActive"), bRequestedActive));
    TestFalse(TEXT("requestedActive carries what the call asked for"), bRequestedActive);

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
