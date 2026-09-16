// Copyright (c) 2026 Alexander Penkin. MIT License.

// WidgetXmlExportHandler.cpp
// Exports a widget blueprint's UMG tree as XML/HTML-like markup with all overridden properties.


#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/UI/LiveUiSnapshot.h"
#include "Handlers/UI/LiveUiSnapshotXmlWriter.h"
#include "Handlers/UI/WidgetAuthoringUtils.h"
#include "Handlers/UI/WidgetHandlerUtils.h"
#include "Handlers/UI/WidgetXmlUtils.h"
#include "Handlers/UI/WidgetInspectHelpers.h"
#include "Handlers/UI/WidgetXmlExporter.h"
#include "Handlers/UI/WidgetGeometryResolver.h"
#include "Utils/AssetDumpSuggestion.h"
#include "Blueprint/WidgetTree.h"
#include "WidgetBlueprint.h"
#include "Components/PanelWidget.h"
#include "Misc/EngineVersionComparison.h"

using namespace WidgetAuthoringHelpers;
using namespace WidgetHandlerUtils;
using namespace WidgetXmlHelpers;

namespace
{
    using WidgetInspectHelpers::GeoFloat;
}

// ---- widget.export_xml ----
REGISTER_RPC_HANDLER("widget.export_xml", "widget",
    "Export a widget blueprint tree as XML/HTML-like markup with all overridden properties",
    RPC_PARAMS(
        WidgetAssetPathParamOpt(TEXT("Path to the widget blueprint")),
        RPC_PARAM_OPT("widget_path", "path", "Alias for widgetPath when capture_source=asset"),
        RPC_PARAM_OPT("asset_path", "path", "Alias for widgetPath when capture_source=asset"),
        RPC_PARAM_OPT("widgetName", "string", "Export only subtree rooted at this widget (canonical; aliases: name, slotName, widget_name, targetName)"),
        RPC_PARAM_OPT("widget_name", "string", "Alias for widgetName"),
        RPC_PARAM_OPT("name", "string", "Alias for widgetName"),
        RPC_PARAM_OPT("slotName", "string", "Alias for widgetName"),
        RPC_PARAM_OPT("targetName", "string", "Alias for widgetName"),
        RPC_PARAM_OPT("include_defaults", "boolean", "Include all properties, not just overridden (default false)"),
        RPC_PARAM_OPT("resolve_geometry", "bool|object",
            "Resolve window-relative geometry (off by default). Pass bool true to enable with defaults, "
            "or object {viewport_size: {w,h}, force_visible_for_measure: bool, instance_name: string} for fine control."),
        RPC_PARAM_OPT("resolveGeometry", "bool|object", "Alias for resolve_geometry"),
        RPC_PARAM_OPT("capture_source", "string",
            "Capture source: 'asset' (default) or 'live' for the active PIE UMG-as-root subtree"),
        RPC_PARAM_OPT("verbose", "boolean",
            "When capture_source=live, include default-valued runtime state (default false)"),
        RPC_PARAM_OPT("include_geometry", "boolean",
            "When capture_source=live, include live geometry in the snapshot (default false)"),
        RPC_PARAM_OPT("instance_name", "string",
            "When capture_source=live and multiple UMG roots are on screen, select the root "
            "whose backing widget name contains this value (e.g. the key ui.create_hud returns). "
            "Resolves AMBIGUOUS_LIVE_ROOT."),
        RPC_PARAM_OPT("instanceName", "string", "Alias for instance_name"),
        RPC_PARAM_OPT("root_index", "integer",
            "When capture_source=live, select the Nth live UMG root (0-based, in collection order). "
            "Alternative to instance_name for disambiguating AMBIGUOUS_LIVE_ROOT."),
        RPC_PARAM_OPT("rootIndex", "integer", "Alias for root_index"),
        RPC_PARAM_OPT("omit_slot_chain", "integer",
            "Drop the Slot.Parent/Slot.Content sibling-chain expansion that causes O(N^2) blow-up on panel-heavy widgets (default false)"),
        RPC_PARAM_OPT("compact", "boolean",
            "Alias for omit_slot_chain")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    TArray<FString> AssetPathKeys = WidgetAssetPathParamNames();
    AppendLegacyWidgetAssetPathSnakeAliases(AssetPathKeys);
    const bool bHasCaptureSource = Payload.IsValid() && Payload->HasField(TEXT("capture_source"));
    const FString RequestedCaptureSource = bHasCaptureSource
        ? Ctx.GetString(TEXT("capture_source"))
        : FString();
    if (bHasCaptureSource && RequestedCaptureSource.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PARAMETER"),
            TEXT("capture_source must be 'asset' or 'live'"));
        return true;
    }

    const FString CaptureSource = bHasCaptureSource
        ? RequestedCaptureSource
        : TEXT("asset");
    if (CaptureSource != TEXT("asset") && CaptureSource != TEXT("live"))
    {
        Ctx.SendError(TEXT("INVALID_PARAMETER"),
            TEXT("capture_source must be 'asset' or 'live'"));
        return true;
    }

    if (CaptureSource == TEXT("live"))
    {
        for (const FString& AssetPathKey : AssetPathKeys)
        {
            if (Payload.IsValid() && Payload->HasField(AssetPathKey))
            {
                Ctx.SendError(TEXT("INVALID_PARAMETER"),
                    TEXT("widgetPath, assetPath, path, blueprintPath, blueprint_path, requestedPath, widget_path, and asset_path are not accepted when capture_source=live"));
                return true;
            }
        }

        const FLiveUiSnapshotRequest Request = FLiveUiSnapshotRequest::FromContext(Ctx);

        FLiveUiSnapshot Snapshot;
        FString ErrorCode;
        FString ErrorMessage;
        if (!FLiveUiSnapshotService::Capture(Request, Snapshot, ErrorCode, ErrorMessage))
        {
            Ctx.SendError(ErrorCode, ErrorMessage);
            return true;
        }

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("xml"), FLiveUiSnapshotXmlWriter::Write(Snapshot));
        Ctx.SendSuccess(Result);
        return true;
    }

    if (Payload.IsValid() &&
        (Payload->HasField(TEXT("verbose")) || Payload->HasField(TEXT("include_geometry"))))
    {
        Ctx.SendError(TEXT("INVALID_PARAMETER"),
            TEXT("verbose and include_geometry are only accepted when capture_source=live"));
        return true;
    }

    const FString WidgetPath = Ctx.GetStringFirstOf(AssetPathKeys);
    if (WidgetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAMETER"),
            TEXT("Missing required parameter: widgetPath"));
        return true;
    }

    const FString SubtreeRoot = Ctx.GetStringFirstOf({TEXT("widgetName"), TEXT("widget_name"), TEXT("name"), TEXT("slotName"), TEXT("targetName")});
    const bool bIncludeDefaults = Ctx.GetBool(TEXT("include_defaults"), false);
    const bool bOmitSlotChain = Ctx.GetBool(TEXT("omit_slot_chain"), false) || Ctx.GetBool(TEXT("compact"), false);

    TSharedPtr<FJsonValue> GeoValue = Ctx.GetJsonValueFirstOf(
        {TEXT("resolve_geometry"), TEXT("resolveGeometry")});

    UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(WidgetPath);
    if (!WidgetBP || !WidgetBP->WidgetTree)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Widget blueprint not found"));
        return true;
    }

    UWidget* StartWidget = nullptr;
    FString InheritedFromPath;
    if (!SubtreeRoot.IsEmpty())
    {
        StartWidget = WidgetBP->WidgetTree->FindWidget(FName(*SubtreeRoot));
        if (!StartWidget)
        {
            Ctx.SendError(TEXT("WIDGET_NOT_FOUND"),
                FString::Printf(TEXT("Widget '%s' not found in tree"), *SubtreeRoot));
            return true;
        }
    }
    else
    {
        WidgetXmlExporter::FWidgetTreeRootResolution RootResolution =
            WidgetXmlExporter::ResolveWidgetTreeRoot(WidgetBP);
        StartWidget = RootResolution.StartWidget;
        InheritedFromPath = RootResolution.InheritedFromPath;
        if (!StartWidget)
        {
            if (RootResolution.bEmptyByDesign)
            {
                TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
                Result->SetStringField(TEXT("xml"), FString());
                Result->SetNumberField(TEXT("widget_count"), 0);
                if (FString DumpHint = AssetDumpSuggestion::BuildDumpSuggestionHint(WidgetPath, AssetDumpSuggestion::EDumpSubjectKind::Asset); !DumpHint.IsEmpty())
                {
                    Result->SetStringField(TEXT("hint"), DumpHint);
                }
                Ctx.SendSuccess(Result);
                return true;
            }

            Ctx.SendError(TEXT("TREE_EMPTY"), RootResolution.Reason);
            return true;
        }
    }

    TMap<FString, TArray<TPair<FString, FString>>> WidgetBindingMap =
        WidgetXmlExporter::BuildBindingMap(WidgetBP);

    // Resolve variable map (editor-only data).
    // UE 5.4/5.5 has no WidgetVariableNameToGuidMap; pass nullptr so BuildXmlString
    // falls back to UWidget::bIsVariable for the IsVariable attribute.
#if UE_VERSION_OLDER_THAN(5, 6, 0)
    const TMap<FName, FGuid>* VariableGuidMap = nullptr;
#else
    const TMap<FName, FGuid>* VariableGuidMap = &WidgetBP->WidgetVariableNameToGuidMap;
#endif

    FWidgetGeometryRequest GeoReq;
    const bool bGeometryEnabled = FWidgetGeometryResolver::ParseRequest(GeoValue, WidgetBP, GeoReq);

    FWidgetGeometryResult GeoResult;
    TMap<FName, UWidget*> GeoNameIndex;
    if (bGeometryEnabled)
    {
        GeoResult = FWidgetGeometryResolver::Resolve(GeoReq);
        FWidgetGeometryResolver::BuildNameIndex(GeoResult.GetLiveRoot(), GeoNameIndex);
    }
    const bool bGeoAmbiguous = GeoResult.bAmbiguous;

    // Pre-walk the asset tree to tally per-node geometry statuses. This lets us emit the
    // summary counts on the root element in a single pass (no post-walk string patching).
    int32 GeoCountOk = 0, GeoCountSkipped = 0, GeoCountError = 0;
    if (bGeometryEnabled && !bGeoAmbiguous)
    {
        TFunction<void(UWidget*)> CountNode = [&](UWidget* W)
        {
            if (!W) return;
            if (GeoNameIndex.Num() > 0)
            {
                if (const FResolvedGeometry* Found = FWidgetGeometryResolver::LookupByName(
                        GeoResult.ByWidget, GeoNameIndex, W->GetFName()))
                {
                    switch (Found->Status)
                    {
                    case FResolvedGeometry::EStatus::Ok:      ++GeoCountOk;      break;
                    case FResolvedGeometry::EStatus::Skipped: ++GeoCountSkipped; break;
                    default:                                   ++GeoCountError;   break;
                    }
                }
                else
                {
                    ++GeoCountError;
                }
            }
            else
            {
                ++GeoCountError;
            }
            if (UPanelWidget* Panel = Cast<UPanelWidget>(W))
            {
                const int32 N = Panel->GetChildrenCount();
                for (int32 i = 0; i < N; ++i)
                {
                    CountNode(Panel->GetChildAt(i));
                }
            }
        };
        CountNode(StartWidget);
    }

    TArray<TPair<FString, FString>> RootGeomAttrs;
    if (!InheritedFromPath.IsEmpty())
    {
        RootGeomAttrs.Emplace(TEXT("inherited_from"), XmlEscapeAttribute(InheritedFromPath));
    }

    if (!bGeometryEnabled)
    {
        RootGeomAttrs.Emplace(TEXT("Geom.source"), TEXT("off"));
    }
    else if (bGeoAmbiguous)
    {
        RootGeomAttrs.Emplace(TEXT("Geom.source"), TEXT("ambiguous"));
        RootGeomAttrs.Emplace(TEXT("Geom.ambiguous"), TEXT("true"));
        RootGeomAttrs.Emplace(TEXT("Geom.top_level_error"), TEXT("AMBIGUOUS_INSTANCE"));
    }
    else
    {
        RootGeomAttrs.Emplace(TEXT("Geom.source"),
            XmlEscapeAttribute(FWidgetGeometryResolver::SourceToString(GeoResult.ServedBy)));
        RootGeomAttrs.Emplace(TEXT("Geom.viewport_w"), GeoFloat(GeoResult.ViewportSizeUsed.X));
        RootGeomAttrs.Emplace(TEXT("Geom.viewport_h"), GeoFloat(GeoResult.ViewportSizeUsed.Y));
        RootGeomAttrs.Emplace(TEXT("Geom.dpi_scale"),  GeoFloat(GeoResult.DpiScale));

        if (!GeoResult.TopLevelError.IsEmpty())
        {
            RootGeomAttrs.Emplace(TEXT("Geom.top_level_error"),
                XmlEscapeAttribute(GeoResult.TopLevelError));
        }

        const int32 GeoTotal = GeoCountOk + GeoCountSkipped + GeoCountError;
        RootGeomAttrs.Emplace(TEXT("Geom.summary_ok"),      FString::Printf(TEXT("%d"), GeoCountOk));
        RootGeomAttrs.Emplace(TEXT("Geom.summary_skipped"), FString::Printf(TEXT("%d"), GeoCountSkipped));
        RootGeomAttrs.Emplace(TEXT("Geom.summary_error"),   FString::Printf(TEXT("%d"), GeoCountError));
        RootGeomAttrs.Emplace(TEXT("Geom.summary_total"),   FString::Printf(TEXT("%d"), GeoTotal));
    }

    WidgetXmlExporter::FGeomContext GeomCtx;
    GeomCtx.bEnabled   = bGeometryEnabled;
    GeomCtx.bAmbiguous = bGeoAmbiguous;
    GeomCtx.Result     = &GeoResult;
    GeomCtx.NameIndex  = &GeoNameIndex;

    int32 WidgetCount = 0;
    FString XmlOutput = WidgetXmlExporter::BuildXmlString(
        StartWidget, 0, bIncludeDefaults, WidgetBindingMap, VariableGuidMap, WidgetCount,
        GeomCtx, /*bIsRoot=*/true, RootGeomAttrs, bOmitSlotChain);

    // Ambiguity candidates emit as a sibling block after the tree.
    if (bGeometryEnabled && bGeoAmbiguous)
    {
        XmlOutput += TEXT("<Candidates>\n");
        for (const TWeakObjectPtr<UUserWidget>& Candidate : GeoResult.AmbiguousCandidates)
        {
            if (!Candidate.IsValid()) continue;
            const FString CandName  = XmlEscapeAttribute(Candidate->GetName());
            const FString CandOuter = Candidate->GetOuter()
                ? XmlEscapeAttribute(Candidate->GetOuter()->GetName())
                : FString();
            XmlOutput += FString::Printf(
                TEXT("  <Candidate name=\"%s\" outer=\"%s\" />\n"),
                *CandName, *CandOuter);
        }
        XmlOutput += TEXT("</Candidates>\n");
    }

    // Remove trailing newline for cleaner output
    XmlOutput.TrimEndInline();

    if (!InheritedFromPath.IsEmpty())
    {
        XmlOutput.InsertAt(0, FString::Printf(TEXT("<!-- inherited from %s -->\n"), *InheritedFromPath));
    }

    // Build result
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("xml"), XmlOutput);
    Result->SetNumberField(TEXT("widget_count"), WidgetCount);

    if (FString DumpHint = AssetDumpSuggestion::BuildDumpSuggestionHint(WidgetPath, AssetDumpSuggestion::EDumpSubjectKind::Asset); !DumpHint.IsEmpty())
    {
        Result->SetStringField(TEXT("hint"), DumpHint);
    }

    Ctx.SendSuccess(Result);
    return true;
}
