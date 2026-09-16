// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-ui-remove-wrong-instance.
//
// ui.remove_widget_from_viewport used to walk every UUserWidget in the process
// and remove the first matching short name, including an editor-world decoy.
// This test creates same-named instances in the editor and PIE worlds, invokes
// the production handler through the capture harness, and asserts that the
// runtime instance is the one detached and identified in the response.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "WidgetBlueprint.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiRemoveWidgetFromViewportTargetsRuntimeInstanceTest,
    "PinWright.ui.remove_widget_from_viewport.TargetsRuntimeInstance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUiRemoveWidgetFromViewportTargetsRuntimeInstanceTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !GEngine || !GEngine->GameViewport)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-runtime-viewport"),
            TEXT("the handler requires an active PIE game viewport"));
        return true;
    }

    UWorld* RuntimeWorld = GEngine->GameViewport->GetWorld();
    UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
    if (!RuntimeWorld || !EditorWorld || GEditor->PlayWorld != RuntimeWorld || EditorWorld == RuntimeWorld)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-separate-pie-world"),
            TEXT("the test needs distinct editor and PIE worlds"));
        return true;
    }

    const FString WidgetName = FString::Printf(TEXT("PW_RemoveWidgetProbe_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackagePath = FString::Printf(TEXT("/Game/_Test/WBP_RemoveWidgetProbe_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    // The compiled fixture Blueprint is RF_Standalone, so the periodic suite GC keeps it and its
    // generated class alive. Runs after the widget-instance owners are released below.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackagePath);
    };

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

    WidgetBP->WidgetTree->RootWidget = WidgetBP->WidgetTree->ConstructWidget<UTextBlock>(
        UTextBlock::StaticClass(), FName(TEXT("Root")));
    FKismetEditorUtilities::CompileBlueprint(WidgetBP);
    if (!WidgetBP->GeneratedClass)
    {
        AddError(TEXT("WidgetBlueprint compiled without a generated class"));
        return true;
    }

    const TSubclassOf<UUserWidget> WidgetClass(WidgetBP->GeneratedClass);
    UUserWidget* EditorDecoy = UUserWidget::CreateWidgetInstance(
        *EditorWorld, WidgetClass, FName(*WidgetName));
    UUserWidget* RuntimeWidget = UUserWidget::CreateWidgetInstance(
        *RuntimeWorld, WidgetClass, FName(*WidgetName));
    TStrongObjectPtr<UUserWidget> EditorDecoyOwner(EditorDecoy);
    TStrongObjectPtr<UUserWidget> RuntimeWidgetOwner(RuntimeWidget);
    if (!EditorDecoy || !RuntimeWidget)
    {
        AddError(TEXT("failed to create same-named editor and runtime widget instances"));
        return true;
    }

    if (!RuntimeWidget->GetOwningPlayer())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-runtime-player"),
            TEXT("CreateWidgetInstance did not resolve a PIE owning player"));
        return true;
    }

    // A second runtime widget uses a distinct UWidgetTree outer, then adopts the
    // same local player. Its requested object name stays identical while its
    // object path remains unique, which makes the ambiguity refusal observable.
    UWidgetTree* SecondaryTree = NewObject<UWidgetTree>(RuntimeWorld, NAME_None, RF_Transient);
    UUserWidget* SecondaryWidget = SecondaryTree
        ? UUserWidget::CreateWidgetInstance(*SecondaryTree, WidgetClass, FName(*WidgetName))
        : nullptr;
    TStrongObjectPtr<UWidgetTree> SecondaryTreeOwner(SecondaryTree);
    TStrongObjectPtr<UUserWidget> SecondaryWidgetOwner(SecondaryWidget);
    if (!SecondaryTree || !SecondaryWidget)
    {
        AddError(TEXT("failed to create a second same-named runtime widget instance"));
        return true;
    }
    SecondaryWidget->SetOwningPlayer(RuntimeWidget->GetOwningPlayer());
    if (!SecondaryWidget->GetOwningPlayer())
    {
        AddError(TEXT("failed to assign the second runtime widget's owning player"));
        return true;
    }

    RuntimeWidget->AddToViewport();
    TestTrue(TEXT("runtime probe widget is attached before removal"), RuntimeWidget->IsInViewport());
    TestEqual(TEXT("editor decoy and runtime target retain the requested short name"),
        EditorDecoy->GetName(), RuntimeWidget->GetName());
    TestEqual(TEXT("second runtime widget retains the requested short name"),
        SecondaryWidget->GetName(), RuntimeWidget->GetName());
    TestTrue(TEXT("editor decoy belongs to a different world"), EditorDecoy->GetWorld() == EditorWorld);
    TestTrue(TEXT("runtime target belongs to the viewport world"), RuntimeWidget->GetWorld() == RuntimeWorld);
    TestTrue(TEXT("second runtime widget belongs to the viewport world"), SecondaryWidget->GetWorld() == RuntimeWorld);
    TestTrue(TEXT("same-named runtime widgets have distinct object paths"),
        RuntimeWidget->GetPathName() != SecondaryWidget->GetPathName());

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("key"), WidgetName);
    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("ui.remove_widget_from_viewport"), Payload, Capture);

    if (TestTrue(TEXT("ui.remove_widget_from_viewport handler registered"), bFound)
        && TestTrue(TEXT("handler sent a response"), Capture.bWasCalled))
    {
        TestTrue(TEXT("named removal succeeds for the runtime target"), Capture.bSuccess);
        TestFalse(TEXT("runtime target is no longer in the viewport"), RuntimeWidget->IsInViewport());
        TestFalse(TEXT("editor-world decoy remains unattached"), EditorDecoy->IsInViewport());

        if (Capture.Result.IsValid())
        {
            FString ObjectPath;
            FString WorldPath;
            FString PlayerPath;
            TestTrue(TEXT("response includes the removed object path"),
                Capture.Result->TryGetStringField(TEXT("objectPath"), ObjectPath));
            TestTrue(TEXT("response includes the owning world path"),
                Capture.Result->TryGetStringField(TEXT("worldPath"), WorldPath));
            TestTrue(TEXT("response includes the owning player path"),
                Capture.Result->TryGetStringField(TEXT("playerPath"), PlayerPath));
            TestEqual(TEXT("response identifies the runtime object"), ObjectPath, RuntimeWidget->GetPathName());
            TestEqual(TEXT("response identifies the viewport world"), WorldPath, RuntimeWorld->GetPathName());
            TestEqual(TEXT("response identifies the runtime player"),
                PlayerPath, RuntimeWidget->GetOwningPlayer()->GetPathName());
        }
        else
        {
            AddError(TEXT("success response carried no result object"));
        }
    }

    // Same-runtime short-name collision: refuse without removing either
    // candidate, and expose both paths so the caller can disambiguate.
    RuntimeWidget->AddToViewport();
    SecondaryWidget->AddToViewport();
    TestTrue(TEXT("first runtime candidate is attached for ambiguity case"), RuntimeWidget->IsInViewport());
    TestTrue(TEXT("second runtime candidate is attached for ambiguity case"), SecondaryWidget->IsInViewport());

    TSharedPtr<FJsonObject> AmbiguousPayload = MakeShared<FJsonObject>();
    AmbiguousPayload->SetStringField(TEXT("key"), WidgetName);
    FTestResponseCapture AmbiguousCapture;
    const bool bAmbiguousFound = InvokeHandlerWithCapture(
        TEXT("ui.remove_widget_from_viewport"), AmbiguousPayload, AmbiguousCapture);

    if (TestTrue(TEXT("ambiguity case found the handler"), bAmbiguousFound)
        && TestTrue(TEXT("ambiguity case sent a response"), AmbiguousCapture.bWasCalled))
    {
        TestFalse(TEXT("ambiguous short name is refused"), AmbiguousCapture.bSuccess);
        TestEqual(TEXT("ambiguous short name returns AMBIGUOUS_ACTOR_NAME"),
            AmbiguousCapture.ErrorCode, FString(TEXT("AMBIGUOUS_ACTOR_NAME")));
        if (AmbiguousCapture.Result.IsValid())
        {
            int32 CandidateCount = 0;
            TestTrue(TEXT("ambiguity response includes candidateCount"),
                AmbiguousCapture.Result->TryGetNumberField(TEXT("candidateCount"), CandidateCount));
            TestEqual(TEXT("ambiguity response lists both runtime candidates"), CandidateCount, 2);
            TestTrue(TEXT("ambiguity response includes the first object path"),
                JsonArrayHasObjectWithStringField(AmbiguousCapture.Result, TEXT("candidates"),
                    TEXT("objectPath"), RuntimeWidget->GetPathName()));
            TestTrue(TEXT("ambiguity response includes the second object path"),
                JsonArrayHasObjectWithStringField(AmbiguousCapture.Result, TEXT("candidates"),
                    TEXT("objectPath"), SecondaryWidget->GetPathName()));
        }
        else
        {
            AddError(TEXT("ambiguity response carried no candidate result object"));
        }
    }
    TestTrue(TEXT("ambiguous request leaves the first runtime widget attached"), RuntimeWidget->IsInViewport());
    TestTrue(TEXT("ambiguous request leaves the second runtime widget attached"), SecondaryWidget->IsInViewport());

    // Exact object-path addressing selects only the requested candidate.
    TSharedPtr<FJsonObject> PathPayload = MakeShared<FJsonObject>();
    PathPayload->SetStringField(TEXT("key"), RuntimeWidget->GetPathName());
    FTestResponseCapture PathCapture;
    const bool bPathFound = InvokeHandlerWithCapture(
        TEXT("ui.remove_widget_from_viewport"), PathPayload, PathCapture);
    if (TestTrue(TEXT("exact-path case found the handler"), bPathFound)
        && TestTrue(TEXT("exact-path case sent a response"), PathCapture.bWasCalled))
    {
        TestTrue(TEXT("exact object path removes the requested runtime widget"), PathCapture.bSuccess);
        TestFalse(TEXT("exact-path target is no longer in the viewport"), RuntimeWidget->IsInViewport());
        TestTrue(TEXT("exact-path removal leaves the other runtime widget attached"), SecondaryWidget->IsInViewport());
        if (PathCapture.Result.IsValid())
        {
            FString RemovedPath;
            TestTrue(TEXT("exact-path response includes objectPath"),
                PathCapture.Result->TryGetStringField(TEXT("objectPath"), RemovedPath));
            TestEqual(TEXT("exact-path response identifies the requested candidate"),
                RemovedPath, RuntimeWidget->GetPathName());
        }
        else
        {
            AddError(TEXT("exact-path success response carried no result object"));
        }
    }

    RuntimeWidget->RemoveFromParent();
    SecondaryWidget->RemoveFromParent();
    // Release in the same order as the former explicit root teardown, after
    // detaching the viewport widget and before RunTest returns.
    EditorDecoyOwner.Reset();
    RuntimeWidgetOwner.Reset();
    SecondaryWidgetOwner.Reset();
    SecondaryTreeOwner.Reset();
    return true;
}
