// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Drive/DriveLiveResolver.h"

#include "Handlers/Drive/DriveElementFactory.h"
#include "Handlers/Drive/DriveSlateTextValue.h"
#include "Handlers/UI/LiveUiSnapshot.h"

#include "Algo/Reverse.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/Visibility.h"
#include "Slate/SObjectWidget.h"
#include "Types/ReflectionMetadata.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

// Most of the live-walk plumbing below is replicated from the file-local anonymous-namespace
// helpers in Handlers/UI/LiveUiSnapshot.cpp (ResolvePieWorld, ResolveGameViewportWindow,
// IsUmgRootCandidate, CollectUmgRootCandidates, BuildBackingWidgetMap, ResolveCandidateName).
// Those are not exposed in LiveUiSnapshot.h, so rather than export them (and widen that unit's
// surface) they are re-implemented here to match exactly. The only LiveUiSnapshot symbol reused
// directly is the public FLiveUiSnapshotService::SelectRootCandidate, which owns the multi-root
// selector logic and its AMBIGUOUS_LIVE_ROOT / LIVE_ROOT_NOT_FOUND / LIVE_UI_NOT_FOUND vocabulary.
// The label / path / interactability helpers are ported from the host project's UI-recording subsystem.
//
// These live in a uniquely-named namespace (not an anonymous one) on purpose: several of the
// replicated helpers share names with LiveUiSnapshot.cpp's anonymous-namespace helpers in this
// same module, and Unity build merges both .cpp files into one TU. Two identically-named symbols
// in the (single, shared) unnamed namespace of a merged TU would be a redefinition; a distinct
// named namespace sidesteps that the way the plugin's other shared-helper clusters do.

namespace DriveLiveResolverLocal
{
    bool ValidateSlateInitialized(FString& OutErrorCode, FString& OutErrorMessage)
    {
        if (FSlateApplication::IsInitialized())
        {
            return true;
        }

        OutErrorCode = TEXT("SLATE_NOT_INITIALIZED");
        OutErrorMessage = TEXT("Slate application is not initialized");
        return false;
    }

    UWorld* ResolvePieWorld()
    {
        if (!GEngine || !GEngine->GameViewport)
        {
            return nullptr;
        }

        UWorld* World = GEngine->GameViewport->GetWorld();
        if (!World)
        {
            return nullptr;
        }

        if (World->WorldType != EWorldType::PIE && !World->IsPlayInEditor())
        {
            return nullptr;
        }

        return World;
    }

    TSharedPtr<SWindow> ResolveGameViewportWindow()
    {
        if (!GEngine || !GEngine->GameViewport)
        {
            return nullptr;
        }

        TSharedPtr<SViewport> ViewportWidget = GEngine->GameViewport->GetGameViewportWidget();
        if (!ViewportWidget.IsValid())
        {
            return nullptr;
        }

        return FSlateApplication::Get().FindWidgetWindow(ViewportWidget.ToSharedRef());
    }

    bool IsUmgRootCandidate(const TSharedRef<SWidget>& Widget)
    {
        if (Widget->GetTypeAsString() != TEXT("SConstraintCanvas"))
        {
            return false;
        }

        FChildren* Children = Widget->GetChildren();
        if (!Children || Children->Num() == 0)
        {
            return false;
        }

        return Children->GetChildAt(0)->GetTypeAsString() == TEXT("SObjectWidget");
    }

    void CollectUmgRootCandidates(const TSharedRef<SWidget>& Widget, TArray<TSharedRef<SWidget>>& OutCandidates)
    {
        if (IsUmgRootCandidate(Widget))
        {
            OutCandidates.Add(Widget);
            return;
        }

        FChildren* Children = Widget->GetChildren();
        if (!Children)
        {
            return;
        }

        for (int32 Index = 0; Index < Children->Num(); ++Index)
        {
            CollectUmgRootCandidates(Children->GetChildAt(Index), OutCandidates);
        }
    }

    void BuildBackingWidgetMap(UWorld* World, TMap<SWidget*, UWidget*>& OutMap)
    {
        if (!World)
        {
            return;
        }

        TArray<UUserWidget*> UserWidgets;
        UWidgetBlueprintLibrary::GetAllWidgetsOfClass(World, UserWidgets, UUserWidget::StaticClass(), false);

        TFunction<void(UWidget*)> VisitWidget = [&](UWidget* Widget)
        {
            if (!Widget)
            {
                return;
            }

            TSharedPtr<SWidget> Cached = Widget->GetCachedWidget();
            if (Cached.IsValid())
            {
                OutMap.Add(Cached.Get(), Widget);
            }
        };

        for (UUserWidget* UserWidget : UserWidgets)
        {
            if (!UserWidget)
            {
                continue;
            }

            // Include the UUserWidget itself so the live SObjectWidget host resolves back to it.
            VisitWidget(UserWidget);

            if (UserWidget->WidgetTree)
            {
                UserWidget->WidgetTree->ForEachWidget([&VisitWidget](UWidget* Widget)
                {
                    VisitWidget(Widget);
                });
            }
        }
    }

    // The backing-widget name a UMG root candidate is addressable by (the SObjectWidget's
    // backing UWidget name, e.g. WBP_PlayerHUD_StateTree_C_0). This is the same name the
    // snapshot selector matches and the key ui.create_hud returns.
    FString ResolveCandidateName(const TSharedRef<SWidget>& Candidate, const TMap<SWidget*, UWidget*>& BackingMap)
    {
        FChildren* Children = Candidate->GetChildren();
        if (Children && Children->Num() > 0)
        {
            const TSharedRef<SWidget> FirstChild = Children->GetChildAt(0);
            if (UWidget* const* Backing = BackingMap.Find(&FirstChild.Get()))
            {
                return (*Backing)->GetName();
            }
        }

        return FString();
    }

    // ── Ported from the host project's UI-recording subsystem ───────────────────────────

    // Human-readable label: accessible text, then STextBlock content, then tag, then type.
    FString ExtractLabel(const TSharedRef<SWidget>& Widget)
    {
#if WITH_ACCESSIBILITY
        // GetAccessibleText is compiled out on platforms without accessibility (e.g. Linux),
        // so it is guarded here; the STextBlock / tag / type fallback covers those builds.
        const FText AccessibleText = Widget->GetAccessibleText(EAccessibleType::Main);
        if (!AccessibleText.IsEmpty())
        {
            // Accessible text may contain duplicated lines - take only the first line.
            FString Label = AccessibleText.ToString();
            int32 NewlineIdx;
            if (Label.FindChar(TEXT('\r'), NewlineIdx) || Label.FindChar(TEXT('\n'), NewlineIdx))
            {
                Label.LeftInline(NewlineIdx);
            }
            return Label;
        }
#endif

        if (Widget->GetType() == FName(TEXT("STextBlock")))
        {
            const STextBlock& TextBlock = static_cast<const STextBlock&>(Widget.Get());
            const FText BlockText = TextBlock.GetText();
            if (!BlockText.IsEmpty())
            {
                return BlockText.ToString();
            }
        }

        const FName Tag = Widget->GetTag();
        if (Tag != NAME_None)
        {
            return Tag.ToString();
        }

        return Widget->GetTypeAsString();
    }

    // True when the widget carries real text/label content (the first three ExtractLabel
    // branches), i.e. a status string a verification step can read - not merely a type fallback.
    bool HasReadableText(const TSharedRef<SWidget>& Widget)
    {
#if WITH_ACCESSIBILITY
        if (!Widget->GetAccessibleText(EAccessibleType::Main).IsEmpty())
        {
            return true;
        }
#endif

        if (Widget->GetType() == FName(TEXT("STextBlock")))
        {
            const STextBlock& TextBlock = static_cast<const STextBlock&>(Widget.Get());
            if (!TextBlock.GetText().IsEmpty())
            {
                return true;
            }
        }

        return Widget->GetTag() != NAME_None;
    }

    // Meaningful name for one segment of a widget path: UMG metadata > SObjectWidget BP name > Slate type.
    FString GetWidgetSegmentName(const TSharedPtr<SWidget>& Widget)
    {
        // FReflectionMetaData is set by UWidget::TakeWidget() on all UMG widgets.
        TSharedPtr<FReflectionMetaData> MetaData = Widget->GetMetaData<FReflectionMetaData>();
        if (MetaData.IsValid() && MetaData->Name != NAME_None)
        {
            return MetaData->Name.ToString();
        }

        const FString TypeStr = Widget->GetTypeAsString();
        if (TypeStr == TEXT("SObjectWidget"))
        {
            const SObjectWidget* ObjWidget = static_cast<const SObjectWidget*>(Widget.Get());
            if (const UUserWidget* UserWidget = ObjWidget->GetWidgetObject())
            {
                FString ClassName = UserWidget->GetClass()->GetName();
                ClassName.RemoveFromEnd(TEXT("_C"));
                if (!ClassName.IsEmpty())
                {
                    return ClassName;
                }
            }
        }

        return TypeStr;
    }

    // Human-readable widget path, trimmed to the game-only portion (from the first SConstraintCanvas).
    FString BuildWidgetPath(const TSharedRef<SWidget>& Widget)
    {
        TArray<FString> Segments;
        TSharedPtr<SWidget> Current = ConstCastSharedRef<SWidget>(Widget);

        while (Current.IsValid())
        {
            Segments.Add(GetWidgetSegmentName(Current));
            Current = Current->GetParentWidget();
        }

        Algo::Reverse(Segments);
        FString FullPath = FString::Join(Segments, TEXT("/"));

        const FString Marker = TEXT("SConstraintCanvas/");
        const int32 MarkerIdx = FullPath.Find(Marker, ESearchCase::CaseSensitive);
        if (MarkerIdx != INDEX_NONE)
        {
            FullPath.RightChopInline(MarkerIdx + Marker.Len());
        }
        return FullPath;
    }

    // A walked element paired with its live Slate widget. BuildElementList drops the widget;
    // ResolveHandle keeps it so an action handler gets the SWidget to act on.
    struct FDriveLiveWalkedElement
    {
        TSharedRef<SWidget> Widget;
        FDriveElement Element;
    };

    // Emit an element for a widget that is either interactable or bears readable text;
    // structural panels (canvases, boxes, the SObjectWidget host) are skipped.
    bool ShouldIncludeElement(const TSharedRef<SWidget>& Widget)
    {
        return FDriveLiveResolver::IsLikelyInteractable(Widget) || HasReadableText(Widget);
    }

    // Build an element snapshot for a widget. The Handle is left empty here and filled in
    // afterwards by AssignDriveHandles: a handle must be unique within the whole walked set,
    // which isn't knowable from a single widget, so it can only be assigned once the full
    // sibling list exists.
    FDriveElement MakeElement(
        const TSharedRef<SWidget>& SlateWidget,
        const DriveElementFactory::FAncestorState& Ancestors)
    {
        FDriveElement Element;
        Element.Type = SlateWidget->GetTypeAsString();
        Element.Label = ExtractLabel(SlateWidget);
        // Live typed value for editable widgets (empty otherwise). ExtractLabel reads the
        // accessible text, which for an editable box is the HINT, so the typed value must be
        // read separately via GetText() and surfaced distinctly from Label.
        Element.Value = DriveSlateTextValue::ReadEditableWidgetText(SlateWidget);
        Element.bInteractable = FDriveLiveResolver::IsLikelyInteractable(SlateWidget);

        DriveElementFactory::FillStateAndGeometry(SlateWidget, Ancestors, Element);

        Element.Path = BuildWidgetPath(SlateWidget);
        Element.Surface = EDriveSurface::Game;

        return Element;
    }

    // A collapsed/hidden or disabled subtree is still WALKED and still emitted - dropping it
    // would silently change widget_present / count / text_* against widgets that do exist -
    // but every element under it is stamped from the folded ancestor state, so it reports
    // visible:false / enabled:false with no geometry rather than last frame's rect.
    void WalkAndCollect(
        const TSharedRef<SWidget>& SlateWidget,
        const DriveElementFactory::FAncestorState& Ancestors,
        TArray<FDriveLiveWalkedElement>& OutElements)
    {
        if (ShouldIncludeElement(SlateWidget))
        {
            OutElements.Add(FDriveLiveWalkedElement{SlateWidget, MakeElement(SlateWidget, Ancestors)});
        }

        FChildren* Children = SlateWidget->GetChildren();
        if (!Children)
        {
            return;
        }

        const DriveElementFactory::FAncestorState ChildAncestors = Ancestors.ForChildrenOf(SlateWidget);
        for (int32 Index = 0; Index < Children->Num(); ++Index)
        {
            WalkAndCollect(Children->GetChildAt(Index), ChildAncestors, OutElements);
        }
    }

    // ── Handle scheme (shared by the builder and the resolver) ──────────────────────────
    //
    // A handle addresses one walked widget within the selected root. The single source of
    // truth for "what handle does this widget get" is AssignHandles below: BuildElementList
    // and ResolveHandle both run the identical WalkAndCollect + AssignDriveHandles, so a
    // listed handle and the handle ResolveHandle matches against can never drift.
    //
    // Base key for one leaf:
    //   - If the leaf has its own backing-UWidget name, the key is just that short name
    //     (e.g. "ListView_Settings") - unchanged from the old behavior.
    //   - Otherwise the key is the chain of NAMED ancestors + the leaf's Slate type
    //     (e.g. "WBP_RootLayout_C_0/WBP_FrontEnd_C_0/WBP_MenuButton/SCommonButton").
    //     Only "anchor" ancestors contribute a segment; the unnamed Slate intermediates
    //     (Overlay_0, CanvasPanel_151, SizeBox_*, InternalRootButtonBase, ...) are dropped.
    // Identical base keys (e.g. two pure-Slate SCommonButtons under the same anchor) are then
    // disambiguated with a "[k]" index in walk order, so the final handles stay unique.

    // Compute a leaf's base key (no disambiguator yet). GetLeafName returns the widget's own
    // short name (empty if it has none); GetAnchorName returns a chain segment for an ancestor
    // (empty if that ancestor is not an anchor). The parent walk stops at RootBoundary so the
    // chain never climbs above the selected root.
    FString ComputeHandleBaseKey(
        const TSharedRef<SWidget>& Leaf,
        const SWidget* RootBoundary,
        const TFunctionRef<FString(const TSharedRef<SWidget>&)>& GetLeafName,
        const TFunctionRef<FString(const TSharedRef<SWidget>&)>& GetAnchorName)
    {
        const FString LeafName = GetLeafName(Leaf);
        if (!LeafName.IsEmpty())
        {
            return LeafName;
        }

        TArray<FString> Segments;
        TSharedPtr<SWidget> Current = Leaf->GetParentWidget();
        while (Current.IsValid())
        {
            const TSharedRef<SWidget> CurrentRef = Current.ToSharedRef();
            const FString AnchorName = GetAnchorName(CurrentRef);
            if (!AnchorName.IsEmpty())
            {
                Segments.Add(AnchorName);
            }
            if (Current.Get() == RootBoundary)
            {
                break;
            }
            Current = Current->GetParentWidget();
        }

        Algo::Reverse(Segments);
        Segments.Add(Leaf->GetTypeAsString());
        return FString::Join(Segments, TEXT("/"));
    }

    // Assign a unique, resolvable handle to each leaf in walk order. A base key that occurs
    // once is used as-is; a base key shared by several leaves gets a trailing "[k]" index by
    // walk position. Deterministic for a given leaf order, which is why re-walking re-derives
    // the exact same handles (the round-trip guarantee ResolveHandle relies on).
    void AssignHandles(
        const TArray<TSharedRef<SWidget>>& Leaves,
        const SWidget* RootBoundary,
        const TFunctionRef<FString(const TSharedRef<SWidget>&)>& GetLeafName,
        const TFunctionRef<FString(const TSharedRef<SWidget>&)>& GetAnchorName,
        TArray<FString>& OutHandles)
    {
        TArray<FString> BaseKeys;
        BaseKeys.Reserve(Leaves.Num());
        TMap<FString, int32> Counts;
        for (const TSharedRef<SWidget>& Leaf : Leaves)
        {
            FString Key = ComputeHandleBaseKey(Leaf, RootBoundary, GetLeafName, GetAnchorName);
            Counts.FindOrAdd(Key)++;
            BaseKeys.Add(MoveTemp(Key));
        }

        TMap<FString, int32> NextIndex;
        OutHandles.Reset();
        OutHandles.Reserve(BaseKeys.Num());
        for (const FString& Key : BaseKeys)
        {
            if (Counts[Key] > 1)
            {
                int32& Index = NextIndex.FindOrAdd(Key);
                OutHandles.Add(FString::Printf(TEXT("%s[%d]"), *Key, Index));
                ++Index;
            }
            else
            {
                OutHandles.Add(Key);
            }
        }
    }

    // Live wrapper over AssignHandles: derives the name providers from the backing map and
    // writes the resulting handles back onto the walked elements. A leaf's own short name is
    // its backing UWidget name (any UWidget); a chain anchor is a backing UWidget that is
    // itself a UUserWidget (a nested WBP boundary) - which keeps the chain to meaningful
    // navigational segments and drops the layout-panel UWidgets like CanvasPanel_151.
    void AssignDriveHandles(
        TArray<FDriveLiveWalkedElement>& Walked,
        const TMap<SWidget*, UWidget*>& BackingMap,
        const SWidget* RootBoundary)
    {
        TArray<TSharedRef<SWidget>> Leaves;
        Leaves.Reserve(Walked.Num());
        for (const FDriveLiveWalkedElement& Entry : Walked)
        {
            Leaves.Add(Entry.Widget);
        }

        auto GetLeafName = [&BackingMap](const TSharedRef<SWidget>& Widget) -> FString
        {
            if (UWidget* const* Backing = BackingMap.Find(&Widget.Get()))
            {
                return (*Backing)->GetName();
            }
            return FString();
        };
        auto GetAnchorName = [&BackingMap](const TSharedRef<SWidget>& Widget) -> FString
        {
            if (UWidget* const* Backing = BackingMap.Find(&Widget.Get()))
            {
                if ((*Backing)->IsA<UUserWidget>())
                {
                    return (*Backing)->GetName();
                }
            }
            return FString();
        };

        TArray<FString> Handles;
        AssignHandles(Leaves, RootBoundary, GetLeafName, GetAnchorName, Handles);

        for (int32 Index = 0; Index < Walked.Num(); ++Index)
        {
            Walked[Index].Element.Handle = MoveTemp(Handles[Index]);
        }
    }

    // Resolve the live game viewport, collect UMG root candidates, build the backing map, and
    // run the shared selector. On success returns the selected root subtree, its backing map,
    // and its addressable name. On failure returns false with a LiveUiSnapshot-vocabulary code.
    bool ResolveSelectedRoot(
        const FDriveRootSelector& Selector,
        TSharedPtr<SWidget>& OutRoot,
        TMap<SWidget*, UWidget*>& OutBackingMap,
        FString& OutRootName,
        FString& OutErrorCode,
        FString& OutErrorMessage)
    {
        OutRoot.Reset();
        OutBackingMap.Reset();
        OutRootName.Reset();
        OutErrorCode.Reset();
        OutErrorMessage.Reset();

        if (!ValidateSlateInitialized(OutErrorCode, OutErrorMessage))
        {
            return false;
        }

        UWorld* World = ResolvePieWorld();
        if (!World)
        {
            OutErrorCode = TEXT("PIE_NOT_RUNNING");
            OutErrorMessage = TEXT("No active PIE game viewport world");
            return false;
        }

        TSharedPtr<SWindow> GameWindow = ResolveGameViewportWindow();
        if (!GameWindow.IsValid())
        {
            OutErrorCode = TEXT("GAME_VIEWPORT_NOT_FOUND");
            OutErrorMessage = TEXT("Could not resolve the active game viewport window");
            return false;
        }

        TArray<TSharedRef<SWidget>> RootCandidates;
        CollectUmgRootCandidates(GameWindow.ToSharedRef(), RootCandidates);

        BuildBackingWidgetMap(World, OutBackingMap);

        if (RootCandidates.Num() == 0)
        {
            OutErrorCode = TEXT("LIVE_UI_NOT_FOUND");
            OutErrorMessage = TEXT("No live UMG root subtree was found in the game viewport window");
            return false;
        }

        TArray<FString> CandidateNames;
        CandidateNames.Reserve(RootCandidates.Num());
        for (const TSharedRef<SWidget>& Candidate : RootCandidates)
        {
            CandidateNames.Add(ResolveCandidateName(Candidate, OutBackingMap));
        }

        // Reuse the production selector so the ambiguity / not-found errors match the snapshot RPCs.
        FLiveUiSnapshotRequest Request;
        Request.InstanceName = Selector.InstanceName;
        Request.RootIndex = Selector.RootIndex;

        int32 SelectedIndex = INDEX_NONE;
        if (!FLiveUiSnapshotService::SelectRootCandidate(
                CandidateNames, Request, SelectedIndex, OutErrorCode, OutErrorMessage))
        {
            return false;
        }

        OutRoot = RootCandidates[SelectedIndex];
        OutRootName = CandidateNames[SelectedIndex];
        return true;
    }

    // Map a root-resolution failure code to a ResolveHandle status. Only the no-selector /
    // loose-selector multi-root case is "ambiguous"; everything else is "no live UI to walk".
    EDriveResolveStatus RootFailureToStatus(const FString& ErrorCode)
    {
        return ErrorCode == TEXT("AMBIGUOUS_LIVE_ROOT")
            ? EDriveResolveStatus::Ambiguous
            : EDriveResolveStatus::NoLiveUi;
    }
}

using namespace DriveLiveResolverLocal;

bool FDriveLiveResolver::IsLikelyInteractable(const TSharedRef<SWidget>& Widget)
{
    static const TArray<FString> InteractableTypes = {
        TEXT("SButton"), TEXT("SCheckBox"), TEXT("SSlider"), TEXT("SSpinBox"),
        TEXT("SEditableText"), TEXT("SEditableTextBox"), TEXT("SMultiLineEditableText"),
        TEXT("SComboBox"), TEXT("SComboButton"), TEXT("SHyperlink"), TEXT("SScrollBar"),
        TEXT("SSearchBox"), TEXT("STableRow"), TEXT("SNumericEntryBox")
    };

    // Wrapper/container types that aren't themselves interactable.
    static const TArray<FString> ExcludedTypes = {
        TEXT("SObjectWidget"), TEXT("SViewport")
    };

    const FString TypeName = Widget->GetTypeAsString();

    for (const FString& Excluded : ExcludedTypes)
    {
        if (TypeName == Excluded)
        {
            return false;
        }
    }

    for (const FString& Interactable : InteractableTypes)
    {
        if (TypeName.Contains(Interactable))
        {
            return true;
        }
    }

    return Widget->SupportsKeyboardFocus();
}

bool FDriveLiveResolver::BuildElementList(
    const FDriveRootSelector& Selector,
    TArray<FDriveElement>& OutElements,
    FString& OutRootName,
    FString& OutErrorCode,
    FString& OutErrorMessage)
{
    OutElements.Reset();

    TSharedPtr<SWidget> Root;
    TMap<SWidget*, UWidget*> BackingMap;
    if (!ResolveSelectedRoot(Selector, Root, BackingMap, OutRootName, OutErrorCode, OutErrorMessage))
    {
        return false;
    }

    TArray<FDriveLiveWalkedElement> Walked;
    // Seeded visible+enabled: the walk starts at the selected live UMG root, and the widgets
    // above it (the viewport, the window) are not part of this surface. A collapsed viewport
    // resolves to no live UI at all, so there is no root-above-root state to fold in here.
    WalkAndCollect(Root.ToSharedRef(), DriveElementFactory::FAncestorState{}, Walked);
    AssignDriveHandles(Walked, BackingMap, Root.Get());

    OutElements.Reserve(Walked.Num());
    for (FDriveLiveWalkedElement& Entry : Walked)
    {
        OutElements.Add(MoveTemp(Entry.Element));
    }

    return true;
}

FDriveResolveResult FDriveLiveResolver::ResolveHandle(
    const FDriveRootSelector& Selector,
    const FString& Handle)
{
    FDriveResolveResult Result;

    TSharedPtr<SWidget> Root;
    TMap<SWidget*, UWidget*> BackingMap;
    FString RootName;
    if (!ResolveSelectedRoot(Selector, Root, BackingMap, RootName, Result.ErrorCode, Result.ErrorMessage))
    {
        Result.Status = RootFailureToStatus(Result.ErrorCode);
        return Result;
    }

    TArray<FDriveLiveWalkedElement> Walked;
    WalkAndCollect(Root.ToSharedRef(), DriveElementFactory::FAncestorState{}, Walked);
    AssignDriveHandles(Walked, BackingMap, Root.Get());

    for (FDriveLiveWalkedElement& Entry : Walked)
    {
        if (Entry.Element.Handle == Handle)
        {
            Result.Status = EDriveResolveStatus::Found;
            Result.Widget = Entry.Widget;
            Result.Element = MoveTemp(Entry.Element);
            return Result;
        }
    }

    Result.Status = EDriveResolveStatus::NotFound;
    return Result;
}

TArray<FString> FDriveLiveResolver::BuildHandlesForTest(
    const TArray<TSharedRef<SWidget>>& Leaves,
    const SWidget* RootBoundary,
    const TMap<const SWidget*, FString>& LeafNames,
    const TMap<const SWidget*, FString>& AnchorNames)
{
    // Same AssignHandles core the live walk uses, but the leaf/anchor names come from explicit
    // maps instead of a live backing map, so the named-ancestor-chain scheme can be unit-tested
    // over a synthetic Slate tree with no PIE viewport.
    auto GetLeafName = [&LeafNames](const TSharedRef<SWidget>& Widget) -> FString
    {
        if (const FString* Name = LeafNames.Find(&Widget.Get()))
        {
            return *Name;
        }
        return FString();
    };
    auto GetAnchorName = [&AnchorNames](const TSharedRef<SWidget>& Widget) -> FString
    {
        if (const FString* Name = AnchorNames.Find(&Widget.Get()))
        {
            return *Name;
        }
        return FString();
    };

    TArray<FString> Handles;
    AssignHandles(Leaves, RootBoundary, GetLeafName, GetAnchorName, Handles);
    return Handles;
}
