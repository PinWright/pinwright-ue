// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-set-widget-text-hits-wrong-instance.
//
// ui.set_widget_text used to build its candidate UserWidget list editor-world
// FIRST, then append the PIE viewport world, and break on the first matching
// TextBlock. When a same-class UserWidget also lived in the editor world its
// child intercepted the write, so the call reported success while the painted
// PIE instance stayed empty (silent success-with-no-effect). The fix resolves
// the target world PIE-first via McpActorUtils::ResolveQueryWorld, restricts the
// search to the resolved world, echoes the mutated owning_user_widget, and errors
// (rather than succeeding on a decoy) when no live instance matches.
//
// In a headless automation run no PIE session exists, so ResolveQueryWorld("auto")
// resolves to the editor world. This test spawns a real live UserWidget (carrying
// a named TextBlock) in the editor world, drives the production handler, and
// asserts the write actually landed AND that the response echoes the new
// owning_user_widget field. The echo assertion is the revert detector: the old
// code never set that field, so it fails if the fix is reverted.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiSetWidgetTextHitsResolvedWorldInstanceTest,
    "PinWright.ui.set_widget_text.HitsResolvedWorldInstance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUiSetWidgetTextHitsResolvedWorldInstanceTest::RunTest(const FString& Parameters)
{
    // This test assumes no PIE session (a headless automation run), so the
    // PIE-first resolver falls back to the editor world that we spawn into.
    if (!GEditor)
    {
        AddError(TEXT("GEditor unavailable"));
        return true;
    }
    TestNull(TEXT("test assumes no active PIE world"), GEditor->PlayWorld);

    UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
    if (!EditorWorld)
    {
        AddError(TEXT("no editor world available"));
        return true;
    }

    // Build a live UserWidget with a named TextBlock in the (resolved) editor world.
    // UUserWidget is UCLASS(Abstract), so CreateWidget on the base class returns null
    // (CreateWidgetHelpers::ValidateUserWidgetClass rejects abstract classes). Compile
    // a concrete transient WidgetBlueprint whose WidgetTree carries the named TextBlock,
    // then CreateWidget from its generated class — Initialize() duplicates that archetype
    // tree onto the live instance, so GetWidgetFromName resolves the (duplicated) child.
    const FName TextBlockName(TEXT("InteractionText_RegressionProbe"));

    const FString PackagePath = FString::Printf(TEXT("/Game/_Test/WBP_SetWidgetTextProbe_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        AddError(TEXT("failed to create transient package for the test widget blueprint"));
        return true;
    }
    Package->SetFlags(RF_Transient);

    UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
        UUserWidget::StaticClass(),
        Package,
        FName(*FPackageName::GetLongPackageAssetName(PackagePath)),
        BPTYPE_Normal,
        UWidgetBlueprint::StaticClass(),
        UWidgetBlueprintGeneratedClass::StaticClass()));
    if (!WidgetBP || !WidgetBP->WidgetTree)
    {
        AddError(TEXT("failed to construct a WidgetBlueprint for the test"));
        return true;
    }

    UTextBlock* ArchetypeTextBlock = WidgetBP->WidgetTree->ConstructWidget<UTextBlock>(
        UTextBlock::StaticClass(), TextBlockName);
    WidgetBP->WidgetTree->RootWidget = ArchetypeTextBlock;
    FKismetEditorUtilities::CompileBlueprint(WidgetBP);
    if (!WidgetBP->GeneratedClass)
    {
        AddError(TEXT("WidgetBlueprint compiled without a generated class"));
        return true;
    }

    UUserWidget* Widget = CreateWidget<UUserWidget>(EditorWorld,
        TSubclassOf<UUserWidget>(WidgetBP->GeneratedClass));
    if (!Widget || !Widget->WidgetTree)
    {
        AddError(TEXT("failed to construct a live UserWidget for the test"));
        return true;
    }
    // Keep the widget alive for the duration of the test (CreateWidget does not root it).
    Widget->AddToRoot();

    // The instance's WidgetTree is a duplicate of the archetype, so the live TextBlock
    // is a distinct object resolved by name — this is the object the handler must mutate.
    UTextBlock* TextBlock = Cast<UTextBlock>(Widget->GetWidgetFromName(TextBlockName));
    if (!TextBlock)
    {
        AddError(TEXT("named TextBlock did not resolve on the live UserWidget instance"));
        Widget->RemoveFromRoot();
        return true;
    }

    // Sanity: GetAllWidgetsOfClass (used by the handler) must be able to find it.
    TestTrue(TEXT("named child resolves from the constructed widget"),
        Widget->GetWidgetFromName(TextBlockName) == TextBlock);

    const FString Value(TEXT("SCORE: 12500"));
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("key"), TextBlockName.ToString());
    Payload->SetStringField(TEXT("value"), Value);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("ui.set_widget_text"), Payload, Capture);

    if (TestTrue(TEXT("ui.set_widget_text handler registered"), bFound)
        && TestTrue(TEXT("handler sent a response"), Capture.bWasCalled))
    {
        // The write must succeed against the resolved-world instance...
        TestTrue(TEXT("set_widget_text reports success on the live instance"), Capture.bSuccess);
        // ...the text must actually land on the painted/live TextBlock (proving the
        // handler mutated the instance in the resolved world, not a phantom)...
        TestEqual(TEXT("text landed on the live TextBlock"),
            TextBlock->GetText().ToString(), Value);
        // ...and the response must echo which UserWidget was mutated. This field is
        // new with the fix; the old editor-world-first code never set it, so this
        // assertion fails if the fix is reverted.
        if (Capture.Result.IsValid())
        {
            FString OwningWidget;
            const bool bHasOwner = Capture.Result->TryGetStringField(
                TEXT("owning_user_widget"), OwningWidget);
            TestTrue(TEXT("response echoes owning_user_widget (revert detector)"), bHasOwner);
            TestEqual(TEXT("owning_user_widget names the mutated instance"),
                OwningWidget, Widget->GetName());
        }
        else
        {
            AddError(TEXT("success response carried no result object"));
        }
    }

    // No-match case: an unknown key must error rather than silently succeed.
    {
        TSharedPtr<FJsonObject> MissPayload = MakeShared<FJsonObject>();
        MissPayload->SetStringField(TEXT("key"), TEXT("NoSuchWidget_RegressionProbe"));
        MissPayload->SetStringField(TEXT("value"), TEXT("ignored"));
        FTestResponseCapture MissCapture;
        InvokeHandlerWithCapture(TEXT("ui.set_widget_text"), MissPayload, MissCapture);
        TestTrue(TEXT("missing widget responded"), MissCapture.bWasCalled);
        TestFalse(TEXT("missing widget does not silently succeed"), MissCapture.bSuccess);
        TestEqual(TEXT("missing widget returns WIDGET_NOT_FOUND"),
            MissCapture.ErrorCode, FString(TEXT("WIDGET_NOT_FOUND")));
    }

    Widget->RemoveFromRoot();
    return true;
}
