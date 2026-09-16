// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Drive/DriveEditorChrome.h"

#include "Handlers/Drive/DriveElementFactory.h"
#include "Handlers/Drive/DriveSlateTextValue.h"
#include "Utils/ScreenshotUtils.h"

#include "Algo/Reverse.h"
#include "Blueprint/UserWidget.h"
#include "Compat/EngineVersionCompat.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/GenericWindowDefinition.h"
#include "Layout/Geometry.h"
#include "Layout/Visibility.h"
#include "Slate/SObjectWidget.h"
#include "Types/ReflectionMetadata.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

// The label / segment-name helpers below are ported from the host project's UI-recording
// subsystem (ExtractLabel / GetWidgetSegmentName), the same source DriveLiveResolver
// ported. They live in a uniquely-named namespace (not anonymous): DriveLiveResolver.cpp in
// this same module already defines identically-named helpers in its own DriveLiveResolverLocal
// namespace, and Unity build merges both .cpp files into one TU. Two identically-named symbols
// in the single shared unnamed namespace of a merged TU would be a redefinition; a distinct
// named namespace sidesteps that the way the plugin's other shared-helper clusters do.
//
// The interactability predicate is NOT re-ported here: FDriveLiveResolver::IsLikelyInteractable
// is already a public, pure type-whitelist predicate with no live-UI dependency, so it is reused
// directly to avoid a second copy that could drift.

namespace DriveEditorChromeLocal
{
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

    // The position of Child among Parent's children, used to disambiguate same-typed siblings
    // in the path. Returns 0 if Child is not found (defensive; the caller only asks for known
    // parent/child pairs).
    int32 ComputeChildIndex(const TSharedPtr<SWidget>& Parent, const TSharedPtr<SWidget>& Child)
    {
        FChildren* Children = Parent->GetChildren();
        if (!Children)
        {
            return 0;
        }

        for (int32 Index = 0; Index < Children->Num(); ++Index)
        {
            if (&Children->GetChildAt(Index).Get() == Child.Get())
            {
                return Index;
            }
        }
        return 0;
    }

    // A walked element paired with its live Slate widget. BuildElementList drops the widget;
    // ResolveHandle keeps it so an action handler gets the SWidget to act on.
    struct FDriveWalkedElement
    {
        TSharedRef<SWidget> Widget;
        FDriveElement Element;
    };

    // Include a widget that is either interactable or bears readable text; structural panels
    // (boxes, overlays, the window chrome itself) are skipped.
    bool ShouldIncludeElement(const TSharedRef<SWidget>& Widget)
    {
        return FDriveLiveResolver::IsLikelyInteractable(Widget) || HasReadableText(Widget);
    }

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

        // Editor widgets have no UMG name, so the window-rooted widget path is the stable-ish key:
        // Handle == Path. Sibling indexing in BuildWidgetPath keeps it unique within one window.
        Element.Path = FDriveEditorChrome::BuildWidgetPath(SlateWidget);
        Element.Handle = Element.Path;
        Element.Surface = EDriveSurface::EditorChrome;
        return Element;
    }

    // A collapsed/hidden or disabled subtree is still WALKED and still emitted - dropping it
    // would silently change widget_present / count / text_* against widgets that do exist -
    // but every element under it is stamped from the folded ancestor state, so it reports
    // visible:false / enabled:false with no geometry rather than last frame's rect.
    void WalkAndCollect(
        const TSharedRef<SWidget>& SlateWidget,
        const DriveElementFactory::FAncestorState& Ancestors,
        TArray<FDriveWalkedElement>& OutElements)
    {
        if (ShouldIncludeElement(SlateWidget))
        {
            OutElements.Add(FDriveWalkedElement{SlateWidget, MakeElement(SlateWidget, Ancestors)});
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

    // Resolve the FDriveWindowSelector to a single live top-level window. On failure returns false
    // with a stable error code. Index takes precedence over Title; both unset selects the active
    // top-level window (falling back to the first regular, then the first visible window).
    // bIncludeMinimizedWindows widens the candidate set by APPENDING minimized top-level windows
    // after the visible ones; see FDriveEditorChrome::ResolveWindow for which callers pass it.
    bool ResolveSelectedWindow(
        const FDriveWindowSelector& Selector,
        TSharedPtr<SWindow>& OutWindow,
        FString& OutTitle,
        FString& OutErrorCode,
        FString& OutErrorMessage,
        // Defaulted so the three drive.* consumers below (BuildElementList / ResolveHandle /
        // CaptureWindow) keep the visible-only set they were written against: walking, clicking
        // or capturing a minimized window observes and acts on nothing.
        bool bIncludeMinimizedWindows = false)
    {
        OutWindow.Reset();
        OutTitle.Reset();
        OutErrorCode.Reset();
        OutErrorMessage.Reset();

        if (!FSlateApplication::IsInitialized())
        {
            OutErrorCode = TEXT("SLATE_NOT_INITIALIZED");
            OutErrorMessage = TEXT("Slate application is not initialized");
            return false;
        }

        FSlateApplication& SlateApp = FSlateApplication::Get();
        TArray<TSharedRef<SWindow>> Windows;
        SlateApp.GetAllVisibleWindowsOrdered(Windows);

        if (bIncludeMinimizedWindows)
        {
            // GetAllVisibleWindowsOrdered filters `IsVisible() && !IsWindowMinimized()`
            // (SlateApplication.cpp:3789-3799 on UE 5.8, re-applied to child windows at :3803).
            // Only the SECOND clause is wrong for a window-state verb: SWindow::IsVisible()
            // (SWindow.cpp:1496) forwards to the native window's IsVisible(), which on Windows is
            // the Show/Hide flag `bIsVisible` (WindowsWindow.cpp:915-918) and stays TRUE while the
            // window is iconic - a minimized window is a perfectly valid, still-visible SWindow
            // that is dropped purely for being minimized. Re-admit exactly that set from the
            // engine's public unfiltered FSlateApplication::GetTopLevelWindows()
            // (SlateApplication.h:1688, inline `return SlateWindows;`), which keeps the
            // IsVisible() half of the filter intact because a genuinely hidden window must stay
            // untargetable. Appended AFTER the visible ones so no window_index changes meaning.
            TArray<TSharedRef<SWindow>> MinimizedWindows;
            for (const TSharedRef<SWindow>& Candidate : SlateApp.GetTopLevelWindows())
            {
                if (Candidate->IsVisible() && Candidate->IsWindowMinimized())
                {
                    MinimizedWindows.Add(Candidate);
                }
            }
            if (MinimizedWindows.Num() > 0)
            {
                TArray<TSharedRef<SWindow>> Candidates;
                FDriveEditorChrome::AppendMinimizedTolerantWindowCandidates(
                    Windows, MinimizedWindows, Candidates);
                Windows = MoveTemp(Candidates);
            }
        }

        if (Windows.Num() == 0)
        {
            OutErrorCode = TEXT("NO_WINDOWS");
            // Say which set was searched, so NO_WINDOWS is never a true statement about a
            // narrower list than the caller believes was consulted.
            OutErrorMessage = bIncludeMinimizedWindows
                ? TEXT("No top-level windows are open (minimized windows were searched too)")
                : TEXT("No visible top-level windows are open");
            return false;
        }

        if (Selector.Index.IsSet())
        {
            const int32 Index = Selector.Index.GetValue();
            if (!Windows.IsValidIndex(Index))
            {
                OutErrorCode = TEXT("WINDOW_NOT_FOUND");
                OutErrorMessage = FString::Printf(
                    TEXT("Window index %d is out of range (0..%d)"), Index, Windows.Num() - 1);
                return false;
            }
            OutWindow = Windows[Index];
        }
        else if (!Selector.Title.IsEmpty())
        {
            for (const TSharedRef<SWindow>& Window : Windows)
            {
                if (Window->GetTitle().ToString().Contains(Selector.Title))
                {
                    OutWindow = Window;
                    break;
                }
            }
            if (!OutWindow.IsValid())
            {
                OutErrorCode = TEXT("WINDOW_NOT_FOUND");
                // Two calls rather than a ternary in the format position: UE 5.8's checked
                // format-string wrapper only accepts a literal there, not a const TCHAR*.
                OutErrorMessage = bIncludeMinimizedWindows
                    ? FString::Printf(
                        TEXT("No open window title contains '%s' (minimized windows were searched too)"),
                        *Selector.Title)
                    : FString::Printf(
                        TEXT("No visible window title contains '%s'"), *Selector.Title);
                return false;
            }
        }
        else
        {
            TSharedPtr<SWindow> Active = SlateApp.GetActiveTopLevelWindow();
            if (Active.IsValid())
            {
                OutWindow = Active;
            }
            else
            {
                for (const TSharedRef<SWindow>& Window : Windows)
                {
                    if (Window->IsRegularWindow())
                    {
                        OutWindow = Window;
                        break;
                    }
                }
                if (!OutWindow.IsValid())
                {
                    OutWindow = Windows[0];
                }
            }
        }

        if (!OutWindow.IsValid())
        {
            // Only reachable if GetActiveTopLevelWindow and every fallback yielded nothing,
            // which the Windows.Num() > 0 check above rules out; kept as a defensive guard.
            OutErrorCode = TEXT("NO_ACTIVE_WINDOW");
            OutErrorMessage = TEXT("Could not resolve an active top-level window");
            return false;
        }

        OutTitle = OutWindow->GetTitle().ToString();
        return true;
    }
}

using namespace DriveEditorChromeLocal;

FString FDriveEditorChrome::WindowTypeToString(EWindowType Type)
{
    switch (Type)
    {
    case EWindowType::Normal:          return TEXT("Normal");
    case EWindowType::Menu:            return TEXT("Menu");
    case EWindowType::ToolTip:         return TEXT("ToolTip");
    case EWindowType::Notification:    return TEXT("Notification");
    case EWindowType::CursorDecorator: return TEXT("CursorDecorator");
#if UE_VERSION_OLDER_THAN(5, 8, 0)
    // 5.8 deprecated EWindowType::GameWindow (no distinction from Normal anymore);
    // 5.8+ game windows report Normal, so the case only exists on older engines.
    case EWindowType::GameWindow:      return TEXT("GameWindow");
#endif
    default:                           return TEXT("Unknown");
    }
}

void FDriveEditorChrome::AppendMinimizedTolerantWindowCandidates(
    const TArray<TSharedRef<SWindow>>& InVisibleOrdered,
    const TArray<TSharedRef<SWindow>>& InExtraWindows,
    TArray<TSharedRef<SWindow>>& OutCandidates)
{
    // The visible enumeration is copied FIRST and unchanged, which is the whole reason this can
    // be a widening rather than a re-ordering: index i of the result is index i of
    // GetAllVisibleWindowsOrdered for every i that existed before. Extras land strictly after.
    OutCandidates = InVisibleOrdered;
    OutCandidates.Reserve(InVisibleOrdered.Num() + InExtraWindows.Num());

    for (const TSharedRef<SWindow>& Extra : InExtraWindows)
    {
        // Identity, not title: two windows may share a title, and a window that somehow appears
        // in both inputs must occupy exactly one index.
        const bool bAlreadyPresent = OutCandidates.ContainsByPredicate(
            [&Extra](const TSharedRef<SWindow>& Existing) { return &Existing.Get() == &Extra.Get(); });
        if (!bAlreadyPresent)
        {
            OutCandidates.Add(Extra);
        }
    }
}

bool FDriveEditorChrome::ResolveWindow(
    const FDriveWindowSelector& Selector,
    TSharedPtr<SWindow>& OutWindow,
    FString& OutTitle,
    FString& OutErrorCode,
    FString& OutErrorMessage,
    bool bIncludeMinimizedWindows)
{
    // Forward to the file-private resolver so the editor.* window verbs share the exact
    // targeting rule (index > title > active) the drive.* surface already uses.
    return ResolveSelectedWindow(
        Selector, OutWindow, OutTitle, OutErrorCode, OutErrorMessage, bIncludeMinimizedWindows);
}

FString FDriveEditorChrome::GetWidgetSegmentName(const TSharedPtr<SWidget>& Widget)
{
    // FReflectionMetaData is set by UWidget::TakeWidget() on all UMG widgets; editor chrome
    // widgets generally lack it and fall through to the Slate type name.
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

FString FDriveEditorChrome::BuildWidgetPath(const TSharedRef<SWidget>& Widget)
{
    TArray<FString> Segments;
    TSharedPtr<SWidget> Current = ConstCastSharedRef<SWidget>(Widget);

    while (Current.IsValid())
    {
        FString Segment = GetWidgetSegmentName(Current);

        // Append the child index within the parent so same-typed siblings get distinct paths.
        // The top-level window root (no parent) keeps a bare segment.
        TSharedPtr<SWidget> Parent = Current->GetParentWidget();
        if (Parent.IsValid())
        {
            Segment += FString::Printf(TEXT("[%d]"), ComputeChildIndex(Parent, Current));
        }

        Segments.Add(MoveTemp(Segment));
        Current = Parent;
    }

    Algo::Reverse(Segments);
    return FString::Join(Segments, TEXT("/"));
}

TArray<FDriveWindowInfo> FDriveEditorChrome::ListWindows()
{
    TArray<FDriveWindowInfo> Result;

    if (!FSlateApplication::IsInitialized())
    {
        return Result;
    }

    TArray<TSharedRef<SWindow>> Windows;
    FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);

    Result.Reserve(Windows.Num());
    for (int32 Index = 0; Index < Windows.Num(); ++Index)
    {
        const TSharedRef<SWindow>& Window = Windows[Index];

        FDriveWindowInfo Info;
        Info.Title = Window->GetTitle().ToString();
        Info.Type = WindowTypeToString(Window->GetType());

        const FGeometry WindowGeometry = Window->GetWindowGeometryInScreen();
        Info.AbsolutePosition = WindowGeometry.GetAbsolutePosition();
        Info.AbsoluteSize = WindowGeometry.GetAbsoluteSize();
        Info.Index = Index;
        // The authoritative maximized signal editor.resize_window / editor.set_window_state read;
        // list_windows surfaces it so its readback agrees with those verbs. (No minimized flag:
        // GetAllVisibleWindowsOrdered above already excludes minimized windows, so IsWindowMinimized()
        // here is always false and no minimized window is ever enumerated.)
        Info.bMaximized = Window->IsWindowMaximized();

        Result.Add(MoveTemp(Info));
    }

    return Result;
}

bool FDriveEditorChrome::BuildElementList(
    const FDriveWindowSelector& Selector,
    TArray<FDriveElement>& OutElements,
    FString& OutWindowTitle,
    FString& OutErrorCode,
    FString& OutErrorMessage)
{
    OutElements.Reset();

    TSharedPtr<SWindow> Window;
    if (!ResolveSelectedWindow(Selector, Window, OutWindowTitle, OutErrorCode, OutErrorMessage))
    {
        return false;
    }

    TArray<FDriveWalkedElement> Walked;
    // The walk root IS the top-level window here, so nothing sits above it to fold in.
    WalkAndCollect(Window.ToSharedRef(), DriveElementFactory::FAncestorState{}, Walked);

    OutElements.Reserve(Walked.Num());
    for (FDriveWalkedElement& Entry : Walked)
    {
        OutElements.Add(MoveTemp(Entry.Element));
    }

    return true;
}

FDriveResolveResult FDriveEditorChrome::ResolveHandle(
    const FDriveWindowSelector& Selector,
    const FString& Handle)
{
    FDriveResolveResult Result;

    TSharedPtr<SWindow> Window;
    FString WindowTitle;
    if (!ResolveSelectedWindow(Selector, Window, WindowTitle, Result.ErrorCode, Result.ErrorMessage))
    {
        // Every window-resolution failure is "no live UI to walk" for this surface; there is no
        // ambiguous-window state (Index/Title both pick a single window deterministically).
        Result.Status = EDriveResolveStatus::NoLiveUi;
        return Result;
    }

    TArray<FDriveWalkedElement> Walked;
    WalkAndCollect(Window.ToSharedRef(), DriveElementFactory::FAncestorState{}, Walked);

    int32 MatchCount = 0;
    int32 MatchIndex = INDEX_NONE;
    for (int32 Index = 0; Index < Walked.Num(); ++Index)
    {
        if (Walked[Index].Element.Handle == Handle)
        {
            ++MatchCount;
            if (MatchIndex == INDEX_NONE)
            {
                MatchIndex = Index;
            }
        }
    }

    if (MatchCount == 0)
    {
        Result.Status = EDriveResolveStatus::NotFound;
        return Result;
    }

    if (MatchCount > 1)
    {
        Result.Status = EDriveResolveStatus::Ambiguous;
        Result.ErrorCode = TEXT("AMBIGUOUS_HANDLE");
        Result.ErrorMessage = FString::Printf(
            TEXT("Handle '%s' matched %d widgets in window '%s'"), *Handle, MatchCount, *WindowTitle);
        return Result;
    }

    Result.Status = EDriveResolveStatus::Found;
    Result.Widget = Walked[MatchIndex].Widget;
    Result.Element = MoveTemp(Walked[MatchIndex].Element);
    return Result;
}

bool FDriveEditorChrome::CaptureWindow(
    const FDriveWindowSelector& Selector,
    TArray<FColor>& OutPixels,
    int32& OutWidth,
    int32& OutHeight,
    FString& OutErrorCode)
{
    OutPixels.Reset();
    OutWidth = 0;
    OutHeight = 0;
    OutErrorCode.Reset();

    TSharedPtr<SWindow> Window;
    FString WindowTitle;
    FString ErrorMessage;
    if (!ResolveSelectedWindow(Selector, Window, WindowTitle, OutErrorCode, ErrorMessage))
    {
        return false;
    }

    TArray<FColor> Bitmap;
    FIntVector SizeVec;
    // Never FSlateApplication::TakeScreenshot directly: a window the pass does not draw leaves the
    // renderer armed with a pointer to this frame. Contract in ScreenshotUtils.h.
    const bool bCaptured = PinWrightScreenshotUtils::TakeSlateScreenshot(
        Window.ToSharedRef(), Bitmap, SizeVec);

    if (!bCaptured || SizeVec.X <= 0 || SizeVec.Y <= 0 || Bitmap.Num() == 0)
    {
        OutErrorCode = TEXT("CAPTURE_FAILED");
        return false;
    }

    // Force alpha opaque (Slate may leave it non-255); contract in ScreenshotUtils.h.
    PinWrightScreenshotUtils::ForceOpaqueAlpha(Bitmap);

    OutPixels = MoveTemp(Bitmap);
    OutWidth = SizeVec.X;
    OutHeight = SizeVec.Y;
    return true;
}
