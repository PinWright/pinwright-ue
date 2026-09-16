// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-niagara-validate-green-while-component-inactive.
//
// THE DEFECT. niagara.validate described the ASSET and nothing else. Whether anything in the
// level actually runs that asset is a property of the placed UNiagaraComponent, which no
// niagara.* verb read - so three shipped systems whose components had been left with
// bAutoActivate:false rendered nothing for a full day while `niagara.validate {level:"basic"}`
// returned valid:true with an empty errors array and an empty warnings array. The only signal
// that disagreed was a raw reflected `object.call_function IsActive`.
//
// WHAT IS ASSERTED. The premise, then the failure, in that order:
//   1. A system with no placed component reports "no_components" and raises NOTHING. The
//      false-positive direction is pinned first: a check that warned whenever a caller
//      validated a system without its level open would fire on nearly every call, and the
//      pressure would be to delete it rather than to fix the asset.
//   2. With one placed, registered, bAutoActivate:false component in the open editor world,
//      validate must NOT come back clean. It reports componentActivation "none_active", counts
//      the component, publishes its measured isActive/autoActivate, and raises
//      NIAGARA_NO_ACTIVE_COMPONENT - warning at `basic`, error at `strict` (so `valid` flips),
//      the same layering the three structural codes use.
//
// Before the fix there was no componentActivation field, no components block and no issue, so
// every assertion in part 2 fails.
//
// The probe actor is spawned into the editor world under FScopedEditorWorldActorGuard, which
// destroys it and restores the level's dirty flag on scope exit - the suite runs against
// whatever map the editor opened with, and a leaked actor would be saved into it.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace PinWrightNiagaraComponentActivationTest
{
    // "error" / "warning" / empty when the code is absent, read off the level-normalized
    // top-level arrays rather than the raw compile block.
    FString SeverityOf(const TSharedPtr<FJsonObject>& Result, const TCHAR* Code)
    {
        if (!Result.IsValid())
        {
            return FString();
        }
        for (const TCHAR* ArrayName : { TEXT("errors"), TEXT("warnings") })
        {
            const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
            if (!Result->TryGetArrayField(ArrayName, Values) || !Values)
            {
                continue;
            }
            for (const TSharedPtr<FJsonValue>& Value : *Values)
            {
                FString IssueCode;
                if (Value.IsValid() && Value->Type == EJson::Object
                    && Value->AsObject()->TryGetStringField(TEXT("code"), IssueCode)
                    && IssueCode.Equals(Code, ESearchCase::IgnoreCase))
                {
                    return FString(ArrayName).Equals(TEXT("errors")) ? TEXT("error") : TEXT("warning");
                }
            }
        }
        return FString();
    }

    TSharedPtr<FJsonObject> MakeValidatePayload(const FString& SystemPath, const TCHAR* Level)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("systemPath"), SystemPath);
        Payload->SetStringField(TEXT("level"), Level);
        return Payload;
    }

    // The aspect flags are all off: this asks about the activation block, which is published
    // outside them, and a full inspect of a real system serializes every graph node and pin.
    TSharedPtr<FJsonObject> MakeInspectPayload(const FString& SystemPath, bool bParametersOnly)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), SystemPath);
        Payload->SetBoolField(TEXT("includeProperties"), false);
        Payload->SetBoolField(TEXT("includeStack"), false);
        Payload->SetBoolField(TEXT("includeGraphs"), false);
        Payload->SetBoolField(TEXT("includeCompile"), false);
        Payload->SetBoolField(TEXT("parametersOnly"), bParametersOnly);
        return Payload;
    }

    // A level actor whose root IS the Niagara component, which is the shape ANiagaraActor
    // places. bAutoActivate is written before RegisterComponent on purpose: registration is
    // what activates an auto-activate component outside a game world, so setting it afterwards
    // would leave the component already running and destroy the case under test.
    UNiagaraComponent* SpawnPlacedNiagaraProbe(UWorld* World, UNiagaraSystem* System, bool bAutoActivate)
    {
        if (!World || !System)
        {
            return nullptr;
        }
        FActorSpawnParameters SpawnParams;
        SpawnParams.ObjectFlags = RF_Transient;
        AActor* Actor = World->SpawnActor<AActor>(
            AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
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
        Component->bAutoActivate = bAutoActivate;
        Component->SetAsset(System);
        Actor->SetRootComponent(Component);
        Actor->AddInstanceComponent(Component);
        Component->OnComponentCreated();
        Component->RegisterComponent();
        return Component;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraValidateReportsInactivePlacedComponentTest,
    "PinWright.niagara.validate.InactivePlacedComponentIsNotAPass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraValidateReportsInactivePlacedComponentTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightNiagaraComponentActivationTest;

    UWorld* const World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world is open; validate's placed-component survey was not asserted"));
        return true;
    }

    FString SystemPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(SystemPath);
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    Roots.System = System;
    if (!System)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-fixture-system-unavailable"),
            TEXT("NewTransientSystem returned null; validate's placed-component survey was not asserted"));
        return true;
    }

    // ------------------------------------------------------------------
    // 1. Nothing placed. The survey must say so and raise no issue.
    // ------------------------------------------------------------------
    {
        FTestResponseCapture Capture;
        if (NiagaraEditTestUtils::InvokeExpectSuccess(
                *this, TEXT("niagara.validate"), MakeValidatePayload(SystemPath, TEXT("basic")), Capture))
        {
            FString Reported;
            TestTrue(TEXT("validate publishes a component-activation verdict"),
                Capture.Result->TryGetStringField(TEXT("componentActivation"), Reported));
            TestEqual(TEXT("an unplaced system reports no_components"), Reported, FString(TEXT("no_components")));
            double Counted = -1.0;
            Capture.Result->TryGetNumberField(TEXT("componentCount"), Counted);
            TestEqual(TEXT("an unplaced system counts no components"), static_cast<int32>(Counted), 0);
            TestEqual(TEXT("an unplaced system raises no activation issue"),
                SeverityOf(Capture.Result, TEXT("NIAGARA_NO_ACTIVE_COMPONENT")), FString());
        }
    }

    // ------------------------------------------------------------------
    // 2. One placed, inactive component. THE REGRESSION.
    // ------------------------------------------------------------------
    FScopedEditorWorldActorGuard Guard;
    UNiagaraComponent* const Component = SpawnPlacedNiagaraProbe(World, System, /*bAutoActivate=*/false);
    if (!Component)
    {
        AddError(TEXT("Failed to spawn the placed Niagara probe component."));
        return false;
    }
    // The premise of the whole case. If registration activated it anyway there is nothing
    // inactive to report and the assertions below would be measuring the wrong thing.
    TestFalse(TEXT("the probe component is inactive"), Component->IsActive());

    {
        FTestResponseCapture Capture;
        if (NiagaraEditTestUtils::InvokeExpectSuccess(
                *this, TEXT("niagara.validate"), MakeValidatePayload(SystemPath, TEXT("basic")), Capture))
        {
            FString Reported;
            Capture.Result->TryGetStringField(TEXT("componentActivation"), Reported);
            TestEqual(TEXT("a placed but inactive system reports none_active"),
                Reported, FString(TEXT("none_active")));
            double Counted = -1.0;
            Capture.Result->TryGetNumberField(TEXT("componentCount"), Counted);
            TestEqual(TEXT("the placed component is counted"), static_cast<int32>(Counted), 1);

            // The block the ticket asks for: whoever reads this must not have to fall back to
            // `object.call_function IsActive` to learn the same thing.
            const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
            if (Capture.Result->TryGetArrayField(TEXT("components"), Components)
                && Components && Components->Num() == 1
                && (*Components)[0].IsValid() && (*Components)[0]->Type == EJson::Object)
            {
                const TSharedPtr<FJsonObject> Entry = (*Components)[0]->AsObject();
                TestFalse(TEXT("the components block reports the component inactive"),
                    Entry->GetBoolField(TEXT("isActive")));
                TestFalse(TEXT("the components block reports bAutoActivate off"),
                    Entry->GetBoolField(TEXT("autoActivate")));
                TestEqual(TEXT("the components block names the component"),
                    Entry->GetStringField(TEXT("component")), Component->GetName());
            }
            else
            {
                AddError(TEXT("validate published no single-entry 'components' block for the placed probe."));
            }

            // What the ticket reported was an empty errors array beside an empty warnings one.
            // At `basic` the honest answer is a warning: a system nothing activates is
            // legitimate when gameplay spawns it, so this must not be a hard error here.
            TestEqual(TEXT("basic validate warns that nothing in the level runs the system"),
                SeverityOf(Capture.Result, TEXT("NIAGARA_NO_ACTIVE_COMPONENT")), FString(TEXT("warning")));
        }
    }

    {
        FTestResponseCapture Capture;
        if (NiagaraEditTestUtils::InvokeExpectSuccess(
                *this, TEXT("niagara.validate"), MakeValidatePayload(SystemPath, TEXT("strict")), Capture))
        {
            TestEqual(TEXT("strict validate errors that nothing in the level runs the system"),
                SeverityOf(Capture.Result, TEXT("NIAGARA_NO_ACTIVE_COMPONENT")), FString(TEXT("error")));
            TestFalse(TEXT("strict validate is not valid while nothing runs the system"),
                Capture.Result->GetBoolField(TEXT("valid")));
        }
    }

    return true;
}

// Defect 2 of the same ticket: the block landed on validate only, so "is anything running
// this" stayed unanswerable from the verb an agent reaches for to ask what an asset IS.
// inspect now publishes the same measured block - and raises nothing off it, because inspect
// reports and validate judges - except under parametersOnly, whose entire purpose is keeping
// a single-parameter readback inline.
//
// Before the fix niagara.inspect had no componentActivation field on any path, so the first
// two assertions of each part fail.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraInspectReportsComponentActivationTest,
    "PinWright.niagara.inspect.PlacedComponentActivationIsReported",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraInspectReportsComponentActivationTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightNiagaraComponentActivationTest;

    UWorld* const World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world is open; inspect's placed-component survey was not asserted"));
        return true;
    }

    FString SystemPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(SystemPath);
    if (!System)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-fixture-system-unavailable"),
            TEXT("NewTransientSystem returned null; inspect's placed-component survey was not asserted"));
        return true;
    }

    // ------------------------------------------------------------------
    // 1. Nothing placed. Same verdict validate gives, from the other verb.
    // ------------------------------------------------------------------
    {
        FTestResponseCapture Capture;
        if (NiagaraEditTestUtils::InvokeExpectSuccess(
                *this, TEXT("niagara.inspect"), MakeInspectPayload(SystemPath, /*bParametersOnly=*/false), Capture))
        {
            FString Reported;
            TestTrue(TEXT("inspect publishes a component-activation verdict"),
                Capture.Result->TryGetStringField(TEXT("componentActivation"), Reported));
            TestEqual(TEXT("an unplaced system reports no_components"), Reported, FString(TEXT("no_components")));
        }
    }

    // ------------------------------------------------------------------
    // 2. One placed, inactive component.
    // ------------------------------------------------------------------
    FScopedEditorWorldActorGuard Guard;
    UNiagaraComponent* const Component = SpawnPlacedNiagaraProbe(World, System, /*bAutoActivate=*/false);
    if (!Component)
    {
        AddError(TEXT("Failed to spawn the placed Niagara probe component."));
        return false;
    }
    TestFalse(TEXT("the probe component is inactive"), Component->IsActive());

    {
        FTestResponseCapture Capture;
        if (NiagaraEditTestUtils::InvokeExpectSuccess(
                *this, TEXT("niagara.inspect"), MakeInspectPayload(SystemPath, /*bParametersOnly=*/false), Capture))
        {
            FString Reported;
            Capture.Result->TryGetStringField(TEXT("componentActivation"), Reported);
            TestEqual(TEXT("a placed but inactive system reports none_active"),
                Reported, FString(TEXT("none_active")));
            double Counted = -1.0;
            Capture.Result->TryGetNumberField(TEXT("componentCount"), Counted);
            TestEqual(TEXT("the placed component is counted"), static_cast<int32>(Counted), 1);

            const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
            if (Capture.Result->TryGetArrayField(TEXT("components"), Components)
                && Components && Components->Num() == 1
                && (*Components)[0].IsValid() && (*Components)[0]->Type == EJson::Object)
            {
                const TSharedPtr<FJsonObject> Entry = (*Components)[0]->AsObject();
                TestFalse(TEXT("the components block reports the component inactive"),
                    Entry->GetBoolField(TEXT("isActive")));
                TestFalse(TEXT("the components block reports bAutoActivate off"),
                    Entry->GetBoolField(TEXT("autoActivate")));
                TestEqual(TEXT("the components block names the component"),
                    Entry->GetStringField(TEXT("component")), Component->GetName());
            }
            else
            {
                AddError(TEXT("inspect published no single-entry 'components' block for the placed probe."));
            }

            // inspect reports; it does not judge. The issue arrays are validate's.
            TestFalse(TEXT("inspect raises no errors of its own"),
                Capture.Result->HasField(TEXT("errors")));
        }
    }

    // ------------------------------------------------------------------
    // 3. parametersOnly stays a projection.
    // ------------------------------------------------------------------
    {
        FTestResponseCapture Capture;
        if (NiagaraEditTestUtils::InvokeExpectSuccess(
                *this, TEXT("niagara.inspect"), MakeInspectPayload(SystemPath, /*bParametersOnly=*/true), Capture))
        {
            TestFalse(TEXT("parametersOnly omits the activation block"),
                Capture.Result->HasField(TEXT("componentActivation")));
            TestFalse(TEXT("parametersOnly omits the component count"),
                Capture.Result->HasField(TEXT("componentCount")));
        }
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
