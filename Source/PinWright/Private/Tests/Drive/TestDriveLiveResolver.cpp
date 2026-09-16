// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for FDriveLiveResolver: a pure unit test for the interactability predicate over
// synthetic widgets, plus live PIE integration tests for the element-list builder and the
// single-handle resolver. The live tests mirror the existing live-UI-snapshot tests - they
// skip (with a warning) rather than fail when the editor isn't in a state where a live UMG
// snapshot can be taken (no PIE, no viewport, ambiguous roots, etc.), so they are safe in
// headless CI and assert for real only against a running PIE HUD.

#include "Misc/AutomationTest.h"

#include "Handlers/Drive/DriveLiveResolver.h"
#include "Handlers/Drive/DriveTypes.h"
#include "Tests/Media/LiveUiSnapshotTestHelpers.h"
#include "Tests/TestSkipReporting.h"

#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SOverlay.h"

namespace
{
    using LiveUiSnapshotTestHelpers::IsAcceptedLiveCaptureFailureCode;

    enum class EDriveBuildTestResult
    {
        Built,
        Skipped,
        Failed
    };

    // Build the element list for the default (single) live root, or classify why we can't:
    // an accepted "no capturable UI" code is a skip; anything else is a real failure.
    EDriveBuildTestResult BuildOrSkip(
        FAutomationTestBase& Test,
        TArray<FDriveElement>& OutElements,
        FString& OutRootName)
    {
        FString ErrorCode;
        FString ErrorMessage;
        if (FDriveLiveResolver::BuildElementList(FDriveRootSelector{}, OutElements, OutRootName, ErrorCode, ErrorMessage))
        {
            return EDriveBuildTestResult::Built;
        }

        if (IsAcceptedLiveCaptureFailureCode(ErrorCode))
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("no-live-root"),
                FString::Printf(
                    TEXT("Skipping live drive element-list assertions: %s - %s"),
                    *ErrorCode, *ErrorMessage));
            return EDriveBuildTestResult::Skipped;
        }

        Test.AddError(FString::Printf(
            TEXT("Unexpected live drive element-list failure: %s - %s"),
            *ErrorCode, *ErrorMessage));
        return EDriveBuildTestResult::Failed;
    }

    bool IsFiniteVector(const FVector2D& Value)
    {
        return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y);
    }
}

// ============================================================================
// Pure: interactability predicate over synthetic widgets (no PIE needed)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveResolverInteractablePredicateTest,
    "PinWright.drive.resolver.InteractablePredicate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveResolverInteractablePredicateTest::RunTest(const FString& Parameters)
{
    const TSharedRef<SWidget> Button = SNew(SButton);
    const TSharedRef<SWidget> EditableText = SNew(SEditableTextBox);
    const TSharedRef<SWidget> Box = SNew(SBox);
    const TSharedRef<SWidget> Image = SNew(SImage);

    TestTrue(TEXT("SButton is interactable (type whitelist)"),
        FDriveLiveResolver::IsLikelyInteractable(Button));
    TestTrue(TEXT("SEditableTextBox is interactable (type whitelist)"),
        FDriveLiveResolver::IsLikelyInteractable(EditableText));
    TestFalse(TEXT("SBox is not interactable (structural container)"),
        FDriveLiveResolver::IsLikelyInteractable(Box));
    TestFalse(TEXT("SImage is not interactable (decoration)"),
        FDriveLiveResolver::IsLikelyInteractable(Image));

    return true;
}

// ============================================================================
// Pure: handle scheme over a synthetic widget tree (no PIE needed). Asserts the
// named-ancestor-chain handles drop unnamed Slate intermediates, stay unique,
// round-trip back to their originating widget, and disambiguate duplicate leaves.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveResolverHandleSchemeTest,
    "PinWright.drive.resolver.HandleScheme",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveResolverHandleSchemeTest::RunTest(const FString& Parameters)
{
    // Parent links are wired explicitly so GetParentWidget() walks deterministically:
    //   Root[SBox, boundary]
    //     AnchorOuter[SBox, anchor "W_Outer"]
    //       AnchorInner[SBox, anchor "W_Inner"]
    //         Structural[SOverlay, unnamed intermediate - contributes no segment]
    //           Leaf1[SButton, pure-Slate]            -> "W_Outer/W_Inner/SButton[0]"
    //           Leaf2[SButton, pure-Slate]            -> "W_Outer/W_Inner/SButton[1]"
    //         NamedLeaf[SButton, name "ListView_Settings"] -> "ListView_Settings"
    const TSharedRef<SWidget> Root = SNew(SBox);
    const TSharedRef<SWidget> AnchorOuter = SNew(SBox);
    const TSharedRef<SWidget> AnchorInner = SNew(SBox);
    const TSharedRef<SWidget> Structural = SNew(SOverlay);
    const TSharedRef<SWidget> Leaf1 = SNew(SButton);
    const TSharedRef<SWidget> Leaf2 = SNew(SButton);
    const TSharedRef<SWidget> NamedLeaf = SNew(SButton);

    AnchorOuter->AssignParentWidget(Root);
    AnchorInner->AssignParentWidget(AnchorOuter);
    Structural->AssignParentWidget(AnchorInner);
    Leaf1->AssignParentWidget(Structural);
    Leaf2->AssignParentWidget(Structural);
    NamedLeaf->AssignParentWidget(AnchorInner);

    TMap<const SWidget*, FString> AnchorNames;
    AnchorNames.Add(&AnchorOuter.Get(), TEXT("W_Outer"));
    AnchorNames.Add(&AnchorInner.Get(), TEXT("W_Inner"));

    TMap<const SWidget*, FString> LeafNames;
    LeafNames.Add(&NamedLeaf.Get(), TEXT("ListView_Settings"));

    // Walk order drives the [k] disambiguator: Leaf1 before Leaf2.
    const TArray<TSharedRef<SWidget>> Leaves = { Leaf1, Leaf2, NamedLeaf };

    const TArray<FString> Handles =
        FDriveLiveResolver::BuildHandlesForTest(Leaves, &Root.Get(), LeafNames, AnchorNames);

    if (!TestEqual(TEXT("one handle per leaf"), Handles.Num(), Leaves.Num()))
    {
        return false;
    }

    // (a) Handles carry no unnamed-intermediate segments: only anchor names + the leaf type/name.
    for (const FString& Handle : Handles)
    {
        TestFalse(FString::Printf(TEXT("handle '%s' has no unnamed SOverlay intermediate"), *Handle),
            Handle.Contains(TEXT("SOverlay")));
        TestFalse(FString::Printf(TEXT("handle '%s' has no unnamed SBox intermediate"), *Handle),
            Handle.Contains(TEXT("SBox")));
    }

    // Concrete shapes: named-ancestor chain + leaf type for pure-Slate leaves, short name for
    // the backing-named leaf (unchanged from the old behavior).
    TestEqual(TEXT("pure-Slate leaf 1 handle"), Handles[0], FString(TEXT("W_Outer/W_Inner/SButton[0]")));
    TestEqual(TEXT("pure-Slate leaf 2 handle"), Handles[1], FString(TEXT("W_Outer/W_Inner/SButton[1]")));
    TestEqual(TEXT("backing-named leaf keeps its short name"), Handles[2], FString(TEXT("ListView_Settings")));

    // (b) All handles are unique within the root.
    const TSet<FString> Unique(Handles);
    TestEqual(TEXT("all handles are unique"), Unique.Num(), Handles.Num());

    // (d) Duplicate leaves disambiguate to distinct handles off the same base key.
    TestNotEqual(TEXT("duplicate leaves get different handles"), Handles[0], Handles[1]);
    TestTrue(TEXT("duplicate leaf 1 keeps the shared base key"),
        Handles[0].StartsWith(TEXT("W_Outer/W_Inner/SButton")));
    TestTrue(TEXT("duplicate leaf 2 keeps the shared base key"),
        Handles[1].StartsWith(TEXT("W_Outer/W_Inner/SButton")));

    // (c) Round-trip: re-deriving the handles (exactly what ResolveHandle does by re-walking the
    // tree) yields an identical list, so each handle re-resolves to its originating leaf index.
    const TArray<FString> Rederived =
        FDriveLiveResolver::BuildHandlesForTest(Leaves, &Root.Get(), LeafNames, AnchorNames);
    for (int32 Index = 0; Index < Handles.Num(); ++Index)
    {
        const int32 ResolvedIndex = Rederived.IndexOfByKey(Handles[Index]);
        TestEqual(FString::Printf(TEXT("handle '%s' resolves back to its originating leaf"), *Handles[Index]),
            ResolvedIndex, Index);
    }

    return true;
}

// ============================================================================
// Live: element-list builder shape and per-element invariants
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveResolverElementListLiveTest,
    "PinWright.drive.resolver.ElementListLive",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveResolverElementListLiveTest::RunTest(const FString& Parameters)
{
    TArray<FDriveElement> Elements;
    FString RootName;

    const EDriveBuildTestResult Result = BuildOrSkip(*this, Elements, RootName);
    if (Result != EDriveBuildTestResult::Built)
    {
        return Result == EDriveBuildTestResult::Skipped;
    }

    TestFalse(TEXT("resolved root name is populated"), RootName.IsEmpty());

    for (const FDriveElement& Element : Elements)
    {
        TestFalse(TEXT("every element carries a handle"), Element.Handle.IsEmpty());
        TestFalse(TEXT("every element carries a type"), Element.Type.IsEmpty());
        TestEqual(TEXT("every element is on the Game surface"),
            static_cast<int32>(Element.Surface), static_cast<int32>(EDriveSurface::Game));
        TestTrue(TEXT("element absolute position is finite"), IsFiniteVector(Element.AbsolutePosition));
        TestTrue(TEXT("element absolute size is finite"), IsFiniteVector(Element.AbsoluteSize));
        // Included only when interactable OR text-bearing; an interactable element must carry the flag.
        TestTrue(TEXT("included element is interactable or label-bearing"),
            Element.bInteractable || !Element.Label.IsEmpty());
    }

    return true;
}

// ============================================================================
// Live: a known handle re-resolves to a matching element
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveResolverResolveKnownHandleLiveTest,
    "PinWright.drive.resolver.ResolveKnownHandleLive",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveResolverResolveKnownHandleLiveTest::RunTest(const FString& Parameters)
{
    TArray<FDriveElement> Elements;
    FString RootName;

    const EDriveBuildTestResult Result = BuildOrSkip(*this, Elements, RootName);
    if (Result != EDriveBuildTestResult::Built)
    {
        return Result == EDriveBuildTestResult::Skipped;
    }

    if (Elements.Num() == 0)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-interactable-elements"),
            TEXT("Live root has no interactable/text elements; skipping known-handle resolution."));
        return true;
    }

    // Take the first listed element as the "known handle" and re-resolve it against the live tree.
    const FString KnownHandle = Elements[0].Handle;
    const FDriveResolveResult Resolved = FDriveLiveResolver::ResolveHandle(FDriveRootSelector{}, KnownHandle);

    TestEqual(TEXT("known handle resolves to Found"),
        static_cast<int32>(Resolved.Status), static_cast<int32>(EDriveResolveStatus::Found));
    TestTrue(TEXT("resolved result carries a live widget"), Resolved.Widget.IsValid());
    TestEqual(TEXT("resolved element handle matches the request"), Resolved.Element.Handle, KnownHandle);
    TestEqual(TEXT("resolved element type matches the listed element"), Resolved.Element.Type, Elements[0].Type);

    return true;
}

// ============================================================================
// Live: a missing handle resolves to NotFound (root present, handle absent)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveResolverResolveMissingHandleLiveTest,
    "PinWright.drive.resolver.ResolveMissingHandleLive",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveResolverResolveMissingHandleLiveTest::RunTest(const FString& Parameters)
{
    TArray<FDriveElement> Elements;
    FString RootName;

    const EDriveBuildTestResult Result = BuildOrSkip(*this, Elements, RootName);
    if (Result != EDriveBuildTestResult::Built)
    {
        return Result == EDriveBuildTestResult::Skipped;
    }

    // A handle that cannot exist on any real widget; the root resolves, so the outcome is a
    // clean NotFound (not NoLiveUi, which is reserved for "couldn't reach a root to walk").
    const FString MissingHandle = TEXT("__pinwright_drive_no_such_handle__");
    const FDriveResolveResult Resolved = FDriveLiveResolver::ResolveHandle(FDriveRootSelector{}, MissingHandle);

    TestEqual(TEXT("missing handle resolves to NotFound"),
        static_cast<int32>(Resolved.Status), static_cast<int32>(EDriveResolveStatus::NotFound));
    TestFalse(TEXT("NotFound result carries no live widget"), Resolved.Widget.IsValid());

    return true;
}
