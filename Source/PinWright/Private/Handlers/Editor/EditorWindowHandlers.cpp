// Copyright (c) 2026 Alexander Penkin. MIT License.

// EditorWindowHandlers.cpp - editor.frame_graph: frame/zoom the on-screen graph panel of ANY
// node-editor kind. editor.resize_window, editor.set_window_state and editor.screenshot_window are
// in the PinWrightRuntime module (shared helpers in Handlers/Editor/EditorWindowHandlers.h).
// The graph framing here reuses no Blueprint-specific knowledge: it walks the widget tree for
// the first SGraphEditor, gets its live SGraphPanel + UEdGraph generically, and frames against
// the panel's on-screen geometry (never an offscreen render target).

#include "Handlers/Editor/EditorWindowHandlers.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Drive/DriveEditorChrome.h"
#include "Handlers/Drive/DriveHandlerCommon.h"

#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "GraphEditor.h"
#include "Layout/ChildrenBase.h"
#include "Layout/Geometry.h"
#include "Math/UnrealMathUtility.h"
#include "Compat/EngineVersionCompat.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Modules/ModuleManager.h"
#include "Rendering/SlateRenderer.h"
#include "SGraphPanel.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/Docking/SDockTab.h"

// Named (not anonymous) helper namespace: Unity merges .cpp files into one TU, so the shared
// helpers below live in a uniquely-named namespace to dodge ODR collisions with same-named
// helpers elsewhere in the module (the plugin's other shared-helper clusters do the same).
namespace EditorWindowHandlersLocal
{
    // UE <= 5.5: the SGraphEditor / SGraphPanel view and bounds APIs take FVector2D (the
    // FVector2f overloads arrived with the 5.6 Slate float-vector migration). These shims keep
    // the handler logic below uniformly in FVector2f on every supported engine.
    void SetGraphViewLocation(const TSharedPtr<SGraphEditor>& GraphEditor, const FVector2f& Location, float Zoom)
    {
#if UE_VERSION_OLDER_THAN(5, 6, 0)
        GraphEditor->SetViewLocation(FVector2D(Location), Zoom);
#else
        GraphEditor->SetViewLocation(Location, Zoom);
#endif
    }

    void GetGraphViewLocation(const TSharedPtr<SGraphEditor>& GraphEditor, FVector2f& OutLocation, float& OutZoom)
    {
#if UE_VERSION_OLDER_THAN(5, 6, 0)
        FVector2D Location(OutLocation);
        GraphEditor->GetViewLocation(Location, OutZoom);
        OutLocation = FVector2f(Location);
#else
        GraphEditor->GetViewLocation(OutLocation, OutZoom);
#endif
    }

    bool GetGraphNodeBounds(const SGraphPanel* GraphPanel, const UObject* Node, FVector2f& OutMin, FVector2f& OutMax)
    {
#if UE_VERSION_OLDER_THAN(5, 6, 0)
        FVector2D NodeMin(OutMin);
        FVector2D NodeMax(OutMax);
        if (!GraphPanel->GetBoundsForNode(Node, NodeMin, NodeMax, 0.0f))
        {
            return false;
        }
        OutMin = FVector2f(NodeMin);
        OutMax = FVector2f(NodeMax);
        return true;
#else
        return GraphPanel->GetBoundsForNode(Node, OutMin, OutMax, 0.0f);
#endif
    }

    // Read a nested {x, y} object field (Obj->Field->{x, y}) into an FVector2f. Returns false if
    // the field is missing or either coordinate is non-numeric. Salvaged verbatim.
    bool ReadPointField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, FVector2f& Out)
    {
        if (!Obj.IsValid())
        {
            return false;
        }
        const TSharedPtr<FJsonObject>* Point = nullptr;
        if (!Obj->TryGetObjectField(Field, Point) || !Point || !(*Point).IsValid())
        {
            return false;
        }
        double X = 0.0;
        double Y = 0.0;
        if (!(*Point)->TryGetNumberField(TEXT("x"), X) || !(*Point)->TryGetNumberField(TEXT("y"), Y))
        {
            return false;
        }
        Out = FVector2f(static_cast<float>(X), static_cast<float>(Y));
        return true;
    }

    // Depth-first walk for the first widget that reports itself as an "SGraphEditor" - the
    // generic node-editor host widget shared by Blueprint, material, Niagara, animation, and
    // other graph editors. Adapted from RenderHandler::FindEditorViewportRecursive; uses
    // GetAllChildren() so it descends through structural panels. StaticCast is safe because the
    // type-string gate guarantees the concrete type.
    TSharedPtr<SGraphEditor> FindGraphEditorRecursive(const TSharedRef<SWidget>& Widget)
    {
        if (Widget->GetTypeAsString() == TEXT("SGraphEditor"))
        {
            return StaticCastSharedRef<SGraphEditor>(Widget);
        }

        FChildren* Children = Widget->GetAllChildren();
        if (!Children)
        {
            return nullptr;
        }
        for (int32 Index = 0; Index < Children->Num(); ++Index)
        {
            TSharedPtr<SGraphEditor> Result = FindGraphEditorRecursive(Children->GetChildAt(Index));
            if (Result.IsValid())
            {
                return Result;
            }
        }
        return nullptr;
    }

    // Fallback for editors that hide the graph behind another tab (e.g. an Actor Blueprint that
    // opens on its Viewport). The active-tab walk above misses inactive tabs because a docking
    // stack only lays out the active tab's content, but the SDockTab widgets themselves are in the
    // tree and each retains its content via GetContent(). Search every tab's retained content for
    // an SGraphEditor and hand back the tab so the caller can bring it to the front.
    TSharedPtr<SGraphEditor> FindGraphEditorViaTabs(const TSharedRef<SWidget>& Widget, TSharedPtr<SDockTab>& OutTab)
    {
        if (Widget->GetTypeAsString() == TEXT("SDockTab"))
        {
            TSharedRef<SDockTab> Tab = StaticCastSharedRef<SDockTab>(Widget);
            TSharedPtr<SGraphEditor> Ge = FindGraphEditorRecursive(Tab->GetContent());
            if (Ge.IsValid())
            {
                OutTab = Tab;
                return Ge;
            }
        }

        FChildren* Children = Widget->GetAllChildren();
        if (!Children)
        {
            return nullptr;
        }
        for (int32 Index = 0; Index < Children->Num(); ++Index)
        {
            TSharedPtr<SGraphEditor> Result = FindGraphEditorViaTabs(Children->GetChildAt(Index), OutTab);
            if (Result.IsValid())
            {
                return Result;
            }
        }
        return nullptr;
    }

    TSharedPtr<FJsonObject> MakeRectObject(const FVector2f& Min, const FVector2f& Max)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetObjectField(TEXT("topLeft"), EditorWindowHandlers::MakePointObject(Min));
        Obj->SetObjectField(TEXT("bottomRight"), EditorWindowHandlers::MakePointObject(Max));
        return Obj;
    }
}

using namespace EditorWindowHandlersLocal;
using namespace EditorWindowHandlers;

// ---- editor.frame_graph ----
REGISTER_RPC_HANDLER("editor.frame_graph", "editor",
    "Frame/zoom the on-screen node graph inside a target editor window - works for ANY node-editor kind "
    "(Blueprint, material, Niagara, animation, etc.), not just Blueprint. Finds the first SGraphEditor in "
    "the selected window and moves its live view. mode: fit_all (default) | nodes | bounds | view. Errors "
    "GRAPH_EDITOR_NOT_FOUND if the window has no graph editor.",
    RPC_PARAMS(
        DRIVE_WINDOW_SELECTOR_PARAMS,
        RPC_PARAM_DEF("mode", "string",
            "Framing mode: 'fit_all' (default, frame every node), 'nodes' (frame the union of nodeIds), "
            "'bounds' (frame an explicit graph-space rect), 'view' (set the view location + zoom directly).", "fit_all"),
        RPC_PARAM_OPT("nodeIds", "array",
            "For mode=nodes, an array of node GUID strings. The union of their graph-space rects is framed. "
            "Missing GUIDs are skipped and reported in nodesNotFound; the request errors NODES_NOT_FOUND only when none match."),
        RPC_PARAM_OPT("bounds", "object",
            "For mode=bounds, an explicit graph-space rect { topLeft: {x, y}, bottomRight: {x, y} } (same space as node positions)."),
        RPC_PARAM_OPT("viewLocation", "object",
            "For mode=view, the top-left graph-space view location { x, y } applied via SetViewLocation with `zoom`."),
        RPC_PARAM_OPT("zoom", "number",
            "For mode=view, the zoom amount (must be strictly positive)."),
        RPC_PARAM_DEF("padding", "number", "Graph-space padding added around the framed rect. Default 48.", "48"),
        RPC_PARAM_OPT("aspect", "string",
            "Optional target aspect ratio (a 'W:H' string like '16:9' or a bare float W/H). When set, the fit "
            "is constrained to the largest box of that ratio inside the live panel (letterbox). Ignored for mode=view.")
    ))
{
    if (!GEditor)
    {
        Ctx.SendError(TEXT("EDITOR_NOT_AVAILABLE"), TEXT("Editor not available"));
        return true;
    }
    if (!FSlateApplication::IsInitialized())
    {
        Ctx.SendError(TEXT("SLATE_NOT_INITIALIZED"), TEXT("Slate application is not initialized"));
        return true;
    }

    FString Mode = Ctx.GetString(TEXT("mode")).ToLower();
    if (Mode.IsEmpty())
    {
        Mode = TEXT("fit_all");
    }
    if (Mode != TEXT("fit_all") && Mode != TEXT("nodes") && Mode != TEXT("bounds") && Mode != TEXT("view"))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("mode must be one of: fit_all, nodes, bounds, view"));
        return true;
    }

    const float Padding = FMath::Max(0.0f, static_cast<float>(Ctx.GetNumber(TEXT("padding"), 48.0)));

    bool bHasAspect = false;
    double AspectRatio = 0.0;
    FString AspectError;
    if (!ParseAspectParam(Ctx, bHasAspect, AspectRatio, AspectError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), AspectError);
        return true;
    }

    // Resolve the target window with the shared selector (index > title > active).
    TSharedPtr<SWindow> Window;
    FString WindowTitle;
    FString ErrorCode;
    FString ErrorMessage;
    if (!FDriveEditorChrome::ResolveWindow(
            FDriveHandlerCommon::ParseWindowSelector(Ctx), Window, WindowTitle, ErrorCode, ErrorMessage))
    {
        Ctx.SendError(ErrorCode, ErrorMessage);
        return true;
    }

    TSharedPtr<SGraphEditor> GraphEditor = FindGraphEditorRecursive(Window.ToSharedRef());
    if (!GraphEditor.IsValid())
    {
        // The graph may live in an inactive tab (e.g. a Blueprint that opened on its Viewport).
        // Find it via the tab's retained content, bring that tab to the front, then pump so the
        // newly-shown graph panel realizes before we frame it.
        TSharedPtr<SDockTab> GraphTab;
        GraphEditor = FindGraphEditorViaTabs(Window.ToSharedRef(), GraphTab);
        if (GraphEditor.IsValid() && GraphTab.IsValid())
        {
            GraphTab->ActivateInParent(ETabActivationCause::SetDirectly);
            for (int32 Activate = 0; Activate < 3; ++Activate)
            {
                PumpEditorSlate(Window);
            }
        }
    }
    if (!GraphEditor.IsValid())
    {
        Ctx.SendError(TEXT("GRAPH_EDITOR_NOT_FOUND"),
            FString::Printf(TEXT("No SGraphEditor found in window '%s'"), *WindowTitle));
        return true;
    }

    SGraphPanel* GraphPanel = GraphEditor->GetGraphPanel();
    UEdGraph* Graph = GraphEditor->GetCurrentGraph();
    if (!GraphPanel || !Graph)
    {
        Ctx.SendError(TEXT("GRAPH_EDITOR_NOT_FOUND"),
            FString::Printf(TEXT("Graph editor in window '%s' has no live graph panel"), *WindowTitle));
        return true;
    }

    // Prime the deferred panel layout so node widgets are realized before we query bounds.
    for (int32 Prime = 0; Prime < 3; ++Prime)
    {
        PumpEditorSlate(Window);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("mode"), Mode);
    Result->SetStringField(TEXT("windowTitle"), WindowTitle);

    // mode=view: set the view directly, no bounds query.
    if (Mode == TEXT("view"))
    {
        FVector2f ViewLoc;
        if (!ReadPointField(Ctx.GetRawPayload(), TEXT("viewLocation"), ViewLoc))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("mode=view requires viewLocation: { x, y }"));
            return true;
        }
        const float Zoom = static_cast<float>(Ctx.GetNumber(TEXT("zoom"), 0.0));
        if (Zoom <= KINDA_SMALL_NUMBER)
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("mode=view requires a strictly positive zoom"));
            return true;
        }

        SetGraphViewLocation(GraphEditor, ViewLoc, Zoom);
        PumpEditorSlate(Window);

        FVector2f AppliedView = ViewLoc;
        float AppliedZoom = Zoom;
        GetGraphViewLocation(GraphEditor, AppliedView, AppliedZoom);
        Result->SetNumberField(TEXT("appliedZoom"), AppliedZoom);
        Result->SetObjectField(TEXT("appliedView"), MakePointObject(AppliedView));
        Ctx.SendSuccess(Result);
        return true;
    }

    // Parse mode-specific inputs up front so bad args fail before any view move.
    FVector2f BoundsMin(0.0f, 0.0f);
    FVector2f BoundsMax(0.0f, 0.0f);
    if (Mode == TEXT("bounds"))
    {
        TSharedPtr<FJsonObject> BoundsObj = Ctx.GetObject(TEXT("bounds"));
        FVector2f TopLeft, BottomRight;
        if (!BoundsObj.IsValid() ||
            !ReadPointField(BoundsObj, TEXT("topLeft"), TopLeft) ||
            !ReadPointField(BoundsObj, TEXT("bottomRight"), BottomRight))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                TEXT("mode=bounds requires bounds: { topLeft: {x, y}, bottomRight: {x, y} }"));
            return true;
        }
        BoundsMin = FVector2f(FMath::Min(TopLeft.X, BottomRight.X), FMath::Min(TopLeft.Y, BottomRight.Y));
        BoundsMax = FVector2f(FMath::Max(TopLeft.X, BottomRight.X), FMath::Max(TopLeft.Y, BottomRight.Y));
    }

    // mode=nodes: match requested GUIDs against the live graph's nodes.
    TArray<UEdGraphNode*> FramedNodes;
    TArray<FString> NodesNotFound;
    if (Mode == TEXT("nodes"))
    {
        const TArray<TSharedPtr<FJsonValue>>* NodeIdArray = Ctx.GetArray(TEXT("nodeIds"));
        if (!NodeIdArray || NodeIdArray->Num() == 0)
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                TEXT("mode=nodes requires a non-empty nodeIds array of node GUID strings"));
            return true;
        }
        for (const TSharedPtr<FJsonValue>& Value : *NodeIdArray)
        {
            FString GuidStr;
            if (!Value.IsValid() || !Value->TryGetString(GuidStr) || GuidStr.TrimStartAndEnd().IsEmpty())
            {
                continue;
            }
            GuidStr = GuidStr.TrimStartAndEnd();

            FGuid Guid;
            UEdGraphNode* Matched = nullptr;
            if (FGuid::Parse(GuidStr, Guid))
            {
                for (UEdGraphNode* Node : Graph->Nodes)
                {
                    if (Node && Node->NodeGuid == Guid)
                    {
                        Matched = Node;
                        break;
                    }
                }
            }
            if (Matched)
            {
                FramedNodes.AddUnique(Matched);
            }
            else
            {
                NodesNotFound.Add(GuidStr);
            }
        }
        if (FramedNodes.Num() == 0)
        {
            Ctx.SendError(TEXT("NODES_NOT_FOUND"),
                FString::Printf(TEXT("None of the requested nodeIds were found in the graph of window '%s'"), *WindowTitle));
            return true;
        }
    }

    // Compute the framed graph-space rect, retrying pumps until node widgets realize with a
    // positive extent (a widget can briefly report zero desired-size right after realization).
    auto HasPositiveExtent = [](const FVector2f& Min, const FVector2f& Max)
    {
        return (Max.X - Min.X) > KINDA_SMALL_NUMBER && (Max.Y - Min.Y) > KINDA_SMALL_NUMBER;
    };

    FVector2f RMin(0.0f, 0.0f);
    FVector2f RMax(0.0f, 0.0f);
    bool bHaveRect = false;
    const int32 MaxRealizeAttempts = 6;
    for (int32 Attempt = 0; Attempt < MaxRealizeAttempts && !bHaveRect; ++Attempt)
    {
        if (Mode == TEXT("bounds"))
        {
            RMin = BoundsMin;
            RMax = BoundsMax;
            bHaveRect = true;
        }
        else if (Mode == TEXT("nodes"))
        {
            FVector2f UnionMin(TNumericLimits<float>::Max(), TNumericLimits<float>::Max());
            FVector2f UnionMax(TNumericLimits<float>::Lowest(), TNumericLimits<float>::Lowest());
            bool bAny = false;
            for (UEdGraphNode* Node : FramedNodes)
            {
                FVector2f NodeMin, NodeMax;
                if (GetGraphNodeBounds(GraphPanel, Node, NodeMin, NodeMax))
                {
                    UnionMin.X = FMath::Min(UnionMin.X, NodeMin.X);
                    UnionMin.Y = FMath::Min(UnionMin.Y, NodeMin.Y);
                    UnionMax.X = FMath::Max(UnionMax.X, NodeMax.X);
                    UnionMax.Y = FMath::Max(UnionMax.Y, NodeMax.Y);
                    bAny = true;
                }
            }
            if (bAny && HasPositiveExtent(UnionMin, UnionMax))
            {
                RMin = UnionMin;
                RMax = UnionMax;
                bHaveRect = true;
            }
        }
        else // fit_all: union every node's bounds. SNodePanel::GetBoundsForNodes (plural) is
             // protected, so accumulate via the public per-node GetBoundsForNode instead.
        {
            FVector2f AllMin(TNumericLimits<float>::Max(), TNumericLimits<float>::Max());
            FVector2f AllMax(TNumericLimits<float>::Lowest(), TNumericLimits<float>::Lowest());
            bool bAny = false;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                FVector2f NodeMin, NodeMax;
                if (Node && GetGraphNodeBounds(GraphPanel, Node, NodeMin, NodeMax))
                {
                    AllMin.X = FMath::Min(AllMin.X, NodeMin.X);
                    AllMin.Y = FMath::Min(AllMin.Y, NodeMin.Y);
                    AllMax.X = FMath::Max(AllMax.X, NodeMax.X);
                    AllMax.Y = FMath::Max(AllMax.Y, NodeMax.Y);
                    bAny = true;
                }
            }
            if (bAny && HasPositiveExtent(AllMin, AllMax))
            {
                RMin = AllMin;
                RMax = AllMax;
                bHaveRect = true;
            }
        }

        if (!bHaveRect)
        {
            PumpEditorSlate(Window);
        }
    }

    if (!bHaveRect)
    {
        Ctx.SendError(TEXT("BOUNDS_EMPTY"),
            FString::Printf(TEXT("Could not resolve node bounds in window '%s' (no realized node widgets)"), *WindowTitle));
        return true;
    }

    // Uniform graph-space padding around the rect (all bound-based modes).
    RMin -= FVector2f(Padding, Padding);
    RMax += FVector2f(Padding, Padding);

    const FVector2f RSize = RMax - RMin;
    const FVector2f RCenter = (RMin + RMax) * 0.5f;
    if (RSize.X <= KINDA_SMALL_NUMBER || RSize.Y <= KINDA_SMALL_NUMBER)
    {
        Ctx.SendError(TEXT("BOUNDS_EMPTY"),
            FString::Printf(TEXT("Framed rect for window '%s' is degenerate (zero size)"), *WindowTitle));
        return true;
    }

    // Frame against the LIVE on-screen panel size, not a fixed offscreen canvas.
    const FVector2f PanelSize = GraphPanel->GetTickSpaceGeometry().GetLocalSize();
    if (PanelSize.X <= KINDA_SMALL_NUMBER || PanelSize.Y <= KINDA_SMALL_NUMBER)
    {
        Ctx.SendError(TEXT("PANEL_NOT_REALIZED"),
            FString::Printf(TEXT("Graph panel in window '%s' has no on-screen size yet"), *WindowTitle));
        return true;
    }

    // When aspect is requested, constrain the fit to the largest box of that ratio inside the
    // panel (letterbox); otherwise fit the whole panel. Centering always uses the full panel.
    FVector2f Canvas = PanelSize;
    if (bHasAspect)
    {
        const float A = static_cast<float>(AspectRatio);
        float BoxW = PanelSize.X;
        float BoxH = BoxW / A;
        if (BoxH > PanelSize.Y)
        {
            BoxH = PanelSize.Y;
            BoxW = BoxH * A;
        }
        Canvas = FVector2f(BoxW, BoxH);
    }

    // Fit-contain zoom with quantization read-back: SetViewLocation snaps zoom to a discrete
    // level, so after the first call we read back the actual zoom and re-center against it.
    const float DesiredZoom = FMath::Max(
        KINDA_SMALL_NUMBER,
        FMath::Min(Canvas.X / RSize.X, Canvas.Y / RSize.Y));

    FVector2f ProvisionalOffset = RCenter - (PanelSize * 0.5f) / DesiredZoom;
    SetGraphViewLocation(GraphEditor, ProvisionalOffset, DesiredZoom);

    float ActualZoom = DesiredZoom;
    {
        FVector2f ReadOffset;
        float ReadZoom = 0.0f;
        GetGraphViewLocation(GraphEditor, ReadOffset, ReadZoom);
        if (ReadZoom > KINDA_SMALL_NUMBER)
        {
            ActualZoom = ReadZoom;
        }
    }

    const FVector2f ViewOffset = RCenter - (PanelSize * 0.5f) / ActualZoom;
    SetGraphViewLocation(GraphEditor, ViewOffset, ActualZoom);
    PumpEditorSlate(Window);

    Result->SetObjectField(TEXT("framed"), MakeRectObject(RMin, RMax));
    Result->SetNumberField(TEXT("appliedZoom"), ActualZoom);
    Result->SetObjectField(TEXT("appliedView"), MakePointObject(ViewOffset));
    if (Mode == TEXT("nodes"))
    {
        Result->SetNumberField(TEXT("nodesFramed"), FramedNodes.Num());
        if (NodesNotFound.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> NotFoundArr;
            for (const FString& Missing : NodesNotFound)
            {
                NotFoundArr.Add(MakeShared<FJsonValueString>(Missing));
            }
            Result->SetArrayField(TEXT("nodesNotFound"), NotFoundArr);
        }
    }
    Ctx.SendSuccess(Result);
    return true;
}

