// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Layout/Visibility.h"
#include "Slate/WidgetTransform.h"

class SWidget;
class UUserWidget;
class UWidget;
class UPanelSlot;
class FJsonObject;
class FHandlerContext;

struct FLiveUiSnapshotRequest
{
    bool bVerbose = false;
    bool bIncludeGeometry = false;

    // Optional root selector for the multi-root case: when more than one live UMG
    // root subtree is present in the viewport (e.g. a level's own HUD plus a
    // ui.create_hud instance), a selector disambiguates which one to dump. Matched
    // by substring against each candidate's backing widget name (the same names the
    // AMBIGUOUS_LIVE_ROOT error enumerates, e.g. WBP_PlayerHUD_StateTree_C_0).
    FString InstanceName;

    // Optional positional selector: pick the Nth live root (0-based, in collection
    // order). Mutually exclusive with InstanceName when both are set and disagree.
    TOptional<int32> RootIndex;

    // Parses the live-snapshot fields (verbose / include_geometry / instance_name /
    // root_index, with their camelCase aliases) out of a request context. Shared by
    // every live-capture handler (widget.describe, widget.export_xml) so the selector
    // wiring lives in exactly one place and the two RPCs accept identical inputs.
    static FLiveUiSnapshotRequest FromContext(const FHandlerContext& Ctx);
};

struct FLiveUiSnapshotSourceInfo
{
    bool bHasBackingWidget = false;
    FString WidgetBlueprintPath;
    FString WidgetClassPath;
    FString WidgetName;
    FString OwningUserWidgetName;
    FString OwningUserWidgetClassPath;
    FString SourceKind;
};

struct FLiveUiSnapshotSlotInfo
{
    FString SlotClassName;
    TSharedPtr<FJsonObject> Properties;
};

struct FLiveUiSnapshotRuntimeState
{
    FString Visibility;
    TOptional<bool> bEnabled = true;
    bool bFocused = false;
    bool bFocusable = false;
    FString Clipping;
    bool bVolatile = false;
    bool bHasRenderTransform = false;
    FWidgetTransform RenderTransform;
    FVector2D DesiredSize = FVector2D::ZeroVector;
    FVector2D AbsolutePosition = FVector2D::ZeroVector;
    FVector2D AbsoluteSize = FVector2D::ZeroVector;
};

struct FLiveUiSnapshotNode
{
    FString SlateType;
    FString DebugName;
    FLiveUiSnapshotRuntimeState RuntimeState;
    FLiveUiSnapshotSourceInfo SourceInfo;
    FLiveUiSnapshotSlotInfo SlotInfo;
    TSharedPtr<FJsonObject> Properties;
    TArray<TSharedPtr<FJsonObject>> Bindings;
    TArray<TSharedPtr<FJsonObject>> Delegates;
    TArray<FLiveUiSnapshotNode> Children;
};

struct FLiveUiSnapshot
{
    FString CaptureSource = TEXT("live");
    bool bVerbose = false;
    bool bGeometryIncluded = false;
    FVector2D ViewportSize = FVector2D::ZeroVector;
    FLiveUiSnapshotNode RootNode;

    // Echo of which live UMG root was selected, so a caller that disambiguated a
    // multi-root viewport can confirm the chosen root. Always populated; for the
    // single-root case it names the only candidate.
    FString SelectedRootName;
    int32 SelectedRootIndex = 0;
    int32 RootCandidateCount = 0;
};

class FLiveUiSnapshotService
{
public:
    static bool Capture(const FLiveUiSnapshotRequest& Request, FLiveUiSnapshot& OutSnapshot, FString& OutErrorCode, FString& OutErrorMessage);

    // Pure root-selection logic, factored out so it can be exercised without a live
    // PIE editor. Given the backing-widget names of every collected UMG root
    // candidate (index-aligned to the candidate array) and the request's optional
    // instance_name/root_index selector, resolves which candidate to dump.
    // Returns true and writes OutSelectedIndex on success. On failure returns false
    // and sets OutErrorCode/OutErrorMessage:
    //  - AMBIGUOUS_LIVE_ROOT: multiple candidates and no selector given.
    //  - LIVE_ROOT_NOT_FOUND: a selector was given but matched no candidate.
    // CandidateNames may contain empty strings for candidates whose backing widget
    // could not be resolved; those never match an instance_name but remain
    // addressable by root_index.
    // DisplaySummaries (optional, index-aligned to CandidateNames) supplies richer
    // human-readable labels ("Type -> Type (name)") for the no-selector ambiguity
    // message; when omitted the bare CandidateNames are listed instead. Selection
    // matching always uses CandidateNames, never the summaries.
    static bool SelectRootCandidate(
        const TArray<FString>& CandidateNames,
        const FLiveUiSnapshotRequest& Request,
        int32& OutSelectedIndex,
        FString& OutErrorCode,
        FString& OutErrorMessage,
        const TArray<FString>* DisplaySummaries = nullptr);
};
