// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the two component-side runtime Niagara parameter setters:
//   effect.set_niagara_parameter (Handlers/VFX/EffectHandler.cpp)
//   niagara.modify_parameter     (Handlers/Niagara/NiagaraHandler.cpp)
//
// Both forward to UNiagaraComponent::SetVariable*/SetFloatParameter/..., which call
// OverrideParameters.SetParameterValue(..., bAdd=true) — silently CREATING an entry for
// any unknown name instead of rejecting it. Before the fix these handlers reported
// {success:true, applied:true} for a typo'd / wrong-prefix / wrong-type parameter name,
// so an agent could not distinguish a real tune from a complete no-op.
//
// The fix routes both handlers through PinWrightNiagara::ClassifyComponentParameter,
// which validates the name against the component's system exposed (User.) parameter store
// and makes the handler return PARAMETER_NOT_FOUND for an unknown name. These tests would
// fail if either handler's validation were reverted (the bogus-name calls would succeed).
//
// The test drives the production dispatcher path (InvokeHandlerWithCapture) against a real
// ANiagaraActor spawned into the editor world — it does not reimplement the handler logic.
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

#include "NiagaraActor.h"
#include "NiagaraComponent.h"
#include "NiagaraParameterStore.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"

namespace
{
    // Spawns an ANiagaraActor into the editor world, assigns System to its component, and
    // labels it. Returns the spawned actor (cleaned up by the caller's world guard) or null.
    ANiagaraActor* SpawnLabeledNiagaraActor(UNiagaraSystem* System, const FString& Label)
    {
        if (!GEditor || !System)
        {
            return nullptr;
        }
        UWorld* World = GEditor->GetEditorWorldContext().World();
        if (!World)
        {
            return nullptr;
        }
        ANiagaraActor* Actor = World->SpawnActor<ANiagaraActor>(
            FVector::ZeroVector, FRotator::ZeroRotator);
        if (!Actor)
        {
            return nullptr;
        }
        if (UNiagaraComponent* Comp = Actor->GetNiagaraComponent())
        {
            Comp->SetAsset(System);
        }
        Actor->SetActorLabel(Label);
        return Actor;
    }
}

// ----------------------------------------------------------------------------
// effect.set_niagara_parameter must reject a name that doesn't exist on the system.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEffectSetNiagaraParameterValidatesNameTest,
    "PinWright.effect.set_niagara_parameter.RejectsUnknownParameter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEffectSetNiagaraParameterValidatesNameTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("effect.set_niagara_parameter is registered"),
        IsRegistered(TEXT("effect.set_niagara_parameter")));

    FString SystemObjectPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(SystemObjectPath);
    if (!TestNotNull(TEXT("Transient UNiagaraSystem constructed"), System))
    {
        return false;
    }
    ON_SCOPE_EXIT { if (System) { System->RemoveFromRoot(); } };

    // Seed one real user parameter so the "valid name still works" leg has something to hit.
    const FNiagaraVariable RealParam(FNiagaraTypeDefinition::GetFloatDef(), FName(TEXT("User.SpawnRate")));
    System->GetExposedParameters().AddParameter(RealParam);

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = NiagaraEditTestUtils::MakeAssetName(TEXT("NiagaraRuntimeFixture"));
    ANiagaraActor* Actor = SpawnLabeledNiagaraActor(System, Label);
    if (!TestNotNull(TEXT("Spawned ANiagaraActor in editor world"), Actor))
    {
        return false;
    }

    // Bogus name -> must be rejected, NOT reported applied:true (the regression).
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("systemName"), Label);
        Payload->SetStringField(TEXT("parameterName"), TEXT("User.ThisParameterDefinitelyDoesNotExist"));
        Payload->SetStringField(TEXT("parameterType"), TEXT("Float"));
        Payload->SetNumberField(TEXT("value"), 999.0);

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler invoked (bogus name)"),
            InvokeHandlerWithCapture(TEXT("effect.set_niagara_parameter"), Payload, Capture));
        TestTrue(TEXT("handler responded (bogus name)"), Capture.bWasCalled);
        TestFalse(TEXT("bogus parameter name must NOT report success"), Capture.bSuccess);
        TestEqual(TEXT("bogus name returns PARAMETER_NOT_FOUND"),
            Capture.ErrorCode, FString(TEXT("PARAMETER_NOT_FOUND")));
    }

    // No-prefix bogus name with Color type -> also rejected.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("systemName"), Label);
        Payload->SetStringField(TEXT("parameterName"), TEXT("CompletelyBogusNoPrefix"));
        Payload->SetStringField(TEXT("parameterType"), TEXT("Color"));
        TArray<TSharedPtr<FJsonValue>> Color;
        Color.Add(MakeShared<FJsonValueNumber>(1.0));
        Color.Add(MakeShared<FJsonValueNumber>(0.0));
        Color.Add(MakeShared<FJsonValueNumber>(0.0));
        Payload->SetArrayField(TEXT("value"), Color);

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler invoked (no-prefix bogus name)"),
            InvokeHandlerWithCapture(TEXT("effect.set_niagara_parameter"), Payload, Capture));
        TestFalse(TEXT("no-prefix bogus name must NOT report success"), Capture.bSuccess);
        TestEqual(TEXT("no-prefix bogus name returns PARAMETER_NOT_FOUND"),
            Capture.ErrorCode, FString(TEXT("PARAMETER_NOT_FOUND")));
    }

    // Real name -> still succeeds (the fix must not break the happy path).
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("systemName"), Label);
        Payload->SetStringField(TEXT("parameterName"), TEXT("User.SpawnRate"));
        Payload->SetStringField(TEXT("parameterType"), TEXT("Float"));
        Payload->SetNumberField(TEXT("value"), 200.0);

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler invoked (real name)"),
            InvokeHandlerWithCapture(TEXT("effect.set_niagara_parameter"), Payload, Capture));
        TestTrue(TEXT("real parameter name reports success"), Capture.bSuccess);
        if (Capture.Result.IsValid())
        {
            bool bApplied = false;
            TestTrue(TEXT("applied field present (real name)"),
                Capture.Result->TryGetBoolField(TEXT("applied"), bApplied));
            TestTrue(TEXT("real parameter applied:true"), bApplied);
        }
    }

    return true;
}

// ----------------------------------------------------------------------------
// niagara.modify_parameter — the twin component-side setter — must reject likewise.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraModifyParameterValidatesNameTest,
    "PinWright.niagara.modify_parameter.RejectsUnknownParameter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraModifyParameterValidatesNameTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("niagara.modify_parameter is registered"),
        IsRegistered(TEXT("niagara.modify_parameter")));

    FString SystemObjectPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(SystemObjectPath);
    if (!TestNotNull(TEXT("Transient UNiagaraSystem constructed"), System))
    {
        return false;
    }
    ON_SCOPE_EXIT { if (System) { System->RemoveFromRoot(); } };

    const FNiagaraVariable RealParam(FNiagaraTypeDefinition::GetFloatDef(), FName(TEXT("User.SpawnRate")));
    System->GetExposedParameters().AddParameter(RealParam);

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = NiagaraEditTestUtils::MakeAssetName(TEXT("NiagaraModifyFixture"));
    ANiagaraActor* Actor = SpawnLabeledNiagaraActor(System, Label);
    if (!TestNotNull(TEXT("Spawned ANiagaraActor in editor world"), Actor))
    {
        return false;
    }

    // Bogus name -> must be rejected, NOT reported success:true (the regression).
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), Label);
        Payload->SetStringField(TEXT("parameterName"), TEXT("User.DoesNotExistEither"));
        Payload->SetStringField(TEXT("parameterType"), TEXT("Float"));
        Payload->SetNumberField(TEXT("value"), 42.0);

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler invoked (bogus name)"),
            InvokeHandlerWithCapture(TEXT("niagara.modify_parameter"), Payload, Capture));
        TestTrue(TEXT("handler responded (bogus name)"), Capture.bWasCalled);
        TestFalse(TEXT("bogus parameter name must NOT report success"), Capture.bSuccess);
        TestEqual(TEXT("bogus name returns PARAMETER_NOT_FOUND"),
            Capture.ErrorCode, FString(TEXT("PARAMETER_NOT_FOUND")));
    }

    // Real name -> still succeeds.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), Label);
        Payload->SetStringField(TEXT("parameterName"), TEXT("User.SpawnRate"));
        Payload->SetStringField(TEXT("parameterType"), TEXT("Float"));
        Payload->SetNumberField(TEXT("value"), 200.0);

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler invoked (real name)"),
            InvokeHandlerWithCapture(TEXT("niagara.modify_parameter"), Payload, Capture));
        TestTrue(TEXT("real parameter name reports success"), Capture.bSuccess);
    }

    return true;
}

// ----------------------------------------------------------------------------
// niagara.modify_parameter must echo the override it wrote, read back off the live
// component's OverrideParameters store, so a set-then-verify loop self-confirms from
// the write result alone (niagara.inspect reads asset defaults, not this store). This
// test would fail if the value-echo were reverted: the result would carry no `value`
// and overrideStored would be false.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraModifyParameterEchoesWrittenValueTest,
    "PinWright.niagara.modify_parameter.EchoesWrittenValue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraModifyParameterEchoesWrittenValueTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("niagara.modify_parameter is registered"),
        IsRegistered(TEXT("niagara.modify_parameter")));

    FString SystemObjectPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(SystemObjectPath);
    if (!TestNotNull(TEXT("Transient UNiagaraSystem constructed"), System))
    {
        return false;
    }
    ON_SCOPE_EXIT { if (System) { System->RemoveFromRoot(); } };

    // Seed Float and Color user parameters so both the scalar and structured-readback legs hit.
    System->GetExposedParameters().AddParameter(
        FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), FName(TEXT("User.SpawnRate"))));
    System->GetExposedParameters().AddParameter(
        FNiagaraVariable(FNiagaraTypeDefinition::GetColorDef(), FName(TEXT("User.TintColor"))));

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = NiagaraEditTestUtils::MakeAssetName(TEXT("NiagaraEchoFixture"));
    ANiagaraActor* Actor = SpawnLabeledNiagaraActor(System, Label);
    if (!TestNotNull(TEXT("Spawned ANiagaraActor in editor world"), Actor))
    {
        return false;
    }

    // Float override -> result echoes value=137.5 read back off the component.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), Label);
        Payload->SetStringField(TEXT("parameterName"), TEXT("User.SpawnRate"));
        Payload->SetStringField(TEXT("parameterType"), TEXT("Float"));
        Payload->SetNumberField(TEXT("value"), 137.5);

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler invoked (float echo)"),
            InvokeHandlerWithCapture(TEXT("niagara.modify_parameter"), Payload, Capture));
        TestTrue(TEXT("float override reports success"), Capture.bSuccess);
        if (TestTrue(TEXT("float result present"), Capture.Result.IsValid()))
        {
            bool bOverrideStored = false;
            TestTrue(TEXT("overrideStored field present (float)"),
                Capture.Result->TryGetBoolField(TEXT("overrideStored"), bOverrideStored));
            TestTrue(TEXT("float override read back as stored"), bOverrideStored);

            double EchoedValue = 0.0;
            TestTrue(TEXT("result echoes a numeric value (float) — absent if echo reverted"),
                Capture.Result->TryGetNumberField(TEXT("value"), EchoedValue));
            TestEqual(TEXT("echoed float value matches the override written"), EchoedValue, 137.5);
        }
    }

    // Color override -> result echoes a {r,g,b,a} object read back off the component.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), Label);
        Payload->SetStringField(TEXT("parameterName"), TEXT("User.TintColor"));
        Payload->SetStringField(TEXT("parameterType"), TEXT("Color"));
        TSharedPtr<FJsonObject> Color = MakeShared<FJsonObject>();
        Color->SetNumberField(TEXT("r"), 0.25);
        Color->SetNumberField(TEXT("g"), 0.5);
        Color->SetNumberField(TEXT("b"), 0.75);
        Color->SetNumberField(TEXT("a"), 1.0);
        Payload->SetObjectField(TEXT("value"), Color);

        FTestResponseCapture Capture;
        TestTrue(TEXT("handler invoked (color echo)"),
            InvokeHandlerWithCapture(TEXT("niagara.modify_parameter"), Payload, Capture));
        TestTrue(TEXT("color override reports success"), Capture.bSuccess);
        if (TestTrue(TEXT("color result present"), Capture.Result.IsValid()))
        {
            const TSharedPtr<FJsonObject>* EchoedColor = nullptr;
            if (TestTrue(TEXT("result echoes a {r,g,b,a} value (color) — absent if echo reverted"),
                    Capture.Result->TryGetObjectField(TEXT("value"), EchoedColor) && EchoedColor))
            {
                double R = 0.0, G = 0.0, B = 0.0;
                (*EchoedColor)->TryGetNumberField(TEXT("r"), R);
                (*EchoedColor)->TryGetNumberField(TEXT("g"), G);
                (*EchoedColor)->TryGetNumberField(TEXT("b"), B);
                TestEqual(TEXT("echoed color R matches"), R, 0.25);
                TestEqual(TEXT("echoed color G matches"), G, 0.5);
                TestEqual(TEXT("echoed color B matches"), B, 0.75);
            }
        }
    }

    return true;
}
