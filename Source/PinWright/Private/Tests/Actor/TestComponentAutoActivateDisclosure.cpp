// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-niagara-validate-green-while-component-inactive, suggested fix 4:
// a verb that writes bAutoActivate:false onto a level component must say so.
//
// THE DEFECT. On a component whose class defaults bAutoActivate to TRUE, turning it off is an
// override serialised into the .umap, and the component never starts again on any later load
// of that level. Both verbs that can write it - actor.set_component_properties and
// actor.add_component - reported the write as an ordinary applied property name and nothing
// else. Three shipped Niagara systems rendered nothing for a full day after a quiesce step
// wrote exactly this and the map save that followed baked it in; the response that made the
// change said nothing a reader could have acted on.
//
// WHAT IS ASSERTED, in both directions:
//   1. Writing bAutoActivate:false lands (the flag really is off) AND the response's
//      `warnings` array carries an entry naming bAutoActivate and the component.
//   2. Writing bAutoActivate:TRUE discloses NOTHING. This is the calibration case, and it is
//      the one that makes the check mean something: the disclosure is derived from the flag
//      read back off the component, not from the request having named the key, so a restoring
//      write - the remedy the warning itself recommends - must be silent. A disclosure keyed
//      on the request would fire here and train readers to ignore the array.
//   3. actor.add_component and property.set disclose the same way, because a component
//      created - or reflected into - with the flag off is a component that never runs, by the
//      same argument. All three share one text (Utils/AutoActivateDisclosure.h).
//
// Before the fix, assertion 1 and 3's warning checks fail; nothing else moves.
//
// The probe actor is NOT RF_Transient: UEditorActorSubsystem::GetAllLevelActors filters
// transient actors out (EditorActorSubsystem.cpp:386) and every actor.* verb walks it, so a
// transient probe reads as a missing actor. FScopedEditorWorldActorGuard destroys it and
// restores the level's dirty flag on scope exit.
//
// THE PROBE IS A CAMERA ACTOR, and that is load-bearing. bAutoActivate is not a universal CDO
// default of true: UActorComponent never assigns the bitfield and USceneComponent sets it to
// false outright (SceneComponent.cpp:128), so the whole UStaticMeshComponent chain starts with
// the flag already OFF - a write of false there changes nothing, and no assertion downstream
// could tell a landed write from a dropped one. Only classes that opt in start true, among
// them UCameraComponent (CameraComponent.cpp:95), USkeletalMeshComponent, UAudioComponent,
// UParticleSystemComponent and UNiagaraComponent - which is why the field defect was a Niagara
// one. ACameraActor's UCameraComponent is the cheapest of those reachable from the Engine
// module alone: no asset to load, and its PostEditChangeProperty only refreshes the editor
// visual representation. Every test below therefore states its premise before writing, on the
// component for the two that already have one and on the CDO for actor.add_component, whose
// component does not exist until the verb runs.

#include "Misc/AutomationTest.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace PinWrightAutoActivateDisclosureTest
{
    // Trailing 'X' so FActorLabelUtilities::SplitActorLabel cannot strip a numeric tail and
    // uniquify the label away from what the test asks for.
    FString MakeProbeLabel()
    {
        return FString::Printf(TEXT("PWAutoAct_%sX"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    ACameraActor* SpawnProbeActor(UWorld* World, const FString& Label)
    {
        if (!World)
        {
            return nullptr;
        }
        ACameraActor* Actor = World->SpawnActor<ACameraActor>(
            ACameraActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
        if (!Actor)
        {
            return nullptr;
        }
        Actor->SetActorLabel(Label);
        return Actor;
    }

    TSharedPtr<FJsonObject> MakeAutoActivatePayload(const FString& ActorLabel,
                                                    const FString& ComponentName,
                                                    bool bAutoActivate)
    {
        TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
        Properties->SetBoolField(TEXT("bAutoActivate"), bAutoActivate);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), ActorLabel);
        Payload->SetStringField(TEXT("componentName"), ComponentName);
        Payload->SetObjectField(TEXT("properties"), Properties);
        return Payload;
    }

    // The disclosure is free text, so membership is by substring rather than by equality -
    // JsonStringArrayContains would only match a whole entry.
    bool WarningsMention(const TSharedPtr<FJsonObject>& Result, const FString& Needle)
    {
        const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("warnings"), Warnings) || !Warnings)
        {
            return false;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Warnings)
        {
            FString Text;
            if (Value.IsValid() && Value->TryGetString(Text) && Text.Contains(Needle))
            {
                return true;
            }
        }
        return false;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSetComponentPropertiesDisclosesAutoActivateOffTest,
    "PinWright.actor.set_component_properties.AutoActivateDisableIsDisclosed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSetComponentPropertiesDisclosesAutoActivateOffTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightAutoActivateDisclosureTest;

    UWorld* const World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world is open; the bAutoActivate disclosure was not asserted"));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    const FString Label = MakeProbeLabel();
    ACameraActor* const Probe = SpawnProbeActor(World, Label);
    if (!Probe)
    {
        AddError(TEXT("Failed to spawn the probe actor."));
        return false;
    }
    UCameraComponent* const Component = Probe->GetCameraComponent();
    const FString ComponentName = Component->GetName();

    // The premise: the flag starts at its CDO default of true, so the write below is the
    // override that gets serialised, and the disclosure is about a state the probe did not
    // start in. A component type that already starts false would make this test vacuous.
    TestTrue(TEXT("the probe starts auto-activating"), Component->bAutoActivate != 0);

    // ------------------------------------------------------------------
    // 1. Turning it off must be disclosed.
    // ------------------------------------------------------------------
    {
        FTestResponseCapture Capture;
        TestTrue(TEXT("actor.set_component_properties is registered"),
            InvokeHandlerWithCapture(TEXT("actor.set_component_properties"),
                MakeAutoActivatePayload(Label, ComponentName, /*bAutoActivate=*/false), Capture));
        TestTrue(TEXT("the write succeeds"), Capture.bSuccess);
        TestTrue(TEXT("bAutoActivate is reported applied"),
            JsonStringArrayContains(Capture.Result, TEXT("applied"), TEXT("bAutoActivate")));

        // What the disclosure claims must be true of the component, or the response is
        // describing a write that did not happen.
        TestTrue(TEXT("the component really stopped auto-activating"), Component->bAutoActivate == 0);

        TestTrue(TEXT("the response discloses the bAutoActivate override"),
            WarningsMention(Capture.Result, TEXT("bAutoActivate")));
        TestTrue(TEXT("the disclosure names the component it applies to"),
            WarningsMention(Capture.Result, ComponentName));
    }

    // ------------------------------------------------------------------
    // 2. Turning it back on must disclose nothing. THE CALIBRATION CASE.
    // ------------------------------------------------------------------
    {
        FTestResponseCapture Capture;
        TestTrue(TEXT("actor.set_component_properties is registered"),
            InvokeHandlerWithCapture(TEXT("actor.set_component_properties"),
                MakeAutoActivatePayload(Label, ComponentName, /*bAutoActivate=*/true), Capture));
        TestTrue(TEXT("the restoring write succeeds"), Capture.bSuccess);
        TestTrue(TEXT("the component auto-activates again"), Component->bAutoActivate != 0);
        TestFalse(TEXT("restoring bAutoActivate discloses nothing"),
            WarningsMention(Capture.Result, TEXT("bAutoActivate")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAddComponentDisclosesAutoActivateOffTest,
    "PinWright.actor.add_component.AutoActivateDisableIsDisclosed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAddComponentDisclosesAutoActivateOffTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightAutoActivateDisclosureTest;

    UWorld* const World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world is open; the bAutoActivate disclosure was not asserted"));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    const FString Label = MakeProbeLabel();
    ACameraActor* const Probe = SpawnProbeActor(World, Label);
    if (!Probe)
    {
        AddError(TEXT("Failed to spawn the probe actor."));
        return false;
    }

    // The same premise as the other two, taken off the CDO because this verb's component does
    // not exist until the call runs. Without it the test is vacuous: the previous fixture asked
    // for a plain SceneComponent, whose CDO already has the flag off, so bAutoActivate:false was
    // a write of the value the component would have had anyway and the assertions below held
    // whether or not the property was ever applied.
    TestTrue(TEXT("the probe component type starts auto-activating"),
        GetDefault<UCameraComponent>()->bAutoActivate != 0);

    TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetBoolField(TEXT("bAutoActivate"), false);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), Label);
    Payload->SetStringField(TEXT("componentType"), TEXT("CameraComponent"));
    Payload->SetStringField(TEXT("componentName"), TEXT("PWAutoActivateProbeComponent"));
    Payload->SetObjectField(TEXT("properties"), Properties);

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.add_component is registered"),
        InvokeHandlerWithCapture(TEXT("actor.add_component"), Payload, Capture));
    TestTrue(TEXT("the component is added"), Capture.bSuccess);

    UActorComponent* const Added = FindActorComponentByName(Probe, TEXT("PWAutoActivateProbeComponent"));
    if (!Added)
    {
        AddError(TEXT("actor.add_component reported success but the component is not on the actor."));
        return false;
    }
    TestTrue(TEXT("the new component really does not auto-activate"), Added->bAutoActivate == 0);
    TestTrue(TEXT("the response discloses the bAutoActivate override"),
        WarningsMention(Capture.Result, TEXT("bAutoActivate")));

    return true;
}

// property.set is the third verb that can write the flag - generic reflection onto any
// UObject, including a placed component addressed by its object path - so it is the third
// that can disable shipped content without saying so.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FPropertySetDisclosesAutoActivateOffTest,
    "PinWright.property.set.AutoActivateDisableIsDisclosed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertySetDisclosesAutoActivateOffTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightAutoActivateDisclosureTest;

    UWorld* const World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world is open; the bAutoActivate disclosure was not asserted"));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    ACameraActor* const Probe = SpawnProbeActor(World, MakeProbeLabel());
    if (!Probe)
    {
        AddError(TEXT("Failed to spawn the probe actor."));
        return false;
    }
    UCameraComponent* const Component = Probe->GetCameraComponent();
    TestTrue(TEXT("the probe starts auto-activating"), Component->bAutoActivate != 0);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("objectPath"), Component->GetPathName());
    Payload->SetStringField(TEXT("propertyName"), TEXT("bAutoActivate"));
    Payload->SetBoolField(TEXT("value"), false);
    Payload->SetBoolField(TEXT("markDirty"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("property.set is registered"),
        InvokeHandlerWithCapture(TEXT("property.set"), Payload, Capture));
    TestTrue(TEXT("the write succeeds"), Capture.bSuccess);
    TestTrue(TEXT("the component really stopped auto-activating"), Component->bAutoActivate == 0);
    TestTrue(TEXT("the response discloses the bAutoActivate override"),
        WarningsMention(Capture.Result, TEXT("bAutoActivate")));

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
