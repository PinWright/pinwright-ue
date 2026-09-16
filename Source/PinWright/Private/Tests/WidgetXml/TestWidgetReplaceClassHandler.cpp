// Copyright (c) 2026 Alexander Penkin. MIT License.

// Counterfactual (root case): if `WidgetTree->RootWidget = NewWidget` is
// reverted in the root branch, RootWidget still points at the old CanvasPanel
// and the IsA<UOverlay>() assertion in Test 2 fails.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"

#include "Handlers/UI/WidgetAuthoringUtils.h"

#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"

#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/VerticalBox.h"
#include "Components/Overlay.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Components/PanelWidget.h"
#include "UObject/Package.h"

// ============================================================================
// 0. Registration
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetReplaceClass_Registered,
    "PinWright.widget.replace_class.Registered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetReplaceClass_Registered::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("widget.replace_class is registered"),
        IsHandlerRegistered(TEXT("widget.replace_class")));
    return true;
}


namespace
{
    // Build a transient UWidgetBlueprint with an empty WidgetTree ready for
    // the tests to populate.
    UWidgetBlueprint* MakeTransientWidgetBlueprint()
    {
        UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
            GetTransientPackage(), NAME_None, RF_Transient);
        if (!WBP)
        {
            return nullptr;
        }
        WBP->WidgetTree = NewObject<UWidgetTree>(WBP, NAME_None, RF_Transient);
        return WBP;
    }
}

// ============================================================================
// 1. Non-root swap preserves children and parent slot layout
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetReplaceClass_NonRoot_PreservesChildrenAndParentSlot,
    "PinWright.widget.replace_class.NonRootPreservesChildrenAndParentSlot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetReplaceClass_NonRoot_PreservesChildrenAndParentSlot::RunTest(const FString& Parameters)
{
    UWidgetBlueprint* WBP = MakeTransientWidgetBlueprint();
    TestNotNull(TEXT("widget blueprint allocated"), WBP);
    if (!WBP) return false;

    UWidgetTree* Tree = WBP->WidgetTree;
    TestNotNull(TEXT("widget tree allocated"), Tree);
    if (!Tree) return false;

    // RootCanvas (CanvasPanel)
    //   H (HorizontalBox) — positioned at (50, 75)
    //     A (TextBlock)
    //     B (TextBlock)
    UCanvasPanel* RootCanvas = Tree->ConstructWidget<UCanvasPanel>(
        UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
    Tree->RootWidget = RootCanvas;

    UHorizontalBox* H = Tree->ConstructWidget<UHorizontalBox>(
        UHorizontalBox::StaticClass(), TEXT("H"));
    UPanelSlot* HSlot = RootCanvas->AddChild(H);
    UCanvasPanelSlot* HCanvasSlot = Cast<UCanvasPanelSlot>(HSlot);
    TestNotNull(TEXT("H is placed in a CanvasPanelSlot"), HCanvasSlot);
    if (!HCanvasSlot) return false;
    HCanvasSlot->SetPosition(FVector2D(50.0f, 75.0f));

    UTextBlock* A = Tree->ConstructWidget<UTextBlock>(
        UTextBlock::StaticClass(), TEXT("A"));
    UTextBlock* B = Tree->ConstructWidget<UTextBlock>(
        UTextBlock::StaticClass(), TEXT("B"));
    H->AddChild(A);
    H->AddChild(B);

    // Sanity: tree built as expected.
    TestEqual(TEXT("H has 2 children before swap"), H->GetChildrenCount(), 2);

    // --- Swap H from HorizontalBox to VerticalBox ---
    bool bWasRoot = true;
    FString Error;
    UWidget* NewH = WidgetAuthoringHelpers::ReplaceWidgetClass(
        WBP, H, UVerticalBox::StaticClass(),
        /*bPreserveProperties=*/true,
        bWasRoot, &Error);

    TestNotNull(TEXT("ReplaceWidgetClass returned a new widget"), NewH);
    TestEqual(TEXT("no error"), Error, FString());
    TestFalse(TEXT("H is not root"), bWasRoot);
    if (!NewH) return false;

    // Assertions
    UWidget* Found = Tree->FindWidget(FName(TEXT("H")));
    TestNotNull(TEXT("H still resolvable by name"), Found);
    TestTrue(TEXT("H is now a VerticalBox"), Found && Found->IsA<UVerticalBox>());

    UVerticalBox* VBox = Cast<UVerticalBox>(Found);
    if (!VBox) return false;

    TestEqual(TEXT("new H has 2 children"), VBox->GetChildrenCount(), 2);
    UWidget* Child0 = VBox->GetChildAt(0);
    UWidget* Child1 = VBox->GetChildAt(1);
    TestTrue(TEXT("first child is TextBlock named A"),
        Child0 && Child0->IsA<UTextBlock>() && Child0->GetFName() == FName(TEXT("A")));
    TestTrue(TEXT("second child is TextBlock named B"),
        Child1 && Child1->IsA<UTextBlock>() && Child1->GetFName() == FName(TEXT("B")));

    // Parent slot preserved.
    TestTrue(TEXT("new H parent is RootCanvas"),
        VBox->GetParent() == static_cast<UPanelWidget*>(RootCanvas));
    UCanvasPanelSlot* NewCanvasSlot = Cast<UCanvasPanelSlot>(VBox->Slot);
    TestNotNull(TEXT("new H slot is a CanvasPanelSlot"), NewCanvasSlot);
    if (NewCanvasSlot)
    {
        const FVector2D Pos = NewCanvasSlot->GetPosition();
        TestEqual(TEXT("slot X preserved"), Pos.X, 50.0);
        TestEqual(TEXT("slot Y preserved"), Pos.Y, 75.0);
    }

    return true;
}

// ============================================================================
// 2. Root swap reassigns WidgetTree->RootWidget
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetReplaceClass_Root_ReassignsRootWidget,
    "PinWright.widget.replace_class.RootReassignsRootWidget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetReplaceClass_Root_ReassignsRootWidget::RunTest(const FString& Parameters)
{
    UWidgetBlueprint* WBP = MakeTransientWidgetBlueprint();
    TestNotNull(TEXT("widget blueprint allocated"), WBP);
    if (!WBP) return false;

    UWidgetTree* Tree = WBP->WidgetTree;
    TestNotNull(TEXT("widget tree allocated"), Tree);
    if (!Tree) return false;

    // RootCanvas (CanvasPanel) <- root
    //   Img (Image)
    UCanvasPanel* RootCanvas = Tree->ConstructWidget<UCanvasPanel>(
        UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
    Tree->RootWidget = RootCanvas;

    UImage* Img = Tree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("Img"));
    RootCanvas->AddChild(Img);

    UWidget* const OldRootPtr = Tree->RootWidget;
    TestTrue(TEXT("RootCanvas is the initial root"),
        OldRootPtr == static_cast<UWidget*>(RootCanvas));

    // --- Swap the root CanvasPanel -> Overlay ---
    bool bWasRoot = false;
    FString Error;
    UWidget* NewRoot = WidgetAuthoringHelpers::ReplaceWidgetClass(
        WBP, RootCanvas, UOverlay::StaticClass(),
        /*bPreserveProperties=*/true,
        bWasRoot, &Error);

    TestNotNull(TEXT("ReplaceWidgetClass returned a new widget"), NewRoot);
    TestEqual(TEXT("no error"), Error, FString());
    TestTrue(TEXT("root-ness reported"), bWasRoot);
    if (!NewRoot) return false;

    // Assertions
    TestTrue(TEXT("RootWidget is now an Overlay"),
        Tree->RootWidget && Tree->RootWidget->IsA<UOverlay>());
    TestTrue(TEXT("RootWidget keeps the original FName"),
        Tree->RootWidget && Tree->RootWidget->GetFName() == FName(TEXT("RootCanvas")));
    TestTrue(TEXT("RootWidget pointer changed"),
        static_cast<UWidget*>(Tree->RootWidget) != OldRootPtr);

    UOverlay* NewRootOverlay = Cast<UOverlay>(Tree->RootWidget);
    if (!NewRootOverlay) return false;

    TestEqual(TEXT("new root has 1 child"), NewRootOverlay->GetChildrenCount(), 1);
    UWidget* Child = NewRootOverlay->GetChildAt(0);
    TestTrue(TEXT("child is the Img widget"),
        Child && Child->IsA<UImage>() && Child->GetFName() == FName(TEXT("Img")));

    return true;
}
