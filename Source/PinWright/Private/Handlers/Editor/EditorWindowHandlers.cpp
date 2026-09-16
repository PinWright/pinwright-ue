// Copyright (c) 2026 Alexander Penkin. MIT License.

// EditorWindowHandlers.cpp - Generic editor-window RPCs that work for ANY node-editor kind
// and any top-level editor window: frame/zoom the on-screen graph panel (editor.frame_graph),
// resize a window's client area (editor.resize_window), and capture a full window to PNG
// (editor.screenshot_window). Explicit selectors use the shared FDriveEditorChrome::ResolveWindow
// rules; screenshot_window alone defaults an omitted selector to the exact main editor frame.
// The graph framing here reuses no Blueprint-specific knowledge: it walks the widget tree for
// the first SGraphEditor, gets its live SGraphPanel + UEdGraph generically, and frames against
// the panel's on-screen geometry (never an offscreen render target).

#include "Handlers/Editor/EditorWindowHandlers.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Drive/DriveEditorChrome.h"
#include "Handlers/Drive/DriveHandlerCommon.h"
#include "Utils/ScreenshotUtils.h"

#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "GraphEditor.h"
#include "ImageUtils.h"
#include "Interfaces/IMainFrameModule.h"
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
    // Node/graph editors build their node widgets via a deferred SGraphPanel::Update() active
    // timer, and bounds/desired-sizes are valid only after a SlatePrepass. Pump one full frame
    // (and force-redraw the host window) so the panel paints before we query bounds or capture.
    // Salvaged from the deleted BlueprintGraphScreenshotHandler::PumpGraphEditorSlate.
    void PumpEditorSlate(const TSharedPtr<SWindow>& HostWindow)
    {
        FSlateApplication& SlateApp = FSlateApplication::Get();
        SlateApp.PumpMessages();
        SlateApp.Tick(ESlateTickType::All);
        if (HostWindow.IsValid())
        {
            SlateApp.ForceRedrawWindow(HostWindow.ToSharedRef());
        }
        if (FSlateRenderer* Renderer = SlateApp.GetRenderer())
        {
            Renderer->FlushCommands();
        }
    }

    // The three window-manager states as one canonical name, so editor.set_window_state can
    // publish what the window MEASURED as after the call in the same vocabulary the caller used
    // to ask for it. Minimized wins over maximized: Windows keeps the zoomed flag set on a
    // window it iconifies (::IsZoomed stays true under ::IsIconic), so testing maximized first
    // would report a minimized-from-maximized window as "maximized" - the exact misreport this
    // readback exists to prevent.
    FString DescribeEditorWindowStateName(bool bMinimized, bool bMaximized)
    {
        if (bMinimized)
        {
            return TEXT("minimized");
        }
        return bMaximized ? TEXT("maximized") : TEXT("restored");
    }

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

    // Parse the optional `aspect` param: either a "W:H" string (e.g. "16:9") or a bare positive
    // float ratio (W/H). Sets bHasAspect + Ratio. Returns false with OutError only when aspect is
    // present but malformed; an absent aspect returns true with bHasAspect=false. Salvaged from
    // the deleted handler's aspect parsing.
    bool ParseAspectParam(const FHandlerContext& Ctx, bool& bHasAspect, double& Ratio, FString& OutError)
    {
        bHasAspect = false;
        Ratio = 0.0;
        OutError.Reset();

        const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
        if (!Payload.IsValid())
        {
            return true;
        }
        const TSharedPtr<FJsonValue>* AspectField = Payload->Values.Find(TEXT("aspect"));
        if (!AspectField || !AspectField->IsValid())
        {
            return true;
        }

        const TSharedPtr<FJsonValue>& AspectVal = *AspectField;
        if (AspectVal->Type == EJson::String)
        {
            const FString AspectStr = AspectVal->AsString();
            FString WPart, HPart;
            if (AspectStr.Split(TEXT(":"), &WPart, &HPart))
            {
                const double W = FCString::Atod(*WPart.TrimStartAndEnd());
                const double H = FCString::Atod(*HPart.TrimStartAndEnd());
                if (W > 0.0 && H > 0.0)
                {
                    Ratio = W / H;
                    bHasAspect = true;
                }
            }
            else
            {
                const double R = FCString::Atod(*AspectStr.TrimStartAndEnd());
                if (R > 0.0)
                {
                    Ratio = R;
                    bHasAspect = true;
                }
            }
        }
        else if (AspectVal->Type == EJson::Number)
        {
            Ratio = AspectVal->AsNumber();
            bHasAspect = Ratio > 0.0;
        }

        if (!bHasAspect)
        {
            OutError = TEXT("aspect must be a positive 'W:H' string (e.g. '16:9') or a positive float ratio");
            return false;
        }
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

    TSharedPtr<FJsonObject> MakePointObject(const FVector2f& P)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), P.X);
        Obj->SetNumberField(TEXT("y"), P.Y);
        return Obj;
    }

    TSharedPtr<FJsonObject> MakeRectObject(const FVector2f& Min, const FVector2f& Max)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetObjectField(TEXT("topLeft"), MakePointObject(Min));
        Obj->SetObjectField(TEXT("bottomRight"), MakePointObject(Max));
        return Obj;
    }
}

using namespace EditorWindowHandlersLocal;

bool EditorWindowHandlers::ResolveScreenshotWindow(
    const FDriveWindowSelector& Selector,
    TSharedPtr<SWindow>& OutWindow,
    FString& OutTitle,
    FString& OutErrorCode,
    FString& OutErrorMessage)
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

    if (!Selector.Title.IsEmpty() || Selector.Index.IsSet())
    {
        return FDriveEditorChrome::ResolveWindow(
            Selector, OutWindow, OutTitle, OutErrorCode, OutErrorMessage);
    }

    IMainFrameModule* MainFrame = FModuleManager::GetModulePtr<IMainFrameModule>(TEXT("MainFrame"));
    if (MainFrame && MainFrame->IsWindowInitialized())
    {
        OutWindow = MainFrame->GetParentWindow();
    }
    if (!OutWindow.IsValid() || !OutWindow->IsVisible() || OutWindow->IsWindowMinimized())
    {
        OutWindow.Reset();
        OutErrorCode = TEXT("WINDOW_NOT_FOUND");
        OutErrorMessage = TEXT("The main editor frame is unavailable, hidden, or minimized");
        return false;
    }

    OutTitle = OutWindow->GetTitle().ToString();
    return true;
}

void EditorWindowHandlers::SetScreenshotWindowIdentityFields(
    FJsonObject& Result, const FString& WindowTitle, EWindowType WindowType)
{
    Result.SetStringField(TEXT("windowTitle"), WindowTitle);
    Result.SetStringField(TEXT("windowType"), FDriveEditorChrome::WindowTypeToString(WindowType));
}

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

// ---- editor.resize_window ----
REGISTER_RPC_HANDLER("editor.resize_window", "editor",
    "Resize a top-level editor window's CLIENT area (chrome excluded). Target the window with the shared "
    "window selector. Provide width/height (px) and/or an aspect ratio; pass logical=true to treat the "
    "sizes as DPI-independent (multiplied by the window DPI scale for the physical resize). A maximized "
    "window errors WINDOW_MAXIMIZED (restore it first via editor.set_window_state {state:'restored'}).",
    RPC_PARAMS(
        DRIVE_WINDOW_SELECTOR_PARAMS,
        RPC_PARAM_OPT("width", "integer", "Target client width in pixels. Combine with height, or with aspect to derive height."),
        RPC_PARAM_OPT("height", "integer", "Target client height in pixels. Combine with width, or with aspect to derive width."),
        RPC_PARAM_OPT("aspect", "string",
            "Target aspect ratio (a 'W:H' string like '16:9' or a bare float W/H). The missing dimension is derived "
            "from the provided one (or from the current width when neither width nor height is given)."),
        RPC_PARAM_DEF("logical", "boolean",
            "When true, width/height are DPI-independent logical pixels and are multiplied by the window DPI scale "
            "for the physical resize. Default false (sizes are physical pixels).", "false")
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

    const bool bLogical = Ctx.GetBool(TEXT("logical"), false);
    const TOptional<int32> WidthOpt = Ctx.GetIntFirstOf({ TEXT("width") });
    const TOptional<int32> HeightOpt = Ctx.GetIntFirstOf({ TEXT("height") });

    bool bHasAspect = false;
    double AspectRatio = 0.0;
    FString AspectError;
    if (!ParseAspectParam(Ctx, bHasAspect, AspectRatio, AspectError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), AspectError);
        return true;
    }

    if (!WidthOpt.IsSet() && !HeightOpt.IsSet() && !bHasAspect)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Provide width and/or height, or an aspect ratio"));
        return true;
    }

    const bool bWasMaximized = Window->IsWindowMaximized();
    if (bWasMaximized)
    {
        Ctx.SendError(TEXT("WINDOW_MAXIMIZED"),
            FString::Printf(TEXT("Window '%s' is maximized and will not visibly resize; restore it first via "
                "editor.set_window_state {state:'restored'}, then retry"), *WindowTitle));
        return true;
    }

    const float Dpi = FMath::Max(Window->GetDPIScaleFactor(), KINDA_SMALL_NUMBER);
    const FVector2f CurrentClient = Window->GetClientSizeInScreen();
    // Current client size expressed in the caller's chosen space (logical or physical).
    const double CurW = bLogical ? static_cast<double>(CurrentClient.X) / Dpi : static_cast<double>(CurrentClient.X);
    const double CurH = bLogical ? static_cast<double>(CurrentClient.Y) / Dpi : static_cast<double>(CurrentClient.Y);

    double ReqW = 0.0;
    double ReqH = 0.0;
    if (bHasAspect)
    {
        if (WidthOpt.IsSet())
        {
            ReqW = WidthOpt.GetValue();
            ReqH = ReqW / AspectRatio;
        }
        else if (HeightOpt.IsSet())
        {
            ReqH = HeightOpt.GetValue();
            ReqW = ReqH * AspectRatio;
        }
        else
        {
            ReqW = CurW;
            ReqH = ReqW / AspectRatio;
        }
    }
    else
    {
        ReqW = WidthOpt.IsSet() ? static_cast<double>(WidthOpt.GetValue()) : CurW;
        ReqH = HeightOpt.IsSet() ? static_cast<double>(HeightOpt.GetValue()) : CurH;
    }

    if (ReqW < 1.0 || ReqH < 1.0)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Resolved client size must be at least 1x1 px"));
        return true;
    }

    // SWindow::Resize takes the DPI-scaled (physical) client size.
    const double PhysW = bLogical ? ReqW * Dpi : ReqW;
    const double PhysH = bLogical ? ReqH * Dpi : ReqH;

    Window->Resize(FVector2f(static_cast<float>(PhysW), static_cast<float>(PhysH)));
    PumpEditorSlate(Window);

    const FVector2f NewClient = Window->GetClientSizeInScreen();
    const FVector2f NewWindow = Window->GetSizeInScreen();
    const FVector2f NewPos = Window->GetPositionInScreen();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("windowTitle"), WindowTitle);
    Result->SetObjectField(TEXT("requestedClientSize"), MakePointObject(FVector2f(static_cast<float>(PhysW), static_cast<float>(PhysH))));
    Result->SetObjectField(TEXT("clientSize"), MakePointObject(NewClient));
    Result->SetObjectField(TEXT("windowSize"), MakePointObject(NewWindow));
    Result->SetObjectField(TEXT("position"), MakePointObject(NewPos));
    Result->SetNumberField(TEXT("dpiScale"), Dpi);
    Result->SetBoolField(TEXT("wasMaximized"), bWasMaximized);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- editor.set_window_state ----
REGISTER_RPC_HANDLER("editor.set_window_state", "editor",
    "Set a top-level editor window's maximize/minimize/restore state. Target the window with the shared "
    "window selector. state: 'restored' (aka 'normal' - un-maximize AND un-minimize to a normal framed "
    "window), 'maximized', or 'minimized'. This is the callable recovery for editor.resize_window's "
    "WINDOW_MAXIMIZED error: set state='restored' first, then resize (the editor commonly launches "
    "maximized). Unlike every other window verb this one CAN target a minimized window - state='restored' "
    "against a minimized editor is the in-band recovery, and needs no selector when the whole editor is "
    "minimized. Reports the before/after maximized+minimized flags, the MEASURED resulting state read back "
    "after the call (with a warning when it differs from the requested one), and the resulting client size.",
    RPC_PARAMS(
        DRIVE_WINDOW_SELECTOR_PARAMS,
        RPC_PARAM_REQ("state", "string",
            "Target window state: 'restored' (or 'normal' - un-maximize and un-minimize to a normal "
            "framed window), 'maximized', or 'minimized'.")
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

    FString StateStr;
    if (!Ctx.RequireString(TEXT("state"), StateStr))
    {
        return true;
    }
    const FString State = StateStr.TrimStartAndEnd().ToLower();

    enum class EDesiredWindowState { Restored, Maximized, Minimized };
    EDesiredWindowState Desired;
    if (State == TEXT("restored") || State == TEXT("restore") || State == TEXT("normal"))
    {
        Desired = EDesiredWindowState::Restored;
    }
    else if (State == TEXT("maximized") || State == TEXT("maximize"))
    {
        Desired = EDesiredWindowState::Maximized;
    }
    else if (State == TEXT("minimized") || State == TEXT("minimize"))
    {
        Desired = EDesiredWindowState::Minimized;
    }
    else
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            TEXT("state must be one of: 'restored' (or 'normal'), 'maximized', 'minimized'"));
        return true;
    }

    // Minimizing the editor is the one direction of this verb that can poison an in-flight
    // measurement: a minimized window stops rendering, so a profile taken against it reads a
    // frozen last value in every column (a hard ~333 ms frame cap, DrawCalls 0, a bit-identical
    // GameThreadTime) with nothing out of range to flag it. GIsAutomationTesting spans the whole
    // automation session (AutomationTest.cpp:1218/:1226, not just one test), so refuse rather
    // than hand a running suite a silently-wrong scene cost. Restore and maximize are unaffected;
    // the recovery direction must never be gated.
    if (Desired == EDesiredWindowState::Minimized && GIsAutomationTesting)
    {
        Ctx.SendError(TEXT("WINDOW_MINIMIZE_REFUSED"),
            TEXT("Refusing to minimize a window while an automation session is running: a minimized "
                 "window stops rendering, and every profiling/render number taken afterwards reports a "
                 "frozen last value that looks like a scene regression. Re-issue after the suite ends."));
        return true;
    }

    TSharedPtr<SWindow> Window;
    FString WindowTitle;
    FString ErrorCode;
    FString ErrorMessage;
    // The ONLY caller that opts into minimized-tolerant resolution. Every other consumer of the
    // shared selector (drive observe/act/capture, editor.frame_graph, editor.resize_window,
    // editor.screenshot_window) keeps the visible-only set: they read or act on a window's
    // contents, which a minimized window does not have. This verb acts on the window's own
    // window-manager state, so a minimized window is exactly the target it exists to fix.
    if (!FDriveEditorChrome::ResolveWindow(
            FDriveHandlerCommon::ParseWindowSelector(Ctx), Window, WindowTitle, ErrorCode, ErrorMessage,
            /*bIncludeMinimizedWindows=*/true))
    {
        Ctx.SendError(ErrorCode, ErrorMessage);
        return true;
    }

    // A window matched, so NO_WINDOWS / WINDOW_NOT_FOUND are no longer honest answers - but a
    // window with no native platform window (a virtual/offscreen SWindow) makes SWindow::Restore /
    // Maximize / Minimize literal no-ops (SWindow.cpp:1754-1776 each guard on NativeWindow), and
    // reporting a readback from one is a fake success. Refuse with a code that says which of the
    // two failures happened: matched, but its state cannot be changed.
    if (!Window->GetNativeWindow().IsValid())
    {
        Ctx.SendError(TEXT("WINDOW_STATE_NOT_CHANGEABLE"),
            FString::Printf(
                TEXT("Window '%s' matched the selector but has no native platform window, so its "
                     "maximize/minimize/restore state cannot be changed"), *WindowTitle));
        return true;
    }

    const bool bWasMaximized = Window->IsWindowMaximized();
    const bool bWasMinimized = Window->IsWindowMinimized();

    switch (Desired)
    {
    case EDesiredWindowState::Restored:  Window->Restore();  break;
    case EDesiredWindowState::Maximized: Window->Maximize(); break;
    case EDesiredWindowState::Minimized: Window->Minimize(); break;
    }
    // Let the native window-mode change settle before reading back the resulting state/size.
    PumpEditorSlate(Window);

    const bool bIsMaximized = Window->IsWindowMaximized();
    const bool bIsMinimized = Window->IsWindowMinimized();
    const FVector2f Client = Window->GetClientSizeInScreen();

    // What the window ACTUALLY is now, read back from the native window - not what was asked for.
    // The window manager is free to decline (an offscreen host under -RenderOffscreen routinely
    // does), and a response that echoed only the request would report that decline as a success.
    const FString MeasuredState = DescribeEditorWindowStateName(bIsMinimized, bIsMaximized);
    const FString DesiredState = DescribeEditorWindowStateName(
        Desired == EDesiredWindowState::Minimized, Desired == EDesiredWindowState::Maximized);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("windowTitle"), WindowTitle);
    Result->SetStringField(TEXT("requestedState"), State);
    Result->SetStringField(TEXT("state"), MeasuredState);
    Result->SetBoolField(TEXT("stateMatchesRequest"), MeasuredState == DesiredState);
    Result->SetBoolField(TEXT("wasMaximized"), bWasMaximized);
    Result->SetBoolField(TEXT("wasMinimized"), bWasMinimized);
    Result->SetBoolField(TEXT("isMaximized"), bIsMaximized);
    Result->SetBoolField(TEXT("isMinimized"), bIsMinimized);
    Result->SetObjectField(TEXT("clientSize"), MakePointObject(Client));
    if (MeasuredState != DesiredState)
    {
        Result->SetStringField(TEXT("warning"), FString::Printf(
            TEXT("Requested state '%s' but the window measured '%s' afterwards; the window manager "
                 "declined the change (common for an offscreen window under -RenderOffscreen)"),
            *DesiredState, *MeasuredState));
    }
    if (bIsMinimized)
    {
        // The window is left in the one state that makes every other verb refuse it and every
        // render/profiling number meaningless. Carry the exact call that undoes it, so the
        // recovery never has to be discovered from outside the RPC surface again.
        Result->SetStringField(TEXT("recovery"), FString::Printf(
            TEXT("editor.set_window_state {\"window_title\": \"%s\", \"state\": \"restored\"}"),
            *WindowTitle));
    }
    Ctx.SendSuccess(Result);
    return true;
}

// ---- editor.screenshot_window ----
REGISTER_RPC_HANDLER("editor.screenshot_window", "editor",
    "Capture a full top-level editor window (including its chrome) to a PNG under Saved/Screenshots/EditorWindow. "
    "With no selector, captures the exact main editor frame; explicit selectors use the shared window resolver. "
    "Returns windowTitle and windowType for the captured window. Uses FSlateApplication::TakeScreenshot rooted "
    "at the window widget with alpha forced opaque.",
    RPC_PARAMS(
        RPC_PARAM_OPT("window_title", "string",
            "Visible top-level window-title substring. With no non-empty title or index, captures the main editor frame."),
        RPC_PARAM_OPT("title", "string", "Alias for window_title."),
        RPC_PARAM_OPT("window_index", "integer",
            "Nth visible top-level window (0-based, see drive.list_windows). Takes precedence over window_title."),
        RPC_PARAM_OPT("index", "integer", "Alias for window_index."),
        RPC_PARAM_OPT("filename", "filepath",
            "Output filename inside Saved/Screenshots/EditorWindow. The .png extension is appended if missing.")
    ))
{
    if (!GEditor)
    {
        Ctx.SendError(TEXT("EDITOR_NOT_AVAILABLE"), TEXT("Editor not available"));
        return true;
    }
    TSharedPtr<SWindow> Window;
    FString WindowTitle;
    FString ErrorCode;
    FString ErrorMessage;
    const FDriveWindowSelector Selector = FDriveHandlerCommon::ParseWindowSelector(Ctx);
    if (!EditorWindowHandlers::ResolveScreenshotWindow(
            Selector, Window, WindowTitle, ErrorCode, ErrorMessage))
    {
        Ctx.SendError(ErrorCode, ErrorMessage);
        return true;
    }

    TArray<FColor> Bitmap;
    FIntVector SizeVec;
    // Never FSlateApplication::TakeScreenshot directly: a window the pass does not draw leaves the
    // renderer armed with a pointer to this frame. Contract in ScreenshotUtils.h.
    const bool bCaptured = PinWrightScreenshotUtils::TakeSlateScreenshot(
        Window.ToSharedRef(), Bitmap, SizeVec);
    if (!bCaptured || SizeVec.X <= 0 || SizeVec.Y <= 0 || Bitmap.Num() == 0)
    {
        Ctx.SendError(TEXT("CAPTURE_FAILED"),
            FString::Printf(TEXT("Failed to capture window '%s'"), *WindowTitle));
        return true;
    }

    // Force alpha opaque (Slate may leave it non-255); contract in ScreenshotUtils.h.
    PinWrightScreenshotUtils::ForceOpaqueAlpha(Bitmap);

    TArray64<uint8> PngData;
    FImageUtils::PNGCompressImageArray(SizeVec.X, SizeVec.Y,
        TArrayView64<const FColor>(Bitmap.GetData(), Bitmap.Num()), PngData);
    if (PngData.Num() == 0)
    {
        Ctx.SendError(TEXT("ENCODE_FAILED"), TEXT("Failed to encode editor window screenshot as PNG"));
        return true;
    }

    TArray<uint8> PngBytes;
    PngBytes.Reset(PngData.Num());
    PngBytes.Append(PngData.GetData(), PngData.Num());

    FString Filename;
    const FString OutputPath = PinWrightScreenshotUtils::MakeScreenshotOutputPath(
        Ctx.GetString(TEXT("filename")),
        TEXT("EditorWindow"),
        TEXT("EditorWindow"),
        Filename);
    if (!FFileHelper::SaveArrayToFile(PngBytes, *OutputPath))
    {
        Ctx.SendError(TEXT("SAVE_FAILED"),
            FString::Printf(TEXT("Failed to save editor window screenshot: %s"), *OutputPath));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("path"), OutputPath);
    Result->SetStringField(TEXT("filename"), Filename);
    Result->SetNumberField(TEXT("width"), SizeVec.X);
    Result->SetNumberField(TEXT("height"), SizeVec.Y);
    Result->SetNumberField(TEXT("sizeBytes"), PngBytes.Num());
    EditorWindowHandlers::SetScreenshotWindowIdentityFields(
        *Result, WindowTitle, Window->GetType());
    Result->SetStringField(TEXT("mimeType"), TEXT("image/png"));
    Ctx.SendSuccess(Result);
    return true;
}
